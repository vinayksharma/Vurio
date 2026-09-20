#include "core/place_resolver.hpp"

#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/places_ops.hpp"
#include "search/location_geocoder.hpp"
#include "search/offline_reverse_geocoder.hpp"

#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

#include <filesystem>

namespace MediaDedup
{
    PlaceResolver &PlaceResolver::instance()
    {
        static PlaceResolver resolver;
        return resolver;
    }

    PlaceResolver::~PlaceResolver()
    {
        stopBackgroundBackfill();
    }

    bool PlaceResolver::configure(const std::shared_ptr<UnifiedObservableConfigManager> &config)
    {
        Poco::Logger &logger = Poco::Logger::get("PlaceResolver");
        std::string places_path = "geodata/places.sqlite";
        std::string bounds_path = "geodata/world_locations.geojson";
        OfflineReverseGeocoder::Options opts;
        if (config)
        {
            places_path = config->getPropertyValue<std::string>("search.placesDbPath", places_path);
            bounds_path = config->getPropertyValue<std::string>("search.locationsPath", bounds_path);
            opts.landmark_radius_m = config->getPropertyValue<int>(
                "search.places.landmarkRadiusM", static_cast<int>(opts.landmark_radius_m));
            opts.locality_max_km = config->getPropertyValue<int>(
                "search.places.localityMaxKm", static_cast<int>(opts.locality_max_km));

            // Same project-root resolution as TextSearchEngine.
            std::filesystem::path abs_config = std::filesystem::absolute(config->getConfigFilePath());
            std::filesystem::path project_root = abs_config.parent_path().parent_path();
            auto resolve = [&](std::string &p) {
                std::filesystem::path fp(p);
                if (!fp.is_absolute())
                    p = (project_root / fp).lexically_normal().string();
            };
            resolve(places_path);
            resolve(bounds_path);
        }

        auto geocoder = std::make_shared<OfflineReverseGeocoder>();
        if (!geocoder->open(places_path, opts))
        {
            logger.warning("Offline place gazetteer unavailable (%s); place names disabled "
                           "until it is built (scripts/build_places_db.py)",
                           places_path);
            geocoder.reset();
        }
        std::shared_ptr<LocationGeocoder> bounds;
        if (geocoder)
        {
            bounds = std::make_shared<LocationGeocoder>();
            if (!bounds->loadFromFile(bounds_path))
            {
                logger.warning("Bounds gazetteer unavailable (%s); park containment disabled", bounds_path);
                bounds.reset();
            }
            geocoder->setBoundsGazetteer(std::shared_ptr<const LocationGeocoder>(bounds));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        geocoder_ = std::move(geocoder);
        bounds_ = std::move(bounds);
        return geocoder_ != nullptr;
    }

    void PlaceResolver::setGeocodersForTests(std::shared_ptr<OfflineReverseGeocoder> geocoder,
                                             std::shared_ptr<LocationGeocoder> bounds)
    {
        stopBackgroundBackfill();
        std::lock_guard<std::mutex> lock(mutex_);
        if (geocoder)
            geocoder->setBoundsGazetteer(std::shared_ptr<const LocationGeocoder>(bounds));
        geocoder_ = std::move(geocoder);
        bounds_ = std::move(bounds);
    }

    bool PlaceResolver::available() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return geocoder_ && geocoder_->isOpen();
    }

    bool PlaceResolver::resolveFile(DatabaseManager &db, int file_id, double lat, double lon)
    {
        std::shared_ptr<OfflineReverseGeocoder> geocoder;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            geocoder = geocoder_;
        }
        if (!geocoder || !geocoder->isOpen() || file_id <= 0)
            return false;

        auto res = geocoder->resolve(lat, lon);
        if (!res)
            return false;

