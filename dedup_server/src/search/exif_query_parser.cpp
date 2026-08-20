#include "search/exif_query_parser.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace MediaDedup
{
    namespace
    {
        std::string toLower(const std::string &s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // Blank out [begin, end) so consumed tokens vanish from the
        // remaining text but positions of later matches stay valid.
        void consume(std::string &text, std::size_t begin, std::size_t length)
        {
            for (std::size_t i = begin; i < begin + length && i < text.size(); ++i)
                text[i] = ' ';
        }

        // Run regex over text; for each match call fn(match), then blank it.
        template <typename Fn>
        void consumeMatches(std::string &text, const std::regex &re, Fn fn)
        {
            // Collect first (mutating during iteration invalidates iterators).
            struct Hit { std::size_t pos, len; std::smatch m; };
            std::vector<Hit> hits;
            auto begin = std::sregex_iterator(text.begin(), text.end(), re);
            for (auto it = begin; it != std::sregex_iterator(); ++it)
                hits.push_back({static_cast<std::size_t>(it->position(0)),
                                static_cast<std::size_t>(it->length(0)), *it});
            for (const auto &h : hits)
            {
                fn(h.m);
                consume(text, h.pos, h.len);
            }
        }

        // "1/500" -> 0.002 s; "2" -> 2 s; "0.5" -> 0.5 s.
        double shutterToSeconds(const std::string &s)
        {
            auto slash = s.find('/');
            if (slash == std::string::npos)
                return std::stod(s);
            double num = std::stod(s.substr(0, slash));
            double den = std::stod(s.substr(slash + 1));
            return den != 0.0 ? num / den : 0.0;
        }

        int monthNumber(const std::string &name)
        {
            static const char *kMonths[] = {"january", "february", "march", "april",
                                            "may", "june", "july", "august",
                                            "september", "october", "november", "december"};
            for (int i = 0; i < 12; ++i)
                if (name == kMonths[i])
                    return i + 1;
            return 0;
        }

        std::string two(int v)
        {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02d", v);
            return buf;
        }

        // Last day that keeps lexicographic BETWEEN correct without a
        // calendar: "31" overshoots short months but exif_datetime never
        // holds an invalid day, so the range just ends at the real month end.
        std::pair<std::string, std::string> monthRange(int year, int month)
        {
            std::string y = std::to_string(year);
            return {y + ":" + two(month) + ":01 00:00:00",
                    y + ":" + two(month) + ":31 23:59:59"};
        }

        std::pair<std::string, std::string> yearRange(int year)
        {
            std::string y = std::to_string(year);
            return {y + ":01:01 00:00:00", y + ":12:31 23:59:59"};
        }
    }

    ExifQueryConstraints ExifQueryParser::parse(const std::string &query) const
    {
        ExifQueryConstraints out;
        std::string text = toLower(query);

        // --- ISO --------------------------------------------------------
        // Ranges first so "iso between 400 and 1600" isn't eaten as "iso 400".
        static const std::regex iso_between(
            R"(\biso\s+between\s+(\d{2,6})\s+and\s+(\d{2,6})\b)");
        consumeMatches(text, iso_between, [&](const std::smatch &m) {
            out.numeric.push_back({"exif_iso", "between",
                                   std::stod(m[1]), std::stod(m[2])});
        });
        static const std::regex iso_cmp(
            R"(\biso\s+(above|over|greater than|more than|at least|below|under|less than|at most)\s+(\d{2,6})\b)");
        consumeMatches(text, iso_cmp, [&](const std::smatch &m) {
            const std::string dir = m[1];
            const bool ge = (dir == "at least"), le = (dir == "at most");
            const bool gt = (dir == "above" || dir == "over" ||
                             dir == "greater than" || dir == "more than");
            out.numeric.push_back({"exif_iso", ge ? ">=" : le ? "<=" : gt ? ">" : "<",
                                   std::stod(m[2]), 0.0});
        });
        static const std::regex iso_eq(R"(\biso\s*[:=]?\s*(\d{2,6})\b)");
        consumeMatches(text, iso_eq, [&](const std::smatch &m) {
            out.numeric.push_back({"exif_iso", "=", std::stod(m[1]), 0.0});
        });

        // --- Aperture ---------------------------------------------------
        static const std::regex f_cmp(
            R"((?:aperture\s+)?(wider|faster|brighter|narrower|smaller)\s+than\s+f\s*/?\s*(\d+(?:\.\d+)?))");
        consumeMatches(text, f_cmp, [&](const std::smatch &m) {
            const std::string dir = m[1];
            // Wider aperture = LOWER f-number.
            const bool wider = (dir == "wider" || dir == "faster" || dir == "brighter");
            out.numeric.push_back({"exif_f_number", wider ? "<" : ">",
                                   std::stod(m[2]), 0.0});
        });
        static const std::regex f_eq(R"(\bf\s*/\s*(\d+(?:\.\d+)?)|\bf(\d+(?:\.\d+)?)\b)");
        consumeMatches(text, f_eq, [&](const std::smatch &m) {
            double v = std::stod(m[1].matched ? m[1] : m[2]);
            out.numeric.push_back({"exif_f_number", "between", v - 0.3, v + 0.3});
        });

        // --- Shutter speed ---------------------------------------------
        static const std::regex shutter_cmp(
            R"((?:shutter\s+(?:speed\s+)?)?(faster|quicker|slower|longer)\s+than\s+(\d+(?:\.\d+)?(?:\s*/\s*\d+)?)(?:\s*s(?:ec(?:onds?)?)?)?\b)");
        consumeMatches(text, shutter_cmp, [&](const std::smatch &m) {
            std::string frac = m[2];
            frac.erase(std::remove(frac.begin(), frac.end(), ' '), frac.end());
            const std::string dir = m[1];
            const bool faster = (dir == "faster" || dir == "quicker");
            // Faster shutter = SHORTER exposure time.
            out.numeric.push_back({"exif_exposure_time", faster ? "<" : ">",
                                   shutterToSeconds(frac), 0.0});
        });
        static const std::regex shutter_eq(
            R"(\bshutter\s+(?:speed\s+)?(?:of\s+)?(\d+(?:\.\d+)?(?:\s*/\s*\d+)?)\b)");
        consumeMatches(text, shutter_eq, [&](const std::smatch &m) {
            std::string frac = m[1];
            frac.erase(std::remove(frac.begin(), frac.end(), ' '), frac.end());
            double v = shutterToSeconds(frac);
            out.numeric.push_back({"exif_exposure_time", "between", v * 0.7, v * 1.4});
        });

        // --- Focal length ----------------------------------------------
        static const std::regex focal_cmp(
            R"((?:focal\s+length\s+)?(above|over|longer than|below|under|shorter than)\s+(\d{1,4})\s*mm\b)");
        consumeMatches(text, focal_cmp, [&](const std::smatch &m) {
            const std::string dir = m[1];
            const bool above = (dir == "above" || dir == "over" || dir == "longer than");
            out.numeric.push_back({"exif_focal_length", above ? ">" : "<",
                                   std::stod(m[2]), 0.0});
        });
        static const std::regex focal_eq(R"(\b(\d{1,4})\s*mm\b)");
        consumeMatches(text, focal_eq, [&](const std::smatch &m) {
            double v = std::stod(m[1]);
            out.numeric.push_back({"exif_focal_length", "between", v - 1.0, v + 1.0});
        });

        // --- Flash ------------------------------------------------------
        static const std::regex flash_off_re(
            R"(\b(?:without|no)\s+flash\b|\bflash\s+off\b)");
        consumeMatches(text, flash_off_re, [&](const std::smatch &) {
            out.flash_off = true;
        });
        static const std::regex flash_on_re(
            R"(\bwith\s+flash\b|\bflash\s+(?:on|fired)\b|\busing\s+flash\b)");
        consumeMatches(text, flash_on_re, [&](const std::smatch &) {
            out.flash_on = true;
        });

        // --- Camera make ------------------------------------------------
        // The whole "taken with a canon camera" phrase is consumed so no
        // "taken with a" residue is left for CLIP.
        static const std::regex camera_re(
            R"((?:(?:taken|shot|captured|photographed)\s+)?(?:(?:with|on|using|by)\s+)?(?:(?:a|an|my)\s+)?)"
            R"(\b(canon|nikon|sony|fuji(?:film)?|panasonic|lumix|olympus|leica|pentax|hasselblad)"
            R"(|iphone|apple|samsung|google|pixel|dji|gopro|ricoh|sigma|kodak)\b)"
            R"((?:\s+camera\b)?)");
        consumeMatches(text, camera_re, [&](const std::smatch &m) {
            std::string term = m[1];
            if (term == "fuji")
                term = "fuji"; // matches FUJIFILM via LIKE
            if (term == "pixel")
                term = "pixel"; // Google Pixel models carry "Pixel"
            out.camera_terms.push_back(term);
        });

        // --- Dates ------------------------------------------------------
        static const std::regex month_year_re(
            R"(\b(january|february|march|april|may|june|july|august|september|october|november|december)\s+((?:19|20)\d{2})\b)");
        consumeMatches(text, month_year_re, [&](const std::smatch &m) {
            out.date_range = monthRange(std::stoi(m[2]), monthNumber(m[1]));
        });
        static const std::regex year_re(R"(\b((?:19|20)\d{2})\b)");
        consumeMatches(text, year_re, [&](const std::smatch &m) {
            if (!out.date_range)
                out.date_range = yearRange(std::stoi(m[1]));
        });
        // Bare month name ("august"): that month across every year. "may" is
        // exempt — as a modal verb it is far more often not a month.
        static const std::regex month_re(
            R"(\b(?:in\s+|from\s+|during\s+)?(january|february|march|april|june|july|august|september|october|november|december)\b)");
        consumeMatches(text, month_re, [&](const std::smatch &m) {
            if (!out.date_range && !out.month_any_year)
                out.month_any_year = two(monthNumber(m[1]));
        });

        // --- Location (preposition-triggered) ---------------------------
        // "in yellowstone", "at grand canyon", "near paris". Tries the
        // longest span (up to 3 words) after the preposition first.
        if (geocoder_ && geocoder_->size() > 0)
        {
            static const std::regex loc_re(
                R"(\b(?:in|at|near|around|from)\s+((?:[a-z][a-z'\-]*)(?:\s+[a-z][a-z'\-]*){0,2}))");
            struct Hit { std::size_t pos, len; std::string span; };
            std::vector<Hit> hits;
            auto begin = std::sregex_iterator(text.begin(), text.end(), loc_re);
            for (auto it = begin; it != std::sregex_iterator(); ++it)
                hits.push_back({static_cast<std::size_t>(it->position(0)),
                                static_cast<std::size_t>(it->length(0)), (*it)[1]});
            for (const auto &h : hits)
            {
                if (out.location)
                    break;
                // Longest word-prefix of the span that the gazetteer knows.
                std::istringstream words(h.span);
                std::vector<std::string> tokens;
                for (std::string w; words >> w;)
                    tokens.push_back(w);
                for (std::size_t take = tokens.size(); take > 0; --take)
                {
                    std::string candidate;
                    for (std::size_t i = 0; i < take; ++i)
                        candidate += (i ? " " : "") + tokens[i];
                    auto bounds = geocoder_->lookup(candidate);
                    if (bounds)
                    {
                        out.location = bounds;
                        out.location_name = candidate;
                        // Consume the preposition plus the words that matched.
                        std::size_t span_start = h.pos + h.len - h.span.size();
                        std::size_t matched_len = candidate.size();
                        consume(text, h.pos, (span_start - h.pos) + matched_len);
                        break;
                    }
                }
            }
        }

        // --- Remaining text --------------------------------------------
        std::istringstream stream(text);
        std::string word, rebuilt;
        // Imperative noise is stripped from the CLIP text ("find all ...");
        // noun filler ("photos", "pictures") is KEPT — CLIP was trained on
        // caption-like phrases — but neither counts as semantic content.
        static const std::unordered_set<std::string> kImperative = {
            "find", "show", "me", "all"};
        static const std::unordered_set<std::string> kFiller = {
            "find", "show", "me", "all", "the", "a", "an", "my", "of",
            "photo", "photos", "picture", "pictures", "pic", "pics",
            "image", "images", "shot", "shots", "taken", "with", "and"};
        int content_words = 0;
        while (stream >> word)
        {
            if (!kImperative.count(word))
                rebuilt += (rebuilt.empty() ? "" : " ") + word;
            if (!kFiller.count(word))
                ++content_words;
        }
        out.remaining = rebuilt;
        out.remaining_is_filler = (content_words == 0);
        return out;
    }
}
