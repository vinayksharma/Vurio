#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "search/exif_keyword_config.hpp"
#include "search/llm_fact_validator.hpp"

using namespace MediaDedup;

// QP-1d: the trust boundary. Every test is hermetic — RawOutput is built by
// hand exactly as a (possibly lying) model would produce it.

namespace
{
    using RawFact = LlmQueryStructurer::RawFact;
    using RawOutput = LlmQueryStructurer::RawOutput;

    RawOutput output(std::vector<RawFact> facts,
                     std::vector<std::string> clip = {})
    {
        RawOutput o;
        o.ok = true;
        o.facts = std::move(facts);
        o.clip_queries = std::move(clip);
        return o;
    }

    LlmFactValidator makeValidator()
    {
        return LlmFactValidator(ExifKeywordConfig::defaults(), nullptr);
    }
}

TEST(LlmFactValidatorTest, GroundedBoundCameraFactIsAccepted)
{
    auto v = makeValidator();
    auto r = v.validate("sunset taken with a canon",
                        output({{"camera", "in", {"canon"}, "filter",
                                 "taken with a canon"}},
                               {"sunset"}));
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.dropped, 0u);
    EXPECT_EQ(r.query.bound.camera_terms, std::vector<std::string>{"canon"});
    EXPECT_TRUE(r.query.bare.empty());
    EXPECT_EQ(r.query.clip_queries, std::vector<std::string>{"sunset"});
    EXPECT_FALSE(r.query.remaining_is_filler);
}

TEST(LlmFactValidatorTest, ExploreFactBecomesBareBranch)
{
    auto v = makeValidator();
    auto r = v.validate("raw, fuji",
                        output({{"file_type", "in", {"raw"}, "explore", "raw"},
                                {"camera", "in", {"fuji"}, "explore", "fuji"}}));
    EXPECT_EQ(r.accepted, 2u);
    EXPECT_FALSE(r.query.bound.hasAny());
    ASSERT_EQ(r.query.bare.size(), 2u);
    EXPECT_FALSE(r.query.bare[0].file_extensions.empty());
    EXPECT_EQ(r.query.bare[1].camera_terms, std::vector<std::string>{"fuji"});
}

TEST(LlmFactValidatorTest, HallucinatedSpanIsDropped)
{
    auto v = makeValidator();
    auto r = v.validate("sunset photos",
                        output({{"camera", "in", {"canon"}, "filter",
                                 "taken with a canon"}}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 1u);
    EXPECT_FALSE(r.query.bound.hasAny());
}

TEST(LlmFactValidatorTest, ValueNotInSpanIsDropped)
{
    auto v = makeValidator();
    // Span exists in the query, but the value "nikon" is not inside it.
    auto r = v.validate("taken with a canon",
                        output({{"camera", "in", {"nikon"}, "filter",
                                 "taken with a canon"}}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 1u);
}

TEST(LlmFactValidatorTest, GroundingIsCaseAndWhitespaceInsensitive)
{
    auto v = makeValidator();
    auto r = v.validate("Flowers,  taken   with a CANON",
                        output({{"camera", "in", {"canon"}, "filter",
                                 "taken with a canon"}},
                               {"Flowers"}));
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.query.clip_queries, std::vector<std::string>{"flowers"});
}

TEST(LlmFactValidatorTest, NumericDimensionsConvertAndClamp)
{
    auto v = makeValidator();
    auto r = v.validate(
        "iso above 800 f2.5 to f12 faster than 1/500 50mm",
        output({{"iso", "gt", {"800"}, "filter", "iso above 800"},
                {"aperture", "range", {"12", "2.5"}, "filter", "f2.5 to f12"},
                {"shutter", "faster", {"1/500"}, "filter", "faster than 1/500"},
                {"focal", "eq", {"50"}, "explore", "50mm"}}));
    EXPECT_EQ(r.accepted, 4u);
    ASSERT_EQ(r.query.bound.numeric.size(), 3u);

    EXPECT_EQ(r.query.bound.numeric[0].column, "exif_iso");
    EXPECT_EQ(r.query.bound.numeric[0].op, ">");
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[0].value, 800.0);

    // Reversed range normalized.
    EXPECT_EQ(r.query.bound.numeric[1].column, "exif_f_number");
    EXPECT_EQ(r.query.bound.numeric[1].op, "between");
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[1].value, 2.5);
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[1].value2, 12.0);

    // Faster shutter = shorter exposure time.
    EXPECT_EQ(r.query.bound.numeric[2].column, "exif_exposure_time");
    EXPECT_EQ(r.query.bound.numeric[2].op, "<");
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[2].value, 1.0 / 500.0);

    // Explore focal probe: its own bare branch, banded like the regex path.
    ASSERT_EQ(r.query.bare.size(), 1u);
    EXPECT_EQ(r.query.bare[0].numeric[0].op, "between");
    EXPECT_DOUBLE_EQ(r.query.bare[0].numeric[0].value, 49.0);
    EXPECT_DOUBLE_EQ(r.query.bare[0].numeric[0].value2, 51.0);
}

