#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Poco
{
    namespace Data
    {
        class Session;
    }
}

namespace MediaDedup
{
    class DatabaseManager;

    /**
     * @brief FACE-1 schema and row operations (docs/todo_face_recognition.md).
     *
     * faces          — one row per detected face: normalized bbox, quality,
     *                  512-D identity embedding, cluster/person assignment
     *                  (NULL until FACE-2/FACE-3).
     * face_clusters  — FACE-2's clusters (created here so the schema ships
     *                  whole; empty until clustering lands).
     * persons        — named clusters (FACE-3).
     * scanned_files  — gains face_scan_version (0 = never scanned; the sweep
     *                  selects < kFaceScanVersion, so bumping the constant
     *                  re-sweeps after a model/threshold change) and
     *                  face_count (NULL = unscanned, never 0-by-default —
     *                  the EXIF NULL discipline).
     */
    class FacesOps
    {
    public:
        /// Bump to re-sweep the library after a model/threshold change.
        static constexpr int kFaceScanVersion = 1;

        struct FaceRow
        {
            int id = 0;
            int file_id = 0;
            // Normalized [0,1] image coordinates, top-left origin.
            double bbox_x = 0, bbox_y = 0, bbox_w = 0, bbox_h = 0;
            double quality = 0; // detector confidence/capture quality 0..1
            std::vector<std::uint8_t> embedding; // 512 floats, unit-normalized
            int cluster_id = 0; // CHAIN-5: filled by listFacesForFile (0 = none)
            int person_id = 0;
        };

        struct SweepCandidate
        {
            int file_id = 0;
            std::string file_path;
        };

        static bool ensureTable(DatabaseManager &db);

        /// Files eligible for the face sweep: fully processed images whose
        /// face_scan_version predates kFaceScanVersion.
        static std::vector<SweepCandidate> listSweepCandidates(DatabaseManager &db, int limit);

        /// Insert one face; returns the new faces.id (0 on failure).
        static int insertFace(DatabaseManager &db, const FaceRow &row);

        /// Stamp a file as swept: face_scan_version = kFaceScanVersion and
        /// face_count = the number of faces stored (0 is a real answer here —
        /// the file WAS scanned and has no faces; pass count < 0 for an
        /// unreadable file: attempted, answer unknown, face_count stays NULL).
        static bool markFileScanned(DatabaseManager &db, int file_id, int face_count);

        /// Remove a file's faces (re-scan after a version bump, file removal).
        static bool deleteFacesForFile(DatabaseManager &db, int file_id);

        /// Delete the faces of `file_ids` on the caller's OPEN transaction
        /// (`sess`), returning the clusters that lost members. Deleting a
        /// scanned_files row must take its faces with it — nothing else does,
        /// and a face that outlives its photo cannot be shown, counted or
        /// clustered honestly. Runs on the caller's session so it commits or
        /// rolls back with the row itself.
        ///
        /// The returned clusters are left stale on purpose: their size,
        /// centroid and cover still count the departed faces. Hand them to
        /// repairClustersAfterFaceDeletion() once the transaction has
        /// committed.
        static std::vector<int> deleteFacesForFiles(Poco::Data::Session &sess,
                                                    const std::vector<int> &file_ids);

        /// One-time FACE-9 repair, at startup: delete every face whose
        /// scanned_files row is already gone and rebuild the clusters that
        /// were counting them. Returns how many faces went. Databases written
        /// before the delete paths took faces with the row are full of these;
        /// a clean database pays one indexed count.
        static int purgeOrphanedFaces(DatabaseManager &db);

        /// Recompute the clusters deleteFacesForFiles() reported, AFTER the
        /// caller's transaction commits (each recompute takes its own
        /// session, so it must not run inside one). A cluster whose every
        /// face has gone is deleted.
        static void repairClustersAfterFaceDeletion(DatabaseManager &db,
                                                    const std::vector<int> &cluster_ids);

        static std::vector<FaceRow> listFacesForFile(DatabaseManager &db, int file_id);

        // ---- FACE-2: clusters --------------------------------------------

        struct ClusterRow
        {
            int id = 0;
            std::vector<float> centroid; // unit-normalized
            int size = 0;
            int cover_face_id = 0;
            double cover_quality = 0;
            // FACE-6: the user pinned this cover; automatic quality-based
            // re-election must leave it alone while the face is a member.
            bool cover_pinned = false;
            int person_id = 0; // 0 = unnamed
        };

        struct UnclusteredFace
        {
            int face_id = 0;
            double quality = 0;
            std::vector<float> embedding;
        };

        static std::vector<ClusterRow> listClusters(DatabaseManager &db);
        static int insertCluster(DatabaseManager &db, const ClusterRow &row);
        static bool updateCluster(DatabaseManager &db, const ClusterRow &row);
        /// Delete a cluster, its exemplars, and — unless the caller is about
        /// to hand the name to another cluster (mergeClusters) — the person
        /// it was the last cluster of.
        static bool deleteCluster(DatabaseManager &db, int cluster_id,
                                  bool reap_orphaned_person = true);

        /// Delete `person_id` and its aliases IF no cluster still carries it.
        /// Runs on the caller's session, so it commits with their work.
        static void deleteOrphanedPerson(Poco::Data::Session &sess, int person_id);

        /// Repair sweep, at startup: delete every person no cluster points at
        /// any more. Cluster deletion used to leave these behind — invisible
        /// in the People grid, which lists clusters, but still resolving in
        /// face search, so "photos of <name>" matched nothing and read as a
        /// broken search rather than an unknown name. Returns how many went.
        static int purgeOrphanedPersons(DatabaseManager &db);
        static bool setFaceCluster(DatabaseManager &db, int face_id, int cluster_id);
        /// Re-point every face of `from` at `to` (consolidation merge).
        static bool moveClusterMembers(DatabaseManager &db, int from, int to);
        /// Embedded faces awaiting a cluster (quality >= min), oldest first —
        /// the catch-up path for faces stored before clustering existed.
        static std::vector<UnclusteredFace> listUnclusteredFaces(DatabaseManager &db,
                                                                 double min_quality, int limit);

        /// FACE sweep progress for the status bar: eligible images vs images
        /// already swept at the current version. One aggregate query.
        static bool scanProgress(DatabaseManager &db, int &total, int &completed);

        // ---- FACE-3: People tab ------------------------------------------

        struct ClusterInfo
        {
            int id = 0;
            std::string name; // empty = unnamed
            int face_count = 0;
            int file_count = 0;
            std::string cover_file_path;
            double cover_x = 0, cover_y = 0, cover_w = 0, cover_h = 0;
            bool hidden = false;
            int person_id = 0;
            std::vector<std::string> aliases; // FACE-5
            int cover_face_id = 0;    // FACE-6: ring the cover in the popup
            bool cover_pinned = false; // FACE-6
        };

        /// Clusters for the People grid, size desc, with the cover face's
        /// file path + bbox so the client can crop the thumbnail itself.
        static std::vector<ClusterInfo> listClusterInfos(DatabaseManager &db, bool include_hidden);

        /// Point `c`'s cover at its best-quality member face that still has a
        /// scanned_files row, for a cluster whose elected cover no longer
        /// does. Leaves `c` untouched when every member has outlived its
        /// photo. Display-only: the stored election is not rewritten.
        static void fillCoverFromLiveMember(DatabaseManager &db, ClusterInfo &c);

        /// Assign/clear a cluster's person name. Creates (or reuses, case-
        /// insensitively) the persons row; members' faces.person_id follow.
        /// An orphaned persons row (no clusters left) is removed.
        static bool nameCluster(DatabaseManager &db, int cluster_id, const std::string &name);

        /// User-forced merge: sources' faces move to target, centroids
        /// combine size-weighted, the target's name (or the first named
        /// source's) survives. The clusterer picks the result up on its
        /// next tick reload. FACE-8: with purge_cutoff > 0 the merged
        /// cluster is purged of misfits afterwards (count via *purged).
        static bool mergeClusters(DatabaseManager &db, int target_id,
                                  const std::vector<int> &source_ids,
                                  double purge_cutoff = 0.0, int *purged = nullptr);

        /// FACE-7a: same-name commit merges the person's clusters — all of
        /// them into the largest (ties: lowest id). Naming two clusters the
        /// same is a deliberate identity statement, stronger than any
        /// centroid threshold. Returns the surviving cluster id (0 when the
        /// person has no clusters or the merge failed).
        static int mergePersonClusters(DatabaseManager &db, int person_id,
                                       double purge_cutoff = 0.0, int *purged = nullptr);

        static bool setClusterHidden(DatabaseManager &db, int cluster_id, bool hidden);

        /// Distinct file ids containing a face of this cluster (day-view
        /// drill-down filter).
        static std::vector<int> listClusterFileIds(DatabaseManager &db, int cluster_id);

        /// QP-2: case-insensitive persons.name lookup (0 = unknown). The
        /// search validator resolves person facts through this — names are
        /// runtime user data and never enter the LLM prompt.
        /// Canonical names win over aliases when both exist.
        static int personIdByName(DatabaseManager &db, const std::string &name);

        // ---- FACE-5: aliases ---------------------------------------------

        /// Replace a person's aliases. Aliases are globally unique (an
        /// alias resolves to exactly one person); one already claimed by
        /// another person is skipped, never stolen.
        static bool setPersonAliases(DatabaseManager &db, int person_id,
                                     const std::vector<std::string> &aliases);
        static std::vector<std::string> listPersonAliases(DatabaseManager &db, int person_id);

        // ---- FACE-3b: per-face corrections -------------------------------

        struct ClusterFace
        {
            int face_id = 0;
            std::string file_path;
            double x = 0, y = 0, w = 0, h = 0;
            double quality = 0;
            // FACE-7d: score against the face's own cluster (self-excluded
            // from the exemplar set); suspect flags it for popup review.
            double fit = 1.0;
            bool suspect = false;
        };

        /// A cluster's faces for the review grid, quality desc, each scored
        /// against its own cluster (FACE-7d). FACE-8: suspect_bar is the
        /// configurable fit cutoff (media.face.mergePurgeCutoff) — the same
        /// value the purge uses, so the amber ring shows exactly what a
        /// merge would set aside.
        static std::vector<ClusterFace> listClusterFaces(DatabaseManager &db, int cluster_id,
                                                         int offset, int limit, int &total,
                                                         double suspect_bar = kSuspectFitBar);

        /// "Not this person": detach the face (cluster_id/person_id NULL),
        /// record the cannot-link, and recompute the source cluster from its
        /// remaining members (deleted when empty). The sweep's catch-up
        /// re-adopts the face on its next tick, honoring the constraint.
        static bool removeFaceFromCluster(DatabaseManager &db, int face_id);

        static bool addCannotLink(DatabaseManager &db, int face_id, int cluster_id);

        struct CannotLink
        {
            int face_id = 0;
            int forbidden_cluster_id = 0;
            int current_cluster_id = 0; // 0 = currently unclustered
        };
        /// All constraints, joined with each face's CURRENT cluster so the
        /// clusterer can block consolidation merges without per-pair queries.
        static std::vector<CannotLink> listCannotLinks(DatabaseManager &db);
        /// A merge dissolves `from` into `to`: constraints follow, because
        /// "not the person of `from`" means "not the person of `to`" once
        /// they are the same identity.
        static bool repointCannotLinks(DatabaseManager &db, int from, int to);

        /// Recompute size/centroid/cover from current members; deletes the
        /// cluster row when no members remain. Returns false on DB failure.
        /// FACE-6: a pinned cover survives while the pinned face is still a
        /// member; once it leaves, the pin clears and quality elects.
        static bool recomputeCluster(DatabaseManager &db, int cluster_id);

        // ---- FACE-7b: exemplars ------------------------------------------

        /// Faces below this fit against their own cluster are flagged
        /// `suspect` for popup review — join_threshold (0.58) minus slack.
        static constexpr double kSuspectFitBar = 0.53;

        struct ExemplarRow
        {
            int cluster_id = 0;
            int face_id = 0;
            std::vector<float> embedding; // unit-normalized
        };

        /// Replace a cluster's exemplar set wholesale.
        static bool setClusterExemplars(DatabaseManager &db, int cluster_id,
                                        const std::vector<int> &face_ids);

        /// Every cluster's exemplars with embeddings, membership-checked
        /// (a face the sweep re-homed stops being an exemplar of its old
        /// cluster). One query; loaded with the clusterer mirror.
        static std::vector<ExemplarRow> listClusterExemplars(DatabaseManager &db);

        /// Re-select a cluster's exemplars: quality-first farthest-point
        /// sampling (a face joins the set only at cosine < spread to every
        /// adopted exemplar); the FACE-6 pinned cover always participates.
        /// Returns the chosen face ids (empty on failure or empty cluster).
        static std::vector<int> refreshClusterExemplars(DatabaseManager &db, int cluster_id,
                                                        int k = 5, double spread = 0.80,
                                                        double min_quality = 0.4);

        /// FACE-7d: fill fit/suspect on listed faces (parallel embeddings
        /// vector, one entry per face; empty = no embedding). FACE-8: the
        /// suspect bar is the caller's configurable cutoff.
        static void annotateFaceFits(DatabaseManager &db, int cluster_id,
                                     std::vector<ClusterFace> &faces,
                                     const std::vector<std::vector<float>> &embeddings,
                                     double suspect_bar = kSuspectFitBar);

        /// FACE-8: detach members whose fit against their own cluster
        /// (centroid ∪ exemplars, self-excluded — annotateFaceFits' score)
        /// falls below `cutoff`, then recompute the cluster. Exemplars are
        /// refreshed first so a just-merged cluster is judged by its
        /// combined model. Detached faces keep their embeddings and return
        /// to the unclustered pool with NO cannot-link — a statistical
        /// judgment, not a human verdict; they cannot immediately rejoin
        /// because joining needs join_threshold (0.58) against the same
        /// scoring function. The pinned cover is exempt. One pass, no
        /// fixed-point loop (iterating can cascade into a minority look).
        /// Returns the number of faces detached; cutoff <= 0 disables.
        static int purgeMisfits(DatabaseManager &db, int cluster_id, double cutoff);

        // ---- FACE-7e: dissolve ("Regroup faces") -------------------------

        /// Dissolve the cluster: detach its faces (embeddings kept — the
        /// sweep's catch-up regroups them within a few ticks), delete the
        /// cluster/exemplars/pin, drop cannot-links against it, and
        /// orphan-clean the person + aliases when this was their only
        /// cluster. The undo story for a bad merge. Returns false when the
        /// cluster does not exist.
        static bool dissolveCluster(DatabaseManager &db, int cluster_id);

        /// FACE-8b (Settings → Advanced): factory-reset the people layer.
        /// Detaches every face (embeddings kept) and deletes all clusters,
        /// exemplars, cannot-links, persons and aliases. The sweep's
        /// catch-up regroups from scratch — dissolveCluster, globally.
        static bool clearAllFaceGroups(DatabaseManager &db);

        // ---- FACE-6: pinned covers ---------------------------------------

        /// Pin `face_id` as the cluster's representative (cover) face. The
        /// pin survives every automatic quality-based election and merges;
        /// it clears only when that face leaves the cluster. Returns false
        /// when the face is not currently a member (the popup may race the
        /// sweep re-homing a face).
        static bool setClusterCover(DatabaseManager &db, int cluster_id, int face_id);
    };
}
