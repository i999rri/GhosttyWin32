# Verification scripts

One file per manual check that needs more than a keybind. Each script's header says what it verifies, which config it needs, and what to expect — run it inside a GhosttyWin32 pane unless it says otherwise.

| Script | Verifies |
|---|---|
| `command-finished.ps1` | `notify-on-command-finish` (COMMAND_FINISHED) without shell integration, via hand-emitted OSC 133 marks |

When a pull request's verification steps need a script, add it here and link it from the PR instead of pasting the one-liner into the body.