TEST(LlmFactValidatorTest, InsaneValuesAreDropped)
{
    auto v = makeValidator();
    auto r = v.validate("iso 5 f/500 photos from 1492",
                        output({{"iso", "eq", {"5"}, "explore", "iso 5"},
                                {"aperture", "eq", {"500"}, "explore", "f/500"},
                                {"date", "eq", {"1492"}, "filter", "1492"}}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 3u);
}

TEST(LlmFactValidatorTest, UnknownDimAndWrongOpAreDropped)
{
    auto v = makeValidator();
    auto r = v.validate("weird 42 iso above 800",
                        output({{"vehicle", "in", {"42"}, "filter", "weird 42"},
                                {"iso", "faster", {"800"}, "filter", "iso above 800"}}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 2u);
}

TEST(LlmFactValidatorTest, DateVariantsConvert)
{
    auto v = makeValidator();
    auto r = v.validate(
        "between 2005 and 2015 in august 2024 photos from august",
        output({{"date", "range", {"2015", "2005"}, "filter",
                 "between 2005 and 2015"}}));
    ASSERT_EQ(r.accepted, 1u);
    ASSERT_TRUE(r.query.bound.date_range.has_value());
    EXPECT_EQ(r.query.bound.date_range->first, "2005:01:01 00:00:00");
    EXPECT_EQ(r.query.bound.date_range->second, "2015:12:31 23:59:59");

    auto r2 = v.validate("in august 2024",
                         output({{"date", "eq", {"august 2024"}, "filter",
                                  "in august 2024"}}));
    ASSERT_EQ(r2.accepted, 1u);
    EXPECT_EQ(r2.query.bound.date_range->first, "2024:08:01 00:00:00");

    auto r3 = v.validate("august snow",
                         output({{"date", "eq", {"august"}, "filter", "august"}},
                                {"snow"}));
    ASSERT_EQ(r3.accepted, 1u);
    ASSERT_TRUE(r3.query.bound.month_any_year.has_value());
    EXPECT_EQ(*r3.query.bound.month_any_year, "08");
}

TEST(LlmFactValidatorTest, FileTypeResolvesThroughKeywordConfig)
{
    auto v = makeValidator();
    auto r = v.validate("raw files",
                        output({{"file_type", "in", {"raw"}, "filter", "raw files"}}));
    ASSERT_EQ(r.accepted, 1u);
    const auto &exts = r.query.bound.file_extensions;
    EXPECT_NE(std::find(exts.begin(), exts.end(), "nef"), exts.end());
    EXPECT_NE(std::find(exts.begin(), exts.end(), "dng"), exts.end());

    auto r2 = v.validate("floppy files",
                         output({{"file_type", "in", {"floppy"}, "filter",
                                  "floppy files"}}));
    EXPECT_EQ(r2.accepted, 0u);
    EXPECT_EQ(r2.dropped, 1u);
}

TEST(LlmFactValidatorTest, LocationResolvesOrDemotesToClip)
{
    LocationGeocoder geocoder;
    auto path = (std::filesystem::temp_directory_path() / "validator_geo.geojson").string();
    std::ofstream(path) << R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"name": "Utah"},
         "bbox": [-114.03, 36.99, -109.04, 42.00], "geometry": null}]})";
    ASSERT_TRUE(geocoder.loadFromFile(path));
    LlmFactValidator v(ExifKeywordConfig::defaults(), &geocoder);

    auto r = v.validate("hiking in utah",
                        output({{"location", "eq", {"utah"}, "filter", "in utah"}},
                               {"hiking"}));
    ASSERT_EQ(r.accepted, 1u);
    ASSERT_TRUE(r.query.bound.location.has_value());
    EXPECT_EQ(r.query.bound.location_name, "utah");

    // Unknown place: fact dropped, but the text survives as a CLIP query.
    auto r2 = v.validate("hiking in narnia",
                         output({{"location", "eq", {"narnia"}, "filter",
                                  "in narnia"}},
                                {"hiking"}));
    EXPECT_EQ(r2.accepted, 0u);
    EXPECT_EQ(r2.dropped, 1u);
    ASSERT_EQ(r2.query.clip_queries.size(), 2u);
    EXPECT_EQ(r2.query.clip_queries[0], "narnia");
    EXPECT_EQ(r2.query.clip_queries[1], "hiking");
    std::remove(path.c_str());
}

