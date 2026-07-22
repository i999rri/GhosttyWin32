# GhosttyWin32

Windows host application for the [Ghostty](https://github.com/ghostty-org/ghostty) terminal emulator.

A WinUI 3 + C++/WinRT shell that hosts the libghostty C API. Each pane owns its own ghostty Surface and DirectX 11 device, rendered into a `SwapChainPanel`. Multi-window, multi-tab, and split-pane are all first-class.

## Features

### Terminal

- Full terminal emulation via libghostty (VT parser, ConPTY backend)
- PowerShell / cmd.exe / arbitrary shell (`command` config option)
- Japanese / CJK glyph rendering with system font fallback
- IME input (Japanese, Chinese, Korean) with composition preview
- UTF-16 surrogate pair support (emoji, supplementary planes)
- Auto-close tab / pane when the shell process exits

### Windows

- **Multiple top-level windows** — `Ctrl+Shift+N` (or `new_window` action) spawns a fresh window; each carries its own tab strip and pane tree
- Cross-window focus, clipboard, and notification routing are per-surface, not per-app
- Overlap-flicker-free: renderer-side focus / occlusion wiring keeps stacked windows from re-arming each other's blink cycle

### Tabs

- Multi-tab UI via WinUI 3 `TabView`
- Per-tab isolation: each tab owns its own ghostty Surface(s) + D3D11 device
- Per-tab SEH guard: a hardware exception in one tab does not take down siblings
- New / close / reorder / go-to via standard `TabView` gestures and keybinds
- **Drag a tab out to spawn a new window at the drop point** (release-time drag & drop)
- **Drag a tab onto another window's strip to merge it there** — surfaces / swap chains / split-panel trees stay live across the move (`TerminalControl::Rehost` re-points only the host HWND + focused callback)
- Custom XAML caption buttons that route through `WM_SYSCOMMAND` to avoid driver-side state-change crashes

### Splits

- Horizontal / vertical splits inside any tab
- Focus navigation between panes (`goto_split_up/down/left/right`)
- Resize, equalize, zoom / unzoom
- Each pane is a full ghostty Surface with its own D3D11 device, isolated behind the same SEH guard as tabs

### Input

- Keyboard: scan-code + text-field forwarding — accurate keybinds across non-Latin layouts, dead keys, and AltGr
- Modifier-aware keybindings (Ctrl / Shift / Alt) forwarded to libghostty
- Mouse: left / middle / right click, drag, scroll wheel
- Per-pane cursor shape from `MOUSE_SHAPE` (text under text, hand over URLs, etc.)
- Selection: drag-to-select, `Ctrl+C` copy, `Ctrl+V` paste, right-click copy; auto-clear after copy
- Ctrl+click a URL to open in the default browser

### Window chrome

- Custom title bar with `ExtendsContentIntoTitleBar`
- Drag region sized to survive many open tabs
- Fullscreen toggle, maximize / restore
- `TOGGLE_WINDOW_DECORATIONS` — 3-state per-window override on top of the config baseline
- DPI scaling (Per-Monitor DPI Aware V2); HiDPI-correct split sizing and text rendering
- Mica / Acrylic backdrop (when running as an MSIX package)

### Visual

- Theme support (config file or `%LOCALAPPDATA%\ghostty\themes\`)
- Background image (`background-image` config)
- Background opacity (`background-opacity` config)
- Title bar tint synced with terminal background (`DwmSetWindowAttribute`, Windows 11)
- Custom ANSI 16-color palette
- Cursor color customization

### Rendering

- DirectX 11 via the libghostty native renderer (no external dependencies)
- FLIP_DISCARD swap chain hosted in a `SwapChainPanel`
- Hybrid render cadence — focused surface polls every 4 ms; unfocused surfaces are event-driven (wakeup / blink / mailbox / 1 s safety net)
- HiDPI / RDP correct at first paint (composition scale followed via `SwapChainPanel::CompositionScaleChanged`)

### libghostty action coverage

63 of 65 upstream actions wired (as of v0.4.0). Highlights beyond the terminal core:

- **Windows**: `NEW_WINDOW`, `CLOSE_WINDOW`, `CLOSE_ALL_WINDOWS`, `QUIT`, `TOGGLE_FULLSCREEN`, `TOGGLE_MAXIMIZE`, `TOGGLE_WINDOW_DECORATIONS`, `PRESENT_TERMINAL`, `SIZE_LIMIT`, `INITIAL_SIZE`, `RESET_WINDOW_SIZE`
- **Tabs**: `NEW_TAB`, `CLOSE_TAB`, `GOTO_TAB`, `MOVE_TAB`
- **Splits**: `NEW_SPLIT`, `GOTO_SPLIT`, `RESIZE_SPLIT`, `EQUALIZE_SPLITS`, `TOGGLE_SPLIT_ZOOM`
- **Terminal**: `SET_TITLE`, `MOUSE_SHAPE`, `MOUSE_VISIBILITY`, `OPEN_URL` (`ShellExecuteW`), `RING_BELL` (flash + system beep), `COLOR_CHANGE`, clipboard read / write
- **System**: notification click → present-pane round-trip (targets the exact pane, not just the app), `SHOW_ON_SCREEN_KEYBOARD`
- Wakeup: thread-safe `PostMessage` to the UI thread

## Architecture

```
GhosttyWin32.exe (WinUI 3 / C++/WinRT)
  ├── App
  │   ├── owns core::ghostty::App (app-wide libghostty handle)
  │   ├── owns MainWindows aggregate (one entry per top-level window)
  │   ├── owns PaneIdAllocator (globally unique surface IDs)
  │   ├── SetUnhandledExceptionFilter — process-wide best-effort cleanup
  │   └── AppNotificationManager + single-instance activation redirect
  │
  ├── MainWindow  (one per top-level window)
  │   ├── Custom title bar + XAML caption buttons
  │   ├── TabView  (owns tab-strip UI)
  │   ├── AppContent  (grid parenting each tab's SplitPanel)
  │   ├── Tabs / TabFactory  (per-window tab bookkeeping)
  │   └── MainWindowRuntime  (IGhosttyRuntime impl per window)
  │
  ├── Tab
  │   └── SplitPanel  (tree of Pane leaves; each leaf hosts a TerminalControl)
  │
  └── TerminalControl  (WinUI UserControl per pane)
      ├── SwapChainPanel → ghostty Surface (own D3D11 device)
      ├── Input forwarding (key scan+text, pointer, IME EditContext)
      └── Per-pane SEH guard

ghostty.dll (Zig, from i999rri/ghostty windows-port branch — git submodule)
  ├── Terminal emulator core (VT parser, Screen)
  ├── DirectX 11 renderer (HLSL SM5.0, d3d11_impl.c COM wrapper)
  ├── Font rendering (Freetype + Harfbuzz)
  ├── ConPTY subprocess management
  └── Windows font discovery (registry lookup, %WINDIR%\Fonts)
```

## Install

The MSIX is signed only with a self-signed publisher certificate
(`CN=i999rri`) — an established CA / OSS Foundation signature isn't in
place yet (see [issue #46](https://github.com/i999rri/GhosttyWin32/issues/46)),
so Windows requires the certificate to be trusted before the MSIX will
install. Manual install is the only supported path today; a Scoop channel
existed for the pre-MSIX ZIP builds (v0.2.x) and will return once signing
is available.

1. Download `Ghostty-<version>-x64.msix` and `Ghostty.cer` from
   [Releases](https://github.com/i999rri/GhosttyWin32/releases).
2. Trust the certificate (one-time per machine; **Local Machine → Trusted
   People** store — see [docs/INSTALL.md](docs/INSTALL.md) for the exact
   wizard choices, since the wrong store choice silently fails with
   `0x800B0109`).
3. Double-click the `.msix` to install.

Subsequent updates only require step 3. Detailed walkthrough and
troubleshooting: [docs/INSTALL.md](docs/INSTALL.md).

## Building from Source

### Prerequisites

- Visual Studio 2022 (17.10+) with the "Desktop development with C++" and
  "Universal Windows Platform development" workloads
- Windows App SDK 1.6+ (installed via the project's NuGet packages)
- Zig 0.15.2+
- Windows SDK 10.0.22621.0+

### Build ghostty.dll

The forked Ghostty source lives as a git submodule under
`external/ghostty/`, pinned to the `windows-port` branch. Initialise
the submodule on first clone:

```bash
git submodule update --init --recursive
```

(Or pass `--recurse-submodules` to the original `git clone`.)

Build libghostty:

```bash
cd external/ghostty
zig build -Doptimize=ReleaseSafe -Drenderer=directx
# The DLL's import lib lands in .zig-cache instead of zig-out/lib/
# on shared Windows builds; surface it next to the DLL so the
# vcxprojs can link straight from external/ghostty/zig-out/lib/.
find .zig-cache -path '*/o/*/ghostty.lib' -size +10k -size -100k \
    -exec cp {} zig-out/lib/ghostty-internal.lib \;
cd ../..
```

Both the App / Tests vcxprojs reference
`$(ProjectDir)..\external\ghostty\zig-out\lib` for linker input and
`$(ProjectDir)..\external\ghostty\include` for `ghostty.h`, so the
submodule pin is the single source of truth for both the C API and
the binary artifacts — no separate output directory at the repo
root is needed.

**Trying a different ghostty branch during development**: switch
inside the submodule and rebuild — the parent repo only notices
the change when you `git add external/ghostty`, so you can
experiment freely and either commit the pin update or
`git submodule update --recursive` to snap back.

```bash
cd external/ghostty
git switch feat/dx-p3-colorspace   # or any other branch
zig build -Doptimize=ReleaseSafe -Drenderer=directx
find .zig-cache -path '*/o/*/ghostty.lib' -size +10k -size -100k \
    -exec cp {} zig-out/lib/ghostty-internal.lib \;
cd ../..
```

### Build GhosttyWin32

Open `GhosttyWin32.slnx` in Visual Studio, select **Release | x64**, and
build the `App` project. F5 deploys as a packaged MSIX into
the local appx registry; `Release | x64` build artifacts land under
`x64/Release/App/`.

## Configuration

Create `%LOCALAPPDATA%\ghostty\config`:

```ini
font-size=15
command=powershell
confirm-close-surface=false
theme=catppuccin-mocha
window-decoration=false
background-opacity=0.85
background-image=C:/Users/you/path/to/image.png
background-image-opacity=0.3
background-image-fit=cover
```

Theme files go in `%LOCALAPPDATA%\ghostty\themes\`. See the upstream
[Ghostty documentation](https://ghostty.org/docs/config) for the full
option list.

## Known Issues

- Windows-specific config options (`windows-tab-bar`, `windows-drag-region`)
  are not exposed yet ([#17](https://github.com/i999rri/GhosttyWin32/issues/17))
- `MOUSE_OVER_LINK`: the Win32 tooltip path crashes on URL click
  ([#61](https://github.com/i999rri/GhosttyWin32/issues/61))
- `Ctrl+Shift+0` keybind is swallowed when the Japanese IME / TSF is active
  ([#83](https://github.com/i999rri/GhosttyWin32/issues/83))
- Occasional `RO_E_CLOSED` on tab close after some operation sequences
  ([#87](https://github.com/i999rri/GhosttyWin32/issues/87))

## Status

Windows port of Ghostty. See the
[windows-port branch](https://github.com/i999rri/ghostty/tree/windows-port) for
Ghostty-side changes and
[Discussion #2563](https://github.com/ghostty-org/ghostty/discussions/2563) for
upstream context.

## License

[MIT](LICENSE). The libghostty library it embeds is also MIT-licensed.

## AI Disclosure

Claude Code was used to assist with development.
