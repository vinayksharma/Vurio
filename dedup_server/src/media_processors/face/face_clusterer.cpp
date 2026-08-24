#include "media_processors/face/face_clusterer.hpp"

#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"

#include <Poco/Logger.h>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace MediaDedup
{
    namespace
    {
        double cosine(const std::vector<float> &a, const std::vector<float> &b)
        {
            if (a.size() != b.size() || a.empty())
                return -1.0;
            double dot = 0;
            for (std::size_t i = 0; i < a.size(); ++i)
                dot += static_cast<double>(a[i]) * b[i];
            return dot; // both unit-normalized
        }

        void renormalize(std::vector<float> &v)
        {
            double n = 0;
            for (float x : v)
                n += static_cast<double>(x) * x;
            if (n <= 0)
                return;
            const float inv = static_cast<float>(1.0 / std::sqrt(n));
            for (float &x : v)
                x *= inv;
        }
    }

    FaceClusterer::FaceClusterer(DatabaseManager *db, Options options)
        : db_(db), options_(options)
    {
    }

    void FaceClusterer::load()
    {
        clusters_.clear();
        if (!db_)
            return;
        for (auto &row : FacesOps::listClusters(*db_))
        {
            Cluster c;
            c.centroid = std::move(row.centroid);
            c.size = row.size;
            c.cover_face_id = row.cover_face_id;
            c.cover_quality = row.cover_quality;
            c.cover_pinned = row.cover_pinned;
            c.person_id = row.person_id;
            clusters_[row.id] = std::move(c);
        }
        // FACE-7b: exemplar embeddings ride the same mirror; persisted rows
        // are current by construction (consolidate refreshes dirty clusters
        // before it returns), so the dirty set resets with the mirror.
        for (auto &row : FacesOps::listClusterExemplars(*db_))
        {
            auto it = clusters_.find(row.cluster_id);
            if (it != clusters_.end())
                it->second.exemplars.push_back(std::move(row.embedding));
        }
        exemplars_dirty_.clear();
        cannot_links_.clear();
        cluster_forbidden_.clear();
        for (const auto &l : FacesOps::listCannotLinks(*db_))
        {
            cannot_links_[l.face_id].insert(l.forbidden_cluster_id);
            if (l.current_cluster_id > 0)
                cluster_forbidden_[l.current_cluster_id].insert(l.forbidden_cluster_id);
        }
        Poco::Logger::get("FaceClusterer")
            .information("Loaded %z face cluster(s), %z cannot-link constraint(s)",
                         clusters_.size(), cannot_links_.size());
    }

    void FaceClusterer::persist(int id, const Cluster &c)
    {
        FacesOps::ClusterRow row;
        row.id = id;
        row.centroid = c.centroid;
        row.size = c.size;
        row.cover_face_id = c.cover_face_id;
        row.cover_quality = c.cover_quality;
        row.cover_pinned = c.cover_pinned;
        row.person_id = c.person_id;
        FacesOps::updateCluster(*db_, row);
    }

    double FaceClusterer::score(const Cluster &c, const std::vector<float> &embedding) const
    {
        double best = cosine(embedding, c.centroid);
        for (const auto &e : c.exemplars)
            best = std::max(best, cosine(embedding, e));
        return best;
    }

    int FaceClusterer::assign(int face_id, const std::vector<float> &embedding, double quality)
    {
        if (!db_ || embedding.empty() || quality < options_.min_quality)
            return 0;

        const auto forbidden_it = cannot_links_.find(face_id);
        int best_id = 0;
        double best_cos = -1.0, second_cos = -1.0;
        for (const auto &[id, c] : clusters_)
        {
            // FACE-3b: "not this person" is absolute — the best-matching
            // cluster is skipped entirely when the user has excluded it.
            if (forbidden_it != cannot_links_.end() && forbidden_it->second.count(id))
                continue;
            // FACE-7b: best exemplar, not the blended centroid alone.
            const double cos = score(c, embedding);
            if (cos > best_cos)
            {
                second_cos = best_cos;
                best_cos = cos;
                best_id = id;
            }
            else if (cos > second_cos)
                second_cos = cos;
        }

        // FACE-7c: a near-tie between two people is a guess, not a match —
        // leave the face unclustered (NOT a new cluster: it clearly is
        // someone known) and let the catch-up retry as clusters sharpen.
        if (best_id != 0 && best_cos >= options_.join_threshold &&
            second_cos >= 0 && best_cos - second_cos < options_.join_margin)
            return 0;

        if (best_id != 0 && best_cos >= options_.join_threshold)
        {
            Cluster &c = clusters_[best_id];
            // Incremental mean, re-normalized: new = norm(centroid*size + e).
            for (std::size_t i = 0; i < c.centroid.size(); ++i)
                c.centroid[i] = c.centroid[i] * c.size + embedding[i];
            renormalize(c.centroid);
            c.size++;
            // FACE-6: a user-pinned cover is never displaced by quality.
            if (!c.cover_pinned && quality > c.cover_quality)
            {
                c.cover_quality = quality;
                c.cover_face_id = face_id;
            }
            persist(best_id, c);
            exemplars_dirty_.insert(best_id);
            FacesOps::setFaceCluster(*db_, face_id, best_id);
            if (forbidden_it != cannot_links_.end())
                cluster_forbidden_[best_id].insert(forbidden_it->second.begin(),
                                                   forbidden_it->second.end());
            return best_id;
        }

        Cluster c;
        c.centroid = embedding;
        c.size = 1;
        c.cover_face_id = face_id;
        c.cover_quality = quality;
        c.exemplars.push_back(embedding);
        FacesOps::ClusterRow row;
        row.centroid = c.centroid;
        row.size = 1;
        row.cover_face_id = face_id;
        row.cover_quality = quality;
        const int id = FacesOps::insertCluster(*db_, row);
        if (id <= 0)
            return 0;
        clusters_[id] = std::move(c);
        exemplars_dirty_.insert(id);
        FacesOps::setFaceCluster(*db_, face_id, id);
        if (forbidden_it != cannot_links_.end())
            cluster_forbidden_[id].insert(forbidden_it->second.begin(),
                                          forbidden_it->second.end());
        return id;
    }

    bool FaceClusterer::mergeViolatesCannotLink(int lose, int win) const
    {
        // "f is not cluster C's person" means f's current cluster and C
        // must never UNIFY — in either direction. A one-way check would
        // let C dissolve into f's cluster and reunite them anyway.
        const auto a = cluster_forbidden_.find(lose);
        if (a != cluster_forbidden_.end() && a->second.count(win) > 0)
            return true;
        const auto b = cluster_forbidden_.find(win);
        return b != cluster_forbidden_.end() && b->second.count(lose) > 0;
    }

    void FaceClusterer::refreshDirtyExemplars()
    {
        // FACE-7b: selection reads the members' embeddings, so it runs here
        // (per consolidate) rather than per join. Persist first, then pull
        // the refreshed sets back into the mirror in one query.
        if (exemplars_dirty_.empty())
            return;
        for (int id : exemplars_dirty_)
            if (clusters_.count(id))
                FacesOps::refreshClusterExemplars(*db_, id, options_.exemplar_k,
                                                  options_.exemplar_spread,
                                                  options_.exemplar_min_quality);
        for (int id : exemplars_dirty_)
        {
            auto it = clusters_.find(id);
            if (it != clusters_.end())
                it->second.exemplars.clear();
        }
        for (auto &row : FacesOps::listClusterExemplars(*db_))
        {
            if (!exemplars_dirty_.count(row.cluster_id))
                continue;
            auto it = clusters_.find(row.cluster_id);
            if (it != clusters_.end())
                it->second.exemplars.push_back(std::move(row.embedding));
        }
        exemplars_dirty_.clear();
    }

    int FaceClusterer::consolidate()
    {
        if (!db_)
            return 0;
        refreshDirtyExemplars();
        int merges = 0;
        bool merged_any = true;
        while (merged_any)
        {
            merged_any = false;
            for (auto it_a = clusters_.begin(); it_a != clusters_.end() && !merged_any; ++it_a)
            {
                for (auto it_b = std::next(it_a); it_b != clusters_.end(); ++it_b)
                {
                    // FACE-7b two-stage test: the centroid bar merges as
                    // before; below it, one strong cross-exemplar pair is
                    // decisive — two looks of one person can have distant
                    // blended centroids while their real photos agree far
                    // above any different-person cosine (live calibration:
                    // strangers <= 0.51, same person 0.67+).
                    const double cc =
                        cosine(it_a->second.centroid, it_b->second.centroid);
                    if (cc < options_.merge_prefilter)
                        continue;
                    bool should_merge = cc >= options_.merge_threshold;
                    if (!should_merge)
                        for (const auto &ea : it_a->second.exemplars)
                        {
                            for (const auto &eb : it_b->second.exemplars)
                                if (cosine(ea, eb) >= options_.exemplar_merge_threshold)
                                {
                                    should_merge = true;
                                    break;
                                }
                            if (should_merge)
                                break;
                        }
                    if (!should_merge)
                        continue;

                    // Winner: the NAMED cluster if exactly one is named,
                    // else the larger. The loser's faces move over.
                    auto *win = &*it_a;
                    auto *lose = &*it_b;
                    const bool a_named = it_a->second.person_id != 0;
                    const bool b_named = it_b->second.person_id != 0;
                    if ((b_named && !a_named) ||
                        (a_named == b_named && it_b->second.size > it_a->second.size))
                        std::swap(win, lose);
                    // Two differently-named clusters never auto-merge — that
                    // is a user decision (FACE-3).
                    if (a_named && b_named &&
                        it_a->second.person_id != it_b->second.person_id)
                        continue;
                    // FACE-3b: a merge moves the loser's faces into the
                    // winner — blocked when any of those faces is forbidden
                    // from the winner ("not this person" must survive
                    // consolidation).
                    if (mergeViolatesCannotLink(lose->first, win->first))
                        continue;

                    Cluster &w = win->second;
                    Cluster &l = lose->second;
                    for (std::size_t i = 0; i < w.centroid.size(); ++i)
                        w.centroid[i] = w.centroid[i] * w.size + l.centroid[i] * l.size;
                    renormalize(w.centroid);
                    w.size += l.size;
                    // FACE-6: pinned beats unpinned; both pinned keeps the
                    // winner's; both unpinned falls back to quality.
                    if (!w.cover_pinned &&
                        (l.cover_pinned || l.cover_quality > w.cover_quality))
                    {
                        w.cover_quality = l.cover_quality;
                        w.cover_face_id = l.cover_face_id;
                        w.cover_pinned = l.cover_pinned;
                    }
                    FacesOps::moveClusterMembers(*db_, lose->first, win->first);
                    // Constraints against the dissolved cluster follow the
                    // identity into the winner — in the DB and in both
                    // in-memory mirrors.
                    FacesOps::repointCannotLinks(*db_, lose->first, win->first);
                    FacesOps::deleteCluster(*db_, lose->first);
                    {
                        const int lose_id = lose->first, win_id = win->first;
                        auto moved = cluster_forbidden_.find(lose_id);
                        if (moved != cluster_forbidden_.end())
                        {
                            cluster_forbidden_[win_id].insert(moved->second.begin(),
                                                              moved->second.end());
                            cluster_forbidden_.erase(moved);
                        }
                        for (auto &[fid, set] : cannot_links_)
                            if (set.erase(lose_id))
                                set.insert(win_id);
                        for (auto &[cid, set] : cluster_forbidden_)
                            if (set.erase(lose_id))
                                set.insert(win_id);
                        // A cluster forbidden from itself is meaningless.
                        cluster_forbidden_[win_id].erase(win_id);
                        if (cluster_forbidden_[win_id].empty())
                            cluster_forbidden_.erase(win_id);
                    }
                    // FACE-7b: keep both identities' looks matchable until
                    // the re-selection below persists the canonical set.
                    w.exemplars.insert(w.exemplars.end(),
                                       std::make_move_iterator(l.exemplars.begin()),
                                       std::make_move_iterator(l.exemplars.end()));
                    if (static_cast<int>(w.exemplars.size()) > 2 * options_.exemplar_k)
                        w.exemplars.resize(2 * options_.exemplar_k);
                    exemplars_dirty_.insert(win->first);
                    exemplars_dirty_.erase(lose->first);
                    persist(win->first, w);
                    clusters_.erase(lose->first);
                    merges++;
                    merged_any = true; // iterators invalidated — restart scan
                    break;
                }
            }
        }
        // Winners re-select from their full membership; the DB rows are
        // current when we return (load() relies on this).
        refreshDirtyExemplars();
        if (merges > 0)
            Poco::Logger::get("FaceClusterer")
                .information("Consolidation merged %d cluster pair(s); %z remain",
                             merges, clusters_.size());
        return merges;
    }
}
