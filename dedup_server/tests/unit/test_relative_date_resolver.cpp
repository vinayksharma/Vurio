#include <gtest/gtest.h>
#include <ctime>
#include "search/relative_date_resolver.hpp"

using namespace MediaDedup;

namespace
{
    const std::vector<std::string> kMonths = {
        "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december"};

    // A fixed local-time reading so the expectations never drift: the given
    // Y/M/D at midday, built through mktime so it lands in the test host's
    // own timezone (which is what the resolver reads it back in).
    std::time_t localTime(int year, int month, int day)
    {
        std::tm t{};
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = 12;
        t.tm_isdst = -1;
        return std::mktime(&t);
    }

    std::string resolve(const std::string &query, std::time_t now)
    {
        return RelativeDateResolver::resolve(query, kMonths, now);
    }
}

// The reported bug: a bare month grounds to that month in EVERY year, so
// "this year" has to become a real year before the parsers see it.
TEST(RelativeDateResolverTest, MonthOfThisYearBecomesMonthAndYear)
{
    EXPECT_EQ(resolve("pictures from july of this year", localTime(2026, 9, 6)),
              "pictures from july 2026");
}

TEST(RelativeDateResolverTest, BareThisYearBecomesTheYear)
{
    EXPECT_EQ(resolve("photos from this year", localTime(2026, 9, 6)),
              "photos from 2026");
}

TEST(RelativeDateResolverTest, LastYearIsThePreviousCalendarYear)
{
    EXPECT_EQ(resolve("beach photos from last year", localTime(2026, 1, 2)),
              "beach photos from 2025");
    EXPECT_EQ(resolve("beach photos from the previous year", localTime(2026, 1, 2)),
              "beach photos from 2025");
}

TEST(RelativeDateResolverTest, ThisMonthBecomesMonthAndYear)
{
    EXPECT_EQ(resolve("pictures taken this month", localTime(2026, 9, 6)),
              "pictures taken september 2026");
}

// December of the year before — the wrap is the case a naive month-1 breaks.
TEST(RelativeDateResolverTest, LastMonthWrapsAcrossTheYearBoundary)
{
    EXPECT_EQ(resolve("pictures from last month", localTime(2026, 1, 15)),
              "pictures from december 2025");
    EXPECT_EQ(resolve("pictures from last month", localTime(2026, 9, 6)),
              "pictures from august 2026");
}

TEST(RelativeDateResolverTest, IsCaseInsensitive)
{
    EXPECT_EQ(resolve("Pictures From July Of This Year", localTime(2026, 9, 6)),
              "Pictures From July 2026");
}

// Day-scale words ground to nothing in the date algebra, so they are left
// for CLIP rather than rewritten into a month they do not mean.
TEST(RelativeDateResolverTest, LeavesDayScaleAndAbsolutePhrasesAlone)
{
    const std::time_t now = localTime(2026, 9, 6);
    EXPECT_EQ(resolve("photos from today", now), "photos from today");
    EXPECT_EQ(resolve("photos from yesterday", now), "photos from yesterday");
    EXPECT_EQ(resolve("photos from july 2013", now), "photos from july 2013");
    EXPECT_EQ(resolve("dog on the beach", now), "dog on the beach");
}

// "yearbook"/"monthly" must not be clipped by a partial phrase match.
TEST(RelativeDateResolverTest, DoesNotMatchInsideLongerWords)
{
    const std::time_t now = localTime(2026, 9, 6);
    EXPECT_EQ(resolve("this yearbook photo", now), "this yearbook photo");
    EXPECT_EQ(resolve("this monthly report", now), "this monthly report");
}

TEST(RelativeDateResolverTest, WrongMonthVocabularySizeIsAPassThrough)
{
    EXPECT_EQ(RelativeDateResolver::resolve("photos from this year", {"january"},
                                            localTime(2026, 9, 6)),
              "photos from this year");
}
