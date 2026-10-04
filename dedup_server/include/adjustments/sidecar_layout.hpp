#pragma once

// ADJ-1 (docs §5.6): THE mapping between an asset and its sidecar. Pure
// path arithmetic, no filesystem access, so it is unit-testable and the
// only place the layout is defined.
//
//   sidecarPath(asset) = editsRoot / relative(asset, locationRoot) + suffix
//   assetPath(sidecar) = locationRoot / relative(sidecar, editsRoot) - suffix
//
// With the default edits root (the location itself) this is "next to the
// asset". With an external root the folder structure is mirrored beneath
// it, so an asset and its sidecar share one relative path.

#include <optional>
#include <string>

namespace MediaDedup::Adjustments
{
    class SidecarLayout
    {
    public:
        static constexpr const char *kDefaultSuffix = ".vurio.json";

        // `edits_root` empty means "same as the location".
        SidecarLayout(std::string location_root, std::string edits_root, std::string suffix = kDefaultSuffix);

        const std::string &locationRoot() const { return location_root_; }
        // The effective root (the location when none was chosen).
        const std::string &editsRoot() const { return edits_root_; }
        const std::string &suffix() const { return suffix_; }
        bool isDefaultRoot() const { return default_root_; }

        // Sidecar path for an asset under the location; nullopt when the
        // asset is not under the location.
        std::optional<std::string> sidecarPath(const std::string &asset_path) const;

        // Asset path for a sidecar under the edits root; nullopt when the
        // path is not under the root or lacks the suffix.
        std::optional<std::string> assetPath(const std::string &sidecar_path) const;

        // Asset-relative path ("2024/DSC04624.JPG") for the document.
        std::optional<std::string> relativePath(const std::string &asset_path) const;

        // True when `name` ends with the suffix (case-sensitive).
        static bool isSidecarName(const std::string &name, const std::string &suffix = kDefaultSuffix);

        // Slash-normalized, trailing slash stripped (keeps "/").
        static std::string normalize(std::string path);

        // True when `path` is `root` or lies beneath it (both normalized).
        static bool isUnder(const std::string &path, const std::string &root);

    private:
        std::string location_root_;
        std::string edits_root_;
        std::string suffix_;
        bool default_root_ = true;
    };
}
