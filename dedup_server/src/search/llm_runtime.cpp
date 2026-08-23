#include "search/llm_runtime.hpp"

#include "config/unified_observable_config.hpp"

#include <llama.h>

#include <Poco/Logger.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <regex>
#include <vector>

namespace MediaDedup
{
    namespace
    {
        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // ChatML — the native format of the default Qwen instruct model and
        // the fallback when llama.cpp doesn't recognize the GGUF's template.
        std::string chatMlPrompt(const std::string &system, const std::string &user)
        {
            return "<|im_start|>system\n" + system + "<|im_end|>\n" +
                   "<|im_start|>user\n" + user + "<|im_end|>\n" +
                   "<|im_start|>assistant\n";
        }
    }

    struct LlmRuntime::Impl
    {
        llama_model *model = nullptr;
        llama_context *ctx = nullptr;
        const llama_vocab *vocab = nullptr;
        // Tokens currently held in the KV cache (prompt + generated). The
        // next call reuses the longest common prefix with its own prompt.
        std::vector<llama_token> evaluated;
        // steady-clock deadline for the in-flight call; 0 = none. Checked
        // by llama's abort callback during decode.
        std::atomic<int64_t> deadline_ms{0};

        ~Impl()
        {
            if (ctx)
                llama_free(ctx);
            if (model)
                llama_model_free(model);
        }

        static bool abortCallback(void *data)
        {
            auto *impl = static_cast<Impl *>(data);
            const int64_t d = impl->deadline_ms.load(std::memory_order_relaxed);
            return d != 0 && nowMs() >= d;
        }

