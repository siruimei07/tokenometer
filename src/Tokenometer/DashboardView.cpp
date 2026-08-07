#include "DashboardView.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace mux = winrt::Microsoft::UI::Xaml;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;

namespace
{
    winrt::Windows::UI::Color Color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
    {
        return { alpha, red, green, blue };
    }

    constexpr auto ShellColor()
    {
        return std::array<uint8_t, 3>{ 18, 16, 17 };
    }

    media::SolidColorBrush Brush(winrt::Windows::UI::Color const& color)
    {
        return media::SolidColorBrush{ color };
    }

    mux::CornerRadius Radius(double value)
    {
        return { value, value, value, value };
    }

    mux::GridLength Pixels(double value)
    {
        return mux::GridLengthHelper::FromPixels(value);
    }

    mux::GridLength Star(double value = 1.0)
    {
        return mux::GridLengthHelper::FromValueAndType(value, mux::GridUnitType::Star);
    }

    void AddRow(controls::Grid const& grid, mux::GridLength const& height)
    {
        controls::RowDefinition row;
        row.Height(height);
        grid.RowDefinitions().Append(row);
    }

    void AddColumn(controls::Grid const& grid, mux::GridLength const& width)
    {
        controls::ColumnDefinition column;
        column.Width(width);
        grid.ColumnDefinitions().Append(column);
    }

    controls::TextBlock Text(
        std::wstring_view value,
        double size,
        winrt::Windows::UI::Color const& color,
        uint16_t weight = 400,
        bool numeric = false)
    {
        controls::TextBlock text;
        text.Text(winrt::hstring{ value });
        text.FontFamily(media::FontFamily{ numeric ? L"Cascadia Mono" : L"Segoe UI Variable Display" });
        text.FontSize(size);
        text.FontWeight({ weight });
        text.Foreground(Brush(color));
        text.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        return text;
    }

    controls::Grid Progress(
        double ratio,
        winrt::Windows::UI::Color const& color,
        controls::ColumnDefinition* fillDefinition = nullptr,
        controls::ColumnDefinition* restDefinition = nullptr)
    {
        ratio = std::clamp(ratio, 0.0, 1.0);

        controls::Grid track;
        track.Height(8);
        controls::ColumnDefinition fillColumn;
        fillColumn.Width(Star(std::max(ratio, 0.001)));
        track.ColumnDefinitions().Append(fillColumn);
        controls::ColumnDefinition restColumn;
        restColumn.Width(Star(std::max(1.0 - ratio, 0.001)));
        track.ColumnDefinitions().Append(restColumn);
        if (fillDefinition)
        {
            *fillDefinition = fillColumn;
        }
        if (restDefinition)
        {
            *restDefinition = restColumn;
        }

        controls::Border background;
        background.Background(Brush(Color(52, 49, 50)));
        background.CornerRadius(Radius(4));
        controls::Grid::SetColumnSpan(background, 2);
        track.Children().Append(background);

        controls::Border fill;
        fill.Background(Brush(color));
        fill.CornerRadius(Radius(4));
        controls::Grid::SetColumn(fill, 0);
        track.Children().Append(fill);
        return track;
    }

    void SetProgress(
        controls::ColumnDefinition const& fill,
        controls::ColumnDefinition const& rest,
        double ratio)
    {
        if (!fill || !rest)
        {
            return;
        }
        ratio = std::clamp(ratio, 0.0, 1.0);
        fill.Width(Star(std::max(ratio, 0.001)));
        rest.Width(Star(std::max(1.0 - ratio, 0.001)));
    }

    controls::Grid DynamicStatLine(
        std::wstring_view label,
        controls::TextBlock& valueTarget,
        std::wstring_view initialValue,
        winrt::Windows::UI::Color const& valueColor = Color(247, 247, 245))
    {
        controls::Grid row;
        AddColumn(row, Star());
        AddColumn(row, mux::GridLengthHelper::Auto());
        row.Children().Append(Text(label, 12, Color(154, 150, 151)));

        valueTarget = Text(initialValue, 12, valueColor, 600, true);
        valueTarget.TextAlignment(mux::TextAlignment::Right);
        controls::Grid::SetColumn(valueTarget, 1);
        row.Children().Append(valueTarget);
        return row;
    }

    controls::Border Card(
        std::wstring_view title,
        winrt::Windows::UI::Color const& accent,
        mux::UIElement const& body)
    {
        controls::Border card;
        card.Background(Brush(Color(38, 36, 37)));
        card.BorderBrush(Brush(Color(255, 255, 255, 18)));
        card.BorderThickness({ 1 });
        card.CornerRadius(Radius(20));
        card.Padding({ 22, 20, 22, 22 });

        controls::StackPanel stack;
        stack.Spacing(14);

        controls::Grid heading;
        AddColumn(heading, Pixels(12));
        AddColumn(heading, Star());

        shapes::Ellipse dot;
        dot.Width(8);
        dot.Height(8);
        dot.Fill(Brush(accent));
        dot.VerticalAlignment(mux::VerticalAlignment::Center);
        heading.Children().Append(dot);

        auto headingText = Text(title, 16, Color(247, 247, 245), 600);
        headingText.Margin({ 4, 0, 0, 0 });
        controls::Grid::SetColumn(headingText, 1);
        heading.Children().Append(headingText);

        stack.Children().Append(heading);
        stack.Children().Append(body);
        card.Child(stack);
        return card;
    }

    controls::Border SoftPanel(mux::UIElement const& child)
    {
        controls::Border panel;
        panel.Background(Brush(Color(29, 27, 28)));
        panel.CornerRadius(Radius(14));
        panel.Padding({ 14, 12, 14, 12 });
        panel.Child(child);
        return panel;
    }

    controls::StackPanel ProviderBlock(
        std::wstring_view name,
        std::wstring_view detail,
        std::wstring_view value,
        double ratio,
        winrt::Windows::UI::Color const& accent)
    {
        controls::StackPanel stack;
        stack.Spacing(8);

        controls::Grid top;
        AddColumn(top, Star());
        AddColumn(top, mux::GridLengthHelper::Auto());
        top.Children().Append(Text(name, 14, Color(247, 247, 245), 600));

        auto amount = Text(value, 13, accent, 600, true);
        controls::Grid::SetColumn(amount, 1);
        top.Children().Append(amount);
        stack.Children().Append(top);

        stack.Children().Append(Progress(ratio, accent));
        stack.Children().Append(Text(detail, 11, Color(143, 139, 140)));
        return stack;
    }

    controls::StackPanel Heatmap(
        int weeks,
        winrt::Windows::UI::Color const& accent,
        std::vector<controls::Border>* cells = nullptr,
        double cellSize = 10,
        double spacing = 3)
    {
        controls::StackPanel map;
        map.Orientation(controls::Orientation::Horizontal);
        map.Spacing(spacing);
        map.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        for (int week = 0; week < weeks; ++week)
        {
            controls::StackPanel column;
            column.Spacing(spacing);
            for (int day = 0; day < 7; ++day)
            {
                auto const active = ((week * 3 + day * 5 + week / 2) % 9) > 3;
                controls::Border cell;
                cell.Width(cellSize);
                cell.Height(cellSize);
                cell.CornerRadius(Radius(3));
                cell.Background(Brush(active ? accent : Color(55, 52, 53)));
                cell.Opacity(active ? 0.42 + 0.08 * ((week + day) % 6) : 0.5);
                column.Children().Append(cell);
                if (cells)
                {
                    cells->push_back(cell);
                }
            }
            map.Children().Append(column);
        }
        return map;
    }

    std::wstring FormatInteger(int64_t value)
    {
        auto digits = std::to_wstring(value < 0 ? -value : value);
        for (std::ptrdiff_t index = static_cast<std::ptrdiff_t>(digits.size()) - 3; index > 0; index -= 3)
        {
            digits.insert(static_cast<size_t>(index), 1, L',');
        }
        return value < 0 ? L"-" + digits : digits;
    }

