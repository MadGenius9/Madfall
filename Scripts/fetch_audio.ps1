# Copyright MadFall. All Rights Reserved.
#
# Downloads the CC0 sound packs that Scripts/prepare_audio.py turns into the
# recordings in Content/Audio (about 12.5 MB) into the uncommitted
# SourceArt/audio/. Only needed to rebuild Content/Audio, which is committed.
#
#   Kenney Impact Sounds, Kenney RPG Audio            https://kenney.nl (CC0)
#   Zombies Sound Pack, 100 CC0 SFX #2, 30 CC0 SFX loops,
#   Wind Whoosh Loop                                   https://opengameart.org (CC0)

$ErrorActionPreference = 'Stop'
$Project = Split-Path -Parent $PSScriptRoot
$Destination = Join-Path $Project 'SourceArt\audio'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$Downloads = @(
    'https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip',
    'https://kenney.nl/media/pages/assets/rpg-audio/8e99002d76-1677590336/kenney_rpg-audio.zip',
    'https://opengameart.org/sites/default/files/zombies.zip',
    'https://opengameart.org/sites/default/files/sfx_100_v2.zip',
    'https://opengameart.org/sites/default/files/sfx_loops.zip',
    'https://opengameart.org/sites/default/files/wind%20woosh%20loop.ogg'
)
# Music (CC0): EmptyCity and Zombies' March by yd, Cold Silence by Eponasoft.
$Music = @(
    'https://opengameart.org/sites/default/files/EmptyCity.ogg',
    'https://opengameart.org/sites/default/files/cold_silence.ogg',
    'https://opengameart.org/sites/default/files/ZombiesAreComing.ogg'
)
New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'music') | Out-Null
foreach ($Url in $Music) {
    Invoke-WebRequest -Uri $Url -OutFile (Join-Path (Join-Path $Destination 'music') (Split-Path -Leaf $Url)) -UserAgent 'Mozilla/5.0 MadFall-asset-fetch'
}

foreach ($Url in $Downloads) {
    $Name = [System.Uri]::UnescapeDataString((Split-Path -Leaf $Url)).Replace(' ', '_')
    $File = Join-Path $Destination $Name
    Write-Host "Downloading $Name"
    # Both sites refuse requests without a browser-like user agent.
    Invoke-WebRequest -Uri $Url -OutFile $File -UserAgent 'Mozilla/5.0 MadFall-asset-fetch'
    if ($Name.EndsWith('.zip')) {
        $Folder = Join-Path $Destination ([System.IO.Path]::GetFileNameWithoutExtension($Name))
        Expand-Archive -Path $File -DestinationPath $Folder -Force
    }
}
Write-Host "Downloaded to $Destination"
