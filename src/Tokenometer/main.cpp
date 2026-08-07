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
    constexpr int widgetWidthDip = 300;
    constexpr int widgetHeightDip = 150;
    constexpr int cornerRadiusDip = 18;

    winrt::Windows::UI::Color Color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
    {
        return { alpha, red, green, blue };
    }

    media::SolidColorBrush Brush(winrt::Windows::UI::Color color)
    {
        return media::SolidColorBrush{ color };
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
        double glyphSize)
    {
        controls::Border icon;
        icon.Width(18);
        icon.Height(18);
        icon.CornerRadius({ 5 });
        icon.Background(Brush(background));

        auto label = Text(glyph, glyphSize, Color(247, 247, 247), 600);
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
        fallback.Background(Brush(Color(16, 19, 43)));
        Place(fallback, 0, 0);
        m_root.Children().Append(fallback);

        m_swapChainPanel = controls::SwapChainPanel{};
        m_swapChainPanel.Width(widgetWidthDip);
        m_swapChainPanel.Height(widgetHeightDip);
        Place(m_swapChainPanel, 0, 0);
        m_root.Children().Append(m_swapChainPanel);
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
        auto usageIcon = Icon(L"✳", Color(204, 113, 82), 12);
        Place(usageIcon, 13, 15);
        m_root.Children().Append(usageIcon);

        auto title = Text(L"Token Usage", 11.5, Color(242, 242, 244), 600);
        Place(title, 38, 15);
        m_root.Children().Append(title);

        auto percent = Text(L"53,8%", 13, Color(242, 242, 244), 500);
        percent.Width(78);
        percent.TextAlignment(mux::TextAlignment::Right);
        Place(percent, 209, 12);
        m_root.Children().Append(percent);

        controls::Border track;
        track.Width(274);
        track.Height(6);
        track.CornerRadius({ 3 });
        track.Background(Brush(Color(42, 45, 74)));
        track.BorderBrush(Brush(Color(67, 70, 99)));
        track.BorderThickness({ 0.5 });
        Place(track, 13, 44);
        m_root.Children().Append(track);

        controls::Border fill;
        fill.Width(146);
        fill.Height(4);
        fill.CornerRadius({ 2 });
        fill.Background(Brush(Color(15, 91, 235)));
        Place(fill, 14, 45);
        m_root.Children().Append(fill);

        auto used = Text(L"18,838", 9.5, Color(240, 240, 242), 600);
        Place(used, 13, 54);
        m_root.Children().Append(used);

        auto total = Text(L"/ 35,000", 9.5, Color(151, 153, 174));
        Place(total, 50, 54);
        m_root.Children().Append(total);

        auto left = Text(L"16,162 left", 9.5, Color(151, 153, 174));
        left.Width(78);
        left.TextAlignment(mux::TextAlignment::Right);
        Place(left, 209, 54);
        m_root.Children().Append(left);

        shapes::Rectangle divider;
        divider.Width(274);
        divider.Height(0.5);
        divider.Fill(Brush(Color(57, 59, 104)));
        Place(divider, 13, 80);
        m_root.Children().Append(divider);

        auto resetIcon = Icon(L"⌛", Color(14, 133, 237), 11);
        Place(resetIcon, 13, 92);
        m_root.Children().Append(resetIcon);

        auto reset = Text(L"Reset Time", 11.5, Color(240, 240, 242), 600);
        Place(reset, 38, 92);
        m_root.Children().Append(reset);

        auto remaining = Text(L"2h 58m", 13, Color(242, 242, 244), 500);
        remaining.Width(78);
        remaining.TextAlignment(mux::TextAlignment::Right);
        Place(remaining, 209, 89);
        m_root.Children().Append(remaining);

        auto refresh = Text(L"↻", 8.5, Color(154, 155, 174), 600);
        Place(refresh, 13, 130);
        m_root.Children().Append(refresh);

        auto updated = Text(L"Updated: Just Now", 8, Color(154, 155, 174));
        Place(updated, 24, 131);
        m_root.Children().Append(updated);

        m_closeButton = controls::Button{};
        m_closeButton.Width(18);
        m_closeButton.Height(18);
        m_closeButton.Padding({ 5.5 });
        m_closeButton.Background(Brush(Color(0, 0, 0, 0)));
        m_closeButton.BorderThickness({ 0 });
        m_closeButton.Opacity(0);

        shapes::Ellipse dot;
        dot.Width(7);
        dot.Height(7);
        dot.Fill(Brush(Color(255, 95, 87)));
        m_closeButton.Content(dot);
        controls::ToolTipService::SetToolTip(m_closeButton, winrt::box_value(L"Close"));
        automation::AutomationProperties::SetName(m_closeButton, L"Close Tokenometer");
        Place(m_closeButton, 3, 3);
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
