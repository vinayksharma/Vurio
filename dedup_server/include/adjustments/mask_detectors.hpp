#pragma once

// CHAIN-5 (docs/todo_tool_chain.md §5.6): the detector backends behind the
// detected mask sources. Every function takes the EXIF-oriented base as
// BGR8 (long side = the mask size) and writes a CV_32FC1 coverage of the
// same size in [0,1]. All portable (OpenCV + ONNX Runtime); the one Apple
// backend (Vision subject lifting) is registered by the server at startup
// through `setSubjectBackend` and never linked into tests.

#include <functional>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments::MaskDetectors
{
    struct Models
    {
        std::string prompt_model;   // CLIPSeg (rd64-refined, int8) ONNX
        std::string people_model;   // MediaPipe selfie segmentation ONNX
        std::string vocab_json;     // CLIP tokenizer files beside the models
        std::string merges_txt;
        // CHAIN-5b: SAM refinement (SlimSAM-77 ONNX, two files: the image
        // encoder and the prompt encoder + mask decoder).
        std::string refine_encoder;
        std::string refine_decoder;
        bool use_coreml = false;
    };

    // Configure once; model files are opened lazily on first use and kept.
    void configure(const Models &models);
    void unload();

    // Whether the backend can run: the runtime is compiled in and the model
    // file exists. `loaded` reports whether a session is open right now.
    struct BackendStatus
    {
        bool available = false;
        bool model_present = false;
        bool loaded = false;
        std::string model_path;
        std::string note;
    };
    BackendStatus promptStatus();
    BackendStatus peopleStatus();
    BackendStatus refineStatus();
    bool subjectBackendRegistered();

    // CLIPSeg: text → coverage. The 352 px logits are upsampled with an
    // edge-aware (guided) filter against the base's luminance.
    bool prompt(const cv::Mat &base_bgr8, const std::string &text, cv::Mat &out, std::string *error = nullptr);

    // Person segmentation (MediaPipe selfie model at 256², guided upsample).
    bool people(const cv::Mat &base_bgr8, cv::Mat &out, std::string *error = nullptr);

    // Salient subject: the registered platform backend when there is one,
    // otherwise CLIPSeg with the prompt "the main subject".
    using SubjectBackend = std::function<bool(const cv::Mat &base_bgr8, cv::Mat &out)>;
    void setSubjectBackend(SubjectBackend backend);
    // `platform` reports whether the registered backend produced the mask
    // (true) or the prompt-model fallback did (false).
    bool subject(const cv::Mat &base_bgr8, cv::Mat &out, std::string *error = nullptr, bool *platform = nullptr);

    // CHAIN-5b: SAM refinement. A coarse coverage (CLIPSeg at 352 px, the
    // selfie model at 256 px) is turned into prompts — one per connected
    // region above half its peak: the region's box and its deepest interior
    // point, plus the response peak when that lies elsewhere — and SlimSAM
    // answers each with a sharp mask. The candidate that agrees best with
    // the coarse region replaces it; a region SAM cannot match (agreement
    // under `kRefineMinAgreement`) keeps its coarse coverage.
    struct RefinePrompt
    {
        cv::Rect box;                      // in coarse-mask pixels
        std::vector<cv::Point> positives;  // interior point(s), label 1
        double area = 0;                   // region pixels
        cv::Mat region;                    // CV_8UC1 region membership
    };
    constexpr double kRefineMinAgreement = 0.25;
    std::vector<RefinePrompt> refinePrompts(const cv::Mat &coarse, int max_components = 6);

    struct RefineReport
    {
        int components = 0;   // regions found in the coarse mask
        int refined = 0;      // regions SAM replaced
        int kept_coarse = 0;  // regions SAM could not match
        double encoder_ms = 0;
        double total_ms = 0;
    };
    bool refineAvailable();
    // `out` is the refined coverage (coarse where nothing could be
    // refined). False only when the model cannot run.
    bool refine(const cv::Mat &base_bgr8, const cv::Mat &coarse, cv::Mat &out, std::string *error = nullptr,
                RefineReport *report = nullptr);

    // Face boxes (normalised, top-left origin) → feathered ellipses, each
    // grown by `grow` about its centre. `person_ids` empty = every face.
    struct FaceBox
    {
        double x = 0, y = 0, w = 0, h = 0;
        int person_id = 0;
        int cluster_id = 0;
    };
    void faces(const std::vector<FaceBox> &boxes, const std::vector<int> &person_ids, double grow,
               int width, int height, cv::Mat &out);

    // Guided filter (He et al.) of a coarse coverage against the base's
    // luminance at full mask size: sharp where the picture has edges, soft
    // where it does not. Exposed for tests.
    cv::Mat guidedUpsample(const cv::Mat &coarse, const cv::Mat &base_bgr8, double radius_fraction = 0.015,
                           double eps = 1e-3);
}
