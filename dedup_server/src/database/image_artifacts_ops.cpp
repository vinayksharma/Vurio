#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "database/db_retry.hpp"
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
            // Losing this write throws away a full ONNX inference and leaves
            // the file looking unprocessed, so it is paid for again on the
            // next pass. Ride out write-lock contention instead.
            if (!DbRetry::withBusyRetry("ImageArtifactsOps", "upsertEmbedding", [&]() {
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

                    // DISC-1: this vector is new or has just changed, so any
                    // discovery pass already run against it is stale. Clearing
                    // the flag in the SAME lease as the write is what finally
                    // closes DF-3 item 5 — a re-embedded file gets compared
                    // again instead of keeping edges from its old vector. On a
                    // first embedding the flag is already 0 and this is a no-op.
                    if (file_id > 0)
                    {
                        int fid = file_id;
                        Statement clr(sess);
                        clr << std::string(SQL::kClearDiscoveryDoneById), Keywords::use(fid), Keywords::now;
                    }
                })) // POCO lease is released, freeing up lock!
            {
                return false;
            }

            // Sync vector index when enabled (for KNN duplicate search) —
            // with the same normalized vector that was stored.
            //
            // This used to fail silently. A vector missing from the
            // index is invisible to every peer that runs KNN discovery before
            // reconcile() backfills it at the next startup, so a skipped insert
            // here is permanently lost edges, not a slow path. Both failure
            // modes are therefore logged AND left pending for re-discovery.
            VectorIndex *vix = db.getVectorIndex();
            if (vix && !normalized_blob.empty())
            {
                if (file_id <= 0)
                {
                    // No scanned_files row for the path we just wrote an
                    // embedding against (rename mid-flight, or the artifact
                    // landing before the scan row).
                    Poco::Logger::get("ImageArtifactsOps").warning(
                        "Vector index insert skipped: no scanned_files row for '" + r.file_path +
                        "'. Edge discovery for this file will be incomplete until it is rescanned.");
                }
                else if (!vix->insert(file_id, normalized_blob.data(), normalized_blob.size()))
                {
                    Poco::Logger::get("ImageArtifactsOps").error(
                        "Vector index insert FAILED for file_id " + std::to_string(file_id) + " ('" +
                        r.file_path + "'): left pending for edge discovery.");
                    vix->clearDiscoveryDone({file_id}, "index_insert_failed");
                }
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
