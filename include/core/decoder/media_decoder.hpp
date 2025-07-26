#ifndef MEDIA_DECODER_HPP
#define MEDIA_DECODER_HPP

#include <cstdint>
#include <string>

/**
 * @brief MediaDecoder class for managing media decoding operations
 *
 * This class provides access to media decoder configuration settings.
 * Currently supports reading the maximum number of decoder threads.
 */
class MediaDecoder
{
public:
    /**
     * @brief Get the singleton instance of MediaDecoder
     * @return Reference to the MediaDecoder instance
     */
    static MediaDecoder &getInstance();

    /**
     * @brief Get the maximum number of decoder threads
     * @return Maximum number of decoder threads
     */
    int getMaxDecoderThreads() const;

    /**
     * @brief Get the decoder cache size in megabytes
     * @return Cache size in megabytes
     */
    uint32_t getCacheSizeMB() const;

    /**
     * @brief Destructor
     */
    ~MediaDecoder() = default;

private:
    /**
     * @brief Private constructor for singleton pattern
     */
    MediaDecoder();

    /**
     * @brief Deleted copy constructor
     */
    MediaDecoder(const MediaDecoder &) = delete;

    /**
     * @brief Deleted assignment operator
     */
    MediaDecoder &operator=(const MediaDecoder &) = delete;

    /**
     * @brief Maximum number of decoder threads
     */
    int max_decoder_threads_;

    /**
     * @brief Decoder cache size in megabytes
     */
    uint32_t cache_size_mb_;
};

#endif // MEDIA_DECODER_HPP