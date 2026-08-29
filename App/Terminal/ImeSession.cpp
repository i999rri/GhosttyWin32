#include "pch.h"
#include "Terminal/ImeSession.h"
#include "Interop/Encoding.h"

namespace winrt::GhosttyWin32::implementation
{
    namespace txtCore = winrt::Windows::UI::Text::Core;
    namespace interop = core::interop;

    std::shared_ptr<ImeSession> ImeSession::Create(EditContextSource& source)
    {
        std::shared_ptr<ImeSession> session(new ImeSession());
        std::weak_ptr<ImeSession> weak = session;
        source.WhenReady([weak](Context const& context) {
            if (auto self = weak.lock()) {
                self->Bind(context);
                self->ApplyEngagement();
            }
        });
        return session;
    }

    void ImeSession::Bind(Context const& context)
    {
        m_context = context;
        m_engaged = false;
        std::weak_ptr<ImeSession> weak = weak_from_this();

        context.TextRequested([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextRequestedEventArgs const& args) {
            auto self = weak.lock();
            if (!self) return;
            args.Request().Text(winrt::hstring(self->m_buffer.paddedText()));
        });

        context.SelectionRequested([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextSelectionRequestedEventArgs const& args) {
            auto self = weak.lock();
            if (!self) return;
            int32_t pos = self->m_buffer.selectionPosition();
            args.Request().Selection({ pos, pos });
        });

        context.TextUpdating([weak](
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

        context.CompositionStarted([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionStartedEventArgs const&) {
            auto self = weak.lock();
            if (!self) return;
            self->m_buffer.compositionStarted();
        });

        context.CompositionCompleted([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionCompletedEventArgs const&) {
            auto self = weak.lock();
            if (!self) return;
            if (self->m_onCommit) {
                self->m_onCommit(interop::Encoding::toUtf8(self->m_buffer.text()));
            }
            self->m_buffer.compositionCompleted();
        });

        context.LayoutRequested([weak](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextLayoutRequestedEventArgs const& args) {
            auto self = weak.lock();
            if (!self || !self->m_caretRect) return;
            auto bounds = self->m_caretRect();
            if (!bounds) return;
            args.Request().LayoutBounds().ControlBounds(*bounds);
            args.Request().LayoutBounds().TextBounds(*bounds);
        });

        context.FocusRemoved([weak](
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
        m_wantEngaged = engaged;
        ApplyEngagement();
    }

    void ImeSession::ApplyEngagement()
    {
        if (!m_context) return;  // remembered; applied when a context is delivered
        if (m_wantEngaged == m_engaged) return;
        if (m_wantEngaged) m_context.NotifyFocusEnter();
        else               m_context.NotifyFocusLeave();
        m_engaged = m_wantEngaged;
    }

    void ImeSession::Reset()
    {
        // Give back only what we took: if this context is the one the
        // OS routes input to, release it before letting go, or the
        // text-services manager keeps a stale focus pointer until GC
        // catches up. The owner's wish (m_wantEngaged) is not ours to
        // change.
        if (m_context) {
            if (m_engaged) {
                m_context.NotifyFocusLeave();
                m_engaged = false;
            }
            m_context = nullptr;
        }
        m_buffer.reset();
    }
}
