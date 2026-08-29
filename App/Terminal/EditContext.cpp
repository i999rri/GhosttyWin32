#include "pch.h"
#include "Terminal/EditContext.h"

namespace winrt::GhosttyWin32::implementation
{
    namespace txtCore = winrt::Windows::UI::Text::Core;

    EditContext::EditContext(Microsoft::UI::Xaml::FrameworkElement element)
        : m_element(std::move(element))
    {
        m_loadedToken = m_element.Loaded([this](auto&&, auto&&) { OnLoaded(); });
    }

    EditContext::~EditContext()
    {
        Release();
        if (m_element && m_loadedToken.value != 0) {
            m_element.Loaded(m_loadedToken);
        }
    }

    void EditContext::SetEngaged(bool engaged)
    {
        m_wantEngaged = engaged;
        ApplyEngagement();
    }

    void EditContext::Release()
    {
        if (!HasContext()) return;
        // Give back only what we took: if this is the context the OS
        // routes input to, say so before dropping it, or the
        // text-services manager keeps a stale focus pointer until GC
        // catches up.
        if (m_engaged) {
            m_context.NotifyFocusLeave();
            m_engaged = false;
        }
        m_textRequested.revoke();
        m_selectionRequested.revoke();
        m_textUpdating.revoke();
        m_compositionStarted.revoke();
        m_compositionCompleted.revoke();
        m_layoutRequested.revoke();
        m_focusRemoved.revoke();
        m_context = nullptr;
    }

    void EditContext::OnLoaded()
    {
        if (HasContext()) return;  // reparent: the context we have is still good
        // CoreTextServicesManager.GetForCurrentView lives at the view
        // (~window) level, but CreateEditContext spins up an
        // independent context — multiple surfaces in the same window
        // each get their own. The OS arbitrates which one receives
        // input via NotifyFocusEnter/Leave.
        auto manager = txtCore::CoreTextServicesManager::GetForCurrentView();
        m_context = manager.CreateEditContext();
        m_context.InputPaneDisplayPolicy(txtCore::CoreTextInputPaneDisplayPolicy::Manual);
        m_context.InputScope(txtCore::CoreTextInputScope::Default);
        BindHandlers();
        ApplyEngagement();
    }

    void EditContext::ApplyEngagement()
    {
        if (!HasContext()) return;  // remembered; applied on creation
        if (m_wantEngaged == m_engaged) return;
        if (m_wantEngaged) m_context.NotifyFocusEnter();
        else               m_context.NotifyFocusLeave();
        m_engaged = m_wantEngaged;
    }

    void EditContext::BindHandlers()
    {
        // auto_revoke: each revoker drops its subscription on Release,
        // so a handler can capture `this` — nothing fires after this
        // object lets go of the context. The subscriptions read the
        // current SetOn… handler at fire time, so setting one later
        // needs no rebind.
        m_textRequested = m_context.TextRequested(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextRequestedEventArgs const& args) {
            if (m_handlers.textRequested) args.Request().Text(m_handlers.textRequested());
        });
        m_selectionRequested = m_context.SelectionRequested(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextSelectionRequestedEventArgs const& args) {
            if (!m_handlers.selectionRequested) return;
            int32_t pos = m_handlers.selectionRequested();
            args.Request().Selection({ pos, pos });
        });
        m_textUpdating = m_context.TextUpdating(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextUpdatingEventArgs const& args) {
            if (!m_handlers.textUpdating) return;
            auto range = args.Range();
            m_handlers.textUpdating(range.StartCaretPosition, range.EndCaretPosition, args.Text());
        });
        m_compositionStarted = m_context.CompositionStarted(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionStartedEventArgs const&) {
            if (m_handlers.compositionStarted) m_handlers.compositionStarted();
        });
        m_compositionCompleted = m_context.CompositionCompleted(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionCompletedEventArgs const&) {
            if (m_handlers.compositionCompleted) m_handlers.compositionCompleted();
        });
        m_layoutRequested = m_context.LayoutRequested(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextLayoutRequestedEventArgs const& args) {
            if (!m_handlers.layoutRequested) return;
            auto bounds = m_handlers.layoutRequested();
            if (!bounds) return;
            args.Request().LayoutBounds().ControlBounds(*bounds);
            args.Request().LayoutBounds().TextBounds(*bounds);
        });
        m_focusRemoved = m_context.FocusRemoved(winrt::auto_revoke, [this](
            txtCore::CoreTextEditContext const&, auto&&) {
            if (m_handlers.focusRemoved) m_handlers.focusRemoved();
        });
    }
}
