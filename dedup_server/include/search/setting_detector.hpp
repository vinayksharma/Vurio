#pragma once

#include <string>
#include <vector>

#include "search/exif_keyword_config.hpp"

namespace MediaDedup
{
    /**
     * @brief PLACE-4: deterministic detection of setting words in a query.
     *
     * The LLM structurer is unreliable about the `setting` dimension — the
     * shipped 1.5B model files "waterfront" and "lake" as location VALUES
     * or leaves them in the semantic text. Setting categories are a closed
     * vocabulary (ExifKeywordConfig::settings), so code can find them
     * directly and the model only refines the at/near nuance when it does
     * emit a fact. Used by both the regex parser and the LLM validator.
     *
     * Rules, on the lowercase query:
     *  - a token that is a category (plural folded) is a setting;
     *  - a scaffold word within the two preceding tokens picks the op and
     *    marks it explicit: near/close/by/around/along/beside/in → "near",
     *    on/at → "at";
     *  - no scaffold → op "at", `inferred` (the engine retries without it
     *    when the intersection is empty);
     *  - tokens inside `covered_spans` (already-accepted fact text, e.g. a
     *    place name like "salt lake city") are skipped.
     */
    struct DetectedSetting
    {
        std::string category;
        std::string op;   // "at" | "near"
        std::string span; // the query text it came from
        bool inferred = false;
    };

    std::vector<DetectedSetting> detectSettings(const std::string &query_lower,
                                                const ExifKeywordConfig &keywords,
                                                const std::vector<std::string> &covered_spans = {});

    /// "the lake" -> "lake": leading articles removed (lowercase input).
    std::string stripLeadingArticles(const std::string &text_lower);

    /// The op a scaffold phrase implies ("near the lake" -> near, "on the
    /// beach" -> at); empty when the span has no scaffold word.
    std::string settingOpForSpan(const std::string &span_lower);
}
