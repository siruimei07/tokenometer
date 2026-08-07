#include <windows.h>
#include <shellapi.h>
#undef GetCurrentTime

#include "CaptureRenderer.h"
#include "CodexCollector.h"
#include "DashboardView.h"
#include "Database.h"
#include "SourceContentReader.h"
#include "TrendAnalytics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <microsoft.ui.xaml.window.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

namespace mux = winrt::Microsoft::UI::Xaml;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace input = winrt::Microsoft::UI::Xaml::Input;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace windowing = winrt::Microsoft::UI::Windowing;

namespace
{
    constexpr int widgetWidthDip = 420;
    constexpr int widgetHeightDip = 240;
    constexpr int cornerRadiusDip = 26;
    constexpr int dashboardWidthDip = 1280;
    constexpr int dashboardHeightDip = 800;

    winrt::Windows::UI::Color Color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
    {
        return { alpha, red, green, blue };
    }

    media::SolidColorBrush Brush(winrt::Windows::UI::Color color)
    {
        return media::SolidColorBrush{ color };
    }

    mux::CornerRadius Radius(double value)
    {
        return { value, value, value, value };
    }

    controls::TextBlock Text(
        std::wstring_view value,
        double size,
        winrt::Windows::UI::Color color,
        uint16_t weight = 400)
    {
        controls::TextBlock text;
        text.Text(value);
        text.FontFamily(media::FontFamily{ L"Segoe UI Variable Display" });
        text.FontSize(size);
        text.FontWeight({ weight });
        text.CharacterSpacing(-10);
        text.Foreground(Brush(color));
        return text;
    }

    template <typename T>
    void Place(T const& element, double left, double top)
    {
        controls::Canvas::SetLeft(element, left);
        controls::Canvas::SetTop(element, top);
    }

    controls::Border Icon(
        std::wstring_view glyph,
        winrt::Windows::UI::Color background,
        double glyphSize,
        winrt::Windows::UI::Color foreground = Color(247, 247, 242))
    {
        controls::Border icon;
        icon.Width(26);
        icon.Height(26);
        icon.CornerRadius(Radius(13));
        icon.Background(Brush(background));

        auto label = Text(glyph, glyphSize, foreground, 600);
        label.FontFamily(media::FontFamily{ L"Segoe UI Symbol" });
        label.HorizontalAlignment(mux::HorizontalAlignment::Center);
        label.VerticalAlignment(mux::VerticalAlignment::Center);
        icon.Child(label);
        return icon;
    }

    bool HasArgument(std::wstring_view expected)
    {
        int count{};
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments)
        {
            return false;
        }
        bool found{};
        for (int index = 1; index < count; ++index)
        {
            if (expected == arguments[index])
            {
                found = true;
                break;
            }
        }
        LocalFree(arguments);
        return found;
    }

    std::wstring_view DetailsDimensionKey(tokenometer::DetailsDimension dimension)
    {
        switch (dimension)
        {
        case tokenometer::DetailsDimension::Model: return L"model";
        case tokenometer::DetailsDimension::Session: return L"session";
        case tokenometer::DetailsDimension::Device: return L"device";
        case tokenometer::DetailsDimension::Project: return L"project";
        case tokenometer::DetailsDimension::Account: return L"account";
        default: return L"tool";
        }
    }

    std::wstring ClipToolContent(std::wstring_view value, size_t maximumCharacters = 16 * 1024)
    {
        if (value.size() <= maximumCharacters)
        {
            return std::wstring(value);
        }
        std::wstring result(value.substr(0, maximumCharacters));
        result += L"\n\n[内容过长，已截断显示]";
        return result;
    }

    int TrendRangeDays(tokenometer::TrendRange range)
    {
        switch (range)
        {
        case tokenometer::TrendRange::Days7: return 7;
        case tokenometer::TrendRange::Days90: return 90;
        case tokenometer::TrendRange::Days365: return 365;
        default: return 30;
        }
    }
}

