# Check the premise behind shipping ghostty-wsl-bridge outside the package
# (#229): a file inside an installed MSIX cannot be run from WSL, and one in
# a directory the user owns can.
#
# DrvFs derives Unix permissions from the Windows ACL, and a file the user
# cannot write arrives without the execute bit. Package contents are
# read-only by design, so the copy that ships inside the package is readable
# from the distribution and not executable -- `exec` fails before a byte of
# it is read. This is why the binary is a release asset the user installs.
#
# Run this in any pwsh, with WSL installed. It touches nothing but one file
# of its own under %LOCALAPPDATA%, which it removes again.

$ErrorActionPreference = 'Stop'

function Get-WslPath([string] $windowsPath) {
    # wslpath is the distribution's own translation, so this does not
    # hard-code how drives are mounted.
    $out = & wsl.exe -- wslpath -a -u -- "$($windowsPath -replace '\\', '/')" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "wslpath failed for $windowsPath : $out" }
    "$out".Trim()
}

function Show-Verdict([string] $label, [string] $windowsPath) {
    $p = Get-WslPath $windowsPath
    $listing = (& wsl.exe -- ls -la -- "$p" 2>&1 | Select-Object -First 1)
    # No `--` here: test counts it as an operand and gives up.
    & wsl.exe -- test -r "$p" 2>&1 | Out-Null
    $readable = $LASTEXITCODE -eq 0
    & wsl.exe -- test -x "$p" 2>&1 | Out-Null
    $executable = $LASTEXITCODE -eq 0

    Write-Host ""
    Write-Host "== $label ==" -ForegroundColor Cyan
    Write-Host "  $windowsPath"
    Write-Host "  $listing"
    Write-Host "  readable=$readable executable=$executable"
    $executable
}

# A directory the user owns. The app keeps its config here, so a copy of the
# binary alongside it needs no new place on disk.
$ownDir = Join-Path $env:LOCALAPPDATA 'ghostty'
New-Item -ItemType Directory -Force -Path $ownDir | Out-Null
$ownFile = Join-Path $ownDir 'exec-bit-probe'
Set-Content -Path $ownFile -Value '#!/bin/sh' -NoNewline
try {
    $ownExecutable = Show-Verdict 'a file in a directory the user owns' $ownFile
} finally {
    Remove-Item $ownFile -ErrorAction SilentlyContinue
}

# The installed package, if there is one. Without it the first half still
# says whether a user-owned copy would run, which is the half the fix needs.
# The package's identity name is a signing-time GUID, so it is found by
# what it contains rather than by what it is called. A registration made
# from a build output is skipped: those files belong to the user, so they
# are executable and would answer a different question than this one.
$pkg = Get-AppxPackage -ErrorAction SilentlyContinue | Where-Object {
    $_.InstallLocation -like (Join-Path $env:ProgramFiles 'WindowsApps\*') -and
    (Test-Path (Join-Path $_.InstallLocation 'GhosttyWin32.exe'))
} | Select-Object -First 1
if (-not $pkg) {
    Write-Host ""
    Write-Host "GhosttyWin32 is not installed, so the package side is not checked." -ForegroundColor Yellow
    Write-Host "Install the MSIX to see both halves."
    if ($ownExecutable) {
        Write-Host ""
        Write-Host "PASS (partial): a user-owned copy is executable from the distribution." -ForegroundColor Green
        exit 0
    }
    Write-Host ""
    Write-Host "FAIL: even a user-owned copy is not executable, so installing the" -ForegroundColor Red
    Write-Host "binary where the docs say would not help. Something else is wrong."
    exit 1
}

# Any file of the package shows the same permissions; the helper is only
# there when the package still ships it.
$helper = Join-Path $pkg.InstallLocation 'ghostty-wsl-bridge'
$packaged = if (Test-Path $helper) { $helper } else { Join-Path $pkg.InstallLocation 'GhosttyWin32.exe' }
$packagedExecutable = Show-Verdict 'a file inside the installed package' $packaged

Write-Host ""
if ($ownExecutable -and -not $packagedExecutable) {
    Write-Host "PASS: the package copy cannot be run from the distribution and a" -ForegroundColor Green
    Write-Host "user-owned one can, which is what #229 concluded."
    exit 0
}

if ($packagedExecutable) {
    Write-Host "The package copy IS executable here." -ForegroundColor Yellow
    Write-Host "That contradicts #229. Worth knowing which Windows or WSL version"
    Write-Host "changed, before the docs keep claiming otherwise."
    exit 1
}

Write-Host "FAIL: neither copy is executable, so the problem is not the package." -ForegroundColor Red
exit 1