TEST(LlmFactValidatorTest, FlashConverts)
{
    auto v = makeValidator();
    auto r = v.validate("with flash", output({{"flash", "on", {}, "filter",
                                               "with flash"}}));
    ASSERT_EQ(r.accepted, 1u);
    EXPECT_TRUE(r.query.bound.flash_on);
    EXPECT_FALSE(r.query.bound.flash_off);
}

TEST(LlmFactValidatorTest, UngroundedClipQueriesAreDroppedAndCapped)
{
    auto v = makeValidator();
    auto r = v.validate("a b c d e f",
                        output({}, {"a", "zeppelin", "b", "c", "d", "e"}));
    // "zeppelin" is not in the query; the rest cap at 4.
    EXPECT_EQ(r.query.clip_queries,
              (std::vector<std::string>{"a", "b", "c", "d"}));
}

// ---- QP-2 (FACE-4): person + face_count -------------------------------

namespace
{
    LlmFactValidator makePersonValidator()
    {
        return LlmFactValidator(ExifKeywordConfig::defaults(), nullptr,
                                [](const std::string &name) -> int {
                                    if (name == "alice") return 7;
                                    if (name == "bob") return 9;
                                    return 0;
                                });
    }
}

TEST(LlmFactValidatorTest, PersonResolvesToBoundFilter)
{
    auto v = makePersonValidator();
    auto r = v.validate("photos of alice at the beach",
                        output({{"person", "in", {"alice"}, "filter", "of alice"}},
                               {"beach"}));
    ASSERT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.query.bound.person_ids, std::vector<int>{7});
    EXPECT_EQ(r.query.clip_queries, std::vector<std::string>{"beach"});
}

TEST(LlmFactValidatorTest, UnknownPersonDemotesToClipQuery)
{
    auto v = makePersonValidator();
    auto r = v.validate("photos of ziggy",
                        output({{"person", "in", {"ziggy"}, "filter", "of ziggy"}}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 1u);
    ASSERT_EQ(r.query.clip_queries.size(), 1u);
    EXPECT_EQ(r.query.clip_queries[0], "ziggy"); // semantic content survives
}

TEST(LlmFactValidatorTest, MixedPersonListKeepsResolvedDemotesRest)
{
    auto v = makePersonValidator();
    auto r = v.validate("alice and ziggy together",
                        output({{"person", "in", {"alice", "ziggy"}, "filter",
                                 "alice and ziggy"}}));
    ASSERT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.query.bound.person_ids, std::vector<int>{7});
    ASSERT_EQ(r.query.clip_queries.size(), 1u);
    EXPECT_EQ(r.query.clip_queries[0], "ziggy");
}

