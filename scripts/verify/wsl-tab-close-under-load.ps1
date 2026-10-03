# Check for the deadlock a review of the WSL bridge found by reading it:
# closing a tab whose output is still flowing can hang the window.
#
# Pty.deinit joins the output pump (pkg/wsl/bridge/Pty.zig:279) before it
# closes the read end of the pipe the pump writes into (:287). Once termio
# stops draining, the pump blocks in WriteFile and the join waits for a
# thread only that close can release. `yes` keeps the pipe full, so it is
# the shape that reaches it.
#
# Run this outside the app, in Windows Terminal or any other pwsh. On v0.8.2
# a WSL tab means `command = wsl` in the config, which makes every tab a WSL
# one, so there is no pwsh tab left to run this in -- and a checker the test
# might close is no checker. (The shim that swaps a single pane is #221.)
#
# The subject is the `yes` process and the session that holds it, found from
# `yes` itself rather than from counting tabs: on a developer's machine WSL
# is usually already running for something else, and which tab was opened
# when is not something this can see. The window is judged by
# IsHungAppWindow, since a teardown that merely takes a while looks the same
# from the outside.

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Hung {
    [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
}
'@

# How long a teardown may reasonably take before the window is called hung.
$kWaitSeconds = 15

function Get-AppWindow {
    Get-Process -Name GhosttyWin32 -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}

# pgrep matches process names only up to 15 characters and
# ghostty-wsl-bridge is longer, so match the whole command line. The pattern
# is passed as its own argument: an alternation would have to survive the
# shell wsl.exe hands the command to.
function Get-DistroPids([string] $pattern) {
    try {
        $out = & wsl.exe -- pgrep -f -- $pattern 2>&1
        if ($LASTEXITCODE -le 1) {
            return @($out | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ })
        }
    } catch { }
    @()
}

function Test-DistroPid([int] $processId) {
    & wsl.exe -- kill -0 $processId 2>&1 | Out-Null
    $LASTEXITCODE -eq 0
}

function Get-Ancestry([int] $processId) {
    # Walk up to init so the session holding `yes` is named, whatever sits
    # between them.
    $chain = @()
    $cur = $processId
    for ($i = 0; $i -lt 8 -and $cur -gt 1; $i++) {
        $line = (& wsl.exe -- ps -o ppid=,comm= -p $cur 2>&1 | Select-Object -First 1)
        if ($LASTEXITCODE -ne 0 -or -not $line) { break }
        $parts = ($line.Trim() -split '\s+', 2)
        if ($parts.Count -lt 2) { break }
        $chain += [pscustomobject]@{ Pid = $cur; Comm = $parts[1] }
        $cur = [int]$parts[0]
    }
    $chain
}

$app = Get-AppWindow
if (-not $app) { throw 'GhosttyWin32 is not running.' }
Write-Host "app: pid $($app.Id), hwnd $($app.MainWindowHandle)"

Write-Host ""
Write-Host "1. In a WSL tab of the app, run  yes  so output keeps coming."
Write-Host "   Any WSL tab will do; this finds the session from the process."
Read-Host "   Enter once yes is running" | Out-Null

$yesPids = @(Get-DistroPids 'yes')
if (-not $yesPids) {
    Write-Host ""
    Write-Host "No 'yes' is running in the distribution." -ForegroundColor Yellow
    Write-Host "Check that the tab really is a WSL one ('command = wsl' in the config)."
    return
}
if ($yesPids.Count -gt 1) {
    Write-Host ""
    Write-Host "More than one 'yes' is running: $($yesPids -join ', ')." -ForegroundColor Yellow
    Write-Host "Stop the others, so what the close takes with it is not in doubt."
    return
}

$yesPid = $yesPids[0]
$chain = Get-Ancestry $yesPid
Write-Host ""
Write-Host "== the session under test ==" -ForegroundColor Cyan
foreach ($p in $chain) { Write-Host "  $($p.Pid) $($p.Comm)" }

# `comm` is the kernel's name for the task, cut to 15 characters the same
# way pgrep matches them, so the full ghostty-wsl-bridge never appears here.
$bridge = $chain | Where-Object { $_.Comm -match '^ghostty-wsl-bri' } | Select-Object -First 1
if (-not $bridge) {
    Write-Host ""
    Write-Host "No ghostty-wsl-bridge above this 'yes', so the tab is not on the bridge." -ForegroundColor Yellow
    Write-Host "A 'wsl' typed inside a pwsh tab runs under ConPTY instead; the bridge"
    Write-Host "only takes over when the tab's own command is wsl."
    return
}

Write-Host ""
Write-Host "2. Close that tab now, with yes still running (the x, or ctrl+shift+w)."
Read-Host "   Enter the moment you have closed it" | Out-Null

$hwnd = $app.MainWindowHandle
$sw = [Diagnostics.Stopwatch]::StartNew()
$hung = $false
while ($sw.Elapsed.TotalSeconds -lt $kWaitSeconds) {
    if ([Hung]::IsHungAppWindow($hwnd)) { $hung = $true; break }
    Start-Sleep -Milliseconds 250
}
$sw.Stop()

# The session is torn down asynchronously; give it a moment before judging.
Start-Sleep -Seconds 2
$yesAlive = Test-DistroPid $yesPid
$bridgeAlive = Test-DistroPid $bridge.Pid

Write-Host ""
Write-Host "== after the tab was closed ==" -ForegroundColor Cyan
Write-Host "  yes    ($yesPid): $(if ($yesAlive) { 'still running' } else { 'gone' })"
Write-Host "  bridge ($($bridge.Pid)): $(if ($bridgeAlive) { 'still running' } else { 'gone' })"

Write-Host ""
if ($hung) {
    Write-Host "FAIL: the window stopped answering after $([int]$sw.Elapsed.TotalSeconds)s." -ForegroundColor Red
    Write-Host "This is the deadlock described at the top of this file."
    Write-Host "Attach a debugger and break all: the UI thread should be in Pty.deinit's join."
    exit 1
}

if ($yesAlive -or $bridgeAlive) {
    Write-Host "FAIL: the window kept answering, but the session outlived its tab." -ForegroundColor Red
    exit 1
}

Write-Host "PASS: the window kept answering and the session went with the tab." -ForegroundColor Green
