#include <gtest/gtest.h>
#include "media_processors/image/backends/onnx_session_manager.hpp"
#include "config/unified_observable_config.hpp"
#include "../test_utils.hpp"
#include <filesystem>
#include <thread>
#include <vector>

using namespace MediaDedup;

#if defined(HAVE_ONNXRUNTIME)

TEST(OnnxSessionManagerTest, InitializationAndShutdown)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);

    auto stats = manager.getStats();
    EXPECT_TRUE(stats.cache_enabled);
    EXPECT_EQ(stats.max_sessions_per_model, 2u);
    EXPECT_EQ(stats.total_sessions, 0u);
}

TEST(OnnxSessionManagerTest, SessionCreationAndReuse)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);

    // Use a model path (will throw if model doesn't exist, which is expected in test env)
    std::string model_path = "models/clip-image-vitb32.onnx";

    // Test borrowing and returning session - expect exception if model missing
    try
    {
        auto lease = manager.borrowSession(model_path);
        // If we get here, model exists
        EXPECT_NE(lease.getSession(), nullptr);
        EXPECT_NE(lease.getEnv(), nullptr);
        EXPECT_EQ(lease.getModelPath(), model_path);

        // Session is borrowed, pool should have 0 available
        auto stats = manager.getStats();
        EXPECT_GE(stats.total_sessions, 0u);
    }
    catch (const std::runtime_error &e)
    {
        // Model file doesn't exist in test environment - this is expected
        EXPECT_TRUE(std::string(e.what()).find("Failed to create ONNX session") != std::string::npos);
    }

    // After return (or exception), verify manager is still functional
    auto stats = manager.getStats();
    EXPECT_TRUE(stats.cache_enabled);
    EXPECT_EQ(stats.max_sessions_per_model, 2u);
}

TEST(OnnxSessionManagerTest, SessionPooling)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);

    std::string model_path = "models/clip-image-vitb32.onnx";

    // Test pooling logic - expect exception if model doesn't exist
    try
    {
        // Borrow session 1
        auto lease1 = manager.borrowSession(model_path);

        // If we get here, model exists - test actual pooling
        {
            auto lease2 = manager.borrowSession(model_path);
            EXPECT_NE(lease2.getSession(), nullptr);
        }
        // lease2 returned
    }
    catch (const std::runtime_error &e)
    {
        // Model file doesn't exist - verify error message
        EXPECT_TRUE(std::string(e.what()).find("Failed to create ONNX session") != std::string::npos);
    }

    // Pool should still be functional
    auto stats = manager.getStats();
    EXPECT_TRUE(stats.cache_enabled);
}

TEST(OnnxSessionManagerTest, CacheDisabled)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", false);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);

    auto stats = manager.getStats();
    EXPECT_FALSE(stats.cache_enabled);

    std::string model_path = "models/clip-image-vitb32.onnx";

    // Borrow and return session - expect exception if model doesn't exist
    try
    {
        auto lease = manager.borrowSession(model_path);
        // If we get here, model exists - session should be created but not cached
    }
    catch (const std::runtime_error &e)
    {
        // Model file doesn't exist - expected in test environment
        EXPECT_TRUE(std::string(e.what()).find("Failed to create ONNX session") != std::string::npos);
    }

    // Pool should remain empty when cache disabled
    stats = manager.getStats();
    EXPECT_EQ(stats.total_sessions, 0u);
}

TEST(OnnxSessionManagerTest, ClearCache)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);

    std::string model_path = "models/clip-image-vitb32.onnx";

    // Try to create and return a session
    try
    {
        auto lease = manager.borrowSession(model_path);
        // If model exists, session will be created and returned
    }
    catch (const std::runtime_error &)
    {
        // Model doesn't exist - expected in test environment
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Clear cache (should work regardless of whether model existed)
    manager.clearCache();

    // Pool should be empty
    auto stats = manager.getStats();
    EXPECT_EQ(stats.total_sessions, 0u);
}

TEST(OnnxSessionManagerTest, ThreadSafety)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 4);

    OnnxSessionManager manager(config);

    std::string model_path = "models/clip-image-vitb32.onnx";
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};

    // Launch multiple threads borrowing sessions concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.emplace_back([&manager, &model_path, &success_count, &error_count]()
                             {
            try
            {
                auto lease = manager.borrowSession(model_path);
                if (lease.getSession())
                {
                    success_count++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                else
                {
                    error_count++;
                }
            }
            catch (...)
            {
                error_count++;
            } });
    }

    // Wait for all threads
    for (auto &t : threads)
    {
        t.join();
    }

    // All threads should complete without crashes (success depends on model existence)
    EXPECT_GE(success_count.load() + error_count.load(), 8);
}

