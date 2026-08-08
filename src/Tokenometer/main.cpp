#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#undef GetCurrentTime

#include "CaptureRenderer.h"
#include "ChatGPTExportImporter.h"
#include "CodexCollector.h"
#include "DashboardView.h"
#include "Database.h"
#include "SourceContentReader.h"
#include "SurfacePreferences.h"
#include "TrayIcon.h"
#include "TrendAnalytics.h"
#include "WslCodexCollector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <microsoft.ui.xaml.window.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Dispatching.h>
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
    constexpr DWORD chatGptAccountLabelControl = 1001;

    struct ChatGptFileSelection
    {
        std::vector<std::filesystem::path> paths;
        std::wstring accountLabel;
    };

    struct BubbleUsageSnapshot
    {
        tokenometer::TokenCounts today;
        std::optional<tokenometer::RateLimitSnapshot> codexLimit;
        int64_t chatGptEstimatedTokens{};
        int64_t chatGptEstimatedSessions{};
        int64_t refreshedAt{};
        bool available{};
    };

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

    ChatGptFileSelection PickChatGptExportFiles(HWND owner, std::wstring_view accountLabel)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        DWORD options{};
        winrt::check_hresult(dialog->GetOptions(&options));
        winrt::check_hresult(dialog->SetOptions(
            options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM));
        COMDLG_FILTERSPEC const filters[] = {
            { L"ChatGPT conversations JSON", L"conversations*.json" },
            { L"JSON files", L"*.json" },
        };
        winrt::check_hresult(dialog->SetFileTypes(2, filters));
        winrt::check_hresult(dialog->SetDefaultExtension(L"json"));

        auto customize = dialog.as<IFileDialogCustomize>();
        winrt::check_hresult(customize->AddText(1000, L"账户标签（可选）"));
        winrt::check_hresult(customize->AddEditBox(
            chatGptAccountLabelControl,
            std::wstring{ accountLabel }.c_str()));

        auto const shown = dialog->Show(owner);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return {};
        }
        winrt::check_hresult(shown);

        winrt::com_ptr<IShellItemArray> results;
        winrt::check_hresult(dialog->GetResults(results.put()));
        DWORD count{};
        winrt::check_hresult(results->GetCount(&count));

        ChatGptFileSelection selection;
        selection.paths.reserve(count);
        for (DWORD index = 0; index < count; ++index)
        {
            winrt::com_ptr<IShellItem> item;
            winrt::check_hresult(results->GetItemAt(index, item.put()));
            PWSTR rawPath{};
            winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
            if (rawPath)
            {
                selection.paths.emplace_back(rawPath);
                CoTaskMemFree(rawPath);
            }
        }
        PWSTR rawAccountLabel{};
        winrt::check_hresult(customize->GetEditBoxText(
            chatGptAccountLabelControl,
            &rawAccountLabel));
        if (rawAccountLabel)
        {
            selection.accountLabel = rawAccountLabel;
            CoTaskMemFree(rawAccountLabel);
        }
        return selection;
    }

    std::wstring NormalizeAccountLabel(std::wstring_view value)
    {
        size_t first{};
        while (first < value.size() && std::iswspace(value[first]))
        {
            ++first;
        }
        size_t last = value.size();
        while (last > first && std::iswspace(value[last - 1]))
        {
            --last;
        }

        std::wstring result;
        result.reserve(std::min<size_t>(last - first, 64));
        for (size_t index = first; index < last && result.size() < 64; ++index)
        {
            if (!std::iswcntrl(value[index]))
            {
                result.push_back(value[index]);
            }
        }
        return result.empty() ? std::wstring{ L"ChatGPT" } : result;
    }

    bool IsCancelled(std::system_error const& error)
    {
        return error.code() == std::make_error_code(std::errc::operation_canceled);
    }

    std::wstring FormatCompactTokens(int64_t value)
    {
        value = std::max<int64_t>(value, 0);
        auto scaled = [value](int64_t divisor, wchar_t suffix)
        {
            int64_t const whole = value / divisor;
            int64_t const tenth = ((value % divisor) * 10) / divisor;
            std::wstring result = std::to_wstring(whole);
            if (whole < 100 && tenth > 0)
            {
                result += L"." + std::to_wstring(tenth);
            }
            result.push_back(suffix);
            return result;
        };
        if (value >= 1'000'000'000) return scaled(1'000'000'000, L'B');
        if (value >= 1'000'000) return scaled(1'000'000, L'M');
        if (value >= 1'000) return scaled(1'000, L'K');
        return std::to_wstring(value);
    }

    int64_t UnixNow()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::wstring FormatResetCountdown(int64_t resetsAt)
    {
        int64_t const seconds = resetsAt - UnixNow();
        if (resetsAt <= 0 || seconds <= 0) return L"等待刷新";
        int64_t const days = seconds / 86400;
        int64_t const hours = (seconds % 86400) / 3600;
        int64_t const minutes = (seconds % 3600) / 60;
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

    std::wstring FormatQuotaWindow(int minutes)
    {
        if (minutes >= 7 * 24 * 60) return L"周额度";
        if (minutes >= 24 * 60) return std::to_wstring(minutes / (24 * 60)) + L"天额度";
        if (minutes >= 60) return std::to_wstring(minutes / 60) + L"小时额度";
        if (minutes > 0) return std::to_wstring(minutes) + L"分钟额度";
        return L"额度";
    }
}

struct TokenometerApp : mux::ApplicationT<TokenometerApp>
{
    ~TokenometerApp()
    {
        StopBackgroundWorkers();
    }

    void OnLaunched(mux::LaunchActivatedEventArgs const&)
    {
        m_bubbleMode = HasArgument(L"--bubble");
        m_backdropDisabled = HasArgument(L"--no-backdrop");
        InitializeStorage();
        BuildBubbleSurface();

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

        m_window = mux::Window{};
        m_window.Title(L"Tokenometer");
        m_window.Content(m_bubbleMode ? mux::UIElement{ m_root } : mux::UIElement{ m_dashboard->Root() });

        auto nativeWindow = m_window.as<::IWindowNative>();
        winrt::check_hresult(nativeWindow->get_WindowHandle(&m_hwnd));
        m_dashboardStyle = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        m_dashboardExtendedStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        InstallWindowProcedure();
        if (m_bubbleMode)
        {
            ConfigureWindow();
        }
        else
        {
            ConfigureDashboardWindow();
        }
        InitializeTrayIcon();
        ApplySurfacePreferences();
        bool const startHidden = m_surfacePreferences.launchToTray &&
            m_trayIcon && m_trayIcon->IsAdded();
        if (!startHidden) m_window.Activate();
        if (!HasArgument(L"--no-collection"))
        {
            StartCollection();
        }
        else
        {
            m_dashboard->SetStatus(L"预览模式", L"数据采集已禁用", false);
        }
        StartDashboardRefresh();
        WireInteractions();
        if (startHidden) ShowWindow(m_hwnd, SW_HIDE);
    }

private:
    void BuildBubbleSurface()
    {
        m_root = controls::Canvas{};
        m_root.Background(Brush(Color(0, 0, 0, 0)));

        m_bubbleBackground = controls::Border{};
        m_bubbleBackground.Width(widgetWidthDip);
        m_bubbleBackground.Height(widgetHeightDip);
        Place(m_bubbleBackground, 0, 0);
        m_root.Children().Append(m_bubbleBackground);

        if (!m_backdropDisabled)
        {
            m_swapChainPanel = controls::SwapChainPanel{};
            m_swapChainPanel.Width(widgetWidthDip);
            m_swapChainPanel.Height(widgetHeightDip);
            Place(m_swapChainPanel, 0, 0);
            m_root.Children().Append(m_swapChainPanel);
            m_swapChainPanel.Loaded([this](auto const&, auto const&)
            {
                if (m_bubbleMode && m_surfacePreferences.blurEnabled) StartBackdrop();
            });
        }

        m_usageCard = controls::Border{};
        m_usageCard.Width(400);
        m_usageCard.Height(124);
        m_usageCard.CornerRadius(Radius(18));
        m_usageCard.BorderThickness({ 0.5 });
        m_usageCard.IsHitTestVisible(false);
        Place(m_usageCard, 10, 10);
        m_root.Children().Append(m_usageCard);

        m_resetCard = controls::Border{};
        m_resetCard.Width(400);
        m_resetCard.Height(58);
        m_resetCard.CornerRadius(Radius(16));
        m_resetCard.BorderThickness({ 0.5 });
        m_resetCard.IsHitTestVisible(false);
        Place(m_resetCard, 10, 144);
        m_root.Children().Append(m_resetCard);
        BuildContent();
    }

