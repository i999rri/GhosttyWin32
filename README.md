# GhosttyWin32

Windows host application for [Ghostty](https://github.com/ghostty-org/ghostty) terminal emulator.

Uses the libghostty C API (DLL) with a Win32 window + OpenGL rendering.

## Architecture

```
GhosttyWin32 (C++/Win32)
  └── GhosttyBridge: libghostty C API wrapper
      ├── Win32 window + WGL OpenGL context
      ├── Keyboard input (WM_CHAR + WM_KEYDOWN → ghostty_surface_text/key)
      ├── Mouse input (click, drag, scroll)
      ├── Clipboard (Ctrl+C/V, right-click copy)
      └── ConPTY → cmd.exe/PowerShell

ghostty.dll (Zig)
  ├── Terminal emulator core
  ├── OpenGL renderer
  ├── Font rendering (Freetype + Harfbuzz)
  └── ConPTY subprocess management
```

## Building

### Prerequisites

- Visual Studio 2022+ with C++ desktop development workload
- Zig 0.15.2+
- Windows SDK

### Build ghostty.dll

```bash
cd path/to/ghostty
zig build -Doptimize=ReleaseFast
```

Copy the following files to `GhosttyWin32/ghostty/`:
- `zig-out/lib/ghostty.dll`
- `.zig-cache/o/<hash>/ghostty.lib` (small import library ~60KB)
- `include/ghostty.h`

### Build GhosttyWin32

Open `GhosttyWin32.slnx` in Visual Studio, set to **Release | x64**, and build.

## Status

This is an experimental Windows port. See [ghostty-windows-port.md](https://github.com/i999rri/ghostty/blob/main/ghostty-windows-port.md) for detailed progress.

## AI Disclosure

Claude Code was used to assist with development.
