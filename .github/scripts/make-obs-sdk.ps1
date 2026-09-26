# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
    Assembles a minimal OBS SDK for building the plugin on Windows.

.DESCRIPTION
    Building libobs from source on Windows means fetching obs-deps and Qt to
    produce a library the plugin links but never needs compiled. This avoids
    all of it.

    OBS's own Windows release already contains obs.dll and
    obs-frontend-api.dll - the exact binaries the plugin will load. Import
    libraries are generated from those DLLs' export tables, and headers come
    from the matching source tag, so the link target and the runtime target
    are the same build of the same version.

    Produces:
      <OutDir>/include/...      headers from libobs/ plus obs-frontend-api.h
      <OutDir>/lib/obs.lib      generated from the shipped obs.dll
      <OutDir>/lib/obs-frontend-api.lib
#>
[CmdletBinding()]
param(
    # Pinned deliberately to the oldest OBS the plugin supports: libobs
    # refuses a plugin whose major.minor exceeds the host's, so building
    # against the floor is what makes one binary load on 30, 31 and 32.
    [string]$ObsVersion = "30.0.2",
    [string]$OutDir = "obs-sdk"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

$work = Join-Path ([System.IO.Path]::GetTempPath()) "obs-sdk-build"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work -Force | Out-Null

# ---------------------------------------------------------------- binaries
Write-Step "Locating the OBS $ObsVersion Windows release"

$headers = @{ "User-Agent" = "motion-blur-build" }
if ($env:GITHUB_TOKEN) { $headers["Authorization"] = "Bearer $env:GITHUB_TOKEN" }

$release = Invoke-RestMethod -Headers $headers `
    -Uri "https://api.github.com/repos/obsproject/obs-studio/releases/tags/$ObsVersion"

# OBS does not put "Windows" in the name of its Windows build: the portable
# archive for 30.0.2 is plainly "OBS-Studio-30.0.2.zip", while every other
# platform is spelled out (…-Ubuntu-x86_64.deb, …-macOS-Apple.dmg). So the
# rule is "the zip that isn't debug symbols", not a name match. If nothing
# qualifies, print the assets that do exist - a log naming the real files
# beats a guess that fails silently.
$candidates = $release.assets |
    Where-Object { $_.name -match '\.zip$' } |
    Where-Object { $_.name -notmatch 'pdb|symbol|dbsym|source|sources' }

# Prefer an explicitly-named Windows/x64 archive if a future release starts
# providing one, otherwise take the plain portable zip.
$asset = $candidates | Where-Object { $_.name -match 'Windows|x64|win' } |
    Select-Object -First 1
if (-not $asset) { $asset = $candidates | Select-Object -First 1 }

if (-not $asset) {
    Write-Host "No Windows zip found. Assets present in $($ObsVersion):"
    $release.assets | ForEach-Object { Write-Host "  $($_.name)" }
    throw "Could not find a Windows archive for OBS $ObsVersion"
}

Write-Step "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB, 1)) MB)"
$zip = Join-Path $work $asset.name
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers $headers

$obsBin = Join-Path $work "obs"
Expand-Archive -Path $zip -DestinationPath $obsBin -Force

$obsDll = Get-ChildItem -Path $obsBin -Recurse -Filter "obs.dll" | Select-Object -First 1
$frontendDll = Get-ChildItem -Path $obsBin -Recurse -Filter "obs-frontend-api.dll" |
    Select-Object -First 1

if (-not $obsDll) {
    Write-Host "Archive contents (top two levels):"
    Get-ChildItem -Path $obsBin -Recurse -Depth 2 | ForEach-Object { Write-Host "  $($_.FullName)" }
    throw "obs.dll not found inside $($asset.name)"
}
Write-Step "Found obs.dll at $($obsDll.FullName)"

# ----------------------------------------------------------------- headers
Write-Step "Fetching headers from the obs-studio source at $ObsVersion"
$src = Join-Path $work "src"
& git clone --depth 1 --branch $ObsVersion --quiet `
    https://github.com/obsproject/obs-studio.git $src
if ($LASTEXITCODE -ne 0) { throw "git clone of obs-studio $ObsVersion failed" }

$include = Join-Path $OutDir "include"
$lib = Join-Path $OutDir "lib"
New-Item -ItemType Directory -Path $include -Force | Out-Null
New-Item -ItemType Directory -Path $lib -Force | Out-Null

# libobs/ is laid out exactly how the headers are included (<obs-module.h>,
# <util/platform.h>, <graphics/graphics.h>), so copying the tree's headers
# verbatim reproduces the include root a libobs install provides.
Get-ChildItem -Path (Join-Path $src "libobs") -Recurse -Filter "*.h" | ForEach-Object {
    $rel = $_.FullName.Substring((Join-Path $src "libobs").Length).TrimStart('\')
    $dest = Join-Path $include $rel
    New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
    Copy-Item $_.FullName $dest -Force
}

# obs-config.h is a plain in-tree header in OBS 30.x (literal version
# numbers, no CMake substitution), so nothing needs generating.
if (-not (Test-Path (Join-Path $include "obs-config.h"))) {
    throw "obs-config.h missing from the assembled include tree"
}

$frontendHeader = Join-Path $src "UI\obs-frontend-api\obs-frontend-api.h"
if (Test-Path $frontendHeader) {
    Copy-Item $frontendHeader (Join-Path $include "obs-frontend-api.h") -Force
    Write-Step "Included obs-frontend-api.h"
} else {
    Write-Warning "obs-frontend-api.h not found; the plugin will build without the divisor helper"
}

# ------------------------------------------------------- import libraries
Write-Step "Entering the MSVC developer environment"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; no Visual Studio install" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ toolset" }

Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

function New-ImportLib {
    param([string]$DllPath, [string]$LibName, [string]$OutLibDir)

    Write-Step "Generating $LibName.lib from $(Split-Path $DllPath -Leaf)"

    $exports = & dumpbin /nologo /exports $DllPath
    # Rows in the export table look like:
    #   ordinal  hint  RVA       name
    #       1     0  0001A2B0  obs_source_create
    # Forwarded exports have no RVA and are skipped: there is nothing to
    # import them from, and libobs does not forward anything the plugin uses.
    $names = $exports |
        Select-String -Pattern '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }

    if ($names.Count -eq 0) { throw "No exports parsed from $DllPath" }
    Write-Host "    $($names.Count) exports"

    $def = Join-Path $OutLibDir "$LibName.def"
    $content = @("LIBRARY $(Split-Path $DllPath -Leaf)", "EXPORTS") + ($names | ForEach-Object { "    $_" })
    Set-Content -Path $def -Value $content -Encoding ASCII

    & lib /nologo "/def:$def" "/out:$(Join-Path $OutLibDir "$LibName.lib")" /machine:x64
    if ($LASTEXITCODE -ne 0) { throw "lib.exe failed for $LibName" }
    Remove-Item $def -Force
    Remove-Item (Join-Path $OutLibDir "$LibName.exp") -Force -ErrorAction SilentlyContinue
}

New-ImportLib -DllPath $obsDll.FullName -LibName "obs" -OutLibDir $lib
if ($frontendDll) {
    New-ImportLib -DllPath $frontendDll.FullName -LibName "obs-frontend-api" -OutLibDir $lib
} else {
    Write-Warning "obs-frontend-api.dll not present; building without the divisor helper"
}

Write-Step "SDK ready at $(Resolve-Path $OutDir)"
Get-ChildItem -Path $lib | ForEach-Object { Write-Host "  lib/$($_.Name)" }
