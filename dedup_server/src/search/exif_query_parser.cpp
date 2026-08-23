#include "search/exif_query_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

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

        // Collapse whitespace runs to single spaces so a matched multi-word
        // synonym ("greater   than") can be looked up in a keyword set.
        std::string normalizeSpaces(const std::string &s)
        {
            std::istringstream stream(s);
            std::string word, out;
            while (stream >> word)
                out += (out.empty() ? "" : " ") + word;
            return out;
        }

        // A keyword as a regex fragment: specials escaped, internal
        // whitespace matching any whitespace run.
        std::string escapeKeyword(const std::string &word)
        {
            std::string out;
            for (char c : word)
            {
                if (std::isspace(static_cast<unsigned char>(c)))
                {
                    if (out.size() < 3 || out.compare(out.size() - 3, 3, "\\s+") != 0)
                        out += "\\s+";
                }
                else if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos)
                {
                    out += '\\';
                    out += c;
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }

        // Alternation over keywords, longest-first so "greater than" is
        // preferred over a hypothetical "greater". An empty list yields a
        // never-matching alternative, disabling that pattern cleanly.
        std::string alternation(std::vector<std::string> words)
        {
            if (words.empty())
                return "(?!)";
            std::sort(words.begin(), words.end(),
                      [](const std::string &a, const std::string &b) {
                          return a.size() > b.size();
                      });
            std::string out;
            for (const auto &w : words)
                out += (out.empty() ? "" : "|") + escapeKeyword(w);
            return out;
        }

        std::unordered_set<std::string> toSet(const std::vector<std::string> &words)
        {
            return {words.begin(), words.end()};
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

    // EXIF-6: every keyword alternation below is spliced in from the
    // ExifKeywordConfig; only the surrounding grammar is fixed here.
    ExifQueryParser::ExifQueryParser(const LocationGeocoder *geocoder,
                                     const ExifKeywordConfig &kw)
        : geocoder_(geocoder)
    {
        months_ = kw.months;
        greater_set_ = toSet(kw.greater_words);
        greater_equal_set_ = toSet(kw.greater_equal_words);
        less_set_ = toSet(kw.less_words);
        less_equal_set_ = toSet(kw.less_equal_words);
        aperture_wider_set_ = toSet(kw.aperture_wider_words);
        shutter_faster_set_ = toSet(kw.shutter_faster_words);
        focal_longer_set_ = toSet(kw.focal_longer_words);
        imperative_set_ = toSet(kw.imperative_words);
        filler_set_ = toSet(kw.filler_words);

        // --- ISO --------------------------------------------------------
        iso_between_ = std::regex(R"(\biso\s+between\s+(\d{2,6})\s+and\s+(\d{2,6})\b)");
        std::vector<std::string> iso_dirs = kw.greater_words;
        iso_dirs.insert(iso_dirs.end(), kw.greater_equal_words.begin(), kw.greater_equal_words.end());
        iso_dirs.insert(iso_dirs.end(), kw.less_words.begin(), kw.less_words.end());
        iso_dirs.insert(iso_dirs.end(), kw.less_equal_words.begin(), kw.less_equal_words.end());
        iso_cmp_ = std::regex(R"(\biso\s+()" + alternation(iso_dirs) + R"()\s+(\d{2,6})\b)");
        iso_eq_ = std::regex(R"(\biso\s*[:=]?\s*(\d{2,6})\b)");

        // --- Aperture ---------------------------------------------------
        std::vector<std::string> f_dirs = kw.aperture_wider_words;
        f_dirs.insert(f_dirs.end(), kw.aperture_narrower_words.begin(),
                      kw.aperture_narrower_words.end());
        f_cmp_ = std::regex(R"((?:aperture\s+)?()" + alternation(f_dirs) +
                            R"()\s+than\s+f\s*/?\s*(\d+(?:\.\d+)?))");
        f_eq_ = std::regex(R"(\bf\s*/\s*(\d+(?:\.\d+)?)|\bf(\d+(?:\.\d+)?)\b)");

        // --- Shutter speed ---------------------------------------------
        std::vector<std::string> sh_dirs = kw.shutter_faster_words;
        sh_dirs.insert(sh_dirs.end(), kw.shutter_slower_words.begin(),
                       kw.shutter_slower_words.end());
        shutter_cmp_ = std::regex(
            R"((?:shutter\s+(?:speed\s+)?)?()" + alternation(sh_dirs) +
            R"()\s+than\s+(\d+(?:\.\d+)?(?:\s*/\s*\d+)?)(?:\s*s(?:ec(?:onds?)?)?)?\b)");
        shutter_eq_ = std::regex(
            R"(\bshutter\s+(?:speed\s+)?(?:of\s+)?(\d+(?:\.\d+)?(?:\s*/\s*\d+)?)\b)");

        // --- Focal length ----------------------------------------------
        // Direction phrases here carry their own "than" where needed
        // ("longer than" vs bare "above"), so no fixed "than" follows.
        std::vector<std::string> focal_dirs = kw.focal_longer_words;
        focal_dirs.insert(focal_dirs.end(), kw.focal_shorter_words.begin(),
                          kw.focal_shorter_words.end());
        focal_cmp_ = std::regex(R"((?:focal\s+length\s+)?()" + alternation(focal_dirs) +
                                R"()\s+(\d{1,4})\s*mm\b)");
        focal_eq_ = std::regex(R"(\b(\d{1,4})\s*mm\b)");

        // --- Flash ------------------------------------------------------
        flash_off_ = std::regex(R"(\b(?:)" + alternation(kw.flash_off_phrases) + R"()\b)");
        flash_on_ = std::regex(R"(\b(?:)" + alternation(kw.flash_on_phrases) + R"()\b)");

        // --- Camera make ------------------------------------------------
        // The whole "taken with a canon camera" phrase is consumed so no
        // "taken with a" residue is left for CLIP. QP-1a: the scaffold
        // groups (verb/preposition/article/suffix) CAPTURE, so parse can
        // tell a bound "taken with a canon" from a bare "canon" — brand is
        // group 4, scaffolding participation is groups 1-3 and 5.
        camera_ = std::regex(
            R"(((?:)" + alternation(kw.camera_context_verbs) + R"()\s+)?)" +
            R"(((?:)" + alternation(kw.camera_context_prepositions) + R"()\s+)?)" +
            R"(((?:)" + alternation(kw.camera_articles) + R"()\s+)?)" +
            R"(\b()" + alternation(kw.camera_brands) + R"()\b)" +
            R"((\s+(?:)" + alternation(kw.camera_suffixes) + R"()\b)?)");

        // --- Dates ------------------------------------------------------
        month_year_ = std::regex(R"(\b()" + alternation(kw.months) +
                                 R"()\s+((?:19|20)\d{2})\b)");
        year_ = std::regex(R"(\b((?:19|20)\d{2})\b)");
        // Bare month names skip the ambiguous ones ("may" as a modal verb
        // is far more often not a month).
        std::vector<std::string> bare_months;
        const auto ambiguous = toSet(kw.ambiguous_months);
        for (const auto &m : kw.months)
            if (!ambiguous.count(m))
                bare_months.push_back(m);
        month_ = std::regex(R"(\b(?:(?:)" + alternation(kw.month_prepositions) +
                            R"()\s+)?()" + alternation(bare_months) + R"()\b)");

        // --- Location (preposition-triggered) ---------------------------
        location_ = std::regex(R"(\b(?:)" + alternation(kw.location_prepositions) +
                               R"()\s+((?:[a-z][a-z'\-]*)(?:\s+[a-z][a-z'\-]*){0,2}))");
    }

    int ExifQueryParser::monthNumber(const std::string &name) const
    {
        for (std::size_t i = 0; i < months_.size(); ++i)
            if (name == months_[i])
                return static_cast<int>(i) + 1;
        return 0;
    }

    ExifQueryConstraints ExifQueryParser::parse(const std::string &query) const
    {
        // Flattened (binding-blind) view of parseStructured(): every fact
        // lands in one constraint set — the pre-QP-1 contract, kept for the
        // golden tests and any caller that doesn't care about intent.
        StructuredQuery sq = parseStructured(query);
        ExifQueryConstraints out = std::move(sq.bound);
        for (auto &b : sq.bare)
        {
            for (const auto &n : b.numeric)
                out.numeric.push_back(n);
            for (auto &t : b.camera_terms)
                out.camera_terms.push_back(std::move(t));
        }
        out.remaining = std::move(sq.remaining);
        out.remaining_is_filler = sq.remaining_is_filler;
        return out;
    }

    StructuredQuery ExifQueryParser::parseStructured(const std::string &query) const
    {
        // QP-1a binding rules (EXIF-7 §2): comparative/range forms,
        // scaffolded camera phrases, flash phrases, locations, and ALL
        // dates are bound (filter intent); exact-value probes ("iso 800",
        // "f2.5", "50mm", "shutter 1/500") and bare brand tokens are bare
        // (explore intent) — each bare fact is its own union branch.
        StructuredQuery sq;
        ExifQueryConstraints &bound = sq.bound;
        auto bareNumeric = [&sq](const char *col, const char *op, double v, double v2) {
            ExifQueryConstraints c;
            c.numeric.push_back({col, op, v, v2});
            sq.bare.push_back(std::move(c));
        };

        std::string text = toLower(query);

        // --- ISO --------------------------------------------------------
        // Ranges first so "iso between 400 and 1600" isn't eaten as "iso 400".
        consumeMatches(text, iso_between_, [&](const std::smatch &m) {
            double lo = std::stod(m[1]), hi = std::stod(m[2]);
            if (lo > hi)
                std::swap(lo, hi); // "between 1600 and 400" still means the range
            bound.numeric.push_back({"exif_iso", "between", lo, hi});
        });
        consumeMatches(text, iso_cmp_, [&](const std::smatch &m) {
            const std::string dir = normalizeSpaces(m[1]);
            const char *op = greater_equal_set_.count(dir) ? ">="
                             : less_equal_set_.count(dir)  ? "<="
                             : greater_set_.count(dir)     ? ">"
                                                           : "<";
            bound.numeric.push_back({"exif_iso", op, std::stod(m[2]), 0.0});
        });
        consumeMatches(text, iso_eq_, [&](const std::smatch &m) {
            bareNumeric("exif_iso", "=", std::stod(m[1]), 0.0);
        });

        // --- Aperture ---------------------------------------------------
        consumeMatches(text, f_cmp_, [&](const std::smatch &m) {
            // Wider aperture = LOWER f-number.
            const bool wider = aperture_wider_set_.count(normalizeSpaces(m[1])) > 0;
            bound.numeric.push_back({"exif_f_number", wider ? "<" : ">",
                                     std::stod(m[2]), 0.0});
        });
        consumeMatches(text, f_eq_, [&](const std::smatch &m) {
            double v = std::stod(m[1].matched ? m[1] : m[2]);
            bareNumeric("exif_f_number", "between", v - 0.3, v + 0.3);
        });

        // --- Shutter speed ---------------------------------------------
        consumeMatches(text, shutter_cmp_, [&](const std::smatch &m) {
            std::string frac = m[2];
            frac.erase(std::remove(frac.begin(), frac.end(), ' '), frac.end());
            // Faster shutter = SHORTER exposure time.
            const bool faster = shutter_faster_set_.count(normalizeSpaces(m[1])) > 0;
            bound.numeric.push_back({"exif_exposure_time", faster ? "<" : ">",
                                     shutterToSeconds(frac), 0.0});
        });
        consumeMatches(text, shutter_eq_, [&](const std::smatch &m) {
            std::string frac = m[1];
            frac.erase(std::remove(frac.begin(), frac.end(), ' '), frac.end());
            double v = shutterToSeconds(frac);
            bareNumeric("exif_exposure_time", "between", v * 0.7, v * 1.4);
        });

        // --- Focal length ----------------------------------------------
        consumeMatches(text, focal_cmp_, [&](const std::smatch &m) {
            const bool above = focal_longer_set_.count(normalizeSpaces(m[1])) > 0;
            bound.numeric.push_back({"exif_focal_length", above ? ">" : "<",
                                     std::stod(m[2]), 0.0});
        });
        consumeMatches(text, focal_eq_, [&](const std::smatch &m) {
            double v = std::stod(m[1]);
            bareNumeric("exif_focal_length", "between", v - 1.0, v + 1.0);
        });

        // --- Flash (phrases are inherently scaffolded => bound) ---------
        consumeMatches(text, flash_off_, [&](const std::smatch &) {
            bound.flash_off = true;
        });
        consumeMatches(text, flash_on_, [&](const std::smatch &) {
            bound.flash_on = true;
        });

        // --- Camera make ------------------------------------------------
        consumeMatches(text, camera_, [&](const std::smatch &m) {
            const bool scaffolded = m[1].matched || m[2].matched ||
                                    m[3].matched || m[5].matched;
            if (scaffolded)
            {
                bound.camera_terms.push_back(m[4]);
            }
            else
            {
                ExifQueryConstraints c;
                c.camera_terms.push_back(m[4]);
                sq.bare.push_back(std::move(c));
            }
        });

        // --- Dates (always bound: a date token has no visual meaning) ---
        consumeMatches(text, month_year_, [&](const std::smatch &m) {
            bound.date_range = monthRange(std::stoi(m[2]), monthNumber(m[1]));
        });
        consumeMatches(text, year_, [&](const std::smatch &m) {
            if (!bound.date_range)
                bound.date_range = yearRange(std::stoi(m[1]));
        });
        // Bare month name ("august"): that month across every year.
        consumeMatches(text, month_, [&](const std::smatch &m) {
            if (!bound.date_range && !bound.month_any_year)
                bound.month_any_year = two(monthNumber(m[1]));
        });

        // --- Location (preposition-triggered => bound) ------------------
        // "in yellowstone", "at grand canyon", "near paris". Tries the
        // longest span (up to 3 words) after the preposition first.
        if (geocoder_ && geocoder_->size() > 0)
        {
            struct Hit { std::size_t pos, len; std::string span; };
            std::vector<Hit> hits;
            auto begin = std::sregex_iterator(text.begin(), text.end(), location_);
            for (auto it = begin; it != std::sregex_iterator(); ++it)
                hits.push_back({static_cast<std::size_t>(it->position(0)),
                                static_cast<std::size_t>(it->length(0)), (*it)[1]});
            for (const auto &h : hits)
            {
                if (bound.location)
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
                        bound.location = bounds;
                        bound.location_name = candidate;
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
        // Punctuation-only residue (a comma a fact left behind) is dropped
        // and never counts as content.
        int content_words = 0;
        auto hasAlnum = [](const std::string &w) {
            for (char c : w)
                if (std::isalnum(static_cast<unsigned char>(c)))
                    return true;
            return false;
        };
        while (stream >> word)
        {
            if (!hasAlnum(word))
                continue;
            if (!imperative_set_.count(word))
                rebuilt += (rebuilt.empty() ? "" : " ") + word;
            if (!filler_set_.count(word))
                ++content_words;
        }
        sq.remaining = rebuilt;
        sq.remaining_is_filler = (content_words == 0);
        if (!sq.remaining_is_filler && !sq.remaining.empty())
            sq.clip_queries.push_back(sq.remaining);
        return sq;
    }
}
