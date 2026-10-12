#include "adjustments/mask_detectors.hpp"

#include "media_processors/text/clip_tokenizer.hpp"

#include <Poco/Logger.h>
#include <algorithm>
#include <chrono>
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
        std::unique_ptr<Session> g_sam_encoder;
        std::unique_ptr<Session> g_sam_decoder;
        std::unique_ptr<ClipTokenizer> g_tokenizer;

        // SlimSAM (the SAM processor): long side 1024, ImageNet mean/std,
        // zero-padded to 1024² after normalisation; prompts in those pixels.
        constexpr int kSamSide = 1024;
        constexpr int kSamLowRes = 256;

        Session *samEncoder(std::string *error)
        {
            if (!g_sam_encoder)
                g_sam_encoder = open(g_models.refine_encoder, g_models.use_coreml, error);
            return g_sam_encoder.get();
        }

        Session *samDecoder(std::string *error)
        {
            if (!g_sam_decoder)
                g_sam_decoder = open(g_models.refine_decoder, g_models.use_coreml, error);
            return g_sam_decoder.get();
        }

        bool samFilesPresent()
        {
            return !g_models.refine_encoder.empty() && !g_models.refine_decoder.empty() &&
                   std::filesystem::exists(g_models.refine_encoder) && std::filesystem::exists(g_models.refine_decoder);
        }

        double iouOf(const cv::Mat &a, const cv::Mat &b)
        {
            double inter = cv::countNonZero(a & b);
            double uni = cv::countNonZero(a | b);
            return uni > 0 ? inter / uni : 0.0;
        }

        // The refinement proper; g_mutex held by the caller.
        bool runSam(const cv::Mat &base_bgr8, const cv::Mat &coarse, const std::vector<RefinePrompt> &prompts,
                    cv::Mat &out, std::string *error, RefineReport *report)
        {
            Session *enc = samEncoder(error);
            Session *dec = samDecoder(error);
            if (!enc || !dec)
                return false;
            auto t0 = std::chrono::steady_clock::now();
            // Encoder: the base resized to a 1024 long side, padded bottom/right.
            double scale = static_cast<double>(kSamSide) / std::max(base_bgr8.cols, base_bgr8.rows);
            int rw = std::max(1, static_cast<int>(std::lround(base_bgr8.cols * scale)));
            int rh = std::max(1, static_cast<int>(std::lround(base_bgr8.rows * scale)));
            cv::Mat resized;
            cv::resize(base_bgr8, resized, cv::Size(rw, rh), 0, 0, scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
            const float mean[3] = {0.485f, 0.456f, 0.406f};
            const float stdev[3] = {0.229f, 0.224f, 0.225f};
            std::vector<float> pixels(static_cast<size_t>(3) * kSamSide * kSamSide, 0.0f);
            for (int y = 0; y < rh; ++y)
            {
                const cv::Vec3b *row = resized.ptr<cv::Vec3b>(y);
                for (int x = 0; x < rw; ++x)
                    for (int c = 0; c < 3; ++c)
                        pixels[static_cast<size_t>(c) * kSamSide * kSamSide + static_cast<size_t>(y) * kSamSide + x] =
                            (row[x][2 - c] / 255.0f - mean[c]) / stdev[c];
            }
            // Coarse-mask pixels → SAM pixels.
            double cx = static_cast<double>(rw) / coarse.cols, cy = static_cast<double>(rh) / coarse.rows;
            try
            {
                Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
                const int64_t image_shape[4] = {1, 3, kSamSide, kSamSide};
                Ort::Value image = Ort::Value::CreateTensor<float>(mem, pixels.data(), pixels.size(), image_shape, 4);
                const char *enc_in[] = {"pixel_values"};
                const char *enc_out[] = {"image_embeddings", "image_positional_embeddings"};
                auto embeddings = enc->session->Run(Ort::RunOptions{nullptr}, enc_in, &image, 1, enc_out, 2);
                if (report)
                    report->encoder_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

                out = coarse.clone();
                int dilate_px = std::max(2, static_cast<int>(std::lround(0.02 * std::max(coarse.cols, coarse.rows))));
                cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * dilate_px + 1, 2 * dilate_px + 1));
                for (const RefinePrompt &p : prompts)
                {
                    // Box as the two corner "points" (labels 2 and 3), then
                    // the positives (label 1): the SAM prompt encoding.
                    std::vector<float> pts;
                    std::vector<int64_t> labels;
                    auto add = [&](double x, double y, int64_t label) {
                        pts.push_back(static_cast<float>(x * cx));
                        pts.push_back(static_cast<float>(y * cy));
                        labels.push_back(label);
                    };
                    add(p.box.x, p.box.y, 2);
                    add(p.box.x + p.box.width, p.box.y + p.box.height, 3);
                    for (const cv::Point &q : p.positives)
                        add(q.x + 0.5, q.y + 0.5, 1);
                    const int64_t n = static_cast<int64_t>(labels.size());
                    const int64_t pts_shape[4] = {1, 1, n, 2};
                    const int64_t lbl_shape[3] = {1, 1, n};
                    std::vector<Ort::Value> inputs;
                    inputs.push_back(Ort::Value::CreateTensor<float>(mem, pts.data(), pts.size(), pts_shape, 4));
                    inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, labels.data(), labels.size(), lbl_shape, 3));
                    // Embeddings are borrowed by shape/data, not moved, so
                    // every region decodes from the one encoder pass.
                    {
                        auto info0 = embeddings[0].GetTensorTypeAndShapeInfo();
                        auto info1 = embeddings[1].GetTensorTypeAndShapeInfo();
                        auto s0 = info0.GetShape(), s1 = info1.GetShape();
                        inputs.push_back(Ort::Value::CreateTensor<float>(mem, embeddings[0].GetTensorMutableData<float>(),
                                                                         info0.GetElementCount(), s0.data(), s0.size()));
                        inputs.push_back(Ort::Value::CreateTensor<float>(mem, embeddings[1].GetTensorMutableData<float>(),
                                                                         info1.GetElementCount(), s1.data(), s1.size()));
                    }
                    const char *dec_in[] = {"input_points", "input_labels", "image_embeddings", "image_positional_embeddings"};
                    const char *dec_out[] = {"iou_scores", "pred_masks"};
                    auto results = dec->session->Run(Ort::RunOptions{nullptr}, dec_in, inputs.data(), inputs.size(), dec_out, 2);
                    const float *logits = results[1].GetTensorData<float>();
                    size_t count = results[1].GetTensorTypeAndShapeInfo().GetElementCount();
                    if (count < static_cast<size_t>(3) * kSamLowRes * kSamLowRes)
                    {
                        if (error)
                            *error = "SAM returned too few mask values";
                        return false;
                    }
                    // Three candidates: the one that agrees best with the
                    // coarse region wins (we know what we asked for).
                    cv::Mat best_logits;
                    double best_iou = -1;
                    for (int k = 0; k < 3; ++k)
                    {
                        cv::Mat low(kSamLowRes, kSamLowRes, CV_32FC1,
                                    const_cast<float *>(logits + static_cast<size_t>(k) * kSamLowRes * kSamLowRes));
                        cv::Mat up;
                        cv::resize(low, up, cv::Size(kSamSide, kSamSide), 0, 0, cv::INTER_LINEAR);
                        cv::Mat full;
                        cv::resize(up(cv::Rect(0, 0, rw, rh)), full, coarse.size(), 0, 0, cv::INTER_LINEAR);
                        cv::Mat binary = full > 0;
                        double iou = iouOf(binary, p.region);
                        if (iou > best_iou)
                        {
                            best_iou = iou;
                            best_logits = full;
                        }
                    }
                    if (best_iou < kRefineMinAgreement)
                    {
                        if (report)
                            ++report->kept_coarse;
                        continue;
                    }
                    // Replace the region's coarse blob (grown a little) by
                    // the sharp coverage: sigmoid of the logits.
                    cv::Mat grown;
                    cv::dilate(p.region, grown, kernel);
                    cv::Mat sharp(best_logits.size(), CV_32FC1);
                    for (int y = 0; y < sharp.rows; ++y)
                    {
                        const float *l = best_logits.ptr<float>(y);
                        float *s = sharp.ptr<float>(y);
                        for (int x = 0; x < sharp.cols; ++x)
                            s[x] = 1.0f / (1.0f + std::exp(-l[x]));
                    }
                    out.setTo(0.0f, grown);
                    cv::max(out, sharp, out);
                    if (report)
                        ++report->refined;
                }
                if (report)
                    report->total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                return true;
            }
            catch (const std::exception &e)
            {
                if (error)
                    *error = std::string("SAM inference failed: ") + e.what();
                return false;
            }
        }

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
        g_sam_encoder.reset();
        g_sam_decoder.reset();
        g_tokenizer.reset();