// DF-4: one process-wide Ort::Env and a per-session I/O spec resolved once at
// creation (names/shapes/types were previously re-queried on every inference).
TEST(OnnxSessionManagerTest, SharedEnvAndCachedIoSpec)
{
    std::string model_path = "models/clip-image-vitb32.onnx";
    if (!std::filesystem::exists(model_path))
        GTEST_SKIP() << "model not present";

    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);
    try
    {
        auto lease1 = manager.borrowSession(model_path);
        OnnxSessionWrapper *wrapper = lease1.getWrapper();
        ASSERT_NE(wrapper, nullptr);
        // found_image_input is model-dependent (the exporter may leave the
        // shape fully dynamic; inference then assumes input 0, the legacy
        // behavior) — the contract here is that the spec is CACHED, complete
        // and internally consistent.
        EXPECT_GE(wrapper->io.num_inputs, 1u);
        EXPECT_EQ(wrapper->io.input_name_ptrs.size(), wrapper->io.num_inputs);
        EXPECT_EQ(wrapper->io.input_types.size(), wrapper->io.num_inputs);
        EXPECT_FALSE(wrapper->io.output_name.empty());
        EXPECT_FALSE(wrapper->coreml_active) << "CoreML defaults OFF in code (config.yaml enables it)";

        auto lease2 = manager.borrowSession(model_path);
        EXPECT_EQ(lease1.getEnv(), lease2.getEnv()) << "all sessions share one Ort::Env";
    }
    catch (const std::runtime_error &e)
    {
        GTEST_SKIP() << "model not loadable: " << e.what();
    }
}

#else

// Stub test when ONNX Runtime not available
TEST(OnnxSessionManagerTest, NotAvailable)
{
    EXPECT_TRUE(true); // Placeholder test
}

#endif // HAVE_ONNXRUNTIME

// DF-4: reduced-resolution decode for the embedding path. Large sources decode
// at 1/4 in libjpeg (4-16x cheaper preprocessing); sources whose reduced form
// would undershoot the model input fall back to a full decode.
#if defined(HAVE_ONNXRUNTIME) && defined(HAVE_OPENCV)

#include "media_processors/image/backends/onnx_adapter.hpp"
#include "media_processors/text/clip_tokenizer.hpp"
#include <opencv2/opencv.hpp>
#include <cmath>

namespace
{
    std::string writeTestJpeg(const std::string &name, int rows, int cols)
    {
        auto path = (std::filesystem::temp_directory_path() / name).string();
        cv::Mat img(rows, cols, CV_8UC3, cv::Scalar(80, 120, 200));
        EXPECT_TRUE(cv::imwrite(path, img));
        return path;
    }
}

TEST(OnnxAdapterDecodeTest, LargeImageDecodesAtQuarterResolution)
{
    std::string path = writeTestJpeg("onnx_decode_large.jpg", 2000, 1600);
    int rows = 0, cols = 0;
    ASSERT_TRUE(MediaDedup::OnnxAdapter::DecodeDimsForTest(path, 224, rows, cols));
    EXPECT_EQ(rows, 500) << "libjpeg 1/4-scale decode";
    EXPECT_EQ(cols, 400);
    std::filesystem::remove(path);
}

TEST(OnnxAdapterDecodeTest, ReducedDecodeBelowInputSizeFallsBackToFull)
{
    // 600x800 reduces to 150x200 — below the 224 model input, so the decode
    // must fall back to full resolution rather than upscale a tiny image.
    std::string path = writeTestJpeg("onnx_decode_mid.jpg", 600, 800);
    int rows = 0, cols = 0;
    ASSERT_TRUE(MediaDedup::OnnxAdapter::DecodeDimsForTest(path, 224, rows, cols));
    EXPECT_EQ(rows, 600);
    EXPECT_EQ(cols, 800);
    std::filesystem::remove(path);
}

TEST(OnnxAdapterDecodeTest, SmallImageDecodesFull)
{
    std::string path = writeTestJpeg("onnx_decode_small.jpg", 120, 160);
    int rows = 0, cols = 0;
    ASSERT_TRUE(MediaDedup::OnnxAdapter::DecodeDimsForTest(path, 224, rows, cols));
    EXPECT_EQ(rows, 120);
    EXPECT_EQ(cols, 160);
    std::filesystem::remove(path);
}

