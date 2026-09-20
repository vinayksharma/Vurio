#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "places_test_fixture.hpp"
#include "search/location_geocoder.hpp"
#include "search/offline_reverse_geocoder.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Test;

// PLACE-1: the offline GPS -> place-name resolver over a miniature gazetteer
// in the production file layout.

namespace
{
    class OfflineReverseGeocoderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            gaz_path_ = writeMiniGazetteer("orv_geocoder.sqlite");
            ASSERT_FALSE(gaz_path_.empty());
            bounds_path_ = writeMiniBounds("orv_bounds.geojson");
            ASSERT_FALSE(bounds_path_.empty());
            ASSERT_TRUE(bounds_.loadFromFile(bounds_path_));
            ASSERT_TRUE(geo_.open(gaz_path_));
            geo_.setBoundsGazetteer(&bounds_);
        }

        void TearDown() override
        {
            geo_.close();
            std::remove(gaz_path_.c_str());
            std::remove(bounds_path_.c_str());
        }

        static const OfflineReverseGeocoder::Place *role(const OfflineReverseGeocoder::Resolution &r,
                                                         const std::string &role)
        {
            for (const auto &p : r.places)
                if (p.role == role)
                    return &p;
            return nullptr;
        }

        std::string gaz_path_, bounds_path_;
        LocationGeocoder bounds_;
        OfflineReverseGeocoder geo_;
    };
}

TEST_F(OfflineReverseGeocoderTest, OpensAndReportsMetadata)
{
    EXPECT_TRUE(geo_.isOpen());
    EXPECT_EQ(geo_.size(), 13u);
    EXPECT_EQ(geo_.regionName("US", "WY"), "Wyoming");
    EXPECT_EQ(geo_.countryName("US"), "United States");
    EXPECT_EQ(geo_.regionName("US", "ZZ"), "");
}

TEST_F(OfflineReverseGeocoderTest, MissingFileDoesNotOpen)
{
    OfflineReverseGeocoder g;
    EXPECT_FALSE(g.open("/nonexistent/places.sqlite"));
    EXPECT_FALSE(g.isOpen());
    EXPECT_FALSE(g.resolve(45.0, -111.0).has_value());
}

TEST_F(OfflineReverseGeocoderTest, InTownFixNamesTownRegionCountry)
{
    // 500 m from Bozeman's point.
    auto r = geo_.resolve(45.6840, -111.0380);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->name, "Bozeman");
    EXPECT_LT(role(*r, "locality")->distance_m, 1000.0);
    ASSERT_NE(role(*r, "region"), nullptr);
    EXPECT_EQ(role(*r, "region")->name, "Montana");
    ASSERT_NE(role(*r, "country"), nullptr);
    EXPECT_EQ(role(*r, "country")->name, "United States");
    EXPECT_FALSE(r->locality_is_near);
    EXPECT_EQ(r->label, "Bozeman · Montana · United States");
    // Every row carries a stable external id for the places table.
    for (const auto &p : r->places)
    {
        EXPECT_FALSE(p.source.empty()) << p.name;
        EXPECT_FALSE(p.external_id.empty()) << p.name;
    }
}

TEST_F(OfflineReverseGeocoderTest, PopulationDiscountPrefersTheCityOverANearerHamlet)
{
    // ~1 km from the nameless "Suburbia" hamlet, ~2.7 km from Bozeman
    // (53k people): Bozeman's score = 2700 / (1 + log10(53294)) ≈ 470,
    // the hamlet's = 1000 / 1 = 1000. The city wins, and 2.7 km is inside
    // its population-scaled town radius (16·√53293 ≈ 3.7 km) — "in", not
    // "near".
    auto r = geo_.resolve(45.7040, -111.0100);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->name, "Bozeman");
    EXPECT_FALSE(r->locality_is_near);
    EXPECT_EQ(r->label, "Bozeman · Montana · United States");
}

TEST_F(OfflineReverseGeocoderTest, HamletRightHereBeatsTheDistantCity)
{
    // 300 m from Tinyville, ~22 km from Bozeman: 300 vs 22000/5.7 ≈ 3860.
    auto r = geo_.resolve(45.6027, -111.3000);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->name, "Tinyville");
    EXPECT_EQ(r->label, "Tinyville · Montana · United States");
}

TEST_F(OfflineReverseGeocoderTest, FarFromTownIsNear)
{
    // ~13 km south-west of West Yellowstone (pop 1271 -> town radius
    // 1.5 km), outside the park box, nothing else within 30 km.
    auto r = geo_.resolve(44.5700, -111.2000);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->name, "West Yellowstone");
    EXPECT_TRUE(r->locality_is_near);
    EXPECT_EQ(r->label.rfind("near West Yellowstone", 0), 0u) << r->label;
}

