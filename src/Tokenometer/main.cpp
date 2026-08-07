#include <windows.h>
#undef GetCurrentTime

#include "CaptureRenderer.h"

#include <chrono>
#include <memory>

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
}

struct TokenometerApp : mux::ApplicationT<TokenometerApp>
{
    void OnLaunched(mux::LaunchActivatedEventArgs const&)
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

        m_window = mux::Window{};
        m_window.Title(L"Tokenometer");
        m_window.Content(m_root);

        auto nativeWindow = m_window.as<::IWindowNative>();
        winrt::check_hresult(nativeWindow->get_WindowHandle(&m_hwnd));
        m_window.Activate();
        ConfigureWindow();
        if (m_swapChainPanel.IsLoaded())
        {
            StartBackdrop();
        }
        else
        {
            m_swapChainPanel.Loaded([this](auto const&, auto const&) { StartBackdrop(); });
        }
        WireInteractions();
    }

private:
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

    void WireInteractions()
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

        m_window.Closed([this](auto const&, auto const&)
        {
            if (m_statusTimer)
            {
                m_statusTimer.Stop();
            }
            if (m_renderer)
            {
                m_renderer->Stop();
                m_renderer.reset();
            }
        });
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
    mux::DispatcherTimer m_statusTimer{ nullptr };
    std::shared_ptr<CaptureRenderer> m_renderer;
    HWND m_hwnd{};
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

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
