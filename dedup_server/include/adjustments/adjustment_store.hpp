#pragma once

// ADJ-1 (docs/todo_image_adjustments.md §5.5-§5.7): the process-wide owner
// of per-file adjustments. One in-memory map (path -> compiled chain) that
// every render path reads, backed by image_adjustments for durability
// across restarts and by the sidecar files for durability across
// registrations and machines.
//
// Precedence: a committed edit writes the DB row first, then the sidecar.
// A sidecar seen by the scan with a different mtime than the row knows wins
// (hand edits, files synced from another machine). An unreachable edits
// root never blocks a commit: the sidecar is spooled under data/ and
// delivered later (§5.6a).

#include "adjustments/adjustment_document.hpp"
#include "adjustments/render_chain.hpp"
#include "adjustments/sidecar_layout.hpp"
#include "database/adjustments_ops.hpp"
#include "utils/display_geometry.hpp"
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;
    class DiskCache;
    class UnifiedObservableConfigManager;
}

namespace MediaDedup::Adjustments
{
    class AdjustmentStore
    {
    public:
        static AdjustmentStore &instance();

        // Loads every row into the map. `spool_root` is where sidecars wait
        // for an unreachable destination (SidecarStore::defaultSpoolRoot).
        void initialize(std::shared_ptr<DatabaseManager> db,
                        std::shared_ptr<UnifiedObservableConfigManager> config,
                        std::string spool_root,
                        std::shared_ptr<DiskCache> thumbnail_cache = nullptr);
        bool isInitialized() const;
        std::shared_ptr<DatabaseManager> database() const;
        // Tests: forget everything (the DB is not touched).
        void reset();

        // ----- hot read side (every render path, every list row) -----
        bool enabled() const;
        RenderChain chainFor(const std::string &file_path) const;
        int revisionFor(const std::string &file_path) const;
        DisplayDims displayDimsFor(const std::string &file_path, int base_width, int base_height) const;
        // The document JSON, or nullopt when the file has no edits.
        std::optional<ImageAdjustmentRow> rowFor(const std::string &file_path) const;

        // ----- commits (the RPC) -----
        enum class Outcome { Ok, Disabled, NotFound, Invalid, Internal };
        struct CommitResult
        {
            Outcome outcome = Outcome::Internal;
            std::string error;
            int render_revision = 0;
            DisplayDims display_dims;
            bool sidecar_spooled = false;
            std::string sidecar_path;
        };
        CommitResult setDocument(const std::string &file_path, const std::string &document_json);

        // ----- edits roots (§5.6) -----
        static std::string editsRootSettingKey(const std::string &location_key);
        // "" when the location uses its own folder.
        std::string explicitEditsRoot(const std::string &location_path) const;
        SidecarLayout layoutFor(const std::string &location_path) const;
        struct SetRootResult
        {
            bool ok = false;
            std::string error;
            std::string conflicting_location; // set when refused as shared/nested
            int moved = 0;
            int failed = 0;
        };
        // Validates (no sharing/nesting with another location), moves the
        // location's existing sidecars into the new layout, saves the
        // setting, then delivers anything spooled.
        SetRootResult setEditsRoot(const std::string &location_path, const std::string &new_root);
        // The conflict check alone (used by RegisterLocation).
        std::string editsRootConflict(const std::string &location_path, const std::string &new_root) const;

        // ----- scan integration (§5.6) -----
        // A sidecar the scanner enumerated. `root` is the registered location
        // whose walk (or whose external edits-root walk) found it.
        void onSidecarSeen(const std::string &root, const std::string &sidecar_path);
        // After a location's media walk: walk an external edits root (if any)
        // and deliver the spool. Returns the number of sidecars delivered.
        int finishLocationScan(const std::string &root);
        // Directories the media walk must skip for this location (an edits
        // root nested inside it).
        std::vector<std::string> ignoredPathsFor(const std::string &root) const;
        int deliverSpooled(const std::string &location_path);

        // ----- registration lifecycle (§5.8, §5.9) -----
        struct LocationSummary
        {
            int sidecars = 0; // rows with a delivered sidecar
            int spooled = 0;  // rows whose sidecar waits in the spool
        };
        LocationSummary summarize(const std::string &location_path) const;
        // Deregistration: optionally unlink the location's sidecars (and
        // prune their directories), always drop its spool and setting and
        // forget its rows in the map. The DB rows go with removeByLocationKey.
        int forgetLocation(const std::string &location_path, bool delete_sidecar_files);
        // Location remap (ChangePath / relink): rows follow their files; a
        // default-root sidecar moved with its asset.
        void onFilePathChanged(const std::string &old_path, const std::string &new_path,
                               const std::string &new_location_key);

    private:
        AdjustmentStore() = default;

        struct Entry
        {
            RenderChain chain;
            ImageAdjustmentRow row;
        };

        struct Location
        {
            std::string key;  // "mediaLocation:<sha>"
            std::string root; // registered path
        };
        std::vector<Location> locations() const;
        std::optional<Location> locationOf(const std::string &file_path) const;
        std::optional<Location> locationByPath(const std::string &location_path) const;
        std::string suffix() const;
        bool sidecarWritesEnabled() const;

        // Shared tail of a commit and an import: row + map + caches + push.
        void storeEntry(const ImageAdjustmentRow &row, const RenderChain &chain, const DisplayDims &dims);
        void dropEntry(const std::string &file_path, const DisplayDims &base_dims);
        void invalidateRenders(const std::string &file_path);
        void publish(const std::string &file_path, int revision, const DisplayDims &dims);
        DisplayDims baseDimsFor(const std::string &file_path) const;
        // Write a document for a row: destination first, spool on failure.
        void writeSidecar(ImageAdjustmentRow &row, const std::string &text, const Location &loc,
                          const std::string &relative_path);
        bool importSidecarFile(const std::string &sidecar_path, const std::string &asset_path,
                               const Location &loc, bool at_layout_path);

        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, Entry> entries_;
        std::shared_ptr<DatabaseManager> db_;
        std::shared_ptr<UnifiedObservableConfigManager> config_;
        std::shared_ptr<DiskCache> thumbnail_cache_;
        std::string spool_root_;
        bool initialized_ = false;
    };
}
