#include "pch.h"
#include "Terminal/EditContextSource.h"

namespace winrt::GhosttyWin32::implementation
{
    namespace txtCore = winrt::Windows::UI::Text::Core;

    EditContextSource::EditContextSource(Microsoft::UI::Xaml::FrameworkElement element)
        : m_element(std::move(element))
    {
        m_loadedToken = m_element.Loaded([this](auto&&, auto&&) { OnLoaded(); });
    }

    EditContextSource::~EditContextSource()
    {
        if (m_element && m_loadedToken.value != 0) {
            m_element.Loaded(m_loadedToken);
        }
    }

    void EditContextSource::WhenReady(std::function<void(Context const&)> fn)
    {
        if (m_context) {
            fn(m_context);
            return;
        }
        m_waiting.push_back(std::move(fn));
    }

    void EditContextSource::Release()
    {
        m_context = nullptr;
    }

    void EditContextSource::OnLoaded()
    {
        if (m_context) return;  // reparent: the context we have is still good
        // CoreTextServicesManager.GetForCurrentView lives at the view
        // (~window) level, but CreateEditContext spins up an
        // independent context — multiple surfaces in the same window
        // each get their own. The OS arbitrates which one receives
        // input via NotifyFocusEnter/Leave.
        auto manager = txtCore::CoreTextServicesManager::GetForCurrentView();
        m_context = manager.CreateEditContext();
        m_context.InputPaneDisplayPolicy(txtCore::CoreTextInputPaneDisplayPolicy::Manual);
        m_context.InputScope(txtCore::CoreTextInputScope::Default);
        // Subscribers stay registered so a context made after Release
        // reaches them too.
        for (auto const& fn : m_waiting) fn(m_context);
    }
}
