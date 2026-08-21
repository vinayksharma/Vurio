#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media_processors/text/clip_tokenizer.hpp"
#include "search/exif_query_parser.hpp"
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
        };

        struct Result
        {
            bool ok = false;
            std::string error;          // set when !ok
            std::vector<Match> matches; // ranked best-first
        };

        TextSearchEngine(std::shared_ptr<UnifiedObservableConfigManager> config,
                         DatabaseManager *db);

        /// Ranked matches for a query. Never throws; failures land in Result.
        Result search(const std::string &query);

    private:
        Result searchUncached(const std::string &query);
        /// CLIP KNN over the vec0 index (the pre-EXIF-4 search body).
        Result clipSearch(const std::string &query);
        /// EXIF-4: drop matches whose rows fail the parsed EXIF constraints.
        void filterMatchesByExif(std::vector<Match> &matches,
                                 const ExifQueryConstraints &constraints);
        /// EXIF-4: pure metadata query ("photos taken with a canon") — no
        /// semantic text left, so skip CLIP and select ids from SQL alone.
        Result exifOnlySearch(const ExifQueryConstraints &constraints);
        void ensureTokenizerLoaded();
        void ensureGeocoderLoaded();
        void ensureParserLoaded();

        std::shared_ptr<UnifiedObservableConfigManager> config_;
        DatabaseManager *db_;

        std::string text_model_path_;
        std::string vocab_path_;
        std::string merges_path_;
        int embedding_dim_ = 512;
        int knn_k_ = 300;
        double min_score_ = 0.2;
        // EXIF-4: cap for constraint-only queries (no KNN k to bound them).
        int exif_only_limit_ = 1000;
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

        std::mutex cache_mutex_;
        // MRU-front query cache: repeated page requests for the same active
        // search reuse the KNN result instead of re-running inference.
        std::list<std::pair<std::string, std::vector<Match>>> cache_;
        static constexpr std::size_t kCacheCapacity = 8;
    };
}
