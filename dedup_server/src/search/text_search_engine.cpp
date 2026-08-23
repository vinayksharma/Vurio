#include "search/text_search_engine.hpp"

#include "search/exif_keyword_config.hpp"

#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/vector_index.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"

#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

#include <cmath>
#include <filesystem>
#include <sstream>
#include <unordered_set>

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
            exif_only_limit_ = config_->getPropertyValue<int>("search.exifOnlyLimit", exif_only_limit_);
            locations_path_ = config_->getPropertyValue<std::string>(
                "search.locationsPath", "geodata/world_locations.geojson");
            keywords_path_ = config_->getPropertyValue<std::string>(
                "search.keywordsPath", "config/exif_search_keywords.json");

            // Same project-root resolution as QualityPipeline uses for the
            // image model, so both towers resolve identically from any CWD.
            std::filesystem::path config_path(config_->getConfigFilePath());
            std::filesystem::path abs_config = std::filesystem::absolute(config_path);
            std::filesystem::path project_root = abs_config.parent_path().parent_path();

            std::filesystem::path model_p(text_model_path_);
            if (!model_p.is_absolute())
                text_model_path_ = (project_root / model_p).lexically_normal().string();

            std::filesystem::path loc_p(locations_path_);
            if (!loc_p.is_absolute())
                locations_path_ = (project_root / loc_p).lexically_normal().string();

            std::filesystem::path kw_p(keywords_path_);
            if (!kw_p.is_absolute())
                keywords_path_ = (project_root / kw_p).lexically_normal().string();
        }
        else
        {
            locations_path_ = "geodata/world_locations.geojson";
            keywords_path_ = "config/exif_search_keywords.json";
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

    void TextSearchEngine::ensureGeocoderLoaded()
    {
        std::call_once(geocoder_once_, [this] {
            // Optional: without a gazetteer, location parsing is skipped but
            // every other EXIF constraint still works.
            if (!geocoder_.loadFromFile(locations_path_))
                Poco::Logger::get("TextSearchEngine")
                    .warning("Location gazetteer unavailable (%s); place-name search disabled",
                             locations_path_);
        });
    }

    void TextSearchEngine::ensureParserLoaded()
    {
        std::call_once(parser_once_, [this] {
            ensureGeocoderLoaded();
            // EXIF-6: vocabulary from JSON; built-in defaults are the
            // fallback, so a missing file degrades to stock keywords.
            ExifKeywordConfig keywords = ExifKeywordConfig::defaults();
            if (!keywords.loadFromFile(keywords_path_))
                Poco::Logger::get("TextSearchEngine")
                    .warning("EXIF keyword file unavailable (%s); using built-in keywords",
                             keywords_path_);
            parser_ = std::make_unique<ExifQueryParser>(&geocoder_, keywords);
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
                if (it->query == key)
                {
                    cache_.splice(cache_.begin(), cache_, it); // MRU refresh
                    result.ok = true;
                    result.matches = cache_.front().matches;
                    result.used_semantic = cache_.front().used_semantic;
                    result.used_exif = cache_.front().used_exif;
                    return result;
                }
            }
        }

        result = searchUncached(key);
        if (result.ok)
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.push_front({key, result.matches, result.used_semantic, result.used_exif});
            while (cache_.size() > kCacheCapacity)
                cache_.pop_back();
        }
        return result;
    }

    TextSearchEngine::Result TextSearchEngine::searchUncached(const std::string &query)
    {
        Poco::Logger &logger = Poco::Logger::get("TextSearchEngine");

        // EXIF-4: split the query into structured EXIF constraints and the
        // semantic remainder, then combine SQL filtering with CLIP ranking.
        ensureParserLoaded();
        ExifQueryConstraints constraints = parser_->parse(query);

        if (!constraints.hasAny())
        {
            Result r = clipSearch(query);
            r.used_semantic = r.ok;
            return r;
        }

        logger.information(
            "Query '%s': %z numeric constraint(s), %z camera term(s)%s%s; remaining text: '%s'",
            query, constraints.numeric.size(), constraints.camera_terms.size(),
            constraints.location ? (", location=" + constraints.location_name) : std::string(),
            constraints.date_range ? std::string(", date range") : std::string(),
            constraints.remaining);

        // Both halves always run and the result is their UNION: a fact query
        // like "f2.5" still surfaces what CLIP finds for the text, and a
        // mixed query keeps semantic hits that fail the constraints. Each
        // match's via_* flags say which half admitted it (the VU ai/xf
        // badges), so showing the superset loses no information.
        Result exif = exifOnlySearch(constraints);

        // When the remainder is pure filler ("photos", "show me") CLIP gets
        // the raw query text instead, so the semantic half still runs.
        const std::string semantic_text =
            constraints.remaining_is_filler ? query : constraints.remaining;
        Result clip = clipSearch(semantic_text);

        if (!exif.ok && !clip.ok)
            return exif; // exif's error names the SQL problem

        if (exif.ok != clip.ok)
            logger.warning("Hybrid search running on one half only (%s down): %s",
                           std::string(exif.ok ? "semantic" : "exif"),
                           exif.ok ? clip.error : exif.error);

        Result result;
        result.ok = true;
        result.used_semantic = clip.ok;
        result.used_exif = exif.ok;

        std::unordered_set<int> exif_ids;
        exif_ids.reserve(exif.matches.size());
        for (const auto &m : exif.matches)
            exif_ids.insert(m.file_id);

        // CLIP-ranked matches first (flagged when they also satisfy the
        // constraints), then the constraint matches CLIP did not rank.
        result.matches.reserve(clip.matches.size() + exif.matches.size());
        std::unordered_set<int> emitted;
        emitted.reserve(clip.matches.size());
        for (auto m : clip.matches)
        {
            m.via_exif = exif_ids.count(m.file_id) > 0;
            emitted.insert(m.file_id);
            result.matches.push_back(m);
        }
        for (const auto &m : exif.matches)
        {
            if (!emitted.count(m.file_id))
                result.matches.push_back(m);
        }
        return result;
    }

    TextSearchEngine::Result TextSearchEngine::clipSearch(const std::string &query)
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
            result.matches.push_back({file_id, score, /*via_semantic=*/true, /*via_exif=*/false});
        }

        logger.debug("Query '%s': %zu of %zu KNN hits above minScore %.3f",
                     query, result.matches.size(), knn.size(), min_score_);
        result.ok = true;
        return result;
    }

    namespace
    {
        // Column names come from the parser's fixed tables, never from user
        // text — but keep an allowlist anyway so a parser bug cannot smuggle
        // SQL through a column slot.
        bool allowedExifColumn(const std::string &col)
        {
            static const char *kAllowed[] = {"exif_iso", "exif_f_number",
                                             "exif_exposure_time", "exif_focal_length",
                                             "exif_exposure_comp"};
            for (const char *a : kAllowed)
                if (col == a)
                    return true;
            return false;
        }

        // WHERE fragment for the parsed constraints. Numeric values are
        // formatted from validated doubles; camera terms come back as binds.
        std::string buildExifWhere(const ExifQueryConstraints &c,
                                   std::vector<std::string> &camera_binds)
        {
            std::ostringstream w;
            auto num = [](double v) {
                std::ostringstream o;
                o << v;
                return o.str();
            };
            for (const auto &n : c.numeric)
            {
                if (!allowedExifColumn(n.column))
                    continue;
                if (n.op == "between")
                    w << " AND " << n.column << " BETWEEN " << num(n.value)
                      << " AND " << num(n.value2);
                else if (n.op == ">" || n.op == "<" || n.op == ">=" ||
                         n.op == "<=" || n.op == "=")
                    w << " AND " << n.column << " " << n.op << " " << num(n.value);
            }
            for (const auto &term : c.camera_terms)
            {
                w << " AND exif_camera LIKE ?";
                camera_binds.push_back("%" + term + "%");
            }
            if (c.flash_on)
                w << " AND (CAST(exif_flash_fired AS INTEGER) & 1) = 1";
            if (c.flash_off)
                w << " AND (CAST(exif_flash_fired AS INTEGER) & 1) = 0";
            if (c.location)
            {
                w << " AND exif_gps_lat BETWEEN " << num(c.location->lat_min)
                  << " AND " << num(c.location->lat_max)
                  << " AND exif_gps_lon BETWEEN " << num(c.location->lon_min)
                  << " AND " << num(c.location->lon_max);
            }
            if (c.date_range)
                w << " AND exif_datetime >= '" << c.date_range->first
                  << "' AND exif_datetime <= '" << c.date_range->second << "'";
            if (c.month_any_year)
                w << " AND substr(exif_datetime, 6, 2) = '" << *c.month_any_year << "'";
            return w.str();
        }
    }

    TextSearchEngine::Result TextSearchEngine::exifOnlySearch(const ExifQueryConstraints &constraints)
    {
        Result result;
        if (!db_)
        {
            result.error = "database_unavailable";
            return result;
        }
        try
        {
            std::vector<std::string> camera_binds;
            std::string where = buildExifWhere(constraints, camera_binds);

            auto lease = db_->acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::vector<int> ids;
            int limit = exif_only_limit_;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id FROM scanned_files WHERE 1=1" + where +
                        " ORDER BY exif_datetime DESC LIMIT ?",
                Poco::Data::Keywords::into(ids);
            for (auto &bind : camera_binds)
                stmt, Poco::Data::Keywords::use(bind);
            stmt, Poco::Data::Keywords::use(limit);
            stmt.execute();

            result.matches.reserve(ids.size());
            for (int id : ids)
                result.matches.push_back({id, 1.0, /*via_semantic=*/false, /*via_exif=*/true});
            result.ok = true;
            Poco::Logger::get("TextSearchEngine")
                .debug("EXIF-only query matched %zu file(s)", result.matches.size());
        }
        catch (const std::exception &e)
        {
            result.error = "exif_query_failed: " + std::string(e.what());
        }
        return result;
    }
}
