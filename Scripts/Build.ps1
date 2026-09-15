# Copyright MadFall. All Rights Reserved.
#
# Thin wrapper around UnrealBuildTool for one target.
#
#   .\Scripts\Build.ps1 -Target MadFallEditor
#   .\Scripts\Build.ps1 -Target MadFallServer -Configuration Shipping
#
# Engine root resolution order:
#   1. -EngineRoot argument
#   2. $env:MADFALL_UE_ROOT
#   3. C:\Program Files\Epic Games\UE_5.8

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('MadFall', 'MadFallEditor', 'MadFallServer')]
    [string] $Target,

    [ValidateSet('Debug', 'DebugGame', 'Development', 'Test', 'Shipping')]
    [string] $Configuration = 'Development',

    [ValidateSet('Win64', 'Linux')]
    [string] $Platform = 'Win64',

    [string] $EngineRoot
)

$ErrorActionPreference = 'Stop'

function Resolve-EngineRoot {
    param([string] $Explicit)

    $candidates = @(
        $Explicit,
        $env:MADFALL_UE_ROOT,
        'C:\Program Files\Epic Games\UE_5.8'
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'Engine\Build\BatchFiles\Build.bat')) {
            return $candidate
        }
    }

    throw "Could not find an Unreal Engine install. Set MADFALL_UE_ROOT or pass -EngineRoot. Tried: $($candidates -join '; ')"
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $RepoRoot 'MadFall.uproject'
$Engine = Resolve-EngineRoot -Explicit $EngineRoot
$BuildBat = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat'

# A Launcher-installed engine ships no precompiled Server target libraries, so
# UnrealBuildTool refuses Server builds outright. Detect it up front and say so
# in one line, rather than letting the user read a generic BuildException.
$IsInstalledEngine = Test-Path (Join-Path $Engine 'Engine\Build\InstalledBuild.txt')
if ($Target -eq 'MadFallServer' -and $IsInstalledEngine) {
    Write-Error @"
Cannot build MadFallServer: $Engine is a Launcher (installed) engine, which does
not ship Server target libraries. A dedicated server needs a source build of
Unreal Engine from github.com/EpicGames/UnrealEngine. See docs/ARCHITECTURE.md,
'Dedicated server'.
"@
    exit 2
}

Write-Host "==> Building $Target | $Platform | $Configuration" -ForegroundColor Cyan
Write-Host "    Engine:  $Engine"
Write-Host "    Project: $ProjectFile"

& $BuildBat $Target $Platform $Configuration -Project="$ProjectFile" -WaitMutex -FromMsBuild
$code = $LASTEXITCODE

if ($code -ne 0) {
    Write-Host "==> $Target FAILED (exit $code)" -ForegroundColor Red
}
else {
    Write-Host "==> $Target OK" -ForegroundColor Green
}

exit $code
