#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/face/face_align.hpp"
#include "media_processors/face/face_clusterer.hpp"
#include "media_processors/face/face_scan_service.hpp"
#include "test_utils.hpp"

namespace MediaDedup
{
    namespace Test
    {
        // FACE-1 hermetic tests: no Vision, no ONNX, no model files. The
        // detector is a fake against the FaceDetector seam; the embedder is
        // a lambda. CI stays model-free (the QP-1 discipline).

        namespace
        {
            class FakeDetector final : public FaceDetector
            {
            public:
                bool readable = true;
                int faces_per_image = 1;

                ImageBuffer loadImage(const std::string &, int) override
                {
                    ImageBuffer b;
                    if (!readable)
                        return b;
                    b.width = 200;
                    b.height = 200;
                    b.rgba.assign(200 * 200 * 4, 128);
                    return b;
                }

                std::vector<DetectedFace> detect(const ImageBuffer &) override
                {
                    std::vector<DetectedFace> out;
                    for (int i = 0; i < faces_per_image; ++i)
                    {
                        DetectedFace f;
                        f.bbox_x = 0.25;
                        f.bbox_y = 0.25;
                        f.bbox_w = 0.5;
                        f.bbox_h = 0.5;
                        f.quality = 0.8;
                        f.has_landmarks = true;
                        // Plausible eye/nose/mouth constellation.
                        const double xs[5] = {0.40, 0.60, 0.50, 0.43, 0.57};
                        const double ys[5] = {0.42, 0.42, 0.55, 0.65, 0.65};
                        for (int k = 0; k < 5; ++k)
                        {
                            f.landmarks.x[k] = xs[k];
                            f.landmarks.y[k] = ys[k];
                        }
                        out.push_back(f);
                    }
                    return out;
                }
            };
        }

        class FaceScanTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                db_path_ = (std::filesystem::path(TestUtils::getTestDataPath()) /
                            "databases" / "test_face_scan.sqlite")
                               .string();
                std::filesystem::create_directories(
                    std::filesystem::path(db_path_).parent_path());
                std::remove(db_path_.c_str());
                db_ = std::make_unique<DatabaseManager>(db_path_);
                ASSERT_TRUE(db_->initialize());
                ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
                ASSERT_TRUE(FacesOps::ensureTable(*db_));
            }

            void TearDown() override
            {
                db_.reset();
                std::remove(db_path_.c_str());
            }

            int insertProcessed(const std::string &path)
            {
                ScannedFileRow row;
                row.file_path = path;
                row.file_name = std::filesystem::path(path).filename().string();
                row.location_key = "mediaLocation:test";
                row.processed = 2;
                EXPECT_TRUE(ScannedFilesOps::upsert(*db_, row));
                auto stored = ScannedFilesOps::getByPath(*db_, path);
                EXPECT_TRUE(stored.has_value());
                return stored ? stored->id : 0;
            }

            /// A real on-disk file so the sweep's existence check passes.
            std::string tempImagePath(const char *name)
            {
                auto p = (std::filesystem::path(TestUtils::getTestDataPath()) /
                          "databases" / name)
                             .string();
                std::ofstream(p) << "not really an image";
                return p;
            }

