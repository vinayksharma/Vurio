#include "search/text_search_engine.hpp"

#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/vector_index.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"

#include <Poco/Logger.h>

#include <cmath>
#include <filesystem>

namespace MediaDedup
{
    namespace
    {
        std::string trimmed(const std::string &s)
        {
            const auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return "";
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }
    }

    TextSearchEngine::TextSearchEngine(std::shared_ptr<UnifiedObservableConfigManager> config,
                                       DatabaseManager *db)
        : config_(std::move(config)), db_(db)
    {
        Poco::Logger &logger = Poco::Logger::get("TextSearchEngine");
        text_model_path_ = "models/clip-text-vitb32.onnx";
        if (config_)
        {
            text_model_path_ = config_->getPropertyValue<std::string>(
                "media.image.onnx.textModelPath", text_model_path_);
            embedding_dim_ = config_->getPropertyValue<int>("media.image.embeddingDim", embedding_dim_);
            knn_k_ = config_->getPropertyValue<int>("search.knnK", knn_k_);
            min_score_ = config_->getPropertyValue<double>("search.minScore", min_score_);

            // Same project-root resolution as QualityPipeline uses for the
            // image model, so both towers resolve identically from any CWD.
            std::filesystem::path model_p(text_model_path_);
            if (!model_p.is_absolute())
            {
                std::filesystem::path config_path(config_->getConfigFilePath());
                std::filesystem::path abs_config = std::filesystem::absolute(config_path);
                std::filesystem::path project_root = abs_config.parent_path().parent_path();
                text_model_path_ = (project_root / model_p).lexically_normal().string();
            }
        }

        // vocab.json / merges.txt ship beside the models.
        const std::filesystem::path model_dir =
            std::filesystem::path(text_model_path_).parent_path();
        vocab_path_ = (model_dir / "vocab.json").string();
        merges_path_ = (model_dir / "merges.txt").string();

        logger.information("Text search engine configured: model=%s, knnK=%d, minScore=%.3f",
                           text_model_path_, knn_k_, min_score_);
    }

    void TextSearchEngine::ensureTokenizerLoaded()
    {
        std::call_once(tokenizer_once_, [this] {
            if (!tokenizer_.load(vocab_path_, merges_path_))
                Poco::Logger::get("TextSearchEngine")
                    .error("Failed to load CLIP tokenizer (%s, %s)", vocab_path_, merges_path_);
        });
    }

    TextSearchEngine::Result TextSearchEngine::search(const std::string &query)
    {
        Result result;
        const std::string key = trimmed(query);
        if (key.empty())
        {
            result.ok = true; // empty query = no filter; callers shouldn't get here
            return result;
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
            {
                if (it->first == key)
                {
                    cache_.splice(cache_.begin(), cache_, it); // MRU refresh
                    result.ok = true;
                    result.matches = cache_.front().second;
                    return result;
                }
            }
        }

        result = searchUncached(key);
        if (result.ok)
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.emplace_front(key, result.matches);
            while (cache_.size() > kCacheCapacity)
                cache_.pop_back();
        }
        return result;
    }

    TextSearchEngine::Result TextSearchEngine::searchUncached(const std::string &query)
    {
        Poco::Logger &logger = Poco::Logger::get("TextSearchEngine");
        Result result;

        ensureTokenizerLoaded();
        if (!tokenizer_.isLoaded())
        {
            result.error = "tokenizer_unavailable";
            return result;
        }
        if (!db_)
        {
            result.error = "database_unavailable";
            return result;
        }
        VectorIndex *index = db_->getVectorIndex();
        if (!index || !index->isReady())
        {
            result.error = "vector_index_unavailable";
            return result;
        }

        const auto encoded = tokenizer_.encode(query);
        std::vector<float> embedding;
        std::string reason;
        if (!OnnxAdapter::ComputeTextEmbedding(encoded.input_ids, encoded.attention_mask,
                                               text_model_path_, embedding_dim_,
                                               embedding, &reason))
        {
            logger.error("Text embedding failed for query '%s': %s", query, reason);
            result.error = "text_embedding_failed: " + reason;
            return result;
        }

        // Unit-normalize to match the stored image embeddings (normalized at
        // DB write time); vec0's cosine metric then behaves as a pure angle.
        double norm_sq = 0.0;
        for (float v : embedding)
            norm_sq += static_cast<double>(v) * v;
        if (norm_sq <= 0.0)
        {
            result.error = "text_embedding_degenerate";
            return result;
        }
        const float inv_norm = static_cast<float>(1.0 / std::sqrt(norm_sq));
        for (float &v : embedding)
            v *= inv_norm;

        auto knn = index->queryKnn(embedding.data(),
                                   embedding.size() * sizeof(float), knn_k_);
        result.matches.reserve(knn.size());
        for (const auto &[file_id, distance] : knn)
        {
            const double score = 1.0 - distance;
            if (score < min_score_)
                break; // ordered by distance: everything after is worse
            result.matches.push_back({file_id, score});
        }

        logger.debug("Query '%s': %zu of %zu KNN hits above minScore %.3f",
                     query, result.matches.size(), knn.size(), min_score_);
        result.ok = true;
        return result;
    }
}
