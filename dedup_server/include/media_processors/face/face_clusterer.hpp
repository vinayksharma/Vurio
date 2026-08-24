#pragma once

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;

    /**
     * @brief FACE-2 incremental identity clustering
     * (docs/todo_face_recognition.md §3).
     *
     * Centroid-based, not single-linkage: each face joins the cluster whose
     * (unit-normalized) centroid it matches at cosine >= join_threshold, or
     * starts a new one. Centroid matching resists the identity chaining
     * single-linkage suffers from. Clusters live in face_clusters and are
     * mirrored in memory (thousands of clusters x 512 floats — trivial), so
     * assignment is a brute-force scan with no index dependency.
     *
     * Live-data calibration (2026-08-23, AuraFace-v1 on the real library):
     * same-person cross-photo cosines measured 0.67-0.83, different-person
     * 0.19-0.51 — join 0.58 / merge 0.65 sit in the gap.
     *
     * Threading: NOT thread-safe; owned and driven by the single faceScan
     * scheduler job, matching the sweep's execution model.
     */
    class FaceClusterer
    {
    public:
        struct Options
        {
            double join_threshold = 0.58;
            double merge_threshold = 0.65;
            double min_quality = 0.2; // below: stored but never clustered
            // FACE-7c: a face joins only when the best cluster beats the
            // runner-up by this margin — ambiguous faces wait unclustered
            // for the catch-up instead of guessing. 0 = pre-FACE-7 joins.
            double join_margin = 0.05;
            // FACE-7b: exemplar-path consolidation. Candidate pairs pass a
            // cheap centroid pre-filter, then merge when any cross-exemplar
            // pair reaches exemplar_merge_threshold — a single strong pair
            // is decisive because live calibration put different-person
            // cosines at <= 0.51 (todo_face_recognition.md §5), while the
            // blended centroids of two looks of one person can sit far
            // apart. Exemplar selection knobs mirror FacesOps defaults.
            double merge_prefilter = 0.35;
            double exemplar_merge_threshold = 0.70;
            int exemplar_k = 5;
            double exemplar_spread = 0.80;
            double exemplar_min_quality = 0.4;
        };

        FaceClusterer(DatabaseManager *db, Options options);

        /// Mirror face_clusters AND the cannot-link constraints into
        /// memory. Called at each tick start (RPC edits land within a tick).
        void load();

        /// Assign one embedded face (unit-normalized floats) to a cluster —
        /// join-or-create — persisting faces.cluster_id and the cluster row.
        /// Returns the cluster id, or 0 when below the quality gate.
        int assign(int face_id, const std::vector<float> &embedding, double quality);

        /// Merge clusters whose centroids agree at >= merge_threshold
        /// (smaller into larger; a named cluster's identity wins over an
        /// unnamed one regardless of size). Returns merges performed.
        int consolidate();

        std::size_t clusterCount() const { return clusters_.size(); }
        double minQuality() const { return options_.min_quality; }

    private:
        struct Cluster
        {
            std::vector<float> centroid; // unit-normalized
            int size = 0;
            int cover_face_id = 0;
            double cover_quality = 0;
            // FACE-6: user-pinned cover — quality-based election skips it.
            bool cover_pinned = false;
            int person_id = 0;
            // FACE-7b: exemplar embeddings; matching scores against the best
            // of {centroid} ∪ exemplars, so a multi-mode identity matches
            // each of its looks instead of their blended average.
            std::vector<std::vector<float>> exemplars;
        };

        void persist(int id, const Cluster &c);
        bool mergeViolatesCannotLink(int lose, int win) const;
        /// FACE-7b: max cosine over centroid and exemplars.
        double score(const Cluster &c, const std::vector<float> &embedding) const;
        /// FACE-7b: persist + reload exemplars for clusters whose
        /// membership changed; called from consolidate().
        void refreshDirtyExemplars();

        DatabaseManager *db_;
        Options options_;
        std::unordered_map<int, Cluster> clusters_;
        // FACE-3b: face_id -> clusters it must never join ("not this
        // person"); consolidation must not merge a cluster into one that
        // any of its members is forbidden from.
        std::unordered_map<int, std::unordered_set<int>> cannot_links_;
        // current cluster -> clusters it must never merge INTO (union of
        // its members' constraints); maintained through in-memory merges.
        std::unordered_map<int, std::unordered_set<int>> cluster_forbidden_;
        // FACE-7b: clusters whose membership changed since their exemplars
        // were selected; consolidate() re-selects these lazily (selection
        // needs the members' embeddings — too heavy per join).
        std::unordered_set<int> exemplars_dirty_;
    };
}
