#pragma once

#include <string>
#include <vector>

namespace MediaDedup
{
    struct ExifKeywordConfig;

    /**
     * @brief QP-1c: the registry of fact dimensions the query structurer
     * understands.
     *
     * One entry per searchable dimension (camera, iso, aperture, shutter,
     * focal, flash, date, location, file_type — `person` joins in QP-2).
     * The registry is the single source of truth that GENERATES:
     * - the schema section of the LLM system prompt (names, ops, hints), and
     * - the GBNF grammar constraining the model's JSON output.
     *
     * Adding a dimension is therefore a registry entry, never a prompt or
     * grammar edit. Hints (camera brands, file-type names) come from the
     * ExifKeywordConfig, so the vocabulary JSON feeds the LLM too.
     *
     * Output contract enforced by the grammar (one line, no extra keys):
     *   {"facts": [{"dim": ..., "op": ..., "values": [str...],
     *               "intent": "filter"|"explore", "span": str}, ...],
     *    "clip_queries": [str...]}
     * Values are ALWAYS strings ("2.5", "1/500", "2024") — the QP-1d
     * validator parses them per dimension.
     */
    struct FactDimension
    {
        std::string name;             // JSON "dim" value
        std::string description;      // one line for the prompt
        std::vector<std::string> ops; // allowed "op" values for this dim
        std::vector<std::string> hints; // known values, prompt-only
    };

    class QueryFactSchema
    {
    public:
        /// The built-in dimensions, with hints drawn from the keyword config.
        static QueryFactSchema fromKeywords(const ExifKeywordConfig &keywords);

        const std::vector<FactDimension> &dimensions() const { return dims_; }
        const FactDimension *find(const std::string &name) const;

        /// Prompt fragment: one line per dimension (name, ops, description,
        /// hints). Deterministic — the LlmRuntime prefix cache depends on a
        /// stable system prompt.
        std::string promptSchemaSection() const;

        /// GBNF grammar for the output contract. Dim and intent literals are
        /// generated per registry; ops are the union across dimensions
        /// (per-dim op enforcement lives in the QP-1d validator, where the
        /// error can be reported instead of silently unparseable).
        std::string gbnf() const;

    private:
        std::vector<FactDimension> dims_;
    };
}
