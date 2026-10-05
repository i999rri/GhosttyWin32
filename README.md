# GhosttyWin32

A Windows host for the [Ghostty](https://github.com/ghostty-org/ghostty) terminal emulator.

The terminal itself is libghostty, upstream's own code, embedded through its C API. What this repository adds is everything Windows asks for around it: a WinUI 3 and C++/WinRT shell where each pane owns a ghostty surface and its own DirectX 11 device, rendered into a `SwapChainPanel`. Multiple windows, tabs and split panes are all first-class, and a WSL pane can run on a Linux pty instead of on Windows' pseudo console.

[What it can do](docs/FEATURES.md) is the list, with what it cannot yet at the end of the same page.

## Install

The MSIX is signed with a self-signed publisher certificate (`CN=i999rri`); a CA or OSS Foundation signature is not in place yet ([#46](https://github.com/i999rri/GhosttyWin32/issues/46)), so Windows wants that certificate trusted before it will install the package.

1. Download `Ghostty-<version>-x64.msix` and `Ghostty.cer` from [Releases](https://github.com/i999rri/GhosttyWin32/releases).
2. Trust the certificate, once per machine, into **Local Machine → Trusted People**. The wrong store fails later with `0x800B0109` and no explanation, so [docs/INSTALL.md](docs/INSTALL.md) walks the wizard choices.
3. Double-click the `.msix`.

Updates need only step 3.

## Configuration

The config file is `%LOCALAPPDATA%\ghostty\config`, and themes go in `%LOCALAPPDATA%\ghostty\themes\`. The keys are upstream's, so the [configuration reference](https://ghostty.org/docs/config) is the list.

```ini
font-size = 15
command = powershell
theme = catppuccin-mocha
background-opacity = 0.85
```

Three of them behave in ways that reference cannot tell you about on Windows, and each has a page here: the [clipboard](docs/CLIPBOARD.md) settings differ per pane, [command-finished notifications](docs/NOTIFICATIONS.md) have no trigger yet, and the [WSL bridge](docs/WSL.md) needs a binary installed inside the distribution.

## Documentation

| Page | What is on it |
| --- | --- |
| [FEATURES.md](docs/FEATURES.md) | What works, and what does not yet |
| [INSTALL.md](docs/INSTALL.md) | Trusting the certificate, and what the failures mean |
| [BUILD.md](docs/BUILD.md) | Building from source, in the order the two builds have to go |
| [WSL.md](docs/WSL.md) | The bridge: what it is for, how to install its helper, how to check which path a tab is on |
| [CLIPBOARD.md](docs/CLIPBOARD.md) | Why a setting applies in one pane and not another |
| [NOTIFICATIONS.md](docs/NOTIFICATIONS.md) | Command-finished notifications, and why they do not fire on their own |
| [KEYBINDS.md](docs/KEYBINDS.md) | The key combinations Windows takes before this app sees them, and how to tell |
| [ASAN.md](docs/ASAN.md) | Running under AddressSanitizer |

## Shape of it

```
GhosttyWin32.exe (WinUI 3 / C++/WinRT)
  ├── App
  │   ├── core::ghostty::App        app-wide libghostty handle
  │   ├── MainWindows               one entry per top-level window
  │   ├── PaneIdAllocator           globally unique surface ids
  │   └── notifications, single-instance activation, crash cleanup
  │
  ├── MainWindow                    one per top-level window
  │   ├── title bar and caption buttons
  │   ├── TabView                   the tab strip
  │   ├── Tabs / TabFactory         per-window tab bookkeeping
  │   └── MainWindowRuntime         this window's libghostty callbacks
  │
  ├── Tab
  │   └── SplitPanel                tree of panes, each hosting a control
  │
  └── TerminalControl               one per pane
      ├── SwapChainPanel → ghostty surface, own D3D11 device
      ├── input forwarding          key scan + text, pointer, IME
      └── its own SEH guard

ghostty.dll (Zig, i999rri/ghostty windows-port, a submodule)
  ├── terminal core                 VT parser, screen, scrollback
  ├── DirectX 11 renderer
  ├── font rendering and Windows font discovery
  ├── ConPTY subprocesses
  └── WSL pty bridge                pkg/wsl, fork-only
```

## Status

A Windows port, tracked here. The Ghostty-side changes live on the [windows-port branch](https://github.com/i999rri/ghostty/tree/windows-port) of the fork, and [Discussion #2563](https://github.com/ghostty-org/ghostty/discussions/2563) is the upstream context for a Windows port at all.

## License

[MIT](LICENSE). libghostty, which this embeds, is MIT as well.

## AI disclosure

Claude Code was used to assist with development.
