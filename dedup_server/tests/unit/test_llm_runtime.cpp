#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "config/unified_observable_config.hpp"
#include "search/llm_runtime.hpp"

using namespace MediaDedup;

// QP-1b: hermetic tests — no model file is ever loaded. Behavior with a
// real GGUF is covered by the model-gated golden tests in QP-1c/e.

TEST(LlmRuntimeTest, MissingModelIsUnavailableAndFailsFast)
{
    LlmRuntime::Options opts;
    opts.model_path = "/nonexistent/model.gguf";
    LlmRuntime llm(opts);

    EXPECT_FALSE(llm.available());

    auto c = llm.complete("system", "user");
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(c.error, "model_unavailable");
    EXPECT_TRUE(c.text.empty());

    // Failure is remembered — no reload attempt storm.
    EXPECT_FALSE(llm.available());
}

TEST(LlmRuntimeTest, GarbageModelFileFailsToLoadCleanly)
{
    auto path = (std::filesystem::temp_directory_path() / "not_a_model.gguf").string();
    std::ofstream(path) << "this is not a gguf file";

    LlmRuntime::Options opts;
    opts.model_path = path;
    LlmRuntime llm(opts);

    EXPECT_FALSE(llm.available());
    auto c = llm.complete("system", "user");
    EXPECT_FALSE(c.ok);
    std::remove(path.c_str());
}

TEST(LlmRuntimeTest, OptionsFromConfigReadsKeysAndResolvesRelativePaths)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("", false);
    config->createProperty<std::string>("search.llm.modelPath",
                                        std::string("/abs/model.gguf"), "test");
    config->createProperty<int>("search.llm.timeoutMs", 1234, "test");
    config->createProperty<int>("search.llm.stallTimeoutMs", 4321, "test");

    auto o = LlmRuntime::optionsFromConfig(config);
    EXPECT_EQ(o.model_path, "/abs/model.gguf"); // absolute: no root rewriting
    EXPECT_EQ(o.timeout_ms, 1234);
    EXPECT_EQ(o.stall_ms, 4321);
    EXPECT_EQ(o.context_tokens, 4096);   // defaults survive
    EXPECT_EQ(o.max_output_tokens, 512);

    EXPECT_TRUE(LlmRuntime::enabledInConfig(config)); // default on
}

// The default budget model is hardware-agnostic: no total wall-clock cap
// (a number tuned on one machine is a permanent regex-fallback on a
// slower one), only the progress guard.
TEST(LlmRuntimeTest, DefaultBudgetIsProgressGuardedNotWallClock)
{
    LlmRuntime::Options o;
    EXPECT_EQ(o.timeout_ms, 0);
    EXPECT_GT(o.stall_ms, 0);
}

TEST(LlmRuntimeTest, OptionsFromConfigResolvesRelativeModelPath)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("", false);
    config->createProperty<std::string>("search.llm.modelPath",
                                        std::string("models/x.gguf"), "test");
    auto o = LlmRuntime::optionsFromConfig(config);
    EXPECT_TRUE(std::filesystem::path(o.model_path).is_absolute())
        << "relative paths must be resolved against the project root: "
        << o.model_path;
    EXPECT_NE(o.model_path.find("models"), std::string::npos);
}

TEST(LlmRuntimeTest, EnabledFlagCanBeTurnedOff)
{
    auto config = std::make_shared<UnifiedObservableConfigManager>("", false);
    config->createProperty<bool>("search.llm.enabled", false, "test");
    EXPECT_FALSE(LlmRuntime::enabledInConfig(config));
}

// --- Model-gated smoke tests -----------------------------------------------
// Run only when VURIO_TEST_GGUF points at a real GGUF (any tiny model works;
// llama.cpp's CI stories260K exercises every code path). Hermetic CI skips.

namespace
{
    std::string testModelPath()
    {
        const char *p = std::getenv("VURIO_TEST_GGUF");
        return p ? std::string(p) : std::string();
    }
}

TEST(LlmRuntimeTest, SmokeGrammarConstrainedGreedyDecode)
{
    const auto model = testModelPath();
    if (model.empty() || !std::filesystem::exists(model))
        GTEST_SKIP() << "set VURIO_TEST_GGUF to run";

    LlmRuntime::Options opts;
    opts.model_path = model;
    opts.context_tokens = 512;
    opts.max_output_tokens = 32;
    opts.timeout_ms = 30000;
    LlmRuntime llm(opts);
    ASSERT_TRUE(llm.available());

    // The grammar, not the model, guarantees the shape — exactly the QP-1
    // contract. A storybook model under this grammar must still emit it.
    const std::string grammar = R"(root ::= "{\"ok\": " ("true" | "false") "}")";
    auto c = llm.complete("You are a test.", "Say something.", grammar);
    ASSERT_TRUE(c.ok) << c.error;
    EXPECT_TRUE(c.text == "{\"ok\": true}" || c.text == "{\"ok\": false}")
        << "got: " << c.text;

    // Determinism + prefix reuse: identical call, identical output.
    auto c2 = llm.complete("You are a test.", "Say something.", grammar);
    ASSERT_TRUE(c2.ok) << c2.error;
    EXPECT_EQ(c.text, c2.text);
}

TEST(LlmRuntimeTest, SmokeBadGrammarIsRejectedNotIgnored)
{
    const auto model = testModelPath();
    if (model.empty() || !std::filesystem::exists(model))
        GTEST_SKIP() << "set VURIO_TEST_GGUF to run";

    LlmRuntime::Options opts;
    opts.model_path = model;
    opts.context_tokens = 512;
    opts.max_output_tokens = 32;
    LlmRuntime llm(opts);
    ASSERT_TRUE(llm.available());

    auto c = llm.complete("s", "u", "this is not ::= valid gbnf ((");
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(c.error, "bad_grammar");
}

TEST(LlmRuntimeTest, SmokeTimeoutAborts)
{
    const auto model = testModelPath();
    if (model.empty() || !std::filesystem::exists(model))
        GTEST_SKIP() << "set VURIO_TEST_GGUF to run";

    LlmRuntime::Options opts;
    opts.model_path = model;
    opts.context_tokens = 512;
    opts.max_output_tokens = 64;
    opts.timeout_ms = 1; // total budget expires before decode starts (0 = no cap)
    LlmRuntime llm(opts);
    ASSERT_TRUE(llm.available());

    auto c = llm.complete("You are a test.", "Tell me a very long story.");
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(c.error, "timeout");

    // The runtime must recover cleanly after an abort.
    opts.timeout_ms = 30000;
    LlmRuntime llm2(opts);
    auto c2 = llm2.complete("You are a test.", "Hi.",
                            R"(root ::= "ok")");
    ASSERT_TRUE(c2.ok) << c2.error;
    EXPECT_EQ(c2.text, "ok");
}
