#include "media_processors/face/face_clusterer.hpp"

#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"

#include <Poco/Logger.h>
#include <cmath>

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
            c.person_id = row.person_id;
            clusters_[row.id] = std::move(c);
        }
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
        row.person_id = c.person_id;
        FacesOps::updateCluster(*db_, row);
    }

    int FaceClusterer::assign(int face_id, const std::vector<float> &embedding, double quality)
    {
        if (!db_ || embedding.empty() || quality < options_.min_quality)
            return 0;

        const auto forbidden_it = cannot_links_.find(face_id);
        int best_id = 0;
        double best_cos = -1.0;
        for (const auto &[id, c] : clusters_)
        {
            // FACE-3b: "not this person" is absolute — the best-matching
            // cluster is skipped entirely when the user has excluded it.
            if (forbidden_it != cannot_links_.end() && forbidden_it->second.count(id))
                continue;
            const double cos = cosine(embedding, c.centroid);
            if (cos > best_cos)
            {
                best_cos = cos;
                best_id = id;
            }
        }

        if (best_id != 0 && best_cos >= options_.join_threshold)
        {
            Cluster &c = clusters_[best_id];
            // Incremental mean, re-normalized: new = norm(centroid*size + e).
            for (std::size_t i = 0; i < c.centroid.size(); ++i)
                c.centroid[i] = c.centroid[i] * c.size + embedding[i];
            renormalize(c.centroid);
            c.size++;
            if (quality > c.cover_quality)
            {
                c.cover_quality = quality;
                c.cover_face_id = face_id;
            }
            persist(best_id, c);
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
        FacesOps::ClusterRow row;
        row.centroid = c.centroid;
        row.size = 1;
        row.cover_face_id = face_id;
        row.cover_quality = quality;
        const int id = FacesOps::insertCluster(*db_, row);
        if (id <= 0)
            return 0;
        clusters_[id] = std::move(c);
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

    int FaceClusterer::consolidate()
    {
        if (!db_)
            return 0;
        int merges = 0;
        bool merged_any = true;
        while (merged_any)
        {
            merged_any = false;
            for (auto it_a = clusters_.begin(); it_a != clusters_.end() && !merged_any; ++it_a)
            {
                for (auto it_b = std::next(it_a); it_b != clusters_.end(); ++it_b)
                {
                    if (cosine(it_a->second.centroid, it_b->second.centroid) <
                        options_.merge_threshold)
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
                    if (l.cover_quality > w.cover_quality)
                    {
                        w.cover_quality = l.cover_quality;
                        w.cover_face_id = l.cover_face_id;
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
                    persist(win->first, w);
                    clusters_.erase(lose->first);
                    merges++;
                    merged_any = true; // iterators invalidated — restart scan
                    break;
                }
            }
        }
        if (merges > 0)
            Poco::Logger::get("FaceClusterer")
                .information("Consolidation merged %d cluster pair(s); %z remain",
                             merges, clusters_.size());
        return merges;
    }
}
