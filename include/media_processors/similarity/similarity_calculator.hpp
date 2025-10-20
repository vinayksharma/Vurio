#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace MediaDedup
{
    /**
     * @brief Utility class for computing similarity scores between media files
     * based on different artifact types (perceptual hash, features, embeddings)
     */
    class SimilarityCalculator
    {
    public:
        /**
         * @brief Compute similarity between two perceptual hashes (pHash)
         *
         * Uses Hamming distance on 64-bit hashes. Returns value in [0.0, 1.0]
         * where 1.0 means identical and 0.0 means completely different.
         *
         * Typical threshold for duplicates: 0.85-0.95 (allowing 3-10 bits difference)
         *
         * @param hash1 First pHash (8 bytes = 64 bits)
         * @param hash2 Second pHash (8 bytes = 64 bits)
         * @return Similarity score [0.0, 1.0]
         */
        static double computePhashSimilarity(const std::vector<std::uint8_t> &hash1,
                                             const std::vector<std::uint8_t> &hash2);

        /**
         * @brief Compute Hamming distance between two byte arrays
         *
         * @param data1 First byte array
         * @param data2 Second byte array
         * @return Number of differing bits
         */
        static int hammingDistance(const std::vector<std::uint8_t> &data1,
                                   const std::vector<std::uint8_t> &data2);

        /**
         * @brief Compute cosine similarity between two embedding vectors
         *
         * Used for CLIP embeddings or other deep learning features.
         * Returns value in [-1.0, 1.0] where 1.0 means identical direction.
         *
         * Typical threshold for duplicates: 0.90-0.98
         *
         * @param embedding1 First embedding vector (float32 bytes)
         * @param embedding2 Second embedding vector (float32 bytes)
         * @param dim Dimension of embeddings (e.g., 512 for CLIP)
         * @return Cosine similarity [-1.0, 1.0]
         */
        static double computeEmbeddingSimilarity(const std::vector<std::uint8_t> &embedding1,
                                                 const std::vector<std::uint8_t> &embedding2,
                                                 int dim);

        /**
         * @brief Compute feature similarity between two ORB/SIFT feature descriptors
         *
         * Deserializes feature blobs, performs matching with Hamming distance
         * for ORB (or L2 for SIFT), and applies Lowe's ratio test.
         * Returns value in [0.0, 1.0] representing match quality.
         *
         * Typical threshold for duplicates: 0.20-0.40 (percentage of good matches)
         *
         * @param features1 First features blob
         * @param features2 Second features blob
         * @param method Feature extraction method ("ORB" or "SIFT")
         * @return Feature match similarity [0.0, 1.0]
         */
        static double computeFeatureSimilarity(const std::vector<std::uint8_t> &features1,
                                               const std::vector<std::uint8_t> &features2,
                                               const std::string &method,
                                               double ratio_threshold = 0.75,
                                               int min_good_matches = 0);

        /**
         * @brief Convert byte array to float32 vector
         *
         * @param data Byte array (must be multiple of 4)
         * @return Vector of float32 values
         */
        static std::vector<float> bytesToFloat32(const std::vector<std::uint8_t> &data);

        /**
         * @brief Deserialize OpenCV features from blob
         *
         * Format: [count:4bytes][kp1_data...][desc1_data...][kp2_data...][desc2_data...]
         *
         * @param blob Serialized features
         * @param keypoints Output keypoints (x, y, size, angle, response, octave, class_id)
         * @param descriptors Output descriptor matrix (one row per keypoint)
         * @return True if deserialization succeeded
         */
        static bool deserializeFeatures(const std::vector<std::uint8_t> &blob,
                                        std::vector<std::vector<float>> &keypoints,
                                        std::vector<std::vector<std::uint8_t>> &descriptors);

    private:
        /**
         * @brief Count number of set bits in a byte (population count)
         *
         * @param byte Input byte
         * @return Number of 1 bits
         */
        static int popcount(std::uint8_t byte);
    };
}
