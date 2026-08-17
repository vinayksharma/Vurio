// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §5.2, §10): the two display
// geometry helpers. computeDisplayDimensions is the identity today; its test
// exists to be extended when the render chain does.

#include <gtest/gtest.h>
#include <string>

#include "utils/display_geometry.hpp"

using MediaDedup::computeDisplayDimensions;
using MediaDedup::displayDimensionsFromExifResolution;
using MediaDedup::parseExifResolution;

namespace
{
    std::pair<int, int> parsed(const std::string &v)
    {
        int w = -99, h = -99;
        parseExifResolution(v, w, h);
        return {w, h};
    }
}

TEST(DisplayGeometryTest, ParsesWellFormedResolution)
{
    EXPECT_EQ(parsed("3024x4032"), std::make_pair(3024, 4032));
    EXPECT_EQ(parsed("4032x3024"), std::make_pair(4032, 3024));
    EXPECT_EQ(parsed("1x1"), std::make_pair(1, 1));
    EXPECT_EQ(parsed("200000x200000"), std::make_pair(200000, 200000));
    EXPECT_EQ(parsed(" 640 x 480 "), std::make_pair(640, 480)) << "tolerates whitespace";
    EXPECT_EQ(parsed("640X480"), std::make_pair(640, 480)) << "tolerates upper-case X";
}

TEST(DisplayGeometryTest, RejectsMalformedResolution)
{
    const std::pair<int, int> zero{0, 0};
    EXPECT_EQ(parsed(""), zero);
    EXPECT_EQ(parsed("abc"), zero);
    EXPECT_EQ(parsed("0x0"), zero);
    EXPECT_EQ(parsed("-1x5"), zero);
    EXPECT_EQ(parsed("5x-1"), zero);
    EXPECT_EQ(parsed("999999999x1"), zero);
    EXPECT_EQ(parsed("1x999999999"), zero);
    EXPECT_EQ(parsed("200001x10"), zero);
    EXPECT_EQ(parsed("x100"), zero);
    EXPECT_EQ(parsed("100x"), zero);
    EXPECT_EQ(parsed("100"), zero);
    EXPECT_EQ(parsed("100x200x300"), zero);
    EXPECT_EQ(parsed("12ax34"), zero);
    EXPECT_EQ(parsed("1.5x2"), zero);
}

TEST(DisplayGeometryTest, ComputeDisplayDimensionsIsIdentityToday)
{
    auto d = computeDisplayDimensions(3000, 2000);
    EXPECT_EQ(d.width, 3000);
    EXPECT_EQ(d.height, 2000);
}

TEST(DisplayGeometryTest, ComputeDisplayDimensionsKeepsUnknownUnknown)
{
    EXPECT_EQ(computeDisplayDimensions(0, 0).width, 0);
    EXPECT_EQ(computeDisplayDimensions(0, 0).height, 0);
    EXPECT_EQ(computeDisplayDimensions(100, 0).width, 0);
    EXPECT_EQ(computeDisplayDimensions(0, 100).height, 0);
    EXPECT_EQ(computeDisplayDimensions(-5, 100).width, 0);
}

TEST(DisplayGeometryTest, FromExifResolutionComposesBoth)
{
    auto d = displayDimensionsFromExifResolution("3024x4032");
    EXPECT_EQ(d.width, 3024);
    EXPECT_EQ(d.height, 4032);
    auto u = displayDimensionsFromExifResolution("");
    EXPECT_EQ(u.width, 0);
    EXPECT_EQ(u.height, 0);
}
