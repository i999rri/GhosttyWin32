# GhosttyWin32

Windows host application for the [Ghostty](https://github.com/ghostty-org/ghostty) terminal emulator.

A WinUI 3 + C++/WinRT shell that hosts the libghostty C API. Each tab owns its own ghostty Surface and DirectX 11 device, rendered into a `SwapChainPanel`.

## Features

### Terminal

- Full terminal emulation via libghostty (VT parser, ConPTY backend)
- PowerShell / cmd.exe / arbitrary shell (`command` config option)
- Japanese / CJK glyph rendering with system font fallback
- IME input (Japanese, Chinese, Korean) with composition preview
- UTF-16 surrogate pair support (emoji, supplementary planes)
- Auto-close tab when the shell process exits

### Tabs

- Multi-tab UI via WinUI 3 `TabView`
- Per-tab isolation: each tab owns its own ghostty Surface + D3D11 device
- Per-tab SEH guard: a hardware exception in one tab does not take down siblings (fatal cases still trigger a clean process exit)
- New tab / close tab / reorder via standard `TabView` gestures
- Custom XAML caption buttons (minimize / maximize / close) that route through `WM_SYSCOMMAND` to avoid driver-side state-change crashes

### Input

- Keyboard: `WM_CHAR` text input plus key-level events for shortcuts
- Modifier-aware keybindings (Ctrl / Shift / Alt) forwarded to libghostty
- Mouse: left / middle / right click, drag, scroll wheel
- Selection: drag-to-select, Ctrl+C copy, Ctrl+V paste, right-click copy
- Selection auto-clear after copy
- Ctrl+click: URL highlight detection (see Known Issues)

### Window

- Custom title bar with `ExtendsContentIntoTitleBar`
- Drag region sized to survive many open tabs
- Fullscreen toggle, maximize / restore
- DPI scaling (Per-Monitor DPI Aware V2)
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
- High-resolution waitable timer for frame pacing (Windows 10 1803+)

### libghostty Callbacks

- `SET_TITLE`: tab title from terminal escape sequences
- `MOUSE_SHAPE`: cursor changes (text, pointer, hand, resize, etc.)
- `MOUSE_VISIBILITY`: hide / show cursor
- `OPEN_URL`: open URLs in the default browser (`ShellExecuteW`)
- `RING_BELL`: flash window + system beep
- `COLOR_CHANGE`: sync title bar tint with terminal background
- `TOGGLE_FULLSCREEN` / `TOGGLE_MAXIMIZE`
- `SIZE_LIMIT` / `INITIAL_SIZE` / `RESET_WINDOW_SIZE`
- `NEW_TAB` / `CLOSE_TAB` / `GOTO_TAB` / `MOVE_TAB`
- `PRESENT_TERMINAL`: restore + foreground the window
- `SHOW_ON_SCREEN_KEYBOARD`: launch the Windows OSK
- `QUIT`: clean shutdown
- Clipboard read / write
- Wakeup: thread-safe `PostMessage` to the UI thread

## Architecture

```
GhosttyWin32.exe (WinUI 3 / C++/WinRT)
  ├── App.xaml — application entry, XAML Controls Resources
  ├── MainWindow
  │   ├── Custom title bar + caption buttons
  │   ├── TabView
  │   │   └── TerminalControl (per tab)
  │   │       ├── SwapChainPanel → ghostty Surface
  │   │       ├── Input forwarding (key / pointer / IME)
  │   │       └── Per-tab D3D11 device + SEH guard
  │   └── SetUnhandledExceptionFilter — best-effort cleanup
  ├── GhosttyApp — ghostty_app_t lifetime wrapper (config, callbacks)
  └── Tabs / TabFactory / TabIdAllocator — tab bookkeeping

ghostty.dll (Zig, from i999rri/ghostty windows-port branch)
  ├── Terminal emulator core (VT parser, Screen)
  ├── DirectX 11 renderer (HLSL SM5.0, d3d11_impl.c COM wrapper)
  ├── Font rendering (Freetype + Harfbuzz)
  ├── ConPTY subprocess management
  └── Windows font discovery (registry lookup, %WINDIR%\Fonts)
```

## Install

### Scoop (recommended)

```powershell
scoop bucket add ghostty https://github.com/i999rri/scoop-bucket
scoop install ghosttywin32
```

**v0.2.x (current latest release):** the bucket ships a portable ZIP. No
elevation required.

**v0.3.0 and later (in development):** the bucket will ship a signed MSIX.
Installation imports the signing certificate into
`LocalMachine\TrustedPeople` and registers the MSIX via `Add-AppxPackage`,
so `scoop install` has to run from an elevated PowerShell. See
[issue #46](https://github.com/i999rri/GhosttyWin32/issues/46) for the
in-flight migration to SignPath Foundation, which will remove the
elevation requirement.

### Manual install

**v0.2.x:** download `GhosttyWin32-v<version>-x64.zip` from
[Releases](https://github.com/i999rri/GhosttyWin32/releases) and unzip
anywhere.

**v0.3.0 and later:**

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

Build and copy the link-time artifacts the host needs:

```bash
cd external/ghostty
zig build -Doptimize=ReleaseSafe -Drenderer=directx
cd ../..
cp external/ghostty/zig-out/lib/ghostty.{dll,lib,pdb} ghostty/
```

`ghostty/` holds the linker artifacts (`.dll` / `.lib` / `.pdb`) and is
gitignored. `ghostty.h` is **not** copied — App / Core / Tests include
it directly from `external/ghostty/include/` via the vcxproj's
`AdditionalIncludeDirectories`, so the submodule pin is always the
source of truth for the C API.

**Trying a different ghostty branch during development**: switch
inside the submodule and rebuild — the parent repo only notices
the change when you `git add external/ghostty`, so you can
experiment freely and either commit the pin update or
`git submodule update --recursive` to snap back.

```bash
cd external/ghostty
git switch feat/dx-p3-colorspace   # or any other branch
zig build -Doptimize=ReleaseSafe -Drenderer=directx
cd ../..
cp external/ghostty/zig-out/lib/ghostty.{dll,lib,pdb} ghostty/
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

- Ctrl+click on a URL exits the process with code 3
  ([#12](https://github.com/i999rri/GhosttyWin32/issues/12)) — ghostty-side issue
- Split panes are not yet implemented; only tabs
  ([#13](https://github.com/i999rri/GhosttyWin32/issues/13))
- Windows-specific config options (`windows-tab-bar`, `windows-drag-region`)
  are not exposed yet ([#17](https://github.com/i999rri/GhosttyWin32/issues/17))

## Status

This is an experimental Windows port. See the
[windows-port branch](https://github.com/i999rri/ghostty/tree/windows-port) for
Ghostty-side changes and
[Discussion #2563](https://github.com/ghostty-org/ghostty/discussions/2563) for
context.

## License

[MIT](LICENSE). The libghostty library it embeds is also MIT-licensed.

## AI Disclosure

Claude Code was used to assist with development.
