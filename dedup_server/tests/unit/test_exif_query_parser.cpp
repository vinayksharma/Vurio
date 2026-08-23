#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "search/exif_query_parser.hpp"

using namespace MediaDedup;

namespace
{
    // Gazetteer fixture shared by the location tests.
    class ExifQueryParserLocationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = (std::filesystem::temp_directory_path() / "parser_gazetteer.geojson").string();
            std::ofstream(path_) << R"({
                "type": "FeatureCollection",
                "features": [
                    {"type": "Feature", "properties": {"name": "Yellowstone NP"},
                     "bbox": [-111.15, 44.13, -109.82, 45.10], "geometry": null},
                    {"type": "Feature", "properties": {"name": "Grand Canyon"},
                     "bbox": [-113.97, 35.74, -111.63, 36.81], "geometry": null},
                    {"type": "Feature", "properties": {"name": "Utah"},
                     "bbox": [-114.03, 36.99, -109.04, 42.00], "geometry": null}
                ]})";
            ASSERT_TRUE(geocoder_.loadFromFile(path_));
        }
        void TearDown() override { std::remove(path_.c_str()); }

        std::string path_;
        LocationGeocoder geocoder_;
    };

    const ExifQueryConstraints::Numeric *findNumeric(const ExifQueryConstraints &c,
                                                     const std::string &column)
    {
        for (const auto &n : c.numeric)
            if (n.column == column)
                return &n;
        return nullptr;
    }
}

TEST(ExifQueryParserTest, PlainQueryHasNoConstraints)
{
    ExifQueryParser parser;
    auto c = parser.parse("sunset over the ocean");
    EXPECT_FALSE(c.hasAny());
    EXPECT_EQ(c.remaining, "sunset over the ocean");
    EXPECT_FALSE(c.remaining_is_filler);
}

TEST(ExifQueryParserTest, IsoEquality)
{
    ExifQueryParser parser;
    auto c = parser.parse("sunset iso 1600");
    auto *n = findNumeric(c, "exif_iso");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, "=");
    EXPECT_DOUBLE_EQ(n->value, 1600);
    EXPECT_EQ(c.remaining, "sunset");
}

TEST(ExifQueryParserTest, IsoAboveAndBelow)
{
    ExifQueryParser parser;
    auto above = parser.parse("iso above 3200");
    ASSERT_NE(findNumeric(above, "exif_iso"), nullptr);
    EXPECT_EQ(findNumeric(above, "exif_iso")->op, ">");

    auto below = parser.parse("iso under 400 portrait");
    ASSERT_NE(findNumeric(below, "exif_iso"), nullptr);
    EXPECT_EQ(findNumeric(below, "exif_iso")->op, "<");
    EXPECT_EQ(below.remaining, "portrait");
}

TEST(ExifQueryParserTest, IsoBetween)
{
    ExifQueryParser parser;
    auto c = parser.parse("iso between 400 and 1600");
    auto *n = findNumeric(c, "exif_iso");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, "between");
    EXPECT_DOUBLE_EQ(n->value, 400);
    EXPECT_DOUBLE_EQ(n->value2, 1600);
}

TEST(ExifQueryParserTest, ApertureApproxAndComparisons)
{
    ExifQueryParser parser;
    auto approx = parser.parse("portrait f/2.8");
    auto *n = findNumeric(approx, "exif_f_number");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, "between");
    EXPECT_NEAR(n->value, 2.5, 1e-9);
    EXPECT_NEAR(n->value2, 3.1, 1e-9);
    EXPECT_EQ(approx.remaining, "portrait");

    auto wider = parser.parse("wider than f/2.8");
    ASSERT_NE(findNumeric(wider, "exif_f_number"), nullptr);
    EXPECT_EQ(findNumeric(wider, "exif_f_number")->op, "<");

    auto narrower = parser.parse("narrower than f5.6");
    ASSERT_NE(findNumeric(narrower, "exif_f_number"), nullptr);
    EXPECT_EQ(findNumeric(narrower, "exif_f_number")->op, ">");
}

TEST(ExifQueryParserTest, ShutterFasterAndSlower)
{
    ExifQueryParser parser;
    auto faster = parser.parse("sports shutter faster than 1/500");
    auto *n = findNumeric(faster, "exif_exposure_time");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, "<");
    EXPECT_NEAR(n->value, 1.0 / 500.0, 1e-9);
    EXPECT_EQ(faster.remaining, "sports");

    auto slower = parser.parse("slower than 2 seconds");
    n = findNumeric(slower, "exif_exposure_time");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, ">");
    EXPECT_NEAR(n->value, 2.0, 1e-9);
}

