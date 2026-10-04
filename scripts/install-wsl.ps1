<#
.SYNOPSIS
    Put the WSL bridge's two binaries where they can run.

.DESCRIPTION
    The bridge needs a piece on each side of the boundary, and neither
    can be run from the place it ships (see docs/WSL.md):

      wsl.exe             the shim a pane's shell runs. The copy in the
                          installed package cannot be started from
                          there, so one goes in
                          %LOCALAPPDATA%\ghostty\bin.

      ghostty-wsl-bridge  the half that runs inside the distribution.
                          A file in the package arrives without the
                          execute bit, so it is not shipped there.

    Both come from the release assets, not from the installed package.
    That is on purpose: what decides whether a shim and a host can work
    together is the wire format the shim reports, not which build
    produced it, and taking the file from a release rather than from the
    package is what shows that. It also means this runs before the MSIX
    is installed, or on a machine where it never will be.

    Re-running it is how an upgrade is followed -- and only needed when
    the wire format changed, which the app will say in the pane.

.PARAMETER Tag
    Release to take both from, e.g. v0.9.0. Defaults to the release the
    installed version came from, by the rule in .github/RELEASING.md:
    revision 65535 is a production tag, 65000+N is rcN. Required when
    nothing is installed to read that from.

.PARAMETER Distro
    Which distribution to install the helper into. Default: wsl.exe's
    default one.

.PARAMETER ShimOnly
    Skip the helper. The shim alone gets a typed `wsl` as far as the
    host; what the host opens with it still needs the helper.

.PARAMETER HelperOnly
    Skip the shim.

.EXAMPLE
    .\scripts\install-wsl.ps1
    # both halves, for the installed release and the default distribution

.EXAMPLE
    .\scripts\install-wsl.ps1 -Tag v0.9.0 -Distro Ubuntu
    # before installing anything, or for another distribution
#>
[CmdletBinding()]
param(
    [string] $Tag,
    [string] $Distro,
    [switch] $ShimOnly,
    [switch] $HelperOnly
)

$ErrorActionPreference = 'Stop'

$repo = 'i999rri/GhosttyWin32'
$wsl = Join-Path $env:SystemRoot 'System32\wsl.exe'
$distroArgs = if ($Distro) { @('--distribution', $Distro) } else { @() }

# ----- which release -----

if (-not $Tag) {
    # The identity comes from the manifest so this script does not hold
    # a second copy of it. Only to pick a release: nothing here depends
    # on the package itself.
    $manifest = Join-Path $PSScriptRoot '..\Package\Package.appxmanifest'
    $identity = if (Test-Path $manifest) {
        ([xml](Get-Content $manifest)).Package.Identity.Name
    } else { $null }

    $installed = if ($identity) {
        Get-AppxPackage -Name $identity -ErrorAction SilentlyContinue | Select-Object -First 1
    } else { $null }

    if ($installed) {
        $v = [version] $installed.Version
        if ($v.Revision -eq 65535) {
            $Tag = 'v{0}.{1}.{2}' -f $v.Major, $v.Minor, $v.Build
        } elseif ($v.Revision -gt 65000) {
            $Tag = 'v{0}.{1}.{2}-rc{3}' -f $v.Major, $v.Minor, $v.Build, ($v.Revision - 65000)
        }
    }
    if (-not $Tag) {
        throw ('Pass -Tag with the release to use. The installed version, if any, ' +
               'names no release -- a development deployment and a dev build both ' +
               'carry a version no tag was cut from.')
    }
    Write-Host "Release  $Tag (from the installed $($installed.Version))"
} else {
    Write-Host "Release  $Tag"
}

function Get-Asset([string] $name, [string] $into) {
    $url = "https://github.com/$repo/releases/download/$Tag/$name"
    Write-Host "  <- $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $into -UseBasicParsing
    } catch {
        throw "Could not download $name from $Tag. Check the tag and that the release has that asset."
    }
}

# ----- the shim -----

if (-not $HelperOnly) {
    $binDir = Join-Path $env:LOCALAPPDATA 'ghostty\bin'
    New-Item -ItemType Directory -Force $binDir | Out-Null
    # The name has to stay wsl.exe: the host puts this directory first
    # on the PATH of the shells it starts, so the shell finds this one
    # before System32's. Nothing outside those panes is affected.
    Get-Asset 'wsl.exe' (Join-Path $binDir 'wsl.exe')
    Write-Host "Shim     $binDir\wsl.exe"
}

# ----- the helper -----

if (-not $ShimOnly) {
    $download = Join-Path ([IO.Path]::GetTempPath()) 'ghostty-wsl-bridge'
    Get-Asset 'ghostty-wsl-bridge' $download

    # Translated by wsl.exe rather than assembled here: a path under a
    # mapped drive or a redirected profile is not /mnt/c/....
    $inside = (& $wsl @distroArgs --exec wslpath -a $download).Trim()

    # /usr/local/bin because the PATH that matters is the one
    # `wsl.exe --exec` gives a session, not a login shell's: a directory
    # added by .profile or .zshrc will not do. sudo runs on this
    # console, so a password prompt is answerable.
    & $wsl @distroArgs -- sudo install -Dm755 $inside /usr/local/bin/ghostty-wsl-bridge
    if ($LASTEXITCODE -ne 0) { throw 'install failed inside the distribution.' }
    Remove-Item $download -Force
    Write-Host 'Helper   /usr/local/bin/ghostty-wsl-bridge'
}

# ----- say whether it worked -----

Write-Host ''
if (-not $HelperOnly) {
    $installedShim = Join-Path $env:LOCALAPPDATA 'ghostty\bin\wsl.exe'
    Write-Host ("Shim in place:   {0}" -f (Test-Path $installedShim))
    # Asked of this shell only to show it runs at all. A pane's PATH is
    # the host's to set, and it puts the directory above first.
    $version = (& $installedShim --version 2>&1 | Select-Object -First 1)
    Write-Host "  it starts, and passes through to: $version"
}
if (-not $ShimOnly) {
    $where = (& $wsl @distroArgs --exec /bin/sh -c 'command -v ghostty-wsl-bridge').Trim()
    if ($where) {
        Write-Host "Helper in place: $where"
    } else {
        Write-Host 'Helper in place: NOT ON THE PATH a session gets -- see docs/WSL.md'
    }
}
Write-Host ''
Write-Host 'Then set wsl-bridge = true in %LOCALAPPDATA%\ghostty\config and restart.'
