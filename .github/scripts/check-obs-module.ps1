# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
    Verifies the built plugin exports what OBS looks up.

.DESCRIPTION
    OBS resolves three symbols by name when loading a plugin. If any is
    missing the load fails and OBS says so only at LOG_DEBUG, so the symptom
    is "the filter isn't in the list" and an apparently clean log. This is the
    Windows counterpart of the dlopen check that ctest runs on Linux.
#>
[CmdletBinding()]
param([string]$BuildDir = "build")

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. "$PSScriptRoot/vsdevshell.ps1"

$dll = Get-ChildItem -Path $BuildDir -Recurse -Filter 'motion-blur.dll' |
    Select-Object -First 1
if (-not $dll) { throw "motion-blur.dll was not produced under $BuildDir" }
Write-Host "Checking $($dll.FullName)"

# Join to a single string: on an array, PowerShell's -match/-notmatch filter
# elements rather than returning a boolean, so testing an array would be
# true whenever any line failed to match - which is always.
$exports = (& dumpbin /nologo /exports $dll.FullName | Out-String)

$missing = @()
foreach ($sym in 'obs_module_load', 'obs_module_ver', 'obs_module_set_pointer') {
    if ($exports -match [regex]::Escape($sym)) {
        Write-Host "  $sym present"
    } else {
        Write-Host "  $sym MISSING"
        $missing += $sym
    }
}

if ($missing.Count -gt 0) {
    throw "the module is missing OBS entry points: $($missing -join ', ')"
}

Write-Host "Module exports all OBS entry points."
