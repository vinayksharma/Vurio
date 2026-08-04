#include "media_processors/image/backends/onnx_adapter.hpp"
#include "media_processors/image/backends/onnx_session_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#endif

namespace MediaDedup
{
    // Static session manager instance
    std::shared_ptr<OnnxSessionManager> OnnxAdapter::session_manager_ = nullptr;

    void OnnxAdapter::initializeSessionManager(std::shared_ptr<UnifiedObservableConfigManager> config)
    {
#if defined(HAVE_ONNXRUNTIME)
        if (!session_manager_)
        {
            session_manager_ = std::make_shared<OnnxSessionManager>(config);
            Poco::Logger::get("OnnxAdapter").information("ONNX Session Manager initialized");
        }
#else
        (void)config; // Suppress unused parameter warning
#endif
    }

    void OnnxAdapter::shutdownSessionManager()
    {
#if defined(HAVE_ONNXRUNTIME)
        if (session_manager_)
        {
            Poco::Logger::get("OnnxAdapter").information("Shutting down ONNX Session Manager");
            session_manager_.reset();
        }
#endif
    }

#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
    namespace
    {
        Poco::Logger &adapterLogger()
        {
            static Poco::Logger &logger = Poco::Logger::get("OnnxAdapter");
            return logger;
        }

        // DF-4: decode at 1/4 resolution when the source is comfortably
        // larger than the model input (the usual case: a 2560px preview for
        // a 224px input). libjpeg does the reduction during decode — 4-16x
        // less preprocessing work and ~1/16th the peak pixel memory. Falls
        // back to a full decode when the reduced image would be smaller than
        // the model input (tiny sources) or the reduced decode fails.
        cv::Mat decodeFromFile(const std::string &file_path, int input_size)
        {
            cv::Mat reduced = cv::imread(file_path, cv::IMREAD_REDUCED_COLOR_4);
            if (!reduced.empty() && std::min(reduced.rows, reduced.cols) >= input_size)
                return reduced;
            return cv::imread(file_path, cv::IMREAD_COLOR);
        }

        cv::Mat decodeFromMemory(const std::vector<std::uint8_t> &image_data, int input_size)
        {
            cv::Mat reduced = cv::imdecode(image_data, cv::IMREAD_REDUCED_COLOR_4);
            if (!reduced.empty() && std::min(reduced.rows, reduced.cols) >= input_size)
                return reduced;
            return cv::imdecode(image_data, cv::IMREAD_COLOR);
        }

        // BGR -> RGB -> resize -> CLIP-normalized CHW floats. Consumes `bgr`.
        std::vector<float> preprocessClip(cv::Mat &bgr, int input_size)
        {
            cv::Mat rgb;
            cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
            bgr.release();

            cv::Mat resized;
            cv::resize(rgb, resized, cv::Size(input_size, input_size), 0, 0, cv::INTER_AREA);
            rgb.release();

            resized.convertTo(resized, CV_32F, 1.0f / 255.0f);

            const cv::Scalar mean(0.48145466f, 0.4578275f, 0.40821073f);
            const cv::Scalar std(0.26862954f, 0.26130258f, 0.27577711f);
            cv::Mat chw[3];
            cv::split(resized, chw);
            resized.release();

            for (int c = 0; c < 3; ++c)
                chw[c] = (chw[c] - mean[c]) / std[c];

            std::vector<float> input_vals;
            input_vals.reserve(3 * static_cast<size_t>(input_size) * input_size);
            for (int c = 0; c < 3; ++c)
                input_vals.insert(input_vals.end(), (float *)chw[c].datastart, (float *)chw[c].dataend);
            return input_vals;
        }

