#include "search/llm_fact_validator.hpp"

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
                                       const LocationGeocoder *geocoder)
        : keywords_(std::move(keywords)), geocoder_(geocoder)
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

        for (const auto &f : raw.facts)
        {
            // --- Grounding: span in query, every value in span -----------
            const std::string span_n = normalize(f.span);
            if (span_n.empty() || query_n.find(span_n) == std::string::npos)
            {
                drop(f, "span not grounded in query");
                continue;
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

            const bool filter = (f.intent == "filter");
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
            else if (f.dim == "location" && f.op == "eq" && f.values.size() == 1)
            {
                const std::string name = normalize(f.values[0]);
                if (geocoder_ && geocoder_->size() > 0)
                {
                    if (auto bounds = geocoder_->lookup(name))
                    {
                        target.location = bounds;
                        target.location_name = name;
                        ok = true;
                    }
                }
                if (!ok)
                {
                    // Unresolvable place: the semantic content must survive —
                    // demote to a CLIP query instead of silently filtering
                    // to zero results.
                    sq.clip_queries.push_back(name);
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
            if (!filter && branch.hasAny())
                sq.bare.push_back(std::move(branch));
        }

        // --- clip_queries: grounded, capped ------------------------------
        for (const auto &q : raw.clip_queries)
        {
            if (sq.clip_queries.size() >= 4)
                break;
            const std::string q_n = normalize(q);
            if (!q_n.empty() && query_n.find(q_n) != std::string::npos)
                sq.clip_queries.push_back(q_n);
            else if (!q_n.empty())
                logger.information("Dropped ungrounded clip query: '%s'", q);
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
