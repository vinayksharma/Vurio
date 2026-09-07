#pragma once

#include <functional>

#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "search/exif_keyword_config.hpp"
#include "search/location_geocoder.hpp"

namespace MediaDedup
{
    /**
     * @brief EXIF-4: constraints parsed out of a natural-language query.
     *
     * "sunset photos taken with a canon iso above 1600 in yellowstone"
     * splits into structured filters (camera ~ canon, iso > 1600, GPS in
     * Yellowstone's bounds) plus the remaining semantic text ("sunset
     * photos") for the CLIP tower.
     */
    struct ExifQueryConstraints
    {
        struct Numeric
        {
            std::string column; // e.g. "exif_iso"
            std::string op;     // ">", "<", ">=", "<=", "=", "between"
            double value = 0.0;
            double value2 = 0.0; // upper bound when op == "between"
        };

        std::vector<Numeric> numeric;

        // Each term filters exif_camera LIKE '%term%' (make+model string).
        // Multiple terms OR together (within-field OR, EXIF-7 §3).
        std::vector<std::string> camera_terms;

        // QP-1d: file-type facts ("raw", "jpeg") resolved to extension
        // lists; matches lower(file_name) suffixes, OR within the list.
        std::vector<std::string> file_extensions;

        // EXIF Flash tag bit 0 = "flash fired".
        bool flash_on = false;
        bool flash_off = false;

        std::optional<LocationGeocoder::Bounds> location;
        std::string location_name; // for logging only

        // Inclusive range on exif_datetime ("YYYY:MM:DD HH:MM:SS" strings —
        // lexicographic comparison works in that format).
        std::optional<std::pair<std::string, std::string>> date_range;

        // Month with no year: match substr(exif_datetime, 6, 2) == "08".
        std::optional<std::string> month_any_year;

        // QP-2 (FACE-4): resolved person ids (persons.id); a file matches
        // when it has a face assigned to ANY of them (OR within the field,
        // like camera_terms). face_count rides `numeric` on the
        // scanned_files.face_count column.
        std::vector<int> person_ids;

        // What's left for semantic search after the EXIF tokens are removed.
        std::string remaining;

        // True when remaining is only filler ("photos", "all", "show me") —
        // the query is a pure metadata filter and CLIP should be skipped.
        bool remaining_is_filler = false;

        bool hasAny() const { return hasExifDims() || hasFaceDims(); }

        // The face route (person names, face counts) is attributed and
        // toggled separately from the EXIF route in the VU search UI, so a
        // constraint set knows which routes its dimensions belong to. A set
        // can span both ("photos of alice taken with a canon").
        bool hasFaceDims() const
        {
            if (!person_ids.empty())
                return true;
            for (const auto &n : numeric)
                if (n.column == "face_count")
                    return true;
            return false;
        }

        bool hasExifDims() const
        {
            for (const auto &n : numeric)
                if (n.column != "face_count")
                    return true;
            return !camera_terms.empty() || !file_extensions.empty() ||
                   flash_on || flash_off ||
                   location.has_value() || date_range.has_value() || month_any_year.has_value();
        }
    };

    /**
     * @brief QP-1a: a query split by INTENT (EXIF-7 result algebra).
     *
     * Bound facts were matched WITH grammatical scaffolding ("taken with
     * fuji", "iso above 800", "in yellowstone", any date) and express
     * filter intent — the engine ANDs them into one hard SQL constraint.
     * Bare facts are lone tokens ("fuji", "f2.5", "iso 800") and express
     * exploration — each becomes an independent union branch beside the
     * CLIP hits. This type is the parser-agnostic contract: the QP-1 LLM
     * structurer will emit it too, so the engine never knows who parsed.
     */
    struct StructuredQuery
    {
        // Filter-intent constraints, AND-combined into one WHERE clause.
        ExifQueryConstraints bound;
        // Explore-intent facts, ONE fact per entry — union branches.
        std::vector<ExifQueryConstraints> bare;
        // Semantic segments for CLIP; empty when only filler remained.
        std::vector<std::string> clip_queries;
        // Leftover text after fact extraction (imperatives stripped) and
        // whether it is filler-only — the pre-QP-1 parse() view of R.
        std::string remaining;
        bool remaining_is_filler = true;

        bool hasBound() const { return bound.hasAny(); }
        bool hasBare() const { return !bare.empty(); }
        bool hasSemantic() const { return !clip_queries.empty(); }
    };