    void ConfigureDashboardCallbacks()
    {
        tokenometer::DetailsCallbacks callbacks;
        callbacks.onScopeChanged = [this](tokenometer::UsageScope scope)
        {
            m_detailsScope = scope;
            if (scope == tokenometer::UsageScope::ChatGptEstimated &&
                (m_detailsDimension == tokenometer::DetailsDimension::Device ||
                 m_detailsDimension == tokenometer::DetailsDimension::Project))
            {
                m_detailsDimension = tokenometer::DetailsDimension::Tool;
            }
            m_selectedBreakdownKey.clear();
            m_selectedSessionId.clear();
            m_selectedSessionAccountId.clear();
            m_selectedSessionSourceKind.clear();
            m_selectedToolLocator.clear();
            m_selectedToolDetails.clear();
            m_toolLocators.clear();
            if (m_toolContentThread.joinable()) m_toolContentThread.request_stop();
            RefreshDetails();
        };
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
        callbacks.onSessionSelected = [this](tokenometer::SessionRef const& session)
        {
            if (m_selectedSessionId == session.sessionId &&
                m_selectedSessionAccountId == session.accountId &&
                m_selectedSessionSourceKind == session.sourceKind)
            {
                m_selectedSessionId.clear();
                m_selectedSessionAccountId.clear();
                m_selectedSessionSourceKind.clear();
            }
            else
            {
                m_selectedSessionId = session.sessionId;
                m_selectedSessionAccountId = session.accountId;
                m_selectedSessionSourceKind = session.sourceKind;
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
                if (m_toolContentThread.joinable()) m_toolContentThread.request_stop();
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
        trendCallbacks.onScopeChanged = [this](tokenometer::UsageScope scope)
        {
            m_trendScope = scope;
            RefreshTrends(true);
        };
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

        tokenometer::ChatGptImportCallbacks importCallbacks;
        importCallbacks.onChooseFilesRequested = [this](std::wstring const& accountLabel)
        {
            BeginChatGptImport(accountLabel);
        };
        importCallbacks.onDetailsToggled = [this](bool expanded)
        {
            m_chatGptImportData.detailsExpanded = expanded;
        };
        m_dashboard->SetChatGptImportCallbacks(std::move(importCallbacks));

        tokenometer::SurfacePreferencesCallbacks surfaceCallbacks;
        surfaceCallbacks.onChanged = [this](tokenometer::SurfacePreferencesViewData const& data)
        {
            static_cast<tokenometer::SurfacePreferences&>(m_surfacePreferences) = data;
            m_layoutEditorExpanded = data.layoutEditorExpanded;
            m_toolManagerExpanded = data.toolManagerExpanded;
            SaveSurfacePreferences();
            ApplySurfacePreferences();
        };
        m_dashboard->SetSurfacePreferencesCallbacks(std::move(surfaceCallbacks));
    }

    void InitializeStorage() noexcept
    {
        try
        {
            auto const databasePath = tokenometer::Database::DefaultDataDirectory() / L"tokenometer.db";
            m_database = std::make_shared<tokenometer::Database>(databasePath);
            m_database->Initialize();
            m_surfacePreferences = tokenometer::SurfacePreferences::Load(*m_database);
            UpdateBubbleSnapshotCache();
        }
        catch (...)
        {
            m_database.reset();
            m_collectionFailed.store(true, std::memory_order_relaxed);
            OutputDebugStringW(L"Tokenometer: local database initialization failed.\n");
        }
    }

    void SaveSurfacePreferences() noexcept
    {
        if (!m_database || !m_surfacePreferences.IsValid()) return;
        try
        {
            m_surfacePreferences.Save(*m_database);
        }
        catch (...)
        {
            OutputDebugStringW(L"Tokenometer: surface preferences could not be saved.\n");
        }
    }

    [[nodiscard]] bool ToolVisible(tokenometer::SurfaceTool tool) const noexcept
    {
        auto const match = std::find_if(
            m_surfacePreferences.tools.begin(),
            m_surfacePreferences.tools.end(),
            [tool](auto const& item) { return item.tool == tool; });
        return match != m_surfacePreferences.tools.end() && match->visible;
    }

    [[nodiscard]] std::optional<tokenometer::SurfaceTool> PrimarySurfaceTool() const noexcept
    {
        for (auto const& item : m_surfacePreferences.tools)
        {
            if (item.visible && item.pinned) return item.tool;
        }
        for (auto const& item : m_surfacePreferences.tools)
        {
            if (item.visible) return item.tool;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::wstring BuildSurfaceText()
    {
        BubbleUsageSnapshot snapshot;
        {
            std::scoped_lock lock(m_bubbleSnapshotMutex);
            snapshot = m_bubbleSnapshot;
        }

        struct Quota
        {
            double remaining{-1.0};
            int64_t resetsAt{};
        } quota;
        if (snapshot.codexLimit)
        {
            int64_t const now = UnixNow();
            auto consider = [&](double used, int64_t resetsAt)
            {
                if (used >= 0.0 && used <= 100.0 && resetsAt > now &&
                    100.0 - used < quota.remaining)
                {
                    quota = { 100.0 - used, resetsAt };
                }
                else if (used >= 0.0 && used <= 100.0 && resetsAt > now && quota.remaining < 0.0)
                {
                    quota = { 100.0 - used, resetsAt };
                }
            };
            consider(snapshot.codexLimit->primaryUsedPercent, snapshot.codexLimit->primaryResetsAt);
            consider(snapshot.codexLimit->secondaryUsedPercent, snapshot.codexLimit->secondaryResetsAt);
        }

        auto toolUsage = [&](tokenometer::SurfaceTool tool)
        {
            if (tool == tokenometer::SurfaceTool::Codex)
            {
                return L"Codex 今日 " + FormatCompactTokens(snapshot.today.DisplayTotal()) + L" token";
            }
            return snapshot.chatGptEstimatedTokens > 0
                ? L"ChatGPT 已导入估算 " + FormatCompactTokens(snapshot.chatGptEstimatedTokens) + L" token"
                : std::wstring{ L"ChatGPT 等待导入" };
        };
        auto quotaPercent = [&]()
        {
            return quota.remaining >= 0.0
                ? L"Codex " + std::to_wstring(static_cast<int>(quota.remaining + 0.5)) + L"% 可用"
                : std::wstring{ L"Codex 额度尚未上报" };
        };

        std::wstring result;
        auto append = [&result](std::wstring value)
        {
            if (value.empty()) return;
            if (!result.empty()) result += L"  ·  ";
            result += std::move(value);
        };
        switch (m_surfacePreferences.layoutPreset)
        {
        case tokenometer::SurfaceLayoutPreset::LiveUsage:
            for (auto const& tool : m_surfacePreferences.tools)
            {
                if (tool.visible) append(toolUsage(tool.tool));
            }
            break;
        case tokenometer::SurfaceLayoutPreset::ProviderLimits:
            if (ToolVisible(tokenometer::SurfaceTool::Codex)) append(quotaPercent());
            if (ToolVisible(tokenometer::SurfaceTool::ChatGpt)) append(L"ChatGPT 额度不可用");
            break;
        case tokenometer::SurfaceLayoutPreset::CostFocus:
            append(L"订阅费用不可用");
            break;
        case tokenometer::SurfaceLayoutPreset::Custom:
            for (auto const& item : m_surfacePreferences.customLayout)
            {
                if (!ToolVisible(item.tool) && item.kind != tokenometer::SurfaceLayoutItemKind::CustomText)
                {
                    continue;
                }
                switch (item.kind)
                {
                case tokenometer::SurfaceLayoutItemKind::ToolIcon:
                    append(item.tool == tokenometer::SurfaceTool::Codex ? L"Codex" : L"ChatGPT");
                    break;
                case tokenometer::SurfaceLayoutItemKind::QuotaBar:
                case tokenometer::SurfaceLayoutItemKind::Percentage:
                    append(item.tool == tokenometer::SurfaceTool::Codex
                        ? quotaPercent() : L"ChatGPT 额度不可用");
                    break;
                case tokenometer::SurfaceLayoutItemKind::ResetTime:
                    append(item.tool == tokenometer::SurfaceTool::Codex && quota.resetsAt > 0
                        ? FormatResetCountdown(quota.resetsAt) : L"重置时间不可用");
                    break;
                case tokenometer::SurfaceLayoutItemKind::Cost:
                    append(L"费用不可用");
                    break;
                case tokenometer::SurfaceLayoutItemKind::CustomText:
                    append(item.customText);
                    break;
                }
            }
            break;
        }
        return result.empty() ? std::wstring{ L"未选择显示工具" } : result;
    }

    [[nodiscard]] std::wstring BuildTrayTooltip()
    {
        auto text = BuildSurfaceText();
        if (text.find(L"Codex") == std::wstring::npos)
        {
            BubbleUsageSnapshot snapshot;
            {
                std::scoped_lock lock(m_bubbleSnapshotMutex);
                snapshot = m_bubbleSnapshot;
            }
            text += L"  ·  Codex 今日 " + FormatCompactTokens(snapshot.today.DisplayTotal()) + L" token";
        }
        return L"Tokenometer · " + text;
    }

    void PushSurfacePreferencesView()
    {
        if (!m_dashboard) return;
        tokenometer::SurfacePreferencesViewData data;
        static_cast<tokenometer::SurfacePreferences&>(data) = m_surfacePreferences;
        data.layoutEditorExpanded = m_layoutEditorExpanded;
        data.toolManagerExpanded = m_toolManagerExpanded;
        data.livePreview = BuildSurfaceText();
        m_dashboard->UpdateSurfacePreferences(data);
    }

    void ApplySurfacePreferences()
    {
        auto const theme = m_surfacePreferences.theme;
        if (m_dashboard) m_dashboard->ApplySurfaceTheme(theme);
        if (m_root)
        {
            m_root.RequestedTheme(
                theme == tokenometer::SurfaceTheme::Light
                    ? mux::ElementTheme::Light
                    : theme == tokenometer::SurfaceTheme::Dark
                        ? mux::ElementTheme::Dark
                        : mux::ElementTheme::Default);
        }

        bool const light = theme == tokenometer::SurfaceTheme::Light;
        uint8_t const cardAlpha = static_cast<uint8_t>(
            std::clamp(m_surfacePreferences.glassOpacityPercent, 25, 90) * 255 / 100);
        auto const cardColor = light ? Color(247, 245, 239, cardAlpha) : Color(38, 36, 37, cardAlpha);
        auto const borderColor = light ? Color(0, 0, 0, 24) : Color(255, 255, 255, 18);
        if (m_bubbleBackground)
        {
            m_bubbleBackground.Background(Brush(
                m_surfacePreferences.transparentWindow
                    ? Color(0, 0, 0, 0)
                    : light ? Color(239, 236, 228) : Color(18, 16, 17)));
        }
        for (auto const& card : { m_usageCard, m_resetCard })
        {
            if (!card) continue;
            card.Background(Brush(cardColor));
            card.BorderBrush(Brush(borderColor));
        }
        auto const primaryText = light ? Color(26, 24, 25) : Color(247, 247, 245);
        auto const secondaryText = light ? Color(91, 86, 87) : Color(167, 163, 164);
        for (auto const& text : { m_bubbleTitle, m_bubbleTotal, m_bubbleInput })
        {
            if (text) text.Foreground(Brush(primaryText));
        }
        for (auto const& text : { m_bubbleOutput, m_bubbleCache, m_bubbleUpdated })
        {
            if (text) text.Foreground(Brush(secondaryText));
        }
        auto const providerAccent = m_surfacePreferences.providerColors
            ? Color(98, 223, 125) : Color(154, 150, 151);
        if (m_bubbleFill) m_bubbleFill.Background(Brush(providerAccent));
        if (m_bubbleMarker) m_bubbleMarker.Stroke(Brush(providerAccent));
        if (m_swapChainPanel)
        {
            m_swapChainPanel.Opacity(0.12 + 0.0024 * m_surfacePreferences.glassOpacityPercent);
            m_swapChainPanel.Visibility(
                m_surfacePreferences.blurEnabled && !m_backdropDisabled
                    ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        }
        if (m_bubbleMode && m_hwnd)
        {
            auto presenter = m_window.AppWindow().Presenter().as<windowing::OverlappedPresenter>();
            presenter.IsAlwaysOnTop(m_surfacePreferences.bubbleAlwaysOnTop);
            if (m_surfacePreferences.blurEnabled && !m_backdropDisabled) StartBackdrop();
            else if (m_renderer)
            {
                m_renderer->Stop();
                m_renderer.reset();
                SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
            }
        }
        PushSurfacePreferencesView();
        if (m_trayIcon) (void)m_trayIcon->UpdateTooltip(BuildTrayTooltip());
        if (m_bubbleMode) RefreshBubble();
    }

    void InitializeTrayIcon()
    {
        tokenometer::TrayIcon::Callbacks callbacks;
        callbacks.leftClick = [this]
        {
            m_hoverPreviewActive = false;
            m_hoverRestoreDashboard = false;
            if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            else if (m_bubbleMode) ShowBubbleSurface();
            else ShowDashboardSurface();
        };
        callbacks.doubleClick = [this]
        {
            m_hoverPreviewActive = false;
            m_hoverRestoreDashboard = false;
            ShowDashboardSurface();
        };
        callbacks.openDashboard = [this]
        {
            m_hoverPreviewActive = false;
            m_hoverRestoreDashboard = false;
            ShowDashboardSurface();
        };
        callbacks.showFloatingBubble = [this]
        {
            m_hoverPreviewActive = false;
            m_hoverRestoreDashboard = false;
            if (m_bubbleMode && IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            else ShowBubbleSurface();
        };
        callbacks.hoverChanged = [this](bool entered)
        {
            if (!m_surfacePreferences.hoverPreview) return;
            if (entered && !IsWindowVisible(m_hwnd))
            {
                m_hoverRestoreDashboard = !m_bubbleMode;
                m_hoverPreviewActive = true;
                ShowBubbleSurface(false);
            }
            else if (!entered && m_hoverPreviewActive)
            {
                m_hoverPreviewActive = false;
                if (m_hoverRestoreDashboard) ShowDashboardSurface(false);
                ShowWindow(m_hwnd, SW_HIDE);
            }
        };
        callbacks.exit = [this]
        {
            m_explicitExit = true;
            if (m_trayIcon) m_trayIcon->Remove();
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        };
        m_trayIcon = std::make_unique<tokenometer::TrayIcon>(m_hwnd, std::move(callbacks));
        if (!m_trayIcon->Add(BuildTrayTooltip()))
        {
            m_trayIcon.reset();
            OutputDebugStringW(L"Tokenometer: tray icon is unavailable.\n");
        }
    }

    void ShowDashboardSurface(bool activate = true)
    {
        if (!m_hwnd) return;
        if (m_bubbleMode)
        {
            SaveBubblePosition();
            if (m_statusTimer) m_statusTimer.Stop();
            if (m_renderer)
            {
                m_renderer->Stop();
                m_renderer.reset();
            }
            SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
            m_bubbleMode = false;
            m_window.Content(m_dashboard->Root());
            ConfigureDashboardWindow();
        }
        ShowWindow(m_hwnd, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        if (activate)
        {
            m_window.Activate();
            SetForegroundWindow(m_hwnd);
        }
        RefreshDashboard();
    }

    void ShowBubbleSurface(bool activate = true)
    {
        if (!m_hwnd) return;
        if (!m_bubbleMode)
        {
            m_bubbleMode = true;
            m_window.Content(m_root);
            ConfigureWindow();
            ApplySurfacePreferences();
        }
        ShowWindow(m_hwnd, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        if (activate)
        {
            m_window.Activate();
            SetForegroundWindow(m_hwnd);
        }
        RefreshBubble();
    }

    void SaveBubblePosition() noexcept
    {
        if (!m_database || !m_hwnd || !m_bubbleMode) return;
        RECT bounds{};
        if (!GetWindowRect(m_hwnd, &bounds)) return;
        m_surfacePreferences.hasBubblePosition = true;
        m_surfacePreferences.bubbleX = bounds.left;
        m_surfacePreferences.bubbleY = bounds.top;
        SaveSurfacePreferences();
    }

    void RequestClose()
    {
        if (!m_explicitExit && m_surfacePreferences.closeToTray &&
            m_trayIcon && m_trayIcon->IsAdded())
        {
            ShowWindow(m_hwnd, SW_HIDE);
            return;
        }
        m_explicitExit = true;
        m_window.Close();
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR,
        DWORD_PTR referenceData) noexcept
    {
        auto self = reinterpret_cast<TokenometerApp*>(referenceData);
        if (!self) return DefSubclassProc(window, message, wParam, lParam);
        return self->HandleWindowMessage(window, message, wParam, lParam);
    }

    void InstallWindowProcedure()
    {
        winrt::check_bool(SetWindowSubclass(
            m_hwnd,
            &TokenometerApp::WindowProcedure,
            1,
            reinterpret_cast<DWORD_PTR>(this)));
    }

    LRESULT HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        try
        {
            if (message == WM_QUERYENDSESSION)
            {
                return DefSubclassProc(window, message, wParam, lParam);
            }
            if (message == WM_ENDSESSION && wParam)
            {
                m_explicitExit = true;
                m_trayIcon.reset();
            }
            if (m_trayIcon && m_trayIcon->HandleMessage(message, wParam, lParam)) return 0;
            if (message == WM_CLOSE && !m_explicitExit && m_surfacePreferences.closeToTray &&
                m_trayIcon && m_trayIcon->IsAdded())
            {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            if (message == WM_EXITSIZEMOVE && m_bubbleMode) SaveBubblePosition();
        }
        catch (...)
        {
        }

        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(window, &TokenometerApp::WindowProcedure, 1);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void BeginChatGptImport(std::wstring_view requestedAccountLabel)
    {
        if (m_closing.load(std::memory_order_relaxed) || !m_dashboard ||
            m_chatGptImporting.exchange(true, std::memory_order_relaxed))
        {
            return;
        }

        auto previousImportData = m_chatGptImportData;
        auto accountLabel = NormalizeAccountLabel(requestedAccountLabel);
        m_chatGptImportData = {};
        m_chatGptImportData.state = tokenometer::ChatGptImportState::SelectingFiles;
        m_chatGptImportData.accountLabel = accountLabel;
        m_chatGptImportData.conversations = previousImportData.conversations;
        m_chatGptImportData.estimatedTokens = previousImportData.estimatedTokens;
        m_dashboard->UpdateChatGptImport(m_chatGptImportData);

        ChatGptFileSelection selection;
        try
        {
            selection = PickChatGptExportFiles(m_hwnd, accountLabel);
        }
        catch (...)
        {
            m_chatGptImporting.store(false, std::memory_order_relaxed);
            m_chatGptImportData.state = tokenometer::ChatGptImportState::Failed;
            m_chatGptImportData.errors = 1;
            m_chatGptImportData.message = L"无法打开 Windows 文件选择器。";
            m_dashboard->UpdateChatGptImport(m_chatGptImportData);
            return;
        }

        if (selection.paths.empty())
        {
            m_chatGptImporting.store(false, std::memory_order_relaxed);
            m_chatGptImportData = std::move(previousImportData);
            m_chatGptImportData.accountLabel = accountLabel;
            m_dashboard->UpdateChatGptImport(m_chatGptImportData);
            return;
        }
        accountLabel = NormalizeAccountLabel(selection.accountLabel);
        auto paths = std::move(selection.paths);

        if (!m_database)
        {
            m_chatGptImporting.store(false, std::memory_order_relaxed);
            m_chatGptImportData.state = tokenometer::ChatGptImportState::Failed;
            m_chatGptImportData.errors = 1;
            m_chatGptImportData.message = L"本地数据库不可用，无法导入。";
            m_dashboard->UpdateChatGptImport(m_chatGptImportData);
            return;
        }

        std::vector<std::wstring> displayFiles;
        displayFiles.reserve(paths.size());
        for (auto const& path : paths)
        {
            displayFiles.push_back(path.filename().wstring());
        }
        m_chatGptImportData = {};
        m_chatGptImportData.state = tokenometer::ChatGptImportState::Importing;
        m_chatGptImportData.accountLabel = accountLabel;
        m_chatGptImportData.selectedFiles = displayFiles;
        m_dashboard->UpdateChatGptImport(m_chatGptImportData);

        auto const dispatcher = m_window.DispatcherQueue();
        auto const database = m_database;
        auto const weakThis = get_weak();
        try
        {
            if (m_chatGptImportThread.joinable())
            {
                m_chatGptImportThread.join();
            }
            m_chatGptImportThread = std::jthread([
                weakThis,
                dispatcher,
                database,
                paths = std::move(paths),
                displayFiles = std::move(displayFiles),
                accountLabel](std::stop_token stopToken) mutable
            {
                tokenometer::ChatGptImportViewData completed;
                completed.state = tokenometer::ChatGptImportState::Succeeded;
                completed.accountLabel = accountLabel;
                completed.selectedFiles = std::move(displayFiles);
                int64_t importedFiles{};
                int64_t unchangedFiles{};
                std::wstring details;
                bool apartmentInitialized{};
                bool cancelled{};

                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    apartmentInitialized = true;
                    tokenometer::ChatGPTExportImporter importer(*database);
                    for (auto const& path : paths)
                    {
                        if (stopToken.stop_requested())
                        {
                            cancelled = true;
                            break;
                        }
                        auto const name = path.filename().wstring();
                        try
                        {
                            auto const result = importer.Import(path, accountLabel, stopToken);
                            completed.conversations += result.conversations;
                            completed.estimatedTokens += result.estimatedTokens;
                            completed.skipped += result.skippedConversations;
                            if (result.unchanged)
                            {
                                ++unchangedFiles;
                                details += name + L" · 内容未变更，已跳过\n";
                            }
                            else
                            {
                                ++importedFiles;
                                details += name + L" · 已导入 "
                                    + std::to_wstring(result.conversations) + L" 个会话\n";
                            }
                        }
                        catch (std::system_error const& error)
                        {
                            if (IsCancelled(error))
                            {
                                cancelled = true;
                                break;
                            }
                            ++completed.errors;
                            details += name + L" · 格式无效、文件过大或无法读取\n";
                        }
                        catch (...)
                        {
                            ++completed.errors;
                            details += name + L" · 格式无效、文件过大或无法读取\n";
                        }
                    }
                }
                catch (std::system_error const& error)
                {
                    if (IsCancelled(error)) cancelled = true;
                    else
                    {
                        ++completed.errors;
                        details += L"导入线程无法初始化。\n";
                    }
                }
                catch (...)
                {
                    ++completed.errors;
                    details += L"导入线程无法初始化。\n";
                }
                if (apartmentInitialized) winrt::uninit_apartment();

                if (!cancelled && !stopToken.stop_requested())
                {
                    try
                    {
                        auto const totals = database->GetChatGPTEstimatedTotals(accountLabel);
                        completed.conversations = totals.estimatedSessions;
                        completed.estimatedTokens = totals.estimatedTokens;
                    }
                    catch (...)
                    {
                        ++completed.errors;
                        details += L"无法读取导入后的账户汇总。\n";
                    }
                }

                completed.unchangedFiles = unchangedFiles;
                completed.details = std::move(details);
                if (cancelled || stopToken.stop_requested())
                {
                    completed.state = tokenometer::ChatGptImportState::Failed;
                    completed.message = L"导入已停止；已完成的会话保持有效。";
                }
                else if (completed.errors > 0)
                {
                    completed.state = tokenometer::ChatGptImportState::Failed;
                    completed.message = importedFiles + unchangedFiles > 0
                        ? L"部分文件未导入；已保留成功结果。"
                        : L"没有文件成功导入。";
                }
                else
                {
                    completed.message = L"已处理 " + std::to_wstring(importedFiles + unchangedFiles)
                        + L" 个文件；token 仅为当前分支可见文本估算。";
                }

                bool queued{};
                try
                {
                    queued = dispatcher.TryEnqueue([
                        weakThis,
                        completed = std::move(completed)]() mutable
                    {
                        if (auto self = weakThis.get())
                        {
                            if (self->m_closing.load(std::memory_order_relaxed)) return;
                            self->m_chatGptImporting.store(false, std::memory_order_relaxed);
                            self->m_chatGptImportData = std::move(completed);
                            if (self->m_dashboard)
                            {
                                self->m_dashboard->UpdateChatGptImport(self->m_chatGptImportData);
                                self->RefreshDashboard();
                            }
                        }
                    });
                }
                catch (...)
                {
                }
                if (!queued)
                {
                    if (auto self = weakThis.get())
                    {
                        self->m_chatGptImporting.store(false, std::memory_order_relaxed);
                    }
                }
            });
        }
        catch (...)
        {
            m_chatGptImporting.store(false, std::memory_order_relaxed);
            m_chatGptImportData.state = tokenometer::ChatGptImportState::Failed;
            m_chatGptImportData.errors = 1;
            m_chatGptImportData.message = L"无法启动 ChatGPT 导入线程。";
            m_dashboard->UpdateChatGptImport(m_chatGptImportData);
        }
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

        auto const render = [](tokenometer::ToolCallContent const& content)
        {
            std::wstring details;
            details.reserve(std::min<size_t>(
                content.input.size() + content.output.size() + 32,
                32 * 1024 + 32));
            details += L"输入\n";
            details += content.input.empty() ? L"（源记录未提供输入）" : ClipToolContent(content.input);
            details += L"\n\n输出\n";
            details += content.output.empty() ? L"（尚未找到输出记录）" : ClipToolContent(content.output);
            return details;
        };

        if (!match->second.sourcePath.starts_with(L"wsl://"))
        {
            try
            {
                tokenometer::SourceContentReader reader;
                m_selectedToolDetails = render(reader.Read(match->second));
            }
            catch (...)
            {
                m_selectedToolDetails = L"无法安全读取该工具记录；源文件可能已移动、被裁剪或不在允许目录。";
            }
            return;
        }

        if (m_toolContentThread.joinable())
        {
            m_toolContentThread.request_stop();
            m_toolContentThread.join();
        }
        m_selectedToolDetails = L"正在从运行中的 WSL 发行版读取已选工具记录…";
        auto weakThis = get_weak();
        auto dispatcher = m_window.DispatcherQueue();
        auto const tool = match->second;
        try
        {
            m_toolContentThread = std::jthread([
                weakThis,
                dispatcher,
                locator,
                tool,
                render](std::stop_token stopToken)
            {
                std::wstring details;
                bool apartmentInitialized{};
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    apartmentInitialized = true;
                    tokenometer::SourceContentReader reader;
                    details = render(reader.Read(tool, stopToken));
                }
                catch (...)
                {
                    details = stopToken.stop_requested()
                        ? L"已取消读取 WSL 工具记录。"
                        : L"无法安全读取 WSL 工具记录；发行版可能已停止或源文件已移动。";
                }
                if (apartmentInitialized) winrt::uninit_apartment();
                if (stopToken.stop_requested()) return;
                try
                {
                    dispatcher.TryEnqueue([
                        weakThis,
                        locator,
                        details = std::move(details)]() mutable
                    {
                        if (auto self = weakThis.get())
                        {
                            if (self->m_closing.load(std::memory_order_relaxed) ||
                                self->m_selectedToolLocator != locator)
                            {
                                return;
                            }
                            self->m_selectedToolDetails = std::move(details);
                            self->RefreshDetails();
                        }
                    });
                }
                catch (...)
                {
                }
            });
        }
        catch (...)
        {
            m_selectedToolDetails = L"无法启动 WSL 工具记录读取线程。";
        }
    }

    void RefreshDetails()
    {
        if (!m_dashboard)
        {
            return;
        }

        tokenometer::DetailsViewData data;
        data.scope = m_detailsScope;
        data.dimension = m_detailsDimension;
        data.selectedKey = m_selectedBreakdownKey;
        data.selectedSessionId = m_selectedSessionId;
        data.selectedSessionAccountId = m_selectedSessionAccountId;
        data.selectedSessionSourceKind = m_selectedSessionSourceKind;
        data.selectedToolCallLocator = m_selectedToolLocator;
        data.selectedToolDetails = m_selectedToolDetails;
        data.loading = m_detailsScope == tokenometer::UsageScope::CodexExact
            ? m_collecting.load(std::memory_order_relaxed)
            : m_chatGptImporting.load(std::memory_order_relaxed);
        m_toolLocators.clear();

        if (!m_database)
        {
            data.error = L"本地使用数据库不可用";
            m_dashboard->UpdateDetails(data);
            return;
        }

        try
        {
            if (m_detailsScope == tokenometer::UsageScope::ChatGptEstimated)
            {
                if (m_detailsDimension == tokenometer::DetailsDimension::Device ||
                    m_detailsDimension == tokenometer::DetailsDimension::Project)
                {
                    data.unavailableReason =
                        L"ChatGPT 官方导出不包含设备或项目归属；这些维度仅适用于 Codex 精确记录。";
                }
                else
                {
                    data.rows = m_database->GetChatGPTEstimatedBreakdown(
                        DetailsDimensionKey(m_detailsDimension), 0, 12);
                }

                for (auto const& session : m_database->GetChatGPTEstimatedSessions({}, 3))
                {
                    tokenometer::SessionSummary summary;
                    summary.id = session.id;
                    summary.title = session.accountId.empty() ? session.id : session.accountId;
                    summary.model = session.model;
                    summary.startedAt = session.startedAt;
                    summary.updatedAt = session.updatedAt;
                    summary.messages = session.messages;
                    summary.counts.input = session.estimatedInputTokens;
                    summary.counts.output = session.estimatedOutputTokens;
                    summary.counts.reportedTotal = session.EstimatedTokens();
                    summary.accountId = session.accountId;
                    summary.sourceKind = session.sourceKind;
                    summary.measurement = tokenometer::MeasurementKind::Estimated;
                    data.recentSessions.push_back(std::move(summary));
                }

                if (!m_selectedSessionId.empty() && !m_selectedSessionAccountId.empty())
                {
                    for (auto const& prompt : m_database->GetChatGPTEstimatedPrompts(
                             m_selectedSessionAccountId, m_selectedSessionId, 8))
                    {
                        tokenometer::TurnSummary turn;
                        turn.sessionId = prompt.sessionId;
                        turn.turnId = prompt.turnId;
                        turn.promptIndex = prompt.promptIndex;
                        turn.timestamp = prompt.timestamp;
                        turn.model = prompt.model;
                        turn.counts.input = prompt.estimatedInputTokens;
                        turn.counts.output = prompt.estimatedOutputTokens;
                        turn.counts.reportedTotal = prompt.EstimatedTokens();
                        turn.measurement = tokenometer::MeasurementKind::Estimated;
                        data.selectedTurns.push_back(std::move(turn));
                    }
                }
            }
            else
            {
                data.rows = m_database->GetBreakdown(DetailsDimensionKey(m_detailsDimension), 0, 12);
                if (m_detailsDimension == tokenometer::DetailsDimension::Device)
                {
                    auto const devices = m_database->GetDeviceSummaries(100);
                    for (auto& row : data.rows)
                    {
                        auto const device = std::find_if(devices.begin(), devices.end(), [&](auto const& item)
                        {
                            return item.id == row.key;
                        });
                        if (device != devices.end())
                        {
                            auto const kind = device->kind == tokenometer::DeviceKind::Wsl
                                ? L"WSL"
                                : L"Windows";
                            auto const shortId = device->id.size() > 8
                                ? device->id.substr(device->id.size() - 8)
                                : device->id;
                            row.displayName = (device->displayName.empty() ? device->id : device->displayName) +
                                L" · " + kind + L" · " + shortId;
                        }
                    }
                }
                data.recentSessions = m_database->GetRecentSessions(3);
                if (!m_selectedSessionId.empty())
                {
                    data.selectedTurns = m_database->GetSessionTurns(m_selectedSessionId, 8);
                    size_t toolIndex{};
                    for (auto const& turn : data.selectedTurns)
                    {
                        for (auto const& tool : m_database->GetToolCalls(m_selectedSessionId, turn.promptIndex))
                        {
                            if (toolIndex >= 20) break;
                            std::wstring locator = L"tool:" + m_selectedSessionId + L":" +
                                std::to_wstring(tool.inputOffset);
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
                        if (toolIndex >= 20) break;
                    }
                    if (!m_selectedToolLocator.empty() && std::ranges::none_of(
                            m_toolLocators,
                            [this](auto const& item)
                            {
                                return item.first == m_selectedToolLocator;
                            }))
                    {
                        if (m_toolContentThread.joinable()) m_toolContentThread.request_stop();
                        m_selectedToolLocator.clear();
                        m_selectedToolDetails.clear();
                        data.selectedToolCallLocator.clear();
                        data.selectedToolDetails.clear();
                    }
                }
            }

            if (m_detailsScope == tokenometer::UsageScope::ChatGptEstimated &&
                (!m_selectedToolLocator.empty() || !m_selectedToolDetails.empty()))
            {
                if (m_toolContentThread.joinable()) m_toolContentThread.request_stop();
                m_selectedToolLocator.clear();
                m_selectedToolDetails.clear();
                data.selectedToolCallLocator.clear();
                data.selectedToolDetails.clear();
            }
            if (!m_selectedSessionId.empty() && std::ranges::none_of(
                    data.recentSessions,
                    [this](auto const& session)
                    {
                        return session.id == m_selectedSessionId &&
                            session.accountId == m_selectedSessionAccountId &&
                            session.sourceKind == m_selectedSessionSourceKind;
                    }))
            {
                if (m_detailsScope == tokenometer::UsageScope::ChatGptEstimated)
                {
                    auto const selected = m_database->GetChatGPTEstimatedSessions(
                        m_selectedSessionAccountId, 100);
                    auto const exists = std::ranges::any_of(selected, [this](auto const& session)
                    {
                        return session.id == m_selectedSessionId;
                    });
                    if (!exists)
                    {
                        m_selectedSessionId.clear();
                        m_selectedSessionAccountId.clear();
                        m_selectedSessionSourceKind.clear();
                        data.selectedSessionId.clear();
                        data.selectedSessionAccountId.clear();
                        data.selectedSessionSourceKind.clear();
                        data.selectedTurns.clear();
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
        data.scope = m_trendScope;
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
            std::vector<tokenometer::DailyUsage> daily;
            std::vector<tokenometer::HourlyUsage> hourly;
            if (m_trendScope == tokenometer::UsageScope::ChatGptEstimated)
            {
                for (auto const& estimate : m_database->GetChatGPTEstimatedDailyUsage(365))
                {
                    tokenometer::DailyUsage row;
                    row.day = estimate.day;
                    row.sourceKind = estimate.sourceKind;
                    row.tool = estimate.tool;
                    row.model = estimate.model.empty() ? L"unclassified" : estimate.model;
                    row.counts.input = estimate.estimatedInputTokens;
                    row.counts.output = estimate.estimatedOutputTokens;
                    row.counts.reportedTotal = estimate.EstimatedTokens();
                    row.messages = estimate.messages;
                    row.accountId = estimate.accountId;
                    daily.push_back(std::move(row));
                }
                if (m_trendChart == tokenometer::TrendChart::Kline)
                {
                    for (auto const& estimate : m_database->GetChatGPTEstimatedHourlyUsage(rangeDays))
                    {
                        tokenometer::HourlyUsage row;
                        row.hourStart = estimate.hourStart;
                        row.day = estimate.day;
                        row.sourceKind = estimate.sourceKind;
                        row.tool = estimate.tool;
                        row.model = estimate.model.empty() ? L"unclassified" : estimate.model;
                        row.counts.input = estimate.estimatedInputTokens;
                        row.counts.output = estimate.estimatedOutputTokens;
                        row.counts.reportedTotal = estimate.EstimatedTokens();
                        row.messages = estimate.messages;
                        row.accountId = estimate.accountId;
                        hourly.push_back(std::move(row));
                    }
                }
            }
            else
            {
                daily = m_database->GetDailyUsage(365);
                if (m_trendChart == tokenometer::TrendChart::Kline)
                {
                    hourly = m_database->GetHourlyUsage(rangeDays);
                }
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
            data.loading = (m_trendScope == tokenometer::UsageScope::CodexExact
                ? m_collecting.load(std::memory_order_relaxed)
                : m_chatGptImporting.load(std::memory_order_relaxed)) && data.series.empty();
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

        m_bubbleTitle = Text(L"Codex 今日 Token", 15, Color(247, 247, 245), 600);
        Place(m_bubbleTitle, 64, 25);
        m_root.Children().Append(m_bubbleTitle);

        m_bubbleTotal = Text(L"—", 32, Color(247, 247, 245), 650);
        m_bubbleTotal.Width(150);
        m_bubbleTotal.TextAlignment(mux::TextAlignment::Right);
        Place(m_bubbleTotal, 242, 14);
        m_root.Children().Append(m_bubbleTotal);

        controls::Border track;
        track.Width(364);
        track.Height(10);
        track.CornerRadius(Radius(5));
        track.Background(Brush(Color(52, 49, 50)));
        track.BorderBrush(Brush(Color(255, 255, 255, 20)));
        track.BorderThickness({ 0.5 });
        Place(track, 28, 70);
        m_root.Children().Append(track);

        m_bubbleFill = controls::Border{};
        m_bubbleFill.Width(0);
        m_bubbleFill.Height(8);
        m_bubbleFill.CornerRadius(Radius(4));
        m_bubbleFill.Background(Brush(Color(98, 223, 125)));
        Place(m_bubbleFill, 29, 71);
        m_root.Children().Append(m_bubbleFill);

        m_bubbleMarker = shapes::Ellipse{};
        m_bubbleMarker.Width(12);
        m_bubbleMarker.Height(12);
        m_bubbleMarker.Fill(Brush(Color(38, 36, 37)));
        m_bubbleMarker.Stroke(Brush(Color(98, 223, 125)));
        m_bubbleMarker.StrokeThickness(3);
        Place(m_bubbleMarker, 23, 69);
        m_root.Children().Append(m_bubbleMarker);

        m_bubbleInput = Text(L"输入 —", 11.5, Color(247, 247, 245), 600);
        Place(m_bubbleInput, 28, 91);
        m_root.Children().Append(m_bubbleInput);

        m_bubbleOutput = Text(L"输出 —", 11.5, Color(167, 163, 164));
        Place(m_bubbleOutput, 125, 91);
        m_root.Children().Append(m_bubbleOutput);

        m_bubbleCache = Text(L"缓存 —", 11.5, Color(167, 163, 164));
        m_bubbleCache.Width(110);
        m_bubbleCache.TextAlignment(mux::TextAlignment::Right);
        Place(m_bubbleCache, 282, 91);
        m_root.Children().Append(m_bubbleCache);

        auto resetIcon = Icon(L"⌛", Color(255, 253, 142), 12, Color(38, 36, 37));
        Place(resetIcon, 28, 160);
        m_root.Children().Append(resetIcon);

        m_bubbleLimitTitle = Text(L"Codex 额度", 15, Color(247, 247, 245), 600);
        Place(m_bubbleLimitTitle, 64, 158);
        m_root.Children().Append(m_bubbleLimitTitle);

        m_bubbleLimit = Text(L"—", 24, Color(255, 253, 142), 600);
        m_bubbleLimit.Width(110);
        m_bubbleLimit.TextAlignment(mux::TextAlignment::Right);
        Place(m_bubbleLimit, 282, 150);
        m_root.Children().Append(m_bubbleLimit);

        auto refresh = Text(L"↻", 10, Color(133, 129, 130), 600);
        Place(refresh, 28, 215);
        m_root.Children().Append(refresh);

        m_bubbleUpdated = Text(L"等待首次同步", 9.5, Color(133, 129, 130));
        Place(m_bubbleUpdated, 43, 215);
        m_root.Children().Append(m_bubbleUpdated);

        m_closeButton = controls::Border{};
        m_closeButton.Width(22);
        m_closeButton.Height(22);
        m_closeButton.Background(Brush(Color(0, 0, 0, 0)));
        m_closeButton.BorderThickness({ 0 });
        m_closeButton.Opacity(0);

        shapes::Ellipse dot;
        dot.Width(8);
        dot.Height(8);
        dot.Fill(Brush(Color(240, 63, 22)));
        dot.HorizontalAlignment(mux::HorizontalAlignment::Center);
        dot.VerticalAlignment(mux::VerticalAlignment::Center);
        m_closeButton.Child(dot);
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
        presenter.IsAlwaysOnTop(m_surfacePreferences.bubbleAlwaysOnTop);
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

        POINT desired{ m_surfacePreferences.bubbleX, m_surfacePreferences.bubbleY };
        HMONITOR monitor = m_surfacePreferences.hasBubblePosition
            ? MonitorFromPoint(desired, MONITOR_DEFAULTTONEAREST)
            : MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof(info) };
        winrt::check_bool(GetMonitorInfoW(monitor, &info));
        int const maxX = std::max(info.rcWork.left, info.rcWork.right - width);
        int const maxY = std::max(info.rcWork.top, info.rcWork.bottom - height);
        int const x = m_surfacePreferences.hasBubblePosition
            ? std::clamp(m_surfacePreferences.bubbleX, static_cast<int>(info.rcWork.left), maxX)
            : info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
        int const y = m_surfacePreferences.hasBubblePosition
            ? std::clamp(m_surfacePreferences.bubbleY, static_cast<int>(info.rcWork.top), maxY)
            : info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2;
        winrt::check_bool(SetWindowPos(
            m_hwnd,
            m_surfacePreferences.bubbleAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
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
        presenter.SetBorderAndTitleBar(true, true);
        presenter.IsResizable(true);
        presenter.IsMaximizable(true);
        presenter.IsMinimizable(true);
        presenter.IsAlwaysOnTop(false);
        appWindow.IsShownInSwitchers(true);
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_dashboardStyle);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_dashboardExtendedStyle);
        SetWindowRgn(m_hwnd, nullptr, TRUE);
        SetWindowDisplayAffinity(m_hwnd, WDA_NONE);

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
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED));
    }

    void StopBackgroundWorkers() noexcept
    {
        m_closing.store(true, std::memory_order_relaxed);
        auto stop = [](std::jthread& worker) noexcept
        {
            if (!worker.joinable()) return;
            worker.request_stop();
            try
            {
                if (worker.get_id() == std::this_thread::get_id()) worker.detach();
                else worker.join();
            }
            catch (...)
            {
                if (worker.joinable()) worker.detach();
            }
        };
        stop(m_chatGptImportThread);
        stop(m_toolContentThread);
        stop(m_wslCollectionThread);
        stop(m_collectionThread);
    }

    void WireInteractions()
    {
        if (m_closeButton)
        {
            m_closeButton.PointerPressed([this](auto const&, input::PointerRoutedEventArgs const& args)
            {
                RequestClose();
                args.Handled(true);
            });
            m_root.PointerEntered([this](auto const&, auto const&) { m_closeButton.Opacity(1); });
            m_root.PointerExited([this](auto const&, auto const&) { m_closeButton.Opacity(0); });
        }
        m_root.PointerPressed([this](auto const&, input::PointerRoutedEventArgs const& args)
        {
            if (!m_bubbleMode) return;
            auto point = args.GetCurrentPoint(m_root);
            if (point.Properties().IsLeftButtonPressed())
            {
                ReleaseCapture();
                SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                args.Handled(true);
            }
        });

        m_window.Closed([this](auto const&, auto const&)
        {
            m_explicitExit = true;
            m_trayIcon.reset();
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
            StopBackgroundWorkers();
        });
    }

    void StartCollection()
    {
        if (!m_database)
        {
            m_collectionFailed.store(true, std::memory_order_relaxed);
            return;
        }
        try
        {
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
                            UpdateBubbleSnapshotCache();
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
                        m_initialLocalCollectionComplete.store(true, std::memory_order_release);
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
                    m_initialLocalCollectionComplete.store(true, std::memory_order_release);
                    OutputDebugStringW(L"Tokenometer: collector thread initialization failed.\n");
                }
            });

            try
            {
                m_wslCollector = std::make_unique<tokenometer::WslCodexCollector>(*m_database);
                m_wslCollectionThread = std::jthread([this](std::stop_token stopToken)
                {
                    try
                    {
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        while (!stopToken.stop_requested() &&
                               !m_initialLocalCollectionComplete.load(std::memory_order_acquire))
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        while (!stopToken.stop_requested())
                        {
                            try
                            {
                                m_wslCollecting.store(true, std::memory_order_relaxed);
                                auto const wslResult = m_wslCollector->CollectOnce(stopToken);
                                UpdateBubbleSnapshotCache();
                                bool const failed = !wslResult.discoverySucceeded ||
                                    wslResult.errors > 0;
                                m_wslCollectionFailed.store(failed, std::memory_order_relaxed);
                                if (failed)
                                {
                                    OutputDebugStringW(
                                        L"Tokenometer: WSL discovery or collection is unavailable.\n");
                                }
                            }
                            catch (...)
                            {
                                m_wslCollectionFailed.store(true, std::memory_order_relaxed);
                                OutputDebugStringW(L"Tokenometer: WSL usage collection failed.\n");
                            }
                            m_wslCollecting.store(false, std::memory_order_relaxed);
                            for (int tick = 0; tick < 3'000 && !stopToken.stop_requested(); ++tick)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }
                        winrt::uninit_apartment();
                    }
                    catch (...)
                    {
                        m_wslCollecting.store(false, std::memory_order_relaxed);
                        m_wslCollectionFailed.store(true, std::memory_order_relaxed);
                        OutputDebugStringW(L"Tokenometer: WSL collector thread initialization failed.\n");
                    }
                });
            }
            catch (...)
            {
                m_wslCollectionFailed.store(true, std::memory_order_relaxed);
                OutputDebugStringW(L"Tokenometer: WSL collector initialization failed.\n");
            }
        }
        catch (...)
        {
            m_collectionFailed.store(true, std::memory_order_relaxed);
            OutputDebugStringW(L"Tokenometer: local database initialization failed.\n");
        }
    }

    void UpdateBubbleSnapshotCache() noexcept
    {
        if (!m_database) return;
        try
        {
            BubbleUsageSnapshot next;
            for (auto const& row : m_database->GetDailyUsage(1))
            {
                next.today.input += row.counts.input;
                next.today.cachedInput += row.counts.cachedInput;
                next.today.cacheWriteInput += row.counts.cacheWriteInput;
                next.today.output += row.counts.output;
                next.today.reasoningOutput += row.counts.reasoningOutput;
                next.today.reportedTotal += row.counts.reportedTotal;
            }
            next.codexLimit = m_database->GetLatestRateLimit();
            auto const chatGpt = m_database->GetChatGPTEstimatedTotals();
            next.chatGptEstimatedTokens = chatGpt.estimatedTokens;
            next.chatGptEstimatedSessions = chatGpt.estimatedSessions;
            next.refreshedAt = UnixNow();
            next.available = true;
            std::scoped_lock lock(m_bubbleSnapshotMutex);
            m_bubbleSnapshot = std::move(next);
            m_bubbleSnapshotFailed.store(false, std::memory_order_relaxed);
        }
        catch (...)
        {
            m_bubbleSnapshotFailed.store(true, std::memory_order_relaxed);
        }
    }

    void StartDashboardRefresh()
    {
        if (m_usageTimer) return;
        if (!m_bubbleMode && !m_dashboard)
        {
            return;
        }
        m_usageTimer = mux::DispatcherTimer{};
        m_usageTimer.Interval(std::chrono::seconds(1));
        m_usageTimer.Tick([this](auto const&, auto const&)
        {
            UpdateBubbleSnapshotCache();
            if (m_bubbleMode) RefreshBubble();
            else RefreshDashboard();
            PushSurfacePreferencesView();
            if (m_trayIcon)
            {
                (void)m_trayIcon->UpdateTooltip(BuildTrayTooltip());
            }
        });
        if (m_bubbleMode) RefreshBubble();
        else RefreshDashboard();
        m_usageTimer.Start();
    }

    void RefreshBubble()
    {
        if (!m_bubbleMode || !m_bubbleTotal)
        {
            return;
        }

        auto setUnavailable = [this](std::wstring_view message)
        {
            m_bubbleTotal.Text(L"—");
            m_bubbleInput.Text(L"输入 —");
            m_bubbleOutput.Text(L"输出 —");
            m_bubbleCache.Text(L"缓存 —");
            m_bubbleFill.Width(0);
            controls::Canvas::SetLeft(m_bubbleMarker, 23);
            m_bubbleLimitTitle.Text(L"同步状态");
            m_bubbleLimit.Text(L"异常");
            m_bubbleLimit.Foreground(Brush(Color(240, 63, 22)));
            m_bubbleUpdated.Text(message);
        };
        if (!m_database)
        {
            setUnavailable(L"本地数据暂不可用");
            return;
        }

        try
        {
            BubbleUsageSnapshot snapshot;
            {
                std::scoped_lock lock(m_bubbleSnapshotMutex);
                snapshot = m_bubbleSnapshot;
            }
            if (!snapshot.available)
            {
                if (m_collectionFailed.load(std::memory_order_relaxed) ||
                    m_bubbleSnapshotFailed.load(std::memory_order_relaxed))
                {
                    setUnavailable(L"首次索引失败，正在重试");
                    m_bubbleLimit.Text(L"重试中");
                    return;
                }
                m_bubbleLimitTitle.Text(L"同步状态");
                m_bubbleLimit.Text(L"等待中");
                m_bubbleLimit.Foreground(Brush(Color(255, 253, 142)));
                m_bubbleUpdated.Text(L"等待首次索引完成");
                return;
            }
            auto const primaryTool = PrimarySurfaceTool();
            if (!primaryTool)
            {
                m_bubbleTitle.Text(L"未选择显示工具");
                m_bubbleTotal.Text(L"—");
                m_bubbleInput.Text(L"请在设置中启用工具");
                m_bubbleOutput.Text(L"");
                m_bubbleCache.Text(L"");
                m_bubbleFill.Width(0);
                m_bubbleLimitTitle.Text(L"表面布局");
                m_bubbleLimit.Text(L"已隐藏");
                m_bubbleUpdated.Text(L"托盘与气泡共用工具顺序");
                return;
            }
            if (*primaryTool == tokenometer::SurfaceTool::ChatGpt)
            {
                m_bubbleTitle.Text(L"ChatGPT 官方导出估算");
                m_bubbleTotal.Text(FormatCompactTokens(snapshot.chatGptEstimatedTokens));
                m_bubbleInput.Text(L"已导入会话 " + std::to_wstring(snapshot.chatGptEstimatedSessions));
                m_bubbleOutput.Text(L"非实时");
                m_bubbleCache.Text(L"缓存不可用");
                m_bubbleFill.Width(0);
                controls::Canvas::SetLeft(m_bubbleMarker, 23);
                m_bubbleLimitTitle.Text(L"ChatGPT 额度");
                m_bubbleLimit.Text(L"不可用");
                m_bubbleLimit.Foreground(Brush(Color(143, 139, 140)));
                m_bubbleUpdated.Text(L"官方 JSON 导出不包含实时 token、缓存或订阅费用");
                return;
            }
            m_bubbleTitle.Text(L"Codex 今日 Token");
            auto const& today = snapshot.today;

            double const cachePercent = today.input > 0
                ? std::clamp(100.0 * static_cast<double>(today.cachedInput) /
                    static_cast<double>(today.input), 0.0, 100.0)
                : 0.0;
            double const fillWidth = 362.0 * cachePercent / 100.0;
            m_bubbleTotal.Text(FormatCompactTokens(today.DisplayTotal()));
            m_bubbleInput.Text(L"输入 " + FormatCompactTokens(today.input));
            m_bubbleOutput.Text(L"输出 " + FormatCompactTokens(today.output));
            m_bubbleCache.Text(
                L"缓存 " + std::to_wstring(static_cast<int>(cachePercent + 0.5)) + L"%");
            m_bubbleFill.Width(fillWidth);
            controls::Canvas::SetLeft(m_bubbleMarker, 23 + fillWidth);

            m_bubbleLimit.Foreground(Brush(Color(255, 253, 142)));
            auto const& limit = snapshot.codexLimit;
            if (limit)
            {
                struct LimitWindow
                {
                    double used{-1.0};
                    int minutes{};
                    int64_t resetsAt{};
                } selected;
                int64_t const now = UnixNow();
                auto consider = [&](double used, int minutes, int64_t resetsAt)
                {
                    if (used >= 0.0 && used <= 100.0 && resetsAt > now && used > selected.used)
                    {
                        selected = {used, minutes, resetsAt};
                    }
                };
                consider(
                    limit->primaryUsedPercent,
                    limit->primaryWindowMinutes,
                    limit->primaryResetsAt);
                consider(
                    limit->secondaryUsedPercent,
                    limit->secondaryWindowMinutes,
                    limit->secondaryResetsAt);
                if (selected.used >= 0.0)
                {
                    double const remaining = std::clamp(100.0 - selected.used, 0.0, 100.0);
                    m_bubbleLimitTitle.Text(L"Codex " + FormatQuotaWindow(selected.minutes));
                    m_bubbleLimit.Text(
                        std::to_wstring(static_cast<int>(remaining + 0.5)) + L"%");
                    std::wstring detail = FormatResetCountdown(selected.resetsAt);
                    int64_t const age = limit->capturedAt > 0
                        ? std::max<int64_t>(now - limit->capturedAt, 0)
                        : 0;
                    if (age >= 300)
                    {
                        detail += L" · " + std::to_wstring(age / 60) + L"分钟前记录";
                    }
                    m_bubbleUpdated.Text(detail);
                }
                else
                {
                    m_bubbleLimitTitle.Text(L"同步状态");
                    m_bubbleLimit.Text(L"实时");
                    m_bubbleUpdated.Text(L"额度窗口尚未上报");
                }
            }
            else
            {
                m_bubbleLimitTitle.Text(L"同步状态");
                bool const collecting = m_collecting.load(std::memory_order_relaxed) ||
                    m_wslCollecting.load(std::memory_order_relaxed);
                m_bubbleLimit.Text(collecting ? L"同步中" : L"实时");
                int64_t const lastSync = m_lastCollectionAt.load(std::memory_order_relaxed);
                int64_t const age = lastSync > 0 ? std::max<int64_t>(UnixNow() - lastSync, 0) : -1;
                if (age < 0) m_bubbleUpdated.Text(L"等待首次同步");
                else if (age < 5) m_bubbleUpdated.Text(L"刚刚更新");
                else if (age < 60) m_bubbleUpdated.Text(std::to_wstring(age) + L" 秒前更新");
                else m_bubbleUpdated.Text(std::to_wstring(age / 60) + L" 分钟前更新");
            }
            int64_t const snapshotAge = std::max<int64_t>(
                UnixNow() - snapshot.refreshedAt,
                0);
            if (m_collectionFailed.load(std::memory_order_relaxed))
            {
                m_bubbleLimitTitle.Text(L"同步状态");
                m_bubbleLimit.Text(L"重试中");
                m_bubbleLimit.Foreground(Brush(Color(240, 63, 22)));
                m_bubbleUpdated.Text(L"保留上次成功数据");
            }
            else if (m_bubbleSnapshotFailed.load(std::memory_order_relaxed) ||
                     snapshotAge > 15)
            {
                m_bubbleLimitTitle.Text(L"快照状态");
                m_bubbleLimit.Text(L"重试中");
                m_bubbleLimit.Foreground(Brush(Color(240, 63, 22)));
                m_bubbleUpdated.Text(
                    snapshotAge < 60
                        ? std::to_wstring(snapshotAge) + L" 秒前的数据"
                        : std::to_wstring(snapshotAge / 60) + L" 分钟前的数据");
            }
            else if (m_wslCollectionFailed.load(std::memory_order_relaxed))
            {
                std::wstring status{m_bubbleUpdated.Text()};
                status += L" · WSL 暂不可用";
                m_bubbleUpdated.Text(status);
            }
            if (m_surfacePreferences.layoutPreset == tokenometer::SurfaceLayoutPreset::CostFocus)
            {
                m_bubbleLimitTitle.Text(L"订阅费用");
                m_bubbleLimit.Text(L"不可用");
                m_bubbleLimit.Foreground(Brush(Color(143, 139, 140)));
                m_bubbleUpdated.Text(L"本地记录不提供可靠费用，不显示估算金额");
            }
        }
        catch (...)
        {
            setUnavailable(L"读取实时快照失败");
        }
    }

    void RefreshDashboard()
    {
        if (!m_dashboard)
        {
            return;
        }
        tokenometer::OverviewViewData snapshot;
        snapshot.collecting = m_collecting.load(std::memory_order_relaxed) ||
            m_wslCollecting.load(std::memory_order_relaxed);
        snapshot.lastSync = m_lastCollectionAt.load(std::memory_order_relaxed);
        if (m_wslCollectionFailed.load(std::memory_order_relaxed))
        {
            snapshot.warning =
                L"WSL 暂不可用或部分发行版扫描失败；Windows 数据仍在实时更新";
        }
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
            snapshot.chatGptTotals = m_database->GetChatGPTEstimatedTotals();
            snapshot.codexLimit = m_database->GetLatestRateLimit();

            for (auto const& summary : m_database->GetDeviceSummaries(4))
            {
                tokenometer::DeviceViewData device;
                device.summary = summary;
                auto const isWsl = summary.kind == tokenometer::DeviceKind::Wsl;
                auto const syncing = isWsl
                    ? m_wslCollecting.load(std::memory_order_relaxed)
                    : m_collecting.load(std::memory_order_relaxed);
                auto const failed = isWsl
                    ? m_wslCollectionFailed.load(std::memory_order_relaxed)
                    : m_collectionFailed.load(std::memory_order_relaxed);
                if (syncing)
                {
                    device.state = tokenometer::DeviceSyncState::Syncing;
                    device.statusText = isWsl ? L"正在合并 WSL 数据" : L"正在扫描本机记录";
                }
                else if (failed)
                {
                    device.state = tokenometer::DeviceSyncState::Warning;
                    device.statusText = isWsl
                        ? L"本轮 WSL 扫描有错误 · 保留上次数据"
                        : L"本轮扫描有错误 · 保留上次数据";
                }
                else if (summary.lastSeen > 0)
                {
                    device.state = tokenometer::DeviceSyncState::Synced;
                    auto const age = std::max<int64_t>(UnixNow() - summary.lastSeen, 0);
                    if (age < 60) device.statusText = L"刚刚同步";
                    else if (age < 3600) device.statusText = std::to_wstring(age / 60) + L" 分钟前同步";
                    else device.statusText = std::to_wstring(age / 3600) + L" 小时前同步";
                }
                else
                {
                    device.state = tokenometer::DeviceSyncState::Never;
                    device.statusText = L"等待首次同步";
                }
                snapshot.devices.push_back(std::move(device));
            }

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
        try
        {
            auto const totals = m_database->GetChatGPTEstimatedTotals(
                m_chatGptImportData.accountLabel);
            m_chatGptImportData.conversations = totals.estimatedSessions;
            m_chatGptImportData.estimatedTokens = totals.estimatedTokens;
            m_dashboard->UpdateChatGptImport(m_chatGptImportData);
        }
        catch (...)
        {
        }
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
        if (m_renderer || m_backdropDisabled || !m_bubbleMode ||
            !m_surfacePreferences.blurEnabled || !m_swapChainPanel)
        {
            return;
        }
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

        if (m_statusTimer) m_statusTimer.Stop();
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
    controls::Border m_bubbleBackground{ nullptr };
    controls::Border m_usageCard{ nullptr };
    controls::Border m_resetCard{ nullptr };
    controls::Border m_closeButton{ nullptr };
    controls::TextBlock m_bubbleTitle{ nullptr };
    controls::TextBlock m_bubbleTotal{ nullptr };
    controls::TextBlock m_bubbleInput{ nullptr };
    controls::TextBlock m_bubbleOutput{ nullptr };
    controls::TextBlock m_bubbleCache{ nullptr };
    controls::TextBlock m_bubbleLimitTitle{ nullptr };
    controls::TextBlock m_bubbleLimit{ nullptr };
    controls::TextBlock m_bubbleUpdated{ nullptr };
    controls::Border m_bubbleFill{ nullptr };
    shapes::Ellipse m_bubbleMarker{ nullptr };
    std::unique_ptr<tokenometer::DashboardView> m_dashboard;
    mux::DispatcherTimer m_statusTimer{ nullptr };
    mux::DispatcherTimer m_usageTimer{ nullptr };
    std::shared_ptr<CaptureRenderer> m_renderer;
    std::shared_ptr<tokenometer::Database> m_database;
    std::unique_ptr<tokenometer::TrayIcon> m_trayIcon;
    std::unique_ptr<tokenometer::CodexCollector> m_collector;
    std::unique_ptr<tokenometer::WslCodexCollector> m_wslCollector;
    std::jthread m_collectionThread;
    std::jthread m_wslCollectionThread;
    std::jthread m_toolContentThread;
    std::jthread m_chatGptImportThread;
    std::atomic<int64_t> m_lastCollectionAt{};
    std::atomic_bool m_collecting{};
    std::atomic_bool m_wslCollecting{};
    std::atomic_bool m_wslCollectionFailed{};
    std::atomic_bool m_initialLocalCollectionComplete{};
    std::atomic_bool m_collectionFailed{};
    std::atomic_bool m_chatGptImporting{};
    std::atomic_bool m_closing{};
    std::atomic_bool m_bubbleSnapshotFailed{};
    std::mutex m_bubbleSnapshotMutex;
    BubbleUsageSnapshot m_bubbleSnapshot;
    tokenometer::ChatGptImportViewData m_chatGptImportData;
    tokenometer::SurfacePreferences m_surfacePreferences;
    tokenometer::UsageScope m_detailsScope{ tokenometer::UsageScope::CodexExact };
    tokenometer::DetailsDimension m_detailsDimension{ tokenometer::DetailsDimension::Tool };
    std::wstring m_selectedBreakdownKey;
    std::wstring m_selectedSessionId;
    std::wstring m_selectedSessionAccountId;
    std::wstring m_selectedSessionSourceKind;
    std::wstring m_selectedToolLocator;
    std::wstring m_selectedToolDetails;
    std::vector<std::pair<std::wstring, tokenometer::ToolCallDetail>> m_toolLocators;
    tokenometer::UsageScope m_trendScope{ tokenometer::UsageScope::CodexExact };
    tokenometer::TrendGroup m_trendGroup{ tokenometer::TrendGroup::Tool };
    tokenometer::TrendChart m_trendChart{ tokenometer::TrendChart::Bars };
    tokenometer::TrendRange m_trendRange{ tokenometer::TrendRange::Days30 };
    int64_t m_lastTrendRefreshTick{};
    HWND m_hwnd{};
    LONG_PTR m_dashboardStyle{};
    LONG_PTR m_dashboardExtendedStyle{};
    bool m_bubbleMode{};
    bool m_backdropDisabled{};
    bool m_explicitExit{};
    bool m_layoutEditorExpanded{};
    bool m_toolManagerExpanded{};
    bool m_hoverPreviewActive{};
    bool m_hoverRestoreDashboard{};
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
        if (!tokenometer::ChatGPTExportImporter::SelfTest()) return 15;
        if (!tokenometer::WslProcessRunner::SelfTest()) return 16;
        if (!tokenometer::WslCodexCollector::SelfTest()) return 17;
        if (!tokenometer::SurfacePreferences::SelfTest()) return 18;
        return 0;
    }

    HANDLE const rawInstanceMutex = CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\Tokenometer.CollectionOwner.v1");
    DWORD const mutexError = GetLastError();
    if (!rawInstanceMutex) return static_cast<int>(mutexError);
    winrt::handle instanceMutex{ rawInstanceMutex };
    if (mutexError == ERROR_ALREADY_EXISTS) return 0;

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