            std::unique_ptr<DatabaseManager> db_;
            std::string db_path_;
        };

        TEST_F(FaceScanTest, FacesOpsRoundtrip)
        {
            int fid = insertProcessed("/nowhere/a.jpg");
            FacesOps::FaceRow row;
            row.file_id = fid;
            row.bbox_x = 0.1;
            row.bbox_y = 0.2;
            row.bbox_w = 0.3;
            row.bbox_h = 0.4;
            row.quality = 0.9;
            row.embedding.assign(512 * sizeof(float), 1);
            const int face_id = FacesOps::insertFace(*db_, row);
            ASSERT_GT(face_id, 0);

            auto faces = FacesOps::listFacesForFile(*db_, fid);
            ASSERT_EQ(faces.size(), 1u);
            EXPECT_EQ(faces[0].id, face_id);
            EXPECT_DOUBLE_EQ(faces[0].bbox_w, 0.3);
            EXPECT_EQ(faces[0].embedding.size(), 512 * sizeof(float));

            ASSERT_TRUE(FacesOps::deleteFacesForFile(*db_, fid));
            EXPECT_TRUE(FacesOps::listFacesForFile(*db_, fid).empty());
        }

        TEST_F(FaceScanTest, SweepSelectorHonorsVersionAndProcessedState)
        {
            int a = insertProcessed("/nowhere/a.jpg");
            ScannedFileRow unprocessed;
            unprocessed.file_path = "/nowhere/b.jpg";
            unprocessed.file_name = "b.jpg";
            unprocessed.location_key = "mediaLocation:test";
            unprocessed.processed = 0;
            ASSERT_TRUE(ScannedFilesOps::upsert(*db_, unprocessed));

            auto c1 = FacesOps::listSweepCandidates(*db_, 10);
            ASSERT_EQ(c1.size(), 1u);
            EXPECT_EQ(c1[0].file_id, a);

            ASSERT_TRUE(FacesOps::markFileScanned(*db_, a, 2));
            EXPECT_TRUE(FacesOps::listSweepCandidates(*db_, 10).empty());
        }

        TEST_F(FaceScanTest, AlignmentSimilarityTransformRecoversKnownMapping)
        {
            // Source points = template scaled by 2 and shifted by (10, 20):
            // the recovered transform must invert that exactly.
            std::array<double, 5> sx, sy;
            for (int i = 0; i < 5; ++i)
            {
                sx[i] = FaceAlign::templateX()[i] * 2.0 + 10.0;
                sy[i] = FaceAlign::templateY()[i] * 2.0 + 20.0;
            }
            std::array<double, 4> t;
            ASSERT_TRUE(FaceAlign::similarityTransform(sx, sy, t));
            EXPECT_NEAR(t[0], 0.5, 1e-9);  // scale 1/2, no rotation
            EXPECT_NEAR(t[1], 0.0, 1e-9);
            EXPECT_NEAR(t[2], -5.0, 1e-9); // -(10 * 0.5)
            EXPECT_NEAR(t[3], -10.0, 1e-9);

            // Degenerate: all points coincident.
            sx.fill(1.0);
            sy.fill(1.0);
            EXPECT_FALSE(FaceAlign::similarityTransform(sx, sy, t));
        }

        TEST_F(FaceScanTest, PreprocessMatchesArcFaceScaling)
        {
            std::vector<std::uint8_t> rgb(112 * 112 * 3, 0);
            rgb[0] = 255; // R of pixel 0
            rgb[1] = 127; // G of pixel 0 -> ~-0.0039
            auto nchw = FaceScanService::preprocessToNchw(rgb);
            ASSERT_EQ(nchw.size(), static_cast<std::size_t>(3) * 112 * 112);
            EXPECT_NEAR(nchw[0], (255 - 127.5) / 128.0, 1e-6);              // R plane
            EXPECT_NEAR(nchw[112 * 112], (127 - 127.5) / 128.0, 1e-6);      // G plane
            EXPECT_NEAR(nchw[2 * 112 * 112], (0 - 127.5) / 128.0, 1e-6);    // B plane
            EXPECT_TRUE(FaceScanService::preprocessToNchw({1, 2, 3}).empty());
        }

        TEST_F(FaceScanTest, SweepStoresFacesAndMarksFiles)
        {
            auto img = tempImagePath("face_a.jpg");
            int fid = insertProcessed(img);
            insertProcessed("/nowhere/offline.jpg"); // no file on disk

            auto detector = std::make_unique<FakeDetector>();
            detector->faces_per_image = 2;
            FaceScanService::Options opts;
            opts.embedding_dim = 4;
            FaceScanService svc(
                db_.get(), std::move(detector),
                [](const std::vector<float> &, std::vector<float> &out) {
                    out = {2.0f, 0.0f, 0.0f, 0.0f}; // unnormalized on purpose
                    return true;
                },
                opts);

            auto stats = svc.runTick();
            EXPECT_EQ(stats.scanned, 1);
            EXPECT_EQ(stats.faces, 2);
            EXPECT_EQ(stats.offline, 1);
            EXPECT_TRUE(stats.exhausted);

            auto faces = FacesOps::listFacesForFile(*db_, fid);
            ASSERT_EQ(faces.size(), 2u);
            // Stored embedding is unit-normalized: {1,0,0,0}.
            ASSERT_EQ(faces[0].embedding.size(), 4 * sizeof(float));
            float e[4];
            std::memcpy(e, faces[0].embedding.data(), sizeof(e));
            EXPECT_NEAR(e[0], 1.0f, 1e-6);
            EXPECT_NEAR(e[1], 0.0f, 1e-6);

            // The scanned file left the selector; the offline one did not
            // (it retries when its volume returns) but is remembered so it
            // cannot starve the next tick.
            auto remaining = FacesOps::listSweepCandidates(*db_, 10);
            ASSERT_EQ(remaining.size(), 1u);
            auto stats2 = svc.runTick();
            EXPECT_EQ(stats2.scanned, 0);
            EXPECT_EQ(stats2.offline, 0); // already remembered, not recounted

            std::remove(img.c_str());
        }

        // ---- FACE-2: clustering (synthetic unit vectors) ----------------

        static std::vector<float> unit4(float a, float b, float c, float d)
        {
            std::vector<float> v = {a, b, c, d};
            double n = 0;
            for (float x : v) n += double(x) * x;
            for (float &x : v) x = float(x / std::sqrt(n));
            return v;
        }

        TEST_F(FaceScanTest, ClustererJoinsAboveThresholdCreatesBelow)
        {
            int fid = insertProcessed("/nowhere/c.jpg");
            auto mkFace = [&](double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts;
            opts.join_threshold = 0.9;
            opts.min_quality = 0.2;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            EXPECT_EQ(cl.clusterCount(), 0u);

            const int f1 = mkFace(0.8), f2 = mkFace(0.9), f3 = mkFace(0.7), f4 = mkFace(0.5);

            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.8);
            ASSERT_GT(c1, 0);
            // Nearly parallel: joins (cos ~0.995 > 0.9); higher quality wins cover.
            EXPECT_EQ(cl.assign(f2, unit4(1, 0.1f, 0, 0), 0.9), c1);
            // Orthogonal: a new cluster.
            const int c2 = cl.assign(f3, unit4(0, 0, 1, 0), 0.7);
            EXPECT_GT(c2, 0);
            EXPECT_NE(c2, c1);
            // Below the quality gate: unclustered.
            EXPECT_EQ(cl.assign(f4, unit4(1, 0, 0, 0), 0.1), 0);
            EXPECT_EQ(cl.clusterCount(), 2u);

            // Persistence: a fresh clusterer reloads the same state, and the
            // cover of c1 is the higher-quality face.
            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            EXPECT_EQ(cl2.clusterCount(), 2u);
            auto rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 2u);
            const auto &r1 = rows[0].id == c1 ? rows[0] : rows[1];
            EXPECT_EQ(r1.size, 2);
            EXPECT_EQ(r1.cover_face_id, f2);
            EXPECT_NEAR(r1.cover_quality, 0.9, 1e-9);

            // faces.cluster_id persisted.
            auto faces = FacesOps::listFacesForFile(*db_, fid);
            (void)faces;
            auto unclustered = FacesOps::listUnclusteredFaces(*db_, 0.2, 10);
            // f4 has no embedding stored (FaceRow embedding empty) so it is
            // not selectable; nothing else remains unclustered.
            EXPECT_TRUE(unclustered.empty());
        }

        TEST_F(FaceScanTest, ConsolidationMergesNearClustersAndMovesFaces)
        {
            int fid = insertProcessed("/nowhere/d.jpg");
            auto mkFace = [&]() {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = 0.5;
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts;
            opts.join_threshold = 0.999; // force separate clusters first
            opts.merge_threshold = 0.9;
            FaceClusterer cl(db_.get(), opts);
            cl.load();

            const int f1 = mkFace(), f2 = mkFace(), f3 = mkFace();
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.5);
            const int c2 = cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.6); // near c1
            const int c3 = cl.assign(f3, unit4(0, 0, 1, 0), 0.5);      // far
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c3 > 0);
            EXPECT_EQ(cl.clusterCount(), 3u);

            EXPECT_EQ(cl.consolidate(), 1);
            EXPECT_EQ(cl.clusterCount(), 2u);

            auto rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 2u);
            int merged_size = 0;
            for (const auto &r : rows)
                merged_size = std::max(merged_size, r.size);
            EXPECT_EQ(merged_size, 2); // f1+f2 ended up together
        }

        TEST_F(FaceScanTest, SweepCatchUpAdoptsPreexistingFaces)
        {
            // A face stored by the FACE-1 sweep before clustering existed:
            // embedded, quality above the gate, cluster NULL.
            int fid = insertProcessed("/nowhere/e.jpg");
            FacesOps::FaceRow r;
            r.file_id = fid;
            r.quality = 0.8;
            auto v = unit4(0, 1, 0, 0);
            r.embedding.resize(v.size() * sizeof(float));
            std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
            ASSERT_GT(FacesOps::insertFace(*db_, r), 0);
            FacesOps::markFileScanned(*db_, fid, 1); // out of the extract selector

            FaceClusterer::Options copts;
            auto clusterer = std::make_shared<FaceClusterer>(db_.get(), copts);
            FaceScanService::Options opts;
            opts.embedding_dim = 4;
            FaceScanService svc(
                db_.get(), std::make_unique<FakeDetector>(),
                [](const std::vector<float> &, std::vector<float> &) { return false; },
                opts, clusterer);

            auto stats = svc.runTick();
            EXPECT_EQ(stats.scanned, 0);   // nothing to extract
            EXPECT_EQ(stats.clustered, 1); // the catch-up adopted the face
            EXPECT_EQ(clusterer->clusterCount(), 1u);
            EXPECT_TRUE(FacesOps::listUnclusteredFaces(*db_, 0.2, 10).empty());
        }

        // ---- FACE-3: People operations ----------------------------------

        TEST_F(FaceScanTest, NameMergeHideClusters)
        {
            int fid = insertProcessed("/nowhere/p.jpg");
            int fid2 = insertProcessed("/nowhere/q.jpg");
            auto mkFace = [&](int file, double q) {
                FacesOps::FaceRow r;
                r.file_id = file;
                r.quality = q;
                auto v = unit4(1, 0, 0, 0);
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts;
            opts.join_threshold = 0.999;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int c1 = cl.assign(mkFace(fid, 0.9), unit4(1, 0, 0, 0), 0.9);
            const int c2 = cl.assign(mkFace(fid2, 0.8), unit4(0, 1, 0, 0), 0.8);
            ASSERT_TRUE(c1 > 0 && c2 > 0);

            // Name c1; the info list carries it, sorted by size.
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, "Alice"));
            auto infos = FacesOps::listClusterInfos(*db_, false);
            ASSERT_EQ(infos.size(), 2u);
            const auto &named = infos[0].id == c1 ? infos[0] : infos[1];
            EXPECT_EQ(named.name, "Alice");
            EXPECT_EQ(named.file_count, 1);
            EXPECT_FALSE(named.cover_file_path.empty());

            // Renaming to a new name removes the orphaned person.
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, "Alicia"));
            infos = FacesOps::listClusterInfos(*db_, false);
            const auto &renamed = infos[0].id == c1 ? infos[0] : infos[1];
            EXPECT_EQ(renamed.name, "Alicia");

            // Hide c2: gone from the default list, present with include_hidden.
            ASSERT_TRUE(FacesOps::setClusterHidden(*db_, c2, true));
            EXPECT_EQ(FacesOps::listClusterInfos(*db_, false).size(), 1u);
            EXPECT_EQ(FacesOps::listClusterInfos(*db_, true).size(), 2u);

            // Forced merge: c2 into c1; the name survives, members move.
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c2}));
            infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].id, c1);
            EXPECT_EQ(infos[0].name, "Alicia");
            EXPECT_EQ(infos[0].face_count, 2);
            EXPECT_EQ(infos[0].file_count, 2);

            auto file_ids = FacesOps::listClusterFileIds(*db_, c1);
            ASSERT_EQ(file_ids.size(), 2u);
            EXPECT_EQ(file_ids[0], std::min(fid, fid2));

            // Clearing the name orphans and removes the person.
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, ""));
            infos = FacesOps::listClusterInfos(*db_, true);
            EXPECT_TRUE(infos[0].name.empty());
        }

        // ---- FACE-3b: "not this person" ---------------------------------

        TEST_F(FaceScanTest, RemoveFaceRecordsConstraintAndNeverRejoins)
        {
            int fid = insertProcessed("/nowhere/r.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts; // join 0.58
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(0.98f, 0.2f, 0, 0), 0.8);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(0.98f, 0.2f, 0, 0), 0.8), c1);

            // "Not this person" on f2: constraint recorded, cluster
            // recomputed (size 1, cover back to f1).
            ASSERT_TRUE(FacesOps::removeFaceFromCluster(*db_, f2));
            auto links = FacesOps::listCannotLinks(*db_);
            ASSERT_EQ(links.size(), 1u);
            EXPECT_EQ(links[0].face_id, f2);
            EXPECT_EQ(links[0].forbidden_cluster_id, c1);
            EXPECT_EQ(links[0].current_cluster_id, 0);
            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].face_count, 1);

            // The catch-up re-adopts f2 but must NOT rejoin c1 — even
            // though c1 is its best match, it founds a new cluster.
            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            const int c2 = cl2.assign(f2, unit4(0.98f, 0.2f, 0, 0), 0.8);
            ASSERT_GT(c2, 0);
            EXPECT_NE(c2, c1);
        }

        TEST_F(FaceScanTest, ConsolidationHonorsAndRepointsCannotLinks)
        {
            int fid = insertProcessed("/nowhere/s.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts;
            opts.join_threshold = 0.999; // force separate clusters
            opts.merge_threshold = 0.9;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(0.99f, 0.14f, 0, 0), 0.8);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            const int c2 = cl.assign(f2, unit4(0.99f, 0.14f, 0, 0), 0.8);
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c1 != c2);

            // f2 is "not the person of c1": near-identical centroids, but
            // the merge is blocked — the correction survives consolidation.
            ASSERT_TRUE(FacesOps::addCannotLink(*db_, f2, c1));
            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            EXPECT_EQ(cl2.consolidate(), 0);
            EXPECT_EQ(cl2.clusterCount(), 2u);

            // A third cluster CAN merge into c1; a constraint against the
            // dissolved cluster follows the identity into the winner.
            const int f3 = mkFace(unit4(0.995f, -0.1f, 0, 0), 0.7);
            const int c3 = cl2.assign(f3, unit4(0.995f, -0.1f, 0, 0), 0.7);
            ASSERT_TRUE(c3 > 0 && c3 != c1 && c3 != c2);
            const int f4 = mkFace(unit4(0, 0, 1, 0), 0.9);
            ASSERT_GT(cl2.assign(f4, unit4(0, 0, 1, 0), 0.9), 0);
            ASSERT_TRUE(FacesOps::addCannotLink(*db_, f4, c3));

            FaceClusterer cl3(db_.get(), opts);
            cl3.load();
            const int merges = cl3.consolidate();
            EXPECT_GE(merges, 1); // c3 merged with a twin (either direction)
            // Whichever direction merged, f4's constraint must now point at
            // f3's CURRENT cluster — the identity, not a dangling id.
            int f3_cluster = 0;
            for (const auto &face : FacesOps::listFacesForFile(*db_, fid))
                if (face.id == f3)
                {
                    int total = 0;
                    for (const auto &info : FacesOps::listClusterInfos(*db_, true))
                        for (const auto &cf : FacesOps::listClusterFaces(*db_, info.id, 0, 100, total))
                            if (cf.face_id == f3)
                                f3_cluster = info.id;
                }
            ASSERT_GT(f3_cluster, 0);
            bool follows = false;
            for (const auto &l : FacesOps::listCannotLinks(*db_))
                if (l.face_id == f4 && l.forbidden_cluster_id == f3_cluster)
                    follows = true;
            EXPECT_TRUE(follows) << "constraint must follow the dissolved cluster's identity";
        }

        TEST_F(FaceScanTest, ListClusterFacesPagesByQuality)
        {
            int fid = insertProcessed("/nowhere/t.jpg");
            FaceClusterer cl(db_.get(), {});
            cl.load();
            int cluster = 0;
            for (double q : {0.5, 0.9, 0.7})
            {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                auto v = unit4(1, 0, 0, 0);
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                const int id = FacesOps::insertFace(*db_, r);
                cluster = cl.assign(id, unit4(1, 0, 0, 0), q);
            }
            int total = 0;
            auto faces = FacesOps::listClusterFaces(*db_, cluster, 0, 2, total);
            EXPECT_EQ(total, 3);
            ASSERT_EQ(faces.size(), 2u);
            EXPECT_DOUBLE_EQ(faces[0].quality, 0.9);
            EXPECT_DOUBLE_EQ(faces[1].quality, 0.7);
            EXPECT_EQ(faces[0].file_path, "/nowhere/t.jpg");
        }

        // ---- FACE-5: aliases --------------------------------------------

        TEST_F(FaceScanTest, AliasesResolveAndFollowThePerson)
        {
            int fid = insertProcessed("/nowhere/u.jpg");
            FacesOps::FaceRow r;
            r.file_id = fid;
            r.quality = 0.9;
            auto v = unit4(1, 0, 0, 0);
            r.embedding.resize(v.size() * sizeof(float));
            std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
            const int face = FacesOps::insertFace(*db_, r);
            FaceClusterer cl(db_.get(), {});
            cl.load();
            const int c1 = cl.assign(face, v, 0.9);
            ASSERT_GT(c1, 0);
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, "Alice"));
            const int alice = FacesOps::personIdByName(*db_, "alice");
            ASSERT_GT(alice, 0);

            ASSERT_TRUE(FacesOps::setPersonAliases(*db_, alice, {"mom", "Ma"}));
            EXPECT_EQ(FacesOps::personIdByName(*db_, "mom"), alice);
            EXPECT_EQ(FacesOps::personIdByName(*db_, "MA"), alice); // NOCASE
            EXPECT_EQ(FacesOps::personIdByName(*db_, "dad"), 0);

            // Aliases surface on the cluster info for the UI prefill.
            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].aliases, (std::vector<std::string>{"Ma", "mom"}));

            // Canonical names win over a colliding alias: an alias "alice"
            // for a second person must not shadow the real Alice.
            ASSERT_TRUE(FacesOps::setPersonAliases(*db_, alice, {"mom"}));
            EXPECT_EQ(FacesOps::personIdByName(*db_, "ma"), 0); // replaced set

            // Renaming away removes the orphaned person AND its aliases.
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, ""));
            EXPECT_EQ(FacesOps::personIdByName(*db_, "alice"), 0);
            EXPECT_EQ(FacesOps::personIdByName(*db_, "mom"), 0);
        }

        TEST_F(FaceScanTest, UnreadableFileIsMarkedAttempted)
        {
            auto img = tempImagePath("face_bad.jpg");
            insertProcessed(img);

            auto detector = std::make_unique<FakeDetector>();
            detector->readable = false;
            FaceScanService svc(
                db_.get(), std::move(detector),
                [](const std::vector<float> &, std::vector<float> &) { return false; },
                {});

            auto stats = svc.runTick();
            EXPECT_EQ(stats.scanned, 1);
            EXPECT_EQ(stats.unreadable, 1);
            // Attempted: out of the selector despite yielding no answer.
            EXPECT_TRUE(FacesOps::listSweepCandidates(*db_, 10).empty());
            std::remove(img.c_str());
        }
    }
}