    /**
     * @brief Parses EXIF constraints from free-text search queries.
     *
     * Recognized patterns (case-insensitive):
     * - Camera makes: "canon", "shot with a sony", "nikon camera", ...
     * - ISO: "iso 1600", "iso above/over 3200", "iso below/under 800",
     *   "iso between 400 and 1600"
     * - Aperture: "f/2.8" (approximate), "wider than f/2.8" (< 2.8),
     *   "narrower than f/5.6" (> 5.6)
     * - Shutter: "shutter 1/500" (approximate exposure time),
     *   "shutter faster than 1/500" (exposure_time < 1/500), "slower than"
     * - Focal length: "50mm" (approximate), "focal length above 200mm"
     * - Flash: "with flash" / "flash on"; "without flash" / "no flash"
     * - Dates: "2024" (year), "august 2024" (month), bare "august"
     *   (that month in any year)
     * - Locations, preposition-triggered: "in yellowstone", "at paris",
     *   "near utah", "from india" — resolved through the LocationGeocoder.
     *   Preposition-less place names are left to the CLIP text search
     *   (guessing turns city names like "Nice" or "Phoenix" into false GPS
     *   filters).
     *
     * EXIF-6: the pattern SHAPES above are fixed, but every word list they
     * alternate over (camera brands, comparison synonyms, flash phrases,
     * months, prepositions, filler words) comes from the ExifKeywordConfig
     * given at construction — extend `config/exif_search_keywords.json`
     * instead of editing this class. Regexes are compiled once in the
     * constructor, so construct the parser once and reuse it.
     *
     * The geocoder pointer may be null (location parsing is skipped).
     * parse() is const and thread-safe once the geocoder is loaded.
     */
    class ExifQueryParser
    {
    public:
        /// QP-2: name -> persons.id (0 = unknown), evaluated at parse time
        /// because person names are runtime user data.
        using PersonResolver = std::function<int(const std::string &)>;

        explicit ExifQueryParser(const LocationGeocoder *geocoder = nullptr,
                                 const ExifKeywordConfig &keywords = ExifKeywordConfig::defaults(),
                                 PersonResolver person_resolver = nullptr);

        ExifQueryConstraints parse(const std::string &query) const;

        /// QP-1a: intent-split parse. Binding rules: comparative/range
        /// numeric forms, scaffolded camera phrases, flash phrases,
        /// locations, and ALL dates are bound; exact-value probes
        /// ("iso 800", "f2.5", "50mm", "shutter 1/500") and bare brand
        /// tokens are bare. parse() is the flattened (binding-blind) view.
        StructuredQuery parseStructured(const std::string &query) const;

    private:
        int monthNumber(const std::string &name) const;

        /// SQL operator for a matched comparator word. `lower_words` /
        /// `higher_words` are the dimension's own direction words, which win
        /// over the generic comparators and may invert the sense.
        const char *comparatorOp(const std::string &word,
                                 const std::unordered_set<std::string> &lower_words,
                                 const std::unordered_set<std::string> &higher_words) const;

        const LocationGeocoder *geocoder_;
        PersonResolver person_resolver_;

        // Keyword-driven state, built once from the ExifKeywordConfig.
        std::vector<std::string> months_;
        std::unordered_set<std::string> greater_set_, greater_equal_set_;
        std::unordered_set<std::string> less_set_, less_equal_set_;
        std::unordered_set<std::string> aperture_wider_set_, aperture_narrower_set_;
        std::unordered_set<std::string> shutter_faster_set_, shutter_slower_set_;
        std::unordered_set<std::string> focal_longer_set_, focal_shorter_set_;
        std::unordered_set<std::string> imperative_set_, filler_set_;

        // Compiled patterns (keyword alternations baked in).
        std::regex iso_between_, iso_cmp_, iso_eq_;
        // Each comparative dimension has a LABELED form (the dimension is
        // spelled out, so a bare number is unambiguous) and an unlabeled one
        // (the value itself must carry f/mm/seconds to identify it).
        std::regex f_cmp_, f_cmp_labeled_, f_eq_;
        std::regex shutter_cmp_, shutter_cmp_labeled_, shutter_eq_;
        std::regex focal_cmp_, focal_cmp_labeled_, focal_eq_;
        std::regex flash_on_, flash_off_;
        std::regex camera_;
        std::regex month_year_, year_, month_;
        std::regex location_;
    };
}
