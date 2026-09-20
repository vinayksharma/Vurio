#include "core/media_backfill.hpp"

#include "core/place_resolver.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <sys/stat.h>
#include <vector>

namespace MediaDedup
{
    void MediaBackfill::installHook()
    {
        MediaBackfillHook::install([](std::shared_ptr<DatabaseManager> db,
                                      MediaBackfillHook::ProgressFn progress,
                                      MediaBackfillHook::PhaseFn phase) {
            if (!db)
                return;
            if (phase)
                phase("Refreshing RAW photo metadata...");
            MediaBackfill::backfillRawExif(*db, Poco::Logger::get("MediaBackfill"), progress);
            // PLACE-1: GPS-tagged rows that never got a place name (older
            // rows, or rows scanned while the gazetteer was absent).
            if (phase)
                phase("Resolving place names...");
            PlaceResolver::instance().backfill(*db, Poco::Logger::get("MediaBackfill"), progress);
        });
    }

    namespace
    {
        // Cloud-evicted (dataless) placeholder: opening one makes the file
        // provider download the ENTIRE object. A metadata sweep must never
        // trigger a mass re-hydration of an evicted OneDrive/iCloud
        // library — treat these as offline; a later sweep picks them up
        // once they are local again.
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

    MediaBackfill::RawExifBackfillStats
    MediaBackfill::backfillRawExif(DatabaseManager &db, Poco::Logger &logger,
                                   const MediaBackfillHook::ProgressFn &progress)
    {
        // RAW-EXIF: the pre-fix LibRaw metadata path wrote only the combined
        // exif_camera, never exif_make — so "camera set, make NULL" is
        // exactly the population written by the old reader. A successful
        // re-read fills exif_make (LibRaw always reports one for files it
        // opens), which removes the row from this selector: the sweep is
        // idempotent and converges. Files it cannot read right now (e.g. on
        // a disconnected drive, or cloud-evicted) simply stay candidates
        // for a later sweep.
        //
        // Each read is a LibRaw metadata-only open (no demosaic, ms-scale),
        // so the sweep runs inline on this background thread instead of
        // re-processing the whole file through the media pipeline.
        RawExifBackfillStats stats;
        try
        {
            std::string ext_filter;
            for (const auto &ext : RawFileDetector::GetSupportedRawExtensions())
            {
                if (!ext_filter.empty())
                    ext_filter += " OR ";
                ext_filter += "lower(file_name) LIKE '%." + ext + "'";
            }

            std::vector<std::string> candidates;
            {
                auto lease = db.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                Poco::Data::Statement sel(sess);
                sel << "SELECT file_path FROM scanned_files "
                       "WHERE exif_camera IS NOT NULL AND exif_make IS NULL "
                       "AND (" + ext_filter + ")";
                sel.execute();
                Poco::Data::RecordSet rs(sel);
                for (bool more = rs.moveFirst(); more; more = rs.moveNext())
                    candidates.push_back(rs[0].convert<std::string>());
            }
            stats.candidates = candidates.size();
            if (candidates.empty())
                return stats;

            if (progress)
                progress(0, stats.candidates);
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                const auto &path = candidates[i];
                if (!std::filesystem::exists(path) || isDatalessPlaceholder(path))
                {
                    stats.offline++;
                }
                else
                {
                    auto exif = ExifReader::read(path);
                    if (exif && ScannedFilesOps::updateExifMetadata(db, path, *exif))
                        stats.updated++;
                    else
                        stats.unreadable++;
                }
                if (progress && ((i + 1) % 50 == 0 || i + 1 == candidates.size()))
                    progress(i + 1, stats.candidates);
            }
            logger.information(
                "RAW EXIF backfill: %d of %z updated (%d offline, %d unreadable).",
                stats.updated, stats.candidates, stats.offline, stats.unreadable);
        }
        catch (const std::exception &e)
        {
            logger.warning("RAW EXIF backfill failed: %s", std::string(e.what()));
        }
        return stats;
    }

} // namespace MediaDedup
