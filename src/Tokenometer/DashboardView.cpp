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

    bool IsColor(
        winrt::Windows::UI::Color const& value,
        uint8_t red,
        uint8_t green,
        uint8_t blue,
        uint8_t alpha = 255) noexcept
    {
        return value.R == red && value.G == green && value.B == blue && value.A == alpha;
    }

    struct SemanticBrushPalette
    {
        media::SolidColorBrush shell{ Color(18, 16, 17) };
        media::SolidColorBrush card{ Color(38, 36, 37) };
        media::SolidColorBrush inset{ Color(29, 27, 28) };
        media::SolidColorBrush track{ Color(52, 49, 50) };
        media::SolidColorBrush selected{ Color(55, 52, 53) };
        media::SolidColorBrush navigation{ Color(28, 26, 27, 248) };
        media::SolidColorBrush primary{ Color(247, 247, 245) };
        media::SolidColorBrush secondaryStrong{ Color(214, 211, 210) };
        media::SolidColorBrush secondary{ Color(154, 150, 151) };
        media::SolidColorBrush muted{ Color(143, 139, 140) };
        media::SolidColorBrush borderSubtle{ Color(255, 255, 255, 12) };
        media::SolidColorBrush border{ Color(255, 255, 255, 18) };
        media::SolidColorBrush borderStrong{ Color(255, 255, 255, 20) };
        media::SolidColorBrush navigationBorder{ Color(255, 255, 255, 22) };
        media::SolidColorBrush brandOrangeText{ Color(240, 63, 22) };
        media::SolidColorBrush brandGreenText{ Color(98, 223, 125) };
        media::SolidColorBrush brandYellowText{ Color(255, 253, 142) };
        media::SolidColorBrush dangerText{ Color(240, 126, 96) };

        void SetLight(bool light)
        {
            shell.Color(light ? Color(239, 236, 228) : Color(18, 16, 17));
            card.Color(light ? Color(247, 245, 239) : Color(38, 36, 37));
            inset.Color(light ? Color(230, 225, 216) : Color(29, 27, 28));
            track.Color(light ? Color(206, 200, 189) : Color(52, 49, 50));
            selected.Color(light ? Color(216, 210, 200) : Color(55, 52, 53));
            navigation.Color(light ? Color(247, 245, 239, 248) : Color(28, 26, 27, 248));
            primary.Color(light ? Color(26, 24, 25) : Color(247, 247, 245));
            secondaryStrong.Color(light ? Color(73, 68, 69) : Color(214, 211, 210));
            secondary.Color(light ? Color(73, 68, 69) : Color(154, 150, 151));
            muted.Color(light ? Color(105, 99, 101) : Color(143, 139, 140));
            borderSubtle.Color(light ? Color(0, 0, 0, 16) : Color(255, 255, 255, 12));
            border.Color(light ? Color(0, 0, 0, 24) : Color(255, 255, 255, 18));
            borderStrong.Color(light ? Color(0, 0, 0, 30) : Color(255, 255, 255, 20));
            navigationBorder.Color(light ? Color(0, 0, 0, 34) : Color(255, 255, 255, 22));
            brandOrangeText.Color(light ? Color(185, 45, 15) : Color(240, 63, 22));
            brandGreenText.Color(light ? Color(22, 122, 60) : Color(98, 223, 125));
            brandYellowText.Color(light ? Color(118, 105, 0) : Color(255, 253, 142));
            dangerText.Color(light ? Color(162, 47, 23) : Color(240, 126, 96));
        }
    };

    constexpr int WeightedLuminance(int red, int green, int blue) noexcept
    {
        return red * 299 + green * 587 + blue * 114;
    }

    static_assert(WeightedLuminance(247, 245, 239) - WeightedLuminance(185, 45, 15) > 100000);
    static_assert(WeightedLuminance(247, 245, 239) - WeightedLuminance(22, 122, 60) > 100000);
    static_assert(WeightedLuminance(247, 245, 239) - WeightedLuminance(118, 105, 0) > 100000);

    SemanticBrushPalette& SemanticBrushes()
    {
        static SemanticBrushPalette palette;
        return palette;
    }

    media::SolidColorBrush Brush(winrt::Windows::UI::Color const& color)
    {
        auto& palette = SemanticBrushes();
        if (IsColor(color, 18, 16, 17)) return palette.shell;
        if (IsColor(color, 38, 36, 37)) return palette.card;
        if (IsColor(color, 29, 27, 28)) return palette.inset;
        if (IsColor(color, 52, 49, 50)) return palette.track;
        if (IsColor(color, 55, 52, 53)) return palette.selected;
        if (IsColor(color, 28, 26, 27, 248)) return palette.navigation;
        if (IsColor(color, 247, 247, 245)) return palette.primary;
        if (IsColor(color, 214, 211, 210)) return palette.secondaryStrong;
        if (IsColor(color, 154, 150, 151)) return palette.secondary;
        if (IsColor(color, 143, 139, 140)) return palette.muted;
        if (IsColor(color, 255, 255, 255, 12)) return palette.borderSubtle;
        if (IsColor(color, 255, 255, 255, 18)) return palette.border;
        if (IsColor(color, 255, 255, 255, 20)) return palette.borderStrong;
        if (IsColor(color, 255, 255, 255, 22)) return palette.navigationBorder;
        return media::SolidColorBrush{ color };
    }

    media::SolidColorBrush TextBrush(winrt::Windows::UI::Color const& color)
    {
        auto& palette = SemanticBrushes();
        if (IsColor(color, 240, 63, 22)) return palette.brandOrangeText;
        if (IsColor(color, 98, 223, 125)) return palette.brandGreenText;
        if (IsColor(color, 255, 253, 142)) return palette.brandYellowText;
        if (IsColor(color, 240, 126, 96)) return palette.dangerText;
        return Brush(color);
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
        text.Foreground(TextBrush(color));
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

    controls::Border SettingsCard(
        std::wstring_view title,
        std::wstring_view caption,
        winrt::Windows::UI::Color const& accent,
        mux::UIElement const& body)
    {
        controls::StackPanel content;
        content.Spacing(10);

        controls::Grid heading;
        AddColumn(heading, Pixels(12));
        AddColumn(heading, Star());

        shapes::Ellipse dot;
        dot.Width(7);
        dot.Height(7);
        dot.Fill(Brush(accent));
        dot.VerticalAlignment(mux::VerticalAlignment::Top);
        dot.Margin({ 0, 6, 0, 0 });
        heading.Children().Append(dot);

        controls::StackPanel copy;
        copy.Spacing(2);
        copy.Children().Append(Text(title, 13.5, Color(247, 247, 245), 650));
        auto captionText = Text(caption, 9.5, Color(143, 139, 140));
        captionText.TextWrapping(mux::TextWrapping::Wrap);
        captionText.TextTrimming(mux::TextTrimming::None);
        copy.Children().Append(captionText);
        controls::Grid::SetColumn(copy, 1);
        heading.Children().Append(copy);

        content.Children().Append(heading);
        content.Children().Append(body);

        controls::Border card;
        card.Background(Brush(Color(38, 36, 37)));
        card.BorderBrush(Brush(Color(255, 255, 255, 18)));
        card.BorderThickness({ 1 });
        card.CornerRadius(Radius(18));
        card.Padding({ 16, 13, 16, 15 });
        card.Child(content);
        return card;
    }

    controls::Button CompactButton(std::wstring_view label, double minimumWidth = 0)
    {
        controls::Button button;
        button.Height(30);
        if (minimumWidth > 0)
        {
            button.MinWidth(minimumWidth);
        }
        button.Padding({ 10, 0, 10, 0 });
        button.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
        button.Background(Brush(Color(29, 27, 28)));
        button.Foreground(Brush(Color(214, 211, 210)));
        button.BorderBrush(Brush(Color(255, 255, 255, 20)));
        button.BorderThickness({ 1 });
        button.CornerRadius(Radius(10));
        button.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
        button.FontSize(10);
        button.FontWeight({ 600 });
        button.Content(winrt::box_value(winrt::hstring{ label }));
        automation::AutomationProperties::SetName(button, winrt::hstring{ label });
        return button;
    }

    controls::Grid ButtonSetting(
        std::wstring_view label,
        std::wstring_view automationName,
        controls::Button& target)
    {
        controls::Grid row;
        row.MinHeight(31);
        AddColumn(row, Star());
        AddColumn(row, mux::GridLengthHelper::Auto());

        auto copy = Text(label, 10.5, Color(214, 211, 210), 550);
        copy.VerticalAlignment(mux::VerticalAlignment::Center);
        row.Children().Append(copy);

        target = CompactButton(L"关", 42);
        target.Height(28);
        target.VerticalAlignment(mux::VerticalAlignment::Center);
        controls::Grid::SetColumn(target, 1);
        automation::AutomationProperties::SetName(target, winrt::hstring{ automationName });
        row.Children().Append(target);
        return row;
    }

    void SetBooleanButton(controls::Button const& button, bool enabled)
    {
        button.Content(winrt::box_value(winrt::hstring{ enabled ? L"开" : L"关" }));
        button.Background(Brush(enabled ? Color(240, 63, 22) : Color(29, 27, 28)));
        button.Foreground(Brush(enabled ? Color(247, 247, 245) : Color(154, 150, 151)));
        button.BorderBrush(Brush(enabled ? Color(240, 63, 22) : Color(255, 255, 255, 20)));
        automation::AutomationProperties::SetHelpText(button, enabled ? L"当前已开启" : L"当前已关闭");
    }

    std::wstring_view SurfaceToolLabel(tokenometer::SurfaceTool tool)
    {
        return tool == tokenometer::SurfaceTool::ChatGpt ? L"ChatGPT" : L"Codex";
    }

    std::wstring_view OverviewModuleLabel(tokenometer::OverviewModule module)
    {
        switch (module)
        {
        case tokenometer::OverviewModule::TokenSummary:
            return L"Token 汇总";
        case tokenometer::OverviewModule::TokenActivity:
            return L"Token 活动";
        case tokenometer::OverviewModule::CodexLimits:
            return L"Codex 限额";
        case tokenometer::OverviewModule::ActivityHeatmap:
            return L"活动热图";
        case tokenometer::OverviewModule::RecentSessions:
            return L"近期会话与设备";
        }
        return L"总览模块";
    }

    std::wstring_view SurfaceLayoutItemLabel(tokenometer::SurfaceLayoutItemKind kind)
    {
        switch (kind)
        {
        case tokenometer::SurfaceLayoutItemKind::ToolIcon:
            return L"工具图标";
        case tokenometer::SurfaceLayoutItemKind::QuotaBar:
            return L"配额条";
        case tokenometer::SurfaceLayoutItemKind::Percentage:
            return L"百分比";
        case tokenometer::SurfaceLayoutItemKind::ResetTime:
            return L"重置时间";
        case tokenometer::SurfaceLayoutItemKind::Cost:
            return L"费用";
        case tokenometer::SurfaceLayoutItemKind::CustomText:
            return L"自定义文本";
        }
        return L"工具图标";
    }

    std::wstring_view SurfaceQuotaLabel(tokenometer::SurfaceQuotaWindow window)
    {
        switch (window)
        {
        case tokenometer::SurfaceQuotaWindow::Nearest:
            return L"最近到期";
        case tokenometer::SurfaceQuotaWindow::FiveHour:
            return L"5 小时";
        case tokenometer::SurfaceQuotaWindow::Weekly:
            return L"每周";
        }
        return L"最近到期";
    }

    std::wstring_view SurfaceFontLabel(tokenometer::SurfaceFontStyle font)
    {
        switch (font)
        {
        case tokenometer::SurfaceFontStyle::System:
            return L"系统";
        case tokenometer::SurfaceFontStyle::Mono:
            return L"等宽";
        case tokenometer::SurfaceFontStyle::Emphasis:
            return L"强调";
        }
        return L"系统";
    }

    tokenometer::SurfaceLayoutItemKind NextLayoutItemKind(tokenometer::SurfaceLayoutItemKind kind)
    {
        switch (kind)
        {
        case tokenometer::SurfaceLayoutItemKind::ToolIcon:
            return tokenometer::SurfaceLayoutItemKind::QuotaBar;
        case tokenometer::SurfaceLayoutItemKind::QuotaBar:
            return tokenometer::SurfaceLayoutItemKind::Percentage;
        case tokenometer::SurfaceLayoutItemKind::Percentage:
            return tokenometer::SurfaceLayoutItemKind::ResetTime;
        case tokenometer::SurfaceLayoutItemKind::ResetTime:
            return tokenometer::SurfaceLayoutItemKind::Cost;
        case tokenometer::SurfaceLayoutItemKind::Cost:
            return tokenometer::SurfaceLayoutItemKind::CustomText;
        case tokenometer::SurfaceLayoutItemKind::CustomText:
            return tokenometer::SurfaceLayoutItemKind::ToolIcon;
        }
        return tokenometer::SurfaceLayoutItemKind::ToolIcon;
    }

    tokenometer::SurfaceQuotaWindow NextQuotaWindow(tokenometer::SurfaceQuotaWindow window)
    {
        switch (window)
        {
        case tokenometer::SurfaceQuotaWindow::Nearest:
            return tokenometer::SurfaceQuotaWindow::FiveHour;
        case tokenometer::SurfaceQuotaWindow::FiveHour:
            return tokenometer::SurfaceQuotaWindow::Weekly;
        case tokenometer::SurfaceQuotaWindow::Weekly:
            return tokenometer::SurfaceQuotaWindow::Nearest;
        }
        return tokenometer::SurfaceQuotaWindow::Nearest;
    }

    tokenometer::SurfaceFontStyle NextFontStyle(tokenometer::SurfaceFontStyle font)
    {
        switch (font)
        {
        case tokenometer::SurfaceFontStyle::System:
            return tokenometer::SurfaceFontStyle::Mono;
        case tokenometer::SurfaceFontStyle::Mono:
            return tokenometer::SurfaceFontStyle::Emphasis;
        case tokenometer::SurfaceFontStyle::Emphasis:
            return tokenometer::SurfaceFontStyle::System;
        }
        return tokenometer::SurfaceFontStyle::System;
    }

    template<size_t Size>
    std::wstring NextTextPreset(
        std::wstring_view current,
        std::array<std::wstring_view, Size> const& presets)
    {
        for (size_t index = 0; index < presets.size(); ++index)
        {
            if (current == presets[index])
            {
                return std::wstring{ presets[(index + 1) % presets.size()] };
            }
        }
        return std::wstring{ presets.front() };
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

        auto live = Text(L"LIVE", 11, Color(17, 15, 16), 700, true);
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
    UpdateSurfacePreferences({});
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
        if (data.warning.empty())
        {
            SetStatus(L"数据已同步", FormatAge(data.lastSync), true);
        }
        else
        {
            SetStatus(L"Windows 数据已同步", data.warning, true);
        }
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
    m_recentSessionCount = sessionCount;
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

    if (data.chatGptTotals.estimatedTokens > 0)
    {
        m_chatGptOverviewValue.Text(winrt::hstring{
            L"≈ " + FormatCompact(data.chatGptTotals.estimatedTokens) + L" tokens" });
        m_chatGptOverviewDetail.Text(winrt::hstring{
            FormatInteger(data.chatGptTotals.estimatedSessions) +
            L" 个会话 · 官方导出估算 · 缓存/费用 N/A" });
    }
    else
    {
        m_chatGptOverviewValue.Text(L"等待官方导出导入");
        m_chatGptOverviewDetail.Text(L"官方导出仅支持离线估算");
    }

    if (m_devicePanel)
    {
        m_devicePanel.Children().Clear();
        auto const visibleDevices = std::min<size_t>(data.devices.size(), 2);
        m_deviceCount = visibleDevices;
        for (size_t index = 0; index < visibleDevices; ++index)
        {
            auto const& device = data.devices[index];
            controls::Grid row;
            AddColumn(row, Star());
            AddColumn(row, mux::GridLengthHelper::Auto());
            controls::StackPanel copy;
            copy.Spacing(1);
            auto name = device.summary.displayName.empty()
                ? device.summary.id
                : device.summary.displayName;
            auto const prefix = device.summary.kind == DeviceKind::Wsl ? L"WSL · " : L"Windows · ";
            copy.Children().Append(Text(prefix + name, 10.5, Color(247, 247, 245), 600));
            auto detail = device.statusText;
            if (detail.empty())
            {
                detail = device.summary.lastSeen > 0 ? FormatAge(device.summary.lastSeen) : L"尚未同步";
            }
            copy.Children().Append(Text(detail, 8.5, Color(143, 139, 140)));
            row.Children().Append(copy);
            auto value = Text(
                FormatCompact(device.summary.counts.DisplayTotal()),
                10.5,
                device.state == DeviceSyncState::Warning ? Color(255, 253, 142) : Color(98, 223, 125),
                600,
                true);
            value.VerticalAlignment(mux::VerticalAlignment::Center);
            controls::Grid::SetColumn(value, 1);
            row.Children().Append(value);
            m_devicePanel.Children().Append(row);
        }
        if (visibleDevices == 0)
        {
            m_devicePanel.Children().Append(Text(
                L"尚未发现 Windows / WSL 设备",
                9,
                Color(143, 139, 140)));
        }
    }
    UpdateDailyVisuals(data.daily);
    ApplyOverviewLayout();
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
    m_detailsScope = data.scope;
    m_detailsDimension = data.dimension;
    UpdateDetailsScopeButtons();
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
    auto appendListControls = [this](
        controls::StackPanel const& panel,
        DetailsList list,
        bool hasMore,
        bool expanded)
    {
        if (!hasMore && !expanded) return;
        controls::StackPanel actions;
        actions.Orientation(controls::Orientation::Horizontal);
        actions.Spacing(6);
        if (hasMore)
        {
            auto more = CompactButton(L"显示更多", 72);
            more.Click([this, list](auto const&, auto const&)
            {
                if (m_detailsCallbacks.onListExpansionChanged)
                {
                    m_detailsCallbacks.onListExpansionChanged(list, true);
                }
            });
            actions.Children().Append(more);
        }
        if (expanded)
        {
            auto collapse = CompactButton(L"收起", 52);
            collapse.Click([this, list](auto const&, auto const&)
            {
                if (m_detailsCallbacks.onListExpansionChanged)
                {
                    m_detailsCallbacks.onListExpansionChanged(list, false);
                }
            });
            actions.Children().Append(collapse);
        }
        panel.Children().Append(actions);
    };

    if (!data.error.empty())
    {
        appendState(m_breakdownList, L"明细加载失败", data.error);
    }
    else if (data.loading && data.rows.empty() && data.unavailableReason.empty())
    {
        appendState(m_breakdownList, L"正在加载明细", L"读取完成后会自动刷新当前维度。");
    }
    else if (!data.unavailableReason.empty())
    {
        appendState(m_breakdownList, L"官方导出不提供该维度", data.unavailableReason);
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

        auto const visibleRows = data.rows.size();
        for (size_t index = 0; index < visibleRows; ++index)
        {
            auto const& row = data.rows[index];
            auto const selected = row.key == data.selectedKey;
            auto const input = std::max<int64_t>(row.counts.input, 0);
            auto const cached = std::clamp<int64_t>(row.counts.cachedInput, 0, input);
            auto const hitRate = input > 0
                ? static_cast<double>(cached) * 100.0 / static_cast<double>(input)
                : 0.0;
            auto const estimated = row.measurement == MeasurementKind::Estimated;
            auto const rowName = row.displayName.empty() ? row.key : row.displayName;

            controls::StackPanel rowContent;
            rowContent.Spacing(6);
            controls::Grid top;
            AddColumn(top, Star());
            AddColumn(top, mux::GridLengthHelper::Auto());
            top.Children().Append(Text(rowName.empty() ? L"未命名" : rowName, 11.5, Color(247, 247, 245), 600));
            auto total = Text(
                (estimated ? L"≈ " : L"") + FormatCompact(row.counts.DisplayTotal()),
                11.5,
                Color(247, 247, 245),
                600,
                true);
            controls::Grid::SetColumn(total, 1);
            top.Children().Append(total);
            rowContent.Children().Append(top);

            auto summary = estimated
                ? L"≈ " + FormatCompact(input) + L" input · ≈ " +
                    FormatCompact(row.counts.output) + L" output · cache N/A"
                : FormatPercent(hitRate) + L" hit · " +
                    FormatCompact(row.counts.output) + L" output";
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
            automation::AutomationProperties::SetName(button, winrt::hstring{ rowName });
            auto key = row.key;
            auto session = row.session;
            button.Click([this, key = std::move(key), session = std::move(session)](auto const&, auto const&)
            {
                if (m_detailsCallbacks.onBreakdownSelected)
                {
                    m_detailsCallbacks.onBreakdownSelected(key, session);
                }
            });
            m_breakdownList.Children().Append(button);
        }
        appendListControls(
            m_breakdownList,
            DetailsList::Breakdown,
            data.breakdownHasMore,
            data.breakdownExpanded);
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
        auto const estimated = selectedRow->measurement == MeasurementKind::Estimated;
        m_detailsSelectedTitle.Text(winrt::hstring{
            selectedRow->displayName.empty() ? selectedRow->key : selectedRow->displayName });
        m_detailsInputText.Text(winrt::hstring{
            (estimated ? L"≈ " : L"") + FormatInteger(input) });
        m_detailsCacheHitTokensText.Text(winrt::hstring{
            estimated ? L"N/A" : FormatInteger(cached) });
        m_detailsCacheMissTokensText.Text(winrt::hstring{
            estimated ? L"N/A" : FormatInteger(miss) });
        m_detailsOutputText.Text(winrt::hstring{
            (estimated ? L"≈ " : L"") + FormatInteger(output) });
        m_detailsHitRateText.Text(winrt::hstring{
            estimated ? L"N/A" : FormatPercent(ratio * 100.0) });
        SetProgress(m_detailsCacheProgressFill, m_detailsCacheProgressRest, estimated ? 0.0 : ratio);
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
        auto const visibleSessions = data.recentSessions.size();
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
            if (session.measurement == MeasurementKind::Estimated)
            {
                detail += L" · 官方导出估算";
            }
            copy.Children().Append(Text(detail, 9, Color(143, 139, 140)));
            content.Children().Append(copy);
            auto value = Text(
                (session.measurement == MeasurementKind::Estimated ? L"≈ " : L"") +
                    FormatCompact(session.counts.DisplayTotal()),
                10.5,
                Color(247, 247, 245),
                600,
                true);
            value.VerticalAlignment(mux::VerticalAlignment::Center);
            controls::Grid::SetColumn(value, 1);
            content.Children().Append(value);

            auto const selected = session.id == data.selectedSession.sessionId &&
                session.accountId == data.selectedSession.accountId &&
                session.sourceKind == data.selectedSession.sourceKind;
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
            SessionRef sessionRef{ session.sourceKind, session.accountId, session.id };
            button.Click([this, sessionRef = std::move(sessionRef)](auto const&, auto const&)
            {
                if (m_detailsCallbacks.onSessionSelected)
                {
                    m_detailsCallbacks.onSessionSelected(sessionRef);
                }
            });
            m_detailsSessionsPanel.Children().Append(button);
        }

        if (visibleSessions == 0)
        {
            appendState(m_detailsSessionsPanel, L"暂无会话", L"采集或导入完成后会在这里显示。");
        }
        appendListControls(
            m_detailsSessionsPanel,
            DetailsList::Sessions,
            data.sessionsHasMore,
            data.sessionsExpanded);

        if (data.selectedSession.Valid())
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
                auto const estimated = turn.measurement == MeasurementKind::Estimated;
                std::wstring promptDetail;
                if (estimated)
                {
                    promptDetail = L"≈ 输入 " + FormatCompact(turn.counts.input) +
                        L" · ≈ 输出 " + FormatCompact(turn.counts.output) +
                        L" · 缓存/工具 N/A";
                }
                else
                {
                    promptDetail = turn.tools.empty() ? L"未使用工具" : turn.tools;
                    promptDetail += L" · 输入 " + FormatCompact(turn.counts.input) +
                        L" · 缓存 " + FormatCompact(turn.counts.cachedInput) +
                        L" · 输出 " + FormatCompact(turn.counts.output);
                }
                auto promptDetailText = Text(promptDetail, 9, Color(143, 139, 140));
                promptDetailText.TextWrapping(mux::TextWrapping::Wrap);
                copy.Children().Append(promptDetailText);
                content.Children().Append(copy);
                auto value = Text(
                    (estimated ? L"≈ " : L"") + FormatCompact(turn.counts.DisplayTotal()),
                    10.5,
                    Color(255, 253, 142),
                    600,
                    true);
                value.VerticalAlignment(mux::VerticalAlignment::Center);
                controls::Grid::SetColumn(value, 1);
                content.Children().Append(value);
                m_detailsSessionsPanel.Children().Append(SoftPanel(content));
            }
            appendListControls(
                m_detailsSessionsPanel,
                DetailsList::Turns,
                data.turnsHasMore,
                data.turnsExpanded);

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
            appendListControls(
                m_detailsSessionsPanel,
                DetailsList::Tools,
                data.toolsHasMore,
                data.toolsExpanded);

            if (!data.selectedToolCallLocator.empty())
            {
                controls::StackPanel detail;
                detail.Spacing(6);
                detail.Children().Append(Text(L"工具输入 / 输出", 10.5, Color(255, 253, 142), 600));
                detail.Children().Append(Text(
                    L"从本地转录按需读取 · 尽力脱敏 · 不写入数据库；屏幕共享时仍请谨慎",
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
    double leftExtra = static_cast<double>(data.rows.size() > 3 ? data.rows.size() - 3 : 0) * 82.0;
    double rightExtra = static_cast<double>(
        data.recentSessions.size() > 3 ? data.recentSessions.size() - 3 : 0) * 54.0;
    if (data.breakdownExpanded) leftExtra += 38.0;
    if (data.sessionsExpanded) rightExtra += 38.0;
    if (data.selectedSession.Valid())
    {
        rightExtra += 54.0;
        rightExtra += static_cast<double>(data.selectedTurns.size()) * 58.0;
        rightExtra += static_cast<double>(data.toolCalls.size()) * 52.0;
        if (data.turnsExpanded) rightExtra += 38.0;
        if (data.toolsExpanded) rightExtra += 38.0;
        if (!data.selectedToolCallLocator.empty())
        {
            auto const approximateLines = (data.selectedToolDetails.size() + 45) / 46;
            rightExtra += 82.0 + static_cast<double>(approximateLines) * 17.0;
        }
    }
    expandedHeight += std::max(leftExtra, rightExtra);
    m_detailsPage.Height(expandedHeight);
    m_detailsExpanded = data.selectedSession.Valid() || data.breakdownExpanded ||
        data.sessionsExpanded || data.turnsExpanded || data.toolsExpanded ||
        !data.selectedToolCallLocator.empty();
    UpdateScrollState();
}

void DashboardView::UpdateDetailsScopeButtons()
{
    auto const selectedIndex = static_cast<size_t>(m_detailsScope);
    for (size_t index = 0; index < m_detailsScopeButtons.size(); ++index)
    {
        auto const selected = index == selectedIndex;
        auto const& button = m_detailsScopeButtons[index];
        button.Background(Brush(selected ? Color(55, 52, 53) : Color(0, 0, 0, 0)));
        button.Foreground(Brush(selected ? Color(247, 247, 245) : Color(143, 139, 140)));
        button.BorderBrush(Brush(selected ? Color(240, 63, 22) : Color(255, 255, 255, 12)));
    }
}

void DashboardView::UpdateDetailsDimensionButtons()
{
    auto const selectedIndex = static_cast<size_t>(m_detailsDimension);
    for (size_t index = 0; index < m_detailsDimensionButtons.size(); ++index)
    {
        auto const selected = index == selectedIndex;
        auto const& button = m_detailsDimensionButtons[index];
        auto const available = m_detailsScope == UsageScope::CodexExact ||
            (index != static_cast<size_t>(DetailsDimension::Device) &&
             index != static_cast<size_t>(DetailsDimension::Project));
        button.IsEnabled(available);
        button.Opacity(available ? 1.0 : 0.38);
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
    m_trendScope = data.scope;
    m_trendGroup = data.group;
    m_trendChart = data.chart;
    m_trendRange = data.range;
    UpdateTrendScopeButtons();
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
    auto const sourceLabel = data.scope == UsageScope::ChatGptEstimated
        ? L"ChatGPT 官方导出估算"
        : L"Codex 精确";
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

    {
        std::wstring caption = m_trendChartCaption.Text().c_str();
        if (!caption.empty())
        {
            caption += L" · ";
        }
        caption += sourceLabel;
        m_trendChartCaption.Text(winrt::hstring{ caption });
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
        if (data.scope == UsageScope::ChatGptEstimated)
        {
            caption += L" · 估算";
        }
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
                (data.scope == UsageScope::ChatGptEstimated ? L"≈ " : L"") +
                    FormatCompact(data.series[index].total) + L" · " +
                    FormatPercent(data.series[index].percent),
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

void DashboardView::UpdateTrendScopeButtons()
{
    auto const selectedIndex = static_cast<size_t>(m_trendScope);
    for (size_t index = 0; index < m_trendScopeButtons.size(); ++index)
    {
        auto const selected = index == selectedIndex;
        auto const& button = m_trendScopeButtons[index];
        button.Background(Brush(selected ? Color(55, 52, 53) : Color(0, 0, 0, 0)));
        button.Foreground(Brush(selected ? Color(247, 247, 245) : Color(143, 139, 140)));
        button.BorderBrush(Brush(selected ? Color(240, 63, 22) : Color(255, 255, 255, 12)));
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
    UpdateScrollState();
}

void DashboardView::SetSurfacePreferencesCallbacks(SurfacePreferencesCallbacks callbacks)
{
    m_surfacePreferencesCallbacks = std::move(callbacks);
}

void DashboardView::ApplyOverviewLayout()
{
    if (!m_overviewPage || m_overviewCards.size() != 5)
    {
        return;
    }

    std::vector<OverviewModule> visible;
    visible.reserve(5);
    for (auto const& preference : m_surfacePreferences.overviewModules)
    {
        auto const index = static_cast<size_t>(preference.module);
        if (index < m_overviewCards.size() && preference.visible)
        {
            visible.push_back(preference.module);
        }
    }
    if (visible.empty())
    {
        return;
    }

    for (auto const& card : m_overviewCards)
    {
        card.Visibility(mux::Visibility::Collapsed);
        controls::Grid::SetRow(card, 0);
        controls::Grid::SetColumn(card, 0);
        controls::Grid::SetRowSpan(card, 1);
        controls::Grid::SetColumnSpan(card, 1);
    }

    auto const setColumns = [this](double first, double second, double third, double spacing)
    {
        m_overviewPage.ColumnDefinitions().GetAt(0).Width(first > 0 ? Star(first) : Pixels(0));
        m_overviewPage.ColumnDefinitions().GetAt(1).Width(second > 0 ? Star(second) : Pixels(0));
        m_overviewPage.ColumnDefinitions().GetAt(2).Width(third > 0 ? Star(third) : Pixels(0));
        m_overviewPage.ColumnSpacing(spacing);
    };
    auto const place = [this](OverviewModule module, int column, int row, int rowSpan)
    {
        auto const index = static_cast<size_t>(module);
        auto const& card = m_overviewCards[index];
        card.Visibility(mux::Visibility::Visible);
        controls::Grid::SetColumn(card, column);
        controls::Grid::SetRow(card, row);
        controls::Grid::SetRowSpan(card, rowSpan);
    };

    bool recentIsTall = false;
    if (visible.size() <= 3)
    {
        setColumns(1, visible.size() >= 2 ? 1 : 0, visible.size() >= 3 ? 1 : 0,
            visible.size() > 1 ? 16 : 0);
        for (size_t index = 0; index < visible.size(); ++index)
        {
            place(visible[index], static_cast<int>(index), 0, 2);
        }
        recentIsTall = std::find(visible.begin(), visible.end(), OverviewModule::RecentSessions)
            != visible.end();
    }
    else if (visible.size() == 4)
    {
        setColumns(1, 1, 0, 16);
        for (size_t index = 0; index < visible.size(); ++index)
        {
            place(visible[index], static_cast<int>(index % 2), static_cast<int>(index / 2), 1);
        }
    }
    else
    {
        setColumns(0.92, 1.42, 1.02, 16);
        for (size_t index = 0; index < 4; ++index)
        {
            place(visible[index], static_cast<int>(index % 2), static_cast<int>(index / 2), 1);
        }
        place(visible[4], 2, 0, 2);
        recentIsTall = visible[4] == OverviewModule::RecentSessions;
    }

    auto const sessionLimit = recentIsTall ? size_t{ 3 } : size_t{ 1 };
    for (size_t index = 0; index < m_sessionRows.size(); ++index)
    {
        m_sessionRows[index].Visibility(
            index < m_recentSessionCount && index < sessionLimit
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
    }
    if (m_recentEmptyState)
    {
        m_recentEmptyState.Visibility(
            m_recentSessionCount == 0 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    auto const deviceVisibility = recentIsTall ? mux::Visibility::Visible : mux::Visibility::Collapsed;
    if (m_overviewDevicesLabel) m_overviewDevicesLabel.Visibility(deviceVisibility);
    if (m_devicePanel) m_devicePanel.Visibility(deviceVisibility);
}

void DashboardView::RebuildOverviewEditor()
{
    if (!m_overviewEditor)
    {
        return;
    }

    m_overviewEditor.Children().Clear();
    auto heading = Text(L"总览模块", 13, Color(247, 247, 245), 650);
    m_overviewEditor.Children().Append(heading);

    auto hint = Text(L"选择显示内容，并用箭头调整顺序。", 9.5, Color(143, 139, 140));
    hint.TextWrapping(mux::TextWrapping::Wrap);
    hint.TextTrimming(mux::TextTrimming::None);
    m_overviewEditor.Children().Append(hint);

    auto const visibleCount = static_cast<size_t>(std::count_if(
        m_surfacePreferences.overviewModules.begin(),
        m_surfacePreferences.overviewModules.end(),
        [](auto const& item) { return item.visible; }));
    for (size_t index = 0; index < m_surfacePreferences.overviewModules.size(); ++index)
    {
        auto const preference = m_surfacePreferences.overviewModules[index];
        controls::Grid row;
        row.MinWidth(338);
        row.ColumnSpacing(7);
        AddColumn(row, Star());
        AddColumn(row, mux::GridLengthHelper::Auto());
        AddColumn(row, mux::GridLengthHelper::Auto());
        AddColumn(row, mux::GridLengthHelper::Auto());

        auto label = Text(OverviewModuleLabel(preference.module), 10.5, Color(247, 247, 245), 600);
        label.VerticalAlignment(mux::VerticalAlignment::Center);
        row.Children().Append(label);

        auto visibility = CompactButton(preference.visible ? L"显示" : L"隐藏", 52);
        visibility.IsEnabled(!preference.visible || visibleCount > 1);
        visibility.Background(Brush(preference.visible ? Color(240, 63, 22) : Color(29, 27, 28)));
        visibility.BorderBrush(Brush(preference.visible ? Color(240, 63, 22) : Color(255, 255, 255, 20)));
        auto const moduleName = std::wstring{ OverviewModuleLabel(preference.module) };
        automation::AutomationProperties::SetName(
            visibility,
            winrt::hstring{ (preference.visible ? L"隐藏 " : L"显示 ") + moduleName });
        automation::AutomationProperties::SetHelpText(
            visibility,
            preference.visible && visibleCount == 1
                ? L"至少保留一个总览模块"
                : L"切换此模块在总览中的显示状态");
        visibility.Click([this, index](auto const&, auto const&)
        {
            if (index >= m_surfacePreferences.overviewModules.size()) return;
            auto& item = m_surfacePreferences.overviewModules[index];
            if (item.visible && std::count_if(
                m_surfacePreferences.overviewModules.begin(),
                m_surfacePreferences.overviewModules.end(),
                [](auto const& candidate) { return candidate.visible; }) == 1)
            {
                return;
            }
            item.visible = !item.visible;
            RebuildOverviewEditor();
            ApplyOverviewLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(visibility, 1);
        row.Children().Append(visibility);

        auto up = CompactButton(L"↑", 30);
        up.IsEnabled(index > 0);
        automation::AutomationProperties::SetName(up, winrt::hstring{ L"上移 " + moduleName });
        automation::AutomationProperties::SetHelpText(up, L"将模块向前移动一位");
        up.Click([this, index](auto const&, auto const&)
        {
            if (index == 0 || index >= m_surfacePreferences.overviewModules.size()) return;
            std::swap(
                m_surfacePreferences.overviewModules[index - 1],
                m_surfacePreferences.overviewModules[index]);
            RebuildOverviewEditor();
            ApplyOverviewLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(up, 2);
        row.Children().Append(up);

        auto down = CompactButton(L"↓", 30);
        down.IsEnabled(index + 1 < m_surfacePreferences.overviewModules.size());
        automation::AutomationProperties::SetName(down, winrt::hstring{ L"下移 " + moduleName });
        automation::AutomationProperties::SetHelpText(down, L"将模块向后移动一位");
        down.Click([this, index](auto const&, auto const&)
        {
            if (index + 1 >= m_surfacePreferences.overviewModules.size()) return;
            std::swap(
                m_surfacePreferences.overviewModules[index],
                m_surfacePreferences.overviewModules[index + 1]);
            RebuildOverviewEditor();
            ApplyOverviewLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(down, 3);
        row.Children().Append(down);
        m_overviewEditor.Children().Append(SoftPanel(row));
    }
}

void DashboardView::ApplySurfaceTheme(SurfaceTheme theme)
{
    m_surfaceTheme = theme;
    m_root.RequestedTheme(
        theme == SurfaceTheme::Light
            ? mux::ElementTheme::Light
            : theme == SurfaceTheme::Dark
                ? mux::ElementTheme::Dark
                : mux::ElementTheme::Default);
    bool const light = theme == SurfaceTheme::Light ||
        (theme == SurfaceTheme::System && m_root.ActualTheme() == mux::ElementTheme::Light);
    SemanticBrushes().SetLight(light);
}

void DashboardView::UpdateSurfacePreferences(SurfacePreferencesViewData const& data)
{
    m_updatingSurfacePreferences = true;
    auto const previousOverviewModules = m_surfacePreferences.overviewModules;
    m_surfacePreferences = data;
    NormalizeSurfacePreferences();
    if (m_surfacePreferences.overviewModules != previousOverviewModules)
    {
        RebuildOverviewEditor();
        ApplyOverviewLayout();
    }
    RebuildSurfaceLayoutEditor();
    RebuildSurfaceToolEditor();
    UpdateSurfacePreferencesLayout();
    m_updatingSurfacePreferences = false;
}

void DashboardView::NormalizeSurfacePreferences()
{
    switch (m_surfacePreferences.theme)
    {
    case SurfaceTheme::System:
    case SurfaceTheme::Dark:
    case SurfaceTheme::Light:
        break;
    default:
        m_surfacePreferences.theme = SurfaceTheme::System;
        break;
    }

    constexpr std::array<int, 4> opacityOptions{ 25, 50, 75, 90 };
    if (std::find(opacityOptions.begin(), opacityOptions.end(),
                  m_surfacePreferences.glassOpacityPercent) == opacityOptions.end())
    {
        m_surfacePreferences.glassOpacityPercent = 75;
    }

    switch (m_surfacePreferences.layoutPreset)
    {
    case SurfaceLayoutPreset::LiveUsage:
    case SurfaceLayoutPreset::ProviderLimits:
    case SurfaceLayoutPreset::CostFocus:
    case SurfaceLayoutPreset::Custom:
        break;
    default:
        m_surfacePreferences.layoutPreset = SurfaceLayoutPreset::LiveUsage;
        break;
    }

    if (m_surfacePreferences.customLayout.empty())
    {
        SurfaceLayoutItem icon;
        icon.kind = SurfaceLayoutItemKind::ToolIcon;
        icon.tool = SurfaceTool::Codex;
        m_surfacePreferences.customLayout.push_back(icon);

        SurfaceLayoutItem quota;
        quota.kind = SurfaceLayoutItemKind::Percentage;
        quota.tool = SurfaceTool::Codex;
        m_surfacePreferences.customLayout.push_back(quota);

        SurfaceLayoutItem cost;
        cost.kind = SurfaceLayoutItemKind::Cost;
        cost.tool = SurfaceTool::ChatGpt;
        m_surfacePreferences.customLayout.push_back(cost);
    }
    if (m_surfacePreferences.customLayout.size() > 6)
    {
        m_surfacePreferences.customLayout.resize(6);
    }
    for (auto& item : m_surfacePreferences.customLayout)
    {
        if (item.accountLabel.empty())
        {
            item.accountLabel = L"当前帐户";
        }
        if (item.accountLabel.size() > 40)
        {
            item.accountLabel.resize(40);
        }
        if (item.customText.size() > 48)
        {
            item.customText.resize(48);
        }

        switch (item.kind)
        {
        case SurfaceLayoutItemKind::ToolIcon:
        case SurfaceLayoutItemKind::QuotaBar:
        case SurfaceLayoutItemKind::Percentage:
        case SurfaceLayoutItemKind::ResetTime:
        case SurfaceLayoutItemKind::Cost:
        case SurfaceLayoutItemKind::CustomText:
            break;
        default:
            item.kind = SurfaceLayoutItemKind::ToolIcon;
            break;
        }
        if (item.tool != SurfaceTool::Codex && item.tool != SurfaceTool::ChatGpt)
        {
            item.tool = SurfaceTool::Codex;
        }
        if (item.quotaWindow != SurfaceQuotaWindow::Nearest
            && item.quotaWindow != SurfaceQuotaWindow::FiveHour
            && item.quotaWindow != SurfaceQuotaWindow::Weekly)
        {
            item.quotaWindow = SurfaceQuotaWindow::Nearest;
        }
        if (item.font != SurfaceFontStyle::System
            && item.font != SurfaceFontStyle::Mono
            && item.font != SurfaceFontStyle::Emphasis)
        {
            item.font = SurfaceFontStyle::System;
        }
    }

    std::vector<SurfaceToolPreference> tools;
    tools.reserve(2);
    bool hasCodex = false;
    bool hasChatGpt = false;
    for (auto const& candidate : m_surfacePreferences.tools)
    {
        auto& seen = candidate.tool == SurfaceTool::ChatGpt ? hasChatGpt : hasCodex;
        if ((candidate.tool == SurfaceTool::Codex || candidate.tool == SurfaceTool::ChatGpt) && !seen)
        {
            tools.push_back(candidate);
            seen = true;
        }
    }
    if (!hasCodex)
    {
        tools.push_back({ SurfaceTool::Codex, true, true });
    }
    if (!hasChatGpt)
    {
        tools.push_back({ SurfaceTool::ChatGpt, true, false });
    }
    m_surfacePreferences.tools = std::move(tools);

    std::array<bool, 5> seenOverview{};
    std::vector<OverviewModulePreference> overviewModules;
    overviewModules.reserve(5);
    for (auto const& candidate : m_surfacePreferences.overviewModules)
    {
        auto const value = static_cast<size_t>(candidate.module);
        if (value < seenOverview.size() && !seenOverview[value])
        {
            overviewModules.push_back(candidate);
            seenOverview[value] = true;
        }
    }
    for (size_t value = 0; value < seenOverview.size(); ++value)
    {
        if (!seenOverview[value])
        {
            overviewModules.push_back({ static_cast<OverviewModule>(value), true });
        }
    }
    if (std::none_of(overviewModules.begin(), overviewModules.end(),
        [](auto const& item) { return item.visible; }))
    {
        overviewModules.front().visible = true;
    }
    m_surfacePreferences.overviewModules = std::move(overviewModules);
}

void DashboardView::UpdateSurfacePreferencesLayout()
{
    auto const wasUpdating = m_updatingSurfacePreferences;
    m_updatingSurfacePreferences = true;

    SetBooleanButton(m_launchToTrayToggle, m_surfacePreferences.launchToTray);
    SetBooleanButton(m_closeToTrayToggle, m_surfacePreferences.closeToTray);
    SetBooleanButton(m_blurToggle, m_surfacePreferences.blurEnabled);
    SetBooleanButton(m_transparentWindowToggle, m_surfacePreferences.transparentWindow);
    SetBooleanButton(m_providerColorsToggle, m_surfacePreferences.providerColors);
    SetBooleanButton(m_bubbleAlwaysOnTopToggle, m_surfacePreferences.bubbleAlwaysOnTop);
    SetBooleanButton(m_hoverPreviewToggle, m_surfacePreferences.hoverPreview);

    auto styleButtons = [](std::vector<controls::Button> const& buttons, size_t selected)
    {
        for (size_t index = 0; index < buttons.size(); ++index)
        {
            auto const active = index == selected;
            buttons[index].Background(Brush(active ? Color(240, 63, 22) : Color(29, 27, 28)));
            buttons[index].Foreground(Brush(active ? Color(247, 247, 245) : Color(154, 150, 151)));
            buttons[index].BorderBrush(Brush(active ? Color(240, 63, 22) : Color(255, 255, 255, 20)));
        }
    };
    styleButtons(m_themeButtons, static_cast<size_t>(m_surfacePreferences.theme));

    constexpr std::array<int, 4> opacityOptions{ 25, 50, 75, 90 };
    auto const opacity = std::find(
        opacityOptions.begin(), opacityOptions.end(), m_surfacePreferences.glassOpacityPercent);
    styleButtons(m_opacityButtons, static_cast<size_t>(std::distance(opacityOptions.begin(), opacity)));
    styleButtons(m_layoutPresetButtons, static_cast<size_t>(m_surfacePreferences.layoutPreset));

    m_surfaceLayoutEditorPanel.Visibility(
        m_surfacePreferences.layoutEditorExpanded ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_surfaceToolEditorPanel.Visibility(
        m_surfacePreferences.toolManagerExpanded ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_surfaceLayoutExpandButton.Content(winrt::box_value(winrt::hstring{
        m_surfacePreferences.layoutEditorExpanded ? L"收起自定义布局" : L"自定义…" }));
    m_surfaceToolExpandButton.Content(winrt::box_value(winrt::hstring{
        m_surfacePreferences.toolManagerExpanded ? L"完成管理" : L"管理工具" }));
    m_surfaceLayoutAddButton.IsEnabled(m_surfacePreferences.customLayout.size() < 6);

    std::wstring preview = m_surfacePreferences.livePreview;
    if (preview.empty())
    {
        preview = L"等待本地数据";
    }
    m_surfacePreviewText.Text(winrt::hstring{ preview });

    std::wstring toolSummary;
    for (auto const& tool : m_surfacePreferences.tools)
    {
        if (!toolSummary.empty())
        {
            toolSummary += L"  ·  ";
        }
        toolSummary += SurfaceToolLabel(tool.tool);
        if (!tool.visible)
        {
            toolSummary += L"（隐藏）";
        }
        else if (tool.pinned)
        {
            toolSummary += L"（置顶）";
        }
    }
    m_surfaceToolSummaryText.Text(winrt::hstring{ toolSummary });
    UpdateScrollState();
    m_updatingSurfacePreferences = wasUpdating;
}

void DashboardView::UpdateScrollState()
{
    if (!m_scroller) return;
    bool const expanded =
        (m_currentPage == DashboardPage::Details && m_detailsExpanded) ||
        (m_currentPage == DashboardPage::Settings &&
          (m_surfacePreferences.layoutEditorExpanded ||
           m_surfacePreferences.toolManagerExpanded ||
           m_chatGptDetailsExpanded));
    bool const constrainedHeight =
        m_scroller.ViewportHeight() > 0 && m_scroller.ViewportHeight() < 548.0;
    bool const scrollingRequired = expanded || constrainedHeight;
    if (!scrollingRequired)
    {
        m_scroller.ScrollToVerticalOffset(0.0);
    }
    m_scroller.VerticalScrollMode(
        scrollingRequired ? controls::ScrollMode::Enabled : controls::ScrollMode::Disabled);
    m_scroller.VerticalScrollBarVisibility(
        scrollingRequired ? controls::ScrollBarVisibility::Auto : controls::ScrollBarVisibility::Hidden);
}

void DashboardView::NotifySurfacePreferencesChanged()
{
    if (!m_updatingSurfacePreferences && m_surfacePreferencesCallbacks.onChanged)
    {
        m_surfacePreferencesCallbacks.onChanged(m_surfacePreferences);
    }
}

void DashboardView::RebuildSurfaceLayoutEditor()
{
    if (!m_surfaceLayoutEditor)
    {
        return;
    }

    m_surfaceLayoutEditor.Children().Clear();
    for (size_t index = 0; index < m_surfacePreferences.customLayout.size(); ++index)
    {
        auto const& item = m_surfacePreferences.customLayout[index];
        controls::StackPanel content;
        content.Spacing(7);

        controls::Grid heading;
        AddColumn(heading, Star());
        AddColumn(heading, mux::GridLengthHelper::Auto());
        heading.Children().Append(Text(
            L"布局项 " + std::to_wstring(index + 1), 10, Color(247, 247, 245), 650, true));
        auto position = Text(
            std::to_wstring(index + 1) + L" / "
                + std::to_wstring(m_surfacePreferences.customLayout.size()),
            9,
            Color(143, 139, 140),
            500,
            true);
        controls::Grid::SetColumn(position, 1);
        heading.Children().Append(position);
        content.Children().Append(heading);

        controls::Grid selectors;
        selectors.ColumnSpacing(6);
        AddColumn(selectors, Pixels(96));
        AddColumn(selectors, Pixels(80));
        AddColumn(selectors, Pixels(90));
        AddColumn(selectors, Pixels(80));
        AddColumn(selectors, Star());

        auto kind = CompactButton(SurfaceLayoutItemLabel(item.kind), 96);
        kind.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        automation::AutomationProperties::SetName(kind, L"布局项类型");
        automation::AutomationProperties::SetHelpText(kind, L"点击循环切换布局项类型");
        kind.Click([this, index](auto const& sender, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            auto& value = m_surfacePreferences.customLayout[index].kind;
            value = NextLayoutItemKind(value);
            sender.template as<controls::Button>().Content(
                winrt::box_value(winrt::hstring{ SurfaceLayoutItemLabel(value) }));
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        selectors.Children().Append(kind);

        auto tool = CompactButton(SurfaceToolLabel(item.tool), 80);
        tool.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        automation::AutomationProperties::SetName(tool, L"布局项工具");
        automation::AutomationProperties::SetHelpText(tool, L"点击切换 Codex 或 ChatGPT");
        tool.Click([this, index](auto const& sender, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            auto& value = m_surfacePreferences.customLayout[index].tool;
            value = value == SurfaceTool::Codex ? SurfaceTool::ChatGpt : SurfaceTool::Codex;
            sender.template as<controls::Button>().Content(
                winrt::box_value(winrt::hstring{ SurfaceToolLabel(value) }));
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(tool, 1);
        selectors.Children().Append(tool);

        auto quota = CompactButton(SurfaceQuotaLabel(item.quotaWindow), 90);
        quota.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        automation::AutomationProperties::SetName(quota, L"布局项配额窗口");
        automation::AutomationProperties::SetHelpText(quota, L"点击循环切换配额窗口");
        quota.Click([this, index](auto const& sender, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            auto& value = m_surfacePreferences.customLayout[index].quotaWindow;
            value = NextQuotaWindow(value);
            sender.template as<controls::Button>().Content(
                winrt::box_value(winrt::hstring{ SurfaceQuotaLabel(value) }));
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(quota, 2);
        selectors.Children().Append(quota);

        auto font = CompactButton(SurfaceFontLabel(item.font), 80);
        font.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        automation::AutomationProperties::SetName(font, L"布局项字体");
        automation::AutomationProperties::SetHelpText(font, L"点击循环切换字体样式");
        font.Click([this, index](auto const& sender, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            auto& value = m_surfacePreferences.customLayout[index].font;
            value = NextFontStyle(value);
            sender.template as<controls::Button>().Content(
                winrt::box_value(winrt::hstring{ SurfaceFontLabel(value) }));
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(font, 3);
        selectors.Children().Append(font);

        auto account = CompactButton(item.accountLabel, 86);
        account.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        automation::AutomationProperties::SetName(account, L"布局项帐户");
        automation::AutomationProperties::SetHelpText(
            account,
            L"仅设置表面显示标签，不会切换 Codex 或 ChatGPT 的实际登录帐户");
        account.Click([this, index](auto const& sender, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            constexpr std::array<std::wstring_view, 3> presets{ L"当前帐户", L"个人", L"工作" };
            auto& value = m_surfacePreferences.customLayout[index].accountLabel;
            value = NextTextPreset(value, presets);
            sender.template as<controls::Button>().Content(winrt::box_value(winrt::hstring{ value }));
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(account, 4);
        selectors.Children().Append(account);
        content.Children().Append(selectors);

        controls::Grid actions;
        actions.ColumnSpacing(6);
        AddColumn(actions, Star());
        AddColumn(actions, mux::GridLengthHelper::Auto());
        AddColumn(actions, mux::GridLengthHelper::Auto());
        AddColumn(actions, mux::GridLengthHelper::Auto());

        auto customText = CompactButton(
            item.customText.empty() ? L"选择安全文案" : std::wstring_view{ item.customText }, 128);
        customText.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        automation::AutomationProperties::SetName(customText, L"布局项自定义文本");
        automation::AutomationProperties::SetHelpText(
            customText,
            L"点击循环选择 Tokenometer、专注模式或实时监看");
        customText.Click([this, index](auto const& sender, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            constexpr std::array<std::wstring_view, 3> presets{
                L"Tokenometer", L"专注模式", L"实时监看"
            };
            auto& value = m_surfacePreferences.customLayout[index].customText;
            value = NextTextPreset(value, presets);
            sender.template as<controls::Button>().Content(winrt::box_value(winrt::hstring{ value }));
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        actions.Children().Append(customText);

        auto up = CompactButton(L"↑", 30);
        up.IsEnabled(index > 0);
        automation::AutomationProperties::SetName(up, L"上移布局项");
        up.Click([this, index](auto const&, auto const&)
        {
            if (index == 0 || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            std::swap(m_surfacePreferences.customLayout[index - 1], m_surfacePreferences.customLayout[index]);
            RebuildSurfaceLayoutEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(up, 1);
        actions.Children().Append(up);

        auto down = CompactButton(L"↓", 30);
        down.IsEnabled(index + 1 < m_surfacePreferences.customLayout.size());
        automation::AutomationProperties::SetName(down, L"下移布局项");
        down.Click([this, index](auto const&, auto const&)
        {
            if (index + 1 >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            std::swap(m_surfacePreferences.customLayout[index], m_surfacePreferences.customLayout[index + 1]);
            RebuildSurfaceLayoutEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(down, 2);
        actions.Children().Append(down);

        auto remove = CompactButton(L"移除", 48);
        remove.IsEnabled(m_surfacePreferences.customLayout.size() > 1);
        remove.Foreground(TextBrush(Color(240, 126, 96)));
        automation::AutomationProperties::SetName(remove, L"移除布局项");
        remove.Click([this, index](auto const&, auto const&)
        {
            if (m_surfacePreferences.customLayout.size() <= 1
                || index >= m_surfacePreferences.customLayout.size())
            {
                return;
            }
            m_surfacePreferences.customLayout.erase(m_surfacePreferences.customLayout.begin() + index);
            RebuildSurfaceLayoutEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(remove, 3);
        actions.Children().Append(remove);
        content.Children().Append(actions);
        if (item.kind == SurfaceLayoutItemKind::CustomText)
        {
            content.Children().Append(Text(
                L"安全文案预设：Tokenometer / 专注模式 / 实时监看。",
                8.5,
                Color(143, 139, 140)));
        }
        m_surfaceLayoutEditor.Children().Append(SoftPanel(content));
    }

    controls::Grid footer;
    AddColumn(footer, Star());
    AddColumn(footer, mux::GridLengthHelper::Auto());
    auto hint = Text(
        std::to_wstring(m_surfacePreferences.customLayout.size()) + L" / 6 项",
        9,
        Color(143, 139, 140),
        500,
        true);
    hint.VerticalAlignment(mux::VerticalAlignment::Center);
    footer.Children().Append(hint);
    controls::Grid::SetColumn(m_surfaceLayoutAddButton, 1);
    footer.Children().Append(m_surfaceLayoutAddButton);
    m_surfaceLayoutEditor.Children().Append(footer);
}

void DashboardView::RebuildSurfaceToolEditor()
{
    if (!m_surfaceToolEditor)
    {
        return;
    }

    m_surfaceToolEditor.Children().Clear();
    for (size_t index = 0; index < m_surfacePreferences.tools.size(); ++index)
    {
        auto const& preference = m_surfacePreferences.tools[index];
        controls::Grid row;
        row.ColumnSpacing(8);
        AddColumn(row, Star());
        AddColumn(row, mux::GridLengthHelper::Auto());
        AddColumn(row, mux::GridLengthHelper::Auto());
        AddColumn(row, mux::GridLengthHelper::Auto());
        AddColumn(row, mux::GridLengthHelper::Auto());

        controls::StackPanel provider;
        provider.Orientation(controls::Orientation::Horizontal);
        provider.Spacing(8);
        shapes::Ellipse dot;
        dot.Width(8);
        dot.Height(8);
        dot.Fill(Brush(preference.tool == SurfaceTool::Codex
            ? Color(98, 223, 125)
            : Color(255, 253, 142)));
        dot.VerticalAlignment(mux::VerticalAlignment::Center);
        provider.Children().Append(dot);
        auto name = Text(SurfaceToolLabel(preference.tool), 10.5, Color(247, 247, 245), 650);
        name.VerticalAlignment(mux::VerticalAlignment::Center);
        provider.Children().Append(name);
        row.Children().Append(provider);

        auto visible = CompactButton(preference.visible ? L"显示" : L"隐藏", 52);
        visible.Background(Brush(preference.visible ? Color(240, 63, 22) : Color(29, 27, 28)));
        visible.BorderBrush(Brush(preference.visible ? Color(240, 63, 22) : Color(255, 255, 255, 20)));
        automation::AutomationProperties::SetName(visible, L"显示或隐藏工具");
        visible.Click([this, index](auto const&, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.tools.size())
            {
                return;
            }
            m_surfacePreferences.tools[index].visible = !m_surfacePreferences.tools[index].visible;
            RebuildSurfaceToolEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(visible, 1);
        row.Children().Append(visible);

        auto pinned = CompactButton(preference.pinned ? L"置顶" : L"普通", 52);
        pinned.Background(Brush(preference.pinned ? Color(240, 63, 22) : Color(29, 27, 28)));
        pinned.BorderBrush(Brush(preference.pinned ? Color(240, 63, 22) : Color(255, 255, 255, 20)));
        automation::AutomationProperties::SetName(pinned, L"置顶工具");
        pinned.Click([this, index](auto const&, auto const&)
        {
            if (m_updatingSurfacePreferences || index >= m_surfacePreferences.tools.size())
            {
                return;
            }
            m_surfacePreferences.tools[index].pinned = !m_surfacePreferences.tools[index].pinned;
            RebuildSurfaceToolEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(pinned, 2);
        row.Children().Append(pinned);

        auto up = CompactButton(L"↑", 30);
        up.IsEnabled(index > 0);
        automation::AutomationProperties::SetName(up, L"上移工具");
        up.Click([this, index](auto const&, auto const&)
        {
            if (index == 0 || index >= m_surfacePreferences.tools.size())
            {
                return;
            }
            std::swap(m_surfacePreferences.tools[index - 1], m_surfacePreferences.tools[index]);
            RebuildSurfaceToolEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(up, 3);
        row.Children().Append(up);

        auto down = CompactButton(L"↓", 30);
        down.IsEnabled(index + 1 < m_surfacePreferences.tools.size());
        automation::AutomationProperties::SetName(down, L"下移工具");
        down.Click([this, index](auto const&, auto const&)
        {
            if (index + 1 >= m_surfacePreferences.tools.size())
            {
                return;
            }
            std::swap(m_surfacePreferences.tools[index], m_surfacePreferences.tools[index + 1]);
            RebuildSurfaceToolEditor();
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        controls::Grid::SetColumn(down, 4);
        row.Children().Append(down);
        m_surfaceToolEditor.Children().Append(SoftPanel(row));
    }
}

void DashboardView::ShowPage(DashboardPage page)
{
    m_currentPage = page;
    m_overviewPage.Visibility(page == DashboardPage::Overview ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_detailsPage.Visibility(page == DashboardPage::Details ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_trendsPage.Visibility(page == DashboardPage::Trends ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    m_settingsPage.Visibility(page == DashboardPage::Settings ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    if (m_overviewCustomizeButton)
    {
        m_overviewCustomizeButton.Visibility(
            page == DashboardPage::Overview ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    UpdateScrollState();
    UpdateNavigationState();
}

void DashboardView::BuildShell()
{
    auto const shell = ShellColor();
    m_root = controls::Grid{};
    m_root.Background(Brush(Color(shell[0], shell[1], shell[2])));
    m_root.ActualThemeChanged([this](auto const&, auto const&)
    {
        if (m_surfaceTheme == SurfaceTheme::System)
        {
            SemanticBrushes().SetLight(m_root.ActualTheme() == mux::ElementTheme::Light);
        }
    });

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
    m_scroller.SizeChanged([this](auto const&, auto const&) { UpdateScrollState(); });
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
    AddColumn(header, mux::GridLengthHelper::Auto());

    controls::StackPanel brand;
    brand.Orientation(controls::Orientation::Horizontal);
    brand.Spacing(12);
    brand.VerticalAlignment(mux::VerticalAlignment::Center);

    auto markText = Text(L"T·", 16, Color(17, 15, 16), 700, true);
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

    m_overviewCustomizeButton = CompactButton(L"自定义总览  +", 124);
    m_overviewCustomizeButton.Height(36);
    m_overviewCustomizeButton.Margin({ 0, 0, 10, 0 });
    m_overviewCustomizeButton.Background(Brush(Color(240, 63, 22)));
    m_overviewCustomizeButton.BorderBrush(Brush(Color(240, 63, 22)));
    automation::AutomationProperties::SetName(m_overviewCustomizeButton, L"自定义总览模块");
    automation::AutomationProperties::SetHelpText(
        m_overviewCustomizeButton,
        L"打开总览模块显示与排序编辑器");
    m_overviewEditor = controls::StackPanel{};
    m_overviewEditor.Spacing(8);
    m_overviewEditor.Margin({ 4 });
    RebuildOverviewEditor();
    controls::Flyout overviewFlyout;
    overviewFlyout.Content(m_overviewEditor);
    m_overviewCustomizeButton.Flyout(overviewFlyout);
    controls::Grid::SetColumn(m_overviewCustomizeButton, 1);
    header.Children().Append(m_overviewCustomizeButton);

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
    controls::Grid::SetColumn(statusPanel, 2);
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
    m_overviewCards.clear();
    m_overviewCards.resize(5);

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
    auto overviewCard = Card(L"Codex Token（精确）", Color(98, 223, 125), overview);
    m_overviewCards[static_cast<size_t>(OverviewModule::TokenSummary)] = overviewCard;
    page.Children().Append(overviewCard);

    controls::StackPanel activity;
    activity.Spacing(10);
    activity.Children().Append(DynamicStatLine(
        L"过去 24 小时", m_dayTokensText, L"0", Color(255, 253, 142)));

    controls::Grid bars;
    bars.Height(104);
    bars.ColumnSpacing(8);
    for (size_t index = 0; index < 12; ++index)
    {
        AddColumn(bars, Star());
        controls::Border bar;
        bar.Height(12);
        bar.CornerRadius(Radius(8));
        bar.Background(Brush(Color(98, 223, 125)));
        bar.Opacity(0.2);
        bar.VerticalAlignment(mux::VerticalAlignment::Bottom);
        controls::Grid::SetColumn(bar, static_cast<int>(index));
        bars.Children().Append(bar);
        m_dailyBars.push_back(bar);
    }
    activity.Children().Append(bars);
    activity.Children().Append(DynamicStatLine(
        L"Messages", m_dayMessagesText, L"0"));
    activity.Children().Append(DynamicStatLine(
        L"Tool calls", m_dayToolCallsText, L"0"));
    auto activityCard = Card(L"Codex Token 活动", Color(255, 253, 142), activity);
    m_overviewCards[static_cast<size_t>(OverviewModule::TokenActivity)] = activityCard;
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
    m_overviewCards[static_cast<size_t>(OverviewModule::CodexLimits)] = limitsCard;
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
    m_overviewCards[static_cast<size_t>(OverviewModule::ActivityHeatmap)] = heatCard;
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
    m_chatGptOverviewPanel = SoftPanel(chatgptCopy);
    accounts.Children().Append(m_chatGptOverviewPanel);
    m_overviewRecentLabel = Text(L"Codex 近期会话", 12, Color(143, 139, 140), 600);
    accounts.Children().Append(m_overviewRecentLabel);
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

    m_overviewDevicesLabel = Text(L"本机 / WSL 设备", 12, Color(143, 139, 140), 600);
    accounts.Children().Append(m_overviewDevicesLabel);
    m_devicePanel = controls::StackPanel{};
    m_devicePanel.Spacing(7);
    accounts.Children().Append(m_devicePanel);
    auto accountsCard = Card(L"ChatGPT 估算与设备", Color(240, 63, 22), accounts);
    m_overviewCards[static_cast<size_t>(OverviewModule::RecentSessions)] = accountsCard;
    controls::Grid::SetColumn(accountsCard, 2);
    controls::Grid::SetRowSpan(accountsCard, 2);
    page.Children().Append(accountsCard);
    ApplyOverviewLayout();
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
    controls::StackPanel scopeButtons;
    scopeButtons.Orientation(controls::Orientation::Horizontal);
    scopeButtons.Spacing(4);
    constexpr std::array<std::wstring_view, 2> scopeLabels{ L"Codex 精确", L"ChatGPT 估算" };
    constexpr std::array scopeValues{ UsageScope::CodexExact, UsageScope::ChatGptEstimated };
    for (size_t index = 0; index < scopeLabels.size(); ++index)
    {
        controls::Button button;
        button.Height(30);
        button.MinWidth(index == 0 ? 82 : 94);
        button.Padding({ 10, 0, 10, 0 });
        button.CornerRadius(Radius(10));
        button.BorderThickness({ 1 });
        button.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
        button.FontSize(10);
        button.FontWeight({ 600 });
        button.Content(winrt::box_value(winrt::hstring{ scopeLabels[index] }));
        auto const scope = scopeValues[index];
        button.Click([this, scope](auto const&, auto const&)
        {
            m_detailsScope = scope;
            UpdateDetailsScopeButtons();
            UpdateDetailsDimensionButtons();
            if (m_detailsCallbacks.onScopeChanged)
            {
                m_detailsCallbacks.onScopeChanged(scope);
            }
        });
        scopeButtons.Children().Append(button);
        m_detailsScopeButtons.push_back(button);
    }
    breakdown.Children().Append(scopeButtons);
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
    breakdown.Children().Append(Text(
        L"ChatGPT 官方导出不含设备、项目、缓存、费用或工具调用。",
        8.5,
        Color(143, 139, 140)));
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

    controls::StackPanel sourceButtons;
    sourceButtons.Orientation(controls::Orientation::Horizontal);
    sourceButtons.Spacing(4);
    sourceButtons.HorizontalAlignment(mux::HorizontalAlignment::Right);
    auto codexSource = makeChoice(L"Codex 精确");
    codexSource.Click([this](auto const&, auto const&)
    {
        m_trendScope = UsageScope::CodexExact;
        UpdateTrendScopeButtons();
        if (m_trendCallbacks.onScopeChanged)
        {
            m_trendCallbacks.onScopeChanged(m_trendScope);
        }
    });
    auto chatGptSource = makeChoice(L"ChatGPT 估算");
    chatGptSource.Click([this](auto const&, auto const&)
    {
        m_trendScope = UsageScope::ChatGptEstimated;
        UpdateTrendScopeButtons();
        if (m_trendCallbacks.onScopeChanged)
        {
            m_trendCallbacks.onScopeChanged(m_trendScope);
        }
    });
    sourceButtons.Children().Append(codexSource);
    sourceButtons.Children().Append(chatGptSource);
    m_trendScopeButtons.push_back(codexSource);
    m_trendScopeButtons.push_back(chatGptSource);
    controls::Grid::SetColumn(sourceButtons, 2);
    controlsRow.Children().Append(sourceButtons);

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
    page.RowSpacing(12);
    AddRow(page, mux::GridLengthHelper::Auto());
    AddRow(page, mux::GridLengthHelper::Auto());
    AddRow(page, mux::GridLengthHelper::Auto());

    controls::Grid surfaceRow;
    surfaceRow.ColumnSpacing(12);
    AddColumn(surfaceRow, Star());
    AddColumn(surfaceRow, Star());

    controls::StackPanel appearance;
    appearance.Spacing(7);

    controls::Grid themeRow;
    AddColumn(themeRow, Pixels(54));
    AddColumn(themeRow, Star());
    auto themeLabel = Text(L"主题", 9.5, Color(143, 139, 140), 600);
    themeLabel.VerticalAlignment(mux::VerticalAlignment::Center);
    themeRow.Children().Append(themeLabel);
    controls::StackPanel themeChoices;
    themeChoices.Orientation(controls::Orientation::Horizontal);
    themeChoices.Spacing(5);
    constexpr std::array<std::wstring_view, 3> themeLabels{ L"跟随系统", L"深色", L"浅色" };
    for (size_t index = 0; index < themeLabels.size(); ++index)
    {
        auto button = CompactButton(themeLabels[index], index == 0 ? 76 : 56);
        auto const theme = static_cast<SurfaceTheme>(index);
        button.Click([this, theme](auto const&, auto const&)
        {
            m_surfacePreferences.theme = theme;
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        themeChoices.Children().Append(button);
        m_themeButtons.push_back(button);
    }
    controls::Grid::SetColumn(themeChoices, 1);
    themeRow.Children().Append(themeChoices);
    appearance.Children().Append(themeRow);

    controls::Grid opacityRow;
    AddColumn(opacityRow, Pixels(54));
    AddColumn(opacityRow, Star());
    auto opacityLabel = Text(L"玻璃", 9.5, Color(143, 139, 140), 600);
    opacityLabel.VerticalAlignment(mux::VerticalAlignment::Center);
    opacityRow.Children().Append(opacityLabel);
    controls::StackPanel opacityChoices;
    opacityChoices.Orientation(controls::Orientation::Horizontal);
    opacityChoices.Spacing(5);
    constexpr std::array<int, 4> opacityValues{ 25, 50, 75, 90 };
    for (auto const opacity : opacityValues)
    {
        auto button = CompactButton(std::to_wstring(opacity) + L"%", 48);
        button.Click([this, opacity](auto const&, auto const&)
        {
            m_surfacePreferences.glassOpacityPercent = opacity;
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        opacityChoices.Children().Append(button);
        m_opacityButtons.push_back(button);
    }
    controls::Grid::SetColumn(opacityChoices, 1);
    opacityRow.Children().Append(opacityChoices);
    appearance.Children().Append(opacityRow);

    controls::Grid effects;
    effects.ColumnSpacing(10);
    AddColumn(effects, Star());
    AddColumn(effects, Star());
    AddColumn(effects, Star());
    effects.Children().Append(ButtonSetting(
        L"模糊",
        L"气泡可见时本地捕获屏幕生成玻璃效果；不保存或上传",
        m_blurToggle));
    auto transparent = ButtonSetting(L"透明窗口", L"启用透明窗口", m_transparentWindowToggle);
    controls::Grid::SetColumn(transparent, 1);
    effects.Children().Append(transparent);
    auto colors = ButtonSetting(L"供应商颜色", L"启用供应商专属颜色", m_providerColorsToggle);
    controls::Grid::SetColumn(colors, 2);
    effects.Children().Append(colors);
    appearance.Children().Append(effects);

    m_blurToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.blurEnabled = !m_surfacePreferences.blurEnabled;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    m_transparentWindowToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.transparentWindow = !m_surfacePreferences.transparentWindow;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    m_providerColorsToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.providerColors = !m_surfacePreferences.providerColors;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });

    auto appearanceCard = SettingsCard(
        L"外观与表面",
        L"主题会实时应用于主仪表盘和气泡；玻璃、透明、模糊与供应商色应用于浮动表面。",
        Color(240, 63, 22),
        appearance);
    surfaceRow.Children().Append(appearanceCard);

    controls::Grid behavior;
    behavior.RowSpacing(5);
    behavior.ColumnSpacing(14);
    AddRow(behavior, mux::GridLengthHelper::Auto());
    AddRow(behavior, mux::GridLengthHelper::Auto());
    AddColumn(behavior, Star());
    AddColumn(behavior, Star());
    behavior.Children().Append(ButtonSetting(
        L"启动到托盘", L"启动应用时进入系统托盘", m_launchToTrayToggle));
    auto closeToTray = ButtonSetting(
        L"关闭到托盘", L"关闭窗口时保留在系统托盘", m_closeToTrayToggle);
    controls::Grid::SetColumn(closeToTray, 1);
    behavior.Children().Append(closeToTray);
    auto alwaysOnTop = ButtonSetting(
        L"气泡始终置顶", L"浮动气泡始终置顶", m_bubbleAlwaysOnTopToggle);
    controls::Grid::SetRow(alwaysOnTop, 1);
    behavior.Children().Append(alwaysOnTop);
    auto hoverPreview = ButtonSetting(
        L"悬停预览", L"悬停气泡时显示预览", m_hoverPreviewToggle);
    controls::Grid::SetRow(hoverPreview, 1);
    controls::Grid::SetColumn(hoverPreview, 1);
    behavior.Children().Append(hoverPreview);

    m_launchToTrayToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.launchToTray = !m_surfacePreferences.launchToTray;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    m_closeToTrayToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.closeToTray = !m_surfacePreferences.closeToTray;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    m_bubbleAlwaysOnTopToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.bubbleAlwaysOnTop = !m_surfacePreferences.bubbleAlwaysOnTop;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    m_hoverPreviewToggle.Click([this](auto const&, auto const&)
    {
        if (m_updatingSurfacePreferences) return;
        m_surfacePreferences.hoverPreview = !m_surfacePreferences.hoverPreview;
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });

    auto behaviorCard = SettingsCard(
        L"系统与浮动气泡",
        L"托盘生命周期与迷你窗口的交互方式。",
        Color(98, 223, 125),
        behavior);
    controls::Grid::SetColumn(behaviorCard, 1);
    surfaceRow.Children().Append(behaviorCard);
    page.Children().Append(surfaceRow);

    controls::Grid editorRow;
    editorRow.ColumnSpacing(12);
    AddColumn(editorRow, Star(1.15));
    AddColumn(editorRow, Star(0.85));
    controls::Grid::SetRow(editorRow, 1);

    controls::StackPanel layoutBody;
    layoutBody.Spacing(8);
    controls::StackPanel presets;
    presets.Orientation(controls::Orientation::Horizontal);
    presets.Spacing(5);
    constexpr std::array<std::wstring_view, 4> presetLabels{
        L"实时", L"限额", L"费用", L"自定义"
    };
    for (size_t index = 0; index < presetLabels.size(); ++index)
    {
        auto button = CompactButton(presetLabels[index], 62);
        auto const preset = static_cast<SurfaceLayoutPreset>(index);
        button.Click([this, preset](auto const&, auto const&)
        {
            m_surfacePreferences.layoutPreset = preset;
            m_surfacePreferences.layoutEditorExpanded = preset == SurfaceLayoutPreset::Custom;
            if (m_surfacePreferences.layoutEditorExpanded)
            {
                RebuildSurfaceLayoutEditor();
            }
            UpdateSurfacePreferencesLayout();
            NotifySurfacePreferencesChanged();
        });
        presets.Children().Append(button);
        m_layoutPresetButtons.push_back(button);
    }
    layoutBody.Children().Append(presets);

    m_surfacePreviewText = Text(L"", 10.5, Color(247, 247, 245), 600, true);
    m_surfacePreviewText.TextWrapping(mux::TextWrapping::Wrap);
    m_surfacePreviewText.TextTrimming(mux::TextTrimming::None);
    auto preview = SoftPanel(m_surfacePreviewText);
    preview.MinHeight(42);
    layoutBody.Children().Append(preview);

    m_surfaceLayoutExpandButton = CompactButton(L"自定义…", 126);
    m_surfaceLayoutExpandButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
    m_surfaceLayoutExpandButton.Click([this](auto const&, auto const&)
    {
        m_surfacePreferences.layoutEditorExpanded = !m_surfacePreferences.layoutEditorExpanded;
        if (m_surfacePreferences.layoutEditorExpanded)
        {
            m_surfacePreferences.layoutPreset = SurfaceLayoutPreset::Custom;
            RebuildSurfaceLayoutEditor();
        }
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    layoutBody.Children().Append(m_surfaceLayoutExpandButton);

    m_surfaceLayoutEditor = controls::StackPanel{};
    m_surfaceLayoutEditor.Spacing(8);
    m_surfaceLayoutAddButton = CompactButton(L"+ 添加布局项", 110);
    m_surfaceLayoutAddButton.Click([this](auto const&, auto const&)
    {
        if (m_surfacePreferences.customLayout.size() >= 6)
        {
            return;
        }
        SurfaceLayoutItem item;
        item.kind = SurfaceLayoutItemKind::Percentage;
        item.tool = SurfaceTool::Codex;
        m_surfacePreferences.customLayout.push_back(std::move(item));
        m_surfacePreferences.layoutPreset = SurfaceLayoutPreset::Custom;
        m_surfacePreferences.layoutEditorExpanded = true;
        RebuildSurfaceLayoutEditor();
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    m_surfaceLayoutEditorPanel = SoftPanel(m_surfaceLayoutEditor);
    m_surfaceLayoutEditorPanel.Visibility(mux::Visibility::Collapsed);
    layoutBody.Children().Append(m_surfaceLayoutEditorPanel);

    auto layoutCard = SettingsCard(
        L"托盘与气泡布局",
        L"选择预设，或展开并按顺序组合最多 6 个信息项。",
        Color(255, 253, 142),
        layoutBody);
    editorRow.Children().Append(layoutCard);

    controls::StackPanel toolsBody;
    toolsBody.Spacing(8);
    m_surfaceToolSummaryText = Text(L"", 10.5, Color(247, 247, 245), 600);
    m_surfaceToolSummaryText.TextWrapping(mux::TextWrapping::Wrap);
    m_surfaceToolSummaryText.TextTrimming(mux::TextTrimming::None);
    auto toolSummary = SoftPanel(m_surfaceToolSummaryText);
    toolSummary.MinHeight(42);
    toolsBody.Children().Append(toolSummary);

    m_surfaceToolExpandButton = CompactButton(L"管理工具", 96);
    m_surfaceToolExpandButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
    m_surfaceToolExpandButton.Click([this](auto const&, auto const&)
    {
        m_surfacePreferences.toolManagerExpanded = !m_surfacePreferences.toolManagerExpanded;
        if (m_surfacePreferences.toolManagerExpanded)
        {
            RebuildSurfaceToolEditor();
        }
        UpdateSurfacePreferencesLayout();
        NotifySurfacePreferencesChanged();
    });
    toolsBody.Children().Append(m_surfaceToolExpandButton);

    m_surfaceToolEditor = controls::StackPanel{};
    m_surfaceToolEditor.Spacing(8);
    m_surfaceToolEditorPanel = SoftPanel(m_surfaceToolEditor);
    m_surfaceToolEditorPanel.Visibility(mux::Visibility::Collapsed);
    toolsBody.Children().Append(m_surfaceToolEditorPanel);

    auto toolsCard = SettingsCard(
        L"托盘与气泡工具列表",
        L"隐藏、置顶或调整表面顺序；不会改变主仪表盘或删除追踪数据。",
        Color(98, 223, 125),
        toolsBody);
    controls::Grid::SetColumn(toolsCard, 1);
    editorRow.Children().Append(toolsCard);
    page.Children().Append(editorRow);

    controls::StackPanel importContent;
    importContent.Spacing(8);
    controls::Grid importRow;
    importRow.ColumnSpacing(18);
    AddColumn(importRow, Star(1.15));
    AddColumn(importRow, Star(0.85));

    controls::StackPanel importAction;
    importAction.Spacing(6);
    controls::Grid importHeader;
    AddColumn(importHeader, Star());
    AddColumn(importHeader, mux::GridLengthHelper::Auto());
    controls::StackPanel accountCopy;
    accountCopy.Spacing(1);
    accountCopy.Children().Append(Text(L"导入帐户", 9, Color(143, 139, 140), 600));
    m_chatGptAccountLabel = Text(L"ChatGPT", 10.5, Color(247, 247, 245), 650, true);
    accountCopy.Children().Append(m_chatGptAccountLabel);
    importHeader.Children().Append(accountCopy);

    m_chatGptChooseFilesButton = CompactButton(L"选择一个或多个 JSON", 146);
    m_chatGptChooseFilesButton.Height(34);
    m_chatGptChooseFilesButton.Background(Brush(Color(240, 63, 22)));
    m_chatGptChooseFilesButton.Foreground(Brush(Color(247, 247, 245)));
    m_chatGptChooseFilesButton.BorderBrush(Brush(Color(240, 63, 22)));
    automation::AutomationProperties::SetName(m_chatGptChooseFilesButton, L"选择 ChatGPT 导出 JSON 文件");
    m_chatGptChooseFilesButton.Click([this](auto const&, auto const&)
    {
        if (m_chatGptImportCallbacks.onChooseFilesRequested)
        {
            m_chatGptImportCallbacks.onChooseFilesRequested(
                std::wstring{ m_chatGptAccountLabel.Text().c_str() });
        }
    });
    controls::Grid::SetColumn(m_chatGptChooseFilesButton, 1);
    importHeader.Children().Append(m_chatGptChooseFilesButton);
    importAction.Children().Append(importHeader);

    controls::Grid stateRow;
    AddColumn(stateRow, Pixels(16));
    AddColumn(stateRow, Star());
    m_chatGptImportDot = shapes::Ellipse{};
    m_chatGptImportDot.Width(8);
    m_chatGptImportDot.Height(8);
    m_chatGptImportDot.VerticalAlignment(mux::VerticalAlignment::Top);
    m_chatGptImportDot.Margin({ 0, 5, 0, 0 });
    stateRow.Children().Append(m_chatGptImportDot);
    controls::StackPanel stateCopy;
    stateCopy.Spacing(1);
    m_chatGptImportTitle = Text(L"等待导入", 10.5, Color(247, 247, 245), 600);
    stateCopy.Children().Append(m_chatGptImportTitle);
    m_chatGptImportMessage = Text(L"", 9, Color(143, 139, 140));
    m_chatGptImportMessage.TextWrapping(mux::TextWrapping::Wrap);
    m_chatGptImportMessage.TextTrimming(mux::TextTrimming::None);
    stateCopy.Children().Append(m_chatGptImportMessage);
    controls::Grid::SetColumn(stateCopy, 1);
    stateRow.Children().Append(stateCopy);
    importAction.Children().Append(stateRow);

    m_chatGptFilesText = Text(L"尚未选择 JSON 文件", 9, Color(154, 150, 151), 500, true);
    m_chatGptFilesText.TextWrapping(mux::TextWrapping::Wrap);
    m_chatGptFilesText.TextTrimming(mux::TextTrimming::None);
    importAction.Children().Append(m_chatGptFilesText);
    importRow.Children().Append(importAction);

    controls::StackPanel importResults;
    importResults.Spacing(7);
    controls::Grid resultGrid;
    resultGrid.ColumnSpacing(6);
    for (int index = 0; index < 5; ++index)
    {
        AddColumn(resultGrid, Star());
    }
    auto appendResult = [&resultGrid](int column, std::wstring_view label, controls::TextBlock& target,
                                      winrt::Windows::UI::Color color)
    {
        controls::StackPanel stat;
        stat.Spacing(2);
        stat.Children().Append(Text(label, 8.5, Color(143, 139, 140), 550));
        target = Text(L"—", 10, color, 650, true);
        stat.Children().Append(target);
        controls::Grid::SetColumn(stat, column);
        resultGrid.Children().Append(stat);
    };
    appendResult(0, L"会话", m_chatGptConversationCount, Color(98, 223, 125));
    appendResult(1, L"估算 token", m_chatGptEstimatedTokens, Color(255, 253, 142));
    appendResult(2, L"跳过", m_chatGptSkippedCount, Color(154, 150, 151));
    appendResult(3, L"未变更", m_chatGptUnchangedCount, Color(154, 150, 151));
    appendResult(4, L"错误", m_chatGptErrorCount, Color(240, 126, 96));
    importResults.Children().Append(resultGrid);

    m_chatGptDetailsButton = CompactButton(L"查看详情", 84);
    m_chatGptDetailsButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
    m_chatGptDetailsButton.Foreground(TextBrush(Color(255, 253, 142)));
    automation::AutomationProperties::SetName(m_chatGptDetailsButton, L"查看 ChatGPT 导入详情");
    m_chatGptDetailsButton.Click([this](auto const&, auto const&)
    {
        m_chatGptDetailsExpanded = !m_chatGptDetailsExpanded;
        UpdateChatGptImportLayout();
        if (m_chatGptImportCallbacks.onDetailsToggled)
        {
            m_chatGptImportCallbacks.onDetailsToggled(m_chatGptDetailsExpanded);
        }
    });
    importResults.Children().Append(m_chatGptDetailsButton);
    controls::Grid::SetColumn(importResults, 1);
    importRow.Children().Append(importResults);
    importContent.Children().Append(importRow);

    m_chatGptDetailsText = Text(L"", 9.5, Color(247, 247, 245), 400, true);
    m_chatGptDetailsText.TextWrapping(mux::TextWrapping::Wrap);
    m_chatGptDetailsText.TextTrimming(mux::TextTrimming::None);
    m_chatGptDetailsPanel = SoftPanel(m_chatGptDetailsText);
    m_chatGptDetailsPanel.Visibility(mux::Visibility::Collapsed);
    importContent.Children().Append(m_chatGptDetailsPanel);

    auto importCard = SettingsCard(
        L"ChatGPT 数据导入",
        L"仅读取官方 JSON 导出；保留聚合统计与来源标识，不保存提示或回答正文。",
        Color(240, 63, 22),
        importContent);
    controls::Grid::SetRow(importCard, 2);
    page.Children().Append(importCard);
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
