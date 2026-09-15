# Copyright MadFall. All Rights Reserved.
#
# Downloads the photo-scanned surface texture sets (ambientCG, CC0 - public
# domain, no attribution required) into SourceArt/ambientCG/<Set>/ and unzips
# them. About 357 MB of zips. Then run, from the editor command line:
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/make_pbr_material.py"
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/import_surface_textures.py"
# SourceArt/ is not committed; the imported Content/Surfaces assets are.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'SourceArt\ambientCG'
New-Item -ItemType Directory -Force $dest | Out-Null

$sets = 'Rock030', 'Ground048', 'Grass004', 'Ground080', 'Planks021', 'Bark012',
        'Concrete034', 'Bricks076C', 'Metal041B', 'Gravel022', 'Snow006', 'Fabric066'
foreach ($set in $sets) {
    $zip = Join-Path $dest "${set}_2K-JPG.zip"
    if (-not (Test-Path $zip)) {
        Write-Host "Downloading $set..."
        Invoke-WebRequest -Uri "https://ambientcg.com/get?file=${set}_2K-JPG.zip" -OutFile $zip
    }
    Expand-Archive -Path $zip -DestinationPath (Join-Path $dest $set) -Force
}
Write-Host "Texture sets ready in $dest"