struct TokenometerApp : mux::ApplicationT<TokenometerApp>
{
    void OnLaunched(mux::LaunchActivatedEventArgs const&)
    {
        m_bubbleMode = HasArgument(L"--bubble");
        if (m_bubbleMode)
        {
            m_root = controls::Canvas{};
            m_root.Background(Brush(Color(0, 0, 0, 0)));

            controls::Border fallback;
            fallback.Width(widgetWidthDip);
            fallback.Height(widgetHeightDip);
            fallback.Background(Brush(Color(18, 16, 17)));
            Place(fallback, 0, 0);
            m_root.Children().Append(fallback);

            m_swapChainPanel = controls::SwapChainPanel{};
            m_swapChainPanel.Width(widgetWidthDip);
            m_swapChainPanel.Height(widgetHeightDip);
            m_swapChainPanel.Opacity(0.28);
            Place(m_swapChainPanel, 0, 0);
            m_root.Children().Append(m_swapChainPanel);

            controls::Border usageCard;
            usageCard.Width(400);
            usageCard.Height(124);
            usageCard.CornerRadius(Radius(18));
            usageCard.Background(Brush(Color(38, 36, 37, 240)));
            usageCard.BorderBrush(Brush(Color(255, 255, 255, 18)));
            usageCard.BorderThickness({ 0.5 });
            usageCard.IsHitTestVisible(false);
            Place(usageCard, 10, 10);
            m_root.Children().Append(usageCard);

            controls::Border resetCard;
            resetCard.Width(400);
            resetCard.Height(58);
            resetCard.CornerRadius(Radius(16));
            resetCard.Background(Brush(Color(38, 36, 37, 240)));
            resetCard.BorderBrush(Brush(Color(255, 255, 255, 18)));
            resetCard.BorderThickness({ 0.5 });
            resetCard.IsHitTestVisible(false);
            Place(resetCard, 10, 144);
            m_root.Children().Append(resetCard);
            BuildContent();
        }
        else
        {
            m_dashboard = std::make_unique<tokenometer::DashboardView>();
            ConfigureDashboardCallbacks();
            if (HasArgument(L"--page-details"))
            {
                m_dashboard->ShowPage(tokenometer::DashboardPage::Details);
            }
            else if (HasArgument(L"--page-trends"))
            {
                m_dashboard->ShowPage(tokenometer::DashboardPage::Trends);
            }
            else if (HasArgument(L"--page-settings"))
            {
                m_dashboard->ShowPage(tokenometer::DashboardPage::Settings);
            }
        }

        m_window = mux::Window{};
        m_window.Title(L"Tokenometer");
        if (m_bubbleMode)
        {
            m_window.Content(m_root);
        }
        else
        {
            m_window.Content(m_dashboard->Root());
        }

        auto nativeWindow = m_window.as<::IWindowNative>();
        winrt::check_hresult(nativeWindow->get_WindowHandle(&m_hwnd));
        m_window.Activate();
        if (m_bubbleMode)
        {
            ConfigureWindow();
        }
        else
        {
            ConfigureDashboardWindow();
        }
        if (!HasArgument(L"--no-collection"))
        {
            StartCollection();
            StartDashboardRefresh();
        }
        else if (!m_bubbleMode)
        {
            m_dashboard->SetStatus(L"预览模式", L"数据采集已禁用", false);
        }
        if (m_bubbleMode && m_swapChainPanel.IsLoaded())
        {
            StartBackdrop();
        }
        else if (m_bubbleMode)
        {
            m_swapChainPanel.Loaded([this](auto const&, auto const&) { StartBackdrop(); });
        }
        WireInteractions();
    }

private:
    void ConfigureDashboardCallbacks()
    {
        tokenometer::DetailsCallbacks callbacks;
        callbacks.onDimensionChanged = [this](tokenometer::DetailsDimension dimension)
        {
            m_detailsDimension = dimension;
            m_selectedBreakdownKey.clear();
            RefreshDetails();
        };
        callbacks.onBreakdownSelected = [this](std::wstring const& key)
        {
            m_selectedBreakdownKey = m_selectedBreakdownKey == key ? std::wstring{} : key;
            RefreshDetails();
        };
        callbacks.onSessionSelected = [this](std::wstring const& sessionId)
        {
            if (m_selectedSessionId == sessionId)
            {
                m_selectedSessionId.clear();
            }
            else
            {
                m_selectedSessionId = sessionId;
            }
            m_selectedToolLocator.clear();
            m_selectedToolDetails.clear();
            RefreshDetails();
        };
        callbacks.onToolCallRequested = [this](std::wstring const& locator)
        {
            if (m_selectedToolLocator == locator)
            {
                m_selectedToolLocator.clear();
                m_selectedToolDetails.clear();
            }
            else
            {
                m_selectedToolLocator = locator;
                LoadToolContent(locator);
            }
            RefreshDetails();
        };
        m_dashboard->SetDetailsCallbacks(std::move(callbacks));

        tokenometer::TrendCallbacks trendCallbacks;
        trendCallbacks.onGroupChanged = [this](tokenometer::TrendGroup group)
        {
            m_trendGroup = group;
            RefreshTrends(true);
        };
        trendCallbacks.onChartChanged = [this](tokenometer::TrendChart chart)
        {
            m_trendChart = chart;
            RefreshTrends(true);
        };
        trendCallbacks.onRangeChanged = [this](tokenometer::TrendRange range)
        {
            m_trendRange = range;
            RefreshTrends(true);
        };
        m_dashboard->SetTrendCallbacks(std::move(trendCallbacks));
    }

