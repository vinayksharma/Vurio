#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief Rewrites calendar-relative date phrases into explicit ones.
     *
     * Neither query parser has a clock: "july of this year" reaches the LLM
     * structurer (and the regex fallback) as a bare month, which grounds to
     * `month_any_year` — July of EVERY year. Resolving the phrase before
     * parsing turns it into "july 2026", the month+year form both parsers
     * already understand, so the fix lands once for both rungs of the
     * fallback ladder instead of twice.
     *
     * Only calendar-aligned year and month phrases are handled. Day-scale
     * words ("today", "yesterday", "this week") are deliberately left alone:
     * the date dimension grounds to year and month spans only, so rewriting
     * them would promise a precision the algebra cannot keep.
     *
     * "last year" is read as the previous CALENDAR year, not a rolling
     * 12 months — the reading people mean when browsing a photo library by
     * date.
     */
    class RelativeDateResolver
    {
    public:
        /// @param months 12 lowercase month names, january..december order
        ///        (the same vocabulary the parsers were built with). Any
        ///        other size returns the query untouched.
        /// @param now   clock reading to resolve against; injectable so the
        ///        behaviour is testable without waiting for a year to pass.
        static std::string resolve(const std::string &query,
                                   const std::vector<std::string> &months,
                                   std::time_t now = std::time(nullptr));
    };
}
