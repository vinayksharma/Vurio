#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/face/face_align.hpp"
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
