# Copyright MadFall. All Rights Reserved.
#
# Packages the game for Windows and installs the shipped example mods.
#
#   .\Scripts\Package.ps1
#   .\Scripts\Package.ps1 -ReleaseVersion 0.2 -OutputDir D:\MadFallBuild
#
# Output: <OutputDir>\Windows\MadFall.exe, with Definitions\ and Mods\ loose
# next to the game's Content (see DefaultGame.ini staging).
#
# -ReleaseVersion records Releases\<version>\, the cooked-asset metadata that
# content mods are cooked against (Scripts/PackageMod.ps1). Mods built for one
# release are not guaranteed to load in a later one.
#
# Mods are installed after packaging rather than staged by the cook, because a
# content mod's source plugin (uncooked .uasset files, its own Saved\) must not
# ship: each mod goes through PackageMod.ps1 and only its build output is copied.

[CmdletBinding()]
param(
    [string] $ReleaseVersion = '0.1',

    [string] $OutputDir,

    [switch] $SkipMods,

    [string] $EngineRoot
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $RepoRoot 'MadFall.uproject'
if (-not $OutputDir) { $OutputDir = Join-Path $RepoRoot 'Saved\Packaged' }

$candidates = @($EngineRoot, $env:MADFALL_UE_ROOT, 'C:\Program Files\Epic Games\UE_5.8') | Where-Object { $_ }
$Engine = $candidates | Where-Object { Test-Path (Join-Path $_ 'Engine\Build\BatchFiles\RunUAT.bat') } | Select-Object -First 1
if (-not $Engine) { throw "Could not find an Unreal Engine install. Set MADFALL_UE_ROOT or pass -EngineRoot." }
$RunUAT = Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'

Write-Host "==> Packaging MadFall (Win64 Development), release $ReleaseVersion -> $OutputDir" -ForegroundColor Cyan

# Quoted: PowerShell otherwise splits "-createreleaseversion=0.1" at the dot and
# UAT receives a stray ".1" command after an otherwise successful package.
& $RunUAT BuildCookRun "-project=$ProjectFile" -noP4 -platform=Win64 -clientconfig=Development `
    -build -cook -stage -pak -archive "-archivedirectory=$OutputDir" `
    "-createreleaseversion=$ReleaseVersion" -unattended -utf8output
if ($LASTEXITCODE -ne 0) {
    Write-Host "==> Package FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

$GameMods = Join-Path $OutputDir 'Windows\MadFall\Mods'
if (Test-Path $GameMods) { Remove-Item -Recurse -Force -LiteralPath $GameMods }
New-Item -ItemType Directory -Force -Path $GameMods | Out-Null
Copy-Item (Join-Path $RepoRoot 'Mods\README.md') $GameMods -ErrorAction SilentlyContinue

if (-not $SkipMods) {
    foreach ($modDir in Get-ChildItem -Path (Join-Path $RepoRoot 'Mods') -Directory) {
        if (-not (Test-Path (Join-Path $modDir.FullName 'mod.json'))) { continue }

        & (Join-Path $PSScriptRoot 'PackageMod.ps1') -Mod $modDir.Name -ReleaseVersion $ReleaseVersion -EngineRoot $Engine
        if ($LASTEXITCODE -ne 0) {
            Write-Host "==> Mod $($modDir.Name) FAILED" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        Copy-Item (Join-Path $RepoRoot "Saved\ModBuilds\$($modDir.Name)") $GameMods -Recurse
    }
}

Write-Host "==> Packaged: $(Join-Path $OutputDir 'Windows\MadFall.exe')" -ForegroundColor Green
exit 0