        // DF-4: inference against the wrapper's CACHED I/O spec — no
        // per-call name/shape/type queries, no per-call allocator churn.
        bool runClipInference(OnnxSessionWrapper &wrapper,
                              std::vector<float> &input_vals,
                              int input_size,
                              int embedding_dim,
                              std::vector<std::uint8_t> &out_blob,
                              std::string *out_reason)
        {
            const auto &io = wrapper.io;
            Ort::Session *session = wrapper.session.get();

            size_t image_input_index = io.image_input_index;
            if (!io.found_image_input)
            {
                if (!io.input_names.empty() &&
                    (io.input_names[0].find("input_ids") != std::string::npos ||
                     io.input_names[0].find("token") != std::string::npos))
                {
                    adapterLogger().error("Model appears to be full CLIP (text+image). Need image-only encoder (e.g. pixel_values input, shape [1,3,224,224]).");
                    if (out_reason)
                        *out_reason = "model_full_clip_need_image_only_encoder";
                    return false;
                }
                image_input_index = 0; // legacy behavior: assume the first input
            }

            static Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

            const int64_t spatial = static_cast<int64_t>(input_size) * static_cast<int64_t>(input_size);
            std::vector<int64_t> image_shape = io.image_input_nhwc
                                                   ? std::vector<int64_t>{1, input_size, input_size, 3}
                                                   : std::vector<int64_t>{1, 3, input_size, input_size};
            const float *image_data = input_vals.data();
            std::vector<float> nhwc_vals;
            if (io.image_input_nhwc)
            {
                nhwc_vals.resize(input_vals.size());
                for (int64_t i = 0; i < spatial; ++i)
                    for (int c = 0; c < 3; ++c)
                        nhwc_vals[static_cast<size_t>(i * 3 + c)] = input_vals[static_cast<size_t>(c * spatial + i)];
                image_data = nhwc_vals.data();
            }

            const ONNXTensorElementDataType input_type =
                image_input_index < io.input_types.size() ? io.input_types[image_input_index]
                                                          : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;

            Ort::Value in_tensor;
            std::vector<int64_t> int64_vals;
            if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
            {
                int64_vals.reserve(input_vals.size());
                for (size_t i = 0; i < input_vals.size(); ++i)
                    int64_vals.push_back(static_cast<int64_t>(image_data[i] * 255.0f));
                in_tensor = Ort::Value::CreateTensor<int64_t>(mem, int64_vals.data(), int64_vals.size(), image_shape.data(), image_shape.size());
            }
            else
            {
                in_tensor = Ort::Value::CreateTensor<float>(mem, const_cast<float *>(image_data), input_vals.size(), image_shape.data(), image_shape.size());
            }

            // Full input list in session order (full CLIP needs input_ids + pixel_values)
            std::vector<Ort::Value> input_values;
            input_values.reserve(io.num_inputs);
            std::vector<std::vector<int64_t>> dummy_storage;
            for (size_t i = 0; i < io.num_inputs; ++i)
            {
                if (i == image_input_index)
                {
                    input_values.push_back(std::move(in_tensor));
                    continue;
                }
                const auto &dims = io.input_dims[i];
                if (dims.size() == 2 && io.input_types[i] == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    int64_t d0 = dims[0] == -1 ? 1 : dims[0];
                    int64_t d1 = dims[1] == -1 ? 77 : dims[1];
                    size_t total = static_cast<size_t>(d0 * d1);
                    dummy_storage.emplace_back(total, 0);
                    std::vector<int64_t> shape = {d0, d1};
                    Ort::Value dummy = Ort::Value::CreateTensor<int64_t>(mem, dummy_storage.back().data(), total, shape.data(), shape.size());
                    input_values.push_back(std::move(dummy));
                }
                else
                {
                    adapterLogger().error("Unsupported non-image input at index %zu (dims=%zu, dtype=%d)", i, dims.size(), (int)io.input_types[i]);
                    if (out_reason)
                        *out_reason = "unsupported_extra_input";
                    return false;
                }
            }

            if (io.output_name.empty())
            {
                if (out_reason)
                    *out_reason = "model_has_no_output";
                return false;
            }
            const char *out_names[] = {io.output_name.c_str()};
            auto results = session->Run(Ort::RunOptions{nullptr}, io.input_name_ptrs.data(),
                                        input_values.data(), io.num_inputs, out_names, 1);
            if (results.empty() || !results[0].IsTensor())
            {
                adapterLogger().warning("Inference returned empty or non-tensor output");
                if (out_reason)
                    *out_reason = "inference_empty_or_non_tensor_output";
                return false;
            }

            auto &tensor = results[0];
            size_t count = tensor.GetTensorTypeAndShapeInfo().GetElementCount();
            float *data = tensor.GetTensorMutableData<float>();

            out_blob.resize(embedding_dim * sizeof(float));
            size_t copy_elems = std::min((size_t)embedding_dim, count);
            std::memcpy(out_blob.data(), data, copy_elems * sizeof(float));
            if (copy_elems < (size_t)embedding_dim)
                std::memset(out_blob.data() + copy_elems * sizeof(float), 0, (embedding_dim - copy_elems) * sizeof(float));
            return true;
        }

        bool embedDecodedImage(cv::Mat &bgr,
                               int input_size,
                               const std::string &model_path,
                               int embedding_dim,
                               std::shared_ptr<OnnxSessionManager> &session_manager,
                               std::vector<std::uint8_t> &out_blob,
                               std::string *out_reason)
        {
            if (bgr.rows == 0 || bgr.cols == 0)
            {
                if (out_reason)
                    *out_reason = "invalid_dimensions_" + std::to_string(bgr.cols) + "x" + std::to_string(bgr.rows);
                return false;
            }

            std::vector<float> input_vals = preprocessClip(bgr, input_size);

            if (!session_manager)
            {
                adapterLogger().error("ONNX Session Manager not initialized");
                if (out_reason)
                    *out_reason = "session_manager_not_initialized";
                return false;
            }
            auto session_lease = session_manager->borrowSession(model_path);
            OnnxSessionWrapper *wrapper = session_lease.getWrapper();
            if (!wrapper || !wrapper->session)
            {
                adapterLogger().error("Failed to borrow ONNX session");
                if (out_reason)
                    *out_reason = "failed_to_borrow_session";
                return false;
            }

            return runClipInference(*wrapper, input_vals, input_size, embedding_dim, out_blob, out_reason);
        }
    }
#endif

