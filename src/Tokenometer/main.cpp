#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace mux = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace media = winrt::Microsoft::UI::Xaml::Media;

struct TokenometerApp : mux::ApplicationT<TokenometerApp>
{
    void OnLaunched(mux::LaunchActivatedEventArgs const&)
    {
        auto root = controls::Grid{};
        root.Background(media::SolidColorBrush{ winrt::Windows::UI::Color{ 255, 15, 18, 28 } });

        auto label = controls::TextBlock{};
        label.Text(L"Tokenometer");
        label.FontSize(24);
        label.HorizontalAlignment(mux::HorizontalAlignment::Center);
        label.VerticalAlignment(mux::VerticalAlignment::Center);
        root.Children().Append(label);

        m_window = mux::Window{};
        m_window.Title(L"Tokenometer");
        m_window.Content(root);
        m_window.Activate();
    }

private:
    mux::Window m_window{ nullptr };
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
