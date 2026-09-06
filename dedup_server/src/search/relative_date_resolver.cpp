#include "search/relative_date_resolver.hpp"

#include <regex>

namespace MediaDedup
{
    std::string RelativeDateResolver::resolve(const std::string &query,
                                              const std::vector<std::string> &months,
                                              std::time_t now)
    {
        if (months.size() != 12 || query.empty())
            return query;

        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &now) != 0)
            return query;
#else
        if (localtime_r(&now, &local) == nullptr)
            return query;
#endif
        const int year = local.tm_year + 1900;
        const int month = local.tm_mon + 1; // 1..12

        // Previous calendar month, wrapping into December of the year before.
        const int prev_month = month == 1 ? 12 : month - 1;
        const int prev_month_year = month == 1 ? year - 1 : year;

        const std::string this_year = std::to_string(year);
        const std::string last_year = std::to_string(year - 1);
        const std::string this_month = months[month - 1] + " " + this_year;
        const std::string last_month =
            months[prev_month - 1] + " " + std::to_string(prev_month_year);

        // A leading "of"/"the" is swallowed with the phrase so that
        // "july of this year" collapses to "july 2026" — adjacent month+year
        // is what the fallback parser's month_year_ regex requires, and what
        // the LLM reliably emits as a single date fact.
        const std::string lead = R"((?:\bof\s+)?(?:\bthe\s+)?)";
        const std::pair<std::string, const std::string *> rules[] = {
            {R"(this\s+year\b)", &this_year},
            {R"(current\s+year\b)", &this_year},
            {R"(last\s+year\b)", &last_year},
            {R"(previous\s+year\b)", &last_year},
            {R"(this\s+month\b)", &this_month},
            {R"(current\s+month\b)", &this_month},
            {R"(last\s+month\b)", &last_month},
            {R"(previous\s+month\b)", &last_month},
        };

        std::string out = query;
        for (const auto &[phrase, replacement] : rules)
        {
            const std::regex re(lead + R"(\b)" + phrase,
                                std::regex::icase | std::regex::optimize);
            out = std::regex_replace(out, re, *replacement);
        }
        return out;
    }
}
