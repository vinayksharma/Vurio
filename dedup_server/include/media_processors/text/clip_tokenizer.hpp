#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief CLIP byte-pair-encoding tokenizer (openai/clip-vit-base-patch32).
     *
     * Reproduces the HuggingFace CLIPTokenizer pipeline used to build the
     * text-encoder golden data: whitespace cleanup + lowercasing, the CLIP
     * lexer regex (contractions / letter runs / single digits / symbol runs),
     * GPT-2 byte-to-unicode mapping, BPE merges with a trailing "</w>" on the
     * last symbol, then BOS/EOS framing padded to the 77-token context with
     * EOS and a matching attention mask.
     *
     * Unicode handling is pragmatic rather than table-complete: ASCII plus
     * Latin-1/CJK classification covers realistic search queries; exotic
     * scripts still tokenize (as symbol runs) without crashing.
     *
     * Thread safety: encode() is const and safe to call concurrently after a
     * successful load().
     */
    class ClipTokenizer
    {
    public:
        static constexpr int kContextLength = 77;
        static constexpr std::int64_t kBosToken = 49406; // <|startoftext|>
        static constexpr std::int64_t kEosToken = 49407; // <|endoftext|> (also pad)

        struct Encoded
        {
            std::vector<std::int64_t> input_ids;      // always kContextLength
            std::vector<std::int64_t> attention_mask; // 1 = real token, 0 = pad
        };

        /// Load vocab.json + merges.txt (the files shipped beside the models).
        bool load(const std::string &vocab_json_path, const std::string &merges_txt_path);

        bool isLoaded() const { return !vocab_.empty(); }

        /// Tokenize UTF-8 text into a padded [kContextLength] id/mask pair.
        Encoded encode(const std::string &text) const;

    private:
        std::unordered_map<std::string, std::int64_t> vocab_;
        std::unordered_map<std::string, int> merge_ranks_; // "left right" -> rank
        std::array<std::string, 256> byte_encoder_;        // GPT-2 bytes_to_unicode

        std::vector<std::string> bpe(const std::string &byte_encoded_token) const;
    };
}
