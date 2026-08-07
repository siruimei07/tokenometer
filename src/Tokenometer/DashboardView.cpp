#include "DashboardView.h"

#include <algorithm>
#include <array>
#include <string>

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

    controls::Grid Progress(double ratio, winrt::Windows::UI::Color const& color)
    {
        ratio = std::clamp(ratio, 0.0, 1.0);

        controls::Grid track;
        track.Height(8);
        AddColumn(track, Star(std::max(ratio, 0.001)));
        AddColumn(track, Star(std::max(1.0 - ratio, 0.001)));

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

    controls::Grid StatLine(
        std::wstring_view label,
        std::wstring_view value,
        winrt::Windows::UI::Color const& valueColor = Color(247, 247, 245))
    {
        controls::Grid row;
        AddColumn(row, Star());
        AddColumn(row, mux::GridLengthHelper::Auto());

        auto labelText = Text(label, 12, Color(154, 150, 151));
        row.Children().Append(labelText);

        auto valueText = Text(value, 12, valueColor, 600, true);
        valueText.TextAlignment(mux::TextAlignment::Right);
        controls::Grid::SetColumn(valueText, 1);
        row.Children().Append(valueText);
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

    controls::StackPanel Heatmap(int weeks, winrt::Windows::UI::Color const& accent)
    {
        controls::StackPanel map;
        map.Orientation(controls::Orientation::Horizontal);
        map.Spacing(3);
        map.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        for (int week = 0; week < weeks; ++week)
        {
            controls::StackPanel column;
            column.Spacing(3);
            for (int day = 0; day < 7; ++day)
            {
                auto const active = ((week * 3 + day * 5 + week / 2) % 9) > 3;
                controls::Border cell;
                cell.Width(10);
                cell.Height(10);
                cell.CornerRadius(Radius(3));
                cell.Background(Brush(active ? accent : Color(55, 52, 53)));
                cell.Opacity(active ? 0.42 + 0.08 * ((week + day) % 6) : 0.5);
                column.Children().Append(cell);
            }
            map.Children().Append(column);
        }
        return map;
    }

    controls::Border Metric(
        std::wstring_view label,
        std::wstring_view value,
        std::wstring_view detail,
        winrt::Windows::UI::Color const& accent)
    {
        controls::StackPanel content;
        content.Spacing(4);
        content.Children().Append(Text(label, 11, Color(143, 139, 140), 500));
        content.Children().Append(Text(value, 22, Color(247, 247, 245), 650, true));
        content.Children().Append(Text(detail, 10.5, accent, 500));
        return SoftPanel(content);
    }

    controls::Border BreakdownRow(
        std::wstring_view name,
        std::wstring_view detail,
        std::wstring_view value,
        double ratio,
        winrt::Windows::UI::Color const& accent)
    {
        controls::StackPanel content;
        content.Spacing(8);

        controls::Grid top;
        AddColumn(top, Star());
        AddColumn(top, mux::GridLengthHelper::Auto());
        top.Children().Append(Text(name, 13.5, Color(247, 247, 245), 600));

        auto valueText = Text(value, 13, Color(247, 247, 245), 600, true);
        controls::Grid::SetColumn(valueText, 1);
        top.Children().Append(valueText);
        content.Children().Append(top);
        content.Children().Append(Progress(ratio, accent));
        content.Children().Append(Text(detail, 10.5, Color(143, 139, 140)));
        return SoftPanel(content);
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

    controls::Grid SettingsBody(
        std::wstring_view first,
        std::wstring_view firstValue,
        std::wstring_view second,
        std::wstring_view secondValue)
    {
        controls::Grid body;
        AddRow(body, Star());
        AddRow(body, Star());
        body.RowSpacing(10);

        auto firstRow = StatLine(first, firstValue, Color(98, 223, 125));
        firstRow.VerticalAlignment(mux::VerticalAlignment::Center);
        body.Children().Append(firstRow);

        auto secondRow = StatLine(second, secondValue, Color(255, 253, 142));
        secondRow.VerticalAlignment(mux::VerticalAlignment::Center);
        controls::Grid::SetRow(secondRow, 1);
        body.Children().Append(secondRow);
        return body;
    }
}

namespace tokenometer
{
DashboardView::DashboardView()
{
    BuildShell();
    SetStatus(L"等待采集器", L"静态界面预览", false);
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

    controls::StackPanel live;
    live.Spacing(10);
    live.Children().Append(Text(L"28,436", 38, Color(247, 247, 245), 650, true));
    live.Children().Append(Text(L"tokens this session · $18.24", 11, Color(143, 139, 140)));
    live.Children().Append(Progress(0.54, Color(98, 223, 125)));
    live.Children().Append(StatLine(L"Cache hit", L"71.4%", Color(98, 223, 125)));
    live.Children().Append(StatLine(L"Output", L"8,126"));
    auto liveCard = Card(L"实时用量", Color(98, 223, 125), live);
    page.Children().Append(liveCard);

    controls::StackPanel activity;
    activity.Spacing(10);
    activity.Children().Append(StatLine(L"过去 24 小时", L"128.6K", Color(255, 253, 142)));

    controls::StackPanel bars;
    bars.Height(104);
    bars.Orientation(controls::Orientation::Horizontal);
    bars.Spacing(8);
    bars.VerticalAlignment(mux::VerticalAlignment::Bottom);
    constexpr std::array heights{ 32.0, 48.0, 42.0, 70.0, 56.0, 86.0, 64.0, 94.0, 78.0, 104.0, 84.0, 96.0 };
    for (size_t index = 0; index < heights.size(); ++index)
    {
        controls::Border bar;
        bar.Width(16);
        bar.Height(heights[index]);
        bar.CornerRadius(Radius(8));
        bar.Background(Brush(index > 8 ? Color(255, 253, 142) : Color(98, 223, 125)));
        bar.Opacity(0.48 + static_cast<double>(index) * 0.035);
        bar.VerticalAlignment(mux::VerticalAlignment::Bottom);
        bars.Children().Append(bar);
    }
    activity.Children().Append(bars);
    activity.Children().Append(StatLine(L"09:00", L"现在"));
    auto activityCard = Card(L"Token 活动", Color(255, 253, 142), activity);
    controls::Grid::SetColumn(activityCard, 1);
    page.Children().Append(activityCard);

    controls::StackPanel limits;
    limits.Spacing(18);
    limits.Children().Append(ProviderBlock(
        L"Codex weekly", L"6d 20h 后重置", L"46% left", 0.46, Color(98, 223, 125)));
    limits.Children().Append(ProviderBlock(
        L"ChatGPT rolling", L"2h 58m 后重置", L"78% left", 0.78, Color(255, 253, 142)));
    auto limitsCard = Card(L"账户限额", Color(240, 63, 22), limits);
    controls::Grid::SetRow(limitsCard, 1);
    page.Children().Append(limitsCard);

    controls::StackPanel heat;
    heat.Spacing(12);
    heat.Children().Append(StatLine(L"连续使用", L"18 days", Color(255, 253, 142)));
    heat.Children().Append(Heatmap(20, Color(255, 253, 142)));
    heat.Children().Append(Text(L"过去 20 周 · 所有设备", 10.5, Color(143, 139, 140)));
    auto heatCard = Card(L"活动热图", Color(255, 253, 142), heat);
    controls::Grid::SetColumn(heatCard, 1);
    controls::Grid::SetRow(heatCard, 1);
    page.Children().Append(heatCard);

    controls::StackPanel accounts;
    accounts.Spacing(12);
    accounts.Children().Append(BreakdownRow(
        L"Codex · l***8@gmail.com", L"Plus · 当前账户", L"86% left", 0.86, Color(98, 223, 125)));
    accounts.Children().Append(BreakdownRow(
        L"ChatGPT · q***t@icloud.com", L"Plus · 1m ago", L"100%", 1.0, Color(255, 253, 142)));
    accounts.Children().Append(Text(L"近期会话", 12, Color(143, 139, 140), 600));
    accounts.Children().Append(StatLine(L"gpt-5.6-sol · 57 msgs", L"6.62M"));
    accounts.Children().Append(StatLine(L"gpt-5.5 · 18 msgs", L"369K"));
    accounts.Children().Append(StatLine(L"ChatGPT · 12 msgs", L"204K"));
    auto accountsCard = Card(L"账户与会话", Color(240, 63, 22), accounts);
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
    AddRow(page, Pixels(82));
    AddRow(page, Pixels(450));

    auto intro = PageIntro(
        L"工具 · 模型 · 会话 · 设备 · 项目 · 账户",
        L"逐层查看每一枚 token",
        L"点击数据行后可在这里展开缓存命中、输入、输出与费用。",
        Color(98, 223, 125));
    page.Children().Append(intro);

    controls::Grid content;
    content.ColumnSpacing(16);
    AddColumn(content, Star(1.48));
    AddColumn(content, Star());
    controls::Grid::SetRow(content, 1);

    controls::StackPanel rows;
    rows.Spacing(10);
    rows.Children().Append(BreakdownRow(
        L"Codex", L"Cache 71.4% · Output 8.1K", L"3.62B", 0.86, Color(98, 223, 125)));
    rows.Children().Append(BreakdownRow(
        L"ChatGPT", L"Cache 64.8% · Output 5.4K", L"1.79B", 0.43, Color(255, 253, 142)));
    rows.Children().Append(BreakdownRow(
        L"gpt-5.6-sol", L"12 sessions · $18.24", L"1.75B", 0.68, Color(98, 223, 125)));
    rows.Children().Append(BreakdownRow(
        L"Desktop · Windows", L"Synced 1m ago", L"137.6M", 0.22, Color(240, 63, 22)));
    auto listCard = Card(L"使用细分", Color(98, 223, 125), rows);
    content.Children().Append(listCard);

    controls::StackPanel cache;
    cache.Spacing(14);
    cache.Children().Append(Text(L"Codex · all models", 13, Color(247, 247, 245), 600));
    cache.Children().Append(Progress(0.714, Color(98, 223, 125)));
    cache.Children().Append(StatLine(L"Input · cache hit", L"71.4%", Color(98, 223, 125)));
    cache.Children().Append(StatLine(L"Input · cache miss", L"20.3%", Color(255, 253, 142)));
    cache.Children().Append(StatLine(L"Output", L"8.3%", Color(240, 63, 22)));
    cache.Children().Append(StatLine(L"Estimated cost", L"$3,243.77"));
    cache.Children().Append(Text(
        L"静态占位：接入 UsageModels 后，选中任意行即可更新本面板。",
        10.5,
        Color(143, 139, 140)));
    auto cacheCard = Card(L"缓存命中详情", Color(255, 253, 142), cache);
    controls::Grid::SetColumn(cacheCard, 1);
    content.Children().Append(cacheCard);

    page.Children().Append(content);
    return page;
}

controls::Grid DashboardView::BuildTrendsPage()
{
    controls::Grid page;
    page.Height(548);
    page.RowSpacing(16);
    AddRow(page, Pixels(98));
    AddRow(page, Pixels(434));

    controls::Grid metrics;
    metrics.ColumnSpacing(10);
    for (int index = 0; index < 4; ++index)
    {
        AddColumn(metrics, Star());
    }
    auto total = Metric(L"TOTAL TOKENS", L"7.02B", L"ALL DEVICES", Color(98, 223, 125));
    metrics.Children().Append(total);
    auto cost = Metric(L"TOTAL COST", L"$5.9K", L"ESTIMATED", Color(255, 253, 142));
    controls::Grid::SetColumn(cost, 1);
    metrics.Children().Append(cost);
    auto streak = Metric(L"CURRENT STREAK", L"18", L"DAYS", Color(240, 63, 22));
    controls::Grid::SetColumn(streak, 2);
    metrics.Children().Append(streak);
    auto model = Metric(L"TOP MODEL", L"gpt-5.6", L"37.0%", Color(98, 223, 125));
    controls::Grid::SetColumn(model, 3);
    metrics.Children().Append(model);
    page.Children().Append(metrics);

    controls::Grid content;
    content.ColumnSpacing(16);
    AddColumn(content, Star(1.45));
    AddColumn(content, Star());
    controls::Grid::SetRow(content, 1);

    controls::StackPanel chart;
    chart.Spacing(12);
    chart.Children().Append(StatLine(L"30 天 · 按工具堆叠", L"Bars / K-line"));
    controls::StackPanel bars;
    bars.Height(250);
    bars.Orientation(controls::Orientation::Horizontal);
    bars.Spacing(10);
    bars.VerticalAlignment(mux::VerticalAlignment::Bottom);
    for (int index = 0; index < 18; ++index)
    {
        controls::StackPanel bar;
        bar.VerticalAlignment(mux::VerticalAlignment::Bottom);

        controls::Border chatgpt;
        chatgpt.Width(20);
        chatgpt.Height(24 + (index * 17) % 58);
        chatgpt.Background(Brush(Color(255, 253, 142)));
        chatgpt.Opacity(0.78);

        controls::Border codex;
        codex.Width(20);
        codex.Height(48 + (index * 29) % 132);
        codex.Background(Brush(Color(98, 223, 125)));
        codex.Opacity(0.72);
        codex.CornerRadius(index % 2 == 0 ? Radius(5) : Radius(2));

        bar.Children().Append(chatgpt);
        bar.Children().Append(codex);
        bars.Children().Append(bar);
    }
    chart.Children().Append(bars);
    chart.Children().Append(StatLine(L"Codex 61.2%", L"ChatGPT 38.8%"));
    auto chartCard = Card(L"历史趋势", Color(98, 223, 125), chart);
    content.Children().Append(chartCard);

    controls::StackPanel year;
    year.Spacing(14);
    year.Children().Append(StatLine(L"活跃天数", L"111", Color(255, 253, 142)));
    year.Children().Append(Heatmap(16, Color(98, 223, 125)));
    year.Children().Append(StatLine(L"Peak day", L"307M"));
    year.Children().Append(StatLine(L"Active time", L"542h 28m"));
    year.Children().Append(Text(L"年度热图汇总所有 Windows 与 WSL 数据。", 10.5, Color(143, 139, 140)));
    auto yearCard = Card(L"年度活动", Color(255, 253, 142), year);
    controls::Grid::SetColumn(yearCard, 1);
    content.Children().Append(yearCard);

    page.Children().Append(content);
    return page;
}

controls::Grid DashboardView::BuildSettingsPage()
{
    controls::Grid page;
    page.Height(548);
    page.ColumnSpacing(16);
    page.RowSpacing(16);
    AddColumn(page, Star());
    AddColumn(page, Star());
    AddRow(page, Pixels(266));
    AddRow(page, Pixels(266));

    auto modules = Card(
        L"仪表盘模块",
        Color(240, 63, 22),
        SettingsBody(L"显示模块", L"6 enabled", L"拖放顺序", L"CUSTOM"));
    page.Children().Append(modules);

    auto appearance = Card(
        L"外观",
        Color(98, 223, 125),
        SettingsBody(L"主题", L"DARK", L"玻璃透明度", L"72%"));
    controls::Grid::SetColumn(appearance, 1);
    page.Children().Append(appearance);

    auto wsl = Card(
        L"WSL 与同步",
        Color(255, 253, 142),
        SettingsBody(L"自动检测", L"ON", L"合并间隔", L"5 MIN"));
    controls::Grid::SetRow(wsl, 1);
    page.Children().Append(wsl);

    auto surfaces = Card(
        L"托盘与浮动气泡",
        Color(240, 63, 22),
        SettingsBody(L"托盘摘要", L"TOKENS", L"气泡预览", L"COMPACT"));
    controls::Grid::SetColumn(surfaces, 1);
    controls::Grid::SetRow(surfaces, 1);
    page.Children().Append(surfaces);
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
