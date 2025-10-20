#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/LOB.h>
#include <Poco/Logger.h>
#include <string>

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
    }

    bool ImageArtifactsOps::upsertPhash(DatabaseManager &db, const ImagePhashRecord &r)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string file_path = r.file_path;
            std::string mode = r.mode;
            // CRITICAL FIX: Use Poco::Data::CLOB for binary data to prevent NULL-byte truncation
            Poco::Data::CLOB blob(reinterpret_cast<const char *>(r.phash.data()), r.phash.size());
            int tw = r.thumb_w;
            int th = r.thumb_h;
            int v = r.version;
            stmt << std::string(SQL::kUpsertImagePhash),
                Keywords::use(file_path),
                Keywords::use(mode),
                Keywords::use(blob),
                Keywords::use(tw),
                Keywords::use(th),
                Keywords::use(v),
                Keywords::now;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageArtifactsOps").error(std::string("upsertPhash exception: ") + e.what());
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ImageArtifactsOps").error("upsertPhash unknown exception");
            return false;
        }
    }

    bool ImageArtifactsOps::upsertFeatures(DatabaseManager &db, const ImageFeaturesRecord &r)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string file_path = r.file_path;
            std::string mode = r.mode;
            std::string method = r.method;
            // CRITICAL FIX: Use Poco::Data::CLOB for binary data to prevent NULL-byte truncation
            Poco::Data::CLOB blob(reinterpret_cast<const char *>(r.features_blob.data()), r.features_blob.size());
            int v = r.version;
            stmt << std::string(SQL::kUpsertImageFeatures),
                Keywords::use(file_path),
                Keywords::use(mode),
                Keywords::use(method),
                Keywords::use(blob),
                Keywords::use(v),
                Keywords::now;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageArtifactsOps").error(std::string("upsertFeatures exception: ") + e.what());
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ImageArtifactsOps").error("upsertFeatures unknown exception");
            return false;
        }
    }

    bool ImageArtifactsOps::upsertEmbedding(DatabaseManager &db, const ImageEmbeddingRecord &r)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string file_path = r.file_path;
            std::string mode = r.mode;
            std::string model = r.model;
            int dim = r.dim;
            // CRITICAL FIX: Use Poco::Data::CLOB for binary data to prevent NULL-byte truncation
            // std::string would truncate at first NULL byte (0x00) in embedding data
            Poco::Data::CLOB blob(reinterpret_cast<const char *>(r.embedding_blob.data()), r.embedding_blob.size());
            int v = r.version;
            stmt << std::string(SQL::kUpsertImageEmbedding),
                Keywords::use(file_path),
                Keywords::use(mode),
                Keywords::use(model),
                Keywords::use(dim),
                Keywords::use(blob),
                Keywords::use(v),
                Keywords::now;
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
