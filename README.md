# GhosttyWin32

Windows host application for [Ghostty](https://github.com/ghostty-org/ghostty) terminal emulator.

Uses the libghostty C API (DLL) with a Win32 window + OpenGL rendering via [Mesa Zink](https://docs.mesa3d.org/drivers/zink.html) (GL-to-Vulkan translation).

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
- OpenGL 4.3+ via Mesa Zink (GL-to-Vulkan)
- No flickering (Vulkan presentation engine)
- V-Sync OFF for low input latency
- Immediate app_tick after text input

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
      ├── WGL OpenGL context → Mesa Zink → Vulkan → GPU
      ├── Input handling (keyboard, mouse, IME)
      ├── Clipboard (Win32 API)
      ├── Config reading (ghostty_config_get API)
      ├── Action dispatch (onAction callback)
      └── 4MB stack threads (ghostty_init, config, surface)

ghostty.dll (Zig, from i999rri/ghostty windows-port branch)
  ├── Terminal emulator core (VT parser, Screen)
  ├── OpenGL 4.3 renderer (GLSL 430 shaders)
  ├── Font rendering (Freetype + Harfbuzz)
  ├── ConPTY subprocess management
  └── Windows font discovery (registry lookup)

Mesa Zink (opengl32.dll + libgallium_wgl.dll)
  └── Translates OpenGL 4.6 → Vulkan (GPU native driver)
```

## Building

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
zig build -Doptimize=ReleaseSafe   # ReleaseSafe for PDB symbols
```

Copy the following files to `GhosttyWin32/ghostty/`:
- `zig-out/lib/ghostty.dll`
- `.zig-cache/o/<hash>/ghostty.lib` (small import library ~60KB)
- `include/ghostty.h` (already included in this repo)

### Build GhosttyWin32

Open `GhosttyWin32.slnx` in Visual Studio, set to **Release | x64**, and build.

### Mesa Zink Setup

Download [mesa-dist-win](https://github.com/pal1000/mesa-dist-win/releases) (MSVC release) and copy to the exe directory:
- `x64/opengl32.dll`
- `x64/libgallium_wgl.dll`

Set environment variable before launching:
```
set GALLIUM_DRIVER=zink
```

### Run

```bash
set GALLIUM_DRIVER=zink
GhosttyWin32.exe
```

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
- Input latency with Mesa Zink (GL-to-Vulkan translation overhead)
- Native OpenGL (without Mesa Zink) has flickering due to WGL+DWM issue

## Status

This is an experimental Windows port. See the [windows-port branch](https://github.com/i999rri/ghostty/tree/windows-port) for Ghostty-side changes and [Discussion #2563](https://github.com/ghostty-org/ghostty/discussions/2563) for context.

## AI Disclosure

Claude Code was used to assist with development.
