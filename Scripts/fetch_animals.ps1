# Copyright MadFall. All Rights Reserved.
#
# Downloads the animated animal models the deer and wolf are drawn with -
# Quaternius' Ultimate Animated Animal Pack (CC0), as served by Poly Pizza -
# into the uncommitted SourceArt/animals/ (about 2 MB). Only needed to rebuild
# Content/Animals, which is committed: then run python Scripts/prepare_animals.py
# and Scripts/import_animals.py.

$ErrorActionPreference = 'Stop'
$Project = Split-Path -Parent $PSScriptRoot
$Destination = Join-Path $Project 'SourceArt\animals'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# https://poly.pizza/m/T6Cs7tmMHJ (Deer) and https://poly.pizza/m/P1gU3Qkr9r (Wolf)
$Models = @{
    'Deer' = 'https://static.poly.pizza/4b6c2a41-43c7-404c-ae37-e8c4645ff93b.glb'
    'Wolf' = 'https://static.poly.pizza/f1d12388-e39b-4157-b32a-646a1d089fc4.glb'
}
foreach ($Name in $Models.Keys) {
    Write-Host "Downloading $Name"
    Invoke-WebRequest -Uri $Models[$Name] -OutFile (Join-Path $Destination "$Name.glb") -UserAgent 'Mozilla/5.0 MadFall-asset-fetch'
}
Write-Host "Downloaded to $Destination"
