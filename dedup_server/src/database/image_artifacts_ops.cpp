#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include "database/vector_index.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/LOB.h>
#include <Poco/Logger.h>
#include <cmath>
#include <string>
#include <vector>

namespace MediaDedup
{
    using namespace Poco::Data;

    bool ImageArtifactsOps::ensureTable(DatabaseManager &db)
    {
        if (!db.ensureTableExists("image_artifacts", SQL::kCreateImageArtifactsTable))
        {
            return false;
        }

        return true;
    }

    namespace
    {
        static std::string toBinaryString(const std::vector<std::uint8_t> &in)
        {
            if (in.empty())
                return std::string();
            return std::string(reinterpret_cast<const char *>(in.data()), static_cast<size_t>(in.size()));
        }

        // DF-1: L2-normalize the float32 embedding blob before it is stored,
        // so cosine similarity downstream is a bare dot product and the vector
        // index sees unit vectors (L2 distance then monotone with cosine).
        // Implemented locally (10 lines) rather than including the
        // media_processors SimilarityCalculator — the database layer must not
        // grow an upward edge. Legacy unnormalized rows stay valid: every
        // reader either re-normalizes on load (arena) or per pair (fallback).
        static void l2NormalizeEmbeddingBlob(std::vector<std::uint8_t> &blob, int dim)
        {
            if (dim <= 0 || blob.size() != static_cast<std::size_t>(dim) * sizeof(float))
                return;
            float *v = reinterpret_cast<float *>(blob.data());
            double norm_sq = 0.0;
            for (int i = 0; i < dim; ++i)
                norm_sq += static_cast<double>(v[i]) * static_cast<double>(v[i]);
            if (norm_sq < 1e-20)
                return; // zero-norm: store as-is; readers reject it anyway
            const float inv = static_cast<float>(1.0 / std::sqrt(norm_sq));
            for (int i = 0; i < dim; ++i)
                v[i] *= inv;
        }
    }

    bool ImageArtifactsOps::upsertEmbedding(DatabaseManager &db, const ImageEmbeddingRecord &r)
    {
        try
        {
            // DF-1: normalize at write time (see l2NormalizeEmbeddingBlob).
            std::vector<std::uint8_t> normalized_blob = r.embedding_blob;
            l2NormalizeEmbeddingBlob(normalized_blob, r.dim);

            int file_id = 0;
            {
                auto lease = db.acquireSessionLease();
                Session &sess = lease.get();
                Statement stmt(sess);
                std::string file_path = r.file_path;
                std::string model = r.model;
                int dim = r.dim;
                // CRITICAL FIX: Use Poco::Data::CLOB for binary data to prevent NULL-byte truncation
                // std::string would truncate at first NULL byte (0x00) in embedding data
                Poco::Data::CLOB blob(reinterpret_cast<const char *>(normalized_blob.data()), normalized_blob.size());
                int v = r.version;
                stmt << std::string(SQL::kUpsertImageEmbedding),
                    Keywords::use(file_path),
                    Keywords::use(model),
                    Keywords::use(dim),
                    Keywords::use(blob),
                    Keywords::use(v),
                    Keywords::now;

                Statement sel(sess);
                sel << std::string(SQL::kSelectScannedFileIdByPath), Keywords::use(file_path), Keywords::into(file_id), Keywords::now;
            } // POCO lease is released, freeing up lock!

            // Sync vector index when enabled (for KNN duplicate search) —
            // with the same normalized vector that was stored.
            VectorIndex *vix = db.getVectorIndex();
            if (vix && !normalized_blob.empty() && file_id > 0)
            {
                vix->insert(file_id, normalized_blob.data(), normalized_blob.size());
            }
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageArtifactsOps").error(std::string("upsertEmbedding exception: ") + e.what());
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ImageArtifactsOps").error("upsertEmbedding unknown exception");
            return false;
        }
    }
}
