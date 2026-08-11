#include "media_processors/text/clip_tokenizer.hpp"

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/Logger.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace MediaDedup
{
    namespace
    {
        // --- UTF-8 <-> codepoint helpers -------------------------------

        void appendUtf8(std::string &out, std::uint32_t cp)
        {
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        std::vector<std::uint32_t> decodeUtf8(const std::string &s)
        {
            std::vector<std::uint32_t> cps;
            cps.reserve(s.size());
            size_t i = 0;
            while (i < s.size())
            {
                unsigned char c = static_cast<unsigned char>(s[i]);
                std::uint32_t cp = 0;
                size_t len = 1;
                if (c < 0x80)
                {
                    cp = c;
                }
                else if ((c >> 5) == 0x6 && i + 1 < s.size())
                {
                    cp = (c & 0x1F) << 6 | (s[i + 1] & 0x3F);
                    len = 2;
                }
                else if ((c >> 4) == 0xE && i + 2 < s.size())
                {
                    cp = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F);
                    len = 3;
                }
                else if ((c >> 3) == 0x1E && i + 3 < s.size())
                {
                    cp = (c & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 |
                         (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F);
                    len = 4;
                }
                else
                {
                    cp = 0xFFFD; // invalid byte: replacement char, keep going
                }
                cps.push_back(cp);
                i += len;
            }
            return cps;
        }

        // --- character classes (pragmatic Unicode) ----------------------

        bool isSpaceCp(std::uint32_t cp)
        {
            return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
                   cp == '\f' || cp == 0x0B || cp == 0xA0 ||
                   (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
                   cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
        }

        bool isDigitCp(std::uint32_t cp) { return cp >= '0' && cp <= '9'; }

        // CJK ideographs / kana: BasicTokenizer isolates each char, so they
        // lex as single-codepoint letter tokens.
        bool isCjkCp(std::uint32_t cp)
        {
            return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
                   (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0x20000 && cp <= 0x2A6DF);
        }

        bool isLetterCp(std::uint32_t cp)
        {
            if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
                return true;
            if (cp < 0x80)
                return false;
            if (isCjkCp(cp))
                return true;
            // Latin-1 supplement + extended (minus x and division signs),
            // Greek, Cyrillic, Armenian, Hebrew, Arabic letter blocks.
            if (cp >= 0xC0 && cp <= 0x24F)
                return cp != 0xD7 && cp != 0xF7;
            if (cp >= 0x370 && cp <= 0x3FF)
                return true;
            if (cp >= 0x400 && cp <= 0x4FF)
                return true;
            if (cp >= 0x531 && cp <= 0x58F)
                return true;
            if (cp >= 0x5D0 && cp <= 0x5EA)
                return true;
            if (cp >= 0x620 && cp <= 0x64A)
                return true;
            return false;
        }

        std::uint32_t lowerCp(std::uint32_t cp)
        {
            if (cp >= 'A' && cp <= 'Z')
                return cp + 0x20;
            if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) // Latin-1 uppercase
                return cp + 0x20;
            return cp;
        }

        // The CLIP lexer: contractions | letter runs | single digits |
        // symbol runs, over lowercased whitespace-cleaned text.
        std::vector<std::string> lexClip(const std::string &text)
        {
            std::vector<std::uint32_t> cps = decodeUtf8(text);
            for (auto &cp : cps)
                cp = lowerCp(cp);

            static const std::vector<std::vector<std::uint32_t>> kContractions = {
                {'\'', 's'}, {'\'', 't'}, {'\'', 'r', 'e'}, {'\'', 'v', 'e'},
                {'\'', 'm'}, {'\'', 'l', 'l'}, {'\'', 'd'}};

            std::vector<std::string> tokens;
            size_t i = 0;
            const size_t n = cps.size();
            auto emit = [&](size_t from, size_t to) {
                std::string s;
                for (size_t k = from; k < to; ++k)
                    appendUtf8(s, cps[k]);
                tokens.push_back(std::move(s));
            };

            while (i < n)
            {
                if (isSpaceCp(cps[i]))
                {
                    ++i;
                    continue;
                }
                if (cps[i] == '\'')
                {
                    bool matched = false;
                    for (const auto &c : kContractions)
                    {
                        if (i + c.size() <= n && std::equal(c.begin(), c.end(), cps.begin() + i))
                        {
                            emit(i, i + c.size());
                            i += c.size();
                            matched = true;
                            break;
                        }
                    }
                    if (matched)
                        continue;
                    // fall through: apostrophe joins a symbol run
                }
                if (isDigitCp(cps[i]))
                {
                    emit(i, i + 1); // \p{N} matches ONE digit per token
                    ++i;
                    continue;
                }
                if (isLetterCp(cps[i]))
                {
                    if (isCjkCp(cps[i]))
                    {
                        emit(i, i + 1);
                        ++i;
                        continue;
                    }
                    size_t j = i + 1;
                    while (j < n && isLetterCp(cps[j]) && !isCjkCp(cps[j]))
                        ++j;
                    emit(i, j);
                    i = j;
                    continue;
                }
                // symbol run: anything that is not space/letter/digit
                // (apostrophes that didn't start a contraction included)
                size_t j = i;
                while (j < n && !isSpaceCp(cps[j]) && !isDigitCp(cps[j]) && !isLetterCp(cps[j]))
                    ++j;
                emit(i, j);
                i = j;
            }
            return tokens;
        }
    }

    bool ClipTokenizer::load(const std::string &vocab_json_path, const std::string &merges_txt_path)
    {
        Poco::Logger &logger = Poco::Logger::get("ClipTokenizer");
        try
        {
            std::ifstream vocab_file(vocab_json_path, std::ios::binary);
            if (!vocab_file)
            {
                logger.error("Cannot open vocab file: %s", vocab_json_path);
                return false;
            }
            std::stringstream vocab_buf;
            vocab_buf << vocab_file.rdbuf();

            Poco::JSON::Parser parser;
            auto parsed = parser.parse(vocab_buf.str());
            auto obj = parsed.extract<Poco::JSON::Object::Ptr>();
            vocab_.clear();
            vocab_.reserve(obj->size());
            for (const auto &kv : *obj)
                vocab_[kv.first] = kv.second.convert<std::int64_t>();

            std::ifstream merges_file(merges_txt_path);
            if (!merges_file)
            {
                logger.error("Cannot open merges file: %s", merges_txt_path);
                vocab_.clear();
                return false;
            }
            merge_ranks_.clear();
            std::string line;
            int rank = 0;
            bool first = true;
            while (std::getline(merges_file, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (first && line.rfind("#version", 0) == 0)
                {
                    first = false;
                    continue;
                }
                first = false;
                if (line.empty())
                    continue;
                merge_ranks_[line] = rank++;
            }

            // GPT-2 bytes_to_unicode: printable Latin-1 ranges map to
            // themselves; remaining bytes map to 256, 257, ... in byte order.
            std::uint32_t next = 256;
            for (int b = 0; b < 256; ++b)
            {
                std::uint32_t cp;
                if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255))
                    cp = static_cast<std::uint32_t>(b);
                else
                    cp = next++;
                byte_encoder_[b].clear();
                appendUtf8(byte_encoder_[b], cp);
            }

            logger.information("Loaded CLIP tokenizer: %zu vocab entries, %zu merges",
                               vocab_.size(), merge_ranks_.size());
            return vocab_.size() > 0 && !merge_ranks_.empty();
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to load tokenizer: %s", std::string(e.what()));
            vocab_.clear();
            merge_ranks_.clear();
            return false;
        }
    }

    std::vector<std::string> ClipTokenizer::bpe(const std::string &byte_encoded_token) const
    {
        // Split into single "alphabet" codepoints; the last carries </w>.
        std::vector<std::string> word;
        {
            const std::vector<std::uint32_t> cps = decodeUtf8(byte_encoded_token);
            word.reserve(cps.size());
            for (std::uint32_t cp : cps)
            {
                std::string s;
                appendUtf8(s, cp);
                word.push_back(std::move(s));
            }
        }
        if (word.empty())
            return word;
        word.back() += "</w>";

        while (word.size() > 1)
        {
            int best_rank = std::numeric_limits<int>::max();
            size_t best_i = 0;
            for (size_t i = 0; i + 1 < word.size(); ++i)
            {
                auto it = merge_ranks_.find(word[i] + " " + word[i + 1]);
                if (it != merge_ranks_.end() && it->second < best_rank)
                {
                    best_rank = it->second;
                    best_i = i;
                }
            }
            if (best_rank == std::numeric_limits<int>::max())
                break;

            // Merge every adjacent occurrence of the winning pair.
            const std::string left = word[best_i];
            const std::string right = word[best_i + 1];
            std::vector<std::string> merged;
            merged.reserve(word.size());
            for (size_t i = 0; i < word.size();)
            {
                if (i + 1 < word.size() && word[i] == left && word[i + 1] == right)
                {
                    merged.push_back(left + right);
                    i += 2;
                }
                else
                {
                    merged.push_back(word[i]);
                    ++i;
                }
            }
            word = std::move(merged);
        }
        return word;
    }

    ClipTokenizer::Encoded ClipTokenizer::encode(const std::string &text) const
    {
        Encoded out;
        out.input_ids.reserve(kContextLength);
        out.input_ids.push_back(kBosToken);

        for (const std::string &raw : lexClip(text))
        {
            if (out.input_ids.size() >= kContextLength - 1)
                break; // room must remain for EOS

            std::string encoded;
            for (unsigned char b : raw)
                encoded += byte_encoder_[b];

            for (const std::string &piece : bpe(encoded))
            {
                if (out.input_ids.size() >= kContextLength - 1)
                    break;
                auto it = vocab_.find(piece);
                out.input_ids.push_back(it != vocab_.end() ? it->second : kEosToken);
            }
        }

        out.input_ids.push_back(kEosToken);
        out.attention_mask.assign(out.input_ids.size(), 1);
        while (out.input_ids.size() < kContextLength)
        {
            out.input_ids.push_back(kEosToken); // HF CLIP pads with EOS
            out.attention_mask.push_back(0);
        }
        return out;
    }
}