TEST(LlmFactValidatorTest, FaceCountConvertsAndClamps)
{
    auto v = makePersonValidator();
    auto r = v.validate("with 3 people between 2 and 5 faces more than 90 people",
                        output({{"face_count", "eq", {"3"}, "filter", "with 3 people"},
                                {"face_count", "range", {"5", "2"}, "filter",
                                 "between 2 and 5 faces"},
                                {"face_count", "gt", {"90"}, "filter",
                                 "more than 90 people"}}));
    EXPECT_EQ(r.accepted, 2u);
    EXPECT_EQ(r.dropped, 1u); // 90 people is outside sanity (0-50)
    ASSERT_EQ(r.query.bound.numeric.size(), 2u);
    EXPECT_EQ(r.query.bound.numeric[0].column, "face_count");
    EXPECT_EQ(r.query.bound.numeric[0].op, "=");
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[0].value, 3.0);
    EXPECT_EQ(r.query.bound.numeric[1].op, "between");
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[1].value, 2.0); // reversed range fixed
    EXPECT_DOUBLE_EQ(r.query.bound.numeric[1].value2, 5.0);
}

// PLACE-2 §6.1: bare place names inferred by the model.
namespace
{
    LlmFactValidator::PlaceResolver photosIn(std::vector<std::string> names)
    {
        return [names](const std::string &n) -> std::vector<int> {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == n)
                    return {static_cast<int>(i) + 100};
            return {};
        };
    }
}

TEST(LlmFactValidatorPlaceTest, UngroundedSpanWithGroundedValueIsAnInferredBoundLocation)
{
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"yellowstone"}));
    // The real model's output for "yellowstone waterfalls": span invented.
    auto r = v.validate("yellowstone waterfalls",
                        output({{"location", "eq", {"yellowstone"}, "filter", "in yellowstone"}},
                               {"yellowstone waterfalls"}));
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.dropped, 0u);
    EXPECT_EQ(r.query.bound.place_ids, std::vector<int>{100});
    EXPECT_TRUE(r.query.bound.location_inferred);
    EXPECT_TRUE(r.query.bare.empty());
    ASSERT_EQ(r.query.clip_queries.size(), 1u);
}

TEST(LlmFactValidatorPlaceTest, GroundedSpanWithoutPrepositionIsAlsoInferred)
{
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"rome"}));
    auto r = v.validate("rome ruins",
                        output({{"location", "eq", {"rome"}, "filter", "rome"}}, {"rome ruins"}));
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_TRUE(r.query.bound.location_inferred);

    // "explore" intent on an inferred place is still a bound filter.
    auto r2 = v.validate("rome ruins",
                         output({{"location", "eq", {"rome"}, "explore", "rome"}}, {"rome ruins"}));
    EXPECT_EQ(r2.accepted, 1u);
    EXPECT_TRUE(r2.query.bare.empty());
    EXPECT_EQ(r2.query.bound.place_ids, std::vector<int>{100});
}

TEST(LlmFactValidatorPlaceTest, PrepositionedSpanIsNotInferred)
{
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"bozeman"}));
    auto r = v.validate("photos in bozeman",
                        output({{"location", "eq", {"bozeman"}, "filter", "in bozeman"}}, {"photos"}));
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_FALSE(r.query.bound.location_inferred);
    EXPECT_EQ(r.query.bound.place_ids, std::vector<int>{100});
}

