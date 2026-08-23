#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include "search/exif_keyword_config.hpp"
#include "search/llm_query_structurer.hpp"
#include "search/llm_runtime.hpp"
#include "search/query_fact_schema.hpp"

using namespace MediaDedup;

namespace
{
    LlmQueryStructurer makeStructurer()
    {
        return LlmQueryStructurer(
            QueryFactSchema::fromKeywords(ExifKeywordConfig::defaults()));
    }

    LlmQueryStructurer::CompleteFn canned(const std::string &text)
    {
        return [text](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = text;
            return c;
        };
    }
}

// --- Registry / schema ------------------------------------------------------

TEST(QueryFactSchemaTest, RegistryCoversAllDimensions)
{
    auto s = QueryFactSchema::fromKeywords(ExifKeywordConfig::defaults());
    const char *expected[] = {"camera", "iso",  "aperture", "shutter",  "focal",
                              "flash",  "date", "location", "file_type"};
    for (const char *name : expected)
        EXPECT_NE(s.find(name), nullptr) << name;
    EXPECT_EQ(s.find("person"), nullptr) << "person joins in QP-2, not before";
    EXPECT_EQ(s.dimensions().size(), 9u);
}

TEST(QueryFactSchemaTest, HintsComeFromTheKeywordConfig)
{
    auto kw = ExifKeywordConfig::defaults();
    kw.camera_brands = {"zenit"};
    kw.file_types = {{"sidecar", {"xmp"}}};
    auto s = QueryFactSchema::fromKeywords(kw);
    ASSERT_NE(s.find("camera"), nullptr);
    EXPECT_EQ(s.find("camera")->hints, std::vector<std::string>{"zenit"});
    ASSERT_NE(s.find("file_type"), nullptr);
    EXPECT_EQ(s.find("file_type")->hints, std::vector<std::string>{"sidecar"});

    // ...and flow into the prompt.
    LlmQueryStructurer st{s};
    EXPECT_NE(st.systemPrompt().find("zenit"), std::string::npos);
    EXPECT_NE(st.systemPrompt().find("sidecar"), std::string::npos);
}

TEST(QueryFactSchemaTest, GrammarIsGeneratedFromTheRegistry)
{
    auto s = QueryFactSchema::fromKeywords(ExifKeywordConfig::defaults());
    auto g = s.gbnf();
    // Has a root rule (the LlmRuntime pre-check requires one).
    EXPECT_NE(g.find("root ::="), std::string::npos);
    // Every dimension name appears as a quoted literal.
    for (const auto &d : s.dimensions())
        EXPECT_NE(g.find("\\\"" + d.name + "\\\""), std::string::npos) << d.name;
    // Intent literals present.
    EXPECT_NE(g.find("\\\"filter\\\""), std::string::npos);
    EXPECT_NE(g.find("\\\"explore\\\""), std::string::npos);
}

TEST(QueryFactSchemaTest, PromptIsDeterministic)
{
    // The LlmRuntime KV prefix cache depends on a byte-stable system prompt.
    auto a = makeStructurer();
    auto b = makeStructurer();
    EXPECT_EQ(a.systemPrompt(), b.systemPrompt());
    EXPECT_EQ(a.grammar(), b.grammar());
}

// --- Structurer parsing (hermetic, canned completions) ----------------------

TEST(LlmQueryStructurerTest, ParsesWellFormedOutput)
{
    auto st = makeStructurer();
    auto out = st.structure(
        "flowers taken with a canon between 2005 and 2015",
        canned(R"({"facts": [{"dim": "camera", "op": "in", "values": ["canon"], )"
               R"("intent": "filter", "span": "taken with a canon"}, )"
               R"({"dim": "date", "op": "range", "values": ["2005", "2015"], )"
               R"("intent": "filter", "span": "between 2005 and 2015"}], )"
               R"("clip_queries": ["flowers"]})"));
    ASSERT_TRUE(out.ok) << out.error;
    ASSERT_EQ(out.facts.size(), 2u);
    EXPECT_EQ(out.facts[0].dim, "camera");
    EXPECT_EQ(out.facts[0].op, "in");
    EXPECT_EQ(out.facts[0].values, std::vector<std::string>{"canon"});
    EXPECT_EQ(out.facts[0].intent, "filter");
    EXPECT_EQ(out.facts[0].span, "taken with a canon");
    EXPECT_EQ(out.facts[1].dim, "date");
    EXPECT_EQ(out.facts[1].values, (std::vector<std::string>{"2005", "2015"}));
    EXPECT_EQ(out.clip_queries, std::vector<std::string>{"flowers"});
}