        std::vector<PlacesOps::FilePlace> rows;
        rows.reserve(res->places.size());
        for (const auto &p : res->places)
        {
            PlacesOps::FilePlace fp;
            fp.place.source = p.source;
            fp.place.external_id = p.external_id;
            fp.place.name = p.name;
            fp.place.kind = p.kind;
            fp.place.fcode = p.fcode;
            fp.place.lat = p.lat;
            fp.place.lon = p.lon;
            fp.place.cc = p.cc;
            fp.role = p.role;
            fp.distance_m = p.distance_m;
            rows.push_back(std::move(fp));
        }
        return PlacesOps::setFilePlaces(db, file_id, rows, res->label, PlacesOps::kLevelOffline,
                                        res->shore_distance_m);
    }

    bool PlaceResolver::resolvePath(DatabaseManager &db, const std::string &file_path,
                                    double lat, double lon)
    {
        if (!available())
            return false;
        int file_id = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::string p = file_path;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id FROM scanned_files WHERE file_path = ? LIMIT 1",
                Poco::Data::Keywords::use(p), Poco::Data::Keywords::into(file_id),
                Poco::Data::Keywords::now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PlaceResolver").warning("resolvePath(%s): %s", file_path,
                                                       std::string(e.what()));
            return false;
        }
        return file_id > 0 && resolveFile(db, file_id, lat, lon);
    }

    PlaceResolver::SweepStats PlaceResolver::backfill(DatabaseManager &db, Poco::Logger &logger,
                                                      const ProgressFn &progress)
    {
        SweepStats stats;
        if (!available())
        {
            logger.information("Place sweep skipped: no offline gazetteer");
            return stats;
        }
        bool expected = false;
        if (!sweep_running_.compare_exchange_strong(expected, true))
        {
            logger.information("Place sweep skipped: another sweep is running");
            return stats;
        }
        struct Release
        {
            std::atomic<bool> &flag;
            ~Release() { flag = false; }
        } release{sweep_running_};
        try
        {
            auto candidates = PlacesOps::listUnresolved(db, PlacesOps::kLevelOffline, 0);
            stats.candidates = candidates.size();
            if (candidates.empty())
                return stats;
            if (progress)
                progress(0, stats.candidates);
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                if (stop_requested_.load())
                {
                    logger.information("Place sweep stopped at %z of %z.", i, stats.candidates);
                    break;
                }
                const auto &c = candidates[i];
                if (resolveFile(db, c.file_id, c.lat, c.lon))
                    stats.resolved++;
                else
                    stats.failed++;
                if (progress && ((i + 1) % 50 == 0 || i + 1 == candidates.size()))
                    progress(i + 1, stats.candidates);
            }
            logger.information("Place sweep: %d of %z resolved (%d failed).",
                               stats.resolved, stats.candidates, stats.failed);
        }
        catch (const std::exception &e)
        {
            logger.warning("Place sweep failed: %s", std::string(e.what()));
        }
        return stats;
    }

    void PlaceResolver::startBackgroundBackfill(std::shared_ptr<DatabaseManager> db)
    {
        Poco::Logger &logger = Poco::Logger::get("PlaceResolver");
        if (!db || !available())
            return;
        const int pending = PlacesOps::countUnresolved(*db, PlacesOps::kLevelOffline);
        if (pending <= 0)
            return;
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_.joinable())
        {
            if (sweep_running_.load())
            {
                logger.information("Place names pending for %d GPS photo(s); background sweep already running", pending);
                return;
            }
            thread_.join(); // a previous sweep that finished on its own
        }
        logger.information("Place names pending for %d GPS photo(s); resolving in the background", pending);
        stop_requested_ = false;
        thread_ = std::thread([this, db = std::move(db)] {
            backfill(*db, Poco::Logger::get("PlaceResolver"));
        });
    }

    void PlaceResolver::stopBackgroundBackfill()
    {
        stop_requested_ = true;
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_.joinable())
        {
            if (thread_.get_id() == std::this_thread::get_id())
                thread_.detach(); // never from within the sweep itself
            else
                thread_.join();
        }
        stop_requested_ = false;
    }
}
