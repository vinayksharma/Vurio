#include "media_processors/face/face_scan_service.hpp"

#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"
#include "media_processors/face/face_align.hpp"

#include <Poco/Logger.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

namespace MediaDedup
{
    namespace
    {
        Poco::Logger &scanLogger()
        {
            static Poco::Logger &logger = Poco::Logger::get("FaceScanService");
            return logger;
        }

        // Cloud-evicted placeholder: reading one forces a full download —
        // a sweep must never trigger a mass re-hydration (same rule as the
        // RAW EXIF backfill).
        bool isDatalessPlaceholder(const std::string &path)
        {
#ifdef SF_DATALESS
            struct stat st{};
            if (::stat(path.c_str(), &st) != 0)
                return false;
            return (st.st_flags & SF_DATALESS) != 0;
#else
            (void)path;
            return false;
#endif
        }
    }

    FaceScanService::FaceScanService(DatabaseManager *db, std::unique_ptr<FaceDetector> detector,
                                     EmbedFn embed, Options options)
        : db_(db), detector_(std::move(detector)), embed_(std::move(embed)),
          options_(options)
    {
    }

    std::vector<float> FaceScanService::preprocessToNchw(const std::vector<std::uint8_t> &rgb112)
    {
        constexpr int kSize = FaceAlign::kSize;
        constexpr std::size_t kPixels = static_cast<std::size_t>(kSize) * kSize;
        std::vector<float> nchw;
        if (rgb112.size() != kPixels * 3)
            return nchw;
        nchw.resize(kPixels * 3);
        for (std::size_t i = 0; i < kPixels; ++i)
            for (int c = 0; c < 3; ++c)
                nchw[c * kPixels + i] = (rgb112[i * 3 + c] - 127.5f) / 128.0f;
        return nchw;
    }

    FaceScanService::TickStats FaceScanService::runTick()
    {
        TickStats stats;
        if (!db_ || !detector_ || !embed_)
            return stats;

        // Over-fetch by the files already known offline so a disconnected
        // volume at the head of the id order can't starve the batch.
        const int fetch = options_.batch + static_cast<int>(skipped_offline_.size());
        auto candidates = FacesOps::listSweepCandidates(*db_, fetch);
        stats.exhausted = static_cast<int>(candidates.size()) < fetch;

        int processed = 0;
        for (const auto &c : candidates)
        {
            if (processed >= options_.batch)
                break;
            if (skipped_offline_.count(c.file_id))
                continue;
            if (!std::filesystem::exists(c.file_path) || isDatalessPlaceholder(c.file_path))
            {
                skipped_offline_.insert(c.file_id);
                stats.offline++;
                continue;
            }
            processed++;

            ImageBuffer image = detector_->loadImage(c.file_path, options_.max_dim);
            if (!image.valid())
            {
                FacesOps::markFileScanned(*db_, c.file_id, -1); // attempted, unknown
                stats.unreadable++;
                stats.scanned++;
                continue;
            }

            // Re-scan safety: a version bump must not duplicate rows.
            FacesOps::deleteFacesForFile(*db_, c.file_id);

            int stored = 0;
            for (const auto &face : detector_->detect(image))
            {
                FacesOps::FaceRow row;
                row.file_id = c.file_id;
                row.bbox_x = face.bbox_x;
                row.bbox_y = face.bbox_y;
                row.bbox_w = face.bbox_w;
                row.bbox_h = face.bbox_h;
                row.quality = face.quality;

                std::vector<std::uint8_t> crop;
                std::vector<float> embedding;
                if (FaceAlign::alignFace(image, face, crop))
                {
                    std::vector<float> raw;
                    if (embed_(preprocessToNchw(crop), raw) &&
                        static_cast<int>(raw.size()) == options_.embedding_dim)
                    {
                        double norm_sq = 0;
                        for (float v : raw)
                            norm_sq += static_cast<double>(v) * v;
                        if (norm_sq > 0)
                        {
                            const float inv = static_cast<float>(1.0 / std::sqrt(norm_sq));
                            for (float &v : raw)
                                v *= inv;
                            row.embedding.resize(raw.size() * sizeof(float));
                            std::memcpy(row.embedding.data(), raw.data(), row.embedding.size());
                        }
                    }
                }
                // A face without an embedding (no landmarks, embedder off)
                // is still stored: it counts for face_count and can be
                // re-embedded by a version bump later.
                if (FacesOps::insertFace(*db_, row) > 0)
                    stored++;
            }
            FacesOps::markFileScanned(*db_, c.file_id, stored);
            stats.scanned++;
            stats.faces += stored;
        }

        if (stats.scanned > 0 || stats.offline > 0)
            scanLogger().information(
                "Face sweep tick: %d file(s) scanned, %d face(s), %d offline, %d unreadable%s",
                stats.scanned, stats.faces, stats.offline, stats.unreadable,
                std::string(stats.exhausted ? " (caught up)" : ""));
        return stats;
    }
}
