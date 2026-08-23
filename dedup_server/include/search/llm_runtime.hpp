#pragma once

#include <memory>
#include <mutex>
#include <string>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;

    /**
     * @brief QP-1b: thin, thread-safe wrapper around a local llama.cpp model.
     *
     * Runs greedy (temperature-0, deterministic) chat completions against a
     * quantized GGUF instruct model, optionally constrained by a GBNF
     * grammar so structurally invalid output is impossible at the sampler.
     * This is the runtime HALF of the QP-1 query structurer — it knows
     * nothing about prompts or fact schemas (QP-1c) or validation (QP-1d).
     *
     * Behavior:
     * - Lazy: nothing is loaded until the first available()/complete()
     *   call. A missing or unloadable model file is logged once and every
     *   later call fails fast — the caller falls back to the regex parser,
     *   exactly like a missing CLIP model degrades search today.
     * - Serialized: one llama context guarded by a mutex; llama contexts
     *   are not thread-safe. The engine's MRU query cache sits in front.
     * - Prefix reuse: the KV cache keeps the longest common token prefix
     *   with the previous call, so a stable system prompt is prefilled
     *   once and per-query cost is the user text plus the JSON decode.
     * - Timeout: opts.timeout_ms bounds each complete() via llama.cpp's
     *   abort callback; a timed-out call reports an error (partial JSON is
     *   useless to the validator).
     *
     * Prompt formatting uses the model's own chat template when llama.cpp
     * recognizes it, falling back to ChatML (the default Qwen model's
     * native format).
     */
    class LlmRuntime
    {
    public:
        struct Options
        {
            std::string model_path;
            int context_tokens = 4096;
            int max_output_tokens = 512;
            // Total wall-clock budget per complete(); 0 (default) = none.
            // Total cost is already bounded by construction (prompt tokens
            // + grammar-bounded output) and scales with the hardware — a
            // fixed total cap tuned on one machine becomes a permanent
            // regex-fallback on a slower one. Set >0 only to deliberately
            // bound search latency at the cost of losing the LLM parse.
            int timeout_ms = 0;
            // Progress guard: abort when no llama_decode completes within
            // this window. Hardware-agnostic — it bounds the time per
            // 128-token prefill batch / per generated token, not the total
            // — so it trips on wedges (driver hang, runaway decode), never
            // on a merely slow machine. 0 disables.
            int stall_ms = 10000;
        };

        struct Completion
        {
            bool ok = false;
            std::string text;  // raw model output (no chat scaffolding)
            std::string error; // set when !ok: model_unavailable, timeout, ...
        };

        /// Reads search.llm.modelPath / contextTokens / maxOutputTokens /
        /// timeoutMs, resolving a relative model path against the project
        /// root the same way the CLIP model path is resolved.
        static Options optionsFromConfig(
            const std::shared_ptr<UnifiedObservableConfigManager> &config);

        /// search.llm.enabled (default true). Read by the caller that
        /// decides whether to construct a runtime at all.
        static bool enabledInConfig(
            const std::shared_ptr<UnifiedObservableConfigManager> &config);

        explicit LlmRuntime(Options opts);
        ~LlmRuntime();
        LlmRuntime(const LlmRuntime &) = delete;
        LlmRuntime &operator=(const LlmRuntime &) = delete;

        /// True when the model is loaded (loads on first call). Failure is
        /// remembered — no retry storm on a missing file.
        bool available();

        /// Greedy chat completion of (system, user); gbnf_grammar (root
        /// rule "root"), when non-empty, constrains every sampled token.
        Completion complete(const std::string &system_prompt,
                            const std::string &user_text,
                            const std::string &gbnf_grammar = "");

    private:
        struct Impl;
        bool ensureLoadedLocked();

        Options opts_;
        std::mutex mutex_;
        std::unique_ptr<Impl> impl_;
        bool load_failed_ = false;
    };
}
