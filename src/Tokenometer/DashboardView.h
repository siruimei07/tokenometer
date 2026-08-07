#pragma once

#include "UsageModels.h"

#include <functional>
#include <optional>
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

    struct OverviewViewData
    {
        UsageTotals total;
        UsageTotals day;
        std::vector<DailyUsage> daily;
        std::vector<SessionSummary> recent;
        std::optional<RateLimitSnapshot> codexLimit;
        int64_t lastSync{};
        bool collecting{};
        std::wstring error;
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
        DetailsDimension dimension{ DetailsDimension::Tool };
        std::vector<BreakdownRow> rows;
        std::wstring selectedKey;
        std::vector<SessionSummary> recentSessions;
        std::wstring selectedSessionId;
        std::vector<TurnSummary> selectedTurns;
        std::vector<ToolCallViewData> toolCalls;
        std::wstring selectedToolCallLocator;
        std::wstring selectedToolDetails;
        bool loading{};
        std::wstring error;
    };

    struct DetailsCallbacks
    {
        std::function<void(DetailsDimension)> onDimensionChanged;
        std::function<void(std::wstring const&)> onBreakdownSelected;
        std::function<void(std::wstring const&)> onSessionSelected;
        std::function<void(std::wstring const&)> onToolCallRequested;
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
        void UpdateDetailsDimensionButtons();

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
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_codexLimitName{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_codexLimitValue{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_codexLimitReset{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_emptyOverviewTitle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_emptyOverviewDetail{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_overviewEmptyState{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_overviewMetricsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_recentEmptyState{ nullptr };

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
        DetailsDimension m_detailsDimension{ DetailsDimension::Tool };
        DetailsCallbacks m_detailsCallbacks;

        DashboardPage m_currentPage{ DashboardPage::Overview };
    };
}