#endif
    }

    void unload()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        g_prompt.reset();
        g_people.reset();
        g_sam_encoder.reset();
        g_sam_decoder.reset();
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

    BackendStatus refineStatus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(HAVE_ONNXRUNTIME)
        BackendStatus st = statusOf(g_models.refine_encoder, g_sam_encoder != nullptr && g_sam_decoder != nullptr);
        if (st.model_present && !samFilesPresent())
        {
            st.available = false;
            st.note = "SAM decoder file missing";
        }
        if (!st.model_present)
            st.note = "model file missing";
        return st;
#else
        return statusOf(g_models.refine_encoder, false);
#endif
    }

    bool refineAvailable()
    {
        return refineStatus().available;
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

    bool subject(const cv::Mat &base_bgr8, cv::Mat &out, std::string *error, bool *used_platform)
    {
        SubjectBackend backend;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            backend = subjectSlot();
        }
        if (used_platform)
            *used_platform = false;
        if (backend)
        {
            cv::Mat platform;
            if (backend(base_bgr8, platform) && !platform.empty())
            {
                if (platform.size() != base_bgr8.size())
                    cv::resize(platform, platform, base_bgr8.size(), 0, 0, cv::INTER_LINEAR);
                out = platform;
                if (used_platform)
                    *used_platform = true;
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

    std::vector<RefinePrompt> refinePrompts(const cv::Mat &coarse, int max_components)
    {
        std::vector<RefinePrompt> prompts;
        if (coarse.empty() || coarse.type() != CV_32FC1)
            return prompts;
        double mn = 0, mx = 0;
        cv::minMaxLoc(coarse, &mn, &mx);
        // A faint response is noise, not a thing: nothing to refine.
        if (mx < 0.3)
            return prompts;
        cv::Mat binary = coarse >= static_cast<float>(0.5 * mx);
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
        if (n <= 1)
            return prompts;
        std::vector<std::pair<double, int>> by_area;
        double largest = 0;
        for (int i = 1; i < n; ++i)
        {
            double area = stats.at<int>(i, cv::CC_STAT_AREA);
            largest = std::max(largest, area);
            by_area.emplace_back(area, i);
        }
        std::sort(by_area.begin(), by_area.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        double min_area = std::max(0.002 * coarse.total(), 0.01 * largest);
        int margin = std::max(2, static_cast<int>(std::lround(0.02 * std::max(coarse.cols, coarse.rows))));
        for (const auto &[area, i] : by_area)
        {
            if (area < min_area || static_cast<int>(prompts.size()) >= std::max(1, max_components))
                break;
            RefinePrompt p;
            p.area = area;
            p.region = labels == i;
            cv::Rect box(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP),
                         stats.at<int>(i, cv::CC_STAT_WIDTH), stats.at<int>(i, cv::CC_STAT_HEIGHT));
            box.x = std::max(0, box.x - margin);
            box.y = std::max(0, box.y - margin);
            box.width = std::min(coarse.cols - box.x, box.width + 2 * margin);
            box.height = std::min(coarse.rows - box.y, box.height + 2 * margin);
            p.box = box;
            // The deepest interior point: safest "this thing" click.
            cv::Mat dist;
            cv::distanceTransform(p.region, dist, cv::DIST_L2, 3);
            cv::Point deepest;
            cv::minMaxLoc(dist, nullptr, nullptr, nullptr, &deepest);
            p.positives.push_back(deepest);
            // The response peak too, when it lies somewhere else.
            cv::Point peak;
            cv::minMaxLoc(coarse, nullptr, nullptr, nullptr, &peak, p.region);
            double apart = cv::norm(peak - deepest);
            if (apart > 0.15 * std::hypot(box.width, box.height))
                p.positives.push_back(peak);
            prompts.push_back(std::move(p));
        }
        return prompts;
    }

    bool refine(const cv::Mat &base_bgr8, const cv::Mat &coarse, cv::Mat &out, std::string *error, RefineReport *report)
    {
        if (base_bgr8.empty() || coarse.empty() || coarse.size() != base_bgr8.size())
        {
            if (error)
                *error = "refine needs the base and a coarse mask of the same size";
            return false;
        }
#if defined(HAVE_ONNXRUNTIME)
        std::vector<RefinePrompt> prompts = refinePrompts(coarse);
        if (report)
            *report = RefineReport{};
        if (report)
            report->components = static_cast<int>(prompts.size());
        if (prompts.empty())
        {
            out = coarse;
            return true;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!samFilesPresent())
        {
            if (error)
                *error = "SAM model files not present";
            return false;
        }
        return runSam(base_bgr8, coarse, prompts, out, error, report);
#else
        (void)out;
        (void)report;
        if (error)
            *error = "built without ONNX Runtime";
        return false;
#endif
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