        std::string tokenToPiece(llama_token tok) const
        {
            char buf[256];
            int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, false);
            if (n < 0)
                return "";
            return std::string(buf, n);
        }
    };

    LlmRuntime::Options LlmRuntime::optionsFromConfig(
        const std::shared_ptr<UnifiedObservableConfigManager> &config)
    {
        Options o;
        o.model_path = "models/qwen2.5-1.5b-instruct-q4_k_m.gguf";
        if (!config)
            return o;
        o.model_path = config->getPropertyValue<std::string>("search.llm.modelPath", o.model_path);
        o.context_tokens = config->getPropertyValue<int>("search.llm.contextTokens", o.context_tokens);
        o.max_output_tokens =
            config->getPropertyValue<int>("search.llm.maxOutputTokens", o.max_output_tokens);
        o.timeout_ms = config->getPropertyValue<int>("search.llm.timeoutMs", o.timeout_ms);

        // Same project-root resolution as the CLIP model path, so both
        // resolve identically from any CWD.
        std::filesystem::path model_p(o.model_path);
        if (!model_p.is_absolute())
        {
            std::filesystem::path config_path(config->getConfigFilePath());
            std::filesystem::path abs_config = std::filesystem::absolute(config_path);
            std::filesystem::path project_root = abs_config.parent_path().parent_path();
            o.model_path = (project_root / model_p).lexically_normal().string();
        }
        return o;
    }

    bool LlmRuntime::enabledInConfig(
        const std::shared_ptr<UnifiedObservableConfigManager> &config)
    {
        return !config || config->getPropertyValue<bool>("search.llm.enabled", true);
    }

    LlmRuntime::LlmRuntime(Options opts) : opts_(std::move(opts)) {}

    LlmRuntime::~LlmRuntime() = default;

    bool LlmRuntime::ensureLoadedLocked()
    {
        if (impl_)
            return true;
        if (load_failed_)
            return false;

        Poco::Logger &logger = Poco::Logger::get("LlmRuntime");
        load_failed_ = true; // cleared only on full success

        if (!std::filesystem::exists(opts_.model_path))
        {
            logger.warning("LLM model not found (%s); query structuring falls back "
                           "to the pattern parser",
                           opts_.model_path);
            return false;
        }

        static std::once_flag backend_once;
        std::call_once(backend_once, [] { llama_backend_init(); });

        auto impl = std::make_unique<Impl>();

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = 99; // small model: offload everything to Metal
        impl->model = llama_model_load_from_file(opts_.model_path.c_str(), mparams);
        if (!impl->model)
        {
            logger.error("Failed to load LLM model %s", opts_.model_path);
            return false;
        }

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = static_cast<uint32_t>(opts_.context_tokens);
        cparams.n_batch = 512;
        cparams.abort_callback = &Impl::abortCallback;
        cparams.abort_callback_data = impl.get();
        impl->ctx = llama_init_from_model(impl->model, cparams);
        if (!impl->ctx)
        {
            logger.error("Failed to create LLM context for %s", opts_.model_path);
            return false;
        }
        impl->vocab = llama_model_get_vocab(impl->model);

        logger.information("LLM runtime loaded: %s (ctx=%d)", opts_.model_path,
                           opts_.context_tokens);
        impl_ = std::move(impl);
        load_failed_ = false;
        return true;
    }

    bool LlmRuntime::available()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensureLoadedLocked();
    }

    LlmRuntime::Completion LlmRuntime::complete(const std::string &system_prompt,
                                                const std::string &user_text,
                                                const std::string &gbnf_grammar)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Completion result;
        if (!ensureLoadedLocked())
        {
            result.error = "model_unavailable";
            return result;
        }
        Poco::Logger &logger = Poco::Logger::get("LlmRuntime");
        Impl &impl = *impl_;

        // --- Prompt via the model's chat template (ChatML fallback) -----
        std::string prompt;
        {
            std::string tmpl;
            int32_t need = llama_model_meta_val_str(impl.model, "tokenizer.chat_template",
                                                    nullptr, 0);
            if (need > 0)
            {
                tmpl.resize(static_cast<std::size_t>(need) + 1);
                llama_model_meta_val_str(impl.model, "tokenizer.chat_template",
                                         tmpl.data(), tmpl.size());
                tmpl.resize(std::strlen(tmpl.c_str()));
            }
            if (!tmpl.empty())
            {
                llama_chat_message msgs[2] = {{"system", system_prompt.c_str()},
                                              {"user", user_text.c_str()}};
                std::vector<char> buf(system_prompt.size() + user_text.size() + 1024);
                int32_t n = llama_chat_apply_template(tmpl.c_str(), msgs, 2, true,
                                                      buf.data(),
                                                      static_cast<int32_t>(buf.size()));
                if (n > static_cast<int32_t>(buf.size()))
                {
                    buf.resize(static_cast<std::size_t>(n));
                    n = llama_chat_apply_template(tmpl.c_str(), msgs, 2, true, buf.data(),
                                                  static_cast<int32_t>(buf.size()));
                }
                if (n > 0)
                    prompt.assign(buf.data(), static_cast<std::size_t>(n));
            }
            if (prompt.empty())
                prompt = chatMlPrompt(system_prompt, user_text);
        }

        // --- Tokenize ---------------------------------------------------
        std::vector<llama_token> tokens(prompt.size() + 16);
        int n_tok = llama_tokenize(impl.vocab, prompt.c_str(),
                                   static_cast<int32_t>(prompt.size()), tokens.data(),
                                   static_cast<int32_t>(tokens.size()),
                                   /*add_special=*/true, /*parse_special=*/true);
        if (n_tok < 0)
        {
            tokens.resize(static_cast<std::size_t>(-n_tok));
            n_tok = llama_tokenize(impl.vocab, prompt.c_str(),
                                   static_cast<int32_t>(prompt.size()), tokens.data(),
                                   static_cast<int32_t>(tokens.size()), true, true);
        }
        if (n_tok < 0)
        {
            result.error = "tokenization_failed";
            return result;
        }
        tokens.resize(static_cast<std::size_t>(n_tok));

        if (static_cast<int>(tokens.size()) + opts_.max_output_tokens >= opts_.context_tokens)
        {
            result.error = "prompt_too_long";
            return result;
        }

        // --- KV prefix reuse --------------------------------------------
        // Keep the longest common prefix with whatever the cache holds; a
        // stable system prompt is therefore prefilled exactly once.
        std::size_t common = 0;
        while (common < tokens.size() && common < impl.evaluated.size() &&
               tokens[common] == impl.evaluated[common])
            ++common;
        if (common == tokens.size())
            --common; // re-decode the last token so its logits are fresh
        llama_kv_cache_seq_rm(impl.ctx, 0, static_cast<llama_pos>(common), -1);
        impl.evaluated.resize(common);

        impl.deadline_ms.store(nowMs() + opts_.timeout_ms, std::memory_order_relaxed);
        auto finish = [&](const char *err) {
            impl.deadline_ms.store(0, std::memory_order_relaxed);
            if (err)
            {
                // Only the failed batch's KV entries are suspect —
                // `evaluated` holds exactly the tokens whose llama_decode
                // SUCCEEDED, so trim to that boundary instead of wiping
                // the cache. This keeps the warm system-prompt prefix
                // across a timeout (a full wipe made every subsequent
                // call re-pay the whole prefill inside its own deadline:
                // one slow query became a permanent timeout storm), and
                // it makes prefill resumable — even a too-short deadline
                // advances the cache a few batches per attempt.
                llama_kv_cache_seq_rm(impl.ctx, 0,
                                      static_cast<llama_pos>(impl.evaluated.size()), -1);
                result.ok = false;
                result.error = err;
            }
            return result;
        };
        auto timedOut = [&] {
            const int64_t d = impl.deadline_ms.load(std::memory_order_relaxed);
            return d != 0 && nowMs() >= d;
        };

        // --- Prefill ----------------------------------------------------
        const int n_batch = 512;
        for (std::size_t i = common; i < tokens.size(); i += n_batch)
        {
            const int n_eval =
                static_cast<int>(std::min<std::size_t>(n_batch, tokens.size() - i));
            llama_batch batch = llama_batch_get_one(tokens.data() + i, n_eval);
            if (llama_decode(impl.ctx, batch) != 0)
                return finish(timedOut() ? "timeout" : "decode_failed");
            impl.evaluated.insert(impl.evaluated.end(), tokens.begin() + i,
                                  tokens.begin() + i + n_eval);
        }

        // --- Sampler: optional grammar, then greedy ---------------------
        // llama.cpp silently degrades an unparseable grammar to a no-op
        // sampler (unconstrained decode) — the one failure mode QP-1 must
        // not have. The grammar is machine-generated (QP-1c), so the
        // realistic bug is a missing/mangled root rule: pre-check for it
        // and refuse; the QP-1d validator is the net for anything subtler.
        llama_sampler *chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
        if (!gbnf_grammar.empty())
        {
            static const std::regex root_rule(R"(\broot\s*::=)");
            if (!std::regex_search(gbnf_grammar, root_rule))
            {
                llama_sampler_free(chain);
                logger.error("GBNF grammar has no root rule; refusing unconstrained decode");
                return finish("bad_grammar");
            }
            llama_sampler *grammar =
                llama_sampler_init_grammar(impl.vocab, gbnf_grammar.c_str(), "root");
            if (!grammar)
            {
                llama_sampler_free(chain);
                logger.error("Rejected GBNF grammar; refusing unconstrained decode");
                return finish("bad_grammar");
            }
            llama_sampler_chain_add(chain, grammar);
        }
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());

        // --- Decode -----------------------------------------------------
        for (int i = 0; i < opts_.max_output_tokens; ++i)
        {
            llama_token tok = llama_sampler_sample(chain, impl.ctx, -1);
            if (llama_vocab_is_eog(impl.vocab, tok))
                break;
            result.text += impl.tokenToPiece(tok);
            llama_batch batch = llama_batch_get_one(&tok, 1);
            if (llama_decode(impl.ctx, batch) != 0)
            {
                llama_sampler_free(chain);
                return finish(timedOut() ? "timeout" : "decode_failed");
            }
            impl.evaluated.push_back(tok);
        }
        llama_sampler_free(chain);

        result.ok = true;
        return finish(nullptr);
    }
}
