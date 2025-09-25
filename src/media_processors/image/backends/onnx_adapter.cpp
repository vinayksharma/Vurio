#include "media_processors/image/backends/onnx_adapter.hpp"
#include <Poco/Logger.h>
#include <iostream>
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#endif

namespace MediaDedup
{
    bool OnnxAdapter::ComputeEmbedding(const std::string &file_path,
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
                Poco::Logger::get("OnnxAdapter").warning("Model path missing");
                std::cerr << "[OnnxAdapter] Model path missing" << std::endl;
                return false;
            }

            cv::setNumThreads(0);
            std::cerr << "[OnnxAdapter] Loading image: " << file_path << std::endl;
            cv::Mat bgr = cv::imread(file_path, cv::IMREAD_COLOR);
            if (bgr.empty())
            {
                Poco::Logger::get("OnnxAdapter").warning("Failed to load image for ONNX: %s", file_path);
                std::cerr << "[OnnxAdapter] Failed to load image" << std::endl;
                return false;
            }
            cv::Mat rgb;
            cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
            cv::Mat resized;
            cv::resize(rgb, resized, cv::Size(input_size, input_size), 0, 0, cv::INTER_AREA);
            resized.convertTo(resized, CV_32F, 1.0f / 255.0f);

            const cv::Scalar mean(0.48145466f, 0.4578275f, 0.40821073f);
            const cv::Scalar std(0.26862954f, 0.26130258f, 0.27577711f);
            cv::Mat chw[3];
            cv::split(resized, chw);
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

            Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "MediaDedup");
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads(1);
            so.SetInterOpNumThreads(1);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
            std::cerr << "[OnnxAdapter] Creating session with model: " << model_path << std::endl;
            Ort::Session session(env, model_path.c_str(), so);
            Ort::AllocatorWithDefaultOptions allocator;

            auto in_name = session.GetInputNameAllocated(0, allocator);
            std::vector<int64_t> in_shape = {1, 3, (int64_t)input_size, (int64_t)input_size};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
            Ort::Value in_tensor = Ort::Value::CreateTensor<float>(mem, input_vals.data(), input_vals.size(), in_shape.data(), in_shape.size());

            auto out_name = session.GetOutputNameAllocated(0, allocator);
            const char *in_names[] = {in_name.get()};
            const char *out_names[] = {out_name.get()};
            std::cerr << "[OnnxAdapter] Running inference..." << std::endl;
            auto results = session.Run(Ort::RunOptions{nullptr}, in_names, &in_tensor, 1, out_names, 1);
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
}
