#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace Poco
{
    class Logger;
}

namespace MediaDedup
{
    class DatabaseManager;

    /**
     * @brief Seam between the database integrity check and the RAW EXIF
     * backfill sweep.
     *
     * The sweep needs ExifReader (LibRaw/ImageMagick); linking that into
     * every test binary that compiles integrity_check_service.cpp is not
     * acceptable. The integrity check therefore calls through this hook,
     * and the real implementation (media_backfill.cpp, linked only into
     * the server) registers itself at startup. Unregistered (tests) →
     * no-op.
     *
     * The former thumbnail/preview backfill is gone: thumbnails and
     * previews are generated on demand by design, and pre-generating
     * them here fought that design (it also never ran — see the
     * unexecuted-statement fix — so removing it restores the actual
     * long-standing behavior).
     */
    class MediaBackfillHook
    {
    public:
        /// Called by the sweep as it progresses: (files done, files total).
        using ProgressFn = std::function<void(std::size_t, std::size_t)>;
        /// PLACE-1: the hook now runs several sweeps in turn; each announces
        /// itself through this so the integrity dialog names the phase.
        using PhaseFn = std::function<void(const std::string &)>;

        using Fn = std::function<void(std::shared_ptr<DatabaseManager>, ProgressFn, PhaseFn)>;

        static void install(Fn fn);

        /// Runs the installed backfill; silently does nothing when none is installed.
        static void run(std::shared_ptr<DatabaseManager> db, ProgressFn progress, PhaseFn phase = {});
    };

    /**
     * @brief The real backfill, linked into the server binary only and
     * installed onto MediaBackfillHook at startup.
     */
    class MediaBackfill
    {
    public:
        static void installHook();

        /**
         * @brief RAW-EXIF: sweep refilling the rich EXIF columns for RAW
         * rows written by the pre-fix LibRaw reader (which captured only
         * camera/datetime/dimensions). Selector: RAW extension with
         * exif_camera set but exif_make NULL — a successful re-read fills
         * exif_make, so the sweep converges. Runs inline (LibRaw
         * metadata-only opens, no demosaic) on the integrity-check thread,
         * reporting progress so the UI can show it.
         *
         * Cloud-evicted (dataless) files are counted as offline, never
         * read — a metadata sweep must not trigger a mass re-download of
         * an evicted OneDrive/iCloud library.
         *
         * Returns the sweep's counts so tests can observe the selector —
         * the original implementation never executed its Poco statement,
         * which silently returned zero candidates forever.
         */
        struct RawExifBackfillStats
        {
            std::size_t candidates = 0;
            int updated = 0;
            int offline = 0;
            int unreadable = 0;
        };
        static RawExifBackfillStats backfillRawExif(
            DatabaseManager &db, Poco::Logger &logger,
            const MediaBackfillHook::ProgressFn &progress = {});
    };

} // namespace MediaDedup
