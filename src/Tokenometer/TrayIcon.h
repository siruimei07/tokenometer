#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace tokenometer
{
    class TrayIcon final
    {
    public:
        struct Callbacks
        {
            std::function<void()> leftClick;
            std::function<void()> doubleClick;
            std::function<void(bool)> hoverChanged;
            std::function<void()> openDashboard;
            std::function<void()> showFloatingBubble;
            std::function<void()> exit;
        };

        static constexpr UINT DefaultCallbackMessage = WM_APP + 42;

        TrayIcon(
            HWND owner,
            Callbacks callbacks,
            // The handle is copied; pass null to use the Windows application icon.
            HICON icon = nullptr,
            UINT identifier = 1,
            UINT callbackMessage = DefaultCallbackMessage);
        ~TrayIcon();

        TrayIcon(TrayIcon const&) = delete;
        TrayIcon& operator=(TrayIcon const&) = delete;
        TrayIcon(TrayIcon&&) = delete;
        TrayIcon& operator=(TrayIcon&&) = delete;

        [[nodiscard]] bool Add(std::wstring_view tooltip = L"Tokenometer");
        [[nodiscard]] bool UpdateTooltip(std::wstring_view tooltip);
        void Remove() noexcept;

        // Forward messages from the owner window procedure. A true result means
        // the message belongs to this tray icon and should return zero.
        [[nodiscard]] bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        [[nodiscard]] UINT CallbackMessage() const noexcept;
        [[nodiscard]] UINT TaskbarCreatedMessage() const noexcept;
        [[nodiscard]] bool IsAdded() const noexcept;

    private:
        [[nodiscard]] bool AddToShell();
        void CancelPendingLeftClick() noexcept;
        void HandleLeftClick();
        void ShowContextMenu(POINT point);
        void StoreTooltip(std::wstring_view tooltip);

        HWND m_owner{};
        HICON m_icon{};
        UINT m_identifier{};
        UINT m_callbackMessage{};
        UINT m_taskbarCreatedMessage{};
        Callbacks m_callbacks;
        std::wstring m_tooltip;
        bool m_shouldBeVisible{};
        bool m_added{};
        bool m_suppressNextLeftUp{};
        UINT_PTR m_leftClickTimer{};
    };
}