    std::wstring FormatCompact(int64_t value)
    {
        double divisor = 1.0;
        wchar_t suffix = L'\0';
        if (value >= 1'000'000'000)
        {
            divisor = 1'000'000'000.0;
            suffix = L'B';
        }
        else if (value >= 1'000'000)
        {
            divisor = 1'000'000.0;
            suffix = L'M';
        }
        else if (value >= 1'000)
        {
            divisor = 1'000.0;
            suffix = L'K';
        }
        if (!suffix)
        {
            return FormatInteger(value);
        }

        std::wostringstream stream;
        stream << std::fixed << std::setprecision(value / divisor >= 100.0 ? 0 : 1)
               << value / divisor << suffix;
        return stream.str();
    }

    std::wstring FormatPercent(double value)
    {
        std::wostringstream stream;
        stream << std::fixed << std::setprecision(1) << value << L'%';
        return stream.str();
    }

    int64_t UnixNow()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::wstring FormatAge(int64_t timestamp)
    {
        if (timestamp <= 0)
        {
            return L"尚未同步";
        }
        auto const seconds = std::max<int64_t>(UnixNow() - timestamp, 0);
        if (seconds < 60)
        {
            return L"刚刚更新";
        }
        if (seconds < 3600)
        {
            return std::to_wstring(seconds / 60) + L" 分钟前";
        }
        if (seconds < 86400)
        {
            return std::to_wstring(seconds / 3600) + L" 小时前";
        }
        return std::to_wstring(seconds / 86400) + L" 天前";
    }

    std::wstring FormatReset(int64_t resetsAt)
    {
        auto const seconds = resetsAt - UnixNow();
        if (resetsAt <= 0 || seconds <= 0)
        {
            return L"等待下一次限额刷新";
        }
        auto const days = seconds / 86400;
        auto const hours = (seconds % 86400) / 3600;
        auto const minutes = (seconds % 3600) / 60;
        if (days > 0)
        {
            return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h 后重置";
        }
        if (hours > 0)
        {
            return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m 后重置";
        }
        return std::to_wstring(std::max<int64_t>(minutes, 1)) + L"m 后重置";
    }

    winrt::Windows::UI::Color SeriesColor(size_t index)
    {
        constexpr std::array<std::array<uint8_t, 3>, 5> palette{
            std::array<uint8_t, 3>{ 98, 223, 125 },
            std::array<uint8_t, 3>{ 255, 253, 142 },
            std::array<uint8_t, 3>{ 240, 63, 22 },
            std::array<uint8_t, 3>{ 153, 183, 163 },
            std::array<uint8_t, 3>{ 204, 164, 92 },
        };
        auto const& value = palette[index % palette.size()];
        return Color(value[0], value[1], value[2]);
    }

    std::wstring_view RangeLabel(tokenometer::TrendRange range)
    {
        switch (range)
        {
        case tokenometer::TrendRange::Days7:
            return L"7 days";
        case tokenometer::TrendRange::Days30:
            return L"30 days";
        case tokenometer::TrendRange::Days90:
            return L"90 days";
        case tokenometer::TrendRange::Days365:
            return L"365 days";
        }
        return L"30 days";
    }

    std::wstring FileNamePart(std::wstring const& path)
    {
        auto const separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? path : path.substr(separator + 1);
    }

    controls::Border SessionRow(
        controls::TextBlock& titleTarget,
        controls::TextBlock& detailTarget,
        controls::TextBlock& valueTarget)
    {
        controls::Grid row;
        AddColumn(row, Star());
        AddColumn(row, mux::GridLengthHelper::Auto());

        controls::StackPanel copy;
        copy.Spacing(2);
        titleTarget = Text(L"—", 12.5, Color(247, 247, 245), 600);
        detailTarget = Text(L"", 10, Color(143, 139, 140));
        copy.Children().Append(titleTarget);
        copy.Children().Append(detailTarget);
        row.Children().Append(copy);

        valueTarget = Text(L"0", 12, Color(247, 247, 245), 600, true);
        valueTarget.VerticalAlignment(mux::VerticalAlignment::Center);
        controls::Grid::SetColumn(valueTarget, 1);
        row.Children().Append(valueTarget);
        return SoftPanel(row);
    }

    controls::Border PageIntro(
        std::wstring_view eyebrow,
        std::wstring_view title,
        std::wstring_view description,
        winrt::Windows::UI::Color const& accent)
    {
        controls::Grid layout;
        AddColumn(layout, Star());
        AddColumn(layout, mux::GridLengthHelper::Auto());

        controls::StackPanel copy;
        copy.Spacing(4);
        copy.Children().Append(Text(eyebrow, 11, accent, 600));
        copy.Children().Append(Text(title, 21, Color(247, 247, 245), 650));
        copy.Children().Append(Text(description, 11.5, Color(154, 150, 151)));
        layout.Children().Append(copy);

        auto live = Text(L"LIVE", 11, Color(18, 16, 17), 700, true);
        live.HorizontalAlignment(mux::HorizontalAlignment::Center);
        live.VerticalAlignment(mux::VerticalAlignment::Center);

        controls::Border badge;
        badge.Width(60);
        badge.Height(30);
        badge.Background(Brush(accent));
        badge.CornerRadius(Radius(15));
        badge.Child(live);
        badge.VerticalAlignment(mux::VerticalAlignment::Center);
        controls::Grid::SetColumn(badge, 1);
        layout.Children().Append(badge);

        controls::Border panel;
        panel.Background(Brush(Color(38, 36, 37)));
        panel.BorderBrush(Brush(Color(255, 255, 255, 18)));
        panel.BorderThickness({ 1 });
        panel.CornerRadius(Radius(18));
        panel.Padding({ 20, 14, 20, 14 });
        panel.Child(layout);
        return panel;
    }

}

namespace tokenometer
{
DashboardView::DashboardView()
{
    BuildShell();
    UpdateOverview({});
    UpdateDetails({});
    UpdateTrends({});
    UpdateChatGptImport({});
    ShowPage(DashboardPage::Overview);
}

controls::Grid DashboardView::Root() const noexcept
{
    return m_root;
}

DashboardPage DashboardView::CurrentPage() const noexcept
{
    return m_currentPage;
}

void DashboardView::SetStatus(
    std::wstring_view status,
    std::wstring_view detail,
    bool healthy)
{
    if (!m_statusText || !m_statusDetail || !m_statusDot)
    {
        return;
    }

    m_statusText.Text(winrt::hstring{ status });
    m_statusDetail.Text(winrt::hstring{ detail });
    m_statusDetail.Visibility(detail.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    m_statusDot.Fill(Brush(healthy ? Color(98, 223, 125) : Color(240, 63, 22)));
}

void DashboardView::UpdateOverview(OverviewViewData const& data)
{
    auto const totalTokens = std::max<int64_t>(data.total.counts.DisplayTotal(), 0);
    auto const outputTokens = std::max<int64_t>(data.total.counts.output, 0);
    auto const inputTokens = std::max<int64_t>(data.total.counts.input, 0);
    auto const cachedTokens = std::clamp<int64_t>(
        data.total.counts.cachedInput,
        0,
        inputTokens);
    auto const cacheRatio = inputTokens > 0
        ? static_cast<double>(cachedTokens) / static_cast<double>(inputTokens)
        : 0.0;

    m_totalTokensText.Text(winrt::hstring{ FormatInteger(totalTokens) });
    m_cacheHitText.Text(winrt::hstring{ FormatPercent(cacheRatio * 100.0) });
    m_outputTokensText.Text(winrt::hstring{ FormatInteger(outputTokens) });
    SetProgress(m_cacheProgressFill, m_cacheProgressRest, cacheRatio);

    m_dayTokensText.Text(winrt::hstring{
        FormatInteger(std::max<int64_t>(data.day.counts.DisplayTotal(), 0)) });
    m_dayMessagesText.Text(winrt::hstring{ FormatInteger(std::max<int64_t>(data.day.messages, 0)) });
    m_dayToolCallsText.Text(winrt::hstring{ FormatInteger(std::max<int64_t>(data.day.toolCalls, 0)) });
    m_activeDaysText.Text(winrt::hstring{ FormatInteger(std::max<int64_t>(data.total.activeDays, 0)) });

    auto const empty = totalTokens == 0 && data.daily.empty() && data.recent.empty();
    m_overviewEmptyState.Visibility(empty ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_overviewMetricsPanel.Visibility(empty ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    if (!data.error.empty())
    {
        m_emptyOverviewTitle.Text(L"暂时无法读取使用记录");
        m_emptyOverviewDetail.Text(winrt::hstring{ data.error });
        SetStatus(L"采集失败", data.error, false);
    }
    else if (data.collecting)
    {
        m_emptyOverviewTitle.Text(L"正在扫描 Codex 记录");
        m_emptyOverviewDetail.Text(L"首批记录将在采集完成后自动出现。");
        SetStatus(L"正在采集 Codex", FormatAge(data.lastSync), false);
    }
    else if (data.lastSync > 0)
    {
        m_emptyOverviewTitle.Text(L"还没有使用记录");
        m_emptyOverviewDetail.Text(L"当前数据源为空；继续使用 Codex 后会自动刷新。");
        SetStatus(L"数据已同步", FormatAge(data.lastSync), true);
    }
    else
    {
        m_emptyOverviewTitle.Text(L"还没有使用记录");
        m_emptyOverviewDetail.Text(L"打开 Codex 后，首批记录会自动出现在这里。");
        SetStatus(L"等待首次同步", L"尚未发现 Codex 记录", false);
    }

    if (data.codexLimit)
    {
        auto const& limit = *data.codexLimit;
        auto const usePrimary = limit.primaryUsedPercent >= 0.0;
        auto const used = usePrimary ? limit.primaryUsedPercent : limit.secondaryUsedPercent;
        if (used >= 0.0)
        {
            auto const left = 100.0 - std::clamp(used, 0.0, 100.0);
            auto const resetsAt = usePrimary ? limit.primaryResetsAt : limit.secondaryResetsAt;
            auto name = limit.limitName.empty() ? std::wstring{ L"Codex" } : limit.limitName;
            name += usePrimary ? L" · primary" : L" · secondary";
            m_codexLimitName.Text(winrt::hstring{ name });
            m_codexLimitValue.Text(winrt::hstring{ FormatPercent(left) + L" left" });
            m_codexLimitReset.Text(winrt::hstring{ FormatReset(resetsAt) });
            SetProgress(m_codexProgressFill, m_codexProgressRest, left / 100.0);
        }
        else
        {
            m_codexLimitName.Text(L"Codex 未提供 used_percent");
            m_codexLimitValue.Text(L"—");
            m_codexLimitReset.Text(L"等待下一次限额快照");
            SetProgress(m_codexProgressFill, m_codexProgressRest, 0.0);
        }
    }
    else
    {
        m_codexLimitName.Text(L"等待 Codex 限额快照");
        m_codexLimitValue.Text(L"—");
        m_codexLimitReset.Text(L"采集到 used_percent 后显示");
        SetProgress(m_codexProgressFill, m_codexProgressRest, 0.0);
    }

    auto const sessionCount = std::min<size_t>(data.recent.size(), m_sessionRows.size());
    for (size_t index = 0; index < m_sessionRows.size(); ++index)
    {
        auto const visible = index < sessionCount;
        m_sessionRows[index].Visibility(visible ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (!visible)
        {
            continue;
        }

        auto const& session = data.recent[index];
        auto title = session.title;
        if (title.empty())
        {
            title = !session.project.empty() ? session.project : session.model;
        }
        if (title.empty())
        {
            title = L"未命名会话";
        }
        auto detail = session.model.empty() ? std::wstring{ L"Codex" } : session.model;
        detail += L" · " + FormatInteger(session.messages) + L" messages";
        if (session.updatedAt > 0)
        {
            detail += L" · " + FormatAge(session.updatedAt);
        }

        m_sessionTitles[index].Text(winrt::hstring{ title });
        m_sessionDetails[index].Text(winrt::hstring{ detail });
        m_sessionValues[index].Text(winrt::hstring{ FormatCompact(session.counts.DisplayTotal()) });
    }
    m_recentEmptyState.Visibility(sessionCount == 0 ? mux::Visibility::Visible : mux::Visibility::Collapsed);

    if (data.total.estimatedTokens > 0)
    {
        m_chatGptOverviewValue.Text(winrt::hstring{
            L"≈ " + FormatCompact(data.total.estimatedTokens) + L" token" });
        m_chatGptOverviewDetail.Text(winrt::hstring{
            FormatInteger(data.total.estimatedSessions) + L" 个会话 · 官方导出可见文本估算" });
    }
    else
    {
        m_chatGptOverviewValue.Text(L"等待官方导出导入");
        m_chatGptOverviewDetail.Text(L"实时 token 不可用");
    }
    UpdateDailyVisuals(data.daily);
}

void DashboardView::UpdateDailyVisuals(std::vector<DailyUsage> const& daily)
{
    std::map<std::wstring, int64_t> totalsByDay;
    for (auto const& row : daily)
    {
        totalsByDay[row.day] += std::max<int64_t>(row.counts.DisplayTotal(), 0);
    }

    std::vector<int64_t> values;
    values.reserve(totalsByDay.size());
    int64_t peak{};
    for (auto const& [day, total] : totalsByDay)
    {
        (void)day;
        values.push_back(total);
        peak = std::max(peak, total);
    }

    auto const barCount = m_dailyBars.size();
    auto const visibleBars = std::min(values.size(), barCount);
    for (size_t index = 0; index < barCount; ++index)
    {
        auto const leadingEmpty = barCount - visibleBars;
        if (index < leadingEmpty || peak <= 0)
        {
            m_dailyBars[index].Height(12);
            m_dailyBars[index].Opacity(0.18);
            m_dailyBars[index].Background(Brush(Color(55, 52, 53)));
            continue;
        }

        auto const valueIndex = values.size() - visibleBars + index - leadingEmpty;
        auto const ratio = static_cast<double>(values[valueIndex]) / static_cast<double>(peak);
        m_dailyBars[index].Height(16.0 + 88.0 * ratio);
        m_dailyBars[index].Opacity(0.42 + 0.48 * ratio);
        m_dailyBars[index].Background(Brush(
            valueIndex + 1 == values.size() ? Color(255, 253, 142) : Color(98, 223, 125)));
    }

    auto const cellCount = m_heatmapCells.size();
    auto const visibleCells = std::min(values.size(), cellCount);
    for (size_t index = 0; index < cellCount; ++index)
    {
        auto const leadingEmpty = cellCount - visibleCells;
        if (index < leadingEmpty || peak <= 0)
        {
            m_heatmapCells[index].Background(Brush(Color(55, 52, 53)));
            m_heatmapCells[index].Opacity(0.5);
            continue;
        }

        auto const valueIndex = values.size() - visibleCells + index - leadingEmpty;
        auto const ratio = static_cast<double>(values[valueIndex]) / static_cast<double>(peak);
        m_heatmapCells[index].Background(Brush(Color(255, 253, 142)));
        m_heatmapCells[index].Opacity(0.28 + 0.72 * ratio);
    }

    if (totalsByDay.empty())
    {
        m_heatmapCaption.Text(L"尚无活动数据");
    }
    else
    {
        auto caption = totalsByDay.begin()->first + L" — " + totalsByDay.rbegin()->first;
        caption += L" · " + FormatInteger(static_cast<int64_t>(totalsByDay.size())) + L" 日有记录";
        m_heatmapCaption.Text(winrt::hstring{ caption });
    }
}

void DashboardView::SetDetailsCallbacks(DetailsCallbacks callbacks)
{
    m_detailsCallbacks = std::move(callbacks);
}

void DashboardView::UpdateDetails(DetailsViewData const& data)
{
    m_detailsDimension = data.dimension;
    UpdateDetailsDimensionButtons();
    m_breakdownList.Children().Clear();

    auto appendState = [](controls::StackPanel const& panel, std::wstring_view title, std::wstring_view detail)
    {
        controls::StackPanel copy;
        copy.Spacing(3);
        copy.Children().Append(Text(title, 11.5, Color(247, 247, 245), 600));
        auto detailText = Text(detail, 9.5, Color(143, 139, 140));
        detailText.TextWrapping(mux::TextWrapping::Wrap);
        copy.Children().Append(detailText);
        panel.Children().Append(SoftPanel(copy));
    };

    if (!data.error.empty())
    {
        appendState(m_breakdownList, L"明细加载失败", data.error);
    }
    else if (data.loading)
    {
        appendState(m_breakdownList, L"正在加载明细", L"读取完成后会自动刷新当前维度。");
    }
    else if (data.rows.empty())
    {
        appendState(m_breakdownList, L"当前维度暂无数据", L"采集到使用记录后会显示在这里。");
    }
    else
    {
        int64_t peak{};
        for (auto const& row : data.rows)
        {
            peak = std::max(peak, row.counts.DisplayTotal());
        }

        auto const visibleRows = std::min<size_t>(data.rows.size(), 3);
        for (size_t index = 0; index < visibleRows; ++index)
        {
            auto const& row = data.rows[index];
            auto const selected = row.key == data.selectedKey;
            auto const input = std::max<int64_t>(row.counts.input, 0);
            auto const cached = std::clamp<int64_t>(row.counts.cachedInput, 0, input);
            auto const hitRate = input > 0
                ? static_cast<double>(cached) * 100.0 / static_cast<double>(input)
                : 0.0;

            controls::StackPanel rowContent;
            rowContent.Spacing(6);
            controls::Grid top;
            AddColumn(top, Star());
            AddColumn(top, mux::GridLengthHelper::Auto());
            top.Children().Append(Text(row.key.empty() ? L"未命名" : row.key, 11.5, Color(247, 247, 245), 600));
            auto total = Text(FormatCompact(row.counts.DisplayTotal()), 11.5, Color(247, 247, 245), 600, true);
            controls::Grid::SetColumn(total, 1);
            top.Children().Append(total);
            rowContent.Children().Append(top);

            auto summary = FormatPercent(hitRate) + L" hit · "
                + FormatCompact(row.counts.output) + L" output";
            rowContent.Children().Append(Text(summary, 9.5, Color(143, 139, 140)));
            auto const ratio = peak > 0
                ? static_cast<double>(row.counts.DisplayTotal()) / static_cast<double>(peak)
                : 0.0;
            rowContent.Children().Append(Progress(ratio, selected ? Color(240, 63, 22) : Color(98, 223, 125)));

            controls::Button button;
            button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            button.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            button.Padding({ 12, 9, 12, 9 });
            button.Background(Brush(selected ? Color(55, 52, 53) : Color(29, 27, 28)));
            button.BorderBrush(Brush(selected ? Color(240, 63, 22) : Color(255, 255, 255, 12)));
            button.BorderThickness({ 1 });
            button.CornerRadius(Radius(13));
            button.Content(rowContent);
            automation::AutomationProperties::SetName(button, winrt::hstring{ row.key });
            auto key = row.key;
            button.Click([this, key = std::move(key)](auto const&, auto const&)
            {
                if (m_detailsCallbacks.onBreakdownSelected)
                {
                    m_detailsCallbacks.onBreakdownSelected(key);
                }
            });
            m_breakdownList.Children().Append(button);
        }

        if (data.rows.size() > visibleRows)
        {
            auto caption = L"显示前 " + std::to_wstring(visibleRows)
                + L" 项 · 共 " + std::to_wstring(data.rows.size()) + L" 项";
            m_breakdownList.Children().Append(Text(caption, 9.5, Color(143, 139, 140)));
        }
    }

    auto const selectedRow = std::find_if(
        data.rows.begin(),
        data.rows.end(),
        [&data](BreakdownRow const& row) { return !data.selectedKey.empty() && row.key == data.selectedKey; });
    auto const hasSelection = selectedRow != data.rows.end();
    m_detailsSelectionState.Visibility(hasSelection ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    m_detailsMetricsPanel.Visibility(hasSelection ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    if (hasSelection)
    {
        auto const input = std::max<int64_t>(selectedRow->counts.input, 0);
        auto const cached = std::clamp<int64_t>(selectedRow->counts.cachedInput, 0, input);
        auto const miss = std::max<int64_t>(selectedRow->counts.UncachedInput(), 0);
        auto const output = std::max<int64_t>(selectedRow->counts.output, 0);
        auto const ratio = input > 0
            ? static_cast<double>(cached) / static_cast<double>(input)
            : 0.0;
        m_detailsSelectedTitle.Text(winrt::hstring{ selectedRow->key });
        m_detailsInputText.Text(winrt::hstring{ FormatInteger(input) });
        m_detailsCacheHitTokensText.Text(winrt::hstring{ FormatInteger(cached) });
        m_detailsCacheMissTokensText.Text(winrt::hstring{ FormatInteger(miss) });
        m_detailsOutputText.Text(winrt::hstring{ FormatInteger(output) });
        m_detailsHitRateText.Text(winrt::hstring{ FormatPercent(ratio * 100.0) });
        SetProgress(m_detailsCacheProgressFill, m_detailsCacheProgressRest, ratio);
    }
    else
    {
        SetProgress(m_detailsCacheProgressFill, m_detailsCacheProgressRest, 0.0);
    }

    m_detailsSessionsPanel.Children().Clear();
    if (!data.error.empty())
    {
        appendState(m_detailsSessionsPanel, L"会话不可用", data.error);
    }
    else
    {
        m_detailsSessionsPanel.Children().Append(Text(L"最近会话", 10.5, Color(143, 139, 140), 600));
        auto const visibleSessions = std::min<size_t>(data.recentSessions.size(), 3);
        for (size_t index = 0; index < visibleSessions; ++index)
        {
            auto const& session = data.recentSessions[index];
            auto title = session.title;
            if (title.empty())
            {
                title = !session.project.empty() ? session.project : session.model;
            }
            if (title.empty())
            {
                title = L"未命名会话";
            }

            controls::Grid content;
            AddColumn(content, Star());
            AddColumn(content, mux::GridLengthHelper::Auto());
            controls::StackPanel copy;
            copy.Spacing(2);
            copy.Children().Append(Text(title, 10.5, Color(247, 247, 245), 600));
            auto detail = session.model.empty() ? std::wstring{ L"Codex" } : session.model;
            detail += L" · " + FormatInteger(session.messages) + L" msgs";
            copy.Children().Append(Text(detail, 9, Color(143, 139, 140)));
            content.Children().Append(copy);
            auto value = Text(FormatCompact(session.counts.DisplayTotal()), 10.5, Color(247, 247, 245), 600, true);
            value.VerticalAlignment(mux::VerticalAlignment::Center);
            controls::Grid::SetColumn(value, 1);
            content.Children().Append(value);

            auto const selected = session.id == data.selectedSessionId;
            controls::Button button;
            button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            button.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            button.Padding({ 10, 7, 10, 7 });
            button.Background(Brush(selected ? Color(55, 52, 53) : Color(29, 27, 28)));
            button.BorderBrush(Brush(selected ? Color(240, 63, 22) : Color(255, 255, 255, 12)));
            button.BorderThickness({ 1 });
            button.CornerRadius(Radius(12));
            button.Content(content);
            automation::AutomationProperties::SetName(button, winrt::hstring{ title });
            auto sessionId = session.id;
            button.Click([this, sessionId = std::move(sessionId)](auto const&, auto const&)
            {
                if (m_detailsCallbacks.onSessionSelected)
                {
                    m_detailsCallbacks.onSessionSelected(sessionId);
                }
            });
            m_detailsSessionsPanel.Children().Append(button);
        }

        if (visibleSessions == 0)
        {
            appendState(m_detailsSessionsPanel, L"暂无会话", L"采集完成后最多显示最近 3 条。");
        }

        if (!data.selectedSessionId.empty())
        {
            m_detailsSessionsPanel.Children().Append(Text(L"提示拆分", 10.5, Color(143, 139, 140), 600));
            if (data.selectedTurns.empty())
            {
                appendState(m_detailsSessionsPanel, L"暂无提示详情", L"该会话尚未生成可展示的提示记录。");
            }
            for (auto const& turn : data.selectedTurns)
            {
                controls::Grid content;
                AddColumn(content, Star());
                AddColumn(content, mux::GridLengthHelper::Auto());
                controls::StackPanel copy;
                copy.Spacing(2);
                auto title = L"Prompt " + std::to_wstring(turn.promptIndex + 1);
                if (!turn.model.empty())
                {
                    title += L" · " + turn.model;
                }
                copy.Children().Append(Text(title, 10.5, Color(247, 247, 245), 600));
                copy.Children().Append(Text(
                    turn.tools.empty() ? L"未使用工具" : turn.tools,
                    9,
                    Color(143, 139, 140)));
                content.Children().Append(copy);
                auto value = Text(FormatCompact(turn.counts.DisplayTotal()), 10.5, Color(255, 253, 142), 600, true);
                value.VerticalAlignment(mux::VerticalAlignment::Center);
                controls::Grid::SetColumn(value, 1);
                content.Children().Append(value);
                m_detailsSessionsPanel.Children().Append(SoftPanel(content));
            }

            if (!data.toolCalls.empty())
            {
                m_detailsSessionsPanel.Children().Append(Text(L"工具调用", 10.5, Color(143, 139, 140), 600));
            }
            for (auto const& tool : data.toolCalls)
            {
                controls::Grid content;
                AddColumn(content, Star());
                AddColumn(content, mux::GridLengthHelper::Auto());
                controls::StackPanel copy;
                copy.Spacing(2);
                copy.Children().Append(Text(
                    tool.name.empty() ? L"未命名工具" : tool.name,
                    10.5,
                    Color(247, 247, 245),
                    600));
                copy.Children().Append(Text(
                    tool.summary.empty() ? L"点击读取输入 / 输出" : tool.summary,
                    9,
                    Color(143, 139, 140)));
                content.Children().Append(copy);
                auto arrow = Text(L">", 12, Color(255, 253, 142), 600, true);
                arrow.VerticalAlignment(mux::VerticalAlignment::Center);
                controls::Grid::SetColumn(arrow, 1);
                content.Children().Append(arrow);

                auto const selected = tool.locator == data.selectedToolCallLocator;
                controls::Button button;
                button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                button.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
                button.Padding({ 10, 7, 10, 7 });
                button.Background(Brush(selected ? Color(55, 52, 53) : Color(29, 27, 28)));
                button.BorderBrush(Brush(selected ? Color(255, 253, 142) : Color(255, 255, 255, 12)));
                button.BorderThickness({ 1 });
                button.CornerRadius(Radius(12));
                button.Content(content);
                automation::AutomationProperties::SetName(button, winrt::hstring{ tool.name });
                auto locator = tool.locator;
                button.Click([this, locator = std::move(locator)](auto const&, auto const&)
                {
                    if (m_detailsCallbacks.onToolCallRequested)
                    {
                        m_detailsCallbacks.onToolCallRequested(locator);
                    }
                });
                m_detailsSessionsPanel.Children().Append(button);
            }

            if (!data.selectedToolCallLocator.empty())
            {
                controls::StackPanel detail;
                detail.Spacing(6);
                detail.Children().Append(Text(L"工具输入 / 输出", 10.5, Color(255, 253, 142), 600));
                detail.Children().Append(Text(
                    L"从本地转录按需读取 · 不写入数据库",
                    9,
                    Color(143, 139, 140),
                    500));
                auto body = Text(
                    data.selectedToolDetails.empty() ? L"正在按需读取工具详情…" : data.selectedToolDetails,
                    10,
                    Color(247, 247, 245));
                body.TextWrapping(mux::TextWrapping::Wrap);
                detail.Children().Append(body);
                m_detailsSessionsPanel.Children().Append(SoftPanel(detail));
            }
        }
    }

    double expandedHeight = 548.0;
    if (!data.selectedSessionId.empty())
    {
        expandedHeight += 54.0;
        expandedHeight += static_cast<double>(data.selectedTurns.size()) * 58.0;
        expandedHeight += static_cast<double>(data.toolCalls.size()) * 52.0;
        if (!data.selectedToolCallLocator.empty())
        {
            auto const approximateLines = (data.selectedToolDetails.size() + 45) / 46;
            expandedHeight += 82.0 + static_cast<double>(approximateLines) * 17.0;
        }
    }
    m_detailsPage.Height(expandedHeight);
}

void DashboardView::UpdateDetailsDimensionButtons()
{
    auto const selectedIndex = static_cast<size_t>(m_detailsDimension);
    for (size_t index = 0; index < m_detailsDimensionButtons.size(); ++index)
    {
        auto const selected = index == selectedIndex;
        auto const& button = m_detailsDimensionButtons[index];
        button.Background(Brush(selected ? Color(55, 52, 53) : Color(0, 0, 0, 0)));
        button.Foreground(Brush(selected ? Color(247, 247, 245) : Color(143, 139, 140)));
        button.BorderBrush(Brush(selected ? Color(98, 223, 125) : Color(255, 255, 255, 12)));
    }
}

void DashboardView::SetTrendCallbacks(TrendCallbacks callbacks)
{
    m_trendCallbacks = std::move(callbacks);
}

void DashboardView::UpdateTrends(TrendViewData const& data)
{
    m_trendGroup = data.group;
    m_trendChart = data.chart;
    m_trendRange = data.range;
    UpdateTrendButtons();

    m_currentStreakText.Text(winrt::hstring{ FormatInteger(std::max(data.currentStreak, 0)) });
    m_longestStreakText.Text(winrt::hstring{ FormatInteger(std::max(data.longestStreak, 0)) });
    m_trendChartHost.Children().Clear();

    auto showChartState = [this](std::wstring_view title, std::wstring_view detail)
    {
        controls::StackPanel copy;
        copy.Spacing(3);
        copy.HorizontalAlignment(mux::HorizontalAlignment::Center);
        copy.VerticalAlignment(mux::VerticalAlignment::Center);
        copy.Children().Append(Text(title, 12, Color(247, 247, 245), 600));
        copy.Children().Append(Text(detail, 9.5, Color(143, 139, 140)));
        auto panel = SoftPanel(copy);
        panel.HorizontalAlignment(mux::HorizontalAlignment::Center);
        panel.VerticalAlignment(mux::VerticalAlignment::Center);
        m_trendChartHost.Children().Append(panel);
    };

    auto const groupLabel = data.group == TrendGroup::Tool ? L"按工具" : L"按模型";
    if (!data.error.empty())
    {
        m_trendChartCaption.Text(L"趋势加载失败");
        showChartState(L"无法读取趋势", data.error);
    }
    else if (data.loading)
    {
        m_trendChartCaption.Text(L"正在汇总趋势");
        showChartState(L"正在加载", L"预聚合完成后会自动刷新图形。");
    }
    else if (data.chart == TrendChart::Bars)
    {
        auto const seriesCount = std::min<size_t>(data.series.size(), 5);
        std::map<std::wstring, std::vector<int64_t>> buckets;
        for (size_t seriesIndex = 0; seriesIndex < seriesCount; ++seriesIndex)
        {
            for (auto const& point : data.series[seriesIndex].points)
            {
                auto& values = buckets[point.day];
                values.resize(seriesCount);
                values[seriesIndex] += std::max<int64_t>(point.value, 0);
            }
        }

        int64_t peak{};
        for (auto const& [day, values] : buckets)
        {
            (void)day;
            int64_t total{};
            for (auto const value : values)
            {
                total += value;
            }
            peak = std::max(peak, total);
        }

        if (buckets.empty() || peak <= 0)
        {
            m_trendChartCaption.Text(winrt::hstring{
                std::wstring{ RangeLabel(data.range) } + L" · " + groupLabel });
            showChartState(L"暂无趋势数据", L"当前范围内没有可绘制的每日汇总。");
        }
        else
        {
            controls::Canvas canvas;
            canvas.Width(960);
            canvas.Height(166);
            constexpr double plotLeft = 20.0;
            constexpr double plotWidth = 920.0;
            constexpr double plotBottom = 136.0;
            constexpr double plotHeight = 120.0;

            controls::Border baseline;
            baseline.Width(plotWidth);
            baseline.Height(1);
            baseline.Background(Brush(Color(255, 255, 255, 22)));
            controls::Canvas::SetLeft(baseline, plotLeft);
            controls::Canvas::SetTop(baseline, plotBottom);
            canvas.Children().Append(baseline);

            auto const step = plotWidth / static_cast<double>(buckets.size());
            auto const barWidth = std::max(1.0, step * 0.72);
            size_t dayIndex{};
            for (auto const& [day, values] : buckets)
            {
                auto bottom = plotBottom;
                for (size_t seriesIndex = 0; seriesIndex < values.size(); ++seriesIndex)
                {
                    auto const height = plotHeight * static_cast<double>(values[seriesIndex])
                        / static_cast<double>(peak);
                    if (height <= 0.0)
                    {
                        continue;
                    }
                    controls::Border segment;
                    segment.Width(barWidth);
                    segment.Height(std::max(height, 1.0));
                    segment.Background(Brush(SeriesColor(seriesIndex)));
                    segment.Opacity(0.82);
                    segment.CornerRadius(Radius(std::min(3.0, barWidth * 0.35)));
                    bottom -= height;
                    controls::Canvas::SetLeft(segment, plotLeft + step * dayIndex + (step - barWidth) * 0.5);
                    controls::Canvas::SetTop(segment, bottom);
                    canvas.Children().Append(segment);
                }
                ++dayIndex;
            }

            auto firstDay = Text(buckets.begin()->first, 8.5, Color(143, 139, 140), 500, true);
            controls::Canvas::SetLeft(firstDay, plotLeft);
            controls::Canvas::SetTop(firstDay, 146);
            canvas.Children().Append(firstDay);
            auto lastDay = Text(buckets.rbegin()->first, 8.5, Color(143, 139, 140), 500, true);
            controls::Canvas::SetLeft(lastDay, 850);
            controls::Canvas::SetTop(lastDay, 146);
            canvas.Children().Append(lastDay);

            controls::Viewbox viewbox;
            viewbox.Stretch(media::Stretch::Fill);
            viewbox.Child(canvas);
            m_trendChartHost.Children().Append(viewbox);

            auto caption = std::wstring{ RangeLabel(data.range) } + L" · " + groupLabel;
            caption += L" · " + FormatInteger(static_cast<int64_t>(buckets.size())) + L" daily points";
            if (data.series.size() > seriesCount)
            {
                caption += L" · 显示前 5 系列";
            }
            m_trendChartCaption.Text(winrt::hstring{ caption });
        }
    }
    else
    {
        auto candleSeries = data.candleSeries;
        if (candleSeries.empty() && !data.series.empty())
        {
            candleSeries = data.series.front().key;
        }
        if (candleSeries.empty())
        {
            candleSeries = L"系列未标明";
        }

        if (data.candles.empty())
        {
            m_trendChartCaption.Text(winrt::hstring{
                std::wstring{ RangeLabel(data.range) } + L" · K-line · " + candleSeries });
            showChartState(L"暂无 K-line 数据", L"当前范围内没有 OHLC 与日总 token 数据。");
        }
        else
        {
            int64_t chartHigh = data.candles.front().high;
            int64_t chartLow = data.candles.front().low;
            int64_t peakVolume{};
            for (auto const& candle : data.candles)
            {
                chartHigh = std::max(chartHigh, std::max({ candle.high, candle.open, candle.close }));
                chartLow = std::min(chartLow, std::min({ candle.low, candle.open, candle.close }));
                peakVolume = std::max(peakVolume, std::max<int64_t>(candle.volume, 0));
            }
            if (chartHigh <= chartLow)
            {
                chartHigh = chartLow + 1;
            }

            controls::Canvas canvas;
            canvas.Width(960);
            canvas.Height(166);
            constexpr double plotLeft = 20.0;
            constexpr double plotWidth = 920.0;
            constexpr double priceTop = 6.0;
            constexpr double priceHeight = 104.0;
            constexpr double volumeTop = 116.0;
            constexpr double volumeHeight = 22.0;
            auto const step = plotWidth / static_cast<double>(data.candles.size());
            auto const bodyWidth = std::clamp(step * 0.55, 1.0, 10.0);
            auto const priceRange = static_cast<double>(chartHigh) - static_cast<double>(chartLow);
            auto yFor = [&](int64_t value)
            {
                return priceTop + (static_cast<double>(chartHigh) - static_cast<double>(value))
                    / priceRange * priceHeight;
            };

            for (size_t index = 0; index < data.candles.size(); ++index)
            {
                auto const& candle = data.candles[index];
                auto const high = std::max({ candle.high, candle.open, candle.close });
                auto const low = std::min({ candle.low, candle.open, candle.close });
                auto const highY = yFor(high);
                auto const lowY = yFor(low);
                auto const openY = yFor(candle.open);
                auto const closeY = yFor(candle.close);
                auto const x = plotLeft + step * index + step * 0.5;
                auto const rising = candle.close >= candle.open;
                auto const color = rising ? Color(98, 223, 125) : Color(240, 63, 22);

                controls::Border wick;
                wick.Width(1);
                wick.Height(std::max(lowY - highY, 1.0));
                wick.Background(Brush(color));
                controls::Canvas::SetLeft(wick, x);
                controls::Canvas::SetTop(wick, highY);
                canvas.Children().Append(wick);

                controls::Border body;
                body.Width(bodyWidth);
                body.Height(std::max(std::abs(closeY - openY), 2.0));
                body.Background(Brush(color));
                body.CornerRadius(Radius(std::min(2.0, bodyWidth * 0.25)));
                controls::Canvas::SetLeft(body, x - bodyWidth * 0.5);
                controls::Canvas::SetTop(body, std::min(openY, closeY));
                canvas.Children().Append(body);

                if (peakVolume > 0 && candle.volume > 0)
                {
                    controls::Border volume;
                    volume.Width(bodyWidth);
                    volume.Height(volumeHeight * static_cast<double>(candle.volume)
                        / static_cast<double>(peakVolume));
                    volume.Background(Brush(color));
                    volume.Opacity(0.36);
                    controls::Canvas::SetLeft(volume, x - bodyWidth * 0.5);
                    controls::Canvas::SetTop(volume, volumeTop + volumeHeight - volume.Height());
                    canvas.Children().Append(volume);
                }
            }

            auto firstDay = Text(data.candles.front().day, 8.5, Color(143, 139, 140), 500, true);
            controls::Canvas::SetLeft(firstDay, plotLeft);
            controls::Canvas::SetTop(firstDay, 146);
            canvas.Children().Append(firstDay);
            auto lastDay = Text(data.candles.back().day, 8.5, Color(143, 139, 140), 500, true);
            controls::Canvas::SetLeft(lastDay, 850);
            controls::Canvas::SetTop(lastDay, 146);
            canvas.Children().Append(lastDay);

            controls::Viewbox viewbox;
            viewbox.Stretch(media::Stretch::Fill);
            viewbox.Child(canvas);
            m_trendChartHost.Children().Append(viewbox);

            auto caption = std::wstring{ RangeLabel(data.range) } + L" · K-line · " + candleSeries + L" · OHLC";
            caption += L" · Peak volume " + FormatCompact(peakVolume) + L" tokens";
            m_trendChartCaption.Text(winrt::hstring{ caption });
        }
    }

    auto const heatCount = std::min({ data.heatCells.size(), m_trendHeatCells.size(), size_t{ 365 } });
    int64_t heatPeak{};
    for (size_t index = data.heatCells.size() - heatCount; index < data.heatCells.size(); ++index)
    {
        heatPeak = std::max(heatPeak, std::max<int64_t>(data.heatCells[index].value, 0));
    }
    auto const leadingCells = m_trendHeatCells.size() - heatCount;
    for (size_t index = 0; index < m_trendHeatCells.size(); ++index)
    {
        if (index < leadingCells || heatPeak <= 0)
        {
            m_trendHeatCells[index].Background(Brush(Color(55, 52, 53)));
            m_trendHeatCells[index].Opacity(0.5);
            continue;
        }
        auto const dataIndex = data.heatCells.size() - heatCount + index - leadingCells;
        auto const value = std::max<int64_t>(data.heatCells[dataIndex].value, 0);
        auto const ratio = static_cast<double>(value) / static_cast<double>(heatPeak);
        m_trendHeatCells[index].Background(Brush(value > 0 ? Color(98, 223, 125) : Color(55, 52, 53)));
        m_trendHeatCells[index].Opacity(value > 0 ? 0.28 + 0.72 * ratio : 0.5);
    }
    if (heatCount == 0)
    {
        m_trendHeatCaption.Text(L"尚无年度活动数据 · 小时历史仅保留 400 日");
    }
    else
    {
        auto const first = data.heatCells[data.heatCells.size() - heatCount].day;
        auto const last = data.heatCells.back().day;
        auto caption = first + L" — " + last;
        caption += L" · 最多 365 日 · 小时历史仅保留 400 日";
        m_trendHeatCaption.Text(winrt::hstring{ caption });
    }

    m_trendLegend.Children().Clear();
    auto const legendCount = std::min<size_t>(data.series.size(), 5);
    if (legendCount == 0)
    {
        controls::StackPanel copy;
        copy.Spacing(2);
        copy.Children().Append(Text(L"暂无系列", 10.5, Color(247, 247, 245), 600));
        copy.Children().Append(Text(L"等待预聚合数据。", 9, Color(143, 139, 140)));
        m_trendLegend.Children().Append(SoftPanel(copy));
    }
    else
    {
        for (size_t index = 0; index < legendCount; ++index)
        {
            controls::Grid row;
            AddColumn(row, Pixels(12));
            AddColumn(row, Star());
            AddColumn(row, mux::GridLengthHelper::Auto());
            shapes::Ellipse dot;
            dot.Width(7);
            dot.Height(7);
            dot.Fill(Brush(SeriesColor(index)));
            dot.VerticalAlignment(mux::VerticalAlignment::Center);
            row.Children().Append(dot);
            auto name = Text(
                data.series[index].key.empty() ? L"未命名" : data.series[index].key,
                9.5,
                Color(247, 247, 245),
                600);
            controls::Grid::SetColumn(name, 1);
            row.Children().Append(name);
            auto value = Text(
                FormatCompact(data.series[index].total) + L" · " + FormatPercent(data.series[index].percent),
                9.5,
                Color(143, 139, 140),
                600,
                true);
            controls::Grid::SetColumn(value, 2);
            row.Children().Append(value);
            m_trendLegend.Children().Append(row);
        }
    }
}

void DashboardView::UpdateTrendButtons()
{
    auto update = [](std::vector<controls::Button> const& buttons, size_t selected, winrt::Windows::UI::Color accent)
    {
        for (size_t index = 0; index < buttons.size(); ++index)
        {
            auto const active = index == selected;
            buttons[index].Background(Brush(active ? Color(55, 52, 53) : Color(0, 0, 0, 0)));
            buttons[index].Foreground(Brush(active ? Color(247, 247, 245) : Color(143, 139, 140)));
            buttons[index].BorderBrush(Brush(active ? accent : Color(255, 255, 255, 12)));
        }
    };
    update(m_trendGroupButtons, static_cast<size_t>(m_trendGroup), Color(98, 223, 125));
    update(m_trendChartButtons, static_cast<size_t>(m_trendChart), Color(255, 253, 142));
    update(m_trendRangeButtons, static_cast<size_t>(m_trendRange), Color(240, 63, 22));
}

void DashboardView::SetChatGptImportCallbacks(ChatGptImportCallbacks callbacks)
{
    m_chatGptImportCallbacks = std::move(callbacks);
}

void DashboardView::UpdateChatGptImport(ChatGptImportViewData const& data)
{
    m_chatGptAccountLabel.Text(winrt::hstring{ data.accountLabel });

    std::wstring filesText;
    if (data.selectedFiles.empty())
    {
        filesText = L"尚未选择 JSON 文件";
    }
    else
    {
        filesText = FormatInteger(static_cast<int64_t>(data.selectedFiles.size())) + L" 个 JSON · ";
        filesText += FileNamePart(data.selectedFiles.front());
        if (data.selectedFiles.size() > 1)
        {
            filesText += L" 等";
        }
    }
    m_chatGptFilesText.Text(winrt::hstring{ filesText });

    std::wstring_view title;
    std::wstring_view fallbackMessage;
    auto stateColor = Color(143, 139, 140);
    switch (data.state)
    {
    case ChatGptImportState::Idle:
        title = L"等待导入";
        fallbackMessage = L"选择 ChatGPT 官方导出的一个或多个 JSON 文件。";
        break;
    case ChatGptImportState::SelectingFiles:
        title = L"正在选择文件";
        fallbackMessage = L"可多选 conversations.json 与编号分片。";
        stateColor = Color(255, 253, 142);
        break;
    case ChatGptImportState::Importing:
        title = L"正在导入";
        fallbackMessage = L"正在解析会话并估算 token，请保持应用运行。";
        stateColor = Color(255, 253, 142);
        break;
    case ChatGptImportState::Succeeded:
        title = L"导入完成";
        fallbackMessage = L"所选记录已完成处理，重复记录已跳过。";
        stateColor = Color(98, 223, 125);
        break;
    case ChatGptImportState::Failed:
        title = L"导入未完成";
        fallbackMessage = L"请查看错误信息后重新选择文件。";
        stateColor = Color(240, 63, 22);
        break;
    }

    auto const message = data.message.empty() ? std::wstring{ fallbackMessage } : data.message;
    m_chatGptImportTitle.Text(winrt::hstring{ title });
    m_chatGptImportMessage.Text(winrt::hstring{ message });
    m_chatGptImportDot.Fill(Brush(stateColor));

    auto const busy = data.state == ChatGptImportState::SelectingFiles
        || data.state == ChatGptImportState::Importing;
    m_chatGptChooseFilesButton.IsEnabled(!busy);
    m_chatGptChooseFilesButton.Content(winrt::box_value(winrt::hstring{
        data.state == ChatGptImportState::SelectingFiles
            ? L"正在选择…"
            : data.state == ChatGptImportState::Importing
                ? L"正在导入…"
                : L"选择一个或多个 JSON" }));

    auto const hasResults = data.state == ChatGptImportState::Succeeded
        || data.state == ChatGptImportState::Failed
        || data.conversations != 0
        || data.estimatedTokens != 0
        || data.skipped != 0
        || data.unchangedFiles != 0
        || data.errors != 0;
    auto const resultText = [hasResults](int64_t value)
    {
        return hasResults ? FormatInteger(std::max<int64_t>(value, 0)) : std::wstring{ L"—" };
    };
    m_chatGptConversationCount.Text(winrt::hstring{ resultText(data.conversations) });
    m_chatGptEstimatedTokens.Text(winrt::hstring{ resultText(data.estimatedTokens) });
    m_chatGptSkippedCount.Text(winrt::hstring{ resultText(data.skipped) });
    m_chatGptUnchangedCount.Text(winrt::hstring{ resultText(data.unchangedFiles) });
    m_chatGptErrorCount.Text(winrt::hstring{ resultText(data.errors) });

    m_chatGptDetails = data.details;
    m_chatGptDetailsExpanded = data.detailsExpanded;
    m_chatGptDetailsText.Text(winrt::hstring{ m_chatGptDetails });
    m_chatGptDetailsButton.Visibility(
        m_chatGptDetails.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);

    UpdateChatGptImportLayout();
}

void DashboardView::UpdateChatGptImportLayout()
{
    auto const showDetails = m_chatGptDetailsExpanded && !m_chatGptDetails.empty();
    m_chatGptDetailsPanel.Visibility(showDetails ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_chatGptDetailsButton.Content(winrt::box_value(winrt::hstring{
        showDetails ? L"收起详情" : L"查看详情" }));

}

void DashboardView::ShowPage(DashboardPage page)
{
    m_currentPage = page;
    m_overviewPage.Visibility(page == DashboardPage::Overview ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_detailsPage.Visibility(page == DashboardPage::Details ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_trendsPage.Visibility(page == DashboardPage::Trends ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_settingsPage.Visibility(page == DashboardPage::Settings ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    UpdateNavigationState();
}

void DashboardView::BuildShell()
{
    auto const shell = ShellColor();
    m_root = controls::Grid{};
    m_root.Background(Brush(Color(shell[0], shell[1], shell[2])));

    AddRow(m_root, Pixels(72));
    AddRow(m_root, Star());
    AddRow(m_root, Pixels(84));

    auto header = BuildHeader();
    controls::Grid::SetRow(header, 0);
    m_root.Children().Append(header);

    m_pageHost = controls::Grid{};
    m_overviewPage = BuildOverviewPage();
    m_detailsPage = BuildDetailsPage();
    m_trendsPage = BuildTrendsPage();
    m_settingsPage = BuildSettingsPage();
    m_pageHost.Children().Append(m_overviewPage);
    m_pageHost.Children().Append(m_detailsPage);
    m_pageHost.Children().Append(m_trendsPage);
    m_pageHost.Children().Append(m_settingsPage);

    m_scroller = controls::ScrollViewer{};
    m_scroller.Padding({ 28, 12, 28, 12 });
    m_scroller.HorizontalScrollMode(controls::ScrollMode::Disabled);
    m_scroller.HorizontalScrollBarVisibility(controls::ScrollBarVisibility::Disabled);
    m_scroller.VerticalScrollMode(controls::ScrollMode::Enabled);
    m_scroller.VerticalScrollBarVisibility(controls::ScrollBarVisibility::Auto);
    m_scroller.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    m_scroller.VerticalContentAlignment(mux::VerticalAlignment::Top);
    m_scroller.Content(m_pageHost);
    controls::Grid::SetRow(m_scroller, 1);
    m_root.Children().Append(m_scroller);

    auto navigation = BuildBottomNavigation();
    controls::Grid::SetRow(navigation, 2);
    m_root.Children().Append(navigation);
}

controls::Grid DashboardView::BuildHeader()
{
    controls::Grid header;
    header.Padding({ 28, 12, 28, 8 });
    AddColumn(header, Star());
    AddColumn(header, mux::GridLengthHelper::Auto());

    controls::StackPanel brand;
    brand.Orientation(controls::Orientation::Horizontal);
    brand.Spacing(12);
    brand.VerticalAlignment(mux::VerticalAlignment::Center);

    auto markText = Text(L"T·", 16, Color(18, 16, 17), 700, true);
    markText.HorizontalAlignment(mux::HorizontalAlignment::Center);
    markText.VerticalAlignment(mux::VerticalAlignment::Center);

    controls::Border mark;
    mark.Width(38);
    mark.Height(38);
    mark.CornerRadius(Radius(13));
    mark.Background(Brush(Color(240, 63, 22)));
    mark.Child(markText);
    brand.Children().Append(mark);

    controls::StackPanel title;
    title.Spacing(1);
    title.Children().Append(Text(L"Tokenometer", 18, Color(247, 247, 245), 650));
    title.Children().Append(Text(L"Codex + ChatGPT usage intelligence", 10.5, Color(143, 139, 140)));
    brand.Children().Append(title);
    header.Children().Append(brand);

    controls::StackPanel status;
    status.Orientation(controls::Orientation::Horizontal);
    status.Spacing(9);
    status.VerticalAlignment(mux::VerticalAlignment::Center);

    m_statusDot = shapes::Ellipse{};
    m_statusDot.Width(8);
    m_statusDot.Height(8);
    m_statusDot.Fill(Brush(Color(98, 223, 125)));
    m_statusDot.VerticalAlignment(mux::VerticalAlignment::Center);
    status.Children().Append(m_statusDot);

    controls::StackPanel statusCopy;
    statusCopy.Spacing(1);
    m_statusText = Text(L"已连接", 11.5, Color(247, 247, 245), 600);
    m_statusDetail = Text(L"刚刚更新", 9.5, Color(143, 139, 140));
    statusCopy.Children().Append(m_statusText);
    statusCopy.Children().Append(m_statusDetail);
    status.Children().Append(statusCopy);

    controls::Border statusPanel;
    statusPanel.Background(Brush(Color(38, 36, 37)));
    statusPanel.BorderBrush(Brush(Color(255, 255, 255, 18)));
    statusPanel.BorderThickness({ 1 });
    statusPanel.CornerRadius(Radius(17));
    statusPanel.Padding({ 14, 8, 14, 8 });
    statusPanel.Child(status);
    controls::Grid::SetColumn(statusPanel, 1);
    header.Children().Append(statusPanel);
    return header;
}

controls::Grid DashboardView::BuildOverviewPage()
{
    controls::Grid page;
    page.Height(548);
    page.ColumnSpacing(16);
    page.RowSpacing(16);
    AddColumn(page, Star(0.92));
    AddColumn(page, Star(1.42));
    AddColumn(page, Star(1.02));
    AddRow(page, Pixels(254));
    AddRow(page, Pixels(278));

    controls::StackPanel overview;
    overview.Spacing(8);
    m_totalTokensText = Text(L"0", 38, Color(247, 247, 245), 650, true);
    overview.Children().Append(m_totalTokensText);
    overview.Children().Append(Text(L"订阅费用不可用", 11, Color(143, 139, 140)));

    controls::StackPanel emptyCopy;
    emptyCopy.Spacing(2);
    m_emptyOverviewTitle = Text(L"还没有使用记录", 11.5, Color(247, 247, 245), 600);
    m_emptyOverviewDetail = Text(L"打开 Codex 后，首批记录会自动出现在这里。", 9.5, Color(143, 139, 140));
    emptyCopy.Children().Append(m_emptyOverviewTitle);
    emptyCopy.Children().Append(m_emptyOverviewDetail);
    m_overviewEmptyState = SoftPanel(emptyCopy);
    overview.Children().Append(m_overviewEmptyState);

    m_overviewMetricsPanel = controls::StackPanel{};
    m_overviewMetricsPanel.Spacing(8);
    m_overviewMetricsPanel.Children().Append(Progress(
        0.0,
        Color(98, 223, 125),
        &m_cacheProgressFill,
        &m_cacheProgressRest));
    m_overviewMetricsPanel.Children().Append(DynamicStatLine(
        L"Cache hit", m_cacheHitText, L"0.0%", Color(98, 223, 125)));
    m_overviewMetricsPanel.Children().Append(DynamicStatLine(
        L"Output", m_outputTokensText, L"0"));
    overview.Children().Append(m_overviewMetricsPanel);
    auto overviewCard = Card(L"Token 总览", Color(98, 223, 125), overview);
    page.Children().Append(overviewCard);

    controls::StackPanel activity;
    activity.Spacing(10);
    activity.Children().Append(DynamicStatLine(
        L"过去 24 小时", m_dayTokensText, L"0", Color(255, 253, 142)));

    controls::StackPanel bars;
    bars.Height(104);
    bars.Orientation(controls::Orientation::Horizontal);
    bars.Spacing(8);
    bars.VerticalAlignment(mux::VerticalAlignment::Bottom);
    for (size_t index = 0; index < 12; ++index)
    {
        controls::Border bar;
        bar.Width(16);
        bar.Height(12);
        bar.CornerRadius(Radius(8));
        bar.Background(Brush(Color(98, 223, 125)));
        bar.Opacity(0.2);
        bar.VerticalAlignment(mux::VerticalAlignment::Bottom);
        bars.Children().Append(bar);
        m_dailyBars.push_back(bar);
    }
    activity.Children().Append(bars);
    activity.Children().Append(DynamicStatLine(
        L"Messages", m_dayMessagesText, L"0"));
    activity.Children().Append(DynamicStatLine(
        L"Tool calls", m_dayToolCallsText, L"0"));
    auto activityCard = Card(L"Token 活动", Color(255, 253, 142), activity);
    controls::Grid::SetColumn(activityCard, 1);
    page.Children().Append(activityCard);

    controls::StackPanel limits;
    limits.Spacing(10);
    controls::Grid limitTop;
    AddColumn(limitTop, Star());
    AddColumn(limitTop, mux::GridLengthHelper::Auto());
    m_codexLimitName = Text(L"等待 Codex 限额快照", 13.5, Color(247, 247, 245), 600);
    limitTop.Children().Append(m_codexLimitName);
    m_codexLimitValue = Text(L"—", 13, Color(98, 223, 125), 600, true);
    controls::Grid::SetColumn(m_codexLimitValue, 1);
    limitTop.Children().Append(m_codexLimitValue);
    limits.Children().Append(limitTop);
    limits.Children().Append(Progress(
        0.0,
        Color(98, 223, 125),
        &m_codexProgressFill,
        &m_codexProgressRest));
    m_codexLimitReset = Text(L"采集到 used_percent 后显示", 10.5, Color(143, 139, 140));
    limits.Children().Append(m_codexLimitReset);
    limits.Children().Append(Text(
        L"仅展示 Codex 本地记录中实际提供的限额数据。",
        9.5,
        Color(143, 139, 140)));
    auto limitsCard = Card(L"Codex 限额", Color(240, 63, 22), limits);
    controls::Grid::SetRow(limitsCard, 1);
    page.Children().Append(limitsCard);

    controls::StackPanel heat;
    heat.Spacing(12);
    heat.Children().Append(DynamicStatLine(
        L"活跃天数", m_activeDaysText, L"0", Color(255, 253, 142)));
    heat.Children().Append(Heatmap(20, Color(255, 253, 142), &m_heatmapCells));
    m_heatmapCaption = Text(L"尚无活动数据", 10.5, Color(143, 139, 140));
    heat.Children().Append(m_heatmapCaption);
    auto heatCard = Card(L"活动热图", Color(255, 253, 142), heat);
    controls::Grid::SetColumn(heatCard, 1);
    controls::Grid::SetRow(heatCard, 1);
    page.Children().Append(heatCard);

    controls::StackPanel accounts;
    accounts.Spacing(12);
    controls::StackPanel chatgptCopy;
    chatgptCopy.Spacing(3);
    chatgptCopy.Children().Append(Text(L"ChatGPT", 13, Color(247, 247, 245), 600));
    m_chatGptOverviewValue = Text(L"等待官方导出导入", 11, Color(255, 253, 142), 600);
    m_chatGptOverviewDetail = Text(L"实时 token 不可用", 9.5, Color(143, 139, 140));
    chatgptCopy.Children().Append(m_chatGptOverviewValue);
    chatgptCopy.Children().Append(m_chatGptOverviewDetail);
    accounts.Children().Append(SoftPanel(chatgptCopy));
    accounts.Children().Append(Text(L"近期会话", 12, Color(143, 139, 140), 600));
    for (int index = 0; index < 3; ++index)
    {
        controls::TextBlock title{ nullptr };
        controls::TextBlock detail{ nullptr };
        controls::TextBlock value{ nullptr };
        auto row = SessionRow(title, detail, value);
        accounts.Children().Append(row);
        m_sessionRows.push_back(row);
        m_sessionTitles.push_back(title);
        m_sessionDetails.push_back(detail);
        m_sessionValues.push_back(value);
    }

    controls::StackPanel noSessions;
    noSessions.Spacing(3);
    noSessions.Children().Append(Text(L"暂无会话", 11.5, Color(247, 247, 245), 600));
    noSessions.Children().Append(Text(L"采集完成后最多显示最近 3 条。", 9.5, Color(143, 139, 140)));
    m_recentEmptyState = SoftPanel(noSessions);
    accounts.Children().Append(m_recentEmptyState);
    auto accountsCard = Card(L"ChatGPT 与近期会话", Color(240, 63, 22), accounts);
    controls::Grid::SetColumn(accountsCard, 2);
    controls::Grid::SetRowSpan(accountsCard, 2);
    page.Children().Append(accountsCard);
    return page;
}

controls::Grid DashboardView::BuildDetailsPage()
{
    controls::Grid page;
    page.Height(548);
    page.RowSpacing(16);
    AddRow(page, Pixels(98));
    AddRow(page, Star());

    auto intro = PageIntro(
        L"工具 · 模型 · 会话 · 设备 · 项目 · 账户",
        L"逐层查看每一枚 token",
        L"选择分组行查看缓存拆分；选择会话后按需展开提示和工具调用。",
        Color(98, 223, 125));
    page.Children().Append(intro);

    controls::Grid content;
    content.ColumnSpacing(16);
    AddColumn(content, Star(1.18));
    AddColumn(content, Star(0.92));
    AddColumn(content, Star(1.22));
    controls::Grid::SetRow(content, 1);

    controls::StackPanel breakdown;
    breakdown.Spacing(10);
    controls::StackPanel dimensions;
    dimensions.Orientation(controls::Orientation::Horizontal);
    dimensions.Spacing(4);
    constexpr std::array<std::wstring_view, 6> labels{
        L"工具", L"模型", L"会话", L"设备", L"项目", L"账户"
    };
    constexpr std::array dimensionValues{
        DetailsDimension::Tool,
        DetailsDimension::Model,
        DetailsDimension::Session,
        DetailsDimension::Device,
        DetailsDimension::Project,
        DetailsDimension::Account,
    };
    for (size_t index = 0; index < labels.size(); ++index)
    {
        controls::Button button;
        button.Width(46);
        button.Height(30);
        button.Padding({ 0 });
        button.CornerRadius(Radius(10));
        button.BorderThickness({ 1 });
        button.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
        button.FontSize(10.5);
        button.FontWeight({ 600 });
        button.Content(winrt::box_value(winrt::hstring{ labels[index] }));
        automation::AutomationProperties::SetName(button, winrt::hstring{ labels[index] });
        auto const dimension = dimensionValues[index];
        button.Click([this, dimension](auto const&, auto const&)
        {
            m_detailsDimension = dimension;
            UpdateDetailsDimensionButtons();
            if (m_detailsCallbacks.onDimensionChanged)
            {
                m_detailsCallbacks.onDimensionChanged(dimension);
            }
        });
        dimensions.Children().Append(button);
        m_detailsDimensionButtons.push_back(button);
    }
    breakdown.Children().Append(dimensions);
    m_breakdownList = controls::StackPanel{};
    m_breakdownList.Spacing(8);
    breakdown.Children().Append(m_breakdownList);
    auto listCard = Card(L"使用细分", Color(98, 223, 125), breakdown);
    content.Children().Append(listCard);

    controls::StackPanel cache;
    cache.Spacing(12);
    controls::StackPanel selectionCopy;
    selectionCopy.Spacing(3);
    selectionCopy.Children().Append(Text(L"选择一项查看详情", 11.5, Color(247, 247, 245), 600));
    selectionCopy.Children().Append(Text(L"这里会显示实际的输入与缓存拆分。", 9.5, Color(143, 139, 140)));
    m_detailsSelectionState = SoftPanel(selectionCopy);
    cache.Children().Append(m_detailsSelectionState);

    m_detailsMetricsPanel = controls::StackPanel{};
    m_detailsMetricsPanel.Spacing(12);
    m_detailsSelectedTitle = Text(L"", 13, Color(247, 247, 245), 600);
    m_detailsMetricsPanel.Children().Append(m_detailsSelectedTitle);
    m_detailsMetricsPanel.Children().Append(Progress(
        0.0,
        Color(98, 223, 125),
        &m_detailsCacheProgressFill,
        &m_detailsCacheProgressRest));
    m_detailsMetricsPanel.Children().Append(DynamicStatLine(
        L"Input", m_detailsInputText, L"—"));
    m_detailsMetricsPanel.Children().Append(DynamicStatLine(
        L"Cache hit", m_detailsCacheHitTokensText, L"—", Color(98, 223, 125)));
    m_detailsMetricsPanel.Children().Append(DynamicStatLine(
        L"Cache miss", m_detailsCacheMissTokensText, L"—", Color(255, 253, 142)));
    m_detailsMetricsPanel.Children().Append(DynamicStatLine(
        L"Output", m_detailsOutputText, L"—", Color(240, 63, 22)));
    m_detailsMetricsPanel.Children().Append(DynamicStatLine(
        L"Hit rate", m_detailsHitRateText, L"—", Color(98, 223, 125)));
    cache.Children().Append(m_detailsMetricsPanel);
    auto cacheCard = Card(L"缓存与 Token", Color(255, 253, 142), cache);
    controls::Grid::SetColumn(cacheCard, 1);
    content.Children().Append(cacheCard);

    m_detailsSessionsPanel = controls::StackPanel{};
    m_detailsSessionsPanel.Spacing(8);
    auto sessionsCard = Card(L"会话与工具", Color(240, 63, 22), m_detailsSessionsPanel);
    controls::Grid::SetColumn(sessionsCard, 2);
    content.Children().Append(sessionsCard);

    page.Children().Append(content);
    return page;
}

controls::Grid DashboardView::BuildTrendsPage()
{
    controls::Grid page;
    page.Height(548);
    page.RowSpacing(14);
    AddRow(page, Pixels(72));
    AddRow(page, Pixels(280));
    AddRow(page, Pixels(168));

    controls::Grid controlsRow;
    controlsRow.ColumnSpacing(18);
    controlsRow.Padding({ 16, 10, 16, 10 });
    controlsRow.Background(Brush(Color(38, 36, 37)));
    controlsRow.CornerRadius(Radius(18));
    AddColumn(controlsRow, mux::GridLengthHelper::Auto());
    AddColumn(controlsRow, mux::GridLengthHelper::Auto());
    AddColumn(controlsRow, Star());
    AddColumn(controlsRow, mux::GridLengthHelper::Auto());

    auto makeChoice = [](std::wstring_view label)
    {
        controls::Button button;
        button.Height(32);
        button.MinWidth(58);
        button.Padding({ 12, 0, 12, 0 });
        button.CornerRadius(Radius(11));
        button.BorderThickness({ 1 });
        button.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
        button.FontSize(10.5);
        button.FontWeight({ 600 });
        button.Content(winrt::box_value(winrt::hstring{ label }));
        automation::AutomationProperties::SetName(button, winrt::hstring{ label });
        return button;
    };

    controls::StackPanel groupButtons;
    groupButtons.Orientation(controls::Orientation::Horizontal);
    groupButtons.Spacing(4);
    auto byTool = makeChoice(L"按工具");
    byTool.Click([this](auto const&, auto const&)
    {
        m_trendGroup = TrendGroup::Tool;
        UpdateTrendButtons();
        if (m_trendCallbacks.onGroupChanged)
        {
            m_trendCallbacks.onGroupChanged(m_trendGroup);
        }
    });
    auto byModel = makeChoice(L"按模型");
    byModel.Click([this](auto const&, auto const&)
    {
        m_trendGroup = TrendGroup::Model;
        UpdateTrendButtons();
        if (m_trendCallbacks.onGroupChanged)
        {
            m_trendCallbacks.onGroupChanged(m_trendGroup);
        }
    });
    groupButtons.Children().Append(byTool);
    groupButtons.Children().Append(byModel);
    m_trendGroupButtons.push_back(byTool);
    m_trendGroupButtons.push_back(byModel);
    controlsRow.Children().Append(groupButtons);

    controls::StackPanel chartButtons;
    chartButtons.Orientation(controls::Orientation::Horizontal);
    chartButtons.Spacing(4);
    auto bars = makeChoice(L"柱图");
    bars.Click([this](auto const&, auto const&)
    {
        m_trendChart = TrendChart::Bars;
        UpdateTrendButtons();
        if (m_trendCallbacks.onChartChanged)
        {
            m_trendCallbacks.onChartChanged(m_trendChart);
        }
    });
    auto kline = makeChoice(L"K-line");
    kline.Click([this](auto const&, auto const&)
    {
        m_trendChart = TrendChart::Kline;
        UpdateTrendButtons();
        if (m_trendCallbacks.onChartChanged)
        {
            m_trendCallbacks.onChartChanged(m_trendChart);
        }
    });
    chartButtons.Children().Append(bars);
    chartButtons.Children().Append(kline);
    m_trendChartButtons.push_back(bars);
    m_trendChartButtons.push_back(kline);
    controls::Grid::SetColumn(chartButtons, 1);
    controlsRow.Children().Append(chartButtons);

    controls::StackPanel rangeButtons;
    rangeButtons.Orientation(controls::Orientation::Horizontal);
    rangeButtons.Spacing(4);
    constexpr std::array<std::wstring_view, 4> rangeLabels{ L"7D", L"30D", L"90D", L"365D" };
    constexpr std::array rangeValues{
        TrendRange::Days7,
        TrendRange::Days30,
        TrendRange::Days90,
        TrendRange::Days365,
    };
    for (size_t index = 0; index < rangeLabels.size(); ++index)
    {
        auto button = makeChoice(rangeLabels[index]);
        button.MinWidth(50);
        auto const range = rangeValues[index];
        button.Click([this, range](auto const&, auto const&)
        {
            m_trendRange = range;
            UpdateTrendButtons();
            if (m_trendCallbacks.onRangeChanged)
            {
                m_trendCallbacks.onRangeChanged(range);
            }
        });
        rangeButtons.Children().Append(button);
        m_trendRangeButtons.push_back(button);
    }
    controls::Grid::SetColumn(rangeButtons, 3);
    controlsRow.Children().Append(rangeButtons);
    page.Children().Append(controlsRow);

    controls::StackPanel chartBody;
    chartBody.Spacing(8);
    m_trendChartCaption = Text(L"等待趋势数据", 10.5, Color(143, 139, 140));
    chartBody.Children().Append(m_trendChartCaption);
    m_trendChartHost = controls::Grid{};
    m_trendChartHost.Height(166);
    chartBody.Children().Append(m_trendChartHost);
    auto chartCard = Card(L"历史趋势", Color(98, 223, 125), chartBody);
    controls::Grid::SetRow(chartCard, 1);
    page.Children().Append(chartCard);

    controls::Grid bottom;
    bottom.ColumnSpacing(16);
    AddColumn(bottom, Star(1.55));
    AddColumn(bottom, Star(0.85));
    controls::Grid::SetRow(bottom, 2);

    controls::StackPanel heatBody;
    heatBody.Spacing(6);
    controls::Grid streaks;
    AddColumn(streaks, mux::GridLengthHelper::Auto());
    AddColumn(streaks, mux::GridLengthHelper::Auto());
    AddColumn(streaks, Star());
    AddColumn(streaks, mux::GridLengthHelper::Auto());
    streaks.Children().Append(Text(L"Current", 9.5, Color(143, 139, 140)));
    m_currentStreakText = Text(L"0", 10.5, Color(98, 223, 125), 600, true);
    m_currentStreakText.Margin({ 7, 0, 0, 0 });
    controls::Grid::SetColumn(m_currentStreakText, 1);
    streaks.Children().Append(m_currentStreakText);
    auto longestLabel = Text(L"Longest", 9.5, Color(143, 139, 140));
    controls::Grid::SetColumn(longestLabel, 2);
    longestLabel.HorizontalAlignment(mux::HorizontalAlignment::Right);
    longestLabel.Margin({ 0, 0, 7, 0 });
    streaks.Children().Append(longestLabel);
    m_longestStreakText = Text(L"0", 10.5, Color(255, 253, 142), 600, true);
    controls::Grid::SetColumn(m_longestStreakText, 3);
    streaks.Children().Append(m_longestStreakText);
    heatBody.Children().Append(streaks);
    heatBody.Children().Append(Heatmap(
        53,
        Color(98, 223, 125),
        &m_trendHeatCells,
        6,
        1.5));
    m_trendHeatCaption = Text(
        L"365 日窗口 · 小时历史仅保留 400 日",
        9,
        Color(143, 139, 140));
    heatBody.Children().Append(m_trendHeatCaption);
    auto heatCard = Card(L"年度活动", Color(255, 253, 142), heatBody);
    bottom.Children().Append(heatCard);

    m_trendLegend = controls::StackPanel{};
    m_trendLegend.Spacing(5);
    auto legendCard = Card(L"系列", Color(240, 63, 22), m_trendLegend);
    controls::Grid::SetColumn(legendCard, 1);
    bottom.Children().Append(legendCard);
    page.Children().Append(bottom);
    return page;
}

controls::Grid DashboardView::BuildSettingsPage()
{
    controls::Grid page;
    page.MinHeight(548);
    page.ColumnSpacing(16);
    AddColumn(page, Star(1.15));
    AddColumn(page, Star(0.85));

    controls::StackPanel importBody;
    importBody.Spacing(14);

    auto badgeText = Text(L"OFFICIAL EXPORT", 9.5, Color(18, 16, 17), 700, true);
    badgeText.HorizontalAlignment(mux::HorizontalAlignment::Center);
    badgeText.VerticalAlignment(mux::VerticalAlignment::Center);
    controls::Border badge;
    badge.Width(128);
    badge.Height(26);
    badge.HorizontalAlignment(mux::HorizontalAlignment::Left);
    badge.Background(Brush(Color(98, 223, 125)));
    badge.CornerRadius(Radius(13));
    badge.Child(badgeText);
    importBody.Children().Append(badge);

    auto hero = Text(L"导入 ChatGPT 使用记录", 25, Color(247, 247, 245), 650);
    importBody.Children().Append(hero);
    auto description = Text(
        L"导入 conversations.json；token 为估算，不读取浏览器 / Cookie，不存正文。",
        11.5,
        Color(154, 150, 151));
    description.TextWrapping(mux::TextWrapping::Wrap);
    description.TextTrimming(mux::TextTrimming::None);
    importBody.Children().Append(description);

    controls::StackPanel boundaries;
    boundaries.Spacing(8);
    auto appendBoundary = [&boundaries](std::wstring_view marker, std::wstring_view copy,
                                        winrt::Windows::UI::Color const& color)
    {
        controls::Grid row;
        AddColumn(row, Pixels(24));
        AddColumn(row, Star());
        auto mark = Text(marker, 10.5, color, 700, true);
        mark.VerticalAlignment(mux::VerticalAlignment::Center);
        row.Children().Append(mark);
        auto body = Text(copy, 10.5, Color(247, 247, 245), 500);
        body.TextWrapping(mux::TextWrapping::Wrap);
        body.TextTrimming(mux::TextTrimming::None);
        controls::Grid::SetColumn(body, 1);
        row.Children().Append(body);
        boundaries.Children().Append(row);
    };
    appendBoundary(L"01", L"仅支持官方导出的 conversations.json、conversations-1.json 等文件；不解压 ZIP。", Color(255, 253, 142));
    appendBoundary(L"02", L"可一次多选编号分片，也可稍后重复选择；导入按稳定 ID 幂等去重。", Color(98, 223, 125));
    appendBoundary(L"03", L"只保存统计与来源标识；提示、回答和 Cookie 均不会写入应用状态。", Color(240, 63, 22));
    importBody.Children().Append(SoftPanel(boundaries));

    controls::StackPanel accountField;
    accountField.Spacing(6);
    accountField.Children().Append(Text(L"当前账户标签", 9.5, Color(143, 139, 140), 600));
    m_chatGptAccountLabel = Text(L"ChatGPT", 11, Color(247, 247, 245), 650, true);
    accountField.Children().Append(m_chatGptAccountLabel);
    auto accountHint = Text(
        L"可在 Windows 文件选择器中修改，用于隔离多个账户的导入统计。",
        9,
        Color(143, 139, 140));
    accountHint.TextWrapping(mux::TextWrapping::Wrap);
    accountHint.TextTrimming(mux::TextTrimming::None);
    accountField.Children().Append(accountHint);
    importBody.Children().Append(SoftPanel(accountField));

    m_chatGptChooseFilesButton = controls::Button{};
    m_chatGptChooseFilesButton.Height(44);
    m_chatGptChooseFilesButton.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    m_chatGptChooseFilesButton.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
    m_chatGptChooseFilesButton.Background(Brush(Color(240, 63, 22)));
    m_chatGptChooseFilesButton.Foreground(Brush(Color(247, 247, 245)));
    m_chatGptChooseFilesButton.BorderThickness({ 0 });
    m_chatGptChooseFilesButton.CornerRadius(Radius(14));
    m_chatGptChooseFilesButton.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
    m_chatGptChooseFilesButton.FontSize(12);
    m_chatGptChooseFilesButton.FontWeight({ 650 });
    automation::AutomationProperties::SetName(
        m_chatGptChooseFilesButton,
        winrt::hstring{ L"选择 ChatGPT 导出 JSON 文件" });
    m_chatGptChooseFilesButton.Click([this](auto const&, auto const&)
    {
        if (m_chatGptImportCallbacks.onChooseFilesRequested)
        {
            m_chatGptImportCallbacks.onChooseFilesRequested(
                std::wstring{ m_chatGptAccountLabel.Text().c_str() });
        }
    });
    importBody.Children().Append(m_chatGptChooseFilesButton);

    auto importCard = Card(L"ChatGPT 数据导入", Color(240, 63, 22), importBody);
    page.Children().Append(importCard);

    controls::StackPanel statusBody;
    statusBody.Spacing(12);

    controls::Grid stateRow;
    AddColumn(stateRow, Pixels(18));
    AddColumn(stateRow, Star());
    m_chatGptImportDot = shapes::Ellipse{};
    m_chatGptImportDot.Width(9);
    m_chatGptImportDot.Height(9);
    m_chatGptImportDot.VerticalAlignment(mux::VerticalAlignment::Center);
    stateRow.Children().Append(m_chatGptImportDot);
    controls::StackPanel stateCopy;
    stateCopy.Spacing(3);
    m_chatGptImportTitle = Text(L"等待导入", 13, Color(247, 247, 245), 600);
    stateCopy.Children().Append(m_chatGptImportTitle);
    m_chatGptImportMessage = Text(L"", 9.5, Color(143, 139, 140));
    m_chatGptImportMessage.TextWrapping(mux::TextWrapping::Wrap);
    m_chatGptImportMessage.TextTrimming(mux::TextTrimming::None);
    stateCopy.Children().Append(m_chatGptImportMessage);
    controls::Grid::SetColumn(stateCopy, 1);
    stateRow.Children().Append(stateCopy);
    statusBody.Children().Append(stateRow);

    controls::StackPanel filesCopy;
    filesCopy.Spacing(3);
    filesCopy.Children().Append(Text(L"所选文件", 9.5, Color(143, 139, 140), 600));
    m_chatGptFilesText = Text(L"尚未选择 JSON 文件", 10.5, Color(247, 247, 245), 600, true);
    m_chatGptFilesText.TextWrapping(mux::TextWrapping::Wrap);
    m_chatGptFilesText.TextTrimming(mux::TextTrimming::None);
    filesCopy.Children().Append(m_chatGptFilesText);
    statusBody.Children().Append(SoftPanel(filesCopy));

    controls::StackPanel results;
    results.Spacing(8);
    results.Children().Append(DynamicStatLine(
        L"账户总会话", m_chatGptConversationCount, L"—", Color(98, 223, 125)));
    results.Children().Append(DynamicStatLine(
        L"账户总估算 token", m_chatGptEstimatedTokens, L"—", Color(255, 253, 142)));
    results.Children().Append(DynamicStatLine(
        L"本次跳过会话", m_chatGptSkippedCount, L"—", Color(143, 139, 140)));
    results.Children().Append(DynamicStatLine(
        L"未变更文件", m_chatGptUnchangedCount, L"—", Color(143, 139, 140)));
    results.Children().Append(DynamicStatLine(
        L"本次错误", m_chatGptErrorCount, L"—", Color(240, 63, 22)));
    statusBody.Children().Append(results);

    auto estimateNote = Text(
        L"估算值仅用于趋势与分组统计，不代表 OpenAI 账单 token。",
        9,
        Color(143, 139, 140));
    estimateNote.TextWrapping(mux::TextWrapping::Wrap);
    estimateNote.TextTrimming(mux::TextTrimming::None);
    statusBody.Children().Append(estimateNote);

    m_chatGptDetailsButton = controls::Button{};
    m_chatGptDetailsButton.Height(34);
    m_chatGptDetailsButton.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    m_chatGptDetailsButton.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
    m_chatGptDetailsButton.Background(Brush(Color(29, 27, 28)));
    m_chatGptDetailsButton.Foreground(Brush(Color(255, 253, 142)));
    m_chatGptDetailsButton.BorderBrush(Brush(Color(255, 255, 255, 20)));
    m_chatGptDetailsButton.BorderThickness({ 1 });
    m_chatGptDetailsButton.CornerRadius(Radius(11));
    m_chatGptDetailsButton.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
    m_chatGptDetailsButton.FontSize(10.5);
    automation::AutomationProperties::SetName(
        m_chatGptDetailsButton,
        winrt::hstring{ L"查看 ChatGPT 导入详情" });
    m_chatGptDetailsButton.Click([this](auto const&, auto const&)
    {
        m_chatGptDetailsExpanded = !m_chatGptDetailsExpanded;
        UpdateChatGptImportLayout();
        if (m_chatGptImportCallbacks.onDetailsToggled)
        {
            m_chatGptImportCallbacks.onDetailsToggled(m_chatGptDetailsExpanded);
        }
    });
    statusBody.Children().Append(m_chatGptDetailsButton);

    m_chatGptDetailsText = Text(L"", 9.5, Color(247, 247, 245), 400, true);
    m_chatGptDetailsText.TextWrapping(mux::TextWrapping::Wrap);
    m_chatGptDetailsText.TextTrimming(mux::TextTrimming::None);
    m_chatGptDetailsPanel = SoftPanel(m_chatGptDetailsText);
    m_chatGptDetailsPanel.Visibility(mux::Visibility::Collapsed);
    statusBody.Children().Append(m_chatGptDetailsPanel);

    auto statusCard = Card(L"导入状态", Color(98, 223, 125), statusBody);
    controls::Grid::SetColumn(statusCard, 1);
    page.Children().Append(statusCard);
    return page;
}

controls::Border DashboardView::BuildBottomNavigation()
{
    controls::StackPanel items;
    items.Orientation(controls::Orientation::Horizontal);
    items.Spacing(6);
    items.HorizontalAlignment(mux::HorizontalAlignment::Center);
    items.VerticalAlignment(mux::VerticalAlignment::Center);

    m_overviewButton = MakeNavigationButton(L"总览", DashboardPage::Overview);
    m_detailsButton = MakeNavigationButton(L"明细", DashboardPage::Details);
    m_trendsButton = MakeNavigationButton(L"趋势", DashboardPage::Trends);
    m_settingsButton = MakeNavigationButton(L"设置", DashboardPage::Settings);
    items.Children().Append(m_overviewButton);
    items.Children().Append(m_detailsButton);
    items.Children().Append(m_trendsButton);
    items.Children().Append(m_settingsButton);

    controls::Border navigation;
    navigation.Width(502);
    navigation.Height(62);
    navigation.HorizontalAlignment(mux::HorizontalAlignment::Center);
    navigation.VerticalAlignment(mux::VerticalAlignment::Center);
    navigation.Background(Brush(Color(28, 26, 27, 248)));
    navigation.BorderBrush(Brush(Color(255, 255, 255, 22)));
    navigation.BorderThickness({ 1 });
    navigation.CornerRadius(Radius(31));
    navigation.Padding({ 8, 7, 8, 7 });
    navigation.Child(items);
    return navigation;
}

controls::Button DashboardView::MakeNavigationButton(
    std::wstring_view label,
    DashboardPage page)
{
    controls::Button button;
    button.Width(116);
    button.Height(46);
    button.Padding({ 16, 0, 16, 0 });
    button.CornerRadius(Radius(23));
    button.BorderThickness({ 1 });
    button.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
    button.FontSize(13);
    button.FontWeight({ 600 });
    button.Content(winrt::box_value(winrt::hstring{ label }));
    automation::AutomationProperties::SetName(button, winrt::hstring{ label });
    button.Click([this, page](auto const&, auto const&) { ShowPage(page); });
    return button;
}

void DashboardView::UpdateNavigationState()
{
    auto update = [this](controls::Button const& button, DashboardPage page)
    {
        auto const selected = page == m_currentPage;
        button.Background(Brush(selected ? Color(55, 52, 53) : Color(0, 0, 0, 0)));
        button.Foreground(Brush(selected ? Color(247, 247, 245) : Color(143, 139, 140)));
        button.BorderBrush(Brush(selected ? Color(240, 63, 22) : Color(0, 0, 0, 0)));
        automation::AutomationProperties::SetHelpText(
            button,
            selected ? L"当前页面" : L"切换页面");
    };

    update(m_overviewButton, DashboardPage::Overview);
    update(m_detailsButton, DashboardPage::Details);
    update(m_trendsButton, DashboardPage::Trends);
    update(m_settingsButton, DashboardPage::Settings);
}
}
