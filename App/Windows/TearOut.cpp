#include "pch.h"
#include "Windows/TearOut.h"
#include "Windows/MainWindow.xaml.h"
#include <winrt/Windows.Graphics.h>

namespace winrt::GhosttyWin32::implementation {

namespace {

// Every way ToNewWindow can return null is silent by design (the
// drag simply ends), which makes a tear-out that "did nothing" hard
// to tell apart from one that never ran. Debug builds say which it
// was.
#if defined(_DEBUG)
void Trace(wchar_t const* what) noexcept
{
    OutputDebugStringW(L"TearOut: ");
    OutputDebugStringW(what);
    OutputDebugStringW(L"\n");
}
#else
void Trace(wchar_t const*) noexcept {}
#endif

// Offsets place the window so its tab strip lands near the pointer
// instead of the window's top-left corner.
winrt::Windows::Graphics::PointInt32 HostPositionFor(POINT dropPoint) noexcept
{
    return { static_cast<int32_t>(dropPoint.x) - 120,
             static_cast<int32_t>(dropPoint.y) - 24 };
}

}  // namespace

MainWindow* TearOut::ToNewWindow(
    MainWindow& source,
    winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& item,
    std::optional<POINT> dropPoint,
    SpawnHost const& spawnHost)
{
    // Dragging out the only tab must not leave an empty shell behind
    // — just move this window to the drop point instead.
    if (source.TabCount() <= 1) {
        Trace(L"only tab in its window; moved the window instead of spawning");
        if (dropPoint) source.AppWindow().Move(HostPositionFor(*dropPoint));
        return nullptr;
    }
    if (!spawnHost) {
        Trace(L"no spawnHost supplied; nothing to tear into");
        return nullptr;
    }
    auto* host = spawnHost(source.State());
    if (!host) {
        Trace(L"spawnHost returned null; tab stays where it is");
        return nullptr;
    }
    try {
        auto tab = source.ReleaseTornOutTab(item);
        if (!tab) {
            // Nothing moved; don't leak an empty host.
            Trace(L"source does not own the dragged item; closing the empty host");
            host->RequestClose();
            return nullptr;
        }
        host->AdoptTornOutTab(std::move(tab), -1);
        if (dropPoint) host->AppWindow().Move(HostPositionFor(*dropPoint));
        // The drag is over, so activation is safe — hand the new
        // window focus like a browser does after a tab is torn off.
        host->Activate();
        return host;
    } catch (winrt::hresult_error const&) {
        // A failed move must not take either window down; whichever
        // side holds the unique_ptr owns the tab.
        Trace(L"adopt threw; whichever side holds the tab keeps it");
        return nullptr;
    }
}

}  // namespace winrt::GhosttyWin32::implementation
