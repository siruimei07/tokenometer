#pragma once

#include "SurfacePreferences.h"
#include "UsageModels.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

namespace tokenometer
{
    enum class DashboardPage
    {
        Overview,
        Details,
        Trends,
        Settings,
    };

    enum class UsageScope
    {
        CodexExact,
        ChatGptEstimated,
    };

    enum class DeviceSyncState
    {
        Never,
        Syncing,
        Synced,
        Warning,
    };

    struct DeviceViewData
    {
        DeviceSummary summary;
        DeviceSyncState state{ DeviceSyncState::Never };
        std::wstring statusText;
    };

    struct OverviewViewData
    {
        UsageTotals total;
        UsageTotals day;
        std::vector<DailyUsage> daily;
        std::vector<SessionSummary> recent;
        UsageTotals chatGptTotals;
        std::vector<DeviceViewData> devices;
        std::optional<RateLimitSnapshot> codexLimit;
        int64_t lastSync{};
        bool collecting{};
        std::wstring error;
        std::wstring warning;
    };

    enum class DetailsDimension
    {
        Tool,
        Model,
        Session,
        Device,
        Project,
        Account,
    };

    struct ToolCallViewData
    {
        std::wstring locator;
        std::wstring name;
        std::wstring summary;
    };

    struct DetailsViewData
    {
        UsageScope scope{ UsageScope::CodexExact };
        DetailsDimension dimension{ DetailsDimension::Tool };
        std::vector<BreakdownRow> rows;
        std::wstring selectedKey;
        std::vector<SessionSummary> recentSessions;
        SessionRef selectedSession;
        std::vector<TurnSummary> selectedTurns;
        std::vector<ToolCallViewData> toolCalls;
        std::wstring selectedToolCallLocator;
        std::wstring selectedToolDetails;
        bool breakdownHasMore{};
        bool breakdownExpanded{};
        bool sessionsHasMore{};
        bool sessionsExpanded{};
        bool turnsHasMore{};
        bool turnsExpanded{};
        bool toolsHasMore{};
        bool toolsExpanded{};
        bool loading{};
        std::wstring unavailableReason;
        std::wstring error;
    };

    enum class DetailsList
    {
        Breakdown,
        Sessions,
        Turns,
        Tools,
    };

    struct DetailsCallbacks
    {
        std::function<void(UsageScope)> onScopeChanged;
        std::function<void(DetailsDimension)> onDimensionChanged;
        std::function<void(std::wstring const&, SessionRef const&)> onBreakdownSelected;
        std::function<void(SessionRef const&)> onSessionSelected;
        std::function<void(std::wstring const&)> onToolCallRequested;
        std::function<void(DetailsList, bool)> onListExpansionChanged;
    };

    enum class TrendGroup
    {
        Tool,
        Model,
    };

    enum class TrendChart
    {
        Bars,
        Kline,
    };

    enum class TrendRange
    {
        Days7,
        Days30,
        Days90,
        Days365,
    };

    struct TrendPoint
    {
        std::wstring day;
        int64_t value{};
    };

    struct TrendSeries
    {
        std::wstring key;
        std::vector<TrendPoint> points;
        int64_t total{};
        double percent{};
    };

    struct TrendCandle
    {
        std::wstring day;
        int64_t open{};
        int64_t high{};
        int64_t low{};
        int64_t close{};
        int64_t volume{};
    };

    struct TrendHeatCell
    {
        std::wstring day;
        int64_t value{};
    };

    struct TrendViewData
    {
        UsageScope scope{ UsageScope::CodexExact };
        TrendGroup group{ TrendGroup::Tool };
        TrendChart chart{ TrendChart::Bars };
        TrendRange range{ TrendRange::Days30 };
        std::vector<TrendSeries> series;
        std::wstring candleSeries;
        std::vector<TrendCandle> candles;
        std::vector<TrendHeatCell> heatCells;
        int currentStreak{};
        int longestStreak{};
        bool loading{};
        std::wstring error;
    };

    struct TrendCallbacks
    {
        std::function<void(UsageScope)> onScopeChanged;
        std::function<void(TrendGroup)> onGroupChanged;
        std::function<void(TrendChart)> onChartChanged;
        std::function<void(TrendRange)> onRangeChanged;
    };

    enum class ChatGptImportState
    {
        Idle,
        SelectingFiles,
        Importing,
        Succeeded,
        Failed,
    };

    struct ChatGptImportViewData
    {
        ChatGptImportState state{ ChatGptImportState::Idle };
        std::vector<std::wstring> selectedFiles;
        std::wstring accountLabel{ L"ChatGPT" };
        int64_t conversations{};
        int64_t estimatedTokens{};
        int64_t skipped{};
        int64_t unchangedFiles{};
        int64_t errors{};
        std::wstring message;
        std::wstring details;
        bool detailsExpanded{};
    };

    struct ChatGptImportCallbacks
    {
        std::function<void(std::wstring const& accountLabel)> onChooseFilesRequested;
        std::function<void(bool expanded)> onDetailsToggled;
    };

    struct SurfacePreferencesViewData : SurfacePreferences
    {
        bool layoutEditorExpanded{};
        bool toolManagerExpanded{};
        std::wstring livePreview;
    };

    struct SurfacePreferencesCallbacks
    {
        std::function<void(SurfacePreferencesViewData const&)> onChanged;
    };

    class DashboardView final
    {
    public:
        DashboardView();

        [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::Grid Root() const noexcept;
        [[nodiscard]] DashboardPage CurrentPage() const noexcept;

        void SetStatus(
            std::wstring_view status,
            std::wstring_view detail = {},
            bool healthy = true);
        void UpdateOverview(OverviewViewData const& data);
        void UpdateDetails(DetailsViewData const& data);
        void SetDetailsCallbacks(DetailsCallbacks callbacks);
        void UpdateTrends(TrendViewData const& data);
        void SetTrendCallbacks(TrendCallbacks callbacks);
        void UpdateChatGptImport(ChatGptImportViewData const& data);
        void SetChatGptImportCallbacks(ChatGptImportCallbacks callbacks);
        void UpdateSurfacePreferences(SurfacePreferencesViewData const& data);
        void SetSurfacePreferencesCallbacks(SurfacePreferencesCallbacks callbacks);
        void ApplySurfaceTheme(SurfaceTheme theme);
        void ShowPage(DashboardPage page);

    private:
        void BuildShell();
        winrt::Microsoft::UI::Xaml::Controls::Grid BuildHeader();
        winrt::Microsoft::UI::Xaml::Controls::Grid BuildOverviewPage();
        winrt::Microsoft::UI::Xaml::Controls::Grid BuildDetailsPage();
        winrt::Microsoft::UI::Xaml::Controls::Grid BuildTrendsPage();
        winrt::Microsoft::UI::Xaml::Controls::Grid BuildSettingsPage();
        winrt::Microsoft::UI::Xaml::Controls::Border BuildBottomNavigation();

        winrt::Microsoft::UI::Xaml::Controls::Button MakeNavigationButton(
            std::wstring_view label,
            DashboardPage page);
        void UpdateNavigationState();
        void UpdateDailyVisuals(std::vector<DailyUsage> const& daily);
        void UpdateDetailsScopeButtons();
        void UpdateDetailsDimensionButtons();
        void UpdateTrendScopeButtons();
        void UpdateTrendButtons();
        void UpdateChatGptImportLayout();
        void ApplyOverviewLayout();
        void RebuildOverviewEditor();
        void NormalizeSurfacePreferences();
        void UpdateSurfacePreferencesLayout();
        void UpdateScrollState();
        void RebuildSurfaceLayoutEditor();
        void RebuildSurfaceToolEditor();
        void NotifySurfacePreferencesChanged();

        winrt::Microsoft::UI::Xaml::Controls::Grid m_root{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_pageHost{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer m_scroller{ nullptr };

        winrt::Microsoft::UI::Xaml::Controls::Grid m_overviewPage{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_detailsPage{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_trendsPage{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_settingsPage{ nullptr };

        winrt::Microsoft::UI::Xaml::Controls::Button m_overviewButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_detailsButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_trendsButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_settingsButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_overviewCustomizeButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_overviewEditor{ nullptr };

        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_statusText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_statusDetail{ nullptr };
        winrt::Microsoft::UI::Xaml::Shapes::Ellipse m_statusDot{ nullptr };

        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_totalTokensText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_cacheHitText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_outputTokensText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_dayTokensText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_dayMessagesText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_dayToolCallsText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_activeDaysText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_heatmapCaption{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptOverviewValue{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptOverviewDetail{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_codexLimitName{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_codexLimitValue{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_codexLimitReset{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_emptyOverviewTitle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_emptyOverviewDetail{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_overviewEmptyState{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_overviewMetricsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_recentEmptyState{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_chatGptOverviewPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_overviewRecentLabel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_overviewDevicesLabel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_devicePanel{ nullptr };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Border> m_overviewCards;
        size_t m_recentSessionCount{};
        size_t m_deviceCount{};

        winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition m_cacheProgressFill{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition m_cacheProgressRest{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition m_codexProgressFill{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition m_codexProgressRest{ nullptr };

        std::vector<winrt::Microsoft::UI::Xaml::Controls::Border> m_dailyBars;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Border> m_heatmapCells;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Border> m_sessionRows;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> m_sessionTitles;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> m_sessionDetails;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> m_sessionValues;

        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_breakdownList{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_detailsSessionsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_detailsSelectionState{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_detailsMetricsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_detailsSelectedTitle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_detailsInputText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_detailsCacheHitTokensText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_detailsCacheMissTokensText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_detailsOutputText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_detailsHitRateText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition m_detailsCacheProgressFill{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition m_detailsCacheProgressRest{ nullptr };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_detailsDimensionButtons;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_detailsScopeButtons;
        UsageScope m_detailsScope{ UsageScope::CodexExact };
        DetailsDimension m_detailsDimension{ DetailsDimension::Tool };
        DetailsCallbacks m_detailsCallbacks;
        bool m_detailsExpanded{};

        winrt::Microsoft::UI::Xaml::Controls::Grid m_trendChartHost{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_trendChartCaption{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_currentStreakText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_longestStreakText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_trendHeatCaption{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_trendLegend{ nullptr };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Border> m_trendHeatCells;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_trendGroupButtons;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_trendChartButtons;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_trendRangeButtons;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_trendScopeButtons;
        UsageScope m_trendScope{ UsageScope::CodexExact };
        TrendGroup m_trendGroup{ TrendGroup::Tool };
        TrendChart m_trendChart{ TrendChart::Bars };
        TrendRange m_trendRange{ TrendRange::Days30 };
        TrendCallbacks m_trendCallbacks;

        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptAccountLabel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_chatGptChooseFilesButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptFilesText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptImportTitle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptImportMessage{ nullptr };
        winrt::Microsoft::UI::Xaml::Shapes::Ellipse m_chatGptImportDot{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptConversationCount{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptEstimatedTokens{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptSkippedCount{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptUnchangedCount{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptErrorCount{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_chatGptDetailsButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_chatGptDetailsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_chatGptDetailsText{ nullptr };
        std::wstring m_chatGptDetails;
        bool m_chatGptDetailsExpanded{};
        ChatGptImportCallbacks m_chatGptImportCallbacks;

        winrt::Microsoft::UI::Xaml::Controls::Button m_launchToTrayToggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_closeToTrayToggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_blurToggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_transparentWindowToggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_providerColorsToggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_bubbleAlwaysOnTopToggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_hoverPreviewToggle{ nullptr };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_themeButtons;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_opacityButtons;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> m_layoutPresetButtons;
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_surfacePreviewText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_surfaceToolSummaryText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_surfaceLayoutExpandButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_surfaceLayoutEditorPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_surfaceLayoutEditor{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_surfaceLayoutAddButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_surfaceToolExpandButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_surfaceToolEditorPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_surfaceToolEditor{ nullptr };
        SurfacePreferencesViewData m_surfacePreferences;
        SurfacePreferencesCallbacks m_surfacePreferencesCallbacks;
        bool m_updatingSurfacePreferences{};
        SurfaceTheme m_surfaceTheme{ SurfaceTheme::System };

        DashboardPage m_currentPage{ DashboardPage::Overview };
    };
}
