#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "media_processors/text/clip_tokenizer.hpp"
#include "search/exif_query_parser.hpp"
#include "search/llm_fact_validator.hpp"
#include "search/llm_query_structurer.hpp"
#include "search/llm_runtime.hpp"
#include "search/location_geocoder.hpp"

namespace MediaDedup
{
    class UnifiedObservableConfigManager;
    class DatabaseManager;

    /**
     * @brief Text -> image semantic search over the vec0 embedding index.
     *
     * Embeds a natural-language query with the CLIP text tower (same
     * checkpoint as the image embeddings written by QualityPipeline) and runs
     * a cosine KNN against vec_embedding_index. Results are scanned_files ids
     * with scores in [0,1]; text-to-image CLIP scores live far below the
     * image-to-image duplicate thresholds, so `search.minScore` (default 0.2)
     * only trims obvious noise — callers should treat the list as ranked.
     *
     * Query embedding runs on the calling (gRPC handler) thread — a single
     * ~ms-scale inference — deliberately NOT on the media_processor pool, so
     * a scan backlog cannot stall searches. A small MRU cache keyed by the
     * trimmed query keeps repeated per-page calls from re-running inference.
     *
     * Thread safety: search() is safe to call concurrently.
     */
    class TextSearchEngine
    {
    public:
        struct Match
        {
            int file_id = 0;
            double score = 0.0; // 1 - cosine distance, higher = more relevant
            // Which half of the hybrid search admitted this file: CLIP ranked
            // it for the semantic text / it satisfied the EXIF constraints.
            // Results are a union of both halves, so any combination occurs.
            bool via_semantic = false;
            bool via_exif = false;
        };

        struct Result
        {
            bool ok = false;
            std::string error;          // set when !ok
            std::vector<Match> matches; // CLIP-ranked first, then EXIF extras
            // Which halves of the hybrid pipeline RAN (successfully) for this
            // query. Per-match attribution lives on Match::via_*.
            bool used_semantic = false;
            bool used_exif = false;
            // The LLM structurer was expected to parse this query but the
            // regex fallback answered instead — fact semantics may be
            // simplified (camera model names, free-form phrasing). Surfaced
            // to the client so degradation is visible, never silent.
            bool parse_degraded = false;
        };

        ~TextSearchEngine();
        TextSearchEngine(std::shared_ptr<UnifiedObservableConfigManager> config,
                         DatabaseManager *db);

        /// Ranked matches for a query. Never throws; failures land in Result.
        Result search(const std::string &query);

        /// QP-1d test seam: force the LLM structurer path with an injected
        /// completion function (no model file involved). Call before the
        /// first search.
        void setLlmCompleteFnForTests(LlmQueryStructurer::CompleteFn fn);

    private:
        Result searchUncached(const std::string &query);
        /// QP-1d: LLM structurer -> validator -> StructuredQuery. Returns
        /// false (regex fallback) on: structurer unavailable, completion
        /// error/timeout, or every claimed fact rejected by the validator.
        bool tryLlmStructure(const std::string &query, StructuredQuery &out);
        /// CLIP KNN over the vec0 index (the pre-EXIF-4 search body).
        Result clipSearch(const std::string &query,
                          const std::vector<int> *scope_ids = nullptr);
        /// Union of clipSearch over up to 4 segments: max score per file,
        /// re-ranked. ok when ANY segment succeeds.
        Result clipSearchUnion(const std::vector<int> *scope_ids,
                               const std::vector<std::string> &queries);
        /// EXIF-4: select ids matching the parsed constraints from SQL alone.
        Result exifOnlySearch(const ExifQueryConstraints &constraints);
        void ensureTokenizerLoaded();
        void ensureGeocoderLoaded();
        void ensureParserLoaded();
        void warmupLlm();

        std::shared_ptr<UnifiedObservableConfigManager> config_;
        DatabaseManager *db_;

        std::string text_model_path_;
        std::string vocab_path_;
        std::string merges_path_;
        int embedding_dim_ = 512;
        int knn_k_ = 300;
        double min_score_ = 0.2;
        // Optional cap for fact-match result sets; 0 (default) = uncapped.
        // The old 1000 default silently amputated whole eras from the day
        // view ("x-pro1" matches 26.5k files). Ids reach the day view as a
        // single bound JSON parameter, so scale is not a concern; set
        // search.exifOnlyLimit only to deliberately bound huge unions.
        int exif_only_limit_ = 0;
        std::string locations_path_;
        std::string keywords_path_; // EXIF-6: JSON keyword vocabulary

        std::once_flag tokenizer_once_;
        ClipTokenizer tokenizer_;

        std::once_flag geocoder_once_;
        LocationGeocoder geocoder_;

        // EXIF-6: parser compiles its regexes from the keyword file once;
        // reused across searches (parse() is const and thread-safe).
        std::once_flag parser_once_;
        std::unique_ptr<ExifQueryParser> parser_;

        // QP-1d: the LLM structurer path. structurer_/llm_validator_ are
        // always built (cheap); the runtime only when search.llm.enabled.
        // A missing model file degrades to the regex parser per query.
        std::unique_ptr<LlmRuntime> llm_;
        std::unique_ptr<LlmQueryStructurer> structurer_;
        std::unique_ptr<LlmFactValidator> llm_validator_;
        LlmQueryStructurer::CompleteFn test_complete_fn_; // test seam

        std::mutex cache_mutex_;
        std::thread llm_warmup_thread_; // QP-1 warmup; joined in the destructor
        // MRU-front query cache: repeated page requests for the same active
        // search reuse the KNN result instead of re-running inference. The
        // used_* flags ride along so cache hits report them identically.
        struct CacheEntry
        {
            std::string query;
            std::vector<Match> matches;
            bool used_semantic = false;
            bool used_exif = false;
        };
        std::list<CacheEntry> cache_;
        static constexpr std::size_t kCacheCapacity = 8;
    };
}
