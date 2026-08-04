#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    struct ImageEmbeddingRecord
    {
        std::string file_path;
        // Note: mode field removed as mode column was removed from image_artifacts table
        std::string model; // e.g., "CLIP-RN50"
        int dim = 512;
        std::vector<std::uint8_t> embedding_blob; // float32 bytes
        int version = 1;
    };

    class ImageArtifactsOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        static bool upsertEmbedding(DatabaseManager &db, const ImageEmbeddingRecord &r);
    };
}