TEST_F(OfflineReverseGeocoderTest, LandmarkAndParkLeadTheLabelAndFixTheState)
{
    // 60 m from Fairy Falls, inside the Yellowstone NP box. The nearest
    // town (West Yellowstone, 24 km) is in Montana; the falls are in
    // Wyoming — the nearest feature's admin codes must win.
    auto r = geo_.resolve(44.5254, -110.8710);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "landmark"), nullptr);
    EXPECT_EQ(role(*r, "landmark")->name, "Fairy Falls");
    EXPECT_EQ(role(*r, "landmark")->kind, "water");
    ASSERT_NE(role(*r, "park"), nullptr);
    EXPECT_EQ(role(*r, "park")->name, "Yellowstone NP");
    EXPECT_EQ(role(*r, "park")->source, "naturalearth");
    ASSERT_NE(role(*r, "region"), nullptr);
    EXPECT_EQ(role(*r, "region")->name, "Wyoming");
    // Locality row is still stored (searchable) but the park replaces it
    // in the label.
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->name, "West Yellowstone");
    EXPECT_EQ(r->label, "Fairy Falls · Yellowstone NP · Wyoming");
}

TEST_F(OfflineReverseGeocoderTest, LandmarkRadiusAndCapAreHonoured)
{
    OfflineReverseGeocoder::Options o;
    o.landmark_radius_m = 100.0; // Fairy Falls is ~60 m away; the geyser is 8 km
    OfflineReverseGeocoder g;
    ASSERT_TRUE(g.open(gaz_path_, o));
    auto r = g.resolve(44.5254, -110.8710);
    ASSERT_TRUE(r.has_value());
    int landmarks = 0;
    for (const auto &p : r->places)
        if (p.role == "landmark")
            ++landmarks;
    EXPECT_EQ(landmarks, 1);

    o.landmark_radius_m = 20.0; // now nothing is close enough
    OfflineReverseGeocoder g2;
    ASSERT_TRUE(g2.open(gaz_path_, o));
    auto r2 = g2.resolve(44.5254, -110.8710);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(role(*r2, "landmark"), nullptr);
}

TEST_F(OfflineReverseGeocoderTest, TownAgreesWithTheStateOfTheLandmarkRightHere)
{
    // 60 m from Newport Waterfront Park (NJ). Manhattan's point (8.8M
    // people) is 2.7 km away and outscores Jersey City's point (3.7 km);
    // without the landmark rule the label would read "New York City ·
    // New Jersey".
    auto r = geo_.resolve(40.7270, -74.0340);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "landmark"), nullptr);
    EXPECT_EQ(role(*r, "landmark")->name, "Newport Waterfront Park");
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->name, "Jersey City");
    ASSERT_NE(role(*r, "region"), nullptr);
    EXPECT_EQ(role(*r, "region")->name, "New Jersey");
    EXPECT_EQ(r->label, "Newport Waterfront Park · Jersey City · New Jersey");

    // 2 km out on the river, no landmark within reach: the population
    // discount alone decides, and Manhattan legitimately wins.
    auto r2 = geo_.resolve(40.7100, -74.0200);
    ASSERT_TRUE(r2.has_value());
    ASSERT_NE(role(*r2, "locality"), nullptr);
    EXPECT_EQ(role(*r2, "locality")->name, "New York City");
}

TEST_F(OfflineReverseGeocoderTest, ShorelineDistanceComesFromTheNearestSegment)
{
    // PLACE-4: fixture shore segment along lat 45.68 from lon -111.045 to
    // -111.040; the fix is 0.004° north and 0.002° east of its end.
    ASSERT_TRUE(geo_.hasShore());
    auto d = geo_.shoreDistance(45.6840, -111.0380);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(*d, 472.0, 25.0);
    // Directly above the segment's middle: pure north-south distance.
    auto d2 = geo_.shoreDistance(45.6810, -111.0425);
    ASSERT_TRUE(d2.has_value());
    EXPECT_NEAR(*d2, 111.0, 5.0);
    // Far away (Fairy Falls, ~140 km): nothing within shore_max_m.
    EXPECT_FALSE(geo_.shoreDistance(44.5254, -110.8710).has_value());
    // resolve() carries it.
    auto r = geo_.resolve(45.6840, -111.0380);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->shore_distance_m.has_value());
    EXPECT_NEAR(*r->shore_distance_m, 472.0, 25.0);
    auto r2 = geo_.resolve(44.5254, -110.8710);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->shore_distance_m.has_value());
}

TEST_F(OfflineReverseGeocoderTest, LandmarkRowsCarryTheExactFeatureCode)
{
    auto r = geo_.resolve(44.5254, -110.8710);
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(role(*r, "landmark"), nullptr);
    EXPECT_EQ(role(*r, "landmark")->fcode, "H.OVF");
    ASSERT_NE(role(*r, "locality"), nullptr);
    EXPECT_EQ(role(*r, "locality")->fcode, "P.PPL");
}

