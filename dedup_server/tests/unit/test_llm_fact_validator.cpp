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
