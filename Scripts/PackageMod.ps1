# Copyright MadFall. All Rights Reserved.
#
# Builds one mod into an installable folder.
#
#   .\Scripts\PackageMod.ps1 -Mod example_paint
#   .\Scripts\PackageMod.ps1 -Mod my_mod -ReleaseVersion 0.1 -OutputDir D:\ModBuilds
#
# Output: <OutputDir>\<mod id>\ - copy that folder into a packaged game's
# MadFall\Mods\ directory (or zip it for players).
#
# A DATA mod (only mod.json and definitions/) is copied as is.
#
# A CONTENT mod also has one Unreal plugin inside its folder
# (Mods/<id>/<PluginName>/<PluginName>.uplugin, CanContainContent) holding its
# assets. The plugin is cooked as DLC against a released base game
# (-basedonreleaseversion), which produces a pak + IoStore container holding
# only the mod's own cooked packages and shader library. The output keeps the
# plugin's descriptor and Content/Paks: that layout is an Unreal "Mod" plugin,
# which a packaged game discovers under <Project>/Mods, mounts, and whose
# /<PluginName>/ content root and shader library it registers by itself.
#
# WHY DLC COOKING AND NOT A HAND-ROLLED UNREALPAK CALL
#   A packaged MadFall loads through IoStore (Zen loader). Loose cooked .uasset
#   files in a legacy pak cannot be loaded by it, and building IoStore
#   containers by hand means reimplementing package ids, the container header
#   and shader library extraction that the DLC cook already does. The cost is
#   that mods must be cooked against the exact release the players run; a base
#   game rebuilt with -createreleaseversion invalidates older mod paks.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Mod,

    [string] $ReleaseVersion = '0.1',

    [string] $OutputDir,

    [string] $EngineRoot
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $RepoRoot 'MadFall.uproject'
if (-not $OutputDir) { $OutputDir = Join-Path $RepoRoot 'Saved\ModBuilds' }

$candidates = @($EngineRoot, $env:MADFALL_UE_ROOT, 'C:\Program Files\Epic Games\UE_5.8') | Where-Object { $_ }
$Engine = $candidates | Where-Object { Test-Path (Join-Path $_ 'Engine\Build\BatchFiles\RunUAT.bat') } | Select-Object -First 1
if (-not $Engine) { throw "Could not find an Unreal Engine install. Set MADFALL_UE_ROOT or pass -EngineRoot." }
$RunUAT = Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'

$ModDir = Join-Path $RepoRoot "Mods\$Mod"
if (-not (Test-Path (Join-Path $ModDir 'mod.json'))) {
    throw "No mod.json in $ModDir."
}

$OutMod = Join-Path $OutputDir $Mod
if (Test-Path $OutMod) { Remove-Item -Recurse -Force -LiteralPath $OutMod }
New-Item -ItemType Directory -Force -Path $OutMod | Out-Null

# Data: the manifest and every definition folder.
Copy-Item (Join-Path $ModDir 'mod.json') $OutMod
foreach ($data in @('definitions', 'scripts')) {
    $path = Join-Path $ModDir $data
    if (Test-Path $path) { Copy-Item $path $OutMod -Recurse }
}

$plugins = @(Get-ChildItem -Path $ModDir -Filter '*.uplugin' -Recurse -Depth 1)
if ($plugins.Count -gt 1) {
    throw "$Mod has $($plugins.Count) plugins; a mod carries at most one content plugin."
}

if ($plugins.Count -eq 0) {
    Write-Host "==> $Mod is a data mod; copied to $OutMod" -ForegroundColor Green
    exit 0
}

$Plugin = $plugins[0]
$PluginName = $Plugin.BaseName
$Release = Join-Path $RepoRoot "Releases\$ReleaseVersion"
if (-not (Test-Path $Release)) {
    throw "Release $ReleaseVersion not found at $Release. Package the base game first: .\Scripts\Package.ps1 -ReleaseVersion $ReleaseVersion"
}

Write-Host "==> Cooking $Mod ($PluginName) against release $ReleaseVersion" -ForegroundColor Cyan
& $RunUAT BuildCookRun "-project=$ProjectFile" -noP4 -platform=Win64 -clientconfig=Development `
    -cook -stage -pak "-dlcname=$PluginName" "-basedonreleaseversion=$ReleaseVersion" `
    -DLCIncludeEngineContent=false -unattended -utf8output
if ($LASTEXITCODE -ne 0) {
    Write-Host "==> $Mod cook FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

# The DLC stage mirrors the install layout: .../MadFall/Mods/<id>/<Plugin>/...
$staged = Join-Path $Plugin.DirectoryName "Saved\StagedBuilds\Windows\MadFall\Mods\$Mod\$PluginName"
$paks = Join-Path $staged 'Content\Paks'
if (-not (Test-Path $paks)) {
    throw "Cook reported success but no paks were staged under $paks."
}

$OutPlugin = Join-Path $OutMod $PluginName
New-Item -ItemType Directory -Force -Path (Join-Path $OutPlugin 'Content') | Out-Null
Copy-Item (Join-Path $staged "$PluginName.uplugin") $OutPlugin
Copy-Item $paks (Join-Path $OutPlugin 'Content') -Recurse

$built = Get-ChildItem -Path $OutPlugin -Recurse -File -Include '*.pak', '*.utoc', '*.ucas'
Write-Host "==> $Mod built: $($built.Count) container file(s), $([math]::Round(($built | Measure-Object Length -Sum).Sum / 1KB, 1)) KiB -> $OutMod" -ForegroundColor Green
exit 0