    bool OnnxAdapter::ComputeEmbedding(const std::string &file_path,
                                       int input_size,
                                       const std::string &model_path,
                                       int embedding_dim,
                                       std::vector<std::uint8_t> &out_blob,
                                       std::string *out_reason)
    {
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
        try
        {
            adapterLogger().trace("ComputeEmbedding called - file_path: %s, model_path: %s, input_size: %d, embedding_dim: %d",
                                  file_path.c_str(), model_path.c_str(), input_size, embedding_dim);

            if (model_path.empty())
            {
                adapterLogger().warning("Model path missing");
                if (out_reason)
                    *out_reason = "model_path_missing";
                return false;
            }

            cv::setNumThreads(0);

            if (!std::filesystem::exists(file_path))
            {
                adapterLogger().error("File does not exist when trying to load: %s", file_path);
                if (out_reason)
                    *out_reason = "file_not_exists";
                return false;
            }

            cv::Mat bgr;
            try
            {
                bgr = decodeFromFile(file_path, input_size);
            }
            catch (const cv::Exception &e)
            {
                adapterLogger().error("OpenCV exception loading image %s: %s", file_path, e.what());
                if (out_reason)
                    *out_reason = std::string("opencv_exception: ") + e.what();
                return false;
            }
            catch (const std::exception &e)
            {
                adapterLogger().error("Exception loading image %s: %s", file_path, e.what());
                if (out_reason)
                    *out_reason = std::string("exception: ") + e.what();
                return false;
            }
            catch (...)
            {
                adapterLogger().error("Unknown exception loading image: %s", file_path);
                if (out_reason)
                    *out_reason = "unknown_exception";
                return false;
            }

            if (bgr.empty())
            {
                bool still_exists = std::filesystem::exists(file_path);
                auto file_size = still_exists ? std::filesystem::file_size(file_path) : 0;
                adapterLogger().warning("Failed to load image for ONNX (empty result): %s (exists: %s, size: %zu)",
                                        file_path, still_exists ? "yes" : "no", file_size);
                if (out_reason)
                    *out_reason = "cv_imread_empty (file_exists=" + std::string(still_exists ? "yes" : "no") + ", size=" + std::to_string(file_size) + ")";
                return false;
            }

            return embedDecodedImage(bgr, input_size, model_path, embedding_dim, session_manager_, out_blob, out_reason);
        }
        catch (const std::exception &e)
        {
            adapterLogger().error("ONNX exception: %s", std::string(e.what()));
            if (out_reason)
                *out_reason = std::string("onnx_exception: ") + e.what();
            return false;
        }
#else
        (void)file_path;
        (void)input_size;
        (void)model_path;
        (void)embedding_dim;
        (void)out_blob;
        (void)out_reason;
        return false;
#endif
    }

    bool OnnxAdapter::ComputeEmbedding(const std::vector<std::uint8_t> &image_data,
                                       int input_size,
                                       const std::string &model_path,
                                       int embedding_dim,
                                       std::vector<std::uint8_t> &out_blob)
    {
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
        try
        {
            if (model_path.empty())
            {
                adapterLogger().warning("Model path missing for memory-based processing");
                return false;
            }
            if (image_data.empty())
            {
                adapterLogger().warning("Image data is empty for memory-based processing");
                return false;
            }

            cv::setNumThreads(0);
            adapterLogger().trace("Loading image from memory data (size: %zu bytes)", image_data.size());

            cv::Mat bgr = decodeFromMemory(image_data, input_size);
            if (bgr.empty())
            {
                adapterLogger().warning("Failed to decode image from memory data");
                return false;
            }

            std::string reason;
            bool ok = embedDecodedImage(bgr, input_size, model_path, embedding_dim, session_manager_, out_blob, &reason);
            if (!ok)
                adapterLogger().warning("Memory-based embedding failed: %s", reason);
            return ok;
        }
        catch (const std::exception &e)
        {
            adapterLogger().error("ONNX memory processing exception: %s", std::string(e.what()));
            return false;
        }
#else
        (void)image_data;
        (void)input_size;
        (void)model_path;
        (void)embedding_dim;
        (void)out_blob;
        return false;
#endif
    }

    bool OnnxAdapter::DecodeDimsForTest(const std::string &file_path,
                                        int input_size,
                                        int &out_rows,
                                        int &out_cols)
    {
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
        cv::Mat decoded = decodeFromFile(file_path, input_size);
        if (decoded.empty())
            return false;
        out_rows = decoded.rows;
        out_cols = decoded.cols;
        return true;
#else
        (void)file_path;
        (void)input_size;
        (void)out_rows;
        (void)out_cols;
        return false;
#endif
    }
}