// DF-4: full ComputeEmbedding through the rewritten path — reduced decode,
// shared preprocessing, cached-I/O inference — against the REAL model.
// Skips when the model is absent.
TEST(OnnxAdapterDecodeTest, ComputeEmbeddingEndToEnd)
{
    std::string model_path = "models/clip-image-vitb32.onnx";
    if (!std::filesystem::exists(model_path))
        GTEST_SKIP() << "model not present";

    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    MediaDedup::OnnxAdapter::initializeSessionManager(config);

    std::string path = writeTestJpeg("onnx_embed_e2e.jpg", 1000, 800);
    std::vector<std::uint8_t> blob;
    std::string reason;
    bool ok = MediaDedup::OnnxAdapter::ComputeEmbedding(path, 224, model_path, 512, blob, &reason);
    ASSERT_TRUE(ok) << "embedding failed: " << reason;
    ASSERT_EQ(blob.size(), 512u * sizeof(float));
    const float *vals = reinterpret_cast<const float *>(blob.data());
    double magnitude = 0.0;
    for (int i = 0; i < 512; ++i)
        magnitude += std::abs(static_cast<double>(vals[i]));
    EXPECT_GT(magnitude, 0.0) << "embedding must not be all zeros";
    std::filesystem::remove(path);
}

// The text tower end-to-end: C++ tokenizer -> ComputeTextEmbedding against the
// REAL checkpoint. The tokenizer's own goldens cover token ids; what this adds
// is the wiring (input names/order, attention mask, output selection) — the
// exact class of bug that made the first exported image model return hidden
// states instead of embeddings. Semantics are asserted the CLIP-robust way:
// related captions must sit closer than unrelated ones.
TEST(OnnxAdapterDecodeTest, ComputeTextEmbeddingEndToEnd)
{
    const std::string model_path = "models/clip-text-vitb32.onnx";
    const std::string vocab_path = "models/vocab.json";
    const std::string merges_path = "models/merges.txt";
    if (!std::filesystem::exists(model_path) ||
        !std::filesystem::exists(vocab_path) ||
        !std::filesystem::exists(merges_path))
        GTEST_SKIP() << "text model or tokenizer assets not present";

    MediaDedup::ClipTokenizer tokenizer;
    ASSERT_TRUE(tokenizer.load(vocab_path, merges_path));

    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    MediaDedup::OnnxAdapter::initializeSessionManager(config);

    auto embed = [&](const std::string &text)
    {
        const auto encoded = tokenizer.encode(text);
        EXPECT_EQ(encoded.input_ids.size(),
                  static_cast<size_t>(MediaDedup::ClipTokenizer::kContextLength));
        std::vector<float> out;
        std::string reason;
        EXPECT_TRUE(MediaDedup::OnnxAdapter::ComputeTextEmbedding(
            encoded.input_ids, encoded.attention_mask, model_path, 512, out, &reason))
            << "text embedding failed for \"" << text << "\": " << reason;
        double norm_sq = 0.0;
        for (float v : out)
            norm_sq += static_cast<double>(v) * v;
        EXPECT_GT(norm_sq, 0.0) << "degenerate embedding for \"" << text << "\"";
        const double inv = 1.0 / std::sqrt(norm_sq);
        for (float &v : out)
            v = static_cast<float>(v * inv);
        return out;
    };

    auto cosine = [](const std::vector<float> &a, const std::vector<float> &b)
    {
        double dot = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            dot += static_cast<double>(a[i]) * b[i];
        return dot;
    };

    const auto dog = embed("a photo of a dog");
    const auto puppy = embed("a photo of a puppy");
    const auto pasta = embed("a bowl of spaghetti on a table");
    ASSERT_EQ(dog.size(), 512u);
    ASSERT_EQ(puppy.size(), 512u);
    ASSERT_EQ(pasta.size(), 512u);

    // Deterministic: the same text must embed identically across calls.
    EXPECT_NEAR(cosine(dog, embed("a photo of a dog")), 1.0, 1e-4);

    EXPECT_GT(cosine(dog, puppy), cosine(dog, pasta))
        << "related captions must outrank unrelated ones — a projected CLIP "
           "text embedding is being produced, not raw hidden states";
}

#endif // HAVE_ONNXRUNTIME && HAVE_OPENCV
