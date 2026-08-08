#include "TrayIcon.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
    constexpr UINT openDashboardCommand = 1;
    constexpr UINT showFloatingBubbleCommand = 2;
    constexpr UINT exitCommand = 3;
    constexpr size_t maximumTooltipCharacters = 127;

    NOTIFYICONDATAW Identity(HWND owner, UINT identifier)
    {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner;
        data.uID = identifier;
        return data;
    }
}

namespace tokenometer
{
    TrayIcon::TrayIcon(
        HWND owner,
        Callbacks callbacks,
        HICON icon,
        UINT identifier,
        UINT callbackMessage)
        : m_owner(owner),
          m_identifier(identifier),
          m_callbackMessage(callbackMessage),
          m_taskbarCreatedMessage(RegisterWindowMessageW(L"TaskbarCreated")),
          m_callbacks(std::move(callbacks))
    {
        HICON const source = icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
        if (source)
        {
            m_icon = CopyIcon(source);
        }
    }

    TrayIcon::~TrayIcon()
    {
        Remove();
        if (m_icon)
        {
            DestroyIcon(m_icon);
        }
    }

    bool TrayIcon::Add(std::wstring_view tooltip)
    {
        StoreTooltip(tooltip);
        m_shouldBeVisible = true;
        if (m_added)
        {
            return UpdateTooltip(tooltip);
        }
        return AddToShell();
    }

    bool TrayIcon::UpdateTooltip(std::wstring_view tooltip)
    {
        StoreTooltip(tooltip);
        if (!m_added)
        {
            return true;
        }

        auto data = Identity(m_owner, m_identifier);
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        std::copy(m_tooltip.begin(), m_tooltip.end(), data.szTip);
        return Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
    }

    void TrayIcon::Remove() noexcept
    {
        m_shouldBeVisible = false;
        CancelPendingLeftClick();
        m_suppressNextLeftUp = false;
        if (!m_added)
        {
            return;
        }

        auto data = Identity(m_owner, m_identifier);
        Shell_NotifyIconW(NIM_DELETE, &data);
        m_added = false;
    }

    bool TrayIcon::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_TIMER && m_leftClickTimer != 0 &&
            wParam == m_leftClickTimer)
        {
            CancelPendingLeftClick();
            if (m_callbacks.leftClick)
            {
                m_callbacks.leftClick();
            }
            return true;
        }

        if (m_taskbarCreatedMessage != 0 && message == m_taskbarCreatedMessage)
        {
            CancelPendingLeftClick();
            m_suppressNextLeftUp = false;
            if (m_shouldBeVisible)
            {
                m_added = false;
                (void)AddToShell();
            }
            return true;
        }

        if (message != m_callbackMessage ||
            HIWORD(lParam) != static_cast<WORD>(m_identifier))
        {
            return false;
        }
        if (!m_shouldBeVisible || !m_added)
        {
            return true;
        }

        switch (LOWORD(lParam))
        {
        case WM_LBUTTONUP:
            if (m_suppressNextLeftUp)
            {
                m_suppressNextLeftUp = false;
            }
            else
            {
                HandleLeftClick();
            }
            return true;

        case NIN_SELECT:
        case NIN_KEYSELECT:
            CancelPendingLeftClick();
            if (m_callbacks.leftClick)
            {
                m_callbacks.leftClick();
            }
            return true;

        case WM_LBUTTONDBLCLK:
            CancelPendingLeftClick();
            m_suppressNextLeftUp = true;
            if (m_callbacks.doubleClick)
            {
                m_callbacks.doubleClick();
            }
            return true;

        case NIN_POPUPOPEN:
            if (m_callbacks.hoverChanged)
            {
                m_callbacks.hoverChanged(true);
            }
            return true;

        case NIN_POPUPCLOSE:
            if (m_callbacks.hoverChanged)
            {
                m_callbacks.hoverChanged(false);
            }
            return true;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu({ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
            return true;

        default:
            return false;
        }
    }

    UINT TrayIcon::CallbackMessage() const noexcept
    {
        return m_callbackMessage;
    }

    UINT TrayIcon::TaskbarCreatedMessage() const noexcept
    {
        return m_taskbarCreatedMessage;
    }

    bool TrayIcon::IsAdded() const noexcept
    {
        return m_added;
    }

    bool TrayIcon::AddToShell()
    {
        if (!m_owner || !m_icon || m_callbackMessage == 0 ||
            m_taskbarCreatedMessage == 0 ||
            m_identifier > std::numeric_limits<WORD>::max())
        {
            return false;
        }

        auto data = Identity(m_owner, m_identifier);
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = m_callbackMessage;
        data.hIcon = m_icon;
        std::copy(m_tooltip.begin(), m_tooltip.end(), data.szTip);
        if (!Shell_NotifyIconW(NIM_ADD, &data))
        {
            return false;
        }

        auto version = Identity(m_owner, m_identifier);
        version.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &version))
        {
            Shell_NotifyIconW(NIM_DELETE, &version);
            return false;
        }

        m_added = true;
        return true;
    }

    void TrayIcon::CancelPendingLeftClick() noexcept
    {
        if (m_leftClickTimer != 0)
        {
            KillTimer(m_owner, m_leftClickTimer);
            m_leftClickTimer = 0;
        }
    }

    void TrayIcon::HandleLeftClick()
    {
        if (!m_callbacks.leftClick)
        {
            return;
        }
        if (!m_callbacks.doubleClick)
        {
            m_callbacks.leftClick();
            return;
        }

        CancelPendingLeftClick();
        UINT_PTR const requestedIdentifier = reinterpret_cast<UINT_PTR>(this);
        m_leftClickTimer = SetTimer(
            m_owner,
            requestedIdentifier,
            GetDoubleClickTime(),
            nullptr);
        if (m_leftClickTimer == 0)
        {
            m_callbacks.leftClick();
        }
    }

    void TrayIcon::ShowContextMenu(POINT point)
    {
        HMENU const menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        BOOL const menuReady =
            AppendMenuW(menu, MF_STRING, openDashboardCommand, L"打开仪表盘") &&
            AppendMenuW(menu, MF_STRING, showFloatingBubbleCommand, L"显示 / 隐藏浮动气泡") &&
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) &&
            AppendMenuW(menu, MF_STRING, exitCommand, L"退出");
        if (!menuReady)
        {
            DestroyMenu(menu);
            return;
        }

        SetMenuDefaultItem(menu, openDashboardCommand, FALSE);
        SetForegroundWindow(m_owner);
        UINT const command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x,
            point.y,
            m_owner,
            nullptr);
        DestroyMenu(menu);

        auto data = Identity(m_owner, m_identifier);
        Shell_NotifyIconW(NIM_SETFOCUS, &data);
        PostMessageW(m_owner, WM_NULL, 0, 0);

        switch (command)
        {
        case openDashboardCommand:
            if (m_callbacks.openDashboard)
            {
                m_callbacks.openDashboard();
            }
            break;
        case showFloatingBubbleCommand:
            if (m_callbacks.showFloatingBubble)
            {
                m_callbacks.showFloatingBubble();
            }
            break;
        case exitCommand:
            if (m_callbacks.exit)
            {
                m_callbacks.exit();
            }
            break;
        default:
            break;
        }
    }

    void TrayIcon::StoreTooltip(std::wstring_view tooltip)
    {
        if (tooltip.empty())
        {
            m_tooltip.clear();
            return;
        }
        m_tooltip.assign(tooltip.data(), std::min(tooltip.size(), maximumTooltipCharacters));
    }
}