TEST_F(OfflineReverseGeocoderTest, LandmarkCapKeepsTheNearestOfEveryKind)
{
    // Review fix: with a cap of 1, the nearest row (Peets Hill, terrain)
    // would have crowded out the pond — but "near the lake" needs the water
    // row, so every kind keeps its nearest feature.
    OfflineReverseGeocoder::Options o;
    o.max_landmarks = 1;
    OfflineReverseGeocoder g;
    ASSERT_TRUE(g.open(gaz_path_, o));
    auto r = g.resolve(45.6840, -111.0380);
    ASSERT_TRUE(r.has_value());
    bool water = false, terrain = false;
    for (const auto &p : r->places)
        if (p.role == "landmark")
        {
            water = water || p.kind == "water";
            terrain = terrain || p.kind == "terrain";
        }
    EXPECT_TRUE(water);
    EXPECT_TRUE(terrain);
    EXPECT_EQ(r->label, "Bozeman · Montana · United States") << "both are past the 500 m label radius";
}

TEST_F(OfflineReverseGeocoderTest, MidOceanFixResolvesToNothing)
{
    auto r = geo_.resolve(0.0, -30.0);
    ASSERT_TRUE(r.has_value()) << "an open geocoder always answers";
    EXPECT_TRUE(r->places.empty());
    EXPECT_TRUE(r->label.empty());
}

TEST_F(OfflineReverseGeocoderTest, LocalityRadiusLimitsTheSearch)
{
    OfflineReverseGeocoder::Options o;
    o.locality_max_km = 5.0;
    OfflineReverseGeocoder g;
    ASSERT_TRUE(g.open(gaz_path_, o));
    // Fairy Falls: nearest town is 24 km away — beyond the limit.
    auto r = g.resolve(44.5254, -110.8710);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(role(*r, "locality"), nullptr);
    // ...but landmark + (admin from the landmark) still resolve.
    ASSERT_NE(role(*r, "landmark"), nullptr);
    ASSERT_NE(role(*r, "region"), nullptr);
    EXPECT_EQ(role(*r, "region")->name, "Wyoming");
}

TEST_F(OfflineReverseGeocoderTest, InvalidCoordinatesAreRejected)
{
    EXPECT_FALSE(geo_.resolve(91.0, 0.0).has_value());
    EXPECT_FALSE(geo_.resolve(0.0, 181.0).has_value());
    EXPECT_FALSE(geo_.resolve(std::nan(""), 0.0).has_value());
}

TEST(OfflineReverseGeocoderMath, HaversineIsSane)
{
    // One degree of latitude ≈ 111.2 km.
    EXPECT_NEAR(OfflineReverseGeocoder::haversineMeters(45.0, -111.0, 46.0, -111.0), 111195.0, 200.0);
    EXPECT_DOUBLE_EQ(OfflineReverseGeocoder::haversineMeters(10.0, 10.0, 10.0, 10.0), 0.0);
}

// ---------------------------------------------------------------------------
// Live smoke over the REAL gazetteer (geodata/places.sqlite, 731 MB, built by
// scripts/build_places_db.py). Skips when the file is absent — it is not
// committed. Asserts label quality on well-known fixes and that a lookup is
// sub-millisecond-ish on the clustered file.

#include <chrono>
#include <filesystem>

TEST(OfflineReverseGeocoderLiveTest, RealGazetteerNamesWellKnownFixes)
{
    std::filesystem::path source(__FILE__);
    auto root = source.parent_path().parent_path().parent_path();
    auto gaz = root / "geodata" / "places.sqlite";
    auto bounds_path = root / "geodata" / "world_locations.geojson";
    if (!std::filesystem::exists(gaz))
        GTEST_SKIP() << "build geodata/places.sqlite to run the live smoke";

    LocationGeocoder bounds;
    ASSERT_TRUE(bounds.loadFromFile(bounds_path.string()));
    OfflineReverseGeocoder geo;
    ASSERT_TRUE(geo.open(gaz.string()));
    geo.setBoundsGazetteer(&bounds);
    EXPECT_GT(geo.size(), 9'000'000u);

    struct Probe { const char *what; double lat, lon; const char *expect; };
    const Probe probes[] = {
        {"Fairy Falls, Yellowstone", 44.5249, -110.8710, "Fairy Falls"},
        {"Old Faithful", 44.4605, -110.8281, "Yellowstone"},
        {"downtown Bozeman", 45.6790, -111.0390, "Bozeman"},
        {"Times Square", 40.7580, -73.9855, "New York"},
        {"Eiffel Tower", 48.8584, 2.2945, "Paris"},
        {"Banff townsite", 51.1784, -115.5708, "Banff"},
        {"Delicate Arch", 38.7436, -109.4993, "Delicate Arch"},
    };
    for (const auto &p : probes)
    {
        auto t0 = std::chrono::steady_clock::now();
        auto r = geo.resolve(p.lat, p.lon);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_TRUE(r.has_value()) << p.what;
        std::cout << "  " << p.what << " -> \"" << r->label << "\" (" << r->places.size()
                  << " rows, " << ms << " ms)\n";
        EXPECT_NE(r->label.find(p.expect), std::string::npos) << p.what << ": " << r->label;
        EXPECT_LT(ms, 250.0) << p.what;
    }
    auto ocean = geo.resolve(0.0, -30.0);
    ASSERT_TRUE(ocean.has_value());
    EXPECT_TRUE(ocean->label.empty()) << ocean->label;
}
