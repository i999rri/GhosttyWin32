#pragma once

#include "Ghostty/Actions/Tags/CellSize.h"
#include "Ghostty/Actions/Tags/SizeLimit.h"
#include <windows.h>

namespace core::win32 {

// The HWND side of a top-level window: everything the host does to
// a window through Win32 that has to stay resident on the HWND — a
// message subclass, a saved placement. The values it enforces come
// from the ghostty action tags (SizeLimit, CellSize), which are pure
// and decide; this class only carries their decisions out.
//
// Lifetime follows the HWND, which arrives late: a tear-out host
// adopts its tab (and so learns its size rules) before its first
// Activated assigns the handle. Rules set before Bind are kept and
// installed the moment the handle is known, so callers never have to
// order "set" against "bind". Win32 removes subclasses when the HWND
// is destroyed; the destructor also removes them if the window is
// still alive, so the subclass never outlives the object it points
// at.
//
// One subclass proc serves both size rules: WM_GETMINMAXINFO for the
// limit, WM_SIZING for cell snapping. Maximize and fullscreen never
// enter the interactive sizing loop, so no guard is needed there.
class NativeWindow {
public:
    NativeWindow() = default;
    ~NativeWindow();
    NativeWindow(const NativeWindow&) = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;

    // Attach to the window. Installs the subclass now if any rule
    // was set earlier. A null handle detaches.
    void Bind(HWND hwnd) noexcept;
    HWND Handle() const noexcept { return m_hwnd; }

    // Enforce `limit` on WM_GETMINMAXINFO. Copies the value; later
    // calls replace it without re-installing.
    void SetSizeLimit(ghostty::actions::tags::SizeLimit const& limit) noexcept;
    // Snap interactive resizing per `cells` on WM_SIZING (only while
    // it says so — see CellSize::Snaps). Copies the value.
    void SetCellSnap(ghostty::actions::tags::CellSize const& cells) noexcept;

    // Span the monitor without chrome, remembering the placement and
    // style to come back to; LeaveFullscreen restores them —
    // including a maximised state (RECT alone would lose that,
    // WINDOWPLACEMENT round-trips it). Both no-op without a handle;
    // Enter while already in, or Leave while out, is a no-op too.
    void EnterFullscreen() noexcept;
    void LeaveFullscreen() noexcept;
    bool InFullscreen() const noexcept { return m_fullscreen; }

private:
    static LRESULT CALLBACK SubclassProc(
        HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR ref) noexcept;

    void EnsureSubclass() noexcept;
    void RemoveSubclass() noexcept;
    void OnGetMinMaxInfo(MINMAXINFO& mmi) const noexcept;
    bool OnSizing(WPARAM edge, RECT& drag) const noexcept;

    HWND m_hwnd = nullptr;
    bool m_subclassed = false;

    ghostty::actions::tags::SizeLimit m_sizeLimit;
    bool m_hasSizeLimit = false;
    ghostty::actions::tags::CellSize m_cellSnap;

    bool m_fullscreen = false;
    WINDOWPLACEMENT m_prevPlacement{};
    LONG_PTR m_prevStyle = 0;
};

}  // namespace core::win32