TEST(LlmQueryStructurerTest, EmptyFactsAndQueriesParse)
{
    auto st = makeStructurer();
    auto out = st.structure("x", canned(R"({"facts": [], "clip_queries": []})"));
    ASSERT_TRUE(out.ok) << out.error;
    EXPECT_TRUE(out.facts.empty());
    EXPECT_TRUE(out.clip_queries.empty());
}

TEST(LlmQueryStructurerTest, CompletionFailurePropagates)
{
    auto st = makeStructurer();
    auto out = st.structure("x", [](const std::string &, const std::string &,
                                    const std::string &) {
        LlmQueryStructurer::Completion c;
        c.error = "timeout";
        return c;
    });
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, "timeout");
}

TEST(LlmQueryStructurerTest, TruncatedOutputFailsCleanly)
{
    // Realistic failure: max_output_tokens hit mid-object.
    auto st = makeStructurer();
    auto out = st.structure("x", canned(R"({"facts": [{"dim": "came)"));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, "unparseable_output");
}

TEST(LlmQueryStructurerTest, StructurerReceivesItsOwnPromptAndGrammar)
{
    auto st = makeStructurer();
    std::string seen_system, seen_user, seen_grammar;
    auto out = st.structure("f2.5", [&](const std::string &sys, const std::string &user,
                                        const std::string &grammar) {
        seen_system = sys;
        seen_user = user;
        seen_grammar = grammar;
        LlmQueryStructurer::Completion c;
        c.ok = true;
        c.text = R"({"facts": [], "clip_queries": []})";
        return c;
    });
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(seen_system, st.systemPrompt());
    EXPECT_EQ(seen_user, "Query: f2.5");
    EXPECT_EQ(seen_grammar, st.grammar());
}

TEST(LlmQueryStructurerTest, FewShotExamplesConformToTheGrammarShape)
{
    // Every example output embedded in the prompt must parse under the same
    // contract structure() expects — a drifted example would teach the model
    // a shape the parser rejects.
    auto st = makeStructurer();
    const std::string &prompt = st.systemPrompt();
    std::size_t pos = 0;
    int examples = 0;
    while ((pos = prompt.find("Output: ", pos)) != std::string::npos)
    {
        pos += 8;
        std::size_t end = prompt.find('\n', pos);
        std::string json = prompt.substr(pos, end - pos);
        auto out = st.structure("x", canned(json));
        EXPECT_TRUE(out.ok) << "example does not parse: " << json;
        ++examples;
    }
    EXPECT_GE(examples, 8);
}

// --- Model-gated grammar smoke ----------------------------------------------
// The strongest property of the design: the GRAMMAR guarantees parseable
// output even from a model that knows nothing about the task. llama.cpp's
// CI storybook model under the generated grammar must yield JSON that
// structure() accepts. This also proves the generated GBNF is valid — an
// unparseable grammar would decode unconstrained prose (or be refused).

TEST(LlmQueryStructurerTest, SmokeGeneratedGrammarForcesParseableOutput)
{
    const char *model = std::getenv("VURIO_TEST_GGUF");
    if (!model || !std::filesystem::exists(model))
        GTEST_SKIP() << "set VURIO_TEST_GGUF to run";

    LlmRuntime::Options opts;
    opts.model_path = model;
    // The CI storybook model has a 512-token vocab, so the prompt
    // tokenizes far bigger than with a real tokenizer — give it room.
    opts.context_tokens = 8192;
    opts.max_output_tokens = 1024;
    opts.timeout_ms = 120000;
    LlmRuntime llm(opts);
    ASSERT_TRUE(llm.available());

    auto st = makeStructurer();
    auto out = st.structure("sunset taken with a canon", llm);
    ASSERT_TRUE(out.ok) << out.error
                        << " (grammar failed to constrain the decode)";
    // No claim about CONTENT from a storybook model — only structure: every
    // fact carries schema-known dims/intents, which only the grammar can
    // have enforced.
    for (const auto &f : out.facts)
    {
        EXPECT_NE(st.schema().find(f.dim), nullptr) << f.dim;
        EXPECT_TRUE(f.intent == "filter" || f.intent == "explore") << f.intent;
    }
}

// --- Golden content checks (real instruct model) ----------------------------
// Gated on VURIO_GOLDEN_GGUF — must be a real instruct model (the shipped
// Qwen2.5-1.5B), NOT the storybook smoke model: these assert CONTENT.

TEST(LlmQueryStructurerTest, GoldenStructuresWorkedExamples)
{
    const char *model = std::getenv("VURIO_GOLDEN_GGUF");
    if (!model || !std::filesystem::exists(model))
        GTEST_SKIP() << "set VURIO_GOLDEN_GGUF to a real instruct model to run";

    LlmRuntime::Options opts;
    opts.model_path = model;
    opts.context_tokens = 4096;
    opts.max_output_tokens = 512;
    opts.timeout_ms = 60000;
    LlmRuntime llm(opts);
    ASSERT_TRUE(llm.available());

    auto st = makeStructurer();

    // Bound camera + semantic subject.
    {
        auto out = st.structure("sunset taken with a canon", llm);
        ASSERT_TRUE(out.ok) << out.error;
        ASSERT_EQ(out.facts.size(), 1u);
        EXPECT_EQ(out.facts[0].dim, "camera");
        EXPECT_EQ(out.facts[0].values, std::vector<std::string>{"canon"});
        EXPECT_EQ(out.facts[0].intent, "filter");
        ASSERT_EQ(out.clip_queries.size(), 1u);
        EXPECT_EQ(out.clip_queries[0], "sunset");
    }

    // The full difficult query: three bound facts, no semantic text.
    {
        auto out = st.structure(
            "f2.5 to f12 but taken with fuji or canon between 2005 and 2015", llm);
        ASSERT_TRUE(out.ok) << out.error;
        ASSERT_EQ(out.facts.size(), 3u);
        EXPECT_EQ(out.facts[0].dim, "aperture");
        EXPECT_EQ(out.facts[0].op, "range");
        EXPECT_EQ(out.facts[0].values, (std::vector<std::string>{"2.5", "12"}));
        EXPECT_EQ(out.facts[1].dim, "camera");
        EXPECT_EQ(out.facts[1].values, (std::vector<std::string>{"fuji", "canon"}));
        EXPECT_EQ(out.facts[2].dim, "date");
        EXPECT_EQ(out.facts[2].op, "range");
        EXPECT_EQ(out.facts[2].values, (std::vector<std::string>{"2005", "2015"}));
        for (const auto &f : out.facts)
            EXPECT_EQ(f.intent, "filter") << f.dim;
        EXPECT_TRUE(out.clip_queries.empty());
    }

    // Loose keyword list: explore intent, semantics kept.
    {
        auto out = st.structure("raw, sunset, fuji", llm);
        ASSERT_TRUE(out.ok) << out.error;
        bool saw_raw = false, saw_fuji = false;
        for (const auto &f : out.facts)
        {
            if (f.dim == "file_type")
            {
                saw_raw = true;
                EXPECT_EQ(f.intent, "explore");
            }
            if (f.dim == "camera")
            {
                saw_fuji = true;
                EXPECT_EQ(f.intent, "explore");
            }
        }
        EXPECT_TRUE(saw_raw);
        EXPECT_TRUE(saw_fuji);
        bool clip_has_sunset = false;
        for (const auto &q : out.clip_queries)
            if (q.find("sunset") != std::string::npos)
                clip_has_sunset = true;
        EXPECT_TRUE(clip_has_sunset);
    }
}
