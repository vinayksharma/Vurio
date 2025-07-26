#include "core/decoder/media_decoder.hpp"
#include "core/server_config_manager.hpp"
#include "core/cache/decoder_cache.hpp"
#include "logging/logger.hpp"

MediaDecoder::MediaDecoder()
{
    // Get configuration from ServerConfigManager
    auto &config_manager = ServerConfigManager::getInstance();
    max_decoder_threads_ = config_manager.getMaxDecoderThreads();

    // Get cache size from DecoderCache
    auto &decoder_cache = DecoderCache::getInstance();
    cache_size_mb_ = decoder_cache.getCacheSizeMB();

    Logger::info("MediaDecoder initialized with " + std::to_string(max_decoder_threads_) +
                 " decoder threads and " + std::to_string(cache_size_mb_) + " MB cache");
}

MediaDecoder &MediaDecoder::getInstance()
{
    static MediaDecoder instance;
    return instance;
}

int MediaDecoder::getMaxDecoderThreads() const
{
    return max_decoder_threads_;
}

uint32_t MediaDecoder::getCacheSizeMB() const
{
    return cache_size_mb_;
}