#pragma once

#include <string_view>

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

        DashboardPage m_currentPage{ DashboardPage::Overview };
    };
}