TEST(LlmFactValidatorPlaceTest, InferredPlaceWithNoPhotosIsSkippedNotDropped)
{
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"bozeman"}));
    // "turkey dinner": the model calls turkey a place; the library has no
    // photos there. Skipped silently — no drop (which would trigger the
    // regex fallback), no clip demotion (the clip text already has it).
    auto r = v.validate("turkey dinner",
                        output({{"location", "eq", {"turkey"}, "filter", "turkey dinner"}},
                               {"turkey dinner"}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 0u);
    EXPECT_FALSE(r.query.bound.hasAny());
    ASSERT_EQ(r.query.clip_queries.size(), 1u);
    EXPECT_EQ(r.query.clip_queries[0], "turkey dinner");

    // Whereas an EXPLICIT unknown place still demotes to clip and counts.
    auto r2 = v.validate("hiking in narnia",
                         output({{"location", "eq", {"narnia"}, "filter", "in narnia"}}, {"hiking"}));
    EXPECT_EQ(r2.dropped, 1u);
    EXPECT_EQ(r2.query.clip_queries.size(), 2u);
}

TEST(LlmFactValidatorPlaceTest, InferredValueMustStillBeInTheQuery)
{
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"paris"}));
    auto r = v.validate("eiffel tower at night",
                        output({{"location", "eq", {"paris"}, "filter", "in paris"}}, {"eiffel tower"}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 1u) << "value not in the text: a hallucination, dropped as before";
}

// PLACE-4: setting facts ("waterfront", "near the lake").
TEST(LlmFactValidatorSettingTest, AtAndNearPickTheRadiusAndFoldPlurals)
{
    LlmFactValidatorOptions o;
    o.setting_at_m = 100;
    o.setting_near_m = 2000;
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, nullptr, o);
    auto r = v.validate("new jersey waterfronts",
                        output({{"setting", "at", {"waterfronts"}, "filter", "waterfronts"}},
                               {"waterfronts"}));
    ASSERT_EQ(r.accepted, 1u);
    ASSERT_EQ(r.query.bound.settings.size(), 1u);
    const auto &st = r.query.bound.settings[0];
    EXPECT_EQ(st.category, "waterfront");
    EXPECT_DOUBLE_EQ(st.max_m, 100.0);
    EXPECT_TRUE(st.shoreline);
    EXPECT_TRUE(st.inferred) << "bare 'waterfronts' carries no scaffolding";
    EXPECT_NE(std::find(st.fcodes.begin(), st.fcodes.end(), "H.BAY"), st.fcodes.end());
    EXPECT_TRUE(r.query.bound.hasExifDims());
    // The setting word stays semantic (the prompt says so, and the regex
    // parser keeps it): an inferred setting that matches nothing retries
    // without the filter and must still rank by "waterfronts".
    EXPECT_EQ(r.query.clip_queries, std::vector<std::string>{"waterfronts"});

    auto r2 = v.validate("sunsets near the lake",
                         output({{"setting", "near", {"lake"}, "filter", "near the lake"}}, {"sunsets"}));
    ASSERT_EQ(r2.query.bound.settings.size(), 1u);
    EXPECT_DOUBLE_EQ(r2.query.bound.settings[0].max_m, 2000.0);
    EXPECT_FALSE(r2.query.bound.settings[0].inferred);
    EXPECT_FALSE(r2.query.bound.settings[0].shoreline);
}

TEST(LlmFactValidatorSettingTest, UnknownCategoryDropsAndExploreStillBinds)
{
    auto v = makeValidator();
    auto r = v.validate("moonbase photos",
                        output({{"setting", "at", {"moonbase"}, "filter", "moonbase"}}, {"moonbase photos"}));
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.dropped, 1u);
    EXPECT_TRUE(r.query.bound.settings.empty());

    // "explore" on a setting is still a bound filter — a union of every
    // beach photo makes no sense as exploration.
    auto r2 = v.validate("beach", output({{"setting", "at", {"beach"}, "explore", "beach"}}, {"beach"}));
    EXPECT_EQ(r2.accepted, 1u);
    EXPECT_TRUE(r2.query.bare.empty());
    ASSERT_EQ(r2.query.bound.settings.size(), 1u);
    EXPECT_TRUE(r2.query.bound.settings[0].inferred);
}

