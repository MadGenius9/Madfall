# Copyright MadFall. All Rights Reserved.
#
# Downloads the animated animal models the deer and wolf are drawn with -
# Quaternius' Ultimate Animated Animal Pack (CC0), as served by Poly Pizza -
# into the uncommitted SourceArt/animals/ (about 2 MB). Only needed to rebuild
# Content/Animals, which is committed: then run python Scripts/prepare_animals.py
# and Scripts/import_animals.py.

param([string[]]$Only)

$ErrorActionPreference = 'Stop'
$Project = Split-Path -Parent $PSScriptRoot
$Destination = Join-Path $Project 'SourceArt\animals'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# https://poly.pizza/m/T6Cs7tmMHJ (Deer) and https://poly.pizza/m/P1gU3Qkr9r (Wolf)
$Models = @{
    'Deer' = 'https://static.poly.pizza/4b6c2a41-43c7-404c-ae37-e8c4645ff93b.glb'
    'Wolf' = 'https://static.poly.pizza/f1d12388-e39b-4157-b32a-646a1d089fc4.glb'
    # Desert and beach. https://poly.pizza/m/qmX6nhnvp7 (Donkey, same pack),
    # https://poly.pizza/m/x9x0viZs8V (Snake) and https://poly.pizza/m/Gs3yfsV5lB
    # (Crab Enemy) - also Quaternius, also CC0, on a different (monster) rig.
    'Donkey' = 'https://static.poly.pizza/ca29f94e-0874-41b6-8334-66642af56a61.glb'
    'Snake' = 'https://static.poly.pizza/0f3a551e-743e-48f5-936f-804c6c3b88bd.glb'
    'Crab' = 'https://static.poly.pizza/b9bbf6bd-2b21-4013-bc38-0f5e524ac12c.glb'
}
# -Only Donkey,Snake fetches just those.
if ($Only) {
    $Only = $Only -split "," | ForEach-Object { $_.Trim() }
    foreach ($Key in @($Models.Keys)) { if ($Only -notcontains $Key) { $Models.Remove($Key) } }
}
foreach ($Name in $Models.Keys) {
    Write-Host "Downloading $Name"
    Invoke-WebRequest -Uri $Models[$Name] -OutFile (Join-Path $Destination "$Name.glb") -UserAgent 'Mozilla/5.0 MadFall-asset-fetch'
}
Write-Host "Downloaded to $Destination"
