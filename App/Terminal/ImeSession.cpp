#include "pch.h"
#include "Terminal/ImeSession.h"
#include "Interop/Encoding.h"

namespace winrt::GhosttyWin32::implementation
{
    namespace txtCore = winrt::Windows::UI::Text::Core;
    namespace interop = core::interop;

    void ImeSession::Ensure()
    {
        if (m_context) return;
        // CoreTextServicesManager.GetForCurrentView lives at the view
        // (~window) level, but CreateEditContext spins up an
        // independent context — multiple surfaces in the same window
        // each get their own. The OS arbitrates which one receives
        // input via NotifyFocusEnter/Leave, driven by SetEngaged.
        auto manager = txtCore::CoreTextServicesManager::GetForCurrentView();
        m_context = manager.CreateEditContext();
        m_context.InputPaneDisplayPolicy(txtCore::CoreTextInputPaneDisplayPolicy::Manual);
        m_context.InputScope(txtCore::CoreTextInputScope::Default);

        std::weak_ptr<ImeSession> weak = weak_from_this();

        m_context.TextRequested([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextRequestedEventArgs const& args) {
            auto self = weak.lock();
            if (!self) return;
            args.Request().Text(winrt::hstring(self->m_buffer.paddedText()));
        });

        m_context.SelectionRequested([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextSelectionRequestedEventArgs const& args) {
            auto self = weak.lock();
            if (!self) return;
            int32_t pos = self->m_buffer.selectionPosition();
            args.Request().Selection({ pos, pos });
        });

        m_context.TextUpdating([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextUpdatingEventArgs const& args) {
            auto self = weak.lock();
            if (!self) return;
            auto range = args.Range();
            auto newText = args.Text();
            self->m_buffer.applyTextUpdate(range.StartCaretPosition, range.EndCaretPosition,
                                           newText.c_str(), newText.size());
            if (self->m_buffer.composing() && self->m_onPreedit) {
                self->m_onPreedit(interop::Encoding::toUtf8(self->m_buffer.text()));
            }
        });

        m_context.CompositionStarted([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionStartedEventArgs const&) {
            auto self = weak.lock();
            if (!self) return;
            self->m_buffer.compositionStarted();
        });

        m_context.CompositionCompleted([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionCompletedEventArgs const&) {
            auto self = weak.lock();
            if (!self) return;
            if (self->m_onCommit) {
                self->m_onCommit(interop::Encoding::toUtf8(self->m_buffer.text()));
            }
            self->m_buffer.compositionCompleted();
        });

        m_context.LayoutRequested([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextLayoutRequestedEventArgs const& args) {
            auto self = weak.lock();
            if (!self || !self->m_caretRect) return;
            auto bounds = self->m_caretRect();
            if (!bounds) return;
            args.Request().LayoutBounds().ControlBounds(*bounds);
            args.Request().LayoutBounds().TextBounds(*bounds);
        });

        m_context.FocusRemoved([weak](
            txtCore::CoreTextEditContext const&, auto&&) {
            auto self = weak.lock();
            if (!self) return;
            // The OS took the context away mid-composition: drop the
            // half-typed text rather than leave a stale preedit on
            // screen.
            if (self->m_buffer.composing()) {
                self->m_buffer.reset();
                if (self->m_onPreedit) self->m_onPreedit(std::string{});
            }
        });
    }

    void ImeSession::SetEngaged(bool engaged)
    {
        if (!m_context) return;
        if (engaged) m_context.NotifyFocusEnter();
        else         m_context.NotifyFocusLeave();
    }

    void ImeSession::Reset()
    {
        if (m_context) {
            // Best-effort: tell the OS the context is leaving focus
            // before we drop our reference. Skipping this leaves the
            // text-services manager holding a stale focus pointer
            // until GC catches up.
            m_context.NotifyFocusLeave();
            m_context = nullptr;
        }
        m_buffer.reset();
    }
}