TEST(LlmFactValidatorSettingTest, ClearInferredKeepsScaffoldedFacts)
{
    auto v = makeValidator();
    auto r = v.validate("beach photos near the lake",
                        output({{"setting", "at", {"beach"}, "filter", "beach"},
                                {"setting", "near", {"lake"}, "filter", "near the lake"}},
                               {"beach photos"}));
    ASSERT_EQ(r.query.bound.settings.size(), 2u);
    EXPECT_TRUE(r.query.bound.hasInferred());
    r.query.bound.clearInferred();
    ASSERT_EQ(r.query.bound.settings.size(), 1u);
    EXPECT_EQ(r.query.bound.settings[0].category, "lake");
    EXPECT_FALSE(r.query.bound.hasInferred());
}

TEST(LlmFactValidatorSettingTest, SettingWordsFiledAsLocationsAreConverted)
{
    // The shipped model's real shape: "in the mountains" -> location=mountains.
    auto v = makeValidator();
    auto r = v.validate("hiking in the mountains",
                        output({{"location", "eq", {"mountains"}, "filter", "in the mountains"}}, {"hiking"}));
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.dropped, 0u);
    EXPECT_FALSE(r.query.bound.location.has_value());
    ASSERT_EQ(r.query.bound.settings.size(), 1u);
    EXPECT_EQ(r.query.bound.settings[0].category, "mountain");
    EXPECT_DOUBLE_EQ(r.query.bound.settings[0].max_m, 1500.0);
    EXPECT_FALSE(r.query.bound.settings[0].inferred);
}

TEST(LlmFactValidatorSettingTest, SettingsTheModelMissedAreDetectedFromTheQuery)
{
    // "new jersey waterfronts" with only the location fact emitted.
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"new jersey"}));
    auto r = v.validate("new jersey waterfronts",
                        output({{"location", "eq", {"new jersey"}, "filter", "new jersey"}}, {"waterfronts"}));
    EXPECT_EQ(r.query.bound.place_ids, std::vector<int>{100});
    ASSERT_EQ(r.query.bound.settings.size(), 1u);
    EXPECT_EQ(r.query.bound.settings[0].category, "waterfront");
    EXPECT_TRUE(r.query.bound.settings[0].inferred);

    // No duplicate when the model DID emit it.
    auto r2 = v.validate("new jersey waterfronts",
                         output({{"location", "eq", {"new jersey"}, "filter", "new jersey"},
                                 {"setting", "near", {"waterfront"}, "filter", "waterfronts"}},
                                {"waterfronts"}));
    ASSERT_EQ(r2.query.bound.settings.size(), 1u);
    EXPECT_DOUBLE_EQ(r2.query.bound.settings[0].max_m, 1500.0) << "the model's op wins";
}

TEST(LlmFactValidatorPlaceTest, ExplicitPlaceAfterInferredOneIsNotInferred)
{
    LlmFactValidator v(ExifKeywordConfig::defaults(), nullptr, nullptr, photosIn({"yellowstone", "bozeman"}));
    auto r = v.validate("yellowstone sunsets in bozeman",
                        output({{"location", "eq", {"yellowstone"}, "filter", "yellowstone"},
                                {"location", "eq", {"bozeman"}, "filter", "in bozeman"}},
                               {"sunsets"}));
    EXPECT_EQ(r.query.bound.location_name, "bozeman");
    EXPECT_FALSE(r.query.bound.location_inferred) << "the explicit 'in bozeman' must never be retried away";
}

TEST(LlmFactValidatorSettingTest, ADemotedPlaceAttemptNeverBecomesASetting)
{
    auto v = makeValidator();
    auto r = v.validate("photos in mountain view",
                        output({{"location", "eq", {"mountain view"}, "filter", "in mountain view"}}, {"photos"}));
    EXPECT_EQ(r.dropped, 1u);
    EXPECT_TRUE(r.query.bound.settings.empty());
    // Same for an inferred (bare) attempt that names no photos.
    auto r2 = v.validate("lake placid winter",
                         output({{"location", "eq", {"lake placid"}, "filter", "lake placid"}}, {"winter"}));
    EXPECT_TRUE(r2.query.bound.settings.empty());
}
