# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
    Puts the MSVC tools (dumpbin, lib, link) on PATH for the current shell.

.DESCRIPTION
    Dot-source this. Each workflow step runs in a fresh shell, so entering the
    developer environment in one step does nothing for the next - every step
    that needs dumpbin or lib has to do this for itself.
#>
$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; no Visual Studio install" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ toolset" }

Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
