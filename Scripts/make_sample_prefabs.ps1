# Copyright MadFall. All Rights Reserved.
#
# Generates the first-party POI prefabs in Definitions/prefabs/.
#
#   .\Scripts\make_sample_prefabs.ps1
#
# WHY A SCRIPT RATHER THAN HAND-WRITTEN JSON:
# A prefab's voxels are run-length encoded, and nobody should be hand-editing
# run lengths. The alternative authoring path - build it in the world, then
# `mad.prefab.capture` - is the right tool for art-directed buildings. These
# four are structural test content with exact, reviewable geometry: a script
# that says "walls on the perimeter, door at x=5..7" is easier to review and
# to fix than 400 numbers.
#
# Each shape is a function (x, y, z) -> palette index. Index 0 is always void
# ("*", leave terrain alone), index 1 is always air (carve).

$ErrorActionPreference = 'Stop'
$OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'Definitions\prefabs'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Write-Prefab {
    param(
        [string]   $Id,
        [string]   $DisplayName,
        [int]      $Tier,
        [string[]] $Tags,
        [int[]]    $Size,
        [string[]] $Palette,      # block ids; "*" = void, "madfall:air" = air
        [scriptblock] $Shape,
        [hashtable] $Placement,
        [object[]] $Markers
    )

    $sx, $sy, $sz = $Size
    $voxels = New-Object 'System.Collections.Generic.List[int]' ($sx * $sy * $sz)

    # X fastest, then Y, then Z - the same order the loader reads.
    for ($z = 0; $z -lt $sz; $z++) {
        for ($y = 0; $y -lt $sy; $y++) {
            for ($x = 0; $x -lt $sx; $x++) {
                $index = [int](& $Shape $x $y $z)
                if ($index -lt 0 -or $index -ge $Palette.Count) {
                    throw "$Id shape returned palette index $index at ($x,$y,$z); palette has $($Palette.Count) entries"
                }
                $voxels.Add($index)
            }
        }
    }

    # Every loot marker sits on a container block, so the player can see and
    # interact with it. The crate is ordinary prefab data - the loot system
    # finds the marker by position, and a mod's prefab can use any block with
    # the block.container tag instead.
    $lootMarkers = @($Markers | Where-Object { $_.type -eq 'loot' })
    if ($lootMarkers.Count -gt 0) {
        if ($Palette -notcontains 'madfall:loot_crate') { $Palette = $Palette + 'madfall:loot_crate' }
        $crate = [Array]::IndexOf($Palette, 'madfall:loot_crate')
        foreach ($marker in $lootMarkers) {
            $mx, $my, $mz = $marker.pos
            $voxels[$mx + $sx * ($my + $sy * $mz)] = $crate
        }
    }

    # Run-length encode.
    $runs = New-Object 'System.Collections.Generic.List[string]'
    $i = 0
    while ($i -lt $voxels.Count) {
        $value = $voxels[$i]
        $run = 1
        while (($i + $run) -lt $voxels.Count -and $voxels[$i + $run] -eq $value) { $run++ }
        $runs.Add("$run, $value")
        $i += $run
    }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('{')
    [void]$sb.AppendLine('	"schema": "madfall.prefab/1",')
    [void]$sb.AppendLine("	""id"": ""$Id"",")
    [void]$sb.AppendLine("	""display_name"": ""$DisplayName"",")
    [void]$sb.AppendLine("	""tier"": $Tier,")
    [void]$sb.AppendLine("	""tags"": [" + (($Tags | ForEach-Object { """$_""" }) -join ', ') + "],")
    [void]$sb.AppendLine("	""size"": [$sx, $sy, $sz],")

    [void]$sb.AppendLine('	"placement": {')
    $biomes = ($Placement.biomes | ForEach-Object { """$_""" }) -join ', '
    [void]$sb.AppendLine("		""rarity"": $($Placement.rarity),")
    [void]$sb.AppendLine("		""conform"": ""$($Placement.conform)"",")
    [void]$sb.AppendLine("		""embed_depth"": $($Placement.embed_depth),")
    [void]$sb.AppendLine("		""max_slope"": $($Placement.max_slope),")
    [void]$sb.AppendLine("		""foundation"": ""$($Placement.foundation)"",")
    [void]$sb.AppendLine("		""max_foundation_depth"": $($Placement.max_foundation_depth),")
    [void]$sb.AppendLine("		""biomes"": [$biomes],")
    [void]$sb.AppendLine('		"underwater": false')
    [void]$sb.AppendLine('	},')

    [void]$sb.AppendLine('	"palette": [')
    for ($p = 0; $p -lt $Palette.Count; $p++) {
        $sep = if ($p -lt $Palette.Count - 1) { ',' } else { '' }
        [void]$sb.AppendLine("		{ ""block"": ""$($Palette[$p])"" }$sep")
    }
    [void]$sb.AppendLine('	],')

    [void]$sb.AppendLine('	"voxels": [')
    $lines = New-Object 'System.Collections.Generic.List[string]'
    for ($r = 0; $r -lt $runs.Count; $r += 16) {
        $end = [Math]::Min($r + 16, $runs.Count)
        $lines.Add('		' + ($runs[$r..($end - 1)] -join ', '))
    }
    [void]$sb.AppendLine(($lines -join ",`n"))
    [void]$sb.AppendLine('	],')

    [void]$sb.AppendLine('	"markers": [')
    for ($m = 0; $m -lt $Markers.Count; $m++) {
        $mk = $Markers[$m]
        $pos = "[$($mk.pos[0]), $($mk.pos[1]), $($mk.pos[2])]"
        $extra = ''
        if ($mk.loot)  { $extra += ", ""loot_table"": ""$($mk.loot)""" }
        if ($mk.spawn) { $extra += ", ""spawn_group"": ""$($mk.spawn)"", ""count"": $($mk.count)" }
        $sep = if ($m -lt $Markers.Count - 1) { ',' } else { '' }
        [void]$sb.AppendLine("		{ ""type"": ""$($mk.type)"", ""position"": $pos$extra }$sep")
    }
    [void]$sb.AppendLine('	]')
    [void]$sb.AppendLine('}')

    $file = Join-Path $OutDir (($Id -replace ':', '__') + '.json')
    [System.IO.File]::WriteAllText($file, $sb.ToString())

    $solid = ($voxels | Where-Object { $_ -ge 2 }).Count
    Write-Host ("{0,-28} {1,2}x{2,2}x{3,2}  tier {4}  runs {5,4}  solid {6,5}  -> {7}" -f `
        $Id, $sx, $sy, $sz, $Tier, $runs.Count, $solid, (Split-Path -Leaf $file))
}

# ---------------------------------------------------------------------------
# Ruined bunker - tier 2
# ---------------------------------------------------------------------------
Write-Prefab -Id 'madfall:ruined_bunker' -DisplayName 'Ruined Bunker' -Tier 2 `
    -Tags @('poi.military', 'poi.shelter') -Size @(13, 11, 7) `
    -Palette @('*', 'madfall:air', 'madfall:rebar_concrete', 'madfall:steel_beam', 'madfall:concrete_frame') `
    -Placement @{ rarity = 1.0; conform = 'base'; embed_depth = 1; max_slope = 6; foundation = 'madfall:concrete_frame';
                  max_foundation_depth = 10; biomes = @('madfall:plains', 'madfall:tundra', 'madfall:forest', 'madfall:desert', 'madfall:highlands') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(6, 0, 1) },
        @{ type = 'loot'; pos = @(2, 8, 1); loot = 'madfall:loot/ammo_crate' },
        @{ type = 'loot'; pos = @(10, 2, 1); loot = 'madfall:loot/medical_cabinet' },
        @{ type = 'spawn'; pos = @(6, 6, 1); spawn = 'madfall:zombies/soldier'; count = 3 }
    ) `
    -Shape {
        param($x, $y, $z)
        $edge = ($x -eq 0 -or $x -eq 12 -or $y -eq 0 -or $y -eq 10)
        if ($z -eq 0) { return 4 }
        if ($z -le 4) {
            if ($y -eq 0 -and $x -ge 5 -and $x -le 7 -and $z -le 3) { return 1 }   # doorway
            if ($edge) { return 2 }
            return 1
        }
        if ($z -eq 5) {
            if ($x -ge 8 -and $x -le 10 -and $y -ge 6 -and $y -le 8) { return 1 }  # collapsed section
            return 3
        }
        # z = 6: broken parapet, every other block
        if ($edge -and (($x + $y) % 2 -eq 0)) { return 2 }
        return 0
    }

# ---------------------------------------------------------------------------
# Watchtower - tier 1
# ---------------------------------------------------------------------------
Write-Prefab -Id 'madfall:watchtower' -DisplayName 'Watchtower' -Tier 1 `
    -Tags @('poi.lookout') -Size @(7, 7, 16) `
    -Palette @('*', 'madfall:air', 'madfall:wood_reinforced', 'madfall:wood_frame') `
    -Placement @{ rarity = 1.2; conform = 'base'; embed_depth = 0; max_slope = 10; foundation = 'madfall:wood_reinforced';
                  max_foundation_depth = 12; biomes = @() } `
    -Markers @(
        @{ type = 'entrance'; pos = @(3, 1, 0) },
        @{ type = 'loot'; pos = @(3, 3, 13); loot = 'madfall:loot/scout_cache' }
    ) `
    -Shape {
        param($x, $y, $z)
        $post = (($x -eq 1 -or $x -eq 5) -and ($y -eq 1 -or $y -eq 5))
        if ($z -le 11) {
            if ($post) { return 2 }
            # cross bracing
            if (($z -eq 4 -or $z -eq 8) -and ((($y -eq 1 -or $y -eq 5) -and $x -ge 1 -and $x -le 5) -or (($x -eq 1 -or $x -eq 5) -and $y -ge 1 -and $y -le 5))) { return 3 }
            # ladder column
            if ($x -eq 3 -and $y -eq 1) { return 3 }
            return 0
        }
        if ($z -eq 12) { return 3 }                                                     # platform
        $rim = ($x -eq 0 -or $x -eq 6 -or $y -eq 0 -or $y -eq 6)
        $corner = (($x -eq 0 -or $x -eq 6) -and ($y -eq 0 -or $y -eq 6))
        # The main posts carry on up through the railing to the roof. With roof
        # posts only at the rim corners the roof centre is 6 steps plus the
        # platform from any load path, and the structural solver drops it.
        if ($z -ge 13 -and $post) { return 2 }
        if ($z -eq 13) { if ($rim) { return 3 } else { return 1 } }                     # railing
        if ($z -eq 14) { if ($corner) { return 2 } else { return 1 } }                  # roof posts
        return 2                                                                        # roof
    }

# ---------------------------------------------------------------------------
# Hunting cabin - tier 1
# ---------------------------------------------------------------------------
Write-Prefab -Id 'madfall:hunting_cabin' -DisplayName 'Hunting Cabin' -Tier 1 `
    -Tags @('poi.residential', 'poi.shelter') -Size @(9, 11, 7) `
    -Palette @('*', 'madfall:air', 'madfall:wood_reinforced', 'madfall:wood_frame', 'madfall:stone') `
    -Placement @{ rarity = 1.5; conform = 'base'; embed_depth = 1; max_slope = 7; foundation = 'madfall:stone';
                  max_foundation_depth = 8; biomes = @('madfall:forest', 'madfall:plains', 'madfall:tundra') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(4, 0, 1) },
        @{ type = 'loot'; pos = @(6, 8, 1); loot = 'madfall:loot/cabin_supplies' },
        @{ type = 'spawn'; pos = @(2, 8, 1); spawn = 'madfall:zombies/civilian'; count = 1 }
    ) `
    -Shape {
        param($x, $y, $z)
        if ($x -eq 7 -and $y -eq 9 -and $z -ge 1) { return 4 }                          # chimney
        $edge = ($x -eq 0 -or $x -eq 8 -or $y -eq 0 -or $y -eq 10)
        if ($z -eq 0) { return 3 }
        if ($z -le 3) {
            if ($y -eq 0 -and $x -eq 4 -and $z -le 2) { return 1 }                       # door
            if ($y -eq 5 -and ($x -eq 0 -or $x -eq 8) -and $z -eq 2) { return 1 }       # windows
            if ($edge) { return 2 }
            return 1
        }
        if ($z -eq 4) { return 3 }
        if ($z -eq 5) { if ($x -ge 1 -and $x -le 7) { return 3 } else { return 0 } }
        if ($x -ge 3 -and $x -le 5) { return 3 }
        return 0
    }

# ---------------------------------------------------------------------------
# Military outpost - tier 4
# ---------------------------------------------------------------------------
Write-Prefab -Id 'madfall:military_outpost' -DisplayName 'Military Outpost' -Tier 4 `
    -Tags @('poi.military', 'poi.fortified') -Size @(25, 21, 9) `
    -Palette @('*', 'madfall:air', 'madfall:rebar_concrete', 'madfall:steel_beam', 'madfall:concrete_frame') `
    -Placement @{ rarity = 0.8; conform = 'base'; embed_depth = 1; max_slope = 5; foundation = 'madfall:concrete_frame';
                  max_foundation_depth = 14; biomes = @('madfall:plains', 'madfall:tundra', 'madfall:desert', 'madfall:highlands') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(12, 0, 1) },
        @{ type = 'loot'; pos = @(10, 12, 1); loot = 'madfall:loot/weapons_locker' },
        @{ type = 'loot'; pos = @(14, 12, 1); loot = 'madfall:loot/ammo_crate' },
        @{ type = 'loot'; pos = @(12, 13, 1); loot = 'madfall:loot/medical_cabinet' },
        @{ type = 'spawn'; pos = @(5, 10, 1); spawn = 'madfall:zombies/soldier'; count = 4 },
        @{ type = 'spawn'; pos = @(19, 10, 1); spawn = 'madfall:zombies/soldier'; count = 4 },
        @{ type = 'spawn'; pos = @(12, 11, 1); spawn = 'madfall:zombies/brute'; count = 1 }
    ) `
    -Shape {
        param($x, $y, $z)
        $perimeter = ($x -eq 0 -or $x -eq 24 -or $y -eq 0 -or $y -eq 20)
        $corner = (($x -eq 0 -or $x -eq 24) -and ($y -eq 0 -or $y -eq 20))
        $house = ($x -ge 8 -and $x -le 16 -and $y -ge 8 -and $y -le 14)
        $houseEdge = $house -and ($x -eq 8 -or $x -eq 16 -or $y -eq 8 -or $y -eq 14)

        if ($z -eq 0) { return 4 }                                                     # courtyard pad

        if ($perimeter) {
            if ($y -eq 0 -and $x -ge 11 -and $x -le 13 -and $z -le 3) { return 1 }    # gate
            if ($z -le 3) { return 2 }
            if ($corner -and $z -le 7) { return 3 }                                    # corner posts
            return 0
        }

        if ($house) {
            if ($z -le 6) {
                if ($y -eq 8 -and $x -eq 12 -and $z -le 2) { return 1 }               # blockhouse door
                if ($houseEdge) { return 2 }
                return 1
            }
            if ($z -eq 7) { return 3 }                                                 # blockhouse roof
            return 0
        }

        if ($z -le 3) { return 1 }                                                     # clear the courtyard
        return 0
    }
