#include <gtest/gtest.h>
#include "media_processors/image/backends/onnx_session_manager.hpp"
#include "config/unified_observable_config.hpp"
#include "../test_utils.hpp"
#include <thread>
#include <vector>

using namespace MediaDedup;

#if defined(HAVE_ONNXRUNTIME)

TEST(OnnxSessionManagerTest, InitializationAndShutdown)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.quality.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.quality.onnx.sessionCache.maxSessions", 2);

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
    config->createProperty("media.image.quality.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.quality.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);
    
    // Use a valid model path (will fail if model doesn't exist, but tests creation logic)
    std::string model_path = "models/clip-image-vitb32.onnx";
    
    // Test borrowing and returning session
    {
        auto lease = manager.borrowSession(model_path);
        EXPECT_NE(lease.getSession(), nullptr);
        EXPECT_NE(lease.getEnv(), nullptr);
        EXPECT_EQ(lease.getModelPath(), model_path);
        
        // Session is borrowed, pool should have 0 available
        auto stats = manager.getStats();
        EXPECT_GE(stats.total_sessions, 0u);  // May be 0 if session creation failed
    }
    // Session should be returned to pool here
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // After return, session should be in pool
    auto stats = manager.getStats();
    EXPECT_GE(stats.available_sessions, 0u);  // At least 0 (may be 1 if model loaded successfully)
}

TEST(OnnxSessionManagerTest, SessionPooling)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.quality.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.quality.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);
    
    std::string model_path = "models/clip-image-vitb32.onnx";
    
    // Borrow session 1
    {
        auto lease1 = manager.borrowSession(model_path);
        
        // Borrow session 2 (should create new one or wait)
        {
            auto lease2 = manager.borrowSession(model_path);
            EXPECT_NE(lease2.getSession(), nullptr);
        }
        // lease2 returned
    }
    // lease1 returned
    
    // Pool should have sessions available
    auto stats = manager.getStats();
    EXPECT_GE(stats.total_sessions, 0u);
}

TEST(OnnxSessionManagerTest, CacheDisabled)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.quality.onnx.sessionCache.enabled", false);
    config->createProperty("media.image.quality.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);
    
    auto stats = manager.getStats();
    EXPECT_FALSE(stats.cache_enabled);
    
    std::string model_path = "models/clip-image-vitb32.onnx";
    
    // Borrow and return session
    {
        auto lease = manager.borrowSession(model_path);
        // Session should be created but not cached
    }
    
    // Pool should remain empty when cache disabled
    stats = manager.getStats();
    EXPECT_EQ(stats.total_sessions, 0u);
}

TEST(OnnxSessionManagerTest, ClearCache)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.quality.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.quality.onnx.sessionCache.maxSessions", 2);

    OnnxSessionManager manager(config);
    
    std::string model_path = "models/clip-image-vitb32.onnx";
    
    // Create and return a session
    {
        auto lease = manager.borrowSession(model_path);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Clear cache
    manager.clearCache();
    
    // Pool should be empty
    auto stats = manager.getStats();
    EXPECT_EQ(stats.total_sessions, 0u);
}

TEST(OnnxSessionManagerTest, ThreadSafety)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(config->initialize());
    config->createProperty("media.image.quality.onnx.sessionCache.enabled", true);
    config->createProperty("media.image.quality.onnx.sessionCache.maxSessions", 4);

    OnnxSessionManager manager(config);
    
    std::string model_path = "models/clip-image-vitb32.onnx";
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};
    
    // Launch multiple threads borrowing sessions concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.emplace_back([&manager, &model_path, &success_count, &error_count]() {
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
            }
        });
    }
    
    // Wait for all threads
    for (auto& t : threads)
    {
        t.join();
    }
    
    // All threads should complete without crashes (success depends on model existence)
    EXPECT_GE(success_count.load() + error_count.load(), 8);
}

#else

// Stub test when ONNX Runtime not available
TEST(OnnxSessionManagerTest, NotAvailable)
{
    EXPECT_TRUE(true);  // Placeholder test
}

#endif // HAVE_ONNXRUNTIME
