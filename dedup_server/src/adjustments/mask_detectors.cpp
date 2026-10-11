#include "adjustments/mask_detectors.hpp"

#include "media_processors/text/clip_tokenizer.hpp"

#include <Poco/Logger.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>

#if defined(HAVE_ONNXRUNTIME)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

namespace MediaDedup::Adjustments::MaskDetectors
{
    namespace
    {
        Poco::Logger &logger()
        {
            static Poco::Logger &l = Poco::Logger::get("MaskDetectors");
            return l;
        }

        std::mutex g_mutex;
        Models g_models;

        // Function-local so the platform backend's static registration
        // (vision_subject_mask.mm) is safe whatever the TU init order.
        SubjectBackend &subjectSlot()
        {
            static SubjectBackend slot;
            return slot;
        }

        // CLIPSeg's processor: 352×352 (aspect not kept), ImageNet mean/std.
        constexpr int kClipSegSide = 352;
        // MediaPipe selfie segmentation: 256×256, [0,1].
        constexpr int kSelfieSide = 256;

        cv::Mat luminance(const cv::Mat &bgr8)
        {
            cv::Mat grey;
            cv::cvtColor(bgr8, grey, cv::COLOR_BGR2GRAY);
            grey.convertTo(grey, CV_32F, 1.0 / 255.0);
            return grey;
        }

#if defined(HAVE_ONNXRUNTIME)
        struct Session
        {
            std::unique_ptr<Ort::Session> session;
            std::vector<std::string> input_names;
            std::vector<std::string> output_names;
            bool coreml = false;
        };

        Ort::Env &env()
        {
            static Ort::Env e(ORT_LOGGING_LEVEL_WARNING, "vurio-masks");
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
            for (size_t i = 0; i < s->session->GetInputCount(); ++i)
                s->input_names.emplace_back(s->session->GetInputNameAllocated(i, alloc).get());
            for (size_t i = 0; i < s->session->GetOutputCount(); ++i)
                s->output_names.emplace_back(s->session->GetOutputNameAllocated(i, alloc).get());
            logger().information("Mask model open (%s): %s", std::string(s->coreml ? "CoreML" : "CPU"), path);
            return s;
        }

        std::unique_ptr<Session> g_prompt;
        std::unique_ptr<Session> g_people;
        std::unique_ptr<ClipTokenizer> g_tokenizer;

        Session *promptSession(std::string *error)
        {
            if (!g_prompt)
                g_prompt = open(g_models.prompt_model, g_models.use_coreml, error);
            return g_prompt.get();
        }

        Session *peopleSession(std::string *error)
        {
            if (!g_people)
                g_people = open(g_models.people_model, g_models.use_coreml, error);
            return g_people.get();
        }

        ClipTokenizer *tokenizer(std::string *error)
        {
            if (!g_tokenizer)
            {
                auto t = std::make_unique<ClipTokenizer>();
                if (!t->load(g_models.vocab_json, g_models.merges_txt))
                {
                    if (error)
                        *error = "CLIP tokenizer files missing (vocab.json / merges.txt)";
                    return nullptr;
                }
                g_tokenizer = std::move(t);
            }
            return g_tokenizer.get();
        }

        // NCHW float tensor from a BGR8 image resized to side×side.
        std::vector<float> planar(const cv::Mat &bgr8, int side, const float mean[3], const float stdev[3])
        {
            cv::Mat resized;
            cv::resize(bgr8, resized, cv::Size(side, side), 0, 0, cv::INTER_AREA);
            std::vector<float> data(static_cast<size_t>(3) * side * side);
            for (int y = 0; y < side; ++y)
            {
                const cv::Vec3b *row = resized.ptr<cv::Vec3b>(y);
                for (int x = 0; x < side; ++x)
                {
                    // RGB planes.
                    for (int c = 0; c < 3; ++c)
                    {
                        float v = row[x][2 - c] / 255.0f;
                        data[static_cast<size_t>(c) * side * side + static_cast<size_t>(y) * side + x] = (v - mean[c]) / stdev[c];
                    }
                }
            }
            return data;
        }