TEST(ExifQueryParserTest, FocalLength)
{
    ExifQueryParser parser;
    auto over = parser.parse("focal length above 200mm wildlife");
    auto *n = findNumeric(over, "exif_focal_length");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, ">");
    EXPECT_DOUBLE_EQ(n->value, 200);
    EXPECT_EQ(over.remaining, "wildlife");

    auto plain = parser.parse("50mm portrait");
    n = findNumeric(plain, "exif_focal_length");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->op, "between");
    EXPECT_DOUBLE_EQ(n->value, 49);
    EXPECT_DOUBLE_EQ(n->value2, 51);
}

TEST(ExifQueryParserTest, CameraMakeConsumesWholePhrase)
{
    ExifQueryParser parser;
    auto c = parser.parse("sunset photos taken with a canon camera");
    ASSERT_EQ(c.camera_terms.size(), 1u);
    EXPECT_EQ(c.camera_terms[0], "canon");
    // "taken with a ... camera" must not leak into the semantic text.
    EXPECT_EQ(c.remaining, "sunset photos");
    EXPECT_FALSE(c.remaining_is_filler);
}

TEST(ExifQueryParserTest, PureCameraQueryIsFillerOnly)
{
    ExifQueryParser parser;
    auto c = parser.parse("find all pictures taken with a sony camera");
    ASSERT_EQ(c.camera_terms.size(), 1u);
    EXPECT_EQ(c.camera_terms[0], "sony");
    EXPECT_TRUE(c.remaining_is_filler)
        << "no semantic content left, CLIP should be skipped; remaining: " << c.remaining;
}

TEST(ExifQueryParserTest, FlashOnAndOff)
{
    ExifQueryParser parser;
    EXPECT_TRUE(parser.parse("party with flash").flash_on);
    EXPECT_TRUE(parser.parse("candlelight without flash").flash_off);
    EXPECT_TRUE(parser.parse("no flash portraits").flash_off);
    EXPECT_FALSE(parser.parse("flashy dress").flash_on)
        << "'flashy' must not read as a flash constraint";
}

TEST(ExifQueryParserTest, YearAndMonthDates)
{
    ExifQueryParser parser;
    auto year = parser.parse("beach 2024");
    ASSERT_TRUE(year.date_range.has_value());
    EXPECT_EQ(year.date_range->first, "2024:01:01 00:00:00");
    EXPECT_EQ(year.date_range->second, "2024:12:31 23:59:59");
    EXPECT_EQ(year.remaining, "beach");

    auto month = parser.parse("snow august 2023");
    ASSERT_TRUE(month.date_range.has_value());
    EXPECT_EQ(month.date_range->first, "2023:08:01 00:00:00");
    EXPECT_EQ(month.date_range->second, "2023:08:31 23:59:59");

    auto bare = parser.parse("photos from december");
    ASSERT_TRUE(bare.month_any_year.has_value());
    EXPECT_EQ(*bare.month_any_year, "12");
    EXPECT_FALSE(bare.date_range.has_value());
}

TEST(ExifQueryParserTest, MayIsNotTreatedAsAMonth)
{
    ExifQueryParser parser;
    auto c = parser.parse("photos that may contain dogs");
    EXPECT_FALSE(c.month_any_year.has_value());
}

TEST(ExifQueryParserTest, IsoNumberNotMistakenForYear)
{
    ExifQueryParser parser;
    auto c = parser.parse("iso 2000 concert");
    auto *n = findNumeric(c, "exif_iso");
    ASSERT_NE(n, nullptr);
    EXPECT_DOUBLE_EQ(n->value, 2000);
    EXPECT_FALSE(c.date_range.has_value())
        << "the 2000 belongs to the ISO constraint, not a year filter";
}

TEST_F(ExifQueryParserLocationTest, PrepositionTriggersLocationLookup)
{
    ExifQueryParser parser(&geocoder_);
    auto c = parser.parse("sunset in yellowstone");
    ASSERT_TRUE(c.location.has_value());
    EXPECT_NEAR(c.location->lat_min, 44.13, 1e-6);
    EXPECT_EQ(c.remaining, "sunset");
}

