#include "search/exif_query_parser.hpp"
#include "search/setting_detector.hpp"

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
                                     const ExifKeywordConfig &kw,
                                     PersonResolver person_resolver,
                                     PlaceResolver place_resolver)
        : geocoder_(geocoder), person_resolver_(std::move(person_resolver)),
          place_resolver_(std::move(place_resolver))
    {
        months_ = kw.months;
        keywords_ = kw;
        greater_set_ = toSet(kw.greater_words);
        greater_equal_set_ = toSet(kw.greater_equal_words);
        less_set_ = toSet(kw.less_words);
        less_equal_set_ = toSet(kw.less_equal_words);
        aperture_wider_set_ = toSet(kw.aperture_wider_words);
        aperture_narrower_set_ = toSet(kw.aperture_narrower_words);
        shutter_faster_set_ = toSet(kw.shutter_faster_words);
        shutter_slower_set_ = toSet(kw.shutter_slower_words);
        focal_longer_set_ = toSet(kw.focal_longer_words);
        focal_shorter_set_ = toSet(kw.focal_shorter_words);
        imperative_set_ = toSet(kw.imperative_words);
        filler_set_ = toSet(kw.filler_words);

        // Every numeric dimension accepts the generic comparators (which
        // compare the NUMBER as written, so ">" and "at least" work on all of
        // them) on top of its own direction words (which compare the physical
        // quantity and may invert). Symbols ride in as ordinary keywords —
        // escapeKeyword leaves > and < alone — so ">f1.4" parses like
        // "wider than f/1.4" instead of collapsing to an equality band.
        const auto withGeneric = [&kw](std::vector<std::string> words) {
            for (const auto *list : {&kw.greater_words, &kw.greater_equal_words,
                                     &kw.less_words, &kw.less_equal_words})
                words.insert(words.end(), list->begin(), list->end());
            return words;
        };
        // "greater than" and "greater" are both keywords, so the comparator
        // may already carry its own "than"; a second one is optional.
        const std::string kThan = R"((?:\s*than)?\s*)";

        // --- ISO --------------------------------------------------------
        iso_between_ = std::regex(R"(\biso\s+between\s+(\d{2,6})\s+and\s+(\d{2,6})\b)");
        std::vector<std::string> iso_dirs = kw.greater_words;
        iso_dirs.insert(iso_dirs.end(), kw.greater_equal_words.begin(), kw.greater_equal_words.end());
        iso_dirs.insert(iso_dirs.end(), kw.less_words.begin(), kw.less_words.end());
        iso_dirs.insert(iso_dirs.end(), kw.less_equal_words.begin(), kw.less_equal_words.end());
        iso_cmp_ = std::regex(R"(\biso\s*()" + alternation(iso_dirs) + ")" + kThan +
                              R"((\d{2,6})\b)");
        iso_eq_ = std::regex(R"(\biso\s*[:=]?\s*(\d{2,6})\b)");

        // --- Aperture ---------------------------------------------------
        std::vector<std::string> f_dirs = kw.aperture_wider_words;
        f_dirs.insert(f_dirs.end(), kw.aperture_narrower_words.begin(),
                      kw.aperture_narrower_words.end());
        f_dirs = withGeneric(std::move(f_dirs));
        // Unlabeled: the VALUE must carry the f ("wider than f/2.8", ">f2.8"),
        // otherwise a bare "above 200" would be read as an aperture.
        f_cmp_ = std::regex(R"(()" + alternation(f_dirs) + ")" + kThan +
                            R"(f\s*/?\s*(\d+(?:\.\d+)?))");
        // Labeled: the dimension is named, so a bare number is unambiguous
        // ("aperture above 2.8", "f-stop >= 1.4", "f number at most 4").
        f_cmp_labeled_ = std::regex(R"(\b(?:apertures?|f\s*-?\s*stops?|f\s*-?\s*numbers?)\s*()" +
                                    alternation(f_dirs) + ")" + kThan +
                                    R"((?:f\s*/?\s*)?(\d+(?:\.\d+)?))");
        f_eq_ = std::regex(R"(\bf\s*/\s*(\d+(?:\.\d+)?)|\bf(\d+(?:\.\d+)?)\b)");

        // --- Shutter speed ---------------------------------------------
        std::vector<std::string> sh_dirs = kw.shutter_faster_words;
        sh_dirs.insert(sh_dirs.end(), kw.shutter_slower_words.begin(),
                       kw.shutter_slower_words.end());
        sh_dirs = withGeneric(std::move(sh_dirs));
        // Unlabeled: the value must LOOK like an exposure — a fraction, or a
        // number carrying a seconds unit. Without that guard "longer than
        // 200mm" was eaten here as a 200-second exposure before the focal
        // pattern (which runs later) ever saw it.
        shutter_cmp_ = std::regex(
            R"(()" + alternation(sh_dirs) + ")" + kThan +
            R"((?:(\d+(?:\.\d+)?\s*/\s*\d+)|(\d+(?:\.\d+)?)\s*s(?:ec(?:onds?)?)?\b))");
        // Labeled: "shutter slower than 2", "shutter speed >= 1/250".
        shutter_cmp_labeled_ = std::regex(
            R"(\bshutter\s*(?:speed\s*)?()" + alternation(sh_dirs) + ")" + kThan +
            R"((\d+(?:\.\d+)?(?:\s*/\s*\d+)?)(?:\s*s(?:ec(?:onds?)?)?)?\b)");
        shutter_eq_ = std::regex(
            R"(\bshutter\s+(?:speed\s+)?(?:of\s+)?(\d+(?:\.\d+)?(?:\s*/\s*\d+)?)\b)");

        // --- Focal length ----------------------------------------------
        // Direction phrases here carry their own "than" where needed
        // ("longer than" vs bare "above"), so no fixed "than" follows.
        std::vector<std::string> focal_dirs = kw.focal_longer_words;
        focal_dirs.insert(focal_dirs.end(), kw.focal_shorter_words.begin(),
                          kw.focal_shorter_words.end());
        focal_dirs = withGeneric(std::move(focal_dirs));
        // Unlabeled: the value must carry mm ("longer than 200mm", "> 85 mm").
        focal_cmp_ = std::regex(R"(()" + alternation(focal_dirs) + ")" + kThan +
                                R"((\d{1,4})\s*mm\b)");
        // Labeled: "focal length above 200", "focal length <= 35mm".
        focal_cmp_labeled_ = std::regex(R"(\bfocal(?:\s*length)?\s*()" +
                                        alternation(focal_dirs) + ")" + kThan +
                                        R"((\d{1,4})(?:\s*mm)?\b)");
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

    // A matched comparator's SQL operator. The dimension's own direction
    // words are consulted FIRST and may invert (a wider aperture is a LOWER
    // f-number, a faster shutter a SHORTER time); the generic comparators
    // fall through and compare the number exactly as written. That ordering
    // is why "smaller than f/2" keeps its photographer reading — a narrower
    // aperture, so a HIGHER f-number — while "less than f/2" reads as the
    // arithmetic it looks like.
    const char *ExifQueryParser::comparatorOp(
        const std::string &word,
        const std::unordered_set<std::string> &lower_words,
        const std::unordered_set<std::string> &higher_words) const
    {
        if (lower_words.count(word))
            return "<";
        if (higher_words.count(word))
            return ">";
        if (greater_equal_set_.count(word))
            return ">=";
        if (less_equal_set_.count(word))
            return "<=";
        if (greater_set_.count(word))
            return ">";
        if (less_set_.count(word))
            return "<";
        return nullptr; // unreachable: the alternation is built from these sets
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
            // ISO has no direction words of its own — bigger is just bigger.
            if (const char *op = comparatorOp(normalizeSpaces(m[1]), {}, {}))
                bound.numeric.push_back({"exif_iso", op, std::stod(m[2]), 0.0});
        });
        consumeMatches(text, iso_eq_, [&](const std::smatch &m) {
            bareNumeric("exif_iso", "=", std::stod(m[1]), 0.0);
        });

        // --- Aperture ---------------------------------------------------
        auto apertureCompare = [&](const std::smatch &m) {
            if (const char *op = comparatorOp(normalizeSpaces(m[1]),
                                              aperture_wider_set_,
                                              aperture_narrower_set_))
                bound.numeric.push_back({"exif_f_number", op, std::stod(m[2]), 0.0});
        };
        // Labeled first: "f-stop above 2.8" must not have its bare "2.8"
        // stolen by the f_eq_ probe below.
        consumeMatches(text, f_cmp_labeled_, apertureCompare);
        consumeMatches(text, f_cmp_, apertureCompare);
        consumeMatches(text, f_eq_, [&](const std::smatch &m) {
            double v = std::stod(m[1].matched ? m[1] : m[2]);
            bareNumeric("exif_f_number", "between", v - 0.3, v + 0.3);
        });

        // --- Shutter speed ---------------------------------------------
        auto shutterCompare = [&](const std::smatch &m, int value_group) {
            std::string frac = m[value_group];
            frac.erase(std::remove(frac.begin(), frac.end(), ' '), frac.end());
            if (const char *op = comparatorOp(normalizeSpaces(m[1]),
                                              shutter_faster_set_,
                                              shutter_slower_set_))
                bound.numeric.push_back({"exif_exposure_time", op,
                                         shutterToSeconds(frac), 0.0});
        };
        consumeMatches(text, shutter_cmp_labeled_,
                       [&](const std::smatch &m) { shutterCompare(m, 2); });
        consumeMatches(text, shutter_cmp_, [&](const std::smatch &m) {
            // The unlabeled form captures the fraction and the seconds value
            // in separate alternatives; exactly one of them participated.
            shutterCompare(m, m[2].matched ? 2 : 3);
        });
        consumeMatches(text, shutter_eq_, [&](const std::smatch &m) {
            std::string frac = m[1];
            frac.erase(std::remove(frac.begin(), frac.end(), ' '), frac.end());
            double v = shutterToSeconds(frac);
            bareNumeric("exif_exposure_time", "between", v * 0.7, v * 1.4);
        });

        // --- Focal length ----------------------------------------------
        auto focalCompare = [&](const std::smatch &m) {
            if (const char *op = comparatorOp(normalizeSpaces(m[1]),
                                              focal_shorter_set_,
                                              focal_longer_set_))
                bound.numeric.push_back({"exif_focal_length", op,
                                         std::stod(m[2]), 0.0});
        };
        consumeMatches(text, focal_cmp_labeled_, focalCompare);
        consumeMatches(text, focal_cmp_, focalCompare);
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

        // --- Person names (QP-2 regex fallback) -------------------------
        // Scaffolded only: "of <name>" / "with <name>" where <name> is a
        // KNOWN named person (resolver-verified at parse time — names are
        // runtime user data). Consumption is CONDITIONAL on resolution:
        // "of sunset" must stay in the semantic text. Bare-name
        // exploration needs the LLM path; the degraded-parse indicator
        // covers its absence.
        if (person_resolver_)
        {
            static const std::regex person_re(R"(\b(of|with)\s+([a-z]+)\b)");
            struct Hit { std::size_t pos, len; std::string name; };
            std::vector<Hit> resolved;
            for (auto it = std::sregex_iterator(text.begin(), text.end(), person_re);
                 it != std::sregex_iterator(); ++it)
            {
                const std::string name = (*it)[2];
                const int pid = person_resolver_(name);
                if (pid > 0)
                {
                    bound.person_ids.push_back(pid);
                    resolved.push_back({static_cast<std::size_t>(it->position(0)),
                                        static_cast<std::size_t>(it->length(0)), name});
                }
            }
            for (const auto &h : resolved)
                consume(text, h.pos, h.len);
        }

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
        std::vector<std::string> place_attempts_out;
        if ((geocoder_ && geocoder_->size() > 0) || place_resolver_)
        {
            struct Hit { std::size_t pos, len; std::string span; };
            std::vector<Hit> hits;
            auto begin = std::sregex_iterator(text.begin(), text.end(), location_);
            for (auto it = begin; it != std::sregex_iterator(); ++it)
                hits.push_back({static_cast<std::size_t>(it->position(0)),
                                static_cast<std::size_t>(it->length(0)), (*it)[1]});
            // PLACE-4: a preposition-triggered span that resolves to no
            // place ("in mountain view") is still a place ATTEMPT — its
            // words must not be re-read as settings ("mountain"). A span
            // that is itself a setting ("in the mountains") stays visible.
            std::vector<std::string> place_attempts;
            for (const auto &h : hits)
            {
                // One location per query (pre-existing); a place_ids-only
                // hit counts as resolved too (review finding: it used to be
                // overwritten by a later bbox hit).
                if (bound.location || !bound.place_ids.empty())
                    break;
                bool resolved = false, setting_candidate = false;
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
                    // PLACE-4: "near the lake" is a setting, not a place —
                    // the gazetteer's substring match would otherwise turn
                    // "lake" into Salt Lake City's bbox. And a bare article
                    // ("the", left over from that skip) must never reach the
                    // substring pass either: "the" is in "Netherlands".
                    const std::string core = stripLeadingArticles(candidate + " ");
                    if (core.find_first_not_of(' ') == std::string::npos)
                        continue;
                    if (keywords_.findSetting(core.substr(0, core.find_last_not_of(' ') + 1)))
                    {
                        setting_candidate = true;
                        continue;
                    }
                    std::optional<LocationGeocoder::Bounds> bounds;
                    if (geocoder_ && geocoder_->size() > 0)
                        bounds = geocoder_->lookup(candidate);
                    // PLACE-2: places the library has photos in, by name.
                    std::vector<int> place_ids;
                    if (place_resolver_)
                        place_ids = place_resolver_(candidate);
                    if (bounds || !place_ids.empty())
                    {
                        bound.location = bounds;
                        bound.place_ids = std::move(place_ids);
                        bound.location_name = candidate;
                        // Consume the preposition plus the words that matched.
                        std::size_t span_start = h.pos + h.len - h.span.size();
                        std::size_t matched_len = candidate.size();
                        consume(text, h.pos, (span_start - h.pos) + matched_len);
                        resolved = true;
                        break;
                    }
                }
                if (!resolved && !setting_candidate)
                    place_attempts.push_back(toLower(h.span));
            }
            place_attempts_out = place_attempts;
        }

        // --- Settings (PLACE-4, deterministic) -------------------------
        // Runs on the text AFTER other facts were consumed, so "salt lake
        // city" (a place) can never leave a stray "lake" behind. The word
        // stays in the text for CLIP.
        for (const auto &d : detectSettings(text, keywords_, place_attempts_out))
        {
            const auto *cat = keywords_.findSetting(d.category);
            if (!cat)
                continue;
            ExifQueryConstraints::Setting st;
            st.category = cat->name;
            st.fcodes = cat->codes;
            st.shoreline = cat->shoreline;
            st.max_m = d.op == "near" ? setting_near_m_ : setting_at_m_;
            st.inferred = d.inferred;
            bound.settings.push_back(std::move(st));
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
