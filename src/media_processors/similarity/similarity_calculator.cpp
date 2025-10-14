#include "media_processors/similarity/similarity_calculator.hpp"
#include <Poco/Logger.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace MediaDedup
{
    int SimilarityCalculator::popcount(std::uint8_t byte)
    {
        // Brian Kernighan's algorithm for counting set bits
        int count = 0;
        while (byte)
        {
            byte &= (byte - 1); // Clear the least significant bit
            count++;
        }
        return count;
    }

    int SimilarityCalculator::hammingDistance(const std::vector<std::uint8_t> &data1,
                                              const std::vector<std::uint8_t> &data2)
    {
        if (data1.size() != data2.size())
        {
            Poco::Logger::get("SimilarityCalculator").warning("Hamming distance: size mismatch (%zu vs %zu)", data1.size(), data2.size());
            return -1;
        }

        int distance = 0;
        for (size_t i = 0; i < data1.size(); ++i)
        {
            std::uint8_t xor_val = data1[i] ^ data2[i];
            distance += popcount(xor_val);
        }
        return distance;
    }

    double SimilarityCalculator::computePhashSimilarity(const std::vector<std::uint8_t> &hash1,
                                                        const std::vector<std::uint8_t> &hash2)
    {
        if (hash1.empty() || hash2.empty())
        {
            Poco::Logger::get("SimilarityCalculator").debug("Empty pHash provided");
            return 0.0;
        }

        if (hash1.size() != hash2.size())
        {
            Poco::Logger::get("SimilarityCalculator").warning("pHash size mismatch: %zu vs %zu", hash1.size(), hash2.size());
            return 0.0;
        }

        int distance = hammingDistance(hash1, hash2);
        if (distance < 0)
        {
            return 0.0;
        }

        // Convert distance to similarity: 1.0 (identical) to 0.0 (completely different)
        int max_bits = static_cast<int>(hash1.size() * 8);
        double similarity = 1.0 - (static_cast<double>(distance) / max_bits);

        Poco::Logger::get("SimilarityCalculator").trace("pHash similarity: %f (distance=%d bits out of %d)", similarity, distance, max_bits);

        return similarity;
    }

    std::vector<float> SimilarityCalculator::bytesToFloat32(const std::vector<std::uint8_t> &data)
    {
        if (data.size() % 4 != 0)
        {
            Poco::Logger::get("SimilarityCalculator").error("Byte array size %zu is not multiple of 4 for float32 conversion", data.size());
            return {};
        }

        std::vector<float> result(data.size() / 4);
        std::memcpy(result.data(), data.data(), data.size());
        return result;
    }

    double SimilarityCalculator::computeEmbeddingSimilarity(const std::vector<std::uint8_t> &embedding1,
                                                            const std::vector<std::uint8_t> &embedding2,
                                                            int dim)
    {
        if (embedding1.empty() || embedding2.empty())
        {
            Poco::Logger::get("SimilarityCalculator").debug("Empty embedding provided");
            return 0.0;
        }

        size_t expected_size = static_cast<size_t>(dim) * 4; // float32 = 4 bytes
        if (embedding1.size() != expected_size || embedding2.size() != expected_size)
        {
            Poco::Logger::get("SimilarityCalculator").warning("Embedding size mismatch: expected %zu, got %zu and %zu", expected_size, embedding1.size(), embedding2.size());
            return 0.0;
        }

        // Convert byte arrays to float32 vectors
        std::vector<float> vec1 = bytesToFloat32(embedding1);
        std::vector<float> vec2 = bytesToFloat32(embedding2);

        if (vec1.empty() || vec2.empty())
        {
            return 0.0;
        }

        // Compute cosine similarity: (A·B) / (||A|| * ||B||)
        double dot_product = 0.0;
        double norm1 = 0.0;
        double norm2 = 0.0;

        for (size_t i = 0; i < vec1.size(); ++i)
        {
            dot_product += static_cast<double>(vec1[i]) * static_cast<double>(vec2[i]);
            norm1 += static_cast<double>(vec1[i]) * static_cast<double>(vec1[i]);
            norm2 += static_cast<double>(vec2[i]) * static_cast<double>(vec2[i]);
        }

        norm1 = std::sqrt(norm1);
        norm2 = std::sqrt(norm2);

        if (norm1 < 1e-10 || norm2 < 1e-10)
        {
            Poco::Logger::get("SimilarityCalculator").warning("Zero-norm embedding detected");
            return 0.0;
        }

        double similarity = dot_product / (norm1 * norm2);

        // Clamp to [-1, 1] due to floating point errors
        similarity = std::max(-1.0, std::min(1.0, similarity));

        Poco::Logger::get("SimilarityCalculator").trace("Embedding similarity: %f (dim=%d)", similarity, dim);

        return similarity;
    }

    bool SimilarityCalculator::deserializeFeatures(const std::vector<std::uint8_t> &blob,
                                                   std::vector<std::vector<float>> &keypoints,
                                                   std::vector<std::vector<std::uint8_t>> &descriptors)
    {
        // Simple deserialization format:
        // [count:4 bytes][keypoint_count:4][descriptor_bytes_per_kp:4][kp_data...][desc_data...]

        if (blob.size() < 12) // Minimum header size
        {
            Poco::Logger::get("SimilarityCalculator").warning("Feature blob too small: %zu bytes", blob.size());
            return false;
        }

        size_t offset = 0;

        // Read count
        int32_t count;
        std::memcpy(&count, blob.data() + offset, 4);
        offset += 4;

        if (count <= 0 || count > 10000) // Sanity check
        {
            Poco::Logger::get("SimilarityCalculator").warning("Invalid feature count: %d", count);
            return false;
        }

        // Read keypoint dimension (usually 7: x, y, size, angle, response, octave, class_id)
        int32_t kp_dim;
        std::memcpy(&kp_dim, blob.data() + offset, 4);
        offset += 4;

        if (kp_dim <= 0 || kp_dim > 10) // Sanity check
        {
            Poco::Logger::get("SimilarityCalculator").warning("Invalid keypoint dimension: %d", kp_dim);
            return false;
        }

        // Read descriptor bytes per keypoint (e.g., 32 for ORB)
        int32_t desc_bytes;
        std::memcpy(&desc_bytes, blob.data() + offset, 4);
        offset += 4;

        if (desc_bytes <= 0 || desc_bytes > 512) // Sanity check
        {
            Poco::Logger::get("SimilarityCalculator").warning("Invalid descriptor bytes: %d", desc_bytes);
            return false;
        }

        size_t expected_size = 12 + (count * kp_dim * 4) + (count * desc_bytes);
        if (blob.size() < expected_size)
        {
            Poco::Logger::get("SimilarityCalculator").warning("Feature blob size mismatch: expected at least %zu, got %zu", expected_size, blob.size());
            return false;
        }

        // Read keypoints (as floats)
        keypoints.resize(count);
        for (int i = 0; i < count; ++i)
        {
            keypoints[i].resize(kp_dim);
            std::memcpy(keypoints[i].data(), blob.data() + offset, kp_dim * 4);
            offset += kp_dim * 4;
        }

        // Read descriptors
        descriptors.resize(count);
        for (int i = 0; i < count; ++i)
        {
            descriptors[i].resize(desc_bytes);
            std::memcpy(descriptors[i].data(), blob.data() + offset, desc_bytes);
            offset += desc_bytes;
        }

        return true;
    }

    double SimilarityCalculator::computeFeatureSimilarity(const std::vector<std::uint8_t> &features1,
                                                          const std::vector<std::uint8_t> &features2,
                                                          const std::string &method)
    {
        if (features1.empty() || features2.empty())
        {
            Poco::Logger::get("SimilarityCalculator").debug("Empty features provided");
            return 0.0;
        }

        // Deserialize both feature blobs
        std::vector<std::vector<float>> keypoints1, keypoints2;
        std::vector<std::vector<std::uint8_t>> descriptors1, descriptors2;

        if (!deserializeFeatures(features1, keypoints1, descriptors1))
        {
            Poco::Logger::get("SimilarityCalculator").warning("Failed to deserialize features1");
            return 0.0;
        }

        if (!deserializeFeatures(features2, keypoints2, descriptors2))
        {
            Poco::Logger::get("SimilarityCalculator").warning("Failed to deserialize features2");
            return 0.0;
        }

        if (descriptors1.empty() || descriptors2.empty())
        {
            return 0.0;
        }

        // Simplified feature matching using Hamming distance
        // For each descriptor in set 1, find best and second-best match in set 2
        int good_matches = 0;
        const double ratio_threshold = 0.75; // Lowe's ratio test threshold

        for (size_t i = 0; i < descriptors1.size(); ++i)
        {
            int best_distance = INT_MAX;
            int second_best_distance = INT_MAX;

            for (size_t j = 0; j < descriptors2.size(); ++j)
            {
                int dist = hammingDistance(descriptors1[i], descriptors2[j]);
                if (dist < 0)
                    continue;

                if (dist < best_distance)
                {
                    second_best_distance = best_distance;
                    best_distance = dist;
                }
                else if (dist < second_best_distance)
                {
                    second_best_distance = dist;
                }
            }

            // Apply Lowe's ratio test: best match must be significantly better than second best
            if (second_best_distance > 0 &&
                static_cast<double>(best_distance) / second_best_distance < ratio_threshold)
            {
                good_matches++;
            }
        }

        // Similarity is the ratio of good matches to total keypoints
        double similarity = static_cast<double>(good_matches) /
                            std::max(descriptors1.size(), descriptors2.size());

        Poco::Logger::get("SimilarityCalculator").trace("Feature similarity (%s): %f (%d good matches out of %zu/%zu features)", method.c_str(), similarity, good_matches, descriptors1.size(), descriptors2.size());

        return similarity;
    }
}

