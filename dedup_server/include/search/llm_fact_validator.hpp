#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "search/exif_keyword_config.hpp"
#include "search/exif_query_parser.hpp"
#include "search/llm_query_structurer.hpp"
#include "search/location_geocoder.hpp"

namespace MediaDedup
{
    /// PLACE-4 distance semantics for setting facts (namespace-level so the
    /// constructor's default argument can name it inside the class).
    struct LlmFactValidatorOptions
    {
        double setting_at_m = 150.0;    // "on the waterfront", "beach photos"
        double setting_near_m = 1500.0; // "near the lake"
    };

    /**
     * @brief QP-1d: the trust boundary between the LLM and SQL.
     *
     * Converts LlmQueryStructurer::RawOutput (the model's CLAIM) into a
     * StructuredQuery the engine may execute — but only the parts that
     * survive deterministic checks:
     *
     * - Span grounding (anti-hallucination core): each fact's span must
     *   occur in the query text (case/whitespace-normalized), and every
     *   value must occur inside that span. A fact whose value is not in
     *   the text does not exist. clip_queries are grounded the same way.
     * - Registry checks: known dimension, op valid for that dimension,
     *   values parse and are sane (ISO 25–1e6, aperture f/0.7–64, focal
     *   1–5000 mm, shutter 1/64000–3600 s, years 1900–2100), reversed
     *   ranges normalized.
     * - Resolution: file-type names map through the keyword JSON's
     *   file_types; unknown names drop the fact. Location names must
     *   resolve in the gazetteer; an unresolved place is NOT discarded —
     *   its text moves to clip_queries so the semantic content survives.
     *
     * Facts that fail any check are dropped (and counted); the caller
     * decides the fallback (all-claimed-facts-rejected => distrust the
     * whole parse and use the regex parser instead).
     */
    class LlmFactValidator
    {
    public:
        /// QP-2: name -> persons.id (0 = unknown). Live per query — person
        /// names are user data that changes at runtime, which is exactly why
        /// they are NOT prompt hints (the location-gazetteer playbook).
        using PersonResolver = std::function<int(const std::string &)>;
        /// PLACE-2: name -> ids of places with photos (see PlacesOps).
        using PlaceResolver = std::function<std::vector<int>(const std::string &)>;

        /// PLACE-4 distance semantics for setting facts.
        using Options = LlmFactValidatorOptions;

        LlmFactValidator(ExifKeywordConfig keywords, const LocationGeocoder *geocoder,
                         PersonResolver person_resolver = nullptr,
                         PlaceResolver place_resolver = nullptr,
                         Options options = Options());

        struct Result
        {
            StructuredQuery query;
            std::size_t accepted = 0;
            std::size_t dropped = 0;
        };

        Result validate(const std::string &raw_query,
                        const LlmQueryStructurer::RawOutput &raw) const;

    private:
        ExifKeywordConfig keywords_;
        const LocationGeocoder *geocoder_;
        PersonResolver person_resolver_;
        PlaceResolver place_resolver_;
        Options options_;
    };
}
