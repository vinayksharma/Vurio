#include "adjustments/export_models.hpp"
#include <Poco/Logger.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <unordered_map>
#include <vector>
#if defined(HAVE_ONNXRUNTIME)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

namespace MediaDedup::Adjustments::ExportModels
{
    namespace
    {
        Poco::Logger &logger() { return Poco::Logger::get("ExportModels"); }

        std::mutex g_mutex;
        Models g_models;

#if defined(HAVE_ONNXRUNTIME)
        struct Session
        {
            std::unique_ptr<Ort::Session> session;
            std::string input_name;
            std::string output_name;
            bool coreml = false;
        };

        Ort::Env &env()
        {
            static Ort::Env e(ORT_LOGGING_LEVEL_ERROR, "vurio-export");
            return e;
        }

        std::unique_ptr<Session> open(const std::string &path, bool try_coreml, std::string *error)
        {
            if (path.empty() || !std::filesystem::exists(path))
            {
                if (error)
                    *error = "model not present: " + path;
                return nullptr;
            }
            auto build = [&](bool coreml) {
                Ort::SessionOptions options;
                options.SetIntraOpNumThreads(4);
                options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                if (coreml)
                {
                    std::unordered_map<std::string, std::string> opts{{"MLComputeUnits", "ALL"}, {"ModelFormat", "MLProgram"}};
                    options.AppendExecutionProvider("CoreML", opts);
                }
                return options;
            };
            auto s = std::make_unique<Session>();
            if (try_coreml)
            {
                try
                {
                    s->session = std::make_unique<Ort::Session>(env(), path.c_str(), build(true));
                    s->coreml = true;
                }
                catch (const std::exception &e)
                {
                    logger().warning("CoreML unavailable for %s, using CPU: %s", path, std::string(e.what()));
                }
            }
            if (!s->session)
            {
                try
                {
                    s->session = std::make_unique<Ort::Session>(env(), path.c_str(), build(false));
                }
                catch (const std::exception &e)
                {
                    if (error)
                        *error = "cannot open " + path + ": " + e.what();
                    return nullptr;
                }
            }
            Ort::AllocatorWithDefaultOptions alloc;
            s->input_name = s->session->GetInputNameAllocated(0, alloc).get();
            s->output_name = s->session->GetOutputNameAllocated(0, alloc).get();
            logger().information("Export model open (%s): %s", std::string(s->coreml ? "CoreML" : "CPU"), path);
            return s;
        }

        std::unique_ptr<Session> g_denoise;
        std::unique_ptr<Session> g_upscale;

        Session *denoiseSession(std::string *error)
        {
            if (!g_denoise)
                g_denoise = open(g_models.denoise_model, false, error);
            return g_denoise.get();
        }
        Session *upscaleSession(std::string *error)
        {
            if (!g_upscale)
                g_upscale = open(g_models.upscale_model, g_models.upscale_coreml, error);
            return g_upscale.get();
        }

        // Run `s` over `src` (CV_32FC3 BGR [0,1]) tile by tile; the model
        // maps an h×w tile to a (scale·h)×(scale·w) tile. Tiles overlap by
        // kOverlap on every side (the border is reflected) and only the
        // inner part of each result is kept, so seams never show; tile
        // sides are padded to a multiple of `multiple` for the network's
        // downsampling stages.
        bool runTiled(Session &s, const cv::Mat &src, cv::Mat &dst, int scale, int multiple, const Progress &progress,
                      std::string *error)
        {
            const int H = src.rows, W = src.cols, ov = kOverlap, inner = kTile - 2 * ov;
            cv::Mat padded;
            cv::copyMakeBorder(src, padded, ov, ov, ov, ov, cv::BORDER_REFLECT_101);
            dst = cv::Mat(H * scale, W * scale, CV_32FC3);
            const int ny = (H + inner - 1) / inner, nx = (W + inner - 1) / inner;
            const int total = ny * nx;
            int done = 0;
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
            std::vector<float> in;
            for (int ty = 0; ty < ny; ++ty)
            {
                for (int tx = 0; tx < nx; ++tx)
                {
                    const int y0 = ty * inner, x0 = tx * inner;
                    const int th = std::min(inner, H - y0), tw = std::min(inner, W - x0);
                    // The tile with its overlap, from the padded image.
                    cv::Mat tile = padded(cv::Rect(x0, y0, tw + 2 * ov, th + 2 * ov));
                    int ph = ((tile.rows + multiple - 1) / multiple) * multiple;
                    int pw = ((tile.cols + multiple - 1) / multiple) * multiple;
                    cv::Mat work;
                    cv::copyMakeBorder(tile, work, 0, ph - tile.rows, 0, pw - tile.cols, cv::BORDER_REFLECT_101);
                    // NCHW RGB.
                    in.assign(static_cast<size_t>(3) * ph * pw, 0.0f);
                    const size_t plane = static_cast<size_t>(ph) * pw;
                    for (int y = 0; y < ph; ++y)
                    {
                        const cv::Vec3f *row = work.ptr<cv::Vec3f>(y);
                        for (int x = 0; x < pw; ++x)
                        {
                            in[0 * plane + static_cast<size_t>(y) * pw + x] = row[x][2];
                            in[1 * plane + static_cast<size_t>(y) * pw + x] = row[x][1];
                            in[2 * plane + static_cast<size_t>(y) * pw + x] = row[x][0];
                        }
                    }
                    const int64_t shape[4] = {1, 3, ph, pw};
                    Ort::Value input = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape, 4);
                    const char *ins[] = {s.input_name.c_str()};
                    const char *outs[] = {s.output_name.c_str()};
                    std::vector<Ort::Value> results;
                    try
                    {
                        results = s.session->Run(Ort::RunOptions{nullptr}, ins, &input, 1, outs, 1);
                    }
                    catch (const std::exception &e)
                    {
                        if (error)
                            *error = std::string("model run failed: ") + e.what();
                        return false;
                    }
                    auto info = results[0].GetTensorTypeAndShapeInfo();
                    auto oshape = info.GetShape();
                    if (oshape.size() != 4 || oshape[2] != static_cast<int64_t>(ph) * scale || oshape[3] != static_cast<int64_t>(pw) * scale)
                    {
                        if (error)
                            *error = "model returned an unexpected shape";
                        return false;
                    }
                    const float *o = results[0].GetTensorData<float>();
                    const int oh = ph * scale, ow = pw * scale;
                    const size_t oplane = static_cast<size_t>(oh) * ow;
                    // Keep the inner th×tw (scaled) region: it starts at ov.
                    for (int y = 0; y < th * scale; ++y)
                    {
                        cv::Vec3f *row = dst.ptr<cv::Vec3f>(y0 * scale + y);
                        const int sy = ov * scale + y;
                        for (int x = 0; x < tw * scale; ++x)
                        {
                            const int sx = ov * scale + x;
                            const size_t idx = static_cast<size_t>(sy) * ow + sx;
                            row[x0 * scale + x] = cv::Vec3f(o[2 * oplane + idx], o[1 * oplane + idx], o[0 * oplane + idx]);
                        }
                    }
                    ++done;
                    if (progress)
                        progress(done, total);
                }
            }
            cv::min(dst, 1.0f, dst);
            cv::max(dst, 0.0f, dst);
            return true;
        }
#endif

