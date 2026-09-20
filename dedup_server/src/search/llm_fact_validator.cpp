#include "search/llm_fact_validator.hpp"
#include "search/setting_detector.hpp"

#include <Poco/Logger.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace MediaDedup
{
    namespace
    {
        // Lowercase + collapse whitespace runs, so grounding is insensitive
        // to case and spacing but nothing else.
        std::string normalize(const std::string &s)
        {
            std::string lowered(s);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::istringstream stream(lowered);
            std::string word, out;
            while (stream >> word)
                out += (out.empty() ? "" : " ") + word;
            return out;
        }

        // The model sometimes echoes units despite the prompt ("85mm",
        // "f/2.8", "1/500s"). Grounding already proved the text honest;
        // stripping adornments is deterministic normalization, not trust.
        std::string stripUnits(const std::string &dim, std::string v)
        {
            auto lower = v;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (dim == "aperture")
            {
                if (lower.rfind("f/", 0) == 0)
                    return v.substr(2);
                if (lower.size() > 1 && lower[0] == 'f' &&
                    std::isdigit(static_cast<unsigned char>(lower[1])))
                    return v.substr(1);
            }
            if (dim == "focal" && lower.size() > 2 &&
                lower.compare(lower.size() - 2, 2, "mm") == 0)
                return v.substr(0, v.size() - 2);
            if (dim == "shutter")
            {
                for (const char *suf : {"seconds", "second", "sec", "s"})
                {
                    const std::size_t n = std::strlen(suf);
                    if (lower.size() > n &&
                        lower.compare(lower.size() - n, n, suf) == 0)
                        return v.substr(0, v.size() - n);
                }
            }
            return v;
        }

        bool parseNumber(const std::string &s, double &out)
        {
            try
            {
                std::size_t used = 0;
                out = std::stod(s, &used);
                return used == s.size();
            }
            catch (...)
            {
                return false;
            }
        }

        // "1/500" -> 0.002 s; "2" -> 2 s; "0.5" -> 0.5 s.
        bool parseShutterSeconds(const std::string &s, double &out)
        {
            auto slash = s.find('/');
            if (slash == std::string::npos)
                return parseNumber(s, out);
            double num = 0, den = 0;
            if (!parseNumber(s.substr(0, slash), num) ||
                !parseNumber(s.substr(slash + 1), den) || den == 0.0)
                return false;
            out = num / den;
            return true;
        }

        bool parseYear(const std::string &s, int &out)
        {
            if (s.size() != 4 ||
                !std::all_of(s.begin(), s.end(),
                             [](unsigned char c) { return std::isdigit(c); }))
                return false;
            out = std::stoi(s);
            return out >= 1900 && out <= 2100;
        }

        std::string two(int v)
        {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02d", v);
            return buf;
        }

        std::pair<std::string, std::string> yearSpan(int lo, int hi)
        {
            return {std::to_string(lo) + ":01:01 00:00:00",
                    std::to_string(hi) + ":12:31 23:59:59"};
        }

        // "31" overshoots short months, but exif_datetime never holds an
        // invalid day, so the lexicographic range just ends at month end.
        std::pair<std::string, std::string> monthSpan(int year, int month)
        {
            std::string y = std::to_string(year);
            return {y + ":" + two(month) + ":01 00:00:00",
                    y + ":" + two(month) + ":31 23:59:59"};
        }
    }

    LlmFactValidator::LlmFactValidator(ExifKeywordConfig keywords,
                                       const LocationGeocoder *geocoder,
                                       PersonResolver person_resolver,
                                       PlaceResolver place_resolver,
                                       Options options)
        : keywords_(std::move(keywords)), geocoder_(geocoder),
          person_resolver_(std::move(person_resolver)),
          place_resolver_(std::move(place_resolver)), options_(options)
    {
    }

    LlmFactValidator::Result
    LlmFactValidator::validate(const std::string &raw_query,
                               const LlmQueryStructurer::RawOutput &raw) const
    {
        Poco::Logger &logger = Poco::Logger::get("LlmFactValidator");
        Result result;
        StructuredQuery &sq = result.query;
        const std::string query_n = normalize(raw_query);

        auto drop = [&](const LlmQueryStructurer::RawFact &f, const char *why) {
            ++result.dropped;
            logger.information("Dropped %s fact (%s): span='%s'", f.dim,
                               std::string(why), f.span);
        };

        auto monthNo = [&](const std::string &name) {
            for (std::size_t i = 0; i < keywords_.months.size(); ++i)
                if (name == keywords_.months[i])
                    return static_cast<int>(i) + 1;
            return 0;
        };

        std::vector<std::string> accepted_spans;
        // Review fix: place ATTEMPTS the model made that resolved to nothing
        // ("in mountain view") — the detector must not re-read their words
        // as settings ("mountain").
        std::vector<std::string> place_attempts;
        // PLACE-4: a setting fact's span IS the setting word ("waterfronts").
        // It must stay in the clip queries (the prompt says so, and the regex
        // parser leaves it in the CLIP text), so setting spans are kept apart
        // from `accepted_spans` (which are stripped from clip queries below)
        // and only join them for the detector's covered-spans check.
        std::vector<std::string> setting_spans;
        // PLACE-4: settings are added through one gate so the model's own
        // facts, converted location facts and the deterministic detector
        // never duplicate a category.
        auto addSetting = [&](const ExifKeywordConfig::SettingCategory &cat, const std::string &op,
                              bool inferred_setting) {
            for (const auto &existing : sq.bound.settings)
                if (existing.category == cat.name)
                    return;
            ExifQueryConstraints::Setting st;
            st.category = cat.name;
            st.fcodes = cat.codes;
            st.shoreline = cat.shoreline;
            st.max_m = op == "near" ? options_.setting_near_m : options_.setting_at_m;
            st.inferred = inferred_setting;
            sq.bound.settings.push_back(std::move(st));
        };
        for (const auto &f : raw.facts)
        {
            // --- Grounding: span in query, every value in span -----------
            std::string span_n = normalize(f.span);
            // PLACE-2 §6.1: a bare place name ("yellowstone waterfalls").
            // The model reliably labels it a location but invents the
            // span it "should" have had ("in yellowstone"), so the span
            // fails grounding. When the VALUE itself is in the query the
            // claim is checkable — accept it as an inferred location and
            // let the resolution gate below decide (it must name a place
            // the library has photos in).
            bool inferred = false;
            if (span_n.empty() || query_n.find(span_n) == std::string::npos)
            {
                std::string value_n = f.values.size() == 1 ? normalize(f.values[0]) : std::string();
                if (f.dim == "location" && !value_n.empty() &&
                    query_n.find(value_n) != std::string::npos)
                {
                    inferred = true;
                    span_n = value_n;
                }
                else
                {
                    drop(f, "span not grounded in query");
                    continue;
                }
            }
            // A grounded location whose span carries no location
            // preposition ("turkey dinner", "yellowstone waterfalls") is
            // equally the model's inference — the user never said "in".
            // PLACE-4: the same for a setting with no scaffolding ("beach
            // photos" vs "on the beach" / "near the lake").
            if ((f.dim == "location" || f.dim == "setting") && !inferred)
            {
                static const char *kSettingScaffold[] = {"on", "at", "by", "near", "along",
                                                         "beside", "close", "next", "around"};
                bool scaffolded = false;
                std::istringstream words(span_n);
                for (std::string w; words >> w && !scaffolded;)
                {
                    for (const auto &prep : keywords_.location_prepositions)
                        if (w == prep)
                            scaffolded = true;
                    if (f.dim == "setting")
                        for (const char *sc : kSettingScaffold)
                            if (w == sc)
                                scaffolded = true;
                }
                inferred = !scaffolded;
            }
            bool values_grounded = true;
            for (const auto &v : f.values)
            {
                if (span_n.find(normalize(v)) == std::string::npos)
                {
                    values_grounded = false;
                    break;
                }
            }
            if (!values_grounded)
            {
                drop(f, "value not grounded in span");
                continue;
            }

            // A value-carrying fact with NO values makes no claim: there is
            // nothing to verify and nothing to lose — skip it as noise
            // without counting a drop (a drop feeds the distrust-the-parse
            // heuristic, and noise is not a lie).
            if (f.values.empty() && f.dim != "flash")
            {
                logger.information("Skipped valueless %s fact (noise): span='%s'",
                                   f.dim, f.span);
                continue;
            }

            // An inferred place/setting is always a bound filter (§6.1):
            // "explore" would union every photo there.
            const bool filter = (f.intent == "filter") || inferred ||
                                f.dim == "setting";
            if (!filter && f.intent != "explore")
            {
                drop(f, "unknown intent");
                continue;
            }

            // A validated fact lands either in the bound set (filter) or in
            // its own bare union branch (explore).
            ExifQueryConstraints branch;
            ExifQueryConstraints &target = filter ? sq.bound : branch;
            bool ok = false;

            // --- Per-dimension checks and conversion --------------------
            if (f.dim == "camera" && f.op == "in" && !f.values.empty())
            {
                for (const auto &v : f.values)
                    target.camera_terms.push_back(normalize(v));
                ok = true;
            }
            else if (f.dim == "file_type" && f.op == "in" && !f.values.empty())
            {
                std::vector<std::string> exts;
                bool all_known = true;
                for (const auto &v : f.values)
                {
                    const std::string name = normalize(v);
                    auto it = std::find_if(keywords_.file_types.begin(),
                                           keywords_.file_types.end(),
                                           [&](const auto &t) { return t.first == name; });
                    if (it == keywords_.file_types.end())
                    {
                        all_known = false;
                        break;
                    }
                    exts.insert(exts.end(), it->second.begin(), it->second.end());
                }
                if (all_known)
                {
                    target.file_extensions.insert(target.file_extensions.end(),
                                                  exts.begin(), exts.end());
                    ok = true;
                }
            }
            else if (f.dim == "iso" || f.dim == "aperture" || f.dim == "focal")
            {
                struct Spec
                {
                    const char *column;
                    double lo, hi;   // sanity bounds
                    double eq_band;  // half-width of the "eq" match band
                    bool eq_band_relative;
                };
                const Spec spec = f.dim == "iso"
                                      ? Spec{"exif_iso", 25.0, 1e6, 0.0, false}
                                      : f.dim == "aperture"
                                            ? Spec{"exif_f_number", 0.7, 64.0, 0.3, false}
                                            : Spec{"exif_focal_length", 1.0, 5000.0, 1.0, false};

                std::vector<double> nums;
                bool parsed = !f.values.empty();
                for (const auto &v : f.values)
                {
                    double d = 0;
                    if (!parseNumber(stripUnits(f.dim, normalize(v)), d) ||
                        d < spec.lo || d > spec.hi)
                    {
                        parsed = false;
                        break;
                    }
                    nums.push_back(d);
                }
                if (parsed)
                {
                    if (f.op == "eq" && nums.size() == 1)
                    {
                        if (spec.eq_band > 0.0)
                            target.numeric.push_back({spec.column, "between",
                                                      nums[0] - spec.eq_band,
                                                      nums[0] + spec.eq_band});
                        else
                            target.numeric.push_back({spec.column, "=", nums[0], 0.0});
                        ok = true;
                    }
                    else if ((f.op == "gt" || f.op == "gte" || f.op == "lt" ||
                              f.op == "lte") &&
                             nums.size() == 1)
                    {
                        const char *op = f.op == "gt"    ? ">"
                                         : f.op == "gte" ? ">="
                                         : f.op == "lt"  ? "<"
                                                         : "<=";
                        target.numeric.push_back({spec.column, op, nums[0], 0.0});
                        ok = true;
                    }
                    else if (f.op == "range" && nums.size() == 2)
                    {
                        target.numeric.push_back({spec.column, "between",
                                                  std::min(nums[0], nums[1]),
                                                  std::max(nums[0], nums[1])});
                        ok = true;
                    }
                }
            }
            else if (f.dim == "shutter")
            {
                std::vector<double> secs;
                bool parsed = !f.values.empty();
                for (const auto &v : f.values)
                {
                    double d = 0;
                    std::string compact = stripUnits("shutter", normalize(v));
                    compact.erase(std::remove(compact.begin(), compact.end(), ' '),
                                  compact.end());
                    if (!parseShutterSeconds(compact, d) || d < 1.0 / 64000.0 ||
                        d > 3600.0)
                    {
                        parsed = false;
                        break;
                    }
                    secs.push_back(d);
                }
                if (parsed)
                {
                    if (f.op == "eq" && secs.size() == 1)
                    {
                        target.numeric.push_back({"exif_exposure_time", "between",
                                                  secs[0] * 0.7, secs[0] * 1.4});
                        ok = true;
                    }
                    else if ((f.op == "faster" || f.op == "slower") && secs.size() == 1)
                    {
                        // Faster shutter = SHORTER exposure time.
                        target.numeric.push_back({"exif_exposure_time",
                                                  f.op == "faster" ? "<" : ">",
                                                  secs[0], 0.0});
                        ok = true;
                    }
                    else if (f.op == "range" && secs.size() == 2)
                    {
                        target.numeric.push_back({"exif_exposure_time", "between",
                                                  std::min(secs[0], secs[1]),
                                                  std::max(secs[0], secs[1])});
                        ok = true;
                    }
                }
            }
            else if (f.dim == "flash" && (f.op == "on" || f.op == "off"))
            {
                (f.op == "on" ? target.flash_on : target.flash_off) = true;
                ok = true;
            }
            else if (f.dim == "date")
            {
                std::vector<std::string> vals;
                for (const auto &v : f.values)
                    vals.push_back(normalize(v));
                // The model sometimes splits "august 2024" into two values;
                // recombine month+year regardless of order.
                if (f.op == "eq" && vals.size() == 2)
                {
                    int m0 = monthNo(vals[0]), m1 = monthNo(vals[1]), y = 0;
                    if (m0 > 0 && parseYear(vals[1], y))
                        vals = {vals[0] + " " + vals[1]};
                    else if (m1 > 0 && parseYear(vals[0], y))
                        vals = {vals[1] + " " + vals[0]};
                }
                if (f.op == "eq" && vals.size() == 1)
                {
                    const std::string v = vals[0];
                    int year = 0;
                    // "august 2024" | "2024" | "august"
                    auto space = v.find(' ');
                    if (space != std::string::npos)
                    {
                        int m = monthNo(v.substr(0, space));
                        if (m > 0 && parseYear(v.substr(space + 1), year))
                        {
                            target.date_range = monthSpan(year, m);
                            ok = true;
                        }
                    }
                    else if (parseYear(v, year))
                    {
                        // A bare-year value whose grounded SPAN names a month
                        // ("from august 2024" -> value "2024") refines to
                        // that month — the span is proven query text.
                        int span_month = 0;
                        {
                            std::istringstream words(span_n);
                            for (std::string w; words >> w;)
                                if (int m = monthNo(w))
                                {
                                    span_month = span_month == 0 ? m : -1;
                                    break;
                                }
                        }
                        target.date_range = span_month > 0
                                                ? monthSpan(year, span_month)
                                                : yearSpan(year, year);
                        ok = true;
                    }
                    else if (int m = monthNo(v); m > 0)
                    {
                        target.month_any_year = two(m);
                        ok = true;
                    }
                }
                else if (f.op == "range" && vals.size() == 2)
                {
                    int a = 0, b = 0;
                    if (parseYear(vals[0], a) && parseYear(vals[1], b))
                    {
                        target.date_range = yearSpan(std::min(a, b), std::max(a, b));
                        ok = true;
                    }
                }
            }
            else if (f.dim == "person" && f.op == "in" && !f.values.empty())
            {
                // The location playbook: resolved names filter, unresolved
                // names demote to CLIP so the semantic content survives
                // ("photos of ziggy the dog" still searches visually).
                std::vector<std::string> unresolved;
                for (const auto &v : f.values)
                {
                    const std::string name = normalize(v);
                    const int pid = person_resolver_ ? person_resolver_(name) : 0;
                    if (pid > 0)
                        target.person_ids.push_back(pid);
                    else
                        unresolved.push_back(name);
                }
                if (!target.person_ids.empty())
                {
                    ok = true;
                    for (const auto &n : unresolved)
                        sq.clip_queries.push_back(n);
                }
                else
                {
                    for (const auto &n : unresolved)
                        sq.clip_queries.push_back(n);
                    drop(f, "no value matched a named person; demoted to clip query");
                    continue;
                }
            }
            else if (f.dim == "face_count")
            {
                auto parseCount = [](const std::string &v, int &out) {
                    try
                    {
                        std::size_t used = 0;
                        out = std::stoi(v, &used);
                        return used == v.size() && out >= 0 && out <= 50;
                    }
                    catch (...)
                    {
                        return false;
                    }
                };
                if ((f.op == "eq" || f.op == "gt" || f.op == "lt") && f.values.size() == 1)
                {
                    int n = 0;
                    if (parseCount(f.values[0], n))
                    {
                        ExifQueryConstraints::Numeric c;
                        c.column = "face_count";
                        c.op = f.op == "eq" ? "=" : (f.op == "gt" ? ">" : "<");
                        c.value = n;
                        target.numeric.push_back(c);
                        ok = true;
                    }
                }
                else if (f.op == "range" && f.values.size() == 2)
                {
                    int a = 0, b = 0;
                    if (parseCount(f.values[0], a) && parseCount(f.values[1], b))
                    {
                        ExifQueryConstraints::Numeric c;
                        c.column = "face_count";
                        c.op = "between";
                        c.value = std::min(a, b);
                        c.value2 = std::max(a, b);
                        target.numeric.push_back(c);
                        ok = true;
                    }
                }
            }
            else if (f.dim == "setting" && (f.op == "at" || f.op == "near") && !f.values.empty())
            {
                // PLACE-4: every value must be a known category; the op
                // picks the radius. Unknown categories drop the fact (the
                // word stays in the model's clip text anyway).
                bool all_known = true;
                std::vector<ExifQueryConstraints::Setting> parsed;
                for (const auto &v : f.values)
                {
                    const auto *cat = keywords_.findSetting(normalize(v));
                    if (!cat)
                    {
                        all_known = false;
                        break;
                    }
                    ExifQueryConstraints::Setting st;
                    st.category = cat->name;
                    st.fcodes = cat->codes;
                    st.shoreline = cat->shoreline;
                    st.max_m = f.op == "at" ? options_.setting_at_m : options_.setting_near_m;
                    st.inferred = inferred;
                    parsed.push_back(std::move(st));
                }
                if (all_known)
                {
                    for (auto &st : parsed)
                    {
                        const auto *cat = keywords_.findSetting(st.category);
                        addSetting(*cat, f.op, st.inferred);
                    }
                    ok = true;
                }
            }
            else if (f.dim == "location" && f.op == "eq" && f.values.size() == 1 &&
                     keywords_.findSetting(stripLeadingArticles(normalize(f.values[0]))))
            {
                // PLACE-4: the model filed a setting word as a place ("in
                // the mountains" -> location=mountains). It is a setting;
                // the span's scaffold decides at/near.
                const auto *cat = keywords_.findSetting(stripLeadingArticles(normalize(f.values[0])));
                const std::string op = settingOpForSpan(span_n);
                addSetting(*cat, op.empty() ? "at" : op, op.empty());
                ++result.accepted;
                setting_spans.push_back(span_n);
                continue;
            }
            else if (f.dim == "location" && f.op == "eq" && f.values.size() == 1)
            {
                const std::string name = normalize(f.values[0]);
                std::optional<LocationGeocoder::Bounds> bounds;
                if (geocoder_ && geocoder_->size() > 0)
                    bounds = geocoder_->lookup(name);
                // PLACE-2: places the library has photos in (file_places).
                std::vector<int> place_ids;
                if (place_resolver_)
                    place_ids = place_resolver_(name);
                if (bounds || !place_ids.empty())
                {
                    target.location = bounds;
                    target.place_ids = std::move(place_ids);
                    target.location_name = name;
                    // Assigned, not OR-ed: an explicit "in <place>" after an
                    // inferred one must not inherit the inferred retry.
                    target.location_inferred = inferred;
                    ok = true;
                }
                if (!ok)
                {
                    if (inferred)
                    {
                        // The model's guess named nothing the library has
                        // photos in ("turkey dinner"): not a lie about the
                        // text, just not a place here. Skip without a drop
                        // (a drop feeds the distrust-the-parse heuristic)
                        // and without a clip demotion — the model's own
                        // clip_queries already carry the full text.
                        logger.information("Skipped inferred location '%s' (no photos there)", name);
                        place_attempts.push_back(span_n);
                        continue;
                    }
                    // Unresolvable place: the semantic content must survive —
                    // demote to a CLIP query instead of silently filtering
                    // to zero results.
                    sq.clip_queries.push_back(name);
                    place_attempts.push_back(span_n);
                    drop(f, "location not in gazetteer; demoted to clip query");
                    continue;
                }
            }

            if (!ok)
            {
                drop(f, "failed dimension validation");
                continue;
            }
            ++result.accepted;
            (f.dim == "setting" ? setting_spans : accepted_spans).push_back(span_n);
            if (!filter && branch.hasAny())
                sq.bare.push_back(std::move(branch));
        }

        // --- PLACE-4: settings the model missed ---------------------------
        // The closed setting vocabulary is detected in code regardless of
        // what the model emitted; spans already claimed by accepted facts
        // (place names such as "salt lake city") are skipped.
        {
            std::vector<std::string> covered = accepted_spans;
            covered.insert(covered.end(), setting_spans.begin(), setting_spans.end());
            covered.insert(covered.end(), place_attempts.begin(), place_attempts.end());
            for (const auto &d : detectSettings(query_n, keywords_, covered))
                if (const auto *cat = keywords_.findSetting(d.category))
                    addSetting(*cat, d.op, d.inferred);
        }

        // --- clip_queries: grounded, capped ------------------------------
        for (const auto &q : raw.clip_queries)
        {
            if (sq.clip_queries.size() >= 4)
                break;
            std::string q_n = normalize(q);
            if (q_n.empty())
                continue;
            if (query_n.find(q_n) == std::string::npos)
            {
                logger.information("Dropped ungrounded clip query: '%s'", q);
                continue;
            }
            // The prompt says filter-fact text is EXCLUDED from clip
            // queries; when the model echoes it anyway (small models echo
            // the whole query), enforce the rule deterministically: strip
            // every accepted fact's span. Spans are grounded literals, so
            // plain substring removal is exact.
            for (const auto &span : accepted_spans)
            {
                for (std::size_t pos = q_n.find(span); pos != std::string::npos;
                     pos = q_n.find(span))
                    q_n.erase(pos, span.size());
            }
            q_n = normalize(q_n);
            if (!q_n.empty())
                sq.clip_queries.push_back(q_n);
        }

        // parse()-compatibility fields of StructuredQuery.
        std::string joined;
        for (const auto &q : sq.clip_queries)
            joined += (joined.empty() ? "" : " ") + q;
        sq.remaining = joined;
        sq.remaining_is_filler = sq.clip_queries.empty();
        return result;
    }
}
