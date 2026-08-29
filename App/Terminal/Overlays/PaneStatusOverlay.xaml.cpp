#include "pch.h"
#include "Terminal/Overlays/PaneStatusOverlay.xaml.h"
#include <string>
#if __has_include("PaneStatusOverlay.g.cpp")
#include "PaneStatusOverlay.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;

    PaneStatusOverlay::PaneStatusOverlay()
    {
        InitializeComponent();
    }

    void PaneStatusOverlay::SetHoveredLink(winrt::hstring const& url)
    {
        auto banner = LinkBanner();
        if (!banner) return;
        if (url.empty()) {
            banner.Visibility(mux::Visibility::Collapsed);
            return;
        }
        LinkBannerText().Text(url);
        banner.Visibility(mux::Visibility::Visible);
    }

    void PaneStatusOverlay::SetReadonly(bool readonly)
    {
        ReadonlyBadge().Visibility(readonly ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void PaneStatusOverlay::SetSecureInput(ghostty_action_secure_input_e mode)
    {
        const bool on = mode == GHOSTTY_SECURE_INPUT_ON    ? true
                      : mode == GHOSTTY_SECURE_INPUT_OFF   ? false
                                                           : !m_secureInput;
        if (on == m_secureInput) return;
        m_secureInput = on;
        SecureInputBadge().Visibility(on ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void PaneStatusOverlay::AppendKeySequence(winrt::hstring const& label)
    {
        m_keySequence.push_back(label);
        UpdateKeyStateBadge();
    }

    void PaneStatusOverlay::ClearKeySequence()
    {
        if (m_keySequence.empty()) return;
        m_keySequence.clear();
        UpdateKeyStateBadge();
    }

    void PaneStatusOverlay::PushKeyTable(winrt::hstring const& name)
    {
        m_keyTables.push_back(name);
        UpdateKeyStateBadge();
    }

    void PaneStatusOverlay::PopKeyTable(bool all)
    {
        if (m_keyTables.empty()) return;
        if (all) {
            m_keyTables.clear();
        } else {
            m_keyTables.pop_back();
        }
        UpdateKeyStateBadge();
    }

    void PaneStatusOverlay::UpdateKeyStateBadge()
    {
        // Table stack first ("resize"), pending chord second
        // ("ctrl+a …"), separated when both are live. Mirrors the
        // information upstream's KeyStateIndicator carries, minus
        // its popover chrome.
        std::wstring text;
        for (auto const& name : m_keyTables) {
            if (!text.empty()) text += L" · ";
            text += name;
        }
        if (!m_keySequence.empty()) {
            if (!text.empty()) text += L" · ";
            for (auto const& label : m_keySequence) {
                text += label;
                text += L' ';
            }
            text += L'…';
        }
        if (text.empty()) {
            KeyStateBadge().Visibility(mux::Visibility::Collapsed);
            return;
        }
        KeyStateBadgeText().Text(text);
        KeyStateBadge().Visibility(mux::Visibility::Visible);
    }
}