        Status statusOf(const std::string &path, bool loaded, const char *note)
        {
            Status s;
            s.model_path = path;
            s.model_present = !path.empty() && std::filesystem::exists(path);
#if defined(HAVE_ONNXRUNTIME)
            s.available = s.model_present;
#else
            s.available = false;
#endif
            s.loaded = loaded;
            s.note = note;
            return s;
        }
    }

    void configure(const Models &models)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_models = models;
#if defined(HAVE_ONNXRUNTIME)
        g_denoise.reset();
        g_upscale.reset();
#endif
    }

    void unload()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        g_denoise.reset();
        g_upscale.reset();
#endif
    }

    Status denoiseStatus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        return statusOf(g_models.denoise_model, g_denoise != nullptr, "NAFNet-SIDD denoises an export (CPU, tiled)");
#else
        return statusOf(g_models.denoise_model, false, "ONNX Runtime is not compiled in");
#endif
    }

    Status upscaleStatus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        return statusOf(g_models.upscale_model, g_upscale != nullptr, "Real-ESRGAN upscales an export ×2 (tiled)");
#else
        return statusOf(g_models.upscale_model, false, "ONNX Runtime is not compiled in");
#endif
    }

    bool denoiseAvailable() { return denoiseStatus().available; }
    bool upscaleAvailable() { return upscaleStatus().available; }

    bool denoise(cv::Mat &bgr32, double strength, std::string *error, const Progress &progress)
    {
#if defined(HAVE_ONNXRUNTIME)
        if (bgr32.empty() || bgr32.type() != CV_32FC3)
        {
            if (error)
                *error = "denoise needs a float BGR frame";
            return false;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        Session *s = denoiseSession(error);
        if (!s)
            return false;
        auto t0 = std::chrono::steady_clock::now();
        cv::Mat out;
        if (!runTiled(*s, bgr32, out, 1, 16, progress, error))
            return false;
        strength = std::clamp(strength, 0.0, 1.0);
        if (strength >= 1.0 - 1e-9)
            bgr32 = out;
        else
            cv::addWeighted(bgr32, 1.0 - strength, out, strength, 0.0, bgr32);
        logger().information("AI denoise of %dx%d at %.2f: %.1f s", bgr32.cols, bgr32.rows, strength,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return true;
#else
        (void)bgr32;
        (void)strength;
        (void)progress;
        if (error)
            *error = "ONNX Runtime is not compiled in";
        return false;
#endif
    }

    bool upscale(cv::Mat &bgr32, std::string *error, const Progress &progress)
    {
#if defined(HAVE_ONNXRUNTIME)
        if (bgr32.empty() || bgr32.type() != CV_32FC3)
        {
            if (error)
                *error = "upscale needs a float BGR frame";
            return false;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        Session *s = upscaleSession(error);
        if (!s)
            return false;
        auto t0 = std::chrono::steady_clock::now();
        cv::Mat out;
        if (!runTiled(*s, bgr32, out, kUpscaleFactor, 8, progress, error))
            return false;
        logger().information("AI upscale of %dx%d to %dx%d (%s): %.1f s", bgr32.cols, bgr32.rows, out.cols, out.rows,
                             std::string(s->coreml ? "CoreML" : "CPU"),
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        bgr32 = out;
        return true;
#else
        (void)bgr32;
        (void)progress;
        if (error)
            *error = "ONNX Runtime is not compiled in";
        return false;
#endif
    }
}
