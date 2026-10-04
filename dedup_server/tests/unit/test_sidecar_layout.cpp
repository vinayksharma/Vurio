// ADJ-1 (docs §5.6, §8): the asset <-> sidecar path mapping for the default
// root, an external root, and a root inside the location.

#include <gtest/gtest.h>
#include "adjustments/sidecar_layout.hpp"

using MediaDedup::Adjustments::SidecarLayout;

TEST(SidecarLayoutTest, DefaultRootPutsTheSidecarNextToTheAsset)
{
    SidecarLayout layout("/Volumes/Photos/", "");
    EXPECT_TRUE(layout.isDefaultRoot());
    EXPECT_EQ(layout.editsRoot(), "/Volumes/Photos");
    auto sidecar = layout.sidecarPath("/Volumes/Photos/2024/DSC04624.JPG");
    ASSERT_TRUE(sidecar.has_value());
    EXPECT_EQ(*sidecar, "/Volumes/Photos/2024/DSC04624.JPG.vurio.json");
    auto asset = layout.assetPath(*sidecar);
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(*asset, "/Volumes/Photos/2024/DSC04624.JPG");
    EXPECT_EQ(layout.relativePath("/Volumes/Photos/2024/DSC04624.JPG").value(), "2024/DSC04624.JPG");
}

TEST(SidecarLayoutTest, ExternalRootMirrorsTheStructure)
{
    SidecarLayout layout("/Volumes/Photos", "/Users/me/Edits");
    EXPECT_FALSE(layout.isDefaultRoot());
    EXPECT_EQ(layout.sidecarPath("/Volumes/Photos/2024/Tetons/DSC04624.JPG").value(),
              "/Users/me/Edits/2024/Tetons/DSC04624.JPG.vurio.json");
    EXPECT_EQ(layout.assetPath("/Users/me/Edits/2024/Tetons/DSC04624.JPG.vurio.json").value(),
              "/Volumes/Photos/2024/Tetons/DSC04624.JPG");
    EXPECT_FALSE(layout.assetPath("/Volumes/Photos/2024/DSC04624.JPG.vurio.json").has_value())
        << "a sidecar beside the asset is not under the external root";
    EXPECT_FALSE(layout.assetPath("/Users/me/Edits/2024/notes.txt").has_value());
    EXPECT_FALSE(layout.sidecarPath("/Elsewhere/a.jpg").has_value());
}

TEST(SidecarLayoutTest, RootInsideTheLocationWorks)
{
    SidecarLayout layout("/Volumes/Photos", "/Volumes/Photos/.vurio-edits");
    EXPECT_EQ(layout.sidecarPath("/Volumes/Photos/a.jpg").value(), "/Volumes/Photos/.vurio-edits/a.jpg.vurio.json");
    EXPECT_EQ(layout.assetPath("/Volumes/Photos/.vurio-edits/a.jpg.vurio.json").value(), "/Volumes/Photos/a.jpg");
}

TEST(SidecarLayoutTest, SameRootSpelledDifferentlyIsDefault)
{
    SidecarLayout layout("/Volumes/Photos", "/Volumes/Photos/");
    EXPECT_TRUE(layout.isDefaultRoot());
    EXPECT_TRUE(SidecarLayout::isSidecarName("x.jpg.vurio.json"));
    EXPECT_FALSE(SidecarLayout::isSidecarName(".vurio.json")) << "a bare suffix names no asset";
    EXPECT_FALSE(SidecarLayout::isSidecarName("x.json"));
    EXPECT_TRUE(SidecarLayout::isUnder("/a/b", "/a"));
    EXPECT_FALSE(SidecarLayout::isUnder("/ab", "/a"));
    EXPECT_TRUE(SidecarLayout::isUnder("/a", "/"));
}