TEST_F(ExifQueryParserLocationTest, MultiWordLocation)
{
    ExifQueryParser parser(&geocoder_);
    auto c = parser.parse("hiking at grand canyon");
    ASSERT_TRUE(c.location.has_value());
    EXPECT_NEAR(c.location->lon_min, -113.97, 1e-6);
    EXPECT_EQ(c.remaining, "hiking");
}

TEST_F(ExifQueryParserLocationTest, NoPrepositionMeansNoLocationFilter)
{
    ExifQueryParser parser(&geocoder_);
    auto c = parser.parse("yellowstone wildlife");
    EXPECT_FALSE(c.location.has_value())
        << "bare place names stay in the semantic text (city names like "
           "Nice or Phoenix would otherwise become false GPS filters)";
    EXPECT_EQ(c.remaining, "yellowstone wildlife");
}

TEST_F(ExifQueryParserLocationTest, UnknownPlaceAfterPrepositionIsLeftAlone)
{
    ExifQueryParser parser(&geocoder_);
    auto c = parser.parse("dinner in atlantis");
    EXPECT_FALSE(c.location.has_value());
    EXPECT_EQ(c.remaining, "dinner in atlantis");
}

TEST_F(ExifQueryParserLocationTest, FullCombinedQuery)
{
    ExifQueryParser parser(&geocoder_);
    auto c = parser.parse("find all sunset pictures taken with a sony camera in yellowstone iso above 1600");

    ASSERT_EQ(c.camera_terms.size(), 1u);
    EXPECT_EQ(c.camera_terms[0], "sony");

    auto *iso = findNumeric(c, "exif_iso");
    ASSERT_NE(iso, nullptr);
    EXPECT_EQ(iso->op, ">");
    EXPECT_DOUBLE_EQ(iso->value, 1600);

    ASSERT_TRUE(c.location.has_value());
    EXPECT_EQ(c.location_name, "yellowstone");

    EXPECT_EQ(c.remaining, "sunset pictures");
    EXPECT_FALSE(c.remaining_is_filler);
}

// --- QP-1a: intent-split parsing (bound vs bare) ---------------------------

TEST(ExifQueryParserIntentTest, ScaffoldedCameraIsBoundBareBrandIsBare)
{
    ExifQueryParser parser;

    auto bound = parser.parseStructured("photos taken with a canon");
    ASSERT_EQ(bound.bound.camera_terms.size(), 1u);
    EXPECT_EQ(bound.bound.camera_terms[0], "canon");
    EXPECT_TRUE(bound.bare.empty());
    EXPECT_TRUE(bound.remaining_is_filler);

    auto bare = parser.parseStructured("canon");
    EXPECT_TRUE(bare.bound.camera_terms.empty());
    ASSERT_EQ(bare.bare.size(), 1u);
    ASSERT_EQ(bare.bare[0].camera_terms.size(), 1u);
    EXPECT_EQ(bare.bare[0].camera_terms[0], "canon");

    // Suffix alone also scaffolds: "canon camera".
    auto suffixed = parser.parseStructured("canon camera");
    ASSERT_EQ(suffixed.bound.camera_terms.size(), 1u);
    EXPECT_TRUE(suffixed.bare.empty());
}

TEST(ExifQueryParserIntentTest, ComparativeIsBoundExactProbeIsBare)
{
    ExifQueryParser parser;

    auto cmp = parser.parseStructured("photos iso above 800");
    ASSERT_EQ(cmp.bound.numeric.size(), 1u);
    EXPECT_EQ(cmp.bound.numeric[0].op, ">");
    EXPECT_TRUE(cmp.bare.empty());

    auto probe = parser.parseStructured("iso 800");
    EXPECT_TRUE(probe.bound.numeric.empty());
    ASSERT_EQ(probe.bare.size(), 1u);
    ASSERT_EQ(probe.bare[0].numeric.size(), 1u);
    EXPECT_EQ(probe.bare[0].numeric[0].column, "exif_iso");
    EXPECT_EQ(probe.bare[0].numeric[0].op, "=");

    // The motivating case: a lone aperture probe is exploration.
    auto f = parser.parseStructured("f2.5");
    EXPECT_TRUE(f.bound.numeric.empty());
    ASSERT_EQ(f.bare.size(), 1u);
    EXPECT_EQ(f.bare[0].numeric[0].column, "exif_f_number");
    EXPECT_EQ(f.bare[0].numeric[0].op, "between");
    EXPECT_TRUE(f.clip_queries.empty()) << "nothing semantic remains; the "
                                           "engine unions CLIP on the raw query";
}

