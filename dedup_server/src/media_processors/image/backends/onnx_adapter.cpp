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
                                       std::vector<std::uint8_t> &out_blob)
    {
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
        try
        {
            Poco::Logger::get("OnnxAdapter").trace("ComputeEmbedding called - file_path: %s, model_path: %s, input_size: %d, embedding_dim: %d", file_path.c_str(), model_path.c_str(), input_size, embedding_dim);

            if (model_path.empty())
            {
                Poco::Logger::get("OnnxAdapter").warning("Model path missing");
                std::cerr << "[OnnxAdapter] Model path missing" << std::endl;
                return false;
            }

            cv::setNumThreads(0);
            Poco::Logger::get("OnnxAdapter").trace("Attempting to load image: %s", file_path);
            std::cerr << "[OnnxAdapter] Loading image: " << file_path << std::endl;

            // Check if file exists before attempting to read
            if (!std::filesystem::exists(file_path))
            {
                Poco::Logger::get("OnnxAdapter").error("File does not exist when trying to load: %s", file_path);
                std::cerr << "[OnnxAdapter] File does not exist: " << file_path << std::endl;
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
                std::cerr << "[OnnxAdapter] OpenCV exception: " << e.what() << std::endl;
                return false;
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("OnnxAdapter").error("Exception loading image %s: %s", file_path, e.what());
                std::cerr << "[OnnxAdapter] Exception: " << e.what() << std::endl;
                return false;
            }
            catch (...)
            {
                Poco::Logger::get("OnnxAdapter").error("Unknown exception loading image: %s", file_path);
                std::cerr << "[OnnxAdapter] Unknown exception loading image" << std::endl;
                return false;
            }

            if (bgr.empty())
            {
                bool still_exists = std::filesystem::exists(file_path);
                auto file_size = still_exists ? std::filesystem::file_size(file_path) : 0;
                Poco::Logger::get("OnnxAdapter").warning("Failed to load image for ONNX (empty result): %s (exists: %s, size: %zu)", file_path, still_exists ? "yes" : "no", file_size);
                Poco::Logger::get("OnnxAdapter").trace("File exists check: %s", still_exists ? "yes" : "no");
                std::cerr << "[OnnxAdapter] Failed to load image (empty result, exists: " << (still_exists ? "yes" : "no") << ", size: " << file_size << ")" << std::endl;
                return false;
            }

            // Additional validation for image dimensions
            if (bgr.rows == 0 || bgr.cols == 0)
            {
                Poco::Logger::get("OnnxAdapter").warning("Invalid image dimensions (%dx%d) for ONNX: %s", bgr.cols, bgr.rows, file_path);
                std::cerr << "[OnnxAdapter] Invalid image dimensions" << std::endl;
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
                std::cerr << "[OnnxAdapter] Session manager not initialized" << std::endl;
                return false;
            }

            std::cerr << "[OnnxAdapter] Borrowing cached session for model: " << model_path << std::endl;
            auto session_lease = session_manager_->borrowSession(model_path);
            Ort::Session *session = session_lease.getSession();

            if (!session)
            {
                Poco::Logger::get("OnnxAdapter").error("Failed to borrow ONNX session");
                std::cerr << "[OnnxAdapter] Failed to borrow session" << std::endl;
                return false;
            }

            Ort::AllocatorWithDefaultOptions allocator;

            auto in_name = session->GetInputNameAllocated(0, allocator);
            std::vector<int64_t> in_shape = {1, 3, (int64_t)input_size, (int64_t)input_size};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

            // Check the expected input type
            auto input_type_info = session->GetInputTypeInfo(0);
            auto tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
            ONNXTensorElementDataType input_type = tensor_info.GetElementType();

            Ort::Value in_tensor;
            if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
            {
                // Convert float data to int64 (multiply by scale and cast)
                std::vector<int64_t> int64_vals;
                int64_vals.reserve(input_vals.size());
                for (float val : input_vals)
                {
                    int64_vals.push_back(static_cast<int64_t>(val * 255.0f)); // Scale back to 0-255 range as int64
                }
                in_tensor = Ort::Value::CreateTensor<int64_t>(mem, int64_vals.data(), int64_vals.size(), in_shape.data(), in_shape.size());
            }
            else
            {
                // Default to float
                in_tensor = Ort::Value::CreateTensor<float>(mem, input_vals.data(), input_vals.size(), in_shape.data(), in_shape.size());
            }

            auto out_name = session->GetOutputNameAllocated(0, allocator);
            const char *in_names[] = {in_name.get()};
            const char *out_names[] = {out_name.get()};
            std::cerr << "[OnnxAdapter] Running inference with cached session..." << std::endl;
            auto results = session->Run(Ort::RunOptions{nullptr}, in_names, &in_tensor, 1, out_names, 1);
            if (results.empty() || !results[0].IsTensor())
            {
                std::cerr << "[OnnxAdapter] Inference returned empty or non-tensor output" << std::endl;
                return false;
            }
            auto &tensor = results[0];
            auto info = tensor.GetTensorTypeAndShapeInfo();
            size_t count = info.GetElementCount();
            std::cerr << "[OnnxAdapter] Output elements: " << count << std::endl;
            float *data = tensor.GetTensorMutableData<float>();

            out_blob.resize(embedding_dim * sizeof(float));
            size_t copy_elems = std::min((size_t)embedding_dim, count);
            std::memcpy(out_blob.data(), data, copy_elems * sizeof(float));
            if (copy_elems < (size_t)embedding_dim)
                std::memset(out_blob.data() + copy_elems * sizeof(float), 0, (embedding_dim - copy_elems) * sizeof(float));
            std::cerr << "[OnnxAdapter] Embedding computed and copied (dim=" << embedding_dim << ")" << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("OnnxAdapter").error("ONNX exception: %s", std::string(e.what()));
            std::cerr << "[OnnxAdapter] Exception: " << e.what() << std::endl;
            return false;
        }
#else
        (void)file_path;
        (void)input_size;
        (void)model_path;
        (void)embedding_dim;
        (void)out_blob;
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

            // Prepare input tensor
            auto in_name = session->GetInputNameAllocated(0, allocator);
            std::vector<int64_t> in_shape = {1, 3, (int64_t)input_size, (int64_t)input_size};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

            // Check the expected input type
            auto input_type_info = session->GetInputTypeInfo(0);
            auto tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
            ONNXTensorElementDataType input_type = tensor_info.GetElementType();

            Ort::Value in_tensor;
            if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
            {
                // Convert float data to int64 (multiply by scale and cast)
                std::vector<int64_t> int64_vals;
                int64_vals.reserve(input_vals.size());
                for (float val : input_vals)
                {
                    int64_vals.push_back(static_cast<int64_t>(val * 255.0f)); // Scale back to 0-255 range as int64
                }
                in_tensor = Ort::Value::CreateTensor<int64_t>(mem, int64_vals.data(), int64_vals.size(), in_shape.data(), in_shape.size());
            }
            else
            {
                // Default to float
                in_tensor = Ort::Value::CreateTensor<float>(mem, input_vals.data(), input_vals.size(), in_shape.data(), in_shape.size());
            }

            // Run inference
            auto out_name = session->GetOutputNameAllocated(0, allocator);
            const char *in_names[] = {in_name.get()};
            const char *out_names[] = {out_name.get()};
            std::cerr << "[OnnxAdapter] Running inference on memory data with cached session..." << std::endl;
            auto results = session->Run(Ort::RunOptions{nullptr}, in_names, &in_tensor, 1, out_names, 1);

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
