# Verification scripts

One file per manual check that needs more than a keybind. Each script's header says what it verifies, which config it needs, and what to expect — run it inside a GhosttyWin32 pane unless it says otherwise.

| Script | Verifies |
|---|---|
| `clipboard-osc52.sh` | #224: whether a program in the pane can read the Windows clipboard with OSC 52. Needs a pane whose pty is not ConPTY's |
| `command-finished.ps1` | `notify-on-command-finish` without shell integration, via hand-written OSC 133 marks. See [docs/NOTIFICATIONS.md](../../docs/NOTIFICATIONS.md) |
| `wsl-bridge-protocol.py` | #206: the in-distro half of the bridge, driven over `wsl.exe` the way the Windows-side `Pty` drives it |
| `wsl-bridge-routing.sh` | Which path a WSL tab is on, the bridge or ConPTY. Both answers come from inside the tab |
| `wsl-exec-bit.ps1` | #229: a file inside an installed MSIX cannot be run from WSL, and one in a directory the user owns can |
| `wsl-stdio-fidelity.py` | #206: whether `wsl.exe`'s redirected stdio relays bytes untouched in both directions |
| `wsl-tab-close-under-load.ps1` | #228: whether closing a tab whose output is still flowing hangs the window |

When a pull request's verification steps need a script, add it here and link it from the PR instead of pasting the one-liner into the body.