        bool runClipSeg(const cv::Mat &base_bgr8, const std::string &text, cv::Mat &out, std::string *error)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Session *s = promptSession(error);
            ClipTokenizer *tok = tokenizer(error);
            if (!s || !tok)
                return false;
            ClipTokenizer::Encoded enc = tok->encode(text);
            const float mean[3] = {0.485f, 0.456f, 0.406f};
            const float stdev[3] = {0.229f, 0.224f, 0.225f};
            std::vector<float> pixels = planar(base_bgr8, kClipSegSide, mean, stdev);
            try
            {
                Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
                const int64_t text_shape[2] = {1, static_cast<int64_t>(enc.input_ids.size())};
                const int64_t image_shape[4] = {1, 3, kClipSegSide, kClipSegSide};
                std::vector<Ort::Value> inputs;
                std::vector<const char *> names;
                for (const std::string &name : s->input_names)
                {
                    names.push_back(name.c_str());
                    if (name == "input_ids")
                        inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, enc.input_ids.data(), enc.input_ids.size(), text_shape, 2));
                    else if (name == "attention_mask")
                        inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, enc.attention_mask.data(), enc.attention_mask.size(), text_shape, 2));
                    else if (name == "pixel_values")
                        inputs.push_back(Ort::Value::CreateTensor<float>(mem, pixels.data(), pixels.size(), image_shape, 4));
                    else
                    {
                        if (error)
                            *error = "unexpected CLIPSeg input " + name;
                        return false;
                    }
                }
                const char *out_name = "logits";
                auto results = s->session->Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(), &out_name, 1);
                const float *logits = results[0].GetTensorData<float>();
                size_t count = results[0].GetTensorTypeAndShapeInfo().GetElementCount();
                if (count < static_cast<size_t>(kClipSegSide) * kClipSegSide)
                {
                    if (error)
                        *error = "CLIPSeg returned too few logits";
                    return false;
                }
                cv::Mat coarse(kClipSegSide, kClipSegSide, CV_32FC1);
                for (int y = 0; y < kClipSegSide; ++y)
                    for (int x = 0; x < kClipSegSide; ++x)
                    {
                        float z = logits[static_cast<size_t>(y) * kClipSegSide + x];
                        coarse.at<float>(y, x) = 1.0f / (1.0f + std::exp(-z));
                    }
                out = guidedUpsample(coarse, base_bgr8);
                return true;
            }
            catch (const std::exception &e)
            {
                if (error)
                    *error = std::string("CLIPSeg inference failed: ") + e.what();
                return false;
            }
        }

        bool runSelfie(const cv::Mat &base_bgr8, cv::Mat &out, std::string *error)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Session *s = peopleSession(error);
            if (!s)
                return false;
            const float mean[3] = {0.f, 0.f, 0.f};
            const float stdev[3] = {1.f, 1.f, 1.f};
            std::vector<float> pixels = planar(base_bgr8, kSelfieSide, mean, stdev);
            try
            {
                Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
                const int64_t image_shape[4] = {1, 3, kSelfieSide, kSelfieSide};
                Ort::Value input = Ort::Value::CreateTensor<float>(mem, pixels.data(), pixels.size(), image_shape, 4);
                const char *in_name = s->input_names.empty() ? "pixel_values" : s->input_names[0].c_str();
                const char *out_name = s->output_names.empty() ? "alphas" : s->output_names[0].c_str();
                auto results = s->session->Run(Ort::RunOptions{nullptr}, &in_name, &input, 1, &out_name, 1);
                const float *alphas = results[0].GetTensorData<float>();
                size_t count = results[0].GetTensorTypeAndShapeInfo().GetElementCount();
                if (count < static_cast<size_t>(kSelfieSide) * kSelfieSide)
                {
                    if (error)
                        *error = "selfie model returned too few values";
                    return false;
                }
                cv::Mat coarse(kSelfieSide, kSelfieSide, CV_32FC1);
                for (int y = 0; y < kSelfieSide; ++y)
                    for (int x = 0; x < kSelfieSide; ++x)
                        coarse.at<float>(y, x) = std::min(1.0f, std::max(0.0f, alphas[static_cast<size_t>(y) * kSelfieSide + x]));
                out = guidedUpsample(coarse, base_bgr8);
                return true;
            }
            catch (const std::exception &e)
            {
                if (error)
                    *error = std::string("selfie inference failed: ") + e.what();
                return false;
            }
        }
#endif
    }

    void configure(const Models &models)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_models = models;
#if defined(HAVE_ONNXRUNTIME)
        g_prompt.reset();
        g_people.reset();
        g_tokenizer.reset();
#endif
    }

    void unload()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        g_prompt.reset();
        g_people.reset();
        g_tokenizer.reset();
#endif
    }

    namespace
    {
        BackendStatus statusOf(const std::string &path, bool loaded)
        {
            BackendStatus st;
            st.model_path = path;
            st.model_present = !path.empty() && std::filesystem::exists(path);
#if defined(HAVE_ONNXRUNTIME)
            st.available = st.model_present;
            st.loaded = loaded;
            if (!st.model_present)
                st.note = "model file missing";
#else
            (void)loaded;
            st.note = "built without ONNX Runtime";
#endif
            return st;
        }
    }

    BackendStatus promptStatus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        BackendStatus st = statusOf(g_models.prompt_model, g_prompt != nullptr);
        if (st.model_present && (!std::filesystem::exists(g_models.vocab_json) || !std::filesystem::exists(g_models.merges_txt)))
        {
            st.available = false;
            st.note = "CLIP tokenizer files missing";
        }
        return st;
#else
        return statusOf(g_models.prompt_model, false);
#endif
    }

    BackendStatus peopleStatus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        return statusOf(g_models.people_model, g_people != nullptr);
#else
        return statusOf(g_models.people_model, false);
