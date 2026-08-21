#pragma once

#include <string>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief EXIF-6: the vocabulary the EXIF query parser matches against,
     * externalized so it can grow without a rebuild.
     *
     * The GRAMMAR (numeric shapes like "iso above N", "f/2.8", "1/500",
     * years) lives in ExifQueryParser's regex templates; every WORD LIST
     * those templates alternate over comes from here. Adding a camera brand,
     * a comparison synonym, or a flash phrase is a JSON edit to
     * `config/exif_search_keywords.json`, not a code change.
     *
     * defaults() returns the built-in vocabulary (identical to the shipped
     * JSON). loadFromFile() overlays a JSON file on top: lists PRESENT in
     * the file replace the defaults wholesale, absent lists keep them — so
     * a user file may state only what it changes.
     *
     * All entries must be lowercase; the parser lowercases queries before
     * matching. Multi-word phrases are fine ("greater than", "no flash") —
     * internal spaces match any whitespace run.
     */
    struct ExifKeywordConfig
    {
        // --- Camera ("taken with a canon camera") ----------------------
        std::vector<std::string> camera_brands;              // canon, nikon, ...
        std::vector<std::string> camera_context_verbs;       // taken, shot, ...
        std::vector<std::string> camera_context_prepositions;// with, on, using, by
        std::vector<std::string> camera_articles;            // a, an, my
        std::vector<std::string> camera_suffixes;            // camera

        // --- Numeric comparison synonyms -------------------------------
        std::vector<std::string> greater_words;              // above, over, ...  -> >
        std::vector<std::string> greater_equal_words;        // at least          -> >=
        std::vector<std::string> less_words;                 // below, under, ... -> <
        std::vector<std::string> less_equal_words;           // at most           -> <=
        std::vector<std::string> aperture_wider_words;       // wider, faster, brighter -> f_number <
        std::vector<std::string> aperture_narrower_words;    // narrower, smaller       -> f_number >
        std::vector<std::string> shutter_faster_words;       // faster, quicker -> exposure_time <
        std::vector<std::string> shutter_slower_words;       // slower, longer  -> exposure_time >
        std::vector<std::string> focal_longer_words;         // above, longer than -> focal >
        std::vector<std::string> focal_shorter_words;        // below, shorter than -> focal <

        // --- Flash phrases ----------------------------------------------
        std::vector<std::string> flash_on_phrases;           // "with flash", ...
        std::vector<std::string> flash_off_phrases;          // "no flash", ...

        // --- Dates ------------------------------------------------------
        std::vector<std::string> months;                     // exactly 12, january..december order
        std::vector<std::string> ambiguous_months;           // "may" — skipped for bare-month matching
        std::vector<std::string> month_prepositions;         // in, from, during

        // --- Location ---------------------------------------------------
        std::vector<std::string> location_prepositions;      // in, at, near, around, from

        // --- Remaining-text handling ------------------------------------
        std::vector<std::string> imperative_words;           // stripped from CLIP text
        std::vector<std::string> filler_words;               // don't count as semantic content

        /// Built-in vocabulary; behavior without any JSON file.
        static ExifKeywordConfig defaults();

        /// Overlay a JSON keyword file onto this config. Lists present in
        /// the file replace the current ones; absent lists are untouched.
        /// Returns false (config unchanged) on missing/unparseable file.
        bool loadFromFile(const std::string &json_path);
    };
}