    void LoadToolContent(std::wstring const& locator)
    {
        auto const match = std::find_if(
            m_toolLocators.begin(),
            m_toolLocators.end(),
            [&locator](auto const& item) { return item.first == locator; });
        if (match == m_toolLocators.end())
        {
            m_selectedToolDetails = L"该工具定位信息已经失效，请重新选择会话。";
            return;
        }

        try
        {
            tokenometer::SourceContentReader reader;
            auto const content = reader.Read(match->second);
            std::wstring details;
            details.reserve(std::min<size_t>(
                content.input.size() + content.output.size() + 32,
                32 * 1024 + 32));
            details += L"输入\n";
            details += content.input.empty() ? L"（源记录未提供输入）" : ClipToolContent(content.input);
            details += L"\n\n输出\n";
            details += content.output.empty() ? L"（尚未找到输出记录）" : ClipToolContent(content.output);
            m_selectedToolDetails = std::move(details);
        }
        catch (...)
        {
            m_selectedToolDetails = L"无法安全读取该工具记录；源文件可能已移动、被裁剪或不在允许目录。";
        }
    }

    void RefreshDetails()
    {
        if (!m_dashboard)
        {
            return;
        }

        tokenometer::DetailsViewData data;
        data.dimension = m_detailsDimension;
        data.selectedKey = m_selectedBreakdownKey;
        data.selectedSessionId = m_selectedSessionId;
        data.selectedToolCallLocator = m_selectedToolLocator;
        data.selectedToolDetails = m_selectedToolDetails;
        data.loading = m_collecting.load(std::memory_order_relaxed);
        m_toolLocators.clear();

        if (!m_database)
        {
            data.error = L"本地使用数据库不可用";
            m_dashboard->UpdateDetails(data);
            return;
        }

        try
        {
            data.rows = m_database->GetBreakdown(DetailsDimensionKey(m_detailsDimension), 0, 12);
            data.recentSessions = m_database->GetRecentSessions(3);
            if (!m_selectedSessionId.empty())
            {
                data.selectedTurns = m_database->GetSessionTurns(m_selectedSessionId, 8);
                size_t toolIndex{};
                for (auto const& turn : data.selectedTurns)
                {
                    for (auto const& tool : m_database->GetToolCalls(m_selectedSessionId, turn.promptIndex))
                    {
                        if (toolIndex >= 20)
                        {
                            break;
                        }
                        std::wstring locator = L"tool-" + std::to_wstring(toolIndex + 1);
                        tokenometer::ToolCallViewData view;
                        view.locator = locator;
                        view.name = tool.name;
                        if (tool.inputLength > 0 && tool.outputLength > 0)
                        {
                            view.summary = L"输入 / 输出可按需读取";
                        }
                        else if (tool.inputLength > 0)
                        {
                            view.summary = L"输入可用 · 暂无输出";
                        }
                        else
                        {
                            view.summary = L"源详情不可用";
                        }
                        data.toolCalls.push_back(std::move(view));
                        m_toolLocators.emplace_back(std::move(locator), tool);
                        ++toolIndex;
                    }
                    if (toolIndex >= 20)
                    {
                        break;
                    }
                }
            }
        }
        catch (...)
        {
            data.error = L"读取本地使用明细失败";
        }
        m_dashboard->UpdateDetails(data);
    }

    void RefreshTrends(bool force = false)
    {
        if (!m_dashboard)
        {
            return;
        }

        auto const nowTick = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!force && nowTick - m_lastTrendRefreshTick < 5000)
        {
            return;
        }
        m_lastTrendRefreshTick = nowTick;

