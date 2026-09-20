#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "search/exif_keyword_config.hpp"
#include "search/llm_fact_validator.hpp"
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
                              "flash",  "date", "location", "file_type",
                              "person", "face_count", "setting"}; // QP-2 (FACE-4), PLACE-4
    for (const char *name : expected)
        EXPECT_NE(s.find(name), nullptr) << name;
    EXPECT_EQ(s.dimensions().size(), 12u);
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

// --- QP-1e: acceptance matrix (EXIF-7 §11) over the REAL model --------------
// Each query runs structurer -> validator; assertions are on the validated
// StructuredQuery — the exact contract the engine executes. Gated on
// VURIO_GOLDEN_GGUF (the shipped Qwen2.5-1.5B).

namespace
{
    class GoldenMatrixTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const char *model = std::getenv("VURIO_GOLDEN_GGUF");
            if (!model || !std::filesystem::exists(model))
                GTEST_SKIP() << "set VURIO_GOLDEN_GGUF to a real instruct model";

            LlmRuntime::Options opts;
            opts.model_path = model;
            opts.context_tokens = 4096;
            opts.max_output_tokens = 512;
            opts.timeout_ms = 60000;
            llm_ = std::make_unique<LlmRuntime>(opts);
            ASSERT_TRUE(llm_->available());

            geo_path_ = (std::filesystem::temp_directory_path() / "golden_geo.geojson").string();
            std::ofstream(geo_path_) << R"({"type": "FeatureCollection", "features": [
                {"type": "Feature", "properties": {"name": "Utah"},
                 "bbox": [-114.03, 36.99, -109.04, 42.00], "geometry": null}]})";
            ASSERT_TRUE(geocoder_.loadFromFile(geo_path_));

            structurer_ = std::make_unique<LlmQueryStructurer>(
                QueryFactSchema::fromKeywords(ExifKeywordConfig::defaults()));
            validator_ = std::make_unique<LlmFactValidator>(
                ExifKeywordConfig::defaults(), &geocoder_,
                // QP-2: the golden library knows one named person.
                [](const std::string &name) -> int {
                    return name == "alice" ? 7 : 0;
                },
                // PLACE-2: the golden library has photos in two places.
                [](const std::string &name) -> std::vector<int> {
                    if (name == "yellowstone")
                        return {1};
                    if (name == "bozeman")
                        return {2};
                    return {};
                });
        }

        void TearDown() override { std::remove(geo_path_.c_str()); }

        // structurer -> validator; asserts the parse itself succeeded.
        StructuredQuery run(const std::string &query)
        {
            auto raw = structurer_->structure(query, *llm_);
            EXPECT_TRUE(raw.ok) << query << ": " << raw.error;
            auto v = validator_->validate(query, raw);
            EXPECT_EQ(v.dropped, 0u) << query << ": validator dropped fact(s)";
            return v.query;
        }

        std::string geo_path_;
        LocationGeocoder geocoder_;
        std::unique_ptr<LlmRuntime> llm_;
        std::unique_ptr<LlmQueryStructurer> structurer_;
        std::unique_ptr<LlmFactValidator> validator_;
    };
}

TEST_F(GoldenMatrixTest, BoundCameraIntersects)
{
    // EXIF-7 §11: "sunset with a canon" -> only canon sunsets.
    auto q = run("sunset with a canon");
    EXPECT_EQ(q.bound.camera_terms, std::vector<std::string>{"canon"});
    EXPECT_TRUE(q.bare.empty());
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_EQ(q.clip_queries[0], "sunset");
}

TEST_F(GoldenMatrixTest, BareProbeExplores)
{
    // EXIF-7 §11: "f2.5" -> superset (explore).
    auto q = run("f2.5");
    EXPECT_FALSE(q.bound.hasAny());
    ASSERT_EQ(q.bare.size(), 1u);
    ASSERT_EQ(q.bare[0].numeric.size(), 1u);
    EXPECT_EQ(q.bare[0].numeric[0].column, "exif_f_number");
}

TEST_F(GoldenMatrixTest, ReversedYearRangeNormalizes)
{
    // EXIF-7 §11: "sunset taken with fuji between 2024 and 2018".
    auto q = run("sunset taken with fuji between 2024 and 2018");
    EXPECT_EQ(q.bound.camera_terms, std::vector<std::string>{"fuji"});
    ASSERT_TRUE(q.bound.date_range.has_value());
    EXPECT_EQ(q.bound.date_range->first, "2018:01:01 00:00:00");
    EXPECT_EQ(q.bound.date_range->second, "2024:12:31 23:59:59");
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_EQ(q.clip_queries[0], "sunset");
}

TEST_F(GoldenMatrixTest, BareBrandListUnions)
{
    // EXIF-7 §11: "canon, nikon" must return results (within-field OR).
    auto q = run("canon, nikon");
    EXPECT_TRUE(q.bound.camera_terms.empty());
    std::vector<std::string> brands;
    for (const auto &b : q.bare)
        for (const auto &t : b.camera_terms)
            brands.push_back(t);
    std::sort(brands.begin(), brands.end());
    EXPECT_EQ(brands, (std::vector<std::string>{"canon", "nikon"}));
}

TEST_F(GoldenMatrixTest, MonthYearAndFlashBind)
{
    auto q = run("photos from august 2024 without flash");
    EXPECT_TRUE(q.bound.flash_off);
    EXPECT_FALSE(q.bound.flash_on);
    ASSERT_TRUE(q.bound.date_range.has_value());
    EXPECT_EQ(q.bound.date_range->first, "2024:08:01 00:00:00");
}

TEST_F(GoldenMatrixTest, LocationAndIsoRangeBind)
{
    auto q = run("photos taken in utah iso between 400 and 1600");
    ASSERT_TRUE(q.bound.location.has_value());
    EXPECT_EQ(q.bound.location_name, "utah");
    ASSERT_EQ(q.bound.numeric.size(), 1u);
    EXPECT_EQ(q.bound.numeric[0].column, "exif_iso");
    EXPECT_EQ(q.bound.numeric[0].op, "between");
    EXPECT_DOUBLE_EQ(q.bound.numeric[0].value, 400.0);
    EXPECT_DOUBLE_EQ(q.bound.numeric[0].value2, 1600.0);
}

TEST_F(GoldenMatrixTest, ScaffoldedFocalBindsWithSemanticSubject)
{
    auto q = run("portraits shot at 85mm");
    ASSERT_EQ(q.bound.numeric.size(), 1u);
    EXPECT_EQ(q.bound.numeric[0].column, "exif_focal_length");
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_NE(q.clip_queries[0].find("portraits"), std::string::npos);
}

TEST_F(GoldenMatrixTest, FullDifficultQueryWithSubject)
{
    // EXIF-7 §11: the "Flowers" variant — same three bound facts plus CLIP.
    auto q = run("Flowers, taken at f2.5 to f12, with fuji or canon, "
                 "between 2005 and 2015");
    ASSERT_EQ(q.bound.numeric.size(), 1u);
    EXPECT_EQ(q.bound.numeric[0].column, "exif_f_number");
    EXPECT_EQ(q.bound.numeric[0].op, "between");
    EXPECT_DOUBLE_EQ(q.bound.numeric[0].value, 2.5);
    EXPECT_DOUBLE_EQ(q.bound.numeric[0].value2, 12.0);
    std::vector<std::string> brands = q.bound.camera_terms;
    std::sort(brands.begin(), brands.end());
    EXPECT_EQ(brands, (std::vector<std::string>{"canon", "fuji"}));
    ASSERT_TRUE(q.bound.date_range.has_value());
    EXPECT_EQ(q.bound.date_range->first, "2005:01:01 00:00:00");
    EXPECT_TRUE(q.bare.empty());
    ASSERT_EQ(q.clip_queries.size(), 1u);
    EXPECT_EQ(q.clip_queries[0], "flowers");
}

TEST_F(GoldenMatrixTest, PersonBindsAndSubjectStaysSemantic)
{
    // QP-2 (FACE-4): "of alice" is a person filter; the subject stays CLIP.
    auto q = run("photos of alice at the beach");
    EXPECT_EQ(q.bound.person_ids, std::vector<int>{7});
    ASSERT_EQ(q.clip_queries.size(), 1u);
    // The validator strips accepted fact spans from clip text, so however
    // the model phrases it, the person's name never reaches CLIP.
    EXPECT_NE(q.clip_queries[0].find("beach"), std::string::npos) << q.clip_queries[0];
    EXPECT_EQ(q.clip_queries[0].find("alice"), std::string::npos) << q.clip_queries[0];
    EXPECT_TRUE(q.bare.empty());
}

TEST_F(GoldenMatrixTest, FaceCountBindsWithDate)
{
    auto q = run("group photos with 3 people from 2020");
    ASSERT_EQ(q.bound.numeric.size(), 1u);
    EXPECT_EQ(q.bound.numeric[0].column, "face_count");
    EXPECT_EQ(q.bound.numeric[0].op, "=");
    EXPECT_DOUBLE_EQ(q.bound.numeric[0].value, 3.0);
    ASSERT_TRUE(q.bound.date_range.has_value());
    EXPECT_EQ(q.bound.date_range->first, "2020:01:01 00:00:00");
    ASSERT_GE(q.clip_queries.size(), 1u);
    EXPECT_EQ(q.clip_queries[0], "group photos");
}

// PLACE-2 §6.1: bare place names — the model decides, the validator gates.
TEST_F(GoldenMatrixTest, BarePlaceNameBecomesAnInferredBoundLocation)
{
    auto q = run("yellowstone waterfalls");
    EXPECT_EQ(q.bound.place_ids, std::vector<int>{1});
    EXPECT_TRUE(q.bound.location_inferred);
    EXPECT_TRUE(q.bare.empty());
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_NE(q.clip_queries[0].find("waterfalls"), std::string::npos) << q.clip_queries[0];
}

TEST_F(GoldenMatrixTest, AdjectiveAndFoodAreNeverPlaces)
{
    // These two are the model's known weak spots (it has volunteered an
    // aperture fact for "nice" and a location for "turkey"); what matters
    // is the VALIDATED query, so drops are allowed here — the assertion is
    // that nothing survives as a filter.
    auto loose = [&](const std::string &query) {
        auto raw = structurer_->structure(query, *llm_);
        EXPECT_TRUE(raw.ok) << query << ": " << raw.error;
        return validator_->validate(query, raw).query;
    };
    auto q = loose("nice sunset");
    EXPECT_FALSE(q.bound.hasAny());
    EXPECT_TRUE(q.bound.place_ids.empty());
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_NE(q.clip_queries[0].find("sunset"), std::string::npos) << q.clip_queries[0];

    // Whatever the model claims about "turkey", no photos there -> no filter.
    auto q2 = loose("turkey dinner");
    EXPECT_FALSE(q2.bound.hasAny());
    ASSERT_FALSE(q2.clip_queries.empty());
    EXPECT_NE(q2.clip_queries[0].find("turkey"), std::string::npos) << q2.clip_queries[0];
}

TEST_F(GoldenMatrixTest, PrepositionedPlaceIsBoundNotInferred)
{
    auto q = run("waterfalls in yellowstone");
    EXPECT_EQ(q.bound.place_ids, std::vector<int>{1});
    EXPECT_FALSE(q.bound.location_inferred);
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_EQ(q.clip_queries[0], "waterfalls");
}

// PLACE-4
TEST(QueryFactSchemaSettingTest, SettingDimensionListsCategoriesAsHints)
{
    auto s = QueryFactSchema::fromKeywords(ExifKeywordConfig::defaults());
    const auto *d = s.find("setting");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->ops, (std::vector<std::string>{"at", "near"}));
    EXPECT_NE(std::find(d->hints.begin(), d->hints.end(), "waterfront"), d->hints.end());
    EXPECT_NE(s.promptSchemaSection().find("waterfront"), std::string::npos);
    EXPECT_NE(s.gbnf().find("setting"), std::string::npos);
}

TEST_F(GoldenMatrixTest, RegionPlusSettingSplitsIntoTwoGeoFacts)
{
    // Whatever the model emits, the validated query must carry the
    // waterfront setting (deterministic detection) and must have seen
    // "new jersey" as a place.
    auto raw = structurer_->structure("new jersey waterfronts", *llm_);
    ASSERT_TRUE(raw.ok) << raw.error;
    auto q = validator_->validate("new jersey waterfronts", raw).query;
    ASSERT_EQ(q.bound.settings.size(), 1u);
    EXPECT_EQ(q.bound.settings[0].category, "waterfront");
    EXPECT_DOUBLE_EQ(q.bound.settings[0].max_m, 150.0);
    bool saw_location = false;
    for (const auto &f : raw.facts)
        saw_location = saw_location || (f.dim == "location");
    EXPECT_TRUE(saw_location);
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_NE(q.clip_queries[0].find("waterfront"), std::string::npos) << q.clip_queries[0];
}

TEST_F(GoldenMatrixTest, NearTheLakeIsANearSetting)
{
    auto q = run("sunsets near the lake");
    ASSERT_EQ(q.bound.settings.size(), 1u);
    EXPECT_EQ(q.bound.settings[0].category, "lake");
    EXPECT_DOUBLE_EQ(q.bound.settings[0].max_m, 1500.0);
    EXPECT_FALSE(q.bound.settings[0].inferred);
    EXPECT_FALSE(q.bound.location.has_value()) << "'the lake' is never Salt Lake City";
    ASSERT_FALSE(q.clip_queries.empty());
    EXPECT_NE(q.clip_queries[0].find("sunset"), std::string::npos) << q.clip_queries[0];
}
