#pragma once

#include "UsageModels.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tokenometer
{
    enum class TrendDimension
    {
        Tool,
        Model
    };

    struct TrendStackValue
    {
        std::wstring series;
        int64_t value{};
    };

    struct TrendDay
    {
        std::wstring day;
        std::vector<TrendStackValue> values;
        int64_t total{};
    };

    struct TrendLegendItem
    {
        std::wstring series;
        int64_t total{};
        double percent{};
    };

    struct TrendHeatmapDay
    {
        std::wstring day;
        int64_t value{};
    };

    struct TrendCandleData
    {
        std::wstring day;
        std::wstring series;
        int64_t open{};
        int64_t high{};
        int64_t low{};
        int64_t close{};
        int64_t volume{};
    };

    struct TrendAnalyticsResult
    {
        std::vector<TrendDay> stackedDays;
        std::vector<TrendLegendItem> legend;
        std::vector<TrendHeatmapDay> heatmap;
        int currentStreak{};
        int longestStreak{};
        std::vector<TrendCandleData> candles;
    };

    [[nodiscard]] TrendAnalyticsResult AnalyzeTrends(
        std::vector<DailyUsage> const& daily,
        std::vector<HourlyUsage> const& hourly,
        TrendDimension dimension,
        int rangeDays,
        std::wstring_view anchorDay = {});

    [[nodiscard]] bool TestTrendAnalytics();
}
