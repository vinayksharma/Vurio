#include "core/cache/decoder_cache.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"

DecoderCache::DecoderCache()
{
    // Get cache size from configuration, default to 1024 MB
    auto &config_manager = ServerConfigManager::getInstance();
    cache_size_mb_ = config_manager.getDecoderCacheSizeMB();

    Logger::info("DecoderCache initialized with cache size: " + std::to_string(cache_size_mb_) + " MB");
}

DecoderCache &DecoderCache::getInstance()
{
    static DecoderCache instance;
    return instance;
}

uint32_t DecoderCache::getCacheSizeMB() const
{
    return cache_size_mb_;
}