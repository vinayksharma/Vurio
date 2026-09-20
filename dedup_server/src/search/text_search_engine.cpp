#include "search/text_search_engine.hpp"

#include "search/exif_keyword_config.hpp"

#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"
#include "database/places_ops.hpp"
#include "database/vector_index.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"

#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace MediaDedup
{
    namespace
    {
        // Rough count of bound facts for the log line.
        std::size_t bound_fact_count(const ExifQueryConstraints &c)
        {
            return c.numeric.size() + c.camera_terms.size() +
                   (c.flash_on ? 1 : 0) + (c.flash_off ? 1 : 0) +
                   (c.location ? 1 : 0) + (c.date_range ? 1 : 0) +
                   (c.month_any_year ? 1 : 0);
        }

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

        // QP-1 warmup: the first structurer call after boot pays model load
        // plus the full system-prompt prefill (seconds). Pay it here, off
        // the request path, so the user's first search finds a warm KV
        // prefix. LlmRuntime serializes internally — a search arriving
        // mid-warmup just queues behind it, never ahead of a cold cache.
        if (LlmRuntime::enabledInConfig(config_))
            llm_warmup_thread_ = std::thread([this] { warmupLlm(); });
    }

    TextSearchEngine::~TextSearchEngine()
    {
        if (llm_warmup_thread_.joinable())
            llm_warmup_thread_.join();
    }

    void TextSearchEngine::warmupLlm()
    {
        ensureParserLoaded();
        if (!llm_ || !structurer_)
            return;
        // Resumable prefill (LlmRuntime keeps successfully decoded KV across
        // a timeout) means each attempt advances the cache; three attempts
        // cover load + prefill + decode even on a busy machine.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (!llm_->available())
                return;
            auto r = structurer_->structure("warmup", *llm_);
            if (r.ok || (r.error != "timeout" && r.error != "stalled"))
            {
                Poco::Logger::get("TextSearchEngine")
                    .information("LLM structurer warmed up (%s)",
                                 r.ok ? std::string("ok") : r.error);
                return;
            }
        }
        Poco::Logger::get("TextSearchEngine")
            .warning("LLM structurer warmup still timing out; first searches may be slow");
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
            // QP-2 (FACE-4): person names resolve against the live persons
            // table at parse time — the location-gazetteer playbook, which
            // is what keeps the LLM prompt byte-stable as people get named.
            auto person_resolver = [this](const std::string &name) -> int {
                return db_ ? FacesOps::personIdByName(*db_, name) : 0;
            };
            // PLACE-2: place names resolve against the places the library
            // has photos in (file_places), live per query like persons.
            auto place_resolver = [this](const std::string &name) -> std::vector<int> {
                return db_ ? PlacesOps::placeIdsByName(*db_, name) : std::vector<int>{};
            };
            months_ = keywords.months;
            parser_ = std::make_unique<ExifQueryParser>(&geocoder_, keywords, person_resolver,
                                                        place_resolver);

            // QP-1d: the LLM structurer path shares the same vocabulary.
            structurer_ = std::make_unique<LlmQueryStructurer>(
                QueryFactSchema::fromKeywords(keywords));
            // PLACE-4: "at the waterfront" radius; "near" = landmark radius.
            LlmFactValidator::Options vopts;
            if (config_)
            {
                vopts.setting_at_m = config_->getPropertyValue<int>(
                    "search.places.settingAtM", static_cast<int>(vopts.setting_at_m));
                vopts.setting_near_m = config_->getPropertyValue<int>(
                    "search.places.landmarkRadiusM", static_cast<int>(vopts.setting_near_m));
            }
            llm_validator_ = std::make_unique<LlmFactValidator>(keywords, &geocoder_,
                                                                person_resolver, place_resolver,
                                                                vopts);
            parser_->setSettingRadii(vopts.setting_at_m, vopts.setting_near_m);
            if (LlmRuntime::enabledInConfig(config_))
                llm_ = std::make_unique<LlmRuntime>(LlmRuntime::optionsFromConfig(config_));
        });
    }

    void TextSearchEngine::setLlmCompleteFnForTests(LlmQueryStructurer::CompleteFn fn)
    {
        test_complete_fn_ = std::move(fn);
    }

    bool TextSearchEngine::tryLlmStructure(const std::string &query, StructuredQuery &out)
    {
        if (!structurer_ || !llm_validator_)
            return false;

        LlmQueryStructurer::RawOutput raw;
        if (test_complete_fn_)
            raw = structurer_->structure(query, test_complete_fn_);
        else if (llm_ && llm_->available())
            raw = structurer_->structure(query, *llm_);
        else
            return false;

        Poco::Logger &logger = Poco::Logger::get("TextSearchEngine");
        if (!raw.ok)
        {
            logger.warning("LLM structurer failed (%s); using pattern parser", raw.error);
            return false;
        }

        auto v = llm_validator_->validate(query, raw);
        // The model claimed facts but none survived grounding/validation:
        // distrust the whole parse rather than searching with only its
        // (equally unverified) clip segments.
        if (!raw.facts.empty() && v.accepted == 0)
        {
            logger.warning("LLM structurer output fully rejected (%z fact(s) dropped); "
                           "using pattern parser",
                           v.dropped);
            return false;
        }
        if (v.dropped > 0)
            logger.information("LLM structurer: %z fact(s) accepted, %z dropped",
                               v.accepted, v.dropped);
        out = std::move(v.query);
        return true;
    }

    TextSearchEngine::Result TextSearchEngine::search(const std::string &query, Routes routes)
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
                if (it->query == key && it->routes == routes)
                {
                    cache_.splice(cache_.begin(), cache_, it); // MRU refresh
                    result.ok = true;
                    result.matches = cache_.front().matches;
                    result.used_semantic = cache_.front().used_semantic;
                    result.used_exif = cache_.front().used_exif;
                    result.used_face = cache_.front().used_face;
                    return result;
                }
            }
        }

        result = searchUncached(key, routes);
        // A degraded parse is never cached: the moment the LLM recovers,
        // re-running the same query gets the full-quality parse instead of
        // a stale simplified one.
        if (result.ok && !result.parse_degraded)
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.push_front({key, routes, result.matches, result.used_semantic,
                               result.used_exif, result.used_face});
            while (cache_.size() > kCacheCapacity)
                cache_.pop_back();
        }
        return result;
    }

    TextSearchEngine::Result TextSearchEngine::searchUncached(const std::string &query, Routes routes)
    {
        Poco::Logger &logger = Poco::Logger::get("TextSearchEngine");

        // The query is split by INTENT (EXIF-7 algebra) — bound facts AND
        // together into a hard filter, bare facts are union branches beside
        // the CLIP hits, clip_queries carry the semantic text. Per-match
        // via_* flags feed the VU ai/xf/fc badges. QP-1d: the LLM structurer
        // is the primary parser; the regex parser is the fallback ladder's
        // last rung (structurer off/unavailable/failed/fully rejected).
        ensureParserLoaded();
        // Neither parser has a clock, so "july of this year" would ground to
        // July of every year. Resolve the calendar-relative phrases into
        // explicit month/year text first and both rungs of the ladder below
        // see the same, already-datable query.
        const std::string dated_query = RelativeDateResolver::resolve(query, months_);
        StructuredQuery sq;
        // Degraded = the LLM path was configured (runtime or test seam)
        // but the regex fallback answered. "Not installed/disabled" is the
        // chosen baseline, not degradation.
        bool parse_degraded = false;
        if (!tryLlmStructure(dated_query, sq))
        {
            sq = parser_->parseStructured(dated_query);
            parse_degraded = static_cast<bool>(test_complete_fn_) || llm_ != nullptr;
        }
        // Route toggles (VU ai/xf/fc) strip whole dimension families from
        // the SAME parse before the algebra runs — never a post-filter of
        // the hybrid result (a bound hybrid result is an intersection whose
        // every match carries all its flags, so a post-filter was vacuous).
        // exif off removes camera/iso/date/location/... dims; face off
        // removes person/face_count dims; ai off skips CLIP entirely below.
        if (!routes.exif || !routes.face)
        {
            auto strip = [&routes](ExifQueryConstraints &c) {
                auto drop_numeric = [&c](bool face_dims) {
                    c.numeric.erase(std::remove_if(c.numeric.begin(), c.numeric.end(),
                                                   [face_dims](const ExifQueryConstraints::Numeric &n) {
                                                       return (n.column == "face_count") == face_dims;
                                                   }),
                                    c.numeric.end());
                };
                if (!routes.face)
                {
                    c.person_ids.clear();
                    drop_numeric(/*face_dims=*/true);
                }
                if (!routes.exif)
                {
                    drop_numeric(/*face_dims=*/false);
                    c.camera_terms.clear();
                    c.file_extensions.clear();
                    c.flash_on = c.flash_off = false;
                    c.location.reset();
                    c.location_name.clear();
                    c.place_ids.clear();
                    c.location_inferred = false;
                    c.settings.clear();
                    c.date_range.reset();
                    c.month_any_year.reset();
                }
            };
            strip(sq.bound);
            for (auto &b : sq.bare)
                strip(b);
            sq.bare.erase(std::remove_if(sq.bare.begin(), sq.bare.end(),
                                         [](const ExifQueryConstraints &c) { return !c.hasAny(); }),
                          sq.bare.end());
        }
        Result out = runAlgebra(query, sq, routes, parse_degraded);
        // PLACE-2 §6.1: an INFERRED place (bare name, no preposition) is
        // the model's judgment, not the user's grammar. When honoring it
        // yields nothing, retry once without it rather than answer empty —
        // an explicit "in <place>" never falls back.
        if (out.ok && out.matches.empty() && sq.bound.hasInferred())
        {
            logger.information("Inferred place/setting facts matched nothing for '%s'; "
                               "retrying without them", query);
            sq.bound.clearInferred();
            out = runAlgebra(query, sq, routes, parse_degraded);
        }
        return out;
    }

    TextSearchEngine::Result TextSearchEngine::runAlgebra(const std::string &query,
                                                          StructuredQuery sq, Routes routes,
                                                          bool parse_degraded)
    {
        Poco::Logger &logger = Poco::Logger::get("TextSearchEngine");
        const bool has_bound = sq.hasBound();
        const bool has_bare = sq.hasBare();
        const bool has_semantic = sq.hasSemantic();

        // ai off: the fact algebra alone answers (bound ∩, bare ∪) over
        // whatever dimensions survived the stripping above.
        if (!routes.ai)
        {
            Result r;
            if (!has_bound && !has_bare)
            {
                // No parseable facts: an honest empty result — the query has
                // no metadata component for this pipeline to act on.
                r.ok = true;
                r.parse_degraded = parse_degraded;
                return r;
            }
            if (has_bound && !has_bare)
                r = exifOnlySearch(sq.bound);
            else
            {
                // Bare facts union; a bound set (if any) intersects them.
                std::unordered_set<int> bound_ids;
                if (has_bound)
                {
                    Result base = exifOnlySearch(sq.bound);
                    if (!base.ok)
                    {
                        base.parse_degraded = parse_degraded;
                        return base;
                    }
                    for (const auto &m : base.matches)
                        bound_ids.insert(m.file_id);
                }
                r.ok = true;
                std::unordered_set<int> emitted_ids;
                for (const auto &b : sq.bare)
                {
                    Result br = exifOnlySearch(b);
                    if (!br.ok)
                        continue;
                    for (auto m : br.matches)
                        if ((!has_bound || bound_ids.count(m.file_id)) &&
                            emitted_ids.insert(m.file_id).second)
                        {
                            // A bound set (if any) also admitted this match.
                            m.via_exif = m.via_exif || (has_bound && sq.bound.hasExifDims());
                            m.via_face = m.via_face || (has_bound && sq.bound.hasFaceDims());
                            r.matches.push_back(m);
                        }
                }
            }
            const bool any_exif_dims = sq.bound.hasExifDims() ||
                                       std::any_of(sq.bare.begin(), sq.bare.end(),
                                                   [](const ExifQueryConstraints &c) { return c.hasExifDims(); });
            const bool any_face_dims = sq.bound.hasFaceDims() ||
                                       std::any_of(sq.bare.begin(), sq.bare.end(),
                                                   [](const ExifQueryConstraints &c) { return c.hasFaceDims(); });
            r.used_exif = r.ok && any_exif_dims;
            r.used_face = r.ok && any_face_dims;
            r.parse_degraded = parse_degraded;
            return r;
        }

        // Case 1: no facts at all — pure CLIP ranking (over the LLM's
        // semantic segments when it produced them, else the raw query).
        if (!has_bound && !has_bare)
        {
            Result r = clipSearchUnion(nullptr, has_semantic ? sq.clip_queries
                                                             : std::vector<std::string>{query});
            r.used_semantic = r.ok;
            r.parse_degraded = parse_degraded;
            return r;
        }

        logger.information(
            "Query '%s': %z bound constraint(s), %z bare fact(s), semantic text '%s'",
            query, bound_fact_count(sq.bound), sq.bare.size(),
            has_semantic ? sq.clip_queries.front() : std::string());

        // Case 4: fully bound, nothing semantic ("taken with fuji in 2024")
        // — a pure filter. The AI half is deliberately skipped.
        if (has_bound && !has_bare && !has_semantic)
        {
            Result r = exifOnlySearch(sq.bound);
            r.used_exif = r.ok && sq.bound.hasExifDims();
            r.used_face = r.ok && sq.bound.hasFaceDims();
            r.parse_degraded = parse_degraded;
            return r;
        }

        // Case 3's bound set is computed FIRST so the semantic ranking can
        // be scoped to it: KNN over the filter's own files, not the global
        // top-K intersected afterwards (which starves large filters and
        // systematically favors whichever twin of a duplicate pair edges
        // into the global ranking).
        Result base;
        std::vector<int> base_ids_vec;
        if (has_bound)
        {
            base = exifOnlySearch(sq.bound);
            if (!base.ok)
            {
                base.parse_degraded = parse_degraded;
                return base; // filter intent cannot be honored without it
            }
            base_ids_vec.reserve(base.matches.size());
            for (const auto &m : base.matches)
                base_ids_vec.push_back(m.file_id);
        }

        // Cases 2 and 3: CLIP runs on the semantic text; a fact-only
        // exploration (case 2 with only filler left, e.g. "f2.5") gets the
        // raw query so the semantic half still participates.
        Result clip;
        bool clip_ran = false;
        if (has_semantic || !has_bound)
        {
            clip = clipSearchUnion(has_bound ? &base_ids_vec : nullptr,
                                   has_semantic ? sq.clip_queries
                                                : std::vector<std::string>{query});
            clip_ran = true;
            if (!clip.ok)
                logger.warning("Semantic half unavailable (%s); answering from EXIF alone",
                               clip.error);
        }
        const bool clip_ok = clip_ran && clip.ok;

        // Bare facts: each is an independent union branch.
        std::vector<Result> branches;
        branches.reserve(sq.bare.size());
        bool any_bare_ok = false;
        for (const auto &b : sq.bare)
        {
            branches.push_back(exifOnlySearch(b));
            if (branches.back().ok)
                any_bare_ok = true;
            else
                logger.error("Bare-fact branch failed: %s", branches.back().error);
        }

        Result out;
        std::unordered_set<int> emitted;

        if (has_bound)
        {
            // Case 3: everything intersects the bound filter. CLIP-ranked
            // hits inside it come first (already scoped to the filter),
            // then bare-fact hits it admits.
            std::unordered_set<int> base_ids;
            base_ids.reserve(base.matches.size());
            for (const auto &m : base.matches)
                base_ids.insert(m.file_id);

            out.ok = true;
            out.used_exif = sq.bound.hasExifDims();
            out.used_face = sq.bound.hasFaceDims();
            out.used_semantic = clip_ok;
            for (const auto &br : branches)
            {
                if (!br.ok)
                    continue;
                out.used_exif = out.used_exif ||
                                std::any_of(br.matches.begin(), br.matches.end(),
                                            [](const Match &m) { return m.via_exif; });
                out.used_face = out.used_face ||
                                std::any_of(br.matches.begin(), br.matches.end(),
                                            [](const Match &m) { return m.via_face; });
            }
            if (clip_ok)
            {
                for (auto m : clip.matches)
                {
                    if (base_ids.count(m.file_id) && emitted.insert(m.file_id).second)
                    {
                        // It satisfies the bound filter's dimensions.
                        m.via_exif = sq.bound.hasExifDims();
                        m.via_face = sq.bound.hasFaceDims();
                        out.matches.push_back(m);
                    }
                }
            }
            for (const auto &br : branches)
            {
                if (!br.ok)
                    continue;
                for (auto m : br.matches)
                    if (base_ids.count(m.file_id) && emitted.insert(m.file_id).second)
                    {
                        m.via_exif = m.via_exif || sq.bound.hasExifDims();
                        m.via_face = m.via_face || sq.bound.hasFaceDims();
                        out.matches.push_back(m);
                    }
            }
            // Semantic half down and no bare refinement: degrade to the
            // bound set itself rather than an artificial empty intersection.
            if (clip_ran && !clip_ok && !has_bare)
                out.matches = std::move(base.matches);
        }
        else
        {
            // Case 2: exploration union of the CLIP hits and every
            // bare-fact branch, deduplicated, CLIP-ranked first.
            if (!clip_ok && !any_bare_ok)
                return branches.empty() ? clip : branches.front();

            out.ok = true;
            out.used_semantic = clip_ok;

            std::unordered_set<int> bare_exif_ids, bare_face_ids;
            for (std::size_t i = 0; i < branches.size(); ++i)
            {
                if (!branches[i].ok)
                    continue;
                const bool b_exif = sq.bare[i].hasExifDims();
                const bool b_face = sq.bare[i].hasFaceDims();
                out.used_exif = out.used_exif || b_exif;
                out.used_face = out.used_face || b_face;
                for (const auto &m : branches[i].matches)
                {
                    if (b_exif)
                        bare_exif_ids.insert(m.file_id);
                    if (b_face)
                        bare_face_ids.insert(m.file_id);
                }
            }

            if (clip_ok)
            {
                for (auto m : clip.matches)
                {
                    if (emitted.insert(m.file_id).second)
                    {
                        m.via_exif = bare_exif_ids.count(m.file_id) > 0;
                        m.via_face = bare_face_ids.count(m.file_id) > 0;
                        out.matches.push_back(m);
                    }
                }
            }
            for (const auto &br : branches)
            {
                if (!br.ok)
                    continue;
                for (const auto &m : br.matches)
                    if (emitted.insert(m.file_id).second)
                        out.matches.push_back(m);
            }
        }

        out.parse_degraded = parse_degraded;
        // Optional safety valve (search.exifOnlyLimit > 0). Uncapped by
        // default: matched ids travel to the day view as one bound JSON
        // parameter, so a full-library match is a few MB of memory and
        // nothing more. Never trim silently — a capped result reads as
        // "that's everything" in the UI.
        if (exif_only_limit_ > 0 &&
            out.matches.size() > static_cast<std::size_t>(exif_only_limit_))
        {
            logger.information(
                "Result cap: trimmed %z of %z matches (search.exifOnlyLimit=%d)",
                out.matches.size() - static_cast<std::size_t>(exif_only_limit_),
                out.matches.size(), exif_only_limit_);
            out.matches.resize(static_cast<std::size_t>(exif_only_limit_));
        }
        return out;
    }

    TextSearchEngine::Result TextSearchEngine::clipSearchUnion(
        const std::vector<int> *scope_ids,
        const std::vector<std::string> &queries)
    {
        // Multi-embedding union (QP-1 §6): each segment is embedded
        // separately and a file keeps its best score across segments —
        // "raw, sunset, fuji" finds Mt-Fuji-looking photos via CLIP("fuji")
        // even though "fuji" is also a camera fact.
        Result merged;
        std::unordered_map<int, std::size_t> index_of;
        std::size_t ran = 0;
        for (const auto &q : queries)
        {
            if (ran++ >= 4)
                break;
            Result r = clipSearch(q, scope_ids);
            if (!r.ok)
            {
                if (!merged.ok && merged.error.empty())
                    merged.error = r.error;
                continue;
            }
            merged.ok = true;
            for (const auto &m : r.matches)
            {
                auto it = index_of.find(m.file_id);
                if (it == index_of.end())
                {
                    index_of.emplace(m.file_id, merged.matches.size());
                    merged.matches.push_back(m);
                }
                else if (m.score > merged.matches[it->second].score)
                {
                    merged.matches[it->second].score = m.score;
                }
            }
        }
        if (merged.ok)
        {
            merged.error.clear();
            std::sort(merged.matches.begin(), merged.matches.end(),
                      [](const Match &a, const Match &b) { return a.score > b.score; });
        }
        return merged;
    }

    TextSearchEngine::Result TextSearchEngine::clipSearch(const std::string &query,
                                                          const std::vector<int> *scope_ids)
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

        // Scoped mode ranks the WHOLE bound set (up to sqlite-vec's k
        // limit), not the global top-K: with a filter of 26k files, the
        // global top-300 keeps only the accidental overlap and starves
        // the filter of its own best matches.
        const int k = scope_ids
                          ? static_cast<int>(std::min<std::size_t>(
                                scope_ids->size(), VectorIndex::kMaxKnnK))
                          : knn_k_;
        auto knn = index->queryKnn(embedding.data(),
                                   embedding.size() * sizeof(float), k, scope_ids);
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
                                             "exif_exposure_comp",
                                             // QP-2 (FACE-4): the sweep-maintained
                                             // people count (NULL = unscanned).
                                             "face_count"};
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
            // Within-field OR (EXIF-7 §3): a photo has one camera, so
            // multiple values of the field alternate; fields AND together.
            if (!c.person_ids.empty())
            {
                // QP-2: a photo matches when ANY named person's face is in it
                // (OR within the field, like camera_terms). Ids are internal
                // integers — safe to inline.
                w << " AND EXISTS (SELECT 1 FROM faces fp WHERE fp.file_id = scanned_files.id"
                     " AND fp.person_id IN (";
                for (std::size_t i = 0; i < c.person_ids.size(); ++i)
                    w << (i ? "," : "") << c.person_ids[i];
                w << "))";
            }
                        if (!c.camera_terms.empty())
            {
                w << " AND (";
                for (std::size_t i = 0; i < c.camera_terms.size(); ++i)
                {
                    w << (i ? " OR " : "") << "exif_camera LIKE ?";
                    camera_binds.push_back("%" + c.camera_terms[i] + "%");
                }
                w << ")";
            }
            if (!c.file_extensions.empty())
            {
                w << " AND (";
                for (std::size_t i = 0; i < c.file_extensions.size(); ++i)
                {
                    w << (i ? " OR " : "") << "lower(file_name) LIKE ?";
                    camera_binds.push_back("%." + c.file_extensions[i]);
                }
                w << ")";
            }
            if (c.flash_on)
                w << " AND (CAST(exif_flash_fired AS INTEGER) & 1) = 1";
            if (c.flash_off)
                w << " AND (CAST(exif_flash_fired AS INTEGER) & 1) = 0";
            if (c.location || !c.place_ids.empty())
            {
                // A place is EITHER inside the gazetteer's bbox OR one the
                // file was resolved to by name (PLACE-2): "in wyoming" is a
                // box, "in bozeman" is file_places, "in yellowstone" both.
                w << " AND (";
                if (c.location)
                {
                    w << "(exif_gps_lat BETWEEN " << num(c.location->lat_min)
                      << " AND " << num(c.location->lat_max)
                      << " AND exif_gps_lon BETWEEN " << num(c.location->lon_min)
                      << " AND " << num(c.location->lon_max) << ")";
                }
                if (!c.place_ids.empty())
                {
                    if (c.location)
                        w << " OR ";
                    // Internal integer ids — safe to inline.
                    w << "scanned_files.id IN (SELECT file_id FROM file_places WHERE place_id IN (";
                    for (std::size_t i = 0; i < c.place_ids.size(); ++i)
                        w << (i ? "," : "") << c.place_ids[i];
                    w << "))";
                }
                w << ")";
            }
            // PLACE-4: setting facts AND together; each is "a landmark of
            // this kind within X m" OR (shoreline kinds) "the shoreline
            // within X m". Codes come from the keyword JSON -> bound.
            for (const auto &st : c.settings)
            {
                if (st.fcodes.empty())
                    continue;
                w << " AND (scanned_files.id IN (SELECT fp.file_id FROM file_places fp "
                     "JOIN places p ON p.id = fp.place_id WHERE fp.distance_m <= " << num(st.max_m)
                  << " AND p.fcode IN (";
                for (std::size_t i = 0; i < st.fcodes.size(); ++i)
                {
                    w << (i ? "," : "") << "?";
                    camera_binds.push_back(st.fcodes[i]);
                }
                w << "))";
                if (st.shoreline)
                    w << " OR shore_distance_m <= " << num(st.max_m);
                w << ")";
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
            // SQLite: LIMIT -1 means unlimited (the default).
            int limit = exif_only_limit_ > 0 ? exif_only_limit_ : -1;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id FROM scanned_files WHERE 1=1" + where +
                        " ORDER BY exif_datetime DESC LIMIT ?",
                Poco::Data::Keywords::into(ids);
            for (auto &bind : camera_binds)
                stmt, Poco::Data::Keywords::use(bind);
            stmt, Poco::Data::Keywords::use(limit);
            stmt.execute();

            result.matches.reserve(ids.size());
            const bool via_exif = constraints.hasExifDims();
            const bool via_face = constraints.hasFaceDims();
            for (int id : ids)
                result.matches.push_back({id, 1.0, /*via_semantic=*/false, via_exif, via_face});
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
