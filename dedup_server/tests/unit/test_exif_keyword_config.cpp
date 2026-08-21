#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "search/exif_keyword_config.hpp"
#include "search/exif_query_parser.hpp"

using namespace MediaDedup;

namespace
{
    std::string writeTemp(const std::string &name, const std::string &content)
    {
        auto path = (std::filesystem::temp_directory_path() / name).string();
        std::ofstream(path) << content;
        return path;
    }
}

TEST(ExifKeywordConfigTest, DefaultsAreComplete)
{
    auto c = ExifKeywordConfig::defaults();
    EXPECT_EQ(c.months.size(), 12u);
    EXPECT_FALSE(c.camera_brands.empty());
    EXPECT_FALSE(c.greater_words.empty());
    EXPECT_FALSE(c.less_words.empty());
    EXPECT_FALSE(c.flash_on_phrases.empty());
    EXPECT_FALSE(c.flash_off_phrases.empty());
    EXPECT_FALSE(c.location_prepositions.empty());
    EXPECT_FALSE(c.filler_words.empty());
}

TEST(ExifKeywordConfigTest, MissingFileLeavesConfigUnchanged)
{
    auto c = ExifKeywordConfig::defaults();
    auto before_brands = c.camera_brands;
    EXPECT_FALSE(c.loadFromFile("/nonexistent/keywords.json"));
    EXPECT_EQ(c.camera_brands, before_brands);
}

TEST(ExifKeywordConfigTest, MalformedJsonLeavesConfigUnchanged)
{
    auto path = writeTemp("kw_malformed.json", "{ not valid json !!");
    auto c = ExifKeywordConfig::defaults();
    auto before_brands = c.camera_brands;
    EXPECT_FALSE(c.loadFromFile(path));
    EXPECT_EQ(c.camera_brands, before_brands);
    std::remove(path.c_str());
}

TEST(ExifKeywordConfigTest, PresentListsReplaceAbsentListsKeepDefaults)
{
    auto path = writeTemp("kw_overlay.json", R"({
        "camera": {"brands": ["Zenit", "praktica"]},
        "comparisons": {"greater": ["exceeding"]}
    })");
    auto c = ExifKeywordConfig::defaults();
    ASSERT_TRUE(c.loadFromFile(path));

    // Present lists replaced wholesale (and lowercased).
    EXPECT_EQ(c.camera_brands, (std::vector<std::string>{"zenit", "praktica"}));
    EXPECT_EQ(c.greater_words, (std::vector<std::string>{"exceeding"}));

    // Absent lists keep their defaults.
    EXPECT_EQ(c.less_words, ExifKeywordConfig::defaults().less_words);
    EXPECT_EQ(c.flash_on_phrases, ExifKeywordConfig::defaults().flash_on_phrases);
    EXPECT_EQ(c.months.size(), 12u);
    std::remove(path.c_str());
}

TEST(ExifKeywordConfigTest, BadMonthCountIsRejectedButRestLoads)
{
    auto path = writeTemp("kw_badmonths.json", R"({
        "dates": {"months": ["jan", "feb"], "prepositions": ["during"]}
    })");
    auto c = ExifKeywordConfig::defaults();
    ASSERT_TRUE(c.loadFromFile(path));
    EXPECT_EQ(c.months.size(), 12u) << "a non-12 month list must not corrupt month numbering";
    EXPECT_EQ(c.month_prepositions, (std::vector<std::string>{"during"}));
    std::remove(path.c_str());
}

TEST(ExifKeywordConfigTest, ShippedKeywordFileMatchesBuiltInDefaults)
{
    // config/exif_search_keywords.json ships the default vocabulary; if it
    // drifts from ExifKeywordConfig::defaults(), behavior would silently
    // depend on whether the file loaded. Keep them in lockstep.
    std::filesystem::path source_file(__FILE__);
    auto shipped = source_file.parent_path().parent_path().parent_path() /
                   "config" / "exif_search_keywords.json";
    ASSERT_TRUE(std::filesystem::exists(shipped)) << shipped;

    auto loaded = ExifKeywordConfig::defaults();
    ASSERT_TRUE(loaded.loadFromFile(shipped.string()));
    auto defaults = ExifKeywordConfig::defaults();

    EXPECT_EQ(loaded.camera_brands, defaults.camera_brands);
    EXPECT_EQ(loaded.camera_context_verbs, defaults.camera_context_verbs);
    EXPECT_EQ(loaded.greater_words, defaults.greater_words);
    EXPECT_EQ(loaded.less_words, defaults.less_words);
    EXPECT_EQ(loaded.aperture_wider_words, defaults.aperture_wider_words);
    EXPECT_EQ(loaded.shutter_faster_words, defaults.shutter_faster_words);
    EXPECT_EQ(loaded.flash_on_phrases, defaults.flash_on_phrases);
    EXPECT_EQ(loaded.flash_off_phrases, defaults.flash_off_phrases);
    EXPECT_EQ(loaded.months, defaults.months);
    EXPECT_EQ(loaded.ambiguous_months, defaults.ambiguous_months);
    EXPECT_EQ(loaded.location_prepositions, defaults.location_prepositions);
    EXPECT_EQ(loaded.imperative_words, defaults.imperative_words);
    EXPECT_EQ(loaded.filler_words, defaults.filler_words);
}

TEST(ExifKeywordConfigTest, ParserPicksUpCustomVocabulary)
{
    // The point of EXIF-6: new words work without touching parser code.
    auto path = writeTemp("kw_custom.json", R"({
        "camera": {"brands": ["zenit"]},
        "comparisons": {"greater": ["exceeding"]}
    })");
    auto kw = ExifKeywordConfig::defaults();
    ASSERT_TRUE(kw.loadFromFile(path));

    ExifQueryParser parser(nullptr, kw);
    auto c = parser.parse("photos taken with a zenit camera iso exceeding 800");
    ASSERT_EQ(c.camera_terms.size(), 1u);
    EXPECT_EQ(c.camera_terms[0], "zenit");
    ASSERT_EQ(c.numeric.size(), 1u);
    EXPECT_EQ(c.numeric[0].column, "exif_iso");
    EXPECT_EQ(c.numeric[0].op, ">");
    EXPECT_DOUBLE_EQ(c.numeric[0].value, 800.0);
    EXPECT_TRUE(c.remaining_is_filler);
    std::remove(path.c_str());
}

TEST(ExifKeywordConfigTest, DefaultParserBehaviorUnchangedByRefactor)
{
    // Golden check: the config-driven parser with defaults must behave
    // exactly like the pre-EXIF-6 hardcoded one.
    ExifQueryParser parser;
    auto c = parser.parse("sunset photos taken with a canon camera iso at least 1600 without flash");
    ASSERT_EQ(c.camera_terms.size(), 1u);
    EXPECT_EQ(c.camera_terms[0], "canon");
    ASSERT_EQ(c.numeric.size(), 1u);
    EXPECT_EQ(c.numeric[0].op, ">=");
    EXPECT_TRUE(c.flash_off);
    EXPECT_FALSE(c.flash_on);
    EXPECT_EQ(c.remaining, "sunset photos");
}
