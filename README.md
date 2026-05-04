# GhosttyWin32

Windows host application for [Ghostty](https://github.com/ghostty-org/ghostty) terminal emulator.

Uses the libghostty C API (DLL) with a Win32 window. Supports **DirectX 11** (default) and **OpenGL** (via [Mesa Zink](https://docs.mesa3d.org/drivers/zink.html)) rendering backends.

## Features

### Terminal
- Full terminal emulation via libghostty (VT parser, ConPTY)
- PowerShell / cmd.exe / custom shell (`command` config)
- Japanese/CJK font rendering with system font fallback
- IME input (Japanese, Chinese, Korean)
- UTF-16 surrogate pair support (emoji)

### Input
- Keyboard: WM_CHAR text input + WM_KEYDOWN special keys
- Modifier keys: Ctrl, Shift, Alt (sent to ghostty for keybindings)
- Mouse: left/middle/right click, drag, scroll wheel
- Text selection: left-click drag, Ctrl+C copy, Ctrl+V paste, right-click copy
- Selection auto-clear after copy
- Ctrl+click: URL highlight detection

### Window Management
- Fullscreen toggle (Ctrl+Enter)
- Maximize/restore
- Window decorations toggle (title bar show/hide)
- Borderless mode with drag (top 30px) and edge resize (WM_NCHITTEST)
- Minimum/maximum window size limits (SIZE_LIMIT)
- DPI scaling (Per-Monitor DPI Aware V2)

### Visual
- Theme support (config file or `%LOCALAPPDATA%\ghostty\themes\`)
- Background image (`background-image` config)
- Background opacity (`background-opacity` config)
- Title bar color synced with terminal background (DwmSetWindowAttribute, Windows 11)
- Custom color palette (ANSI 16 colors)
- Cursor color customization

### Rendering
- **DirectX 11** (default) — native D3D11, no external dependencies, FLIP_DISCARD swap chain
- **OpenGL 4.3+** (fallback) — via Mesa Zink (GL-to-Vulkan translation)
- High-resolution waitable timer for frame pacing (Windows 10 1803+)
- V-Sync OFF for low input latency

### Callbacks
- `SET_TITLE`: window title from terminal
- `MOUSE_SHAPE`: cursor changes (text, pointer, hand, resize, etc.)
- `MOUSE_VISIBILITY`: hide/show cursor
- `OPEN_URL`: open URLs in default browser (ShellExecuteW)
- `RING_BELL`: flash window + system beep
- `COLOR_CHANGE`: sync title bar color with background
- `TOGGLE_FULLSCREEN` / `TOGGLE_MAXIMIZE` / `TOGGLE_WINDOW_DECORATIONS`
- `SIZE_LIMIT` / `INITIAL_SIZE` / `RESET_WINDOW_SIZE`
- `QUIT`: clean shutdown
- Clipboard read/write (onReadClipboard, onWriteClipboard)
- Wakeup: thread-safe PostMessage to main thread

## Architecture

```
GhosttyWin32.exe (C++/Win32)
  └── GhosttyBridge (singleton)
      ├── Win32 window (WS_POPUP or WS_OVERLAPPEDWINDOW)
      ├── Input handling (keyboard, mouse, IME)
      ├── Clipboard (Win32 API)
      ├── Config reading (ghostty_config_get API)
      ├── Action dispatch (onAction callback)
      └── 4MB stack threads (ghostty_init, config, surface)

ghostty.dll (Zig, from i999rri/ghostty windows-port branch)
  ├── Terminal emulator core (VT parser, Screen)
  ├── DirectX 11 renderer (HLSL SM5.0 shaders, d3d11_impl.c COM wrapper)
  ├── OpenGL 4.3 renderer (GLSL 430 shaders, Mesa Zink fallback)
  ├── Font rendering (Freetype + Harfbuzz)
  ├── ConPTY subprocess management
  └── Windows font discovery (registry lookup, %WINDIR%\Fonts)

[Optional] Mesa Zink (opengl32.dll + libgallium_wgl.dll)
  └── Translates OpenGL 4.6 → Vulkan (only needed for OpenGL fallback)
```

## Install

### Scoop (Recommended)

```powershell
scoop bucket add ghostty https://github.com/i999rri/scoop-bucket
scoop install ghosttywin32
```

Then run from terminal or start menu:
```powershell
GhosttyWin32
```

### Manual (MSIX)

1. Download both `Ghostty-X.Y.Z-x64.msix` and `Ghostty.cer` from
   [Releases](https://github.com/i999rri/GhosttyWin32/releases).
2. Right-click `Ghostty.cer` → Install Certificate → **Local Machine** → place
   in **Trusted People** (UAC prompt). This is a one-time step per machine; the
   self-signed publisher certificate has to be trusted before Windows will
   sideload the MSIX.
3. Double-click the `.msix` to install.

Subsequent updates only require step 3.

## Building from Source

### Prerequisites

- Visual Studio 2022+ with C++ desktop development workload
- Zig 0.15.2+
- Windows SDK

### Build ghostty.dll

This requires a forked version of Ghostty with Windows support patches:

```bash
git clone https://github.com/i999rri/ghostty.git
cd ghostty
git switch windows-port
zig build -Doptimize=ReleaseSafe -Drenderer=directx
```

Copy the following files to `GhosttyWin32/ghostty/`:
- `zig-out/lib/ghostty.dll`
- `.zig-cache/o/<hash>/ghostty.lib` (small import library ~60KB)
- `include/ghostty.h` (already included in this repo)

### Build GhosttyWin32

Open `GhosttyWin32.slnx` in Visual Studio, set to **Release | x64**, and build.

### Mesa Zink Setup (Optional, for OpenGL fallback)

Only needed if using the OpenGL renderer instead of DirectX.

Download [mesa-dist-win](https://github.com/pal1000/mesa-dist-win/releases) (MSVC release) and copy to the exe directory:
- `x64/opengl32.dll`
- `x64/libgallium_wgl.dll`

### Run

```bash
GhosttyWin32.exe
```

## Renderer Selection

DirectX 11 is the default renderer. To use OpenGL instead:

```powershell
$env:GHOSTTY_RENDERER = "opengl"
$env:GALLIUM_DRIVER = "zink"
GhosttyWin32.exe
```

| Environment Variable | Default | Description |
|---------------------|---------|-------------|
| `GHOSTTY_RENDERER` | (DirectX) | Set to `opengl` for OpenGL/Zink fallback |
| `GALLIUM_DRIVER` | — | Set to `zink` when using OpenGL renderer |

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

Theme files go in `%LOCALAPPDATA%\ghostty\themes\`. See [Ghostty documentation](https://ghostty.org/docs/config) for all options.

## Known Issues

- `exit` does not close window ([#8](https://github.com/i999rri/GhosttyWin32/issues/8))
- Ctrl+click URL causes process exit ([#12](https://github.com/i999rri/GhosttyWin32/issues/12)) — ghostty-side issue
- Mesa Zink DLLs trigger Windows Defender false positive ([#11](https://github.com/i999rri/GhosttyWin32/issues/11))
- OpenGL renderer: native WGL (without Mesa Zink) has flickering due to WGL+DWM issue

## Status

This is an experimental Windows port. See the [windows-port branch](https://github.com/i999rri/ghostty/tree/windows-port) for Ghostty-side changes and [Discussion #2563](https://github.com/ghostty-org/ghostty/discussions/2563) for context.

## AI Disclosure

Claude Code was used to assist with development.
