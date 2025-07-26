#ifndef DECODER_CACHE_HPP
#define DECODER_CACHE_HPP

#include <cstdint>

/**
 * @brief DecoderCache class for managing decoder cache configuration
 *
 * This class provides access to decoder cache configuration settings.
 * Currently supports reading the cache size in megabytes.
 */
class DecoderCache
{
public:
    /**
     * @brief Get the singleton instance of DecoderCache
     * @return Reference to the DecoderCache instance
     */
    static DecoderCache &getInstance();

    /**
     * @brief Get the decoder cache size in megabytes
     * @return Cache size in megabytes
     */
    uint32_t getCacheSizeMB() const;

    /**
     * @brief Destructor
     */
    ~DecoderCache() = default;

private:
    /**
     * @brief Private constructor for singleton pattern
     */
    DecoderCache();

    /**
     * @brief Deleted copy constructor
     */
    DecoderCache(const DecoderCache &) = delete;

    /**
     * @brief Deleted assignment operator
     */
    DecoderCache &operator=(const DecoderCache &) = delete;

    /**
     * @brief Cache size in megabytes
     */
    uint32_t cache_size_mb_;
};

#endif // DECODER_CACHE_HPP