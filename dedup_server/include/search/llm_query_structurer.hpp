#pragma once

#include <functional>
#include <string>
#include <vector>

#include "search/query_fact_schema.hpp"

namespace MediaDedup
{
    class LlmRuntime;

    /**
     * @brief QP-1c: turns a natural-language query into RAW structured
     * facts via the local LLM.
     *
     * Owns the system prompt (task rules + registry schema + few-shot
     * examples) and the GBNF grammar, both generated from QueryFactSchema —
     * the prompt is deterministic so the LlmRuntime KV prefix cache holds
     * across queries. structure() runs one completion and parses the
     * grammar-guaranteed JSON into RawOutput.
     *
     * TRUST BOUNDARY: RawOutput is the model's CLAIM, not the truth — spans
     * may be hallucinated, values ungrounded, ops wrong for the dim. The
     * QP-1d validator grounds every fact against the query text before
     * anything reaches SQL; nothing here is safe to execute directly.
     *
     * The completion function is injected so tests run hermetically with a
     * canned completer; production wires LlmRuntime::complete.
     */
    class LlmQueryStructurer
    {
    public:
        struct RawFact
        {
            std::string dim;
            std::string op;
            std::vector<std::string> values;
            std::string intent; // "filter" | "explore"
            std::string span;   // model's quote of the source text
        };

        struct RawOutput
        {
            bool ok = false;
            std::string error; // set when !ok
            std::vector<RawFact> facts;
            std::vector<std::string> clip_queries;
        };

        struct Completion
        {
            bool ok = false;
            std::string text;
            std::string error;
        };
        using CompleteFn = std::function<Completion(
            const std::string &system_prompt, const std::string &user_text,
            const std::string &gbnf_grammar)>;

        explicit LlmQueryStructurer(QueryFactSchema schema);

        const std::string &systemPrompt() const { return system_prompt_; }
        const std::string &grammar() const { return grammar_; }
        const QueryFactSchema &schema() const { return schema_; }

        /// One completion + JSON parse. Fails (ok=false) on completion
        /// error or unparseable output — the caller falls back to the
        /// regex parser.
        RawOutput structure(const std::string &query, const CompleteFn &complete) const;

        /// Production convenience: completes through an LlmRuntime.
        RawOutput structure(const std::string &query, LlmRuntime &runtime) const;

    private:
        QueryFactSchema schema_;
        std::string system_prompt_;
        std::string grammar_;
    };
}
