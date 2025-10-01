#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    struct ImagePhashRecord
    {
        std::string file_path;
        std::vector<std::uint8_t> phash; // 8 bytes (64-bit) initially; future-proof as BLOB
        int thumb_w = 0;
        int thumb_h = 0;
        int version = 1;
    };

    struct ImageFeaturesRecord
    {
        std::string file_path;
        std::string method;                      // "SIFT" or "ORB"
        std::vector<std::uint8_t> features_blob; // compact serialized keypoints+descriptors
        int version = 1;
    };

    struct ImageEmbeddingRecord
    {
        std::string file_path;
        std::string model; // e.g., "CLIP-RN50"
        int dim = 512;
        std::vector<std::uint8_t> embedding_blob; // float32 bytes
        int version = 1;
    };

    class ImageArtifactsOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        static bool upsertPhash(DatabaseManager &db, const ImagePhashRecord &r);
        static bool upsertFeatures(DatabaseManager &db, const ImageFeaturesRecord &r);
        static bool upsertEmbedding(DatabaseManager &db, const ImageEmbeddingRecord &r);
    };
}






