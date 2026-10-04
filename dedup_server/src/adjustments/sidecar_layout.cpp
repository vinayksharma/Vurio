#include "adjustments/sidecar_layout.hpp"
#include <algorithm>

namespace MediaDedup::Adjustments
{
    SidecarLayout::SidecarLayout(std::string location_root, std::string edits_root, std::string suffix)
        : location_root_(normalize(std::move(location_root))), suffix_(std::move(suffix))
    {
        std::string e = normalize(std::move(edits_root));
        default_root_ = e.empty() || e == location_root_;
        edits_root_ = default_root_ ? location_root_ : e;
    }

    std::string SidecarLayout::normalize(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();
        return path;
    }

    bool SidecarLayout::isUnder(const std::string &path, const std::string &root)
    {
        if (root.empty())
            return false;
        if (path == root)
            return true;
        if (root == "/")
            return !path.empty() && path[0] == '/';
        return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
    }

    bool SidecarLayout::isSidecarName(const std::string &name, const std::string &suffix)
    {
        return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::optional<std::string> SidecarLayout::relativePath(const std::string &asset_path) const
    {
        std::string p = normalize(asset_path);
        if (!isUnder(p, location_root_) || p == location_root_)
            return std::nullopt;
        size_t cut = location_root_ == "/" ? 1 : location_root_.size() + 1;
        return p.substr(cut);
    }

    std::optional<std::string> SidecarLayout::sidecarPath(const std::string &asset_path) const
    {
        auto rel = relativePath(asset_path);
        if (!rel)
            return std::nullopt;
        std::string root = edits_root_ == "/" ? "" : edits_root_;
        return root + "/" + *rel + suffix_;
    }

    std::optional<std::string> SidecarLayout::assetPath(const std::string &sidecar_path) const
    {
        std::string p = normalize(sidecar_path);
        if (!isUnder(p, edits_root_) || p == edits_root_)
            return std::nullopt;
        if (!isSidecarName(p, suffix_))
            return std::nullopt;
        size_t cut = edits_root_ == "/" ? 1 : edits_root_.size() + 1;
        std::string rel = p.substr(cut, p.size() - cut - suffix_.size());
        if (rel.empty())
            return std::nullopt;
        std::string root = location_root_ == "/" ? "" : location_root_;
        return root + "/" + rel;
    }
}
