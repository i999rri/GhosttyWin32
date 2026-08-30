# Core

Everything in this project that does not depend on the UI framework (WinUI 3 / XAML). That is the one line: `Tests.exe` links Core and nothing else, and it cannot spin up a XAML runtime, so whatever lives here is what can be tested without a window on screen. Win32 is the OS, not the framework — it is allowed here, and it is where the OS-facing effects go.

Inside that line there are three kinds of code, by directory:

| directory | what it is | may include |
|---|---|---|
| `Ghostty/` | libghostty as C++ values: the app and surface wrappers, `Config`, the action dispatch, and `Actions/Tags/` — one value per window-scoped action (`SizeLimit`, `CellSize`, `Fullscreen`, `WindowDecorations`, `BackgroundOpacity`) that holds the action's state and makes its decision, and nothing else | `ghostty.h`, the standard library. Not `<windows.h>` |
| `Host/` | the contracts between the terminal side and the window side (`IWindow`, `ISurfaceView`), and host logic that is pure by nature (`ImeBuffer`, `EngagementState`) | as above; interface headers may name Win32 handle types |
| `Win32/` | OS effects: `NativeWindow` (the HWND side of a top-level window — size-rule subclass, fullscreen placement), `Clipboard`, `SEHGuard`, `DebugTrace` | `<windows.h>`; may take the values from `Ghostty/Actions/Tags` to carry out |
| `Input/`, `Interop/`, `Display/` | translations: key events, text encodings, DPI | as needed |

The rule of thumb when adding something: if it decides, it is a value in `Ghostty/` (or `Host/`) with a test that needs no window; if it does something to an HWND, it is in `Win32/` and takes the decision as input; if it touches XAML, it is not Core — it belongs in `App/`.
