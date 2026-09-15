# Copyright MadFall. All Rights Reserved.
#
# Copies Epic's UE5 mannequins (Manny and Quinn, their materials and the unarmed,
# hit-react and death animations) from the installed engine's Third Person
# template into Content/Characters/Mannequins, where the humanoid rig looks for
# them. Nothing is downloaded.
#
# WHY COPIED AND NOT COMMITTED: they are Epic's uncooked assets, which the
# Unreal Engine EULA does not let us publish in a public source repository;
# every developer already has them with the engine. Content/Characters/Mannequins
# is gitignored. A cooked build ships them like any engine content. Without
# them the game runs as before, with the box-figure rig.
#
#   .\Scripts\copy_mannequin.ps1 [-EngineDir "C:\Program Files\Epic Games\UE_5.8"]

param(
    [string]$EngineDir = "C:\Program Files\Epic Games\UE_5.8"
)

$ErrorActionPreference = 'Stop'
$Source = Join-Path $EngineDir 'Templates\TemplateResources\High\Characters\Content\Mannequins'
$Project = Split-Path -Parent $PSScriptRoot
$Destination = Join-Path $Project 'Content\Characters\Mannequins'

if (-not (Test-Path $Source)) {
    throw "No mannequins at $Source - is the engine installed with its templates?"
}

# The template's paths are /Game/Characters/Mannequins/..., so keeping the same
# folder keeps every reference between the assets valid. The pistol and rifle
# sets are skipped (about 40 MB the game has no use for), except the hit
# reactions, which live under Rifle.
$Files = Get-ChildItem -Path $Source -Recurse -File | Where-Object {
    $Relative = $_.FullName.Substring($Source.Length + 1)
    -not ($Relative -like 'Anims\Pistol\*') -and (-not ($Relative -like 'Anims\Rifle\*') -or ($Relative -like 'Anims\Rifle\HitReact\*'))
}

$Bytes = 0
foreach ($File in $Files) {
    $Relative = $File.FullName.Substring($Source.Length + 1)
    $Target = Join-Path $Destination $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
    Copy-Item -Path $File.FullName -Destination $Target -Force
    $Bytes += $File.Length
}

Write-Host ("Copied {0} mannequin assets ({1:N1} MB) to {2}" -f $Files.Count, ($Bytes / 1MB), $Destination)
