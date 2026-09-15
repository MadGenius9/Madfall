# Copyright MadFall. All Rights Reserved.
#
# Downloads the photo-scanned models model blocks draw (Poly Haven, CC0 - public
# domain, no attribution required): the 1K FBX of each and the textures it
# includes, into SourceArt/polyhaven/<Asset>/. About 7 MB. Then run
# Scripts/make_model_material.py and Scripts/import_models.py through
# UnrealEditor-Cmd -ExecutePythonScript. SourceArt/ is not committed; the
# imported Content/Models assets are.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'SourceArt\polyhaven'
# Poly Haven's API refuses requests without an identifying user agent.
$headers = @{ 'User-Agent' = 'MadFall-asset-fetch/1.0 (game project; CC0 import)' }

foreach ($asset in 'Barrel_01', 'wooden_crate_01') {
    $files = Invoke-RestMethod -Uri "https://api.polyhaven.com/files/$asset" -Headers $headers
    $fbx = $files.fbx.'1k'.fbx
    $downloads = @(@{ Url = $fbx.url; Path = Split-Path -Leaf $fbx.url })
    foreach ($include in $fbx.include.PSObject.Properties) {
        $downloads += @{ Url = $include.Value.url; Path = $include.Name }
    }
    foreach ($download in $downloads) {
        $target = Join-Path (Join-Path $dest $asset) $download.Path
        New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
        if (-not (Test-Path $target)) {
            Invoke-WebRequest -Uri $download.Url -Headers $headers -OutFile $target
        }
    }
    Write-Host "$asset ready"
}
