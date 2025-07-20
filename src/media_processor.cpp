#include "core/media_processor.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>

ProcessingResult MediaProcessor::processFile(const std::string &file_path, DedupMode mode)
{
    Logger::info("Processing file: " + file_path + " with mode: " + DedupModes::getModeName(mode));

    // Check if file exists and is supported
    if (!isSupportedFile(file_path))
    {
        return ProcessingResult(false, "Unsupported file type: " + file_path);
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open())
    {
        return ProcessingResult(false, "Could not open file: " + file_path);
    }

    try
    {
        // Determine media type
        std::string media_type;
        if (isImageFile(file_path))
            media_type = "image";
        else if (isVideoFile(file_path))
            media_type = "video";
        else if (isAudioFile(file_path))
            media_type = "audio";
        else
        {
            return ProcessingResult(false, "Unsupported file type: " + file_path);
        }

        // Get processing algorithm information
        const ProcessingAlgorithm *algorithm = getProcessingAlgorithm(media_type, mode);
        if (!algorithm)
        {
            return ProcessingResult(false, "No processing algorithm found for " + media_type + " with mode " + DedupModes::getModeName(mode));
        }

        Logger::info("Using algorithm: " + algorithm->name + " for " + media_type + " processing");

        // Process file based on media type and mode
        if (media_type == "image")
        {
            switch (mode)
            {
            case DedupMode::FAST:
                return processImageFast(file_path);
            case DedupMode::BALANCED:
                return processImageBalanced(file_path);
            case DedupMode::QUALITY:
                return processImageQuality(file_path);
            default:
                return ProcessingResult(false, "Unknown dedup mode for image processing");
            }
        }
        else if (media_type == "video")
        {
            switch (mode)
            {
            case DedupMode::FAST:
                return processVideoFast(file_path);
            case DedupMode::BALANCED:
                return processVideoBalanced(file_path);
            case DedupMode::QUALITY:
                return processVideoQuality(file_path);
            default:
                return ProcessingResult(false, "Unknown dedup mode for video processing");
            }
        }
        else if (media_type == "audio")
        {
            switch (mode)
            {
            case DedupMode::FAST:
                return processAudioFast(file_path);
            case DedupMode::BALANCED:
                return processAudioBalanced(file_path);
            case DedupMode::QUALITY:
                return processAudioQuality(file_path);
            default:
                return ProcessingResult(false, "Unknown dedup mode for audio processing");
            }
        }
        else
        {
            return ProcessingResult(false, "Unsupported file type: " + file_path);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error processing file: " + std::string(e.what()));
        return ProcessingResult(false, "Processing error: " + std::string(e.what()));
    }
}

bool MediaProcessor::isSupportedFile(const std::string &file_path)
{
    std::string ext = getFileExtension(file_path);
    auto supported = getSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

// Static extension lists
const std::vector<std::string> MediaProcessor::image_extensions_ = {
    "jpg", "jpeg", "png", "bmp", "gif", "tiff", "webp", "jp2", "ppm", "pgm", "pbm", "pnm", "exr", "hdr"};
const std::vector<std::string> MediaProcessor::video_extensions_ = {
    "mp4", "avi", "mov", "mkv", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "3gp", "ts", "mts", "m2ts", "ogv"};
const std::vector<std::string> MediaProcessor::audio_extensions_ = {
    "mp3", "wav", "flac", "ogg", "m4a", "aac", "opus", "wma", "aiff", "alac", "amr", "au"};

std::vector<std::string> MediaProcessor::getSupportedExtensions()
{
    std::vector<std::string> all;
    all.insert(all.end(), image_extensions_.begin(), image_extensions_.end());
    all.insert(all.end(), video_extensions_.begin(), video_extensions_.end());
    all.insert(all.end(), audio_extensions_.begin(), audio_extensions_.end());
    return all;
}

ProcessingResult MediaProcessor::processImageFast(const std::string &file_path)
{
    // Get algorithm information from lookup table
    const ProcessingAlgorithm *algorithm = getProcessingAlgorithm("image", DedupMode::FAST);
    if (!algorithm)
    {
        return ProcessingResult(false, "No processing algorithm found for image FAST mode");
    }

    Logger::info("Processing image with " + algorithm->name + ": " + file_path);

    // TODO: IMPLEMENTATION - Use algorithm information
    // Libraries needed: algorithm->libraries
    // Output format: algorithm->output_format
    // Expected data size: algorithm->data_size_bytes
    // Typical confidence: algorithm->typical_confidence

    // Placeholder implementation using algorithm info
    std::vector<uint8_t> dhash_data(algorithm->data_size_bytes, 0); // Use algorithm data size
    std::string hash = generateHash(dhash_data);

    MediaArtifact artifact;
    artifact.data = dhash_data;
    artifact.format = algorithm->output_format; // Use algorithm output format
    artifact.hash = hash;
    artifact.confidence = algorithm->typical_confidence; // Use algorithm confidence
    artifact.metadata = algorithm->metadata_template;    // Use algorithm metadata template

    ProcessingResult result(true);
    result.artifact = artifact;

    Logger::info("FAST mode processing completed for: " + file_path + " using " + algorithm->name);
    return result;
}

ProcessingResult MediaProcessor::processImageBalanced(const std::string &file_path)
{
    // Get algorithm information from lookup table
    const ProcessingAlgorithm *algorithm = getProcessingAlgorithm("image", DedupMode::BALANCED);
    if (!algorithm)
    {
        return ProcessingResult(false, "No processing algorithm found for image BALANCED mode");
    }

    Logger::info("Processing image with " + algorithm->name + ": " + file_path);

    // TODO: IMPLEMENTATION - Use algorithm information
    // Libraries needed: algorithm->libraries
    // Output format: algorithm->output_format
    // Expected data size: algorithm->data_size_bytes
    // Typical confidence: algorithm->typical_confidence

    // Placeholder implementation using algorithm info
    std::vector<uint8_t> phash_data(algorithm->data_size_bytes, 0); // Use algorithm data size
    std::string hash = generateHash(phash_data);

    MediaArtifact artifact;
    artifact.data = phash_data;
    artifact.format = algorithm->output_format; // Use algorithm output format
    artifact.hash = hash;
    artifact.confidence = algorithm->typical_confidence; // Use algorithm confidence
    artifact.metadata = algorithm->metadata_template;    // Use algorithm metadata template

    ProcessingResult result(true);
    result.artifact = artifact;

    Logger::info("BALANCED mode processing completed for: " + file_path + " using " + algorithm->name);
    return result;
}

ProcessingResult MediaProcessor::processImageQuality(const std::string &file_path)
{
    // Get algorithm information from lookup table
    const ProcessingAlgorithm *algorithm = getProcessingAlgorithm("image", DedupMode::QUALITY);
    if (!algorithm)
    {
        return ProcessingResult(false, "No processing algorithm found for image QUALITY mode");
    }

    Logger::info("Processing image with " + algorithm->name + ": " + file_path);

    // TODO: IMPLEMENTATION - Use algorithm information
    // Libraries needed: algorithm->libraries
    // Output format: algorithm->output_format
    // Expected data size: algorithm->data_size_bytes
    // Typical confidence: algorithm->typical_confidence

    // Placeholder implementation using algorithm info
    std::vector<uint8_t> embedding_data(algorithm->data_size_bytes, 0); // Use algorithm data size
    std::string hash = generateHash(embedding_data);

    MediaArtifact artifact;
    artifact.data = embedding_data;
    artifact.format = algorithm->output_format; // Use algorithm output format
    artifact.hash = hash;
    artifact.confidence = algorithm->typical_confidence; // Use algorithm confidence
    artifact.metadata = algorithm->metadata_template;    // Use algorithm metadata template

    ProcessingResult result(true);
    result.artifact = artifact;

    Logger::info("QUALITY mode processing completed for: " + file_path + " using " + algorithm->name);
    return result;
}

ProcessingResult MediaProcessor::processVideoFast(const std::string &file_path)
{
    // Get algorithm information from lookup table
    const ProcessingAlgorithm *algorithm = getProcessingAlgorithm("video", DedupMode::FAST);
    if (!algorithm)
    {
        return ProcessingResult(false, "No processing algorithm found for video FAST mode");
    }

    Logger::info("Processing video with " + algorithm->name + ": " + file_path);

    // TODO: IMPLEMENTATION - Use algorithm information
    // Libraries needed: algorithm->libraries
    // Output format: algorithm->output_format
    // Expected data size: algorithm->data_size_bytes
    // Typical confidence: algorithm->typical_confidence

    // Placeholder implementation using algorithm info
    std::vector<uint8_t> video_hash_data(algorithm->data_size_bytes, 0); // Use algorithm data size
    std::string hash = generateHash(video_hash_data);

    MediaArtifact artifact;
    artifact.data = video_hash_data;
    artifact.format = algorithm->output_format; // Use algorithm output format
    artifact.hash = hash;
    artifact.confidence = algorithm->typical_confidence; // Use algorithm confidence
    artifact.metadata = algorithm->metadata_template;    // Use algorithm metadata template

    ProcessingResult result(true);
    result.artifact = artifact;

    Logger::info("FAST mode video processing completed for: " + file_path + " using " + algorithm->name);
    return result;
}

ProcessingResult MediaProcessor::processVideoBalanced(const std::string &file_path)
{
    // Get algorithm information from lookup table
    const ProcessingAlgorithm *algorithm = getProcessingAlgorithm("video", DedupMode::BALANCED);
    if (!algorithm)
    {
        return ProcessingResult(false, "No processing algorithm found for video BALANCED mode");
    }

    Logger::info("Processing video with " + algorithm->name + ": " + file_path);

    // TODO: IMPLEMENTATION - Use algorithm information
    // Libraries needed: algorithm->libraries
    // Output format: algorithm->output_format
    // Expected data size: algorithm->data_size_bytes
    // Typical confidence: algorithm->typical_confidence

    // Placeholder implementation using algorithm info
    std::vector<uint8_t> video_hash_data(algorithm->data_size_bytes, 0); // Use algorithm data size
    std::string hash = generateHash(video_hash_data);

    MediaArtifact artifact;
    artifact.data = video_hash_data;
    artifact.format = algorithm->output_format; // Use algorithm output format
    artifact.hash = hash;
    artifact.confidence = algorithm->typical_confidence; // Use algorithm confidence
    artifact.metadata = algorithm->metadata_template;    // Use algorithm metadata template

    ProcessingResult result(true);
    result.artifact = artifact;

    Logger::info("BALANCED mode video processing completed for: " + file_path + " using " + algorithm->name);
    return result;
}

ProcessingResult MediaProcessor::processVideoQuality(const std::string &file_path)
{
    Logger::info("Processing video with QUALITY mode: " + file_path);

    // TODO: IMPLEMENTATION - FFmpeg + ONNX Runtime + CNN embeddings
    // 1. Extract key frames with FFmpeg
    // 2. Run CNN inference on each key frame
    // 3. Extract embeddings and combine
    // 4. Return binary artifact

    // Placeholder implementation
    std::vector<uint8_t> video_embedding_data(1024, 0); // Video embedding
    std::string hash = generateHash(video_embedding_data);

    MediaArtifact artifact;
    artifact.data = video_embedding_data;
    artifact.format = "video_cnn_embedding";
    artifact.hash = hash;
    artifact.confidence = 0.95;
    artifact.metadata = "{\"algorithm\":\"video_cnn_embedding\",\"model\":\"ResNet\",\"keyframes\":12,\"mode\":\"QUALITY\"}";

    ProcessingResult result(true);
    result.artifact = artifact;

    Logger::info("QUALITY mode video processing completed for: " + file_path);
    return result;
}

ProcessingResult MediaProcessor::processAudioFast(const std::string &file_path)
{
    Logger::info("Processing audio with FAST mode (Chromaprint): " + file_path);
    // TODO: IMPLEMENTATION - Chromaprint/AcoustID
    // 1. Extract fingerprint using Chromaprint
    // 2. Return binary artifact
    std::vector<uint8_t> fingerprint_data(32, 0); // Placeholder
    std::string hash = generateHash(fingerprint_data);
    MediaArtifact artifact;
    artifact.data = fingerprint_data;
    artifact.format = "chromaprint";
    artifact.hash = hash;
    artifact.confidence = 0.80;
    artifact.metadata = "{\"algorithm\":\"chromaprint\",\"mode\":\"FAST\"}";
    ProcessingResult result(true);
    result.artifact = artifact;
    Logger::info("FAST mode audio processing completed for: " + file_path);
    return result;
}

ProcessingResult MediaProcessor::processAudioBalanced(const std::string &file_path)
{
    Logger::info("Processing audio with BALANCED mode (Essentia MFCCs): " + file_path);
    // TODO: IMPLEMENTATION - Essentia/LibROSA MFCCs
    // 1. Extract MFCCs or spectral fingerprint
    // 2. Return binary artifact
    std::vector<uint8_t> mfcc_data(64, 0); // Placeholder
    std::string hash = generateHash(mfcc_data);
    MediaArtifact artifact;
    artifact.data = mfcc_data;
    artifact.format = "mfcc";
    artifact.hash = hash;
    artifact.confidence = 0.90;
    artifact.metadata = "{\"algorithm\":\"mfcc\",\"mode\":\"BALANCED\"}";
    ProcessingResult result(true);
    result.artifact = artifact;
    Logger::info("BALANCED mode audio processing completed for: " + file_path);
    return result;
}

ProcessingResult MediaProcessor::processAudioQuality(const std::string &file_path)
{
    Logger::info("Processing audio with QUALITY mode (ONNX Runtime + VGGish/YAMNet/OpenL3): " + file_path);
    // TODO: IMPLEMENTATION - ONNX Runtime + VGGish/YAMNet/OpenL3
    // 1. Extract embedding vector
    // 2. Return binary artifact
    std::vector<uint8_t> embedding_data(128, 0); // Placeholder
    std::string hash = generateHash(embedding_data);
    MediaArtifact artifact;
    artifact.data = embedding_data;
    artifact.format = "audio_embedding";
    artifact.hash = hash;
    artifact.confidence = 0.97;
    artifact.metadata = "{\"algorithm\":\"audio_embedding\",\"model\":\"VGGish/YAMNet/OpenL3\",\"mode\":\"QUALITY\"}";
    ProcessingResult result(true);
    result.artifact = artifact;
    Logger::info("QUALITY mode audio processing completed for: " + file_path);
    return result;
}

bool MediaProcessor::isImageFile(const std::string &file_path)
{
    std::string ext = getFileExtension(file_path);
    return std::find(image_extensions_.begin(), image_extensions_.end(), ext) != image_extensions_.end();
}

bool MediaProcessor::isVideoFile(const std::string &file_path)
{
    std::string ext = getFileExtension(file_path);
    return std::find(video_extensions_.begin(), video_extensions_.end(), ext) != video_extensions_.end();
}

bool MediaProcessor::isAudioFile(const std::string &file_path)
{
    std::string ext = getFileExtension(file_path);
    return std::find(audio_extensions_.begin(), audio_extensions_.end(), ext) != audio_extensions_.end();
}

std::string MediaProcessor::generateHash(const std::vector<uint8_t> &data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data.data(), data.size());
    SHA256_Final(hash, &sha256);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

std::string MediaProcessor::getFileExtension(const std::string &file_path)
{
    size_t dot_pos = file_path.find_last_of('.');
    if (dot_pos == std::string::npos)
    {
        return "";
    }

    std::string extension = file_path.substr(dot_pos + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    return extension;
}

// Static lookup table for processing algorithms
const std::unordered_map<std::string, std::unordered_map<DedupMode, ProcessingAlgorithm>> MediaProcessor::processing_algorithms_ = {
    {"image", {{DedupMode::FAST, {"dHash", "Fast perceptual hashing using OpenCV dHash algorithm", {"OpenCV"}, "dhash", 0.85, 8, "{\"algorithm\":\"dhash\",\"size\":\"9x8\",\"mode\":\"FAST\",\"libraries\":[\"OpenCV\"]}"}}, {DedupMode::BALANCED, {"pHash", "Balanced perceptual hashing using libvips + OpenCV pHash algorithm", {"libvips", "OpenCV"}, "phash", 0.92, 8, "{\"algorithm\":\"phash\",\"size\":\"32x32\",\"mode\":\"BALANCED\",\"libraries\":[\"libvips\",\"OpenCV\"]}"}}, {DedupMode::QUALITY, {"CNN Embeddings", "High-quality feature extraction using CNN embeddings via ONNX Runtime", {"ONNX Runtime", "OpenCV", "CNN Models"}, "cnn_embedding", 0.98, 512, "{\"algorithm\":\"cnn_embedding\",\"model\":\"ResNet\",\"dimensions\":512,\"mode\":\"QUALITY\",\"libraries\":[\"ONNX Runtime\",\"OpenCV\"]}"}}}},
    {"video", {{DedupMode::FAST, {"Video dHash", "Fast video fingerprinting using FFmpeg + OpenCV dHash on key frames", {"FFmpeg", "OpenCV"}, "video_dhash", 0.80, 32, "{\"algorithm\":\"video_dhash\",\"keyframes\":5,\"mode\":\"FAST\",\"libraries\":[\"FFmpeg\",\"OpenCV\"]}"}}, {DedupMode::BALANCED, {"Video pHash", "Balanced video fingerprinting using FFmpeg + libvips + OpenCV pHash on key frames", {"FFmpeg", "libvips", "OpenCV"}, "video_phash", 0.88, 32, "{\"algorithm\":\"video_phash\",\"keyframes\":8,\"mode\":\"BALANCED\",\"libraries\":[\"FFmpeg\",\"libvips\",\"OpenCV\"]}"}}, {DedupMode::QUALITY, {"Video CNN Embeddings", "High-quality video feature extraction using FFmpeg + ONNX Runtime + CNN embeddings on key frames", {"FFmpeg", "ONNX Runtime", "CNN Models"}, "video_cnn_embedding", 0.95, 1024, "{\"algorithm\":\"video_cnn_embedding\",\"model\":\"ResNet\",\"keyframes\":12,\"mode\":\"QUALITY\",\"libraries\":[\"FFmpeg\",\"ONNX Runtime\"]}"}}}},
    {"audio", {{DedupMode::FAST, {"Chromaprint", "Fast audio fingerprinting using Chromaprint/AcoustID algorithm", {"Chromaprint", "FFmpeg"}, "chromaprint", 0.80, 32, "{\"algorithm\":\"chromaprint\",\"mode\":\"FAST\",\"libraries\":[\"Chromaprint\",\"FFmpeg\"]}"}}, {DedupMode::BALANCED, {"MFCCs", "Balanced audio feature extraction using MFCCs via Essentia/LibROSA", {"Essentia", "LibROSA", "FFmpeg"}, "mfcc", 0.90, 64, "{\"algorithm\":\"mfcc\",\"mode\":\"BALANCED\",\"libraries\":[\"Essentia\",\"LibROSA\",\"FFmpeg\"]}"}}, {DedupMode::QUALITY, {"Audio Embeddings", "High-quality audio feature extraction using VGGish/YAMNet/OpenL3 via ONNX Runtime", {"ONNX Runtime", "VGGish/YAMNet/OpenL3", "FFmpeg"}, "audio_embedding", 0.97, 128, "{\"algorithm\":\"audio_embedding\",\"model\":\"VGGish/YAMNet/OpenL3\",\"mode\":\"QUALITY\",\"libraries\":[\"ONNX Runtime\",\"FFmpeg\"]}"}}}}};

const ProcessingAlgorithm *MediaProcessor::getProcessingAlgorithm(const std::string &media_type, DedupMode mode)
{
    auto media_it = processing_algorithms_.find(media_type);
    if (media_it == processing_algorithms_.end())
    {
        return nullptr;
    }

    auto mode_it = media_it->second.find(mode);
    if (mode_it == media_it->second.end())
    {
        return nullptr;
    }

    return &(mode_it->second);
}