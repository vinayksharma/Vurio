#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Poco
{
    class Logger;
}

namespace MediaDedup
{
    class DatabaseManager;
    class LocationGeocoder;
    class OfflineReverseGeocoder;
    class UnifiedObservableConfigManager;

    /**
     * @brief PLACE-1: turns a file's GPS fix into place rows + a label.
     *
     * Process-wide (the scan pipeline and the integrity-check sweep both
     * need the same open gazetteer), configured once at startup from
     * `search.placesDbPath` (the GeoNames SQLite from
     * scripts/build_places_db.py) and `search.locationsPath` (the small
     * bbox gazetteer, for park containment). Without a gazetteer file
     * every call is a cheap no-op and place_level stays 0 — the sweep
     * picks those files up once the file appears.
     *
     * Entirely offline: nothing here ever opens a socket. Online lookup
     * (PLACE-3) is a separate, consent-gated client-side feature.
     */
    class PlaceResolver
    {
    public:
        using ProgressFn = std::function<void(std::size_t, std::size_t)>;

        struct SweepStats
        {
            std::size_t candidates = 0;
            int resolved = 0;
            int failed = 0;
        };

        static PlaceResolver &instance();

        /// Opens the gazetteers named in config. Returns available().
        bool configure(const std::shared_ptr<UnifiedObservableConfigManager> &config);

        /// Test seam: inject already-open geocoders (either may be null).
        void setGeocodersForTests(std::shared_ptr<OfflineReverseGeocoder> geocoder,
                                  std::shared_ptr<LocationGeocoder> bounds);

        bool available() const;

        /// Resolve one file and store its places/label. False when the
        /// resolver is unavailable or the write failed.
        bool resolveFile(DatabaseManager &db, int file_id, double lat, double lon);
        bool resolvePath(DatabaseManager &db, const std::string &file_path, double lat, double lon);

        /// Sweep: every GPS-tagged file still below the offline level.
        /// Only one sweep runs at a time: a second caller (the boot sweep
        /// and the integrity check both come here) returns at once with
        /// empty stats. Stops early when stopBackgroundBackfill() is called.
        SweepStats backfill(DatabaseManager &db, Poco::Logger &logger,
                            const ProgressFn &progress = {});

        /// Run backfill() on an owned thread (boot: name what an older
        /// build left unresolved without waiting for an integrity check).
        /// Call after every schema migration has run — the sweep writes
        /// scanned_files continuously. No-op when unavailable, nothing is
        /// pending, or a sweep is already running.
        void startBackgroundBackfill(std::shared_ptr<DatabaseManager> db);

        /// Ask a running sweep to stop after its current file and join the
        /// thread. Safe to call when none is running; called at shutdown
        /// and from the destructor.
        void stopBackgroundBackfill();

        ~PlaceResolver();

    private:
        PlaceResolver() = default;

        mutable std::mutex mutex_;
        std::shared_ptr<OfflineReverseGeocoder> geocoder_;
        std::shared_ptr<LocationGeocoder> bounds_;

        std::atomic<bool> sweep_running_{false};
        std::atomic<bool> stop_requested_{false};
        std::mutex thread_mutex_;
        std::thread thread_;
    };
}
