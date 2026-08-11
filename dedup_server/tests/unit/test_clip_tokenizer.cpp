#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

#include "media_processors/text/clip_tokenizer.hpp"

using MediaDedup::ClipTokenizer;

#include "clip_tokenizer_golden.inc"

namespace
{
    // Tests may run from the build dir or the repo root; probe both.
    std::string findModelsDir()
    {
        for (const char *candidate : {"models", "../models", "dedup_server/models"})
        {
            if (std::filesystem::exists(std::string(candidate) + "/vocab.json") &&
                std::filesystem::exists(std::string(candidate) + "/merges.txt"))
                return candidate;
        }
        return "";
    }

    class ClipTokenizerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::string dir = findModelsDir();
            if (dir.empty())
                GTEST_SKIP() << "vocab.json/merges.txt not found (run scripts/export_clip_encoders_onnx.py)";
            ASSERT_TRUE(tokenizer_.load(dir + "/vocab.json", dir + "/merges.txt"));
        }

        ClipTokenizer tokenizer_;
    };
}

TEST_F(ClipTokenizerTest, MatchesHuggingFaceGoldenData)
{
    for (const auto &golden : kGoldenCases)
    {
        auto enc = tokenizer_.encode(golden.text);
        EXPECT_EQ(enc.input_ids, golden.input_ids) << "text: \"" << golden.text << "\"";
        EXPECT_EQ(enc.attention_mask, golden.attention_mask) << "text: \"" << golden.text << "\"";
    }
}

TEST_F(ClipTokenizerTest, AlwaysProducesContextLengthOutput)
{
    for (const std::string &text : std::vector<std::string>{"", "dog", std::string(4000, 'a'), "!!!???"})
    {
        auto enc = tokenizer_.encode(text);
        ASSERT_EQ(enc.input_ids.size(), static_cast<size_t>(ClipTokenizer::kContextLength));
        ASSERT_EQ(enc.attention_mask.size(), static_cast<size_t>(ClipTokenizer::kContextLength));
        EXPECT_EQ(enc.input_ids.front(), ClipTokenizer::kBosToken);
        // EOS present at the end of the real tokens
        size_t real = 0;
        for (auto m : enc.attention_mask)
            real += static_cast<size_t>(m);
        ASSERT_GE(real, 2u);
        EXPECT_EQ(enc.input_ids[real - 1], ClipTokenizer::kEosToken);
    }
}

TEST(ClipTokenizerUnloadedTest, LoadFailsOnMissingFiles)
{
    ClipTokenizer t;
    EXPECT_FALSE(t.load("/nonexistent/vocab.json", "/nonexistent/merges.txt"));
    EXPECT_FALSE(t.isLoaded());
}
