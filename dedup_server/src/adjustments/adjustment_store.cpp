#include "adjustments/adjustment_store.hpp"
#include "adjustments/sidecar_store.hpp"
#include "adjustments/tool_registry.hpp"
#include "config/unified_observable_config.hpp"
#include "core/grpc_server/full_res_cache_registry.hpp"
#include "core/media_generation_failures.hpp"
#include "core/media_ready_bus.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "filesmanager/disk_cache.hpp"
#include "filesmanager/file_scanner.hpp"
#include "filesmanager/files_service.hpp"
#include "filesmanager/media_availability.hpp"
#include <Poco/Logger.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace MediaDedup::Adjustments
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("AdjustmentStore"); }

        int64_t nowSeconds()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        // Commits and imports are serialized: two writers on one file must
        // not interleave their row/sidecar/map updates.
        std::mutex &writeMutex()
        {
            static std::mutex m;
            return m;
        }

        // A directory we may create files under: its volume is mounted and
        // it exists. Writing beneath an unmounted /Volumes/<x> would create
        // a phantom local folder that shadows the real mount later.
        bool directoryReachable(const std::string &dir)
        {
            if (dir.empty())
                return false;
            auto mounts = MediaAvailability::mountedFilesystems();
            if (!MediaAvailability::isPathAvailable(dir, mounts))
                return false;
            std::error_code ec;
            return fs::is_directory(dir, ec) && !ec;
        }

        Size toSize(const DisplayDims &d) { return {static_cast<double>(d.width), static_cast<double>(d.height)}; }

        int64_t sizeBytesOf(const ScannedFileRow &row, const std::string &path)
        {
            try
            {
                auto meta = nlohmann::json::parse(row.file_metadata);
                if (meta.contains("sizeBytes"))
                    return meta["sizeBytes"].get<int64_t>();
            }
            catch (...)
            {
            }
            std::error_code ec;
            auto bytes = fs::file_size(path, ec);
            return ec ? 0 : static_cast<int64_t>(bytes);
        }
    }

    AdjustmentStore &AdjustmentStore::instance()
    {
        static AdjustmentStore store;
        return store;
    }

    void AdjustmentStore::initialize(std::shared_ptr<DatabaseManager> db,
                                     std::shared_ptr<UnifiedObservableConfigManager> config,
                                     std::string spool_root,
                                     std::shared_ptr<DiskCache> thumbnail_cache)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        db_ = std::move(db);
        config_ = std::move(config);
        spool_root_ = std::move(spool_root);
        thumbnail_cache_ = std::move(thumbnail_cache);
        entries_.clear();
        initialized_ = db_ != nullptr;
        if (!initialized_)
            return;

        AdjustmentsOps::ensureTable(*db_);
        int loaded = 0, unreadable = 0;
        for (ImageAdjustmentRow &row : AdjustmentsOps::listAll(*db_))
        {
            Entry e;
            e.row = row;
            AdjustmentDocument doc;
            std::string error;
            if (AdjustmentDocument::parse(row.document, doc, error) &&
                doc.compile(ToolRegistry::instance(), e.chain, error))
            {
                ++loaded;
            }
            else
            {
                // Keep the row's revision (so caches stay keyed consistently)
                // but render the base image: this build cannot apply it.
                ++unreadable;
                log().warning("Adjustments for %s cannot be applied by this build: %s", row.file_path, error);
                e.chain = RenderChain();
                e.chain.setRevision(row.render_revision);
            }
            entries_[row.file_path] = std::move(e);
        }
        log().information("Adjustment store loaded: %d edited files (%d not applicable), spool at %s",
                          loaded, unreadable, spool_root_);
    }

    bool AdjustmentStore::isInitialized() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return initialized_;
    }

    std::shared_ptr<DatabaseManager> AdjustmentStore::database() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return db_;
    }

    void AdjustmentStore::reset()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        entries_.clear();
        db_.reset();
        config_.reset();
        thumbnail_cache_.reset();
        spool_root_.clear();
        initialized_ = false;
    }

    bool AdjustmentStore::enabled() const
    {
        std::shared_ptr<UnifiedObservableConfigManager> cfg;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!initialized_)
                return false;
            cfg = config_;
        }
        return cfg ? cfg->getPropertyValue<bool>("media.adjustments.enabled", true) : true;
    }

    std::string AdjustmentStore::suffix() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::string s = config_ ? config_->getPropertyValue<std::string>("media.adjustments.sidecar.suffix",
                                                                        SidecarLayout::kDefaultSuffix)
                                : SidecarLayout::kDefaultSuffix;
        return s.empty() ? SidecarLayout::kDefaultSuffix : s;
    }

    bool AdjustmentStore::sidecarWritesEnabled() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return config_ ? config_->getPropertyValue<bool>("media.adjustments.sidecar.write", true) : true;
    }

    // ------------------------------------------------------------ reads

    RenderChain AdjustmentStore::chainFor(const std::string &file_path) const
    {
        if (!enabled())
            return RenderChain();
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = entries_.find(file_path);
        if (it == entries_.end())
            return RenderChain();
        return it->second.chain;
    }

    int AdjustmentStore::revisionFor(const std::string &file_path) const
    {
        if (!enabled())
            return 0;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = entries_.find(file_path);
        return it == entries_.end() ? 0 : it->second.chain.revision();
    }

    DisplayDims AdjustmentStore::displayDimsFor(const std::string &file_path, int base_width, int base_height) const
    {
        if (!enabled())
            return computeDisplayDimensions(base_width, base_height);
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = entries_.find(file_path);
        if (it == entries_.end())
            return computeDisplayDimensions(base_width, base_height);
        return it->second.chain.displayDims(base_width, base_height);
    }

    std::optional<ImageAdjustmentRow> AdjustmentStore::rowFor(const std::string &file_path) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = entries_.find(file_path);
        if (it == entries_.end())
            return std::nullopt;
        return it->second.row;
    }

    DisplayDims AdjustmentStore::baseDimsFor(const std::string &file_path) const
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (!db)
            return {};
        auto row = ScannedFilesOps::getByPath(*db, file_path);
        if (!row || !row->exif_resolution)
            return {};
        return displayDimensionsFromExifResolution(*row->exif_resolution);
    }

    // -------------------------------------------------------- locations

    std::string AdjustmentStore::editsRootSettingKey(const std::string &location_key)
    {
        return "editsRoot:" + location_key;
    }

    std::vector<AdjustmentStore::Location> AdjustmentStore::locations() const
    {
        std::vector<Location> out;
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (!db)
            return out;
        for (const auto &kv : UserSettingsOps::list(*db))
            if (kv.first.rfind("mediaLocation:", 0) == 0)
                out.push_back({kv.first, SidecarLayout::normalize(kv.second)});
        return out;
    }

    std::optional<AdjustmentStore::Location> AdjustmentStore::locationByPath(const std::string &location_path) const
    {
        std::string key = FilesService::makeMediaLocationKey(location_path);
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        std::string root;
        if (!db || !UserSettingsOps::get(*db, key, root))
            return std::nullopt;
        return Location{key, SidecarLayout::normalize(root)};
    }

    std::optional<AdjustmentStore::Location> AdjustmentStore::locationOf(const std::string &file_path) const
    {
        std::string p = SidecarLayout::normalize(file_path);
        std::optional<Location> best;
        for (const Location &loc : locations())
            if (SidecarLayout::isUnder(p, loc.root) && (!best || loc.root.size() > best->root.size()))
                best = loc;
        return best;
    }

    std::string AdjustmentStore::explicitEditsRoot(const std::string &location_path) const
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (!db)
            return "";
        std::string key = FilesService::makeMediaLocationKey(location_path);
        std::string root;
        if (UserSettingsOps::get(*db, editsRootSettingKey(key), root))
            return SidecarLayout::normalize(root);
        return "";
    }

    SidecarLayout AdjustmentStore::layoutFor(const std::string &location_path) const
    {
        return SidecarLayout(location_path, explicitEditsRoot(location_path), suffix());
    }

    std::vector<std::string> AdjustmentStore::ignoredPathsFor(const std::string &root) const
    {
        SidecarLayout layout = layoutFor(root);
        if (!layout.isDefaultRoot() && SidecarLayout::isUnder(layout.editsRoot(), layout.locationRoot()))
            return {layout.editsRoot()};
        return {};
    }

    std::string AdjustmentStore::editsRootConflict(const std::string &location_path, const std::string &new_root) const
    {
        std::string candidate = SidecarLayout::normalize(new_root);
        if (candidate.empty())
            return "";
        std::string self = SidecarLayout::normalize(location_path);
        auto nested = [](const std::string &a, const std::string &b)
        { return a == b || SidecarLayout::isUnder(a, b) || SidecarLayout::isUnder(b, a); };
        for (const Location &loc : locations())
        {
            if (loc.root == self)
                continue;
            if (nested(candidate, loc.root))
                return loc.root;
            std::string other_edits = layoutFor(loc.root).editsRoot();
            if (nested(candidate, other_edits))
                return loc.root;
        }
        return "";
    }

    // ------------------------------------------------------- invalidation

    void AdjustmentStore::invalidateRenders(const std::string &file_path)
    {
        std::shared_ptr<DatabaseManager> db;
        std::shared_ptr<DiskCache> cache;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
            cache = thumbnail_cache_;
        }
        if (db)
        {
            for (const ThumbnailCacheRecord &rec : ThumbnailCacheOps::listForPath(*db, file_path))
            {
                bool removed = cache && cache->deleteFromCache(rec.cached_path);
                if (!removed)
                {
                    std::error_code ec;
                    fs::remove(rec.cached_path, ec);
                }
            }
            ThumbnailCacheOps::deleteThumbnail(*db, file_path, 0);
        }
        Core::fullResCache().erase(file_path);
        for (const char *type : {"thumbnail", "preview", "preview_full"})
            MediaGenerationFailures::instance().recordSuccess(file_path, type);
    }

    void AdjustmentStore::publish(const std::string &file_path, int revision, const DisplayDims &dims)
    {
        MediaReadyBus::Event ev;
        ev.file_path = file_path;
        ev.media_type = "adjustments";
        ev.success = true;
        ev.render_revision = revision;
        ev.display_width = dims.width;
        ev.display_height = dims.height;
        MediaReadyBus::instance().publish(ev);
    }

    void AdjustmentStore::storeEntry(const ImageAdjustmentRow &row, const RenderChain &chain, const DisplayDims &dims)
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (db)
            AdjustmentsOps::upsert(*db, row);
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            Entry &e = entries_[row.file_path];
            e.chain = chain;
            e.row = row;
        }
        invalidateRenders(row.file_path);
        publish(row.file_path, chain.revision(), dims);
    }

    void AdjustmentStore::dropEntry(const std::string &file_path, const DisplayDims &base_dims)
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (db)
        {
            if (auto row = AdjustmentsOps::get(*db, file_path))
            {
                // The delivered sidecar, or the spooled copy — whichever the
                // row points at — plus any spooled copy left behind.
                std::string stop_root;
                if (auto loc = locationOf(file_path))
                {
                    SidecarLayout layout = layoutFor(loc->root);
                    stop_root = layout.editsRoot();
                    if (auto rel = layout.relativePath(file_path))
                    {
                        std::string spool = SidecarStore::spoolPath(spool_root_, loc->key, *rel, suffix());
                        SidecarStore::removeFile(spool, spool_root_);
                    }
                }
                if (!row->sidecar_path.empty() && SidecarLayout::isSidecarName(row->sidecar_path, suffix()))
                    SidecarStore::removeFile(row->sidecar_path,
                                             SidecarLayout::isUnder(row->sidecar_path, spool_root_) ? spool_root_ : stop_root);
                AdjustmentsOps::remove(*db, file_path);
            }
        }
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            entries_.erase(file_path);
        }
        invalidateRenders(file_path);
        publish(file_path, 0, base_dims);
    }

    // ------------------------------------------------------------ writes

    void AdjustmentStore::writeSidecar(ImageAdjustmentRow &row, const std::string &text, const Location &loc,
                                       const std::string &relative_path)
    {
        SidecarLayout layout = layoutFor(loc.root);
        std::string dest = layout.sidecarPath(row.file_path).value_or(layout.editsRoot() + "/" + relative_path + suffix());
        row.sidecar_path = dest;
        row.sidecar_mtime = 0;

        if (!sidecarWritesEnabled())
        {
            row.sidecar_mtime = -1; // by policy: DB only, nothing to deliver
            return;
        }

        std::string error;
        if (directoryReachable(layout.editsRoot()) && SidecarStore::writeAtomic(dest, text, error))
        {
            row.sidecar_mtime = SidecarStore::mtimeOf(dest);
            return;
        }
        if (error.empty())
            error = "edits root is not reachable: " + layout.editsRoot();
        std::string spool = SidecarStore::spoolPath(spool_root_, loc.key, relative_path, suffix());
        std::string spool_error;
        if (SidecarStore::writeAtomic(spool, text, spool_error))
        {
            log().warning("Sidecar for %s spooled (%s); it will be delivered to %s when reachable",
                          row.file_path, error, dest);
            row.sidecar_path = spool;
        }
        else
        {
            log().error("Sidecar for %s could not be written (%s) nor spooled (%s); the database holds the edit",
                        row.file_path, error, spool_error);
        }
    }

    AdjustmentStore::CommitResult AdjustmentStore::setDocument(const std::string &file_path,
                                                               const std::string &document_json)
    {
        CommitResult result;
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!initialized_)
            {
                result.error = "adjustment store is not initialized";
                return result;
            }
            db = db_;
        }
        if (!enabled())
        {
            result.outcome = Outcome::Disabled;
            result.error = "adjustments are disabled (media.adjustments.enabled)";
            return result;
        }

        std::lock_guard<std::mutex> write_lock(writeMutex());

        auto scanned = ScannedFilesOps::getByPath(*db, file_path);
        if (!scanned)
        {
            result.outcome = Outcome::NotFound;
            result.error = "file is not registered: " + file_path;
            return result;
        }
        std::string root;
        if (!UserSettingsOps::get(*db, scanned->location_key, root))
        {
            result.outcome = Outcome::NotFound;
            result.error = "the file's location is not registered";
            return result;
        }
        Location loc{scanned->location_key, SidecarLayout::normalize(root)};

        AdjustmentDocument doc;
        RenderChain chain;
        std::string error;
        if (!AdjustmentDocument::parse(document_json, doc, error) ||
            !doc.compile(ToolRegistry::instance(), chain, error))
        {
            result.outcome = Outcome::Invalid;
            result.error = error;
            return result;
        }
        DisplayDims base = scanned->exif_resolution ? displayDimensionsFromExifResolution(*scanned->exif_resolution)
                                                    : DisplayDims{};
        if (!chain.validate(toSize(base), error))
        {
            result.outcome = Outcome::Invalid;
            result.error = error;
            return result;
        }

        SidecarLayout layout = layoutFor(loc.root);
        std::string relative = layout.relativePath(file_path).value_or(scanned->relative_path);

        if (doc.isIdentity(ToolRegistry::instance()))
        {
            dropEntry(file_path, base);
            log().information("Adjustments removed for %s", file_path);
            result.outcome = Outcome::Ok;
            result.render_revision = 0;
            result.display_dims = base;
            return result;
        }

        doc.asset_name = fs::path(file_path).filename().string();
        doc.asset_relative_path = relative;
        doc.asset_size_bytes = sizeBytesOf(*scanned, file_path);
        doc.updated_at = AdjustmentDocument::nowIso8601();

        ImageAdjustmentRow row;
        row.file_path = file_path;
        row.location_key = loc.key;
        row.render_revision = chain.revision();
        row.document = doc.serialize();
        row.updated_at = nowSeconds();
        writeSidecar(row, row.document, loc, relative);

        DisplayDims dims = chain.displayDims(base.width, base.height);
        storeEntry(row, chain, dims);
        log().information("Adjustments committed for %s: revision %d, %dx%d, sidecar %s%s", file_path,
                          row.render_revision, dims.width, dims.height, row.sidecar_path,
                          std::string(row.sidecar_mtime == 0 ? " (spooled)" : ""));

        result.outcome = Outcome::Ok;
        result.render_revision = row.render_revision;
        result.display_dims = dims;
        result.sidecar_spooled = row.sidecar_mtime == 0;
        result.sidecar_path = row.sidecar_path;
        return result;
    }

    // ------------------------------------------------------------ import

    bool AdjustmentStore::importSidecarFile(const std::string &sidecar_path, const std::string &asset_path,
                                            const Location &loc, bool at_layout_path)
    {
        int64_t mtime = SidecarStore::mtimeOf(sidecar_path);
        if (mtime == 0)
            return false;

        std::lock_guard<std::mutex> write_lock(writeMutex());

        std::optional<ImageAdjustmentRow> existing = rowFor(asset_path);
        if (existing)
        {
            // A copy beside the asset while the location uses an external
            // root is stale: the external copy is authoritative (§5.6).
            if (!at_layout_path)
                return false;
            if (existing->sidecar_path == sidecar_path && existing->sidecar_mtime == mtime)
                return false; // unchanged since we last read or wrote it
        }

        std::string text, error;
        if (!SidecarStore::readText(sidecar_path, text, error))
        {
            log().warning("Cannot read sidecar %s: %s", sidecar_path, error);
            return false;
        }
        AdjustmentDocument doc;
        RenderChain chain;
        if (!AdjustmentDocument::parse(text, doc, error) || !doc.compile(ToolRegistry::instance(), chain, error))
        {
            log().warning("Ignoring sidecar %s: %s", sidecar_path, error);
            return false;
        }
        DisplayDims base = baseDimsFor(asset_path);
        if (!chain.validate(toSize(base), error))
        {
            log().warning("Ignoring sidecar %s: %s", sidecar_path, error);
            return false;
        }

        if (doc.isIdentity(ToolRegistry::instance()))
        {
            if (existing)
                dropEntry(asset_path, base);
            return existing.has_value();
        }

        int revision = chain.revision();
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (existing && existing->render_revision == revision)
        {
            // Same content, different file identity (touched, moved, or
            // re-saved elsewhere): just remember where it is now.
            if (db)
                AdjustmentsOps::updateSidecar(*db, asset_path, sidecar_path, mtime);
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = entries_.find(asset_path);
            if (it != entries_.end())
            {
                it->second.row.sidecar_path = sidecar_path;
                it->second.row.sidecar_mtime = mtime;
            }
            return false;
        }

        ImageAdjustmentRow row;
        row.file_path = asset_path;
        row.location_key = loc.key;
        row.render_revision = revision;
        row.document = doc.serialize();
        row.sidecar_path = sidecar_path;
        row.sidecar_mtime = mtime;
        row.updated_at = nowSeconds();
        storeEntry(row, chain, chain.displayDims(base.width, base.height));
        log().information("Imported adjustments for %s from %s (revision %d)", asset_path, sidecar_path, revision);
        return true;
    }

    void AdjustmentStore::onSidecarSeen(const std::string &root, const std::string &sidecar_path)
    {
        if (!enabled())
            return;
        auto loc = locationByPath(root);
        if (!loc)
            return;
        SidecarLayout layout = layoutFor(loc->root);
        if (auto asset = layout.assetPath(sidecar_path))
        {
            importSidecarFile(sidecar_path, *asset, *loc, /*at_layout_path=*/true);
            return;
        }
        if (!layout.isDefaultRoot())
        {
            SidecarLayout beside(loc->root, "", suffix());
            if (auto asset = beside.assetPath(sidecar_path))
                importSidecarFile(sidecar_path, *asset, *loc, /*at_layout_path=*/false);
        }
    }

    int AdjustmentStore::finishLocationScan(const std::string &root)
    {
        if (!enabled())
            return 0;
        auto loc = locationByPath(root);
        if (!loc)
            return 0;
        SidecarLayout layout = layoutFor(loc->root);
        if (!layout.isDefaultRoot() && directoryReachable(layout.editsRoot()))
        {
            Files::FileScannerOptions opt;
            opt.recursive = true;
            opt.includeHidden = true;
            std::string sfx = suffix();
            Files::scan(layout.editsRoot(), opt, [this, &root, &sfx](const Files::FileRecord &rec)
                        {
                            if (!rec.hasError() && SidecarLayout::isSidecarName(rec.fileName, sfx))
                                onSidecarSeen(root, rec.fullPath);
                        });
        }
        return deliverSpooled(root);
    }

    int AdjustmentStore::deliverSpooled(const std::string &location_path)
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (!db || !enabled() || !sidecarWritesEnabled())
            return 0;
        auto loc = locationByPath(location_path);
        if (!loc)
            return 0;
        SidecarLayout layout = layoutFor(loc->root);
        if (!directoryReachable(layout.editsRoot()))
            return 0;

        int delivered = 0;
        for (const ImageAdjustmentRow &row : AdjustmentsOps::listByLocation(*db, loc->key))
        {
            if (row.sidecar_mtime != 0)
                continue;
            auto dest = layout.sidecarPath(row.file_path);
            if (!dest)
                continue;
            auto rel = layout.relativePath(row.file_path);
            std::string spool = rel ? SidecarStore::spoolPath(spool_root_, loc->key, *rel, suffix()) : "";

            // A newer sidecar already at the destination (edited elsewhere
            // while this machine was disconnected) wins.
            int64_t dest_mtime = SidecarStore::mtimeOf(*dest);
            if (dest_mtime > 0 && dest_mtime > row.updated_at)
            {
                log().information("Spooled edit for %s superseded by a newer sidecar at %s", row.file_path, *dest);
                if (!spool.empty())
                    SidecarStore::removeFile(spool, spool_root_);
                importSidecarFile(*dest, row.file_path, *loc, /*at_layout_path=*/true);
                continue;
            }

            std::string text, error;
            if (spool.empty() || !SidecarStore::readText(spool, text, error))
                text = row.document; // re-spool from the database document
            if (!SidecarStore::writeAtomic(*dest, text, error))
            {
                log().warning("Could not deliver sidecar for %s to %s: %s", row.file_path, *dest, error);
                continue;
            }
            int64_t mtime = SidecarStore::mtimeOf(*dest);
            AdjustmentsOps::updateSidecar(*db, row.file_path, *dest, mtime);
            {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                auto it = entries_.find(row.file_path);
                if (it != entries_.end())
                {
                    it->second.row.sidecar_path = *dest;
                    it->second.row.sidecar_mtime = mtime;
                }
            }
            if (!spool.empty())
                SidecarStore::removeFile(spool, spool_root_);
            ++delivered;
        }
        if (delivered > 0)
            log().information("Delivered %d spooled sidecar(s) for %s", delivered, loc->root);
        return delivered;
    }

    // -------------------------------------------------------- edits root

    AdjustmentStore::SetRootResult AdjustmentStore::setEditsRoot(const std::string &location_path,
                                                                 const std::string &new_root)
    {
        SetRootResult result;
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        auto loc = locationByPath(location_path);
        if (!db || !loc)
        {
            result.error = "location is not registered: " + location_path;
            return result;
        }
        std::string candidate = SidecarLayout::normalize(new_root);
        std::string conflict = editsRootConflict(loc->root, candidate);
        if (!conflict.empty())
        {
            result.conflicting_location = conflict;
            result.error = "that folder is already used by the location " + conflict;
            return result;
        }

        SidecarLayout old_layout = layoutFor(loc->root);
        SidecarLayout new_layout(loc->root, candidate, suffix());
        {
        std::lock_guard<std::mutex> write_lock(writeMutex());

        if (!new_layout.isDefaultRoot())
        {
            std::error_code ec;
            if (!fs::exists(new_layout.editsRoot(), ec))
            {
                auto mounts = MediaAvailability::mountedFilesystems();
                if (!MediaAvailability::isPathAvailable(new_layout.editsRoot(), mounts))
                {
                    result.error = "the folder's volume is not mounted: " + new_layout.editsRoot();
                    return result;
                }
                fs::create_directories(new_layout.editsRoot(), ec);
            }
            if (!SidecarStore::isWritableDirectory(new_layout.editsRoot()))
            {
                result.error = "the folder is not writable: " + new_layout.editsRoot();
                return result;
            }
        }

        if (old_layout.editsRoot() != new_layout.editsRoot())
        {
            for (const ImageAdjustmentRow &row : AdjustmentsOps::listByLocation(*db, loc->key))
            {
                if (row.sidecar_mtime == 0)
                    continue; // spooled: delivery below targets the new root
                auto dest = new_layout.sidecarPath(row.file_path);
                if (!dest || *dest == row.sidecar_path)
                    continue;
                std::string error;
                bool ok = false;
                std::error_code ec;
                if (row.sidecar_mtime > 0 && fs::exists(row.sidecar_path, ec))
                {
                    std::string text;
                    ok = SidecarStore::readText(row.sidecar_path, text, error) &&
                         SidecarStore::writeAtomic(*dest, text, error);
                    if (ok)
                        SidecarStore::removeFile(row.sidecar_path, old_layout.editsRoot());
                }
                else if (row.sidecar_mtime > 0)
                {
                    ok = SidecarStore::writeAtomic(*dest, row.document, error);
                }
                else
                {
                    continue; // -1: writes disabled by policy
                }
                if (ok)
                {
                    int64_t mtime = SidecarStore::mtimeOf(*dest);
                    AdjustmentsOps::updateSidecar(*db, row.file_path, *dest, mtime);
                    std::unique_lock<std::shared_mutex> lock(mutex_);
                    auto it = entries_.find(row.file_path);
                    if (it != entries_.end())
                    {
                        it->second.row.sidecar_path = *dest;
                        it->second.row.sidecar_mtime = mtime;
                    }
                    ++result.moved;
                }
                else
                {
                    // Cannot reach the old copy: mark pending; the spool
                    // delivery re-creates it from the database document.
                    log().warning("Could not move sidecar for %s: %s", row.file_path, error);
                    AdjustmentsOps::updateSidecar(*db, row.file_path, row.sidecar_path, 0);
                    std::unique_lock<std::shared_mutex> lock(mutex_);
                    auto it = entries_.find(row.file_path);
                    if (it != entries_.end())
                        it->second.row.sidecar_mtime = 0;
                    ++result.failed;
                }
            }
        }

        std::string setting = editsRootSettingKey(loc->key);
        if (new_layout.isDefaultRoot())
            UserSettingsOps::remove(*db, setting);
        else
            UserSettingsOps::upsert(*db, setting, new_layout.editsRoot());
        } // write mutex released: delivery takes it per file

        deliverSpooled(location_path);
        result.ok = true;
        return result;
    }

    // --------------------------------------------------------- lifecycle

    AdjustmentStore::LocationSummary AdjustmentStore::summarize(const std::string &location_path) const
    {
        LocationSummary s;
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        auto loc = locationByPath(location_path);
        if (!db || !loc)
            return s;
        int rows = 0, spooled = 0;
        AdjustmentsOps::counts(*db, loc->key, rows, spooled);
        s.spooled = spooled;
        s.sidecars = rows - spooled;
        return s;
    }

    int AdjustmentStore::forgetLocation(const std::string &location_path, bool delete_sidecar_files)
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        auto loc = locationByPath(location_path);
        if (!db || !loc)
            return 0;

        if (!delete_sidecar_files)
            deliverSpooled(location_path); // a kept edit should reach its folder if it can

        std::lock_guard<std::mutex> write_lock(writeMutex());
        SidecarLayout layout = layoutFor(loc->root);
        int deleted = 0;
        if (delete_sidecar_files)
        {
            for (const ImageAdjustmentRow &row : AdjustmentsOps::listByLocation(*db, loc->key))
            {
                std::error_code ec;
                if (row.sidecar_mtime > 0 && SidecarLayout::isSidecarName(row.sidecar_path, suffix()) &&
                    fs::exists(row.sidecar_path, ec))
                {
                    if (SidecarStore::removeFile(row.sidecar_path, layout.editsRoot()))
                        ++deleted;
                }
            }
        }
        std::error_code ec;
        fs::remove_all(SidecarStore::spoolDirForLocation(spool_root_, loc->key), ec);
        UserSettingsOps::remove(*db, editsRootSettingKey(loc->key));
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            for (auto it = entries_.begin(); it != entries_.end();)
            {
                if (it->second.row.location_key == loc->key)
                    it = entries_.erase(it);
                else
                    ++it;
            }
        }
        return deleted;
    }

    void AdjustmentStore::onFilePathChanged(const std::string &old_path, const std::string &new_path,
                                            const std::string &new_location_key)
    {
        std::shared_ptr<DatabaseManager> db;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            db = db_;
        }
        if (!db)
            return;
        std::lock_guard<std::mutex> write_lock(writeMutex());
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = entries_.find(old_path);
        if (it == entries_.end())
            return;
        Entry e = std::move(it->second);
        entries_.erase(it);
        e.row.file_path = new_path;
        e.row.location_key = new_location_key;
        lock.unlock();

        AdjustmentsOps::updateFilePath(*db, old_path, new_path, new_location_key);
        // A default-root sidecar travelled with its asset; point the row at
        // its new home (missing there → pending, re-created from the DB on
        // the next spool delivery).
        std::string root;
        if (UserSettingsOps::get(*db, new_location_key, root))
        {
            SidecarLayout layout = layoutFor(root);
            if (layout.isDefaultRoot())
            {
                if (auto dest = layout.sidecarPath(new_path))
                {
                    int64_t mtime = SidecarStore::mtimeOf(*dest);
                    e.row.sidecar_path = *dest;
                    e.row.sidecar_mtime = mtime;
                    AdjustmentsOps::updateSidecar(*db, new_path, *dest, mtime);
                }
            }
        }
        lock.lock();
        entries_[new_path] = std::move(e);
    }
}
