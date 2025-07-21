#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <opencv2/core.hpp>
#include "dedup_modes.hpp"
#include "logging/logger.hpp"

/**
 * @brief Binary artifact structure for processed media
 */
struct MediaArtifact
{
    std::vector<uint8_t> data; // Binary data
    std::string format;        // Format/type of the artifact
    std::string hash;          // Hash/fingerprint of the artifact
    double confidence;         // Confidence score (0.0 to 1.0)
    std::string metadata;      // Additional metadata as JSON string

    MediaArtifact() : confidence(0.0) {}
};

/**
 * @brief Media processing result
 */
struct ProcessingResult
{
    bool success;
    std::string error_message;
    MediaArtifact artifact;

    ProcessingResult() : success(false) {}
    ProcessingResult(bool s, const std::string &msg = "")
        : success(s), error_message(msg) {}
};

/**
 * @brief Processing algorithm information
 */
struct ProcessingAlgorithm
{
    std::string name;                   // Algorithm name (e.g., "dHash", "pHash", "CNN Embeddings")
    std::string description;            // Human-readable description
    std::vector<std::string> libraries; // Required libraries (e.g., ["OpenCV", "FFmpeg"])
    std::string output_format;          // Output format (e.g., "dhash", "phash", "cnn_embedding")
    double typical_confidence;          // Typical confidence score
    int data_size_bytes;                // Typical output data size in bytes
    std::string metadata_template;      // JSON metadata template
};

/**
 * @brief Media processor for different quality modes
 *
 * This class handles media file processing based on the selected quality mode.
 * It supports images and videos with different processing strategies for each mode.
 */
class MediaProcessor
{
public:
    /**
     * @brief Process a media file and return a binary artifact
     * @param file_path Path to the media file to process
     * @param mode Quality mode for processing
     * @return ProcessingResult containing the binary artifact
     */
    static ProcessingResult processFile(const std::string &file_path, DedupMode mode);

    /**
     * @brief Get processing algorithm information for a media type and mode
     * @param media_type Media type ("image", "video", "audio")
     * @param mode Quality mode
     * @return ProcessingAlgorithm information, or nullptr if not found
     */
    static const ProcessingAlgorithm *getProcessingAlgorithm(const std::string &media_type, DedupMode mode);

    /**
     * @brief Check if a file is supported for processing
     * @param file_path Path to the file to check
     * @return true if the file type is supported
     */
    static bool isSupportedFile(const std::string &file_path);

    /**
     * @brief Get supported file extensions
     * @return Vector of supported file extensions
     */
    static std::vector<std::string> getSupportedExtensions();

    // Audio support
    static bool isAudioFile(const std::string &file_path);

    // Move these to public for testing and utility
    static bool isImageFile(const std::string &file_path);
    static bool isVideoFile(const std::string &file_path);
    static std::string generateHash(const std::vector<uint8_t> &data);
    static std::string getFileExtension(const std::string &file_path);

private:
    // Common extension lists
    static const std::vector<std::string> image_extensions_;
    static const std::vector<std::string> video_extensions_;
    static const std::vector<std::string> audio_extensions_;

    // Static lookup table for processing algorithms
    static const std::unordered_map<std::string, std::unordered_map<DedupMode, ProcessingAlgorithm>> processing_algorithms_;

    /**
     * @brief Process image file using FAST mode (OpenCV dHash)
     * @param file_path Path to the image file
     * @return ProcessingResult with dHash artifact
     */
    static ProcessingResult processImageFast(const std::string &file_path);

    /**
     * @brief Process image file using BALANCED mode (libvips + OpenCV pHash)
     * @param file_path Path to the image file
     * @return ProcessingResult with pHash artifact
     */
    static ProcessingResult processImageBalanced(const std::string &file_path);

    /**
     * @brief Process image file using QUALITY mode (CNN embeddings)
     * @param file_path Path to the image file
     * @return ProcessingResult with CNN embedding artifact
     */
    static ProcessingResult processImageQuality(const std::string &file_path);

    /**
     * @brief Process video file using FAST mode
     * @param file_path Path to the video file
     * @return ProcessingResult with video fingerprint artifact
     */
    static ProcessingResult processVideoFast(const std::string &file_path);

    /**
     * @brief Process video file using BALANCED mode
     * @param file_path Path to the video file
     * @return ProcessingResult with video fingerprint artifact
     */
    static ProcessingResult processVideoBalanced(const std::string &file_path);

    /**
     * @brief Process video file using QUALITY mode
     * @param file_path Path to the video file
     * @return ProcessingResult with video fingerprint artifact
     */
    static ProcessingResult processVideoQuality(const std::string &file_path);

    // Audio processing methods
    static ProcessingResult processAudioFast(const std::string &file_path);
    static ProcessingResult processAudioBalanced(const std::string &file_path);
    static ProcessingResult processAudioQuality(const std::string &file_path);

    // Helper functions for video processing
    static std::vector<uint8_t> generateFrameDHash(const cv::Mat &frame);
    static std::vector<uint8_t> generateFramePHash(const cv::Mat &frame);
    static std::vector<uint8_t> combineFrameHashes(const std::vector<std::vector<uint8_t>> &frame_hashes, int target_size);
};

// TODO: IMPLEMENTATION NOTES
//
// The MediaProcessor class will integrate with the following libraries:
//
// FAST MODE:
// - OpenCV for dHash generation
// - FFmpeg for video key frame extraction
//
// BALANCED MODE:
// - libvips for image thumbnail generation
// - OpenCV for pHash generation
// - FFmpeg for video key frame extraction
//
// QUALITY MODE:
// - ONNX Runtime for CNN model inference
// - FFmpeg for video key frame extraction
// - CNN models (ResNet/CLIP) for embeddings
//
// The class will be implemented in src/media_processor.cpp