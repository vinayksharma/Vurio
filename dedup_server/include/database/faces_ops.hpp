#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

        static std::vector<FaceRow> listFacesForFile(DatabaseManager &db, int file_id);

        // ---- FACE-2: clusters --------------------------------------------

        struct ClusterRow
        {
            int id = 0;
            std::vector<float> centroid; // unit-normalized
            int size = 0;
            int cover_face_id = 0;
            double cover_quality = 0;
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
        static bool deleteCluster(DatabaseManager &db, int cluster_id);
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
        };

        /// Clusters for the People grid, size desc, with the cover face's
        /// file path + bbox so the client can crop the thumbnail itself.
        static std::vector<ClusterInfo> listClusterInfos(DatabaseManager &db, bool include_hidden);

        /// Assign/clear a cluster's person name. Creates (or reuses, case-
        /// insensitively) the persons row; members' faces.person_id follow.
        /// An orphaned persons row (no clusters left) is removed.
        static bool nameCluster(DatabaseManager &db, int cluster_id, const std::string &name);

        /// User-forced merge: sources' faces move to target, centroids
        /// combine size-weighted, the target's name (or the first named
        /// source's) survives. The clusterer picks the result up on its
        /// next tick reload.
        static bool mergeClusters(DatabaseManager &db, int target_id,
                                  const std::vector<int> &source_ids);

        static bool setClusterHidden(DatabaseManager &db, int cluster_id, bool hidden);

        /// Distinct file ids containing a face of this cluster (day-view
        /// drill-down filter).
        static std::vector<int> listClusterFileIds(DatabaseManager &db, int cluster_id);

        /// QP-2: case-insensitive persons.name lookup (0 = unknown). The
        /// search validator resolves person facts through this — names are
        /// runtime user data and never enter the LLM prompt.
        static int personIdByName(DatabaseManager &db, const std::string &name);
    };
}
