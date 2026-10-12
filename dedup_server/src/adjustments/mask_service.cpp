#include "adjustments/mask_service.hpp"
#include "adjustments/export_models.hpp"

#include <Poco/Logger.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        Poco::Logger &logger()
        {
            static Poco::Logger &l = Poco::Logger::get("MaskService");
            return l;
        }

        uint64_t fnv1a64(const std::string &text)
        {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : text)
            {
                h ^= c;
                h *= 1099511628211ull;
            }
            return h;
        }

        std::string hex64(uint64_t v)
        {
            static const char *digits = "0123456789abcdef";
            std::string out(16, '0');
            for (int i = 15; i >= 0; --i)
            {
                out[static_cast<size_t>(i)] = digits[v & 0xF];
                v >>= 4;
            }
            return out;
        }

        std::vector<int> parsePersonIds(const nlohmann::json &params)
        {
            std::vector<int> ids;
            if (!params.is_object() || !params.contains("person_ids"))
                return ids;
            const auto &v = params["person_ids"];
            std::string text = v.is_string() ? v.get<std::string>() : "";
            if (v.is_array())
                for (const auto &e : v)
                    if (e.is_number_integer())
                        ids.push_back(e.get<int>());
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find(',', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string part = text.substr(start, end - start);
                part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char c) { return std::isspace(c); }), part.end());
                if (!part.empty())
                {
                    try
                    {
                        ids.push_back(std::stoi(part));
                    }
                    catch (...)
                    {
                    }
                }
                start = end + 1;
            }
            return ids;
        }
    }

    MaskService &MaskService::instance()
    {
        static MaskService *service = new MaskService();
        return *service;
    }

    MaskService::~MaskService() { reset(); }

    const std::vector<std::string> &MaskService::detectedSources()
    {
        static const std::vector<std::string> ids = {"subject", "people", "faces", "prompt"};
        return ids;
    }

    void MaskService::configure(Config config, BaseLoader loader, FaceLookup faces, Notifier notifier)
    {
        reset();
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(config);
        loader_ = std::move(loader);
        faces_ = std::move(faces);
        notifier_ = std::move(notifier);
        if (config_.size < 256)
            config_.size = 256;
        MaskDetectors::configure(config_.models);
        if (!config_.cache_dir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(config_.cache_dir, ec);
            if (ec)
                logger().warning("Cannot create mask cache dir %s: %s", config_.cache_dir, ec.message());
        }
        logger().information("Mask service configured: %s, size %d, cache %s", std::string(config_.enabled ? "enabled" : "disabled"),
                             config_.size, config_.cache_dir);
    }

    void MaskService::setDetectorForTest(Detector detector)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        test_detector_ = std::move(detector);
    }

    void MaskService::reset()
    {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            queue_.clear();
            queued_.clear();
            queue_cv_.notify_all();
            worker = std::move(worker_);
        }
        if (worker.joinable())
            worker.join();
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        memory_.clear();
        lru_.clear();
        memory_bytes_ = 0;
        failed_.clear();
        in_flight_.clear();
        // The hooks hold the database and the decode paths; a stopped
        // server (or a test fixture) must not keep them alive.
        loader_ = nullptr;
        faces_ = nullptr;
        notifier_ = nullptr;
        MaskDetectors::unload();
    }

    bool MaskService::enabled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.enabled;
    }

    int MaskService::size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.size;
    }

    std::string MaskService::cacheKey(const std::string &file_path, const std::string &source,
                                      const nlohmann::json &canonical_params)
    {
        return hex64(fnv1a64(file_path + "|" + source + "|" + canonical_params.dump()));
    }

    std::string MaskService::cachePath(const std::string &key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.cache_dir.empty())
            return {};
        return (std::filesystem::path(config_.cache_dir) / (key + "_" + std::to_string(config_.size) + ".png")).string();
    }

    bool MaskService::available(const std::string &source) const
    {
        const auto &ids = detectedSources();
        if (std::find(ids.begin(), ids.end(), source) == ids.end())
            return true; // procedural: always
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!config_.enabled)
                return false;
            if (test_detector_)
                return true;
        }
        if (source == "faces")
            return static_cast<bool>(faces_);
        if (source == "prompt")
            return MaskDetectors::promptStatus().available;
        if (source == "people")
            return MaskDetectors::peopleStatus().available;
        if (source == "subject")
            return MaskDetectors::subjectBackendRegistered() || MaskDetectors::promptStatus().available;
        return false;
    }

    std::vector<MaskService::SourceStatus> MaskService::status() const
    {
        std::vector<SourceStatus> out;
        bool enabled;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            enabled = config_.enabled;
        }
        auto prompt = MaskDetectors::promptStatus();
        auto people = MaskDetectors::peopleStatus();
        bool vision = MaskDetectors::subjectBackendRegistered();
        {
            SourceStatus s;
            s.id = "subject";
            s.backend = vision ? "vision" : "onnx";
            s.model_path = vision ? "" : prompt.model_path;
            s.model_present = vision || prompt.model_present;
            s.loaded = vision || prompt.loaded;
            s.available = enabled && (vision || prompt.available);
            s.note = vision ? "Apple Vision foreground lifting" : (prompt.available ? "CLIPSeg \"the main subject\"" : prompt.note);
            out.push_back(s);
        }
        {
            SourceStatus s;
            s.id = "people";
            s.backend = "onnx";
            s.model_path = people.model_path;
            s.model_present = people.model_present;
            s.loaded = people.loaded;
            s.available = enabled && people.available;
            s.note = people.available ? "MediaPipe selfie segmentation" : people.note;
            out.push_back(s);
        }
        {
            SourceStatus s;
            s.id = "faces";
            s.backend = "database";
            s.available = enabled && static_cast<bool>(faces_);
            s.model_present = true;
            s.loaded = true;
            s.note = s.available ? "face boxes from the library" : "face detection disabled";
            out.push_back(s);
        }
        {
            SourceStatus s;
            s.id = "prompt";
            s.backend = "onnx";
            s.model_path = prompt.model_path;
            s.model_present = prompt.model_present;
            s.loaded = prompt.loaded;
            s.available = enabled && prompt.available;
            s.note = prompt.available ? "CLIPSeg rd64-refined (int8)" : prompt.note;
            out.push_back(s);
        }
        {
            // CHAIN-5b: not a source of its own; the sharpener behind the
            // prompt, people and portable subject masks.
            auto refine = MaskDetectors::refineStatus();
            SourceStatus s;
            s.id = "refine";
            s.backend = "onnx";
            s.model_path = refine.model_path;
            s.model_present = refine.model_present;
            s.loaded = refine.loaded;
            s.available = enabled && refine.available;
            s.note = refine.available ? "SlimSAM-77 sharpens prompt, people and portable subject masks" : refine.note;
            out.push_back(s);
        }
        // CHAIN-7h: the export-time models, in the same list so Settings
        // shows every optional model in one place.
        {
            for (const auto &[id, st] : {std::pair<const char *, ExportModels::Status>{"denoise", ExportModels::denoiseStatus()},
                                          std::pair<const char *, ExportModels::Status>{"upscale", ExportModels::upscaleStatus()}})
            {
                SourceStatus s;
                s.id = id;
                s.backend = "onnx";
                s.model_path = st.model_path;
                s.model_present = st.model_present;
                s.loaded = st.loaded;
                s.available = st.available;
                s.note = st.note;
                out.push_back(s);
            }
        }
        if (!enabled)
            for (auto &s : out)
                s.note = "detected masks disabled (media.adjustments.masks.enabled)";
        return out;
    }

    MaskService::CacheStats MaskService::cacheStats() const
    {
        CacheStats stats;
        std::string dir;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dir = config_.cache_dir;
            stats.memory_entries = memory_.size();
            stats.pending = queue_.size() + in_flight_.size();
        }
        if (dir.empty())
            return stats;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            if (entry.path().extension() != ".png")
                continue;
            ++stats.files;
            stats.bytes += static_cast<size_t>(entry.file_size(ec));
        }
        return stats;
    }

    void MaskService::clearCache()
    {
        std::string dir;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dir = config_.cache_dir;
            memory_.clear();
            lru_.clear();
            memory_bytes_ = 0;
            failed_.clear();
        }
        if (dir.empty())
            return;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".png")
                std::filesystem::remove(entry.path(), ec);
        logger().information("Mask cache cleared: %s", dir);
    }

    // ---- memory LRU ------------------------------------------------------

    void MaskService::remember(const std::string &key, const cv::Mat &mask)
    {
        // Caller holds mutex_.
        size_t bytes = mask.total() * mask.elemSize();
        auto it = memory_.find(key);
        if (it != memory_.end())
        {
            lru_.erase(it->second.lru);
            memory_bytes_ -= it->second.mask.total() * it->second.mask.elemSize();
            memory_.erase(it);
        }
        lru_.push_front(key);
        memory_[key] = Entry{mask, lru_.begin()};
        memory_bytes_ += bytes;
        while (memory_bytes_ > config_.memory_bytes && lru_.size() > 1)
        {
            const std::string &victim = lru_.back();
            auto v = memory_.find(victim);
            if (v != memory_.end())
            {
                memory_bytes_ -= v->second.mask.total() * v->second.mask.elemSize();
                memory_.erase(v);
            }
            lru_.pop_back();
        }
    }

    bool MaskService::recall(const std::string &key, cv::Mat &out)
    {
        // Caller holds mutex_.
        auto it = memory_.find(key);
        if (it == memory_.end())
            return false;
        lru_.erase(it->second.lru);
        lru_.push_front(key);
        it->second.lru = lru_.begin();
        out = it->second.mask;
        return true;
    }

    // ---- disk cache ------------------------------------------------------

    bool MaskService::loadFromDisk(const std::string &key, cv::Mat &out)
    {
        std::string path = cachePath(key);
        if (path.empty())
            return false;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return false;
        cv::Mat png = cv::imread(path, cv::IMREAD_GRAYSCALE);
        if (png.empty())
            return false;
        png.convertTo(out, CV_32F, 1.0 / 255.0);
        return true;
    }

    void MaskService::storeToDisk(const std::string &key, const cv::Mat &mask) const
    {
        std::string path = cachePath(key);
        if (path.empty())
            return;
        cv::Mat png;
        mask.convertTo(png, CV_8U, 255.0);
        std::vector<uint8_t> bytes;
        try
        {
            std::vector<int> params{cv::IMWRITE_PNG_COMPRESSION, 3};
            if (!cv::imencode(".png", png, bytes, params))
                return;
        }
        catch (const std::exception &e)
        {
            logger().warning("Mask PNG encode failed: %s", std::string(e.what()));
            return;
        }
        // Written beside the destination and renamed into place so a
        // reader never sees a half file.
        std::string tmp = path + ".part";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
            std::filesystem::remove(tmp, ec);
    }

    // ---- detection -------------------------------------------------------

    bool MaskService::detectFaces(const Job &job, cv::Mat &out, std::string &error)
    {
        FaceLookup faces;
        BaseLoader loader;
        int size;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            faces = faces_;
            loader = loader_;
            size = config_.size;
        }
        if (!faces)
        {
            error = "face lookup unavailable";
            return false;
        }
        std::vector<MaskDetectors::FaceBox> boxes = faces(job.file_path);
        // The base's aspect, without decoding when the library knows it.
        Size base_full;
        cv::Mat base;
        if (loader)
            base = loader(job.file_path, size, base_full);
        int w = base.cols, h = base.rows;
        if (w <= 0 || h <= 0)
        {
            if (base_full.width <= 0 || base_full.height <= 0)
            {
                error = "cannot size the base for face masks";
                return false;
            }
            double scale = size / std::max(base_full.width, base_full.height);
            w = std::max(1, static_cast<int>(std::lround(base_full.width * scale)));
            h = std::max(1, static_cast<int>(std::lround(base_full.height * scale)));
        }
        double grow = job.params.is_object() && job.params.contains("grow") && job.params["grow"].is_number()
                          ? job.params["grow"].get<double>()
                          : 1.4;
        MaskDetectors::faces(boxes, parsePersonIds(job.params), grow, w, h, out);
        return true;
    }

    bool MaskService::detect(const Job &job, cv::Mat &out, std::string &error)
    {
        Detector test;
        BaseLoader loader;
        int size;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            test = test_detector_;
            loader = loader_;
            size = config_.size;
        }
        if (job.source == "faces" && !test)
            return detectFaces(job, out, error);
        if (!loader)
        {
            error = "no base loader";
            return false;
        }
        Size base_full;
        cv::Mat base = loader(job.file_path, size, base_full);
        if (base.empty())
        {
            error = "cannot decode the base";
            return false;
        }
        if (std::max(base.cols, base.rows) > size)
        {
            double s = static_cast<double>(size) / std::max(base.cols, base.rows);
            cv::resize(base, base, cv::Size(), s, s, cv::INTER_AREA);
        }
        if (test)
            return test(job.source, job.params, base, out, error);
        bool wants_refine = job.params.is_object() && job.params.contains("_refine");
        bool platform = false;
        cv::Mat coarse;
        if (job.source == "prompt")
        {
            std::string text = job.params.is_object() && job.params.contains("text") && job.params["text"].is_string()
                                   ? job.params["text"].get<std::string>()
                                   : "";
            if (text.empty())
            {
                out = cv::Mat(base.rows, base.cols, CV_32FC1, cv::Scalar(0.0f));
                return true;
            }
            if (!MaskDetectors::prompt(base, text, coarse, &error))
                return false;
        }
        else if (job.source == "people")
        {
            if (!MaskDetectors::people(base, coarse, &error))
                return false;
        }
        else if (job.source == "subject")
        {
            if (!MaskDetectors::subject(base, coarse, &error, &platform))
                return false;
        }
        else
        {
            error = "unknown detected source " + job.source;
            return false;
        }
        // CHAIN-5b: SAM sharpens the coarse coverage; a failure keeps it.
        if (wants_refine && !platform)
        {
            MaskDetectors::RefineReport report;
            std::string refine_error;
            cv::Mat refined;
            if (MaskDetectors::refine(base, coarse, refined, &refine_error, &report))
            {
                logger().information("Mask %s refined for %s: %d regions, %d sharpened, %d kept, encoder %.0f ms, total %.0f ms",
                                     job.source, job.file_path, report.components, report.refined, report.kept_coarse,
                                     report.encoder_ms, report.total_ms);
                out = refined;
                return true;
            }
            logger().warning("Mask %s for %s stays coarse: %s", job.source, job.file_path, refine_error);
        }
        out = coarse;
        return true;
    }

    MaskService::Status MaskService::runOrWait(const Job &job, cv::Mat &out)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Someone else is on it: wait for them.
            while (in_flight_.count(job.key))
                done_cv_.wait(lock);
            if (recall(job.key, out))
                return Status::Ready;
            if (failed_.count(job.key))
                return Status::Unavailable;
            in_flight_.insert(job.key);
        }
        cv::Mat mask;
        std::string error;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        try
        {
            ok = detect(job, mask, error);
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ok && !mask.empty())
        {
            storeToDisk(job.key, mask);
            logger().information("Mask %s computed for %s in %Ld ms", job.source, job.file_path, static_cast<long long>(ms));
        }
        else
            logger().warning("Mask %s failed for %s: %s", job.source, job.file_path, error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok && !mask.empty())
                remember(job.key, mask);
            else
                failed_.insert(job.key);
            in_flight_.erase(job.key);
            done_cv_.notify_all();
        }
        if (ok && !mask.empty())
        {
            out = mask;
            return Status::Ready;
        }
        return Status::Unavailable;
    }

    nlohmann::json MaskService::effectiveParams(const std::string &source, const nlohmann::json &canonical_params) const
    {
        // CHAIN-5b: the key says whether SAM will refine, so a mask cached
        // before the model arrived is not mistaken for a refined one.
        if (source != "prompt" && source != "people" && source != "subject")
            return canonical_params;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (test_detector_)
                return canonical_params;
        }
        if (canonical_params.is_object() && canonical_params.contains("refine") &&
            canonical_params["refine"].is_boolean() && !canonical_params["refine"].get<bool>())
            return canonical_params;
        if (source == "subject" && MaskDetectors::subjectBackendRegistered())
            return canonical_params; // Vision's mask is sharp already
        if (!MaskDetectors::refineAvailable())
            return canonical_params;
        nlohmann::json effective = canonical_params.is_object() ? canonical_params : nlohmann::json::object();
        effective["_refine"] = "slimsam";
        return effective;
    }

    MaskService::Status MaskService::coverage(const std::string &file_path, const std::string &source,
                                              const nlohmann::json &params, bool inline_compute, cv::Mat &out)
    {
        if (!available(source))
            return Status::Unavailable;
        nlohmann::json canonical_params = effectiveParams(source, params);
        std::string key = cacheKey(file_path, source, canonical_params);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (recall(key, out))
                return Status::Ready;
            if (failed_.count(key))
                return Status::Unavailable;
        }
        if (loadFromDisk(key, out))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remember(key, out);
            return Status::Ready;
        }
        Job job{key, file_path, source, canonical_params};
        // Faces are cheap (boxes from the library): always inline.
        if (inline_compute || source == "faces")
            return runOrWait(job, out);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queued_.count(key) && !in_flight_.count(key))
            {
                queue_.push_back(job);
                queued_.insert(key);
                startWorker();
                queue_cv_.notify_one();
            }
        }
        return Status::Pending;
    }

    void MaskService::startWorker()
    {
        // Caller holds mutex_.
        if (worker_.joinable())
            return;
        stop_ = false;
        worker_ = std::thread([this] { workerLoop(); });
    }

    void MaskService::workerLoop()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                queue_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_)
                    return;
                job = queue_.front();
                queue_.pop_front();
                queued_.erase(job.key);
            }
            cv::Mat mask;
            runOrWait(job, mask);
            Notifier notifier;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                notifier = notifier_;
            }
            if (notifier)
                notifier(job.file_path);
        }
    }
}
