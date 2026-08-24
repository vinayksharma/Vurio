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

        // ---- FACE-6: pinned covers --------------------------------------

        TEST_F(FaceScanTest, PinnedCoverSurvivesQualityElection)
        {
            int fid = insertProcessed("/nowhere/v.jpg");
            auto mkFace = [&](double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                auto v = unit4(1, 0, 0, 0);
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(0.9), f2 = mkFace(0.5);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.5), c1);

            // Pin the mediocre face; the row carries the pin and its quality.
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c1, f2));
            auto rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 1u);
            EXPECT_EQ(rows[0].cover_face_id, f2);
            EXPECT_NEAR(rows[0].cover_quality, 0.5, 1e-9);
            EXPECT_TRUE(rows[0].cover_pinned);

            // A higher-quality join must not displace the pin.
            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            const int f3 = mkFace(0.95);
            ASSERT_EQ(cl2.assign(f3, unit4(1, 0, 0.02f, 0), 0.95), c1);
            rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 1u);
            EXPECT_EQ(rows[0].cover_face_id, f2);
            EXPECT_TRUE(rows[0].cover_pinned);

            // The info list exposes the cover id + pin for the popup ring.
            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].cover_face_id, f2);
            EXPECT_TRUE(infos[0].cover_pinned);
        }

        TEST_F(FaceScanTest, SetClusterCoverRejectsNonMembers)
        {
            int fid = insertProcessed("/nowhere/w.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };

            FaceClusterer::Options opts;
            opts.join_threshold = 0.999;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(0, 1, 0, 0), 0.8);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            const int c2 = cl.assign(f2, unit4(0, 1, 0, 0), 0.8);
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c1 != c2);

            // A face from another cluster, and one from no cluster at all.
            EXPECT_FALSE(FacesOps::setClusterCover(*db_, c1, f2));
            const int loose = mkFace(unit4(0, 0, 1, 0), 0.7);
            EXPECT_FALSE(FacesOps::setClusterCover(*db_, c1, loose));
            auto rows = FacesOps::listClusters(*db_);
            for (const auto &r : rows)
                EXPECT_FALSE(r.cover_pinned);
        }

        TEST_F(FaceScanTest, MergePropagatesPinnedCovers)
        {
            int fid = insertProcessed("/nowhere/x.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            opts.join_threshold = 0.999;
            FaceClusterer cl(db_.get(), opts);
            cl.load();

            auto coverOf = [&](int id) {
                for (const auto &r : FacesOps::listClusters(*db_))
                    if (r.id == id)
                        return std::make_pair(r.cover_face_id, r.cover_pinned);
                return std::make_pair(0, false);
            };

            // pinned source -> unpinned target: the pin crosses the merge.
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(0, 1, 0, 0), 0.5);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            const int c2 = cl.assign(f2, unit4(0, 1, 0, 0), 0.5);
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c2, f2));
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c2}));
            EXPECT_EQ(coverOf(c1), std::make_pair(f2, true));

            // pinned target <- unpinned higher-quality source: pin wins.
            const int f3 = mkFace(unit4(0, 0, 1, 0), 0.99);
            const int c3 = cl.assign(f3, unit4(0, 0, 1, 0), 0.99);
            ASSERT_GT(c3, 0);
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c3}));
            EXPECT_EQ(coverOf(c1), std::make_pair(f2, true));

            // both pinned: the target the user chose keeps its own.
            const int f4 = mkFace(unit4(0, 0, 0, 1), 0.6);
            const int c4 = cl.assign(f4, unit4(0, 0, 0, 1), 0.6);
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c4, f4));
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c4}));
            EXPECT_EQ(coverOf(c1), std::make_pair(f2, true));
        }

        TEST_F(FaceScanTest, ConsolidationRespectsAndCarriesPins)
        {
            int fid = insertProcessed("/nowhere/y.jpg");
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

            // The loser (smaller cluster) carries a pin; the winner must
            // adopt it through the in-memory merge.
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f1b = mkFace(unit4(1, 0.01f, 0, 0), 0.4);
            const int f2 = mkFace(unit4(0.99f, 0.14f, 0, 0), 0.5);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f1b, unit4(1, 0.01f, 0, 0), 0.4), c1);
            const int c2 = cl.assign(f2, unit4(0.99f, 0.14f, 0, 0), 0.5);
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c1 != c2);
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c2, f2));

            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            EXPECT_EQ(cl2.consolidate(), 1);
            auto rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 1u);
            EXPECT_EQ(rows[0].cover_face_id, f2);
            EXPECT_TRUE(rows[0].cover_pinned);
        }

        TEST_F(FaceScanTest, RemovingPinnedCoverUnpinsAndReelects)
        {
            int fid = insertProcessed("/nowhere/z.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer cl(db_.get(), {});
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(1, 0.05f, 0, 0), 0.5);
            const int f3 = mkFace(unit4(1, 0, 0.05f, 0), 0.7);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.5), c1);
            ASSERT_EQ(cl.assign(f3, unit4(1, 0, 0.05f, 0), 0.7), c1);
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c1, f2));

            // Removing a NON-cover member leaves the pin alone…
            ASSERT_TRUE(FacesOps::removeFaceFromCluster(*db_, f3));
            auto rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 1u);
            EXPECT_EQ(rows[0].cover_face_id, f2);
            EXPECT_TRUE(rows[0].cover_pinned);

            // …removing the pinned cover unpins and re-elects by quality.
            ASSERT_TRUE(FacesOps::removeFaceFromCluster(*db_, f2));
            rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 1u);
            EXPECT_EQ(rows[0].cover_face_id, f1);
            EXPECT_NEAR(rows[0].cover_quality, 0.9, 1e-9);
            EXPECT_FALSE(rows[0].cover_pinned);
        }

        // ---- FACE-7: matching accuracy ----------------------------------

        TEST_F(FaceScanTest, SameNameMergeCombinesClusters)
        {
            int fid = insertProcessed("/nowhere/n1.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            // c1 twice the size of c2 — the survivor.
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(1, 0.05f, 0, 0), 0.8);
            const int f3 = mkFace(unit4(0, 1, 0, 0), 0.7);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.8), c1);
            const int c2 = cl.assign(f3, unit4(0, 1, 0, 0), 0.7);
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c1 != c2);

            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, "Alice"));
            const int alice = FacesOps::personIdByName(*db_, "Alice");
            ASSERT_TRUE(FacesOps::setPersonAliases(*db_, alice, {"mom"}));
            // The same-name act on c2 makes them one identity.
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c2, "Alice"));
            EXPECT_EQ(FacesOps::mergePersonClusters(*db_, alice), c1);

            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].id, c1);
            EXPECT_EQ(infos[0].name, "Alice");
            EXPECT_EQ(infos[0].face_count, 3);
            EXPECT_EQ(infos[0].aliases, (std::vector<std::string>{"mom"}));
        }

        TEST_F(FaceScanTest, UserMergeRepointsCannotLinks)
        {
            int fid = insertProcessed("/nowhere/n2.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            opts.join_threshold = 0.999;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(0, 1, 0, 0), 0.8);
            const int f3 = mkFace(unit4(0, 0, 1, 0), 0.7);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            const int c2 = cl.assign(f2, unit4(0, 1, 0, 0), 0.8);
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c1 != c2);

            // f3 is "not the person of c2"; the user then merges c2 into c1
            // — the constraint must follow the identity into c1 (this was
            // silently dropped before FACE-7a).
            ASSERT_TRUE(FacesOps::addCannotLink(*db_, f3, c2));
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c2}));
            auto links = FacesOps::listCannotLinks(*db_);
            ASSERT_EQ(links.size(), 1u);
            EXPECT_EQ(links[0].face_id, f3);
            EXPECT_EQ(links[0].forbidden_cluster_id, c1);
        }

        TEST_F(FaceScanTest, ExemplarSelectionQualitySpreadAndPin)
        {
            int fid = insertProcessed("/nowhere/n3.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FacesOps::ClusterRow row;
            row.centroid = unit4(1, 0, 0, 0);
            row.size = 4;
            const int cid = FacesOps::insertCluster(*db_, row);
            ASSERT_GT(cid, 0);
            const int a = mkFace(unit4(1, 0, 0, 0), 0.9);           // best
            const int b = mkFace(unit4(0.9f, 0.436f, 0, 0), 0.8);   // same look as a
            const int c = mkFace(unit4(0, 1, 0, 0), 0.7);           // distinct look
            const int d = mkFace(unit4(0, 0, 1, 0), 0.3);           // below quality bar
            for (int f : {a, b, c, d})
                ASSERT_TRUE(FacesOps::setFaceCluster(*db_, f, cid));

            // Quality-first, spread-deduped: b (cos 0.9 to a) collapses into
            // a's look; d misses the quality bar.
            auto chosen = FacesOps::refreshClusterExemplars(*db_, cid);
            EXPECT_EQ(chosen, (std::vector<int>{a, c}));

            // The pinned cover joins regardless of quality.
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, cid, d));
            chosen = FacesOps::refreshClusterExemplars(*db_, cid);
            EXPECT_EQ(chosen, (std::vector<int>{d, a, c}));
        }

        TEST_F(FaceScanTest, ExemplarScoringJoinsSecondaryMode)
        {
            int fid = insertProcessed("/nowhere/n4.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FacesOps::ClusterRow row;
            row.centroid = unit4(1, 0, 0, 0);
            row.size = 2;
            const int cid = FacesOps::insertCluster(*db_, row);
            const int a = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int c = mkFace(unit4(0, 1, 0, 0), 0.7); // secondary look
            ASSERT_TRUE(FacesOps::setFaceCluster(*db_, a, cid));
            ASSERT_TRUE(FacesOps::setFaceCluster(*db_, c, cid));
            ASSERT_EQ(FacesOps::refreshClusterExemplars(*db_, cid),
                      (std::vector<int>{a, c}));

            // A face matching the secondary exemplar but nearly orthogonal
            // to the centroid joins the cluster — pre-FACE-7 it would have
            // founded a new one.
            FaceClusterer cl(db_.get(), {});
            cl.load();
            const int nf = mkFace(unit4(0, 0.999f, 0.04f, 0), 0.8);
            EXPECT_EQ(cl.assign(nf, unit4(0, 0.999f, 0.04f, 0), 0.8), cid);
        }

        TEST_F(FaceScanTest, ConsolidationMergesViaExemplarAgreement)
        {
            int fid = insertProcessed("/nowhere/n5.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            auto mean2 = [](std::vector<float> a, std::vector<float> b) {
                std::vector<float> m(a.size());
                double n = 0;
                for (std::size_t i = 0; i < a.size(); ++i)
                {
                    m[i] = a[i] + b[i];
                    n += double(m[i]) * m[i];
                }
                for (auto &x : m)
                    x = float(x / std::sqrt(n));
                return m;
            };
            auto mkCluster = [&](std::vector<std::pair<std::vector<float>, double>> faces,
                                 std::vector<float> centroid) {
                FacesOps::ClusterRow row;
                row.centroid = std::move(centroid);
                row.size = static_cast<int>(faces.size());
                const int cid = FacesOps::insertCluster(*db_, row);
                for (auto &[v, q] : faces)
                    FacesOps::setFaceCluster(*db_, mkFace(v, q), cid);
                FacesOps::refreshClusterExemplars(*db_, cid);
                return cid;
            };

            // A and B share one look — (0.6,0.8) vs (0.62,0.785), cosine
            // ~0.9998 — but their blended centroids sit at ~0.715, below the
            // 0.9 centroid bar. The shared-look exemplar pair must merge
            // them; C overlaps A only weakly and must survive.
            const int ca = mkCluster({{unit4(1, 0, 0, 0), 0.9},
                                      {unit4(0.6f, 0.8f, 0, 0), 0.8}},
                                     mean2(unit4(1, 0, 0, 0), unit4(0.6f, 0.8f, 0, 0)));
            const int cb = mkCluster({{unit4(0.62f, 0.785f, 0, 0), 0.9},
                                      {unit4(0, 1, 0, 0), 0.8}},
                                     mean2(unit4(0.62f, 0.785f, 0, 0), unit4(0, 1, 0, 0)));
            const int cc = mkCluster({{unit4(0.5f, 0, 0.866f, 0), 0.9}},
                                     unit4(0.5f, 0, 0.866f, 0));

            FaceClusterer::Options opts;
            opts.merge_threshold = 0.9;
            opts.exemplar_merge_threshold = 0.95;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            EXPECT_EQ(cl.consolidate(), 1);
            EXPECT_EQ(cl.clusterCount(), 2u);
            // A+B merged (either direction), C intact.
            auto rows = FacesOps::listClusters(*db_);
            ASSERT_EQ(rows.size(), 2u);
            for (const auto &r : rows)
                if (r.id == cc)
                    EXPECT_EQ(r.size, 1);
                else
                    EXPECT_EQ(r.size, 4);
            (void)ca;
            (void)cb;
        }

        TEST_F(FaceScanTest, AmbiguousFaceWaitsUnclustered)
        {
            int fid = insertProcessed("/nowhere/n6.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            auto mkCluster = [&](std::vector<float> v, double q) {
                FacesOps::ClusterRow row;
                row.centroid = v;
                row.size = 1;
                const int cid = FacesOps::insertCluster(*db_, row);
                FacesOps::setFaceCluster(*db_, mkFace(v, q), cid);
                return cid;
            };
            const int c1 = mkCluster(unit4(1, 0, 0, 0), 0.9);
            const int c2 = mkCluster(unit4(0.995f, 0.0999f, 0, 0), 0.9);

            // Equidistant between two people: a guess, not a match — wait.
            FaceClusterer cl(db_.get(), {});
            cl.load();
            const int nf = mkFace(unit4(0.9988f, 0.05f, 0, 0), 0.8);
            EXPECT_EQ(cl.assign(nf, unit4(0.9988f, 0.05f, 0, 0), 0.8), 0);

            // Once the competition resolves (c2 gone), the same face joins.
            ASSERT_TRUE(FacesOps::moveClusterMembers(*db_, c2, c1));
            ASSERT_TRUE(FacesOps::deleteCluster(*db_, c2));
            FaceClusterer cl2(db_.get(), {});
            cl2.load();
            EXPECT_EQ(cl2.assign(nf, unit4(0.9988f, 0.05f, 0, 0), 0.8), c1);
        }

        TEST_F(FaceScanTest, SuspectFlagsOffIdentityFaceNeverThePin)
        {
            int fid = insertProcessed("/nowhere/n7.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FacesOps::ClusterRow row;
            row.centroid = unit4(1, 0, 0, 0);
            row.size = 3;
            const int cid = FacesOps::insertCluster(*db_, row);
            const int a = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int b = mkFace(unit4(0.99f, 0.14f, 0, 0), 0.8);
            const int x = mkFace(unit4(0, 1, 0, 0), 0.7); // not this person
            for (int f : {a, b, x})
                ASSERT_TRUE(FacesOps::setFaceCluster(*db_, f, cid));
            FacesOps::refreshClusterExemplars(*db_, cid);

            int total = 0;
            auto faces = FacesOps::listClusterFaces(*db_, cid, 0, 10, total);
            ASSERT_EQ(faces.size(), 3u);
            for (const auto &f : faces)
            {
                // fit excludes self: x scores only against a/b/centroid.
                if (f.face_id == x)
                {
                    EXPECT_LT(f.fit, 0.2);
                    EXPECT_TRUE(f.suspect);
                }
                else
                {
                    EXPECT_GT(f.fit, 0.9);
                    EXPECT_FALSE(f.suspect);
                }
            }

            // The pinned cover is the human-verified anchor — never suspect.
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, cid, x));
            faces = FacesOps::listClusterFaces(*db_, cid, 0, 10, total);
            for (const auto &f : faces)
                if (f.face_id == x)
                    EXPECT_FALSE(f.suspect);
        }

        TEST_F(FaceScanTest, DissolveClusterDetachesCleansAndRegroups)
        {
            int fid = insertProcessed("/nowhere/n8.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(1, 0.05f, 0, 0), 0.8);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.8), c1);
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, "Alice"));
            const int alice = FacesOps::personIdByName(*db_, "Alice");
            ASSERT_TRUE(FacesOps::setPersonAliases(*db_, alice, {"mom"}));
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c1, f2));
            const int outsider = mkFace(unit4(0, 1, 0, 0), 0.7);
            ASSERT_TRUE(FacesOps::addCannotLink(*db_, outsider, c1));

            ASSERT_TRUE(FacesOps::dissolveCluster(*db_, c1));
            EXPECT_FALSE(FacesOps::dissolveCluster(*db_, c1)); // already gone

            // Cluster, person, aliases, and corrections against it are gone.
            EXPECT_TRUE(FacesOps::listClusterInfos(*db_, true).empty());
            EXPECT_EQ(FacesOps::personIdByName(*db_, "Alice"), 0);
            EXPECT_EQ(FacesOps::personIdByName(*db_, "mom"), 0);
            EXPECT_TRUE(FacesOps::listCannotLinks(*db_).empty());

            // Faces kept their embeddings and await the catch-up…
            auto loose = FacesOps::listUnclusteredFaces(*db_, 0.2, 10);
            EXPECT_EQ(loose.size(), 3u); // f1, f2, and the outsider

            // …which regroups them into a fresh cluster.
            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            const int c2 = cl2.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_GT(c2, 0);
            EXPECT_NE(c2, c1);
            EXPECT_EQ(cl2.assign(f2, unit4(1, 0.05f, 0, 0), 0.8), c2);
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

        // ---- FACE-8: merge-time purge at the configurable cutoff ---------

        TEST_F(FaceScanTest, MergePurgeShedsMisfitWithoutCannotLink)
        {
            int fid = insertProcessed("/nowhere/p1.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(1, 0.05f, 0, 0), 0.85);
            const int f3 = mkFace(unit4(1, 0, 0.05f, 0), 0.8);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.85), c1);
            ASSERT_EQ(cl.assign(f3, unit4(1, 0, 0.05f, 0), 0.8), c1);
            // The stranger founds its own cluster…
            const int fm = mkFace(unit4(0, 1, 0, 0), 0.7);
            const int c2 = cl.assign(fm, unit4(0, 1, 0, 0), 0.7);
            ASSERT_TRUE(c2 > 0 && c2 != c1);

            // …and a user merge at the cutoff sheds it: against the combined
            // model (self-excluded) it scores ~0.33, below 0.53.
            int purged = -1;
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c2},
                                                FacesOps::kSuspectFitBar, &purged));
            EXPECT_EQ(purged, 1);
            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].face_count, 3);
            // Detached to the pool, NOT constrained — a statistical verdict
            // leaves the door open (unlike "not this person").
            EXPECT_TRUE(FacesOps::listCannotLinks(*db_).empty());
            bool misfit_unclustered = false;
            for (const auto &u : FacesOps::listUnclusteredFaces(*db_, 0.0, 10))
                if (u.face_id == fm)
                    misfit_unclustered = true;
            EXPECT_TRUE(misfit_unclustered);
        }

        TEST_F(FaceScanTest, MergePurgeSparesPinnedCoverAndZeroCutoffDisables)
        {
            int fid = insertProcessed("/nowhere/p2.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(mkFace(unit4(1, 0.05f, 0, 0), 0.85),
                                unit4(1, 0.05f, 0, 0), 0.85), c1);
            const int fm = mkFace(unit4(0, 1, 0, 0), 0.7);
            const int c2 = cl.assign(fm, unit4(0, 1, 0, 0), 0.7);
            ASSERT_TRUE(c2 > 0 && c2 != c1);

            // cutoff 0 = purging disabled: the misfit rides along.
            int purged = -1;
            ASSERT_TRUE(FacesOps::mergeClusters(*db_, c1, {c2}, 0.0, &purged));
            EXPECT_EQ(purged, 0);
            {
                auto infos = FacesOps::listClusterInfos(*db_, true);
                ASSERT_EQ(infos.size(), 1u);
                EXPECT_EQ(infos[0].face_count, 3);
            }

            // Pin the misfit as cover: the human-verified anchor is exempt
            // even below the bar…
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c1, fm));
            EXPECT_EQ(FacesOps::purgeMisfits(*db_, c1, FacesOps::kSuspectFitBar), 0);

            // …but once unpinned (pin a member face instead), a direct purge
            // — the config-change sweep path — sheds it.
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c1, f1));
            EXPECT_EQ(FacesOps::purgeMisfits(*db_, c1, FacesOps::kSuspectFitBar), 1);
            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 1u);
            EXPECT_EQ(infos[0].face_count, 2);
        }

        TEST_F(FaceScanTest, SuspectBarIsCallerConfigurable)
        {
            int fid = insertProcessed("/nowhere/p3.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(mkFace(unit4(1, 0.15f, 0, 0), 0.8),
                                unit4(1, 0.15f, 0, 0), 0.8), c1);

            int total = 0;
            // Two slightly-apart faces: fits are high but < 0.999.
            auto strict = FacesOps::listClusterFaces(*db_, c1, 0, 10, total, 0.999);
            ASSERT_EQ(strict.size(), 2u);
            int suspects = 0;
            for (const auto &f : strict)
                suspects += f.suspect ? 1 : 0;
            EXPECT_GT(suspects, 0);

            // Bar 0 (purging off): the ring never shows.
            auto lax = FacesOps::listClusterFaces(*db_, c1, 0, 10, total, 0.0);
            for (const auto &f : lax)
                EXPECT_FALSE(f.suspect);
        }

        TEST_F(FaceScanTest, ClearAllFaceGroupsWipesPeopleLayerAndRegroups)
        {
            int fid = insertProcessed("/nowhere/c1.jpg");
            auto mkFace = [&](std::vector<float> v, double q) {
                FacesOps::FaceRow r;
                r.file_id = fid;
                r.quality = q;
                r.embedding.resize(v.size() * sizeof(float));
                std::memcpy(r.embedding.data(), v.data(), r.embedding.size());
                return FacesOps::insertFace(*db_, r);
            };
            FaceClusterer::Options opts;
            FaceClusterer cl(db_.get(), opts);
            cl.load();
            const int f1 = mkFace(unit4(1, 0, 0, 0), 0.9);
            const int f2 = mkFace(unit4(1, 0.05f, 0, 0), 0.8);
            const int f3 = mkFace(unit4(0, 1, 0, 0), 0.7);
            const int c1 = cl.assign(f1, unit4(1, 0, 0, 0), 0.9);
            ASSERT_EQ(cl.assign(f2, unit4(1, 0.05f, 0, 0), 0.8), c1);
            const int c2 = cl.assign(f3, unit4(0, 1, 0, 0), 0.7);
            ASSERT_TRUE(c1 > 0 && c2 > 0 && c1 != c2);
            // Name + alias + pin + cannot-link: every people-layer artifact.
            ASSERT_TRUE(FacesOps::nameCluster(*db_, c1, "Alice"));
            const int alice = FacesOps::personIdByName(*db_, "Alice");
            ASSERT_TRUE(FacesOps::setPersonAliases(*db_, alice, {"mom"}));
            ASSERT_TRUE(FacesOps::setClusterCover(*db_, c1, f2));
            ASSERT_TRUE(FacesOps::addCannotLink(*db_, f3, c1));

            ASSERT_TRUE(FacesOps::clearAllFaceGroups(*db_));

            EXPECT_TRUE(FacesOps::listClusterInfos(*db_, true).empty());
            EXPECT_TRUE(FacesOps::listCannotLinks(*db_).empty());
            EXPECT_TRUE(FacesOps::listClusterExemplars(*db_).empty());
            EXPECT_EQ(FacesOps::personIdByName(*db_, "Alice"), 0);
            EXPECT_EQ(FacesOps::personIdByName(*db_, "mom"), 0);
            // Faces + embeddings survive, unclustered…
            EXPECT_EQ(FacesOps::listUnclusteredFaces(*db_, 0.0, 10).size(), 3u);

            // …and the sweep's catch-up regroups them into fresh unnamed
            // clusters, unconstrained by the dropped cannot-link.
            FaceClusterer cl2(db_.get(), opts);
            cl2.load();
            int adopted = 0;
            for (const auto &u : FacesOps::listUnclusteredFaces(*db_, opts.min_quality, 10))
                if (cl2.assign(u.face_id, u.embedding, u.quality) > 0)
                    adopted++;
            EXPECT_EQ(adopted, 3);
            auto infos = FacesOps::listClusterInfos(*db_, true);
            ASSERT_EQ(infos.size(), 2u);
            for (const auto &i : infos)
                EXPECT_TRUE(i.name.empty());
        }
    }
}
