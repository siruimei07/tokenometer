#include "TrendAnalytics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace tokenometer
{
    namespace
    {
        using Day = std::chrono::sys_days;

        constexpr size_t maximumSeries = 5;
        constexpr int heatmapDays = 365;
        constexpr std::wstring_view otherSeries = L"其他";
        constexpr std::wstring_view uncategorizedSeries = L"未分类";

        std::optional<Day> ParseDay(std::wstring_view text)
        {
            if (text.size() != 10 || text[4] != L'-' || text[7] != L'-')
            {
                return std::nullopt;
            }
            auto number = [&](size_t offset, size_t length) -> std::optional<unsigned>
            {
                unsigned value{};
                for (size_t index = offset; index < offset + length; ++index)
                {
                    if (text[index] < L'0' || text[index] > L'9') return std::nullopt;
                    value = value * 10 + static_cast<unsigned>(text[index] - L'0');
                }
                return value;
            };
            auto const year = number(0, 4);
            auto const month = number(5, 2);
            auto const day = number(8, 2);
            if (!year || !month || !day) return std::nullopt;
            std::chrono::year_month_day const value{
                std::chrono::year{ static_cast<int>(*year) },
                std::chrono::month{ *month },
                std::chrono::day{ *day }
            };
            return value.ok() ? std::optional<Day>{ Day{ value } } : std::nullopt;
        }

        std::wstring FormatDay(Day day)
        {
            std::chrono::year_month_day const value{ day };
            std::array<wchar_t, 11> text{};
            swprintf_s(
                text.data(),
                text.size(),
                L"%04d-%02u-%02u",
                static_cast<int>(value.year()),
                static_cast<unsigned>(value.month()),
                static_cast<unsigned>(value.day()));
            return text.data();
        }

        int64_t UsageValue(TokenCounts const& counts)
        {
            return std::max<int64_t>(counts.DisplayTotal(), 0);
        }

        std::wstring Series(DailyUsage const& row, TrendDimension dimension)
        {
            auto const& value = dimension == TrendDimension::Tool ? row.tool : row.model;
            return value.empty() ? std::wstring(uncategorizedSeries) : value;
        }

        std::wstring Series(HourlyUsage const& row, TrendDimension dimension)
        {
            auto const& value = dimension == TrendDimension::Tool ? row.tool : row.model;
            return value.empty() ? std::wstring(uncategorizedSeries) : value;
        }

        struct Range
        {
            Day first;
            Day last;
            bool valid{};
        };

        Day LocalToday()
        {
            std::time_t const now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            return Day{ std::chrono::year_month_day{
                std::chrono::year{ local.tm_year + 1900 },
                std::chrono::month{ static_cast<unsigned>(local.tm_mon + 1) },
                std::chrono::day{ static_cast<unsigned>(local.tm_mday) }
            } };
        }

        Range DateRange(
            std::vector<DailyUsage> const& daily,
            std::vector<HourlyUsage> const& hourly,
            int rangeDays,
            std::wstring_view anchorText)
        {
            std::optional<Day> first;
            std::optional<Day> const requestedAnchor = anchorText.empty()
                ? std::optional<Day>{ LocalToday() }
                : ParseDay(anchorText);
            if (!requestedAnchor) return {};
            Day const last = *requestedAnchor;
            auto include = [&](std::wstring_view text)
            {
                auto const day = ParseDay(text);
                if (!day || *day > last) return;
                first = first ? std::min(*first, *day) : *day;
            };
            for (auto const& row : daily) include(row.day);
            for (auto const& row : hourly) include(row.day);
            if (rangeDays > 0)
            {
                first = last - std::chrono::days{ rangeDays - 1 };
            }
            return { first.value_or(last), last, true };
        }

        bool InRange(Day day, Range const& range)
        {
            return range.valid && day >= range.first && day <= range.last;
        }

        struct SeriesSelection
        {
            std::vector<std::wstring> visible;
            std::unordered_map<std::wstring, std::wstring> display;
        };

        SeriesSelection SelectSeries(std::map<std::wstring, int64_t> const& totals)
        {
            std::vector<std::pair<std::wstring, int64_t>> ranked(totals.begin(), totals.end());
            std::sort(ranked.begin(), ranked.end(), [](auto const& left, auto const& right)
            {
                return left.second != right.second
                    ? left.second > right.second
                    : left.first < right.first;
            });

            SeriesSelection result;
            size_t const namedCount = ranked.size() > maximumSeries ? maximumSeries - 1 : ranked.size();
            for (size_t index = 0; index < namedCount; ++index)
            {
                result.visible.push_back(ranked[index].first);
                result.display.emplace(ranked[index].first, ranked[index].first);
            }
            if (ranked.size() > maximumSeries)
            {
                if (std::find(result.visible.begin(), result.visible.end(), otherSeries) == result.visible.end())
                {
                    result.visible.emplace_back(otherSeries);
                }
                for (size_t index = namedCount; index < ranked.size(); ++index)
                {
                    result.display.emplace(ranked[index].first, otherSeries);
                }
            }
            return result;
        }

        std::wstring DisplaySeries(
            std::wstring const& source,
            SeriesSelection const& selection)
        {
            auto const found = selection.display.find(source);
            return found == selection.display.end() ? std::wstring{} : found->second;
        }
    }

    TrendAnalyticsResult AnalyzeTrends(
        std::vector<DailyUsage> const& daily,
        std::vector<HourlyUsage> const& hourly,
        TrendDimension dimension,
        int rangeDays,
        std::wstring_view anchorDay)
    {
        TrendAnalyticsResult result;
        Range const range = DateRange(daily, hourly, rangeDays, anchorDay);
        if (!range.valid) return result;

        std::map<std::pair<Day, std::wstring>, int64_t> dailySeries;
        std::map<Day, int64_t> dailyTotals;
        std::map<std::wstring, int64_t> seriesTotals;
        for (auto const& row : daily)
        {
            auto const day = ParseDay(row.day);
            int64_t const value = UsageValue(row.counts);
            if (!day || *day > range.last || value == 0) continue;
            dailyTotals[*day] += value;
            if (!InRange(*day, range)) continue;
            auto const series = Series(row, dimension);
            dailySeries[{ *day, series }] += value;
            seriesTotals[series] += value;
        }

        SeriesSelection const selection = SelectSeries(seriesTotals);
        std::map<std::pair<Day, std::wstring>, int64_t> visibleDaily;
        std::map<std::wstring, int64_t> visibleTotals;
        for (auto const& [key, value] : dailySeries)
        {
            auto const display = DisplaySeries(key.second, selection);
            if (display.empty()) continue;
            visibleDaily[{ key.first, display }] += value;
            visibleTotals[display] += value;
        }

        for (Day day = range.first; day <= range.last; day += std::chrono::days{ 1 })
        {
            TrendDay item;
            item.day = FormatDay(day);
            for (auto const& series : selection.visible)
            {
                int64_t const value = visibleDaily[{ day, series }];
                item.values.push_back({ series, value });
                item.total += value;
            }
            result.stackedDays.push_back(std::move(item));
        }

        int64_t overall{};
        for (auto const& [series, total] : visibleTotals) overall += total;
        for (auto const& series : selection.visible)
        {
            int64_t const total = visibleTotals[series];
            result.legend.push_back({
                series,
                total,
                overall > 0 ? static_cast<double>(total) * 100.0 / static_cast<double>(overall) : 0.0
            });
        }

        Day const heatmapFirst = range.last - std::chrono::days{ heatmapDays - 1 };
        int runningStreak{};
        for (Day day = heatmapFirst; day <= range.last; day += std::chrono::days{ 1 })
        {
            int64_t const value = dailyTotals[day];
            result.heatmap.push_back({ FormatDay(day), value });
            if (value > 0)
            {
                ++runningStreak;
                result.longestStreak = std::max(result.longestStreak, runningStreak);
            }
            else
            {
                runningStreak = 0;
            }
        }
        for (auto iterator = result.heatmap.rbegin(); iterator != result.heatmap.rend() && iterator->value > 0; ++iterator)
        {
            ++result.currentStreak;
        }

        std::map<std::tuple<Day, int64_t, std::wstring>, int64_t> hourlySeries;
        for (auto const& row : hourly)
        {
            auto const day = ParseDay(row.day);
            int64_t const value = UsageValue(row.counts);
            if (!day || !InRange(*day, range) || value == 0) continue;
            auto const display = DisplaySeries(Series(row, dimension), selection);
            if (display.empty()) continue;
            hourlySeries[{ *day, row.hourStart, display }] += value;
        }

        std::map<std::pair<Day, std::wstring>, std::vector<std::pair<int64_t, int64_t>>> candleHours;
        for (auto const& [key, value] : hourlySeries)
        {
            auto const& [day, hour, series] = key;
            candleHours[{ day, series }].push_back({ hour, value });
        }
        for (auto& [key, hours] : candleHours)
        {
            if (hours.empty()) continue;
            std::sort(hours.begin(), hours.end());
            TrendCandleData candle;
            candle.day = FormatDay(key.first);
            candle.series = key.second;
            candle.open = hours.front().second;
            candle.close = hours.back().second;
            candle.high = hours.front().second;
            candle.low = hours.front().second;
            for (auto const& [hour, value] : hours)
            {
                static_cast<void>(hour);
                candle.high = std::max(candle.high, value);
                candle.low = std::min(candle.low, value);
                candle.volume += value;
            }
            result.candles.push_back(std::move(candle));
        }
        return result;
    }

    bool TestTrendAnalytics()
    {
        auto daily = [](std::wstring day, std::wstring series, int64_t value)
        {
            DailyUsage row;
            row.day = std::move(day);
            row.tool = series;
            row.model = L"model-" + series;
            row.counts.reportedTotal = value;
            return row;
        };
        std::vector<DailyUsage> days{
            daily(L"2026-01-01", L"A", 70), daily(L"2026-01-01", L"A", 30),
            daily(L"2026-01-01", L"B", 80), daily(L"2026-01-01", L"C", 60),
            daily(L"2026-01-01", L"D", 40), daily(L"2026-01-01", L"E", 20),
            daily(L"2026-01-01", L"F", 10), daily(L"2026-01-02", L"A", 50),
            daily(L"2026-01-04", L"A", 30)
        };

        auto hourly = [](std::wstring day, int64_t hour, std::wstring series, int64_t value)
        {
            HourlyUsage row;
            row.day = std::move(day);
            row.hourStart = hour;
            row.tool = series;
            row.model = L"model-" + series;
            row.counts.reportedTotal = value;
            return row;
        };
        std::vector<HourlyUsage> hours{
            hourly(L"2026-01-01", 1000, L"A", 10),
            hourly(L"2026-01-01", 1000, L"A", 5),
            hourly(L"2026-01-01", 2000, L"A", 20),
            hourly(L"2026-01-01", 3000, L"A", 8),
            hourly(L"2026-01-01", 1000, L"E", 2),
            hourly(L"2026-01-01", 1000, L"F", 3),
            hourly(L"2026-01-01", 2000, L"E", 7),
            // The UTC bucket intentionally belongs to another date; row.day is the local-device day.
            hourly(L"2026-01-04", 400, L"A", 11)
        };

        auto const result = AnalyzeTrends(days, hours, TrendDimension::Tool, 4, L"2026-01-04");
        if (result.legend.size() != maximumSeries || result.stackedDays.size() != 4 ||
            result.heatmap.size() != heatmapDays || result.currentStreak != 1 || result.longestStreak != 2)
        {
            return false;
        }
        auto findLegend = [&](std::wstring_view series) -> TrendLegendItem const*
        {
            auto const found = std::find_if(result.legend.begin(), result.legend.end(), [&](auto const& item)
            {
                return item.series == series;
            });
            return found == result.legend.end() ? nullptr : &*found;
        };
        auto const other = findLegend(otherSeries);
        double percentTotal{};
        for (auto const& item : result.legend) percentTotal += item.percent;
        if (!other || other->total != 30 || result.legend.front().series != L"A" ||
            result.legend.front().total != 180 || std::abs(percentTotal - 100.0) > 0.001)
        {
            return false;
        }
        auto const gap = std::find_if(result.stackedDays.begin(), result.stackedDays.end(), [](auto const& day)
        {
            return day.day == L"2026-01-03";
        });
        if (gap == result.stackedDays.end() || gap->total != 0) return false;
        auto const heatmapGap = std::find_if(result.heatmap.begin(), result.heatmap.end(), [](auto const& day)
        {
            return day.day == L"2026-01-03";
        });
        auto const heatmapLatest = std::find_if(result.heatmap.begin(), result.heatmap.end(), [](auto const& day)
        {
            return day.day == L"2026-01-04";
        });
        if (heatmapGap == result.heatmap.end() || heatmapGap->value != 0 ||
            heatmapLatest == result.heatmap.end() || heatmapLatest->value != 30)
        {
            return false;
        }

        auto findCandle = [&](std::wstring_view day, std::wstring_view series) -> TrendCandleData const*
        {
            auto const found = std::find_if(result.candles.begin(), result.candles.end(), [&](auto const& candle)
            {
                return candle.day == day && candle.series == series;
            });
            return found == result.candles.end() ? nullptr : &*found;
        };
        auto const first = findCandle(L"2026-01-01", L"A");
        auto const otherCandle = findCandle(L"2026-01-01", otherSeries);
        auto const localDay = findCandle(L"2026-01-04", L"A");
        if (!first || first->open != 15 || first->high != 20 || first->low != 8 ||
            first->close != 8 || first->volume != 43 ||
            !otherCandle || otherCandle->open != 5 || otherCandle->close != 7 ||
            otherCandle->volume != 12 || !localDay || localDay->volume != 11 ||
            findCandle(L"2026-01-03", L"A"))
        {
            return false;
        }

        auto const byModel = AnalyzeTrends(days, hours, TrendDimension::Model, 4, L"2026-01-04");
        if (byModel.legend.empty() || byModel.legend.front().series != L"model-A") return false;

        auto futureDays = days;
        futureDays.push_back(daily(L"2026-01-06", L"future", 10'000));
        auto const nextDay = AnalyzeTrends(futureDays, hours, TrendDimension::Tool, 5, L"2026-01-05");
        return nextDay.currentStreak == 0 &&
               std::none_of(nextDay.legend.begin(), nextDay.legend.end(), [](auto const& item)
               {
                   return item.series == L"future";
               }) &&
               std::none_of(nextDay.candles.begin(), nextDay.candles.end(), [](auto const& candle)
               {
                   return candle.day > L"2026-01-05";
               });
    }
}