TEST(ExifQueryParserIntentTest, BareDatesAreBound)
{
    // User decision 2026-08-23: a date token has no visual meaning for
    // CLIP, so even scaffold-less dates express filter intent.
    ExifQueryParser parser;

    auto year = parser.parseStructured("sunset 2024");
    ASSERT_TRUE(year.bound.date_range.has_value());
    EXPECT_TRUE(year.bare.empty());
    ASSERT_EQ(year.clip_queries.size(), 1u);
    EXPECT_EQ(year.clip_queries[0], "sunset");

    auto month = parser.parseStructured("beach august");
    ASSERT_TRUE(month.bound.month_any_year.has_value());
    EXPECT_EQ(*month.bound.month_any_year, "08");

    // "may" keeps its ambiguity exemption: bare it stays semantic.
    auto may = parser.parseStructured("may flowers");
    EXPECT_FALSE(may.bound.month_any_year.has_value());
    ASSERT_EQ(may.clip_queries.size(), 1u);
    EXPECT_EQ(may.clip_queries[0], "may flowers");
}

TEST(ExifQueryParserIntentTest, MultipleBareBrandsBecomeSeparateBranches)
{
    // "canon, nikon" — each bare brand is its own union branch (within-field
    // OR at the engine level), fixing the old AND-of-LIKEs that could never
    // match. The comma residue must not count as semantic content.
    ExifQueryParser parser;
    auto sq = parser.parseStructured("canon, nikon");
    EXPECT_TRUE(sq.bound.camera_terms.empty());
    ASSERT_EQ(sq.bare.size(), 2u);
    EXPECT_EQ(sq.bare[0].camera_terms[0], "canon");
    EXPECT_EQ(sq.bare[1].camera_terms[0], "nikon");
    EXPECT_TRUE(sq.remaining_is_filler);
    EXPECT_TRUE(sq.clip_queries.empty());
}

TEST(ExifQueryParserIntentTest, MixedQuerySplitsByIntent)
{
    // Bound camera + bare aperture probe + semantic text, all in one query.
    ExifQueryParser parser;
    auto sq = parser.parseStructured("sunset taken with a canon, f2.5");
    ASSERT_EQ(sq.bound.camera_terms.size(), 1u);
    EXPECT_EQ(sq.bound.camera_terms[0], "canon");
    ASSERT_EQ(sq.bare.size(), 1u);
    EXPECT_EQ(sq.bare[0].numeric[0].column, "exif_f_number");
    ASSERT_EQ(sq.clip_queries.size(), 1u);
    EXPECT_EQ(sq.clip_queries[0], "sunset");
}

TEST(ExifQueryParserIntentTest, FlatParseMatchesStructuredView)
{
    // parse() is the binding-blind flattening of parseStructured().
    ExifQueryParser parser;
    const std::string q = "sunset taken with a canon iso 800 in 2024";
    auto flat = parser.parse(q);
    auto sq = parser.parseStructured(q);

    std::size_t structured_numeric = sq.bound.numeric.size();
    std::size_t structured_cameras = sq.bound.camera_terms.size();
    for (const auto &b : sq.bare)
    {
        structured_numeric += b.numeric.size();
        structured_cameras += b.camera_terms.size();
    }
    EXPECT_EQ(flat.numeric.size(), structured_numeric);
    EXPECT_EQ(flat.camera_terms.size(), structured_cameras);
    EXPECT_EQ(flat.date_range.has_value(), sq.bound.date_range.has_value());
    EXPECT_EQ(flat.remaining, sq.remaining);
    EXPECT_EQ(flat.remaining_is_filler, sq.remaining_is_filler);
}

TEST(ExifQueryParserIntentTest, ReversedIsoRangeIsNormalized)
{
    ExifQueryParser parser;
    auto sq = parser.parseStructured("iso between 1600 and 400");
    ASSERT_EQ(sq.bound.numeric.size(), 1u);
    EXPECT_EQ(sq.bound.numeric[0].op, "between");
    EXPECT_DOUBLE_EQ(sq.bound.numeric[0].value, 400.0);
    EXPECT_DOUBLE_EQ(sq.bound.numeric[0].value2, 1600.0);
}
