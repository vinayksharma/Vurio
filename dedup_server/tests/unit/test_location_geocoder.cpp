#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "search/location_geocoder.hpp"

using namespace MediaDedup;

namespace
{
    // A miniature gazetteer covering the shapes the loader must handle:
    // explicit bbox with null geometry (the bundled file's form), a Polygon
    // with no bbox, a Point with no bbox, and multiple name property keys.
    constexpr const char *kFixture = R"({
        "type": "FeatureCollection",
        "features": [
            {"type": "Feature",
             "properties": {"name": "Yellowstone NP", "kind": "park"},
             "bbox": [-111.1544, 44.1343, -109.8265, 45.1001],
             "geometry": null},
            {"type": "Feature",
             "properties": {"name": "Yellowstone", "kind": "park"},
             "bbox": [-111.1544, 44.1343, -109.8265, 45.1001],
             "geometry": null},
            {"type": "Feature",
             "properties": {"NAME": "Utah", "ADMIN": "United States of America"},
             "geometry": {"type": "Polygon", "coordinates":
                 [[[-114.05, 37.0], [-109.05, 37.0], [-109.05, 42.0], [-114.05, 42.0], [-114.05, 37.0]]]}},
            {"type": "Feature",
             "properties": {"name": "Paris"},
             "bbox": [2.103, 48.6081, 2.603, 49.1081],
             "geometry": null},
            {"type": "Feature",
             "properties": {"name": "Pointville"},
             "geometry": {"type": "Point", "coordinates": [10.5, 20.5]}}
        ]
    })";

    std::string writeFixture()
    {
        auto path = (std::filesystem::temp_directory_path() / "geocoder_fixture.geojson").string();
        std::ofstream out(path);
        out << kFixture;
        return path;
    }
}

TEST(LocationGeocoderTest, LoadsBboxPolygonAndPointFeatures)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));
    // 5 features but "United States of America" adds a 6th name for Utah's row.
    EXPECT_EQ(geo.size(), 6u);
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, ExactLookupIsCaseInsensitive)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));

    auto b = geo.lookup("YELLOWSTONE");
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->lon_min, -111.1544, 1e-4);
    EXPECT_NEAR(b->lat_min, 44.1343, 1e-4);
    EXPECT_NEAR(b->lon_max, -109.8265, 1e-4);
    EXPECT_NEAR(b->lat_max, 45.1001, 1e-4);
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, PolygonGeometryProducesComputedBounds)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));

    auto b = geo.lookup("utah");
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->lon_min, -114.05, 1e-4);
    EXPECT_NEAR(b->lon_max, -109.05, 1e-4);
    EXPECT_NEAR(b->lat_min, 37.0, 1e-4);
    EXPECT_NEAR(b->lat_max, 42.0, 1e-4);
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, PointGeometryLoads)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));

    auto b = geo.lookup("pointville");
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->lon_min, 10.5, 1e-4);
    EXPECT_NEAR(b->lat_max, 20.5, 1e-4);
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, SubstringMatchesStoredNameContainingQuery)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));

    // No exact "yellow" entry; "Yellowstone"/"Yellowstone NP" contain it.
    auto b = geo.lookup("yellow");
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->lat_min, 44.1343, 1e-4);
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, ShortQueriesOnlyMatchExactly)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));

    // "pa" is a substring of "Paris" but two chars must not fuzzy-match.
    EXPECT_FALSE(geo.lookup("pa").has_value());
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, UnknownNameReturnsNothing)
{
    LocationGeocoder geo;
    auto path = writeFixture();
    ASSERT_TRUE(geo.loadFromFile(path));
    EXPECT_FALSE(geo.lookup("atlantis").has_value());
    EXPECT_FALSE(geo.lookup("   ").has_value());
    EXPECT_FALSE(geo.lookup("").has_value());
    std::remove(path.c_str());
}

TEST(LocationGeocoderTest, MissingOrMalformedFileFailsCleanly)
{
    LocationGeocoder geo;
    EXPECT_FALSE(geo.loadFromFile("/nonexistent/nowhere.geojson"));

    auto path = (std::filesystem::temp_directory_path() / "geocoder_bad.geojson").string();
    std::ofstream(path) << "this is not json{";
    EXPECT_FALSE(geo.loadFromFile(path));
    EXPECT_EQ(geo.size(), 0u);
    std::remove(path.c_str());
}

// The bundled gazetteer ships in the repo; make sure it actually loads and
// answers the queries the feature was built around.
TEST(LocationGeocoderTest, BundledGazetteerAnswersRealQueries)
{
    // tests run from the build tree; the gazetteer lives in the source tree.
    std::filesystem::path source_file(__FILE__);
    auto bundled = source_file.parent_path().parent_path().parent_path() / "geodata" / "world_locations.geojson";
    if (!std::filesystem::exists(bundled))
        GTEST_SKIP() << "bundled gazetteer not found at " << bundled;

    LocationGeocoder geo;
    ASSERT_TRUE(geo.loadFromFile(bundled.string()));
    EXPECT_GT(geo.size(), 400u);

    for (const char *q : {"yellowstone", "utah", "paris", "india", "yosemite", "grand canyon"})
    {
        auto b = geo.lookup(q);
        EXPECT_TRUE(b.has_value()) << "no bounds for: " << q;
        if (b)
        {
            EXPECT_LT(b->lat_min, b->lat_max) << q;
            EXPECT_LT(b->lon_min, b->lon_max) << q;
            EXPECT_GE(b->lat_min, -90.0) << q;
            EXPECT_LE(b->lat_max, 90.0) << q;
            EXPECT_GE(b->lon_min, -180.0) << q;
            EXPECT_LE(b->lon_max, 180.0) << q;
        }
    }

    // "india" must resolve to the country, not Indiana (exact beats substring).
    auto india = geo.lookup("india");
    ASSERT_TRUE(india.has_value());
    EXPECT_GT(india->lon_min, 60.0);
}