        tokenometer::TrendViewData data;
        data.group = m_trendGroup;
        data.chart = m_trendChart;
        data.range = m_trendRange;
        if (!m_database)
        {
            data.error = L"本地使用数据库不可用";
            m_dashboard->UpdateTrends(data);
            return;
        }

        try
        {
            int const rangeDays = TrendRangeDays(m_trendRange);
            auto const daily = m_database->GetDailyUsage(365);
            std::vector<tokenometer::HourlyUsage> hourly;
            if (m_trendChart == tokenometer::TrendChart::Kline)
            {
                hourly = m_database->GetHourlyUsage(rangeDays);
            }
            auto const dimension = m_trendGroup == tokenometer::TrendGroup::Tool
                ? tokenometer::TrendDimension::Tool
                : tokenometer::TrendDimension::Model;
            auto const analysis = tokenometer::AnalyzeTrends(daily, hourly, dimension, rangeDays);

            for (auto const& legend : analysis.legend)
            {
                tokenometer::TrendSeries series;
                series.key = legend.series;
                series.total = legend.total;
                series.percent = legend.percent;
                series.points.reserve(analysis.stackedDays.size());
                for (auto const& day : analysis.stackedDays)
                {
                    auto const value = std::find_if(day.values.begin(), day.values.end(), [&](auto const& item)
                    {
                        return item.series == legend.series;
                    });
                    series.points.push_back({ day.day, value == day.values.end() ? 0 : value->value });
                }
                data.series.push_back(std::move(series));
            }

            data.heatCells.reserve(analysis.heatmap.size());
            for (auto const& day : analysis.heatmap)
            {
                data.heatCells.push_back({ day.day, day.value });
            }
            data.currentStreak = analysis.currentStreak;
            data.longestStreak = analysis.longestStreak;

            if (!analysis.legend.empty())
            {
                data.candleSeries = analysis.legend.front().series;
                for (auto const& candle : analysis.candles)
                {
                    if (candle.series == data.candleSeries)
                    {
                        data.candles.push_back({
                            candle.day,
                            candle.open,
                            candle.high,
                            candle.low,
                            candle.close,
                            candle.volume,
                        });
                    }
                }
            }
            data.loading = m_collecting.load(std::memory_order_relaxed) && data.series.empty();
        }
        catch (...)
        {
            data.error = L"读取或汇总趋势数据失败";
        }
        m_dashboard->UpdateTrends(data);
    }

    void BuildContent()
    {
        auto usageIcon = Icon(L"✳", Color(240, 63, 22), 13);
        Place(usageIcon, 28, 27);
        m_root.Children().Append(usageIcon);

        auto title = Text(L"Token Usage", 15, Color(247, 247, 245), 600);
        Place(title, 64, 25);
        m_root.Children().Append(title);

        auto percent = Text(L"53,8%", 32, Color(247, 247, 245), 650);
        percent.Width(104);
        percent.TextAlignment(mux::TextAlignment::Right);
        Place(percent, 288, 14);
        m_root.Children().Append(percent);

        controls::Border track;
        track.Width(364);
        track.Height(10);
        track.CornerRadius(Radius(5));
        track.Background(Brush(Color(52, 49, 50)));
        track.BorderBrush(Brush(Color(255, 255, 255, 20)));
        track.BorderThickness({ 0.5 });
        Place(track, 28, 70);
        m_root.Children().Append(track);

        controls::Border fill;
        fill.Width(195);
        fill.Height(8);
        fill.CornerRadius(Radius(4));
        fill.Background(Brush(Color(98, 223, 125)));
        Place(fill, 29, 71);
        m_root.Children().Append(fill);

        shapes::Ellipse marker;
        marker.Width(12);
        marker.Height(12);
        marker.Fill(Brush(Color(38, 36, 37)));
        marker.Stroke(Brush(Color(98, 223, 125)));
        marker.StrokeThickness(3);
        Place(marker, 218, 69);
        m_root.Children().Append(marker);

        auto used = Text(L"18,838", 11.5, Color(247, 247, 245), 600);
        Place(used, 28, 91);
        m_root.Children().Append(used);

        auto total = Text(L"/ 35,000", 11.5, Color(167, 163, 164));
        Place(total, 76, 91);
        m_root.Children().Append(total);

        auto left = Text(L"16,162 left", 11.5, Color(167, 163, 164));
        left.Width(100);
        left.TextAlignment(mux::TextAlignment::Right);
        Place(left, 292, 91);
        m_root.Children().Append(left);

        auto resetIcon = Icon(L"⌛", Color(255, 253, 142), 12, Color(38, 36, 37));
        Place(resetIcon, 28, 160);
        m_root.Children().Append(resetIcon);

        auto reset = Text(L"Reset Time", 15, Color(247, 247, 245), 600);
        Place(reset, 64, 158);
        m_root.Children().Append(reset);

        auto remaining = Text(L"2h 58m", 24, Color(255, 253, 142), 600);
        remaining.Width(110);
        remaining.TextAlignment(mux::TextAlignment::Right);
        Place(remaining, 282, 150);
        m_root.Children().Append(remaining);

        auto refresh = Text(L"↻", 10, Color(133, 129, 130), 600);
        Place(refresh, 28, 215);
        m_root.Children().Append(refresh);

        auto updated = Text(L"Updated: Just Now", 9.5, Color(133, 129, 130));
        Place(updated, 43, 215);
        m_root.Children().Append(updated);

        m_closeButton = controls::Button{};
        m_closeButton.Width(22);
        m_closeButton.Height(22);
        m_closeButton.Padding({ 7 });
        m_closeButton.Background(Brush(Color(0, 0, 0, 0)));
        m_closeButton.BorderThickness({ 0 });
        m_closeButton.Opacity(0);

        shapes::Ellipse dot;
        dot.Width(8);
        dot.Height(8);
        dot.Fill(Brush(Color(240, 63, 22)));
        m_closeButton.Content(dot);
        controls::ToolTipService::SetToolTip(m_closeButton, winrt::box_value(L"Close"));
        automation::AutomationProperties::SetName(m_closeButton, L"Close Tokenometer");
        Place(m_closeButton, 2, 2);
        m_root.Children().Append(m_closeButton);
    }

    void ConfigureWindow()
    {
        auto appWindow = m_window.AppWindow();
        auto presenter = appWindow.Presenter().as<windowing::OverlappedPresenter>();
        presenter.SetBorderAndTitleBar(false, false);
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
        presenter.IsAlwaysOnTop(true);
        appWindow.IsShownInSwitchers(false);

        auto const style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        winrt::check_bool(SetWindowLongPtrW(
            m_hwnd,
            GWL_STYLE,
            (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                       WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) |
                WS_POPUP));
        auto const extendedStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        winrt::check_bool(SetWindowLongPtrW(
            m_hwnd,
            GWL_EXSTYLE,
            extendedStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                              WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE)));

        UINT const dpi = GetDpiForWindow(m_hwnd);
        int const width = MulDiv(widgetWidthDip, dpi, 96);
        int const height = MulDiv(widgetHeightDip, dpi, 96);
        appWindow.Resize({ width, height });

        HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof(info) };
        winrt::check_bool(GetMonitorInfoW(monitor, &info));
        int const x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
        int const y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2;
        winrt::check_bool(SetWindowPos(
            m_hwnd,
            HWND_TOPMOST,
            x,
            y,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED));

        int const diameter = MulDiv(cornerRadiusDip * 2, dpi, 96);
        HRGN const region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
        winrt::check_pointer(region);
        if (!SetWindowRgn(m_hwnd, region, FALSE))
        {
            DeleteObject(region);
            winrt::throw_last_error();
        }
    }

    void ConfigureDashboardWindow()
    {
        auto appWindow = m_window.AppWindow();
        auto presenter = appWindow.Presenter().as<windowing::OverlappedPresenter>();
        presenter.IsResizable(true);
        presenter.IsMaximizable(true);
        presenter.IsMinimizable(true);
        presenter.IsAlwaysOnTop(false);
        appWindow.IsShownInSwitchers(true);

        HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof(info) };
        winrt::check_bool(GetMonitorInfoW(monitor, &info));
        int const workWidth = info.rcWork.right - info.rcWork.left;
        int const workHeight = info.rcWork.bottom - info.rcWork.top;
        UINT const dpi = GetDpiForWindow(m_hwnd);
        int const width = std::min(MulDiv(dashboardWidthDip, dpi, 96), std::max(workWidth - 48, 640));
        int const height = std::min(MulDiv(dashboardHeightDip, dpi, 96), std::max(workHeight - 48, 520));
        appWindow.Resize({ width, height });

        int const x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
        int const y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2;
        winrt::check_bool(SetWindowPos(
            m_hwnd,
            HWND_NOTOPMOST,
            x,
            y,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE));
    }

    void WireInteractions()
    {
        if (m_bubbleMode)
        {
            m_closeButton.Click([this](auto const&, auto const&) { m_window.Close(); });
            m_root.PointerEntered([this](auto const&, auto const&) { m_closeButton.Opacity(1); });
            m_root.PointerExited([this](auto const&, auto const&) { m_closeButton.Opacity(0); });
            m_root.PointerPressed([this](auto const&, input::PointerRoutedEventArgs const& args)
            {
                auto point = args.GetCurrentPoint(m_root);
                if (point.Properties().IsLeftButtonPressed())
                {
                    ReleaseCapture();
                    SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    args.Handled(true);
                }
            });
        }

        m_window.Closed([this](auto const&, auto const&)
        {
            if (m_statusTimer)
            {
                m_statusTimer.Stop();
            }
            if (m_usageTimer)
            {
                m_usageTimer.Stop();
            }
            if (m_renderer)
            {
                m_renderer->Stop();
                m_renderer.reset();
            }
            if (m_collectionThread.joinable())
            {
                m_collectionThread.request_stop();
            }
        });
    }

    void StartCollection()
    {
        try
        {
            auto const databasePath = tokenometer::Database::DefaultDataDirectory() / L"tokenometer.db";
            m_database = std::make_shared<tokenometer::Database>(databasePath);
            m_database->Initialize();
            m_collector = std::make_unique<tokenometer::CodexCollector>(*m_database);
            m_collectionThread = std::jthread([this](std::stop_token stopToken)
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    while (!stopToken.stop_requested())
                    {
                        try
                        {
                            m_collecting.store(true, std::memory_order_relaxed);
                            auto const result = m_collector->CollectOnce(stopToken);
                            m_lastCollectionAt.store(result.completedAt, std::memory_order_relaxed);
                            m_collectionFailed.store(false, std::memory_order_relaxed);
                            if (!stopToken.stop_requested() && m_database->PruneDetailsIfDue())
                            {
                                m_database->Optimize();
                            }
                        }
                        catch (...)
                        {
                            m_collectionFailed.store(true, std::memory_order_relaxed);
                            OutputDebugStringW(L"Tokenometer: local usage collection failed.\n");
                        }
                        m_collecting.store(false, std::memory_order_relaxed);
                        for (int tick = 0; tick < 20 && !stopToken.stop_requested(); ++tick)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                    winrt::uninit_apartment();
                }
                catch (...)
                {
                    OutputDebugStringW(L"Tokenometer: collector thread initialization failed.\n");
                }
            });
        }
        catch (...)
        {
            m_collectionFailed.store(true, std::memory_order_relaxed);
            OutputDebugStringW(L"Tokenometer: local database initialization failed.\n");
        }
    }

    void StartDashboardRefresh()
    {
        if (m_bubbleMode || !m_dashboard)
        {
            return;
        }
        m_usageTimer = mux::DispatcherTimer{};
        m_usageTimer.Interval(std::chrono::seconds(1));
        m_usageTimer.Tick([this](auto const&, auto const&) { RefreshDashboard(); });
        RefreshDashboard();
        m_usageTimer.Start();
    }

    void RefreshDashboard()
    {
        if (!m_dashboard)
        {
            return;
        }

        tokenometer::OverviewViewData snapshot;
        snapshot.collecting = m_collecting.load(std::memory_order_relaxed);
        snapshot.lastSync = m_lastCollectionAt.load(std::memory_order_relaxed);
        if (!m_database)
        {
            snapshot.error = L"本地使用数据库不可用";
            m_dashboard->UpdateOverview(snapshot);
            return;
        }

        try
        {
            snapshot.total = m_database->GetTotals();
            snapshot.daily = m_database->GetDailyUsage(30);
            snapshot.recent = m_database->GetRecentSessions(3);
            snapshot.codexLimit = m_database->GetLatestRateLimit();

            int64_t const now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            int64_t const cutoff = now - 86400;
            for (auto const& hour : m_database->GetHourlyUsage(2))
            {
                if (hour.hourStart < cutoff)
                {
                    continue;
                }
                snapshot.day.counts.input += hour.counts.input;
                snapshot.day.counts.cachedInput += hour.counts.cachedInput;
                snapshot.day.counts.cacheWriteInput += hour.counts.cacheWriteInput;
                snapshot.day.counts.output += hour.counts.output;
                snapshot.day.counts.reasoningOutput += hour.counts.reasoningOutput;
                snapshot.day.counts.reportedTotal += hour.counts.reportedTotal;
                snapshot.day.messages += hour.messages;
                snapshot.day.toolCalls += hour.toolCalls;
            }
            if (m_collectionFailed.load(std::memory_order_relaxed))
            {
                snapshot.error = L"最近一次增量采集失败；已保留上次数据";
            }
        }
        catch (...)
        {
            snapshot.error = L"读取本地使用快照失败";
        }
        m_dashboard->UpdateOverview(snapshot);
        if (m_dashboard->CurrentPage() == tokenometer::DashboardPage::Details)
        {
            RefreshDetails();
        }
        else if (m_dashboard->CurrentPage() == tokenometer::DashboardPage::Trends)
        {
            RefreshTrends();
        }
    }

    void StartBackdrop()
    {
        if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
        {
            OutputDebugStringW(L"Tokenometer: capture exclusion unavailable; using static background.\n");
            return;
        }

        try
        {
            m_renderer = CaptureRenderer::Create(m_hwnd, m_swapChainPanel);
        }
        catch (winrt::hresult_error const& error)
        {
            OutputDebugStringW(error.message().c_str());
            OutputDebugStringW(L"\nTokenometer: capture renderer unavailable; using static background.\n");
            return;
        }
        catch (...)
        {
            OutputDebugStringW(L"Tokenometer: capture renderer unavailable; using static background.\n");
            return;
        }

        m_statusTimer = mux::DispatcherTimer{};
        m_statusTimer.Interval(std::chrono::milliseconds(250));
        m_statusTimer.Tick([this](auto const&, auto const&)
        {
            if (m_renderer && m_renderer->PresentedFrames() > 0)
            {
                m_window.Title(L"Tokenometer [presenting]");
                m_statusTimer.Stop();
            }
        });
        m_statusTimer.Start();
    }

    mux::Window m_window{ nullptr };
    controls::Canvas m_root{ nullptr };
    controls::SwapChainPanel m_swapChainPanel{ nullptr };
    controls::Button m_closeButton{ nullptr };
    std::unique_ptr<tokenometer::DashboardView> m_dashboard;
    mux::DispatcherTimer m_statusTimer{ nullptr };
    mux::DispatcherTimer m_usageTimer{ nullptr };
    std::shared_ptr<CaptureRenderer> m_renderer;
    std::shared_ptr<tokenometer::Database> m_database;
    std::unique_ptr<tokenometer::CodexCollector> m_collector;
    std::jthread m_collectionThread;
    std::atomic<int64_t> m_lastCollectionAt{};
    std::atomic_bool m_collecting{};
    std::atomic_bool m_collectionFailed{};
    tokenometer::DetailsDimension m_detailsDimension{ tokenometer::DetailsDimension::Tool };
    std::wstring m_selectedBreakdownKey;
    std::wstring m_selectedSessionId;
    std::wstring m_selectedToolLocator;
    std::wstring m_selectedToolDetails;
    std::vector<std::pair<std::wstring, tokenometer::ToolCallDetail>> m_toolLocators;
    tokenometer::TrendGroup m_trendGroup{ tokenometer::TrendGroup::Tool };
    tokenometer::TrendChart m_trendChart{ tokenometer::TrendChart::Bars };
    tokenometer::TrendRange m_trendRange{ tokenometer::TrendRange::Days30 };
    int64_t m_lastTrendRefreshTick{};
    HWND m_hwnd{};
    bool m_bubbleMode{};
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    if (HasArgument(L"--self-test-storage"))
    {
        if (!tokenometer::Database::SelfTest()) return 11;
        if (!tokenometer::CodexCollector::SelfTest()) return 12;
        if (!tokenometer::TestSourceContentReader()) return 13;
        if (!tokenometer::TestTrendAnalytics()) return 14;
        return 0;
    }

    try
    {
        mux::Application::Start([](auto&&) { winrt::make<TokenometerApp>(); });
        return 0;
    }
    catch (winrt::hresult_error const& error)
    {
        MessageBoxW(nullptr, error.message().c_str(), L"Tokenometer failed", MB_OK | MB_ICONERROR);
        return static_cast<int>(error.code());
    }
}
