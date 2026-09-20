#include "search/setting_detector.hpp"

#include <cctype>
#include <sstream>

namespace MediaDedup
{
    namespace
    {
        const char *kNearWords[] = {"near", "close", "by", "around", "along", "beside", "in", "nearby"};
        const char *kAtWords[] = {"on", "at"};

        std::string stripPunct(const std::string &w)
        {
            std::string out;
            for (unsigned char c : w)
                if (std::isalnum(c) || c == '-')
                    out.push_back(static_cast<char>(c));
            return out;
        }

        bool isIn(const std::string &w, const char *const *list, std::size_t n)
        {
            for (std::size_t i = 0; i < n; ++i)
                if (w == list[i])
                    return true;
            return false;
        }
    }

    std::string stripLeadingArticles(const std::string &text_lower)
    {
        std::string s = text_lower;
        for (;;)
        {
            bool stripped = false;
            for (const char *art : {"the ", "a ", "an "})
            {
                const std::string a(art);
                if (s.compare(0, a.size(), a) == 0)
                {
                    s = s.substr(a.size());
                    stripped = true;
                }
            }
            if (!stripped)
                break;
        }
        return s;
    }

    std::string settingOpForSpan(const std::string &span_lower)
    {
        std::istringstream words(span_lower);
        for (std::string w; words >> w;)
        {
            w = stripPunct(w);
            if (isIn(w, kNearWords, sizeof(kNearWords) / sizeof(*kNearWords)))
                return "near";
            if (isIn(w, kAtWords, sizeof(kAtWords) / sizeof(*kAtWords)))
                return "at";
        }
        return "";
    }

    std::vector<DetectedSetting> detectSettings(const std::string &query_lower,
                                                const ExifKeywordConfig &keywords,
                                                const std::vector<std::string> &covered_spans)
    {
        std::vector<DetectedSetting> out;
        if (keywords.settings.empty())
            return out;

        // Character ranges already claimed by other facts.
        std::vector<std::pair<std::size_t, std::size_t>> covered;
        for (const auto &span : covered_spans)
        {
            if (span.empty())
                continue;
            for (std::size_t pos = query_lower.find(span); pos != std::string::npos;
                 pos = query_lower.find(span, pos + 1))
                covered.emplace_back(pos, pos + span.size());
        }

        struct Tok { std::string word; std::size_t pos; };
        std::vector<Tok> toks;
        {
            std::size_t i = 0;
            while (i < query_lower.size())
            {
                while (i < query_lower.size() && std::isspace(static_cast<unsigned char>(query_lower[i])))
                    ++i;
                std::size_t start = i;
                while (i < query_lower.size() && !std::isspace(static_cast<unsigned char>(query_lower[i])))
                    ++i;
                if (i > start)
                    toks.push_back({stripPunct(query_lower.substr(start, i - start)), start});
            }
        }

        for (std::size_t i = 0; i < toks.size(); ++i)
        {
            const auto *cat = keywords.findSetting(toks[i].word);
            if (!cat)
                continue;
            bool skip = false;
            for (const auto &[b, e] : covered)
                if (toks[i].pos >= b && toks[i].pos < e)
                    skip = true;
            if (skip)
                continue;
            bool dup = false;
            for (const auto &d : out)
                if (d.category == cat->name)
                    dup = true;
            if (dup)
                continue;

            DetectedSetting d;
            d.category = cat->name;
            // Scaffold within the two preceding tokens ("near the lake",
            // "on the beach", "in the mountains").
            std::size_t span_start = toks[i].pos;
            std::string op;
            for (std::size_t back = 1; back <= 2 && back <= i; ++back)
            {
                const std::string &w = toks[i - back].word;
                if (isIn(w, kNearWords, sizeof(kNearWords) / sizeof(*kNearWords)))
                {
                    op = "near";
                    span_start = toks[i - back].pos;
                    break;
                }
                if (isIn(w, kAtWords, sizeof(kAtWords) / sizeof(*kAtWords)))
                {
                    op = "at";
                    span_start = toks[i - back].pos;
                    break;
                }
            }
            d.inferred = op.empty();
            d.op = op.empty() ? "at" : op;
            d.span = query_lower.substr(span_start, toks[i].pos + toks[i].word.size() - span_start);
            out.push_back(std::move(d));
        }
        return out;
    }
}
