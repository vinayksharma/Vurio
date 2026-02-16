#include "media_processors/image/backends/onnx_adapter.hpp"
#include "media_processors/image/backends/onnx_session_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <iostream>
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
            Poco::Logger::get("OnnxAdapter").trace("ComputeEmbedding called - file_path: %s, model_path: %s, input_size: %d, embedding_dim: %d", file_path.c_str(), model_path.c_str(), input_size, embedding_dim);

            if (model_path.empty())
            {
                Poco::Logger::get("OnnxAdapter").warning("Model path missing");
                if (out_reason) *out_reason = "model_path_missing";
                return false;
            }

            cv::setNumThreads(0);
            Poco::Logger::get("OnnxAdapter").trace("Attempting to load image: %s", file_path);

            // Check if file exists before attempting to read
            if (!std::filesystem::exists(file_path))
            {
                Poco::Logger::get("OnnxAdapter").error("File does not exist when trying to load: %s", file_path);
                if (out_reason) *out_reason = "file_not_exists";
                return false;
            }
            Poco::Logger::get("OnnxAdapter").trace("File exists, proceeding to load with OpenCV");

            // Enhanced error handling for OpenCV imread
            cv::Mat bgr;
            try
            {
                bgr = cv::imread(file_path, cv::IMREAD_COLOR);
                Poco::Logger::get("OnnxAdapter").trace("cv::imread returned - rows: %d, cols: %d, empty: %s", bgr.rows, bgr.cols, bgr.empty() ? "true" : "false");
            }
            catch (const cv::Exception &e)
            {
                Poco::Logger::get("OnnxAdapter").error("OpenCV exception loading image %s: %s", file_path, e.what());
                if (out_reason) *out_reason = std::string("opencv_exception: ") + e.what();
                return false;
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("OnnxAdapter").error("Exception loading image %s: %s", file_path, e.what());
                if (out_reason) *out_reason = std::string("exception: ") + e.what();
                return false;
            }
            catch (...)
            {
                Poco::Logger::get("OnnxAdapter").error("Unknown exception loading image: %s", file_path);
                if (out_reason) *out_reason = "unknown_exception";
                return false;
            }

            if (bgr.empty())
            {
                bool still_exists = std::filesystem::exists(file_path);
                auto file_size = still_exists ? std::filesystem::file_size(file_path) : 0;
                Poco::Logger::get("OnnxAdapter").warning("Failed to load image for ONNX (empty result): %s (exists: %s, size: %zu)", file_path, still_exists ? "yes" : "no", file_size);
                if (out_reason) *out_reason = "cv_imread_empty (file_exists=" + std::string(still_exists ? "yes" : "no") + ", size=" + std::to_string(file_size) + ")";
                return false;
            }

            // Additional validation for image dimensions
            if (bgr.rows == 0 || bgr.cols == 0)
            {
                Poco::Logger::get("OnnxAdapter").warning("Invalid image dimensions (%dx%d) for ONNX: %s", bgr.cols, bgr.rows, file_path);
                if (out_reason) *out_reason = "invalid_dimensions_" + std::to_string(bgr.cols) + "x" + std::to_string(bgr.rows);
                return false;
            }
            cv::Mat rgb;
            cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

            // Release BGR image memory immediately (can be 70MB+ for large images)
            bgr.release();

            cv::Mat resized;
            cv::resize(rgb, resized, cv::Size(input_size, input_size), 0, 0, cv::INTER_AREA);

            // Release RGB image memory immediately
            rgb.release();

            resized.convertTo(resized, CV_32F, 1.0f / 255.0f);

            const cv::Scalar mean(0.48145466f, 0.4578275f, 0.40821073f);
            const cv::Scalar std(0.26862954f, 0.26130258f, 0.27577711f);
            cv::Mat chw[3];
            cv::split(resized, chw);

            // Release resized image memory immediately after split
            resized.release();

            for (int c = 0; c < 3; ++c)
            {
                chw[c] = (chw[c] - mean[c]) / std[c];
            }
            std::vector<float> input_vals;
            input_vals.reserve(3 * input_size * input_size);
            for (int c = 0; c < 3; ++c)
            {
                input_vals.insert(input_vals.end(), (float *)chw[c].datastart, (float *)chw[c].dataend);
            }

            // Release channel matrices memory
            for (int c = 0; c < 3; ++c)
            {
                chw[c].release();
            }

            // Use session manager for cached session reuse (massive memory savings)
            if (!session_manager_)
            {
                Poco::Logger::get("OnnxAdapter").error("ONNX Session Manager not initialized");
                if (out_reason) *out_reason = "session_manager_not_initialized";
                return false;
            }
            auto session_lease = session_manager_->borrowSession(model_path);
            Ort::Session *session = session_lease.getSession();

            if (!session)
            {
                Poco::Logger::get("OnnxAdapter").error("Failed to borrow ONNX session");
                if (out_reason) *out_reason = "failed_to_borrow_session";
                return false;
            }

            Ort::AllocatorWithDefaultOptions allocator;
            std::vector<int64_t> in_shape = {1, 3, (int64_t)input_size, (int64_t)input_size};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

            // Find the image input: 4D tensor, NCHW (?,3,?,?) or NHWC (?,?,?,3). Accept -1 (dynamic) for dimensions.
            size_t num_inputs = session->GetInputCount();
            size_t image_input_index = 0;
            bool found_image_input = false;
            bool image_input_nhwc = false;
            for (size_t i = 0; i < num_inputs; ++i)
            {
                auto type_info = session->GetInputTypeInfo(i);
                auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
                std::vector<int64_t> dims = tensor_info.GetShape();
                if (dims.size() != 4) continue;
                if (dims[1] == 3 || dims[1] == -1)
                {
                    image_input_index = i;
                    found_image_input = true;
                    break;
                }
                if (dims[3] == 3 || dims[3] == -1)
                {
                    image_input_index = i;
                    found_image_input = true;
                    image_input_nhwc = true;
                    break;
                }
            }
            if (!found_image_input && num_inputs > 0)
            {
                for (size_t i = 0; i < num_inputs; ++i)
                {
                    auto name_ptr = session->GetInputNameAllocated(i, allocator);
                    auto type_info = session->GetInputTypeInfo(i);
                    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
                    std::vector<int64_t> dims = tensor_info.GetShape();
                    std::string dim_str;
                    for (size_t j = 0; j < dims.size(); ++j)
                        dim_str += (j ? "," : "") + std::to_string(dims[j]);
                    std::string msg = "Input[" + std::to_string(i) + "] name=" + std::string(name_ptr.get()) + " shape=[" + dim_str + "]";
                    Poco::Logger::get("OnnxAdapter").debug("%s", msg.c_str());
                }
                auto first_name = session->GetInputNameAllocated(0, allocator);
                std::string first_str(first_name.get());
                if (first_str.find("input_ids") != std::string::npos || first_str.find("token") != std::string::npos)
                {
                    Poco::Logger::get("OnnxAdapter").error("Model appears to be full CLIP (text+image). Need image-only encoder (e.g. pixel_values input, shape [1,3,224,224]).");
                    if (out_reason) *out_reason = "model_full_clip_need_image_only_encoder";
                    return false;
                }
            }

            auto input_type_info = session->GetInputTypeInfo(image_input_index);
            auto tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
            ONNXTensorElementDataType input_type = tensor_info.GetElementType();

            const int64_t spatial = static_cast<int64_t>(input_size) * static_cast<int64_t>(input_size);
            std::vector<int64_t> image_shape = image_input_nhwc
                ? std::vector<int64_t>{1, static_cast<int64_t>(input_size), static_cast<int64_t>(input_size), 3}
                : std::vector<int64_t>{1, 3, static_cast<int64_t>(input_size), static_cast<int64_t>(input_size)};
            const float *image_data = input_vals.data();
            std::vector<float> nhwc_vals;
            if (image_input_nhwc)
            {
                nhwc_vals.resize(input_vals.size());
                for (int64_t i = 0; i < spatial; ++i)
                    for (int c = 0; c < 3; ++c)
                        nhwc_vals[static_cast<size_t>(i * 3 + c)] = input_vals[static_cast<size_t>(c * spatial + i)];
                image_data = nhwc_vals.data();
            }

            Ort::Value in_tensor;
            if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
            {
                std::vector<int64_t> int64_vals;
                int64_vals.reserve(input_vals.size());
                for (size_t i = 0; i < input_vals.size(); ++i)
                    int64_vals.push_back(static_cast<int64_t>(image_data[i] * 255.0f));
                in_tensor = Ort::Value::CreateTensor<int64_t>(mem, int64_vals.data(), int64_vals.size(), image_shape.data(), image_shape.size());
            }
            else
            {
                in_tensor = Ort::Value::CreateTensor<float>(mem, const_cast<float *>(image_data), input_vals.size(), image_shape.data(), image_shape.size());
            }

            // Build full input list in session order (full CLIP needs input_ids + pixel_values)
            using InputNamePtr = decltype(session->GetInputNameAllocated(0, allocator));
            std::vector<InputNamePtr> input_name_ptrs;
            input_name_ptrs.reserve(num_inputs);
            for (size_t i = 0; i < num_inputs; ++i)
                input_name_ptrs.push_back(session->GetInputNameAllocated(i, allocator));
            std::vector<const char *> in_names_vec;
            in_names_vec.reserve(num_inputs);
            for (auto &p : input_name_ptrs)
                in_names_vec.push_back(p.get());

            std::vector<Ort::Value> input_values;
            input_values.reserve(num_inputs);
            std::vector<std::vector<int64_t>> dummy_storage;
            for (size_t i = 0; i < num_inputs; ++i)
            {
                if (i == image_input_index)
                {
                    input_values.push_back(std::move(in_tensor));
                    continue;
                }
                auto type_info_i = session->GetInputTypeInfo(i);
                auto tensor_info_i = type_info_i.GetTensorTypeAndShapeInfo();
                std::vector<int64_t> dims = tensor_info_i.GetShape();
                ONNXTensorElementDataType dtype = tensor_info_i.GetElementType();
                if (dims.size() == 2 && dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
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
                    Poco::Logger::get("OnnxAdapter").error("Unsupported non-image input at index %zu (dims=%zu, dtype=%d)", i, dims.size(), (int)dtype);
                    if (out_reason) *out_reason = "unsupported_extra_input";
                    return false;
                }
            }

            auto out_name = session->GetOutputNameAllocated(0, allocator);
            const char *out_names[] = {out_name.get()};
            auto results = session->Run(Ort::RunOptions{nullptr}, in_names_vec.data(), input_values.data(), num_inputs, out_names, 1);
            if (results.empty() || !results[0].IsTensor())
            {
                Poco::Logger::get("OnnxAdapter").warning("Inference returned empty or non-tensor output");
                if (out_reason) *out_reason = "inference_empty_or_non_tensor_output";
                return false;
            }
            auto &tensor = results[0];
            auto info = tensor.GetTensorTypeAndShapeInfo();
            size_t count = info.GetElementCount();
            float *data = tensor.GetTensorMutableData<float>();

            out_blob.resize(embedding_dim * sizeof(float));
            size_t copy_elems = std::min((size_t)embedding_dim, count);
            std::memcpy(out_blob.data(), data, copy_elems * sizeof(float));
            if (copy_elems < (size_t)embedding_dim)
                std::memset(out_blob.data() + copy_elems * sizeof(float), 0, (embedding_dim - copy_elems) * sizeof(float));
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("OnnxAdapter").error("ONNX exception: %s", std::string(e.what()));
            if (out_reason) *out_reason = std::string("onnx_exception: ") + e.what();
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
                Poco::Logger::get("OnnxAdapter").warning("Model path missing for memory-based processing");
                std::cerr << "[OnnxAdapter] Model path missing for memory-based processing" << std::endl;
                return false;
            }

            if (image_data.empty())
            {
                Poco::Logger::get("OnnxAdapter").warning("Image data is empty for memory-based processing");
                std::cerr << "[OnnxAdapter] Image data is empty for memory-based processing" << std::endl;
                return false;
            }

            cv::setNumThreads(0);
            std::cerr << "[OnnxAdapter] Loading image from memory data (size: " << image_data.size() << " bytes)" << std::endl;

            // Decode image from memory buffer
            cv::Mat bgr = cv::imdecode(image_data, cv::IMREAD_COLOR);
            if (bgr.empty())
            {
                Poco::Logger::get("OnnxAdapter").warning("Failed to decode image from memory data");
                std::cerr << "[OnnxAdapter] Failed to decode image from memory data" << std::endl;
                return false;
            }

            // Convert BGR to RGB
            cv::Mat rgb;
            cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

            // Release BGR memory immediately (can be 70MB+ for large images)
            bgr.release();

            // Resize to required input size
            cv::Mat resized;
            cv::resize(rgb, resized, cv::Size(input_size, input_size), 0, 0, cv::INTER_AREA);

            // Release RGB memory immediately
            rgb.release();

            resized.convertTo(resized, CV_32F, 1.0f / 255.0f);

            // Normalize with CLIP model statistics
            const cv::Scalar mean(0.48145466f, 0.4578275f, 0.40821073f);
            const cv::Scalar std(0.26862954f, 0.26130258f, 0.27577711f);
            cv::Mat chw[3];
            cv::split(resized, chw);

            // Release resized memory immediately after split
            resized.release();

            for (int c = 0; c < 3; ++c)
            {
                chw[c] = (chw[c] - mean[c]) / std[c];
            }

            // Flatten to CHW format for ONNX input
            std::vector<float> input_vals;
            input_vals.reserve(3 * input_size * input_size);
            for (int c = 0; c < 3; ++c)
            {
                input_vals.insert(input_vals.end(), (float *)chw[c].datastart, (float *)chw[c].dataend);
            }

            // Release channel matrices memory
            for (int c = 0; c < 3; ++c)
            {
                chw[c].release();
            }

            // Use session manager for cached session reuse (massive memory savings)
            if (!session_manager_)
            {
                Poco::Logger::get("OnnxAdapter").error("ONNX Session Manager not initialized for memory-based processing");
                std::cerr << "[OnnxAdapter] Session manager not initialized" << std::endl;
                return false;
            }

            std::cerr << "[OnnxAdapter] Borrowing cached session for model: " << model_path << std::endl;
            auto session_lease = session_manager_->borrowSession(model_path);
            Ort::Session *session = session_lease.getSession();

            if (!session)
            {
                Poco::Logger::get("OnnxAdapter").error("Failed to borrow ONNX session for memory-based processing");
                std::cerr << "[OnnxAdapter] Failed to borrow session" << std::endl;
                return false;
            }

            Ort::AllocatorWithDefaultOptions allocator;
            std::vector<int64_t> in_shape = {1, 3, (int64_t)input_size, (int64_t)input_size};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

            // Find image input: 4D NCHW (dims[1]==3 or -1) or NHWC (dims[3]==3 or -1)
            size_t num_inputs = session->GetInputCount();
            size_t image_input_index = 0;
            bool image_input_nhwc = false;
            for (size_t i = 0; i < num_inputs; ++i)
            {
                auto type_info = session->GetInputTypeInfo(i);
                auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
                std::vector<int64_t> dims = tensor_info.GetShape();
                if (dims.size() != 4) continue;
                if (dims[1] == 3 || dims[1] == -1) { image_input_index = i; break; }
                if (dims[3] == 3 || dims[3] == -1) { image_input_index = i; image_input_nhwc = true; break; }
            }

            const int64_t spatial_mem = static_cast<int64_t>(input_size) * static_cast<int64_t>(input_size);
            std::vector<int64_t> image_shape_mem = image_input_nhwc
                ? std::vector<int64_t>{1, static_cast<int64_t>(input_size), static_cast<int64_t>(input_size), 3}
                : in_shape;
            const float *image_data_mem = input_vals.data();
            std::vector<float> nhwc_vals_mem;
            if (image_input_nhwc)
            {
                nhwc_vals_mem.resize(input_vals.size());
                for (int64_t i = 0; i < spatial_mem; ++i)
                    for (int c = 0; c < 3; ++c)
                        nhwc_vals_mem[static_cast<size_t>(i * 3 + c)] = input_vals[static_cast<size_t>(c * spatial_mem + i)];
                image_data_mem = nhwc_vals_mem.data();
            }

            auto input_type_info = session->GetInputTypeInfo(image_input_index);
            auto tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
            ONNXTensorElementDataType input_type = tensor_info.GetElementType();

            Ort::Value in_tensor;
            if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
            {
                std::vector<int64_t> int64_vals;
                int64_vals.reserve(input_vals.size());
                for (size_t i = 0; i < input_vals.size(); ++i)
                    int64_vals.push_back(static_cast<int64_t>(image_data_mem[i] * 255.0f));
                in_tensor = Ort::Value::CreateTensor<int64_t>(mem, int64_vals.data(), int64_vals.size(), image_shape_mem.data(), image_shape_mem.size());
            }
            else
            {
                in_tensor = Ort::Value::CreateTensor<float>(mem, const_cast<float *>(image_data_mem), input_vals.size(), image_shape_mem.data(), image_shape_mem.size());
            }

            using InputNamePtr = decltype(session->GetInputNameAllocated(0, allocator));
            std::vector<InputNamePtr> input_name_ptrs;
            input_name_ptrs.reserve(num_inputs);
            for (size_t i = 0; i < num_inputs; ++i)
                input_name_ptrs.push_back(session->GetInputNameAllocated(i, allocator));
            std::vector<const char *> in_names_vec;
            in_names_vec.reserve(num_inputs);
            for (auto &p : input_name_ptrs)
                in_names_vec.push_back(p.get());

            std::vector<Ort::Value> input_values;
            input_values.reserve(num_inputs);
            std::vector<std::vector<int64_t>> dummy_storage_mem;
            for (size_t i = 0; i < num_inputs; ++i)
            {
                if (i == image_input_index)
                {
                    input_values.push_back(std::move(in_tensor));
                    continue;
                }
                auto type_info_i = session->GetInputTypeInfo(i);
                auto tensor_info_i = type_info_i.GetTensorTypeAndShapeInfo();
                std::vector<int64_t> dims = tensor_info_i.GetShape();
                ONNXTensorElementDataType dtype = tensor_info_i.GetElementType();
                if (dims.size() == 2 && dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    int64_t d0 = dims[0] == -1 ? 1 : dims[0];
                    int64_t d1 = dims[1] == -1 ? 77 : dims[1];
                    size_t total = static_cast<size_t>(d0 * d1);
                    dummy_storage_mem.emplace_back(total, 0);
                    std::vector<int64_t> shape = {d0, d1};
                    Ort::Value dummy = Ort::Value::CreateTensor<int64_t>(mem, dummy_storage_mem.back().data(), total, shape.data(), shape.size());
                    input_values.push_back(std::move(dummy));
                }
                else
                {
                    Poco::Logger::get("OnnxAdapter").error("Unsupported non-image input at index %zu for memory path", i);
                    return false;
                }
            }

            auto out_name = session->GetOutputNameAllocated(0, allocator);
            const char *out_names[] = {out_name.get()};
            auto results = session->Run(Ort::RunOptions{nullptr}, in_names_vec.data(), input_values.data(), num_inputs, out_names, 1);

            if (results.empty() || !results[0].IsTensor())
            {
                std::cerr << "[OnnxAdapter] Inference returned empty or non-tensor output" << std::endl;
                return false;
            }

            // Extract embedding from results
            auto &tensor = results[0];
            auto info = tensor.GetTensorTypeAndShapeInfo();
            size_t count = info.GetElementCount();
            std::cerr << "[OnnxAdapter] Output elements: " << count << std::endl;
            float *data = tensor.GetTensorMutableData<float>();

            // Copy embedding to output blob
            out_blob.resize(embedding_dim * sizeof(float));
            size_t copy_elems = std::min((size_t)embedding_dim, count);
            std::memcpy(out_blob.data(), data, copy_elems * sizeof(float));
            if (copy_elems < (size_t)embedding_dim)
                std::memset(out_blob.data() + copy_elems * sizeof(float), 0, (embedding_dim - copy_elems) * sizeof(float));

            std::cerr << "[OnnxAdapter] Embedding computed from memory data and copied (dim=" << embedding_dim << ")" << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("OnnxAdapter").error("ONNX memory processing exception: %s", std::string(e.what()));
            std::cerr << "[OnnxAdapter] Exception in memory processing: " << e.what() << std::endl;
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
}
