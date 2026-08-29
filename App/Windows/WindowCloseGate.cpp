#include "pch.h"
#include "Windows/WindowCloseGate.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>

namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::GhosttyWin32::implementation {

namespace {

winrt::hstring DialogTitle(WindowCloseGate::Scope scope) noexcept {
    switch (scope) {
        case WindowCloseGate::Scope::Surface: return L"Close terminal?";
        case WindowCloseGate::Scope::Tab:     return L"Close tab?";
        case WindowCloseGate::Scope::Window:  return L"Close window?";
    }
    return L"Close?";
}

winrt::hstring DialogBody(WindowCloseGate::Scope scope) noexcept {
    switch (scope) {
        case WindowCloseGate::Scope::Surface:
            return L"This terminal still has a running process. It will be terminated.";
        case WindowCloseGate::Scope::Tab:
            return L"One or more terminals in this tab still have running processes. "
                   L"They will be terminated.";
        case WindowCloseGate::Scope::Window:
            return L"One or more terminals in this window still have running processes. "
                   L"They will be terminated.";
    }
    return L"There are still running processes. They will be terminated.";
}

}  // namespace

void WindowCloseGate::Submit(Scope scope,
                             winrt::Microsoft::UI::Xaml::XamlRoot xamlRoot,
                             std::function<bool()> needsConfirm,
                             std::function<void()> onApproved)
{
    // Overlapping submit while a dialog is already up would stack a
    // second ContentDialog under the first — drop the newcomer.
    if (*m_pending) return;

    if (!needsConfirm || !needsConfirm()) {
        if (onApproved) onApproved();
        return;
    }

    // Without XamlRoot the ContentDialog has nothing to anchor to.
    // Fall back to approving so the close path still completes
    // (matches the pre-gate behaviour of TryClose).
    if (!xamlRoot) {
        if (onApproved) onApproved();
        return;
    }

    muxc::ContentDialog dlg;
    dlg.XamlRoot(xamlRoot);
    dlg.Title(winrt::box_value(DialogTitle(scope)));
    dlg.Content(winrt::box_value(DialogBody(scope)));
    dlg.PrimaryButtonText(L"Close");
    dlg.CloseButtonText(L"Cancel");
    dlg.DefaultButton(muxc::ContentDialogButton::Close);

    *m_pending = true;
    auto op = dlg.ShowAsync();
    op.Completed(
        [pending = m_pending, approved = std::move(onApproved)]
        (auto&& sender, auto&& status)
    {
        // Reset before firing so an approval callback that triggers
        // another close (e.g. last-tab-close → close window) sees
        // IsIdle(). The pending flag lives in a shared_ptr so this
        // lambda stays safe when the gate has already been destroyed
        // (dialog can outlive MainWindow on abrupt teardown).
        *pending = false;
        if (status != winrt::Windows::Foundation::AsyncStatus::Completed) return;
        if (sender.GetResults() != muxc::ContentDialogResult::Primary) return;
        if (approved) approved();
    });
}

}  // namespace winrt::GhosttyWin32::implementation