#endif
    }

    bool subjectBackendRegistered()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return static_cast<bool>(subjectSlot());
    }

    void setSubjectBackend(SubjectBackend backend)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        subjectSlot() = std::move(backend);
    }

    bool prompt(const cv::Mat &base_bgr8, const std::string &text, cv::Mat &out, std::string *error)
    {
        if (base_bgr8.empty())
        {
            if (error)
                *error = "no base pixels";
            return false;
        }
#if defined(HAVE_ONNXRUNTIME)
        return runClipSeg(base_bgr8, text, out, error);
#else
        (void)text;
        (void)out;
        if (error)
            *error = "built without ONNX Runtime";
        return false;
#endif
    }

    bool people(const cv::Mat &base_bgr8, cv::Mat &out, std::string *error)
    {
        if (base_bgr8.empty())
        {
            if (error)
                *error = "no base pixels";
            return false;
        }
#if defined(HAVE_ONNXRUNTIME)
        return runSelfie(base_bgr8, out, error);
#else
        (void)out;
        if (error)
            *error = "built without ONNX Runtime";
        return false;
#endif
    }

    bool subject(const cv::Mat &base_bgr8, cv::Mat &out, std::string *error)
    {
        SubjectBackend backend;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            backend = subjectSlot();
        }
        if (backend)
        {
            cv::Mat platform;
            if (backend(base_bgr8, platform) && !platform.empty())
            {
                if (platform.size() != base_bgr8.size())
                    cv::resize(platform, platform, base_bgr8.size(), 0, 0, cv::INTER_LINEAR);
                out = platform;
                return true;
            }
            logger().debug("platform subject backend produced nothing; falling back to the prompt model");
        }
        // CLIPSeg answers an abstract prompt faintly (sigmoid peaks of 0.01
        // to 0.2 on library photos, measured 2026-10-11), so the fallback
        // is stretched per picture: the strongest response is the subject.
        cv::Mat raw;
        if (!prompt(base_bgr8, "the main subject", raw, error))
            return false;
        double mn = 0, mx = 0;
        cv::minMaxLoc(raw, &mn, &mx);
        if (mx - mn > 1e-6)
            out = (raw - mn) / (mx - mn);
        else
            out = raw;
        return true;
    }

    void faces(const std::vector<FaceBox> &boxes, const std::vector<int> &person_ids, double grow,
               int width, int height, cv::Mat &out)
    {
        out = cv::Mat(std::max(height, 1), std::max(width, 1), CV_32FC1, cv::Scalar(0.0f));
        double g = std::max(0.5, grow);
        for (const FaceBox &b : boxes)
        {
            if (!person_ids.empty() &&
                std::find(person_ids.begin(), person_ids.end(), b.person_id) == person_ids.end())
                continue;
            double cx = (b.x + b.w / 2) * width;
            double cy = (b.y + b.h / 2) * height;
            double rx = std::max(1.0, b.w * width * g / 2);
            double ry = std::max(1.0, b.h * height * g / 2);
            cv::ellipse(out, cv::Point2d(cx, cy), cv::Size2d(rx, ry), 0, 0, 360, cv::Scalar(1.0f), cv::FILLED, cv::LINE_AA);
        }
        // Soft rim: a blur of a tenth of the typical face radius.
        double sigma = 0;
        for (const FaceBox &b : boxes)
            sigma = std::max(sigma, std::min(b.w * width, b.h * height) * g * 0.1);
        if (sigma > 0.5)
        {
            int k = static_cast<int>(std::ceil(sigma * 3)) * 2 + 1;
            cv::GaussianBlur(out, out, cv::Size(k, k), sigma, sigma, cv::BORDER_REPLICATE);
        }
        cv::min(out, 1.0f, out);
        cv::max(out, 0.0f, out);
    }

    cv::Mat guidedUpsample(const cv::Mat &coarse, const cv::Mat &base_bgr8, double radius_fraction, double eps)
    {
        cv::Mat p;
        cv::resize(coarse, p, base_bgr8.size(), 0, 0, cv::INTER_LINEAR);
        cv::Mat I = luminance(base_bgr8);
        int r = std::max(2, static_cast<int>(std::lround(radius_fraction * std::max(I.cols, I.rows))));
        cv::Size k(2 * r + 1, 2 * r + 1);
        auto box = [&](const cv::Mat &m) {
            cv::Mat o;
            cv::boxFilter(m, o, CV_32F, k, cv::Point(-1, -1), true, cv::BORDER_REFLECT);
            return o;
        };
        cv::Mat mean_I = box(I), mean_p = box(p);
        cv::Mat corr_I = box(I.mul(I)), corr_Ip = box(I.mul(p));
        cv::Mat var_I = corr_I - mean_I.mul(mean_I);
        cv::Mat cov_Ip = corr_Ip - mean_I.mul(mean_p);
        cv::Mat a = cov_Ip / (var_I + static_cast<float>(eps));
        cv::Mat b = mean_p - a.mul(mean_I);
        cv::Mat q = box(a).mul(I) + box(b);
        cv::min(q, 1.0f, q);
        cv::max(q, 0.0f, q);
        return q;
    }
}
