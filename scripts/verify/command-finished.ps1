<#
.SYNOPSIS
    Fire a COMMAND_FINISHED notification by hand (OSC 133 prompt marks).

.DESCRIPTION
    Command tracking in libghostty needs the shell-integration marks
    OSC 133;C (command start) and OSC 133;D;<exit> (command end). Shell
    integration is not active in this port (no resources dir), so a
    plain `sleep 6` never notifies — emit the marks yourself instead.

    Run inside a GhosttyWin32 pane. The notification fires only when
    the pane is NOT focused at the moment the "command" ends, so start
    it, then click another pane or alt-tab away before it finishes.

    Config (ghostty config, then reload with ctrl+shift+,):

        notify-on-command-finish = unfocused      # or: always
        notify-on-command-finish-action = bell,notify
        # notify-on-command-finish-after = 5s     # default; the script waits longer

.PARAMETER Seconds
    How long the fake command "runs". Must exceed notify-on-command-finish-after.

.PARAMETER ExitCode
    Exit code reported in the D mark. 0 -> "Command Succeeded", non-zero -> "Command Failed".

.EXAMPLE
    .\scripts\verify\command-finished.ps1
    # then alt-tab away within 6 seconds -> bell + toast "Command Failed / Finished in 6s (exit 2)"

.EXAMPLE
    .\scripts\verify\command-finished.ps1 -ExitCode 0
    # -> "Command Succeeded"

.NOTES
    Expected results:
      - focused the whole time  : nothing (with `unfocused`)
      - other pane / other app  : bell + toast
      - toast click             : the originating tab / pane is selected
    Duration shorter than the threshold: nothing.
#>
param(
    [int] $Seconds = 6,
    [int] $ExitCode = 2
)

$esc = [char]27
Write-Host -NoNewline ($esc + ']133;C' + $esc + '\')
Start-Sleep -Seconds $Seconds
Write-Host -NoNewline ($esc + ']133;D;' + $ExitCode + $esc + '\')
