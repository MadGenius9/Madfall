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

# ---------------------------------------------------------------------------
# Desert way station - tier 1
# ---------------------------------------------------------------------------
#
# WHY: counted by biome, the desert had two POIs of its own and both were out
# of reach of a new survivor - the ruined bunker at tier 2 and the military
# outpost at tier 4 - leaving the watchtower, which lists no biomes and so
# stands in all of them. The first hours in the sand were a watchtower or
# nothing. Cells near the spawn are tier 1, so only a tier-1 prefab fills it.
Write-Prefab -Id 'madfall:desert_way_station' -DisplayName 'Way Station' -Tier 1 `
    -Tags @('poi.roadside', 'poi.shelter') -Size @(11, 9, 6) `
    -Palette @('*', 'madfall:air', 'madfall:stone', 'madfall:wood_frame', 'madfall:gravel_path') `
    -Placement @{ rarity = 1.6; conform = 'base'; embed_depth = 1; max_slope = 8; foundation = 'madfall:stone';
                  max_foundation_depth = 8; biomes = @('madfall:desert', 'madfall:beach') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(3, 1, 1) },
        @{ type = 'loot'; pos = @(3, 4, 1); loot = 'madfall:loot/scout_cache' },
        @{ type = 'spawn'; pos = @(8, 4, 1); spawn = 'madfall:zombies/civilian'; count = 1 }
    ) `
    -Shape {
        param($x, $y, $z)
        if ($z -eq 0) { return 4 }                                                     # gravel pad
        $hut = ($x -ge 1 -and $x -le 5 -and $y -ge 1 -and $y -le 7)
        $hutEdge = $hut -and ($x -eq 1 -or $x -eq 5 -or $y -eq 1 -or $y -eq 7)
        $shade = ($x -ge 7 -and $x -le 9 -and $y -ge 2 -and $y -le 6)

        if ($hut) {
            if ($z -le 3) {
                if ($y -eq 1 -and $x -eq 3 -and $z -le 2) { return 1 }                  # doorway
                if ($x -eq 5 -and $y -eq 4 -and $z -eq 2) { return 1 }                  # shutter
                if ($hutEdge) { return 2 }
                return 1
            }
            if ($z -eq 4) { return 3 }                                                 # roof
            return 0
        }

        if ($shade) {
            # Four posts and a plank roof: shade is the whole point of it.
            if ($z -le 3) {
                if (($x -eq 7 -or $x -eq 9) -and ($y -eq 2 -or $y -eq 6)) { return 3 }
                return 1
            }
            if ($z -eq 4) { return 3 }
            return 0
        }

        if ($z -le 4) { return 1 }                                                     # clear the yard
        return 0
    }

# ---------------------------------------------------------------------------
# Fishing shack - tier 1
# ---------------------------------------------------------------------------
#
# WHY: the beach had nothing of its own in any tier - only the watchtower,
# which stands in every biome and so says nothing about where you are. It is
# also the biome a survivor is most likely to walk along, because the coast is
# the one line in the world that reads as a direction.
Write-Prefab -Id 'madfall:fishing_shack' -DisplayName 'Fishing Shack' -Tier 1 `
    -Tags @('poi.residential', 'poi.shelter') -Size @(9, 11, 6) `
    -Palette @('*', 'madfall:air', 'madfall:wood_reinforced', 'madfall:wood_frame', 'madfall:base_log') `
    -Placement @{ rarity = 1.5; conform = 'base'; embed_depth = 1; max_slope = 6; foundation = 'madfall:wood_frame';
                  max_foundation_depth = 6; biomes = @('madfall:beach') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(3, 4, 1) },
        @{ type = 'loot'; pos = @(3, 7, 1); loot = 'madfall:loot/food' },
        @{ type = 'spawn'; pos = @(2, 6, 1); spawn = 'madfall:zombies/civilian'; count = 1 }
    ) `
    -Shape {
        param($x, $y, $z)
        $shack = ($x -ge 1 -and $x -le 5 -and $y -ge 4 -and $y -le 9)
        $shackEdge = $shack -and ($x -eq 1 -or $x -eq 5 -or $y -eq 4 -or $y -eq 9)
        $jetty = ($x -ge 2 -and $x -le 4 -and $y -le 3)

        if ($z -eq 0) {
            if ($shack) { return 3 }                                                   # plank floor
            if ($jetty) { return 4 }                                                   # posts in the sand
            return 0
        }

        if ($shack) {
            if ($z -le 3) {
                if ($y -eq 4 -and $x -eq 3 -and $z -le 2) { return 1 }                  # door onto the jetty
                if ($x -eq 1 -and $y -eq 7 -and $z -eq 2) { return 1 }                  # window
                if ($shackEdge) { return 2 }
                return 1
            }
            if ($z -eq 4) { return 3 }
            return 0
        }

        if ($jetty -and $z -eq 1) { return 3 }                                         # the deck
        if ($z -le 4) { return 1 }
        return 0
    }

# ---------------------------------------------------------------------------
# Logging camp - tier 2
# ---------------------------------------------------------------------------
#
# WHY: the forest had the cabin and the bunker, and the bunker is in five
# biomes, so a forest walk was a cabin or a building you had already seen
# elsewhere. A camp is also the one POI whose loot says what it was for.
Write-Prefab -Id 'madfall:logging_camp' -DisplayName 'Logging Camp' -Tier 2 `
    -Tags @('poi.industrial', 'poi.shelter') -Size @(15, 13, 7) `
    -Palette @('*', 'madfall:air', 'madfall:wood_reinforced', 'madfall:wood_frame', 'madfall:pine_log', 'madfall:storage_barrel') `
    -Placement @{ rarity = 1.3; conform = 'base'; embed_depth = 1; max_slope = 7; foundation = 'madfall:stone';
                  max_foundation_depth = 10; biomes = @('madfall:forest', 'madfall:tundra') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(4, 1, 1) },
        @{ type = 'loot'; pos = @(3, 5, 1); loot = 'madfall:loot/tools' },
        @{ type = 'loot'; pos = @(5, 5, 1); loot = 'madfall:loot/cabin_supplies' },
        @{ type = 'spawn'; pos = @(2, 3, 1); spawn = 'madfall:zombies/civilian'; count = 2 }
    ) `
    -Shape {
        param($x, $y, $z)
        if ($z -eq 0) { return 3 }                                                     # planked yard
        $cabin = ($x -ge 1 -and $x -le 7 -and $y -ge 1 -and $y -le 7)
        $cabinEdge = $cabin -and ($x -eq 1 -or $x -eq 7 -or $y -eq 1 -or $y -eq 7)
        $piles = ($x -ge 10 -and $x -le 13) -and (($y -ge 2 -and $y -le 4) -or ($y -ge 8 -and $y -le 10))

        if ($cabin) {
            if ($z -le 4) {
                if ($y -eq 1 -and $x -eq 4 -and $z -le 2) { return 1 }                  # door
                if ($x -eq 7 -and $y -eq 4 -and $z -eq 2) { return 1 }                  # window
                if ($cabinEdge) { return 2 }
                return 1
            }
            if ($z -eq 5) { return 3 }                                                 # roof
            return 0
        }

        if ($piles -and $z -le 2) { return 4 }                                         # stacked timber
        if ($x -eq 9 -and $y -eq 6 -and $z -eq 1) { return 5 }                          # a barrel by the track
        if ($z -le 5) { return 1 }
        return 0
    }

# ---------------------------------------------------------------------------
# Collapsed warehouse - tier 3
# ---------------------------------------------------------------------------
#
# WHY: the tiers went 1, 2, 4 - there was nothing between the bunker and the
# military outpost, so the middle of a run had no POI that was new. Half of
# this one has come down, which is the point: the standing half is a roofed
# building to clear, and the fallen half is rubble to dig through to reach it.
Write-Prefab -Id 'madfall:collapsed_warehouse' -DisplayName 'Collapsed Warehouse' -Tier 3 `
    -Tags @('poi.industrial', 'poi.ruin') -Size @(21, 17, 8) `
    -Palette @('*', 'madfall:air', 'madfall:concrete_frame', 'madfall:rebar_concrete', 'madfall:steel_beam', 'madfall:concrete_rubble') `
    -Placement @{ rarity = 1.0; conform = 'base'; embed_depth = 1; max_slope = 5; foundation = 'madfall:concrete_frame';
                  max_foundation_depth = 12; biomes = @('madfall:plains', 'madfall:desert', 'madfall:tundra', 'madfall:forest') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(9, 0, 1) },
        @{ type = 'loot'; pos = @(4, 8, 1); loot = 'madfall:loot/ammo_crate' },
        @{ type = 'loot'; pos = @(4, 12, 1); loot = 'madfall:loot/tools' },
        @{ type = 'loot'; pos = @(8, 12, 1); loot = 'madfall:loot/medical_cabinet' },
        @{ type = 'spawn'; pos = @(3, 4, 1); spawn = 'madfall:zombies/civilian'; count = 3 },
        @{ type = 'spawn'; pos = @(9, 8, 1); spawn = 'madfall:zombies/soldier'; count = 2 }
    ) `
    -Shape {
        param($x, $y, $z)
        if ($z -eq 0) { return 2 }                                                     # the slab
        $wall = ($x -eq 0 -or $x -eq 20 -or $y -eq 0 -or $y -eq 16)
        $fallen = ($x -ge 12)                                                          # the half that came down

        if ($wall) {
            if ($y -eq 0 -and $x -ge 8 -and $x -le 11 -and $z -le 3) { return 1 }      # loading door
            if ($fallen) { if ($z -le 2) { return 3 } else { return 0 } }               # broken off at waist height
            if ($z -le 6) { return 3 }
            return 0
        }

        # Two rows of roof posts. They are what is holding the standing half up,
        # and what the fallen half is missing.
        if (($x -eq 6 -or $x -eq 12) -and ($y % 6) -eq 3 -and $z -le 5) { return 4 }

        if ($fallen) {
            # Scattered, not striped: (x + y) % 3 laid the rubble in clean
            # diagonal rows that read as tiling the moment you stood in it.
            # Two coprime multipliers into a prime modulus break the eye's line
            # while staying a pure function of the position, which is what lets
            # the prefab be data rather than a seed.
            if ($z -eq 1 -and ((($x * 37 + $y * 101) % 17) -lt 11)) { return 5 }         # rubble to dig through
            if ($z -le 6) { return 1 }
            return 0
        }

        if ($z -eq 6) { return 3 }                                                     # the roof that held
        if ($z -le 6) { return 1 }
        return 0
    }

# ---------------------------------------------------------------------------
# Wrecked freighter - tier 3
# ---------------------------------------------------------------------------
#
# WHY: with the shack added, every POI on the beach was tier 1, so the coast
# was somewhere to start and never somewhere to go - a survivor who walked it
# outward found the same two buildings further from home. MadFall.WorldGen.
# PoiContent fails on exactly that ("beach has somewhere to go later"), which
# is how this one came to be written.
#
# The hull tapers to a point at both ends: half-width is 6 amidships and loses
# a voxel a step outside that, which is a bow and a stern without a curve
# table. The deck survives only amidships, so the hold is open to the sky at
# both ends - that is the way in, and the reason it reads as a wreck from
# outside rather than a box.
Write-Prefab -Id 'madfall:wrecked_freighter' -DisplayName 'Wrecked Freighter' -Tier 3 `
    -Tags @('poi.industrial', 'poi.ruin') -Size @(27, 13, 9) `
    -Palette @('*', 'madfall:air', 'madfall:steel_beam', 'madfall:rebar_concrete', 'madfall:concrete_rubble') `
    -Placement @{ rarity = 1.1; conform = 'base'; embed_depth = 2; max_slope = 6; foundation = 'madfall:sand';
                  max_foundation_depth = 10; biomes = @('madfall:beach') } `
    -Markers @(
        @{ type = 'entrance'; pos = @(13, 0, 1) },
        @{ type = 'loot'; pos = @(10, 6, 1); loot = 'madfall:loot/weapons_locker' },
        @{ type = 'loot'; pos = @(15, 6, 1); loot = 'madfall:loot/medical_cabinet' },
        @{ type = 'loot'; pos = @(13, 4, 1); loot = 'madfall:loot/tools' },
        @{ type = 'spawn'; pos = @(11, 8, 1); spawn = 'madfall:zombies/civilian'; count = 4 },
        @{ type = 'spawn'; pos = @(17, 6, 1); spawn = 'madfall:zombies/brute'; count = 1 }
    ) `
    -Shape {
        param($x, $y, $z)
        $half = 6 - [Math]::Max(0, 6 - $x) - [Math]::Max(0, $x - 20)
        $dy = [Math]::Abs($y - 6)
        if ($half -lt 0 -or $dy -gt $half) { return 0 }                                # outside the hull

        if ($z -eq 0) { return 2 }                                                     # the keel
        if ($dy -eq $half) {
            if ($x -ge 12 -and $x -le 14 -and $y -eq 0 -and $z -le 2) { return 1 }      # the gash you get in by
            # Up to the deck, not short of it: at z <= 5 the plates stopped a
            # voxel below the deck they are supposed to carry, and
            # MadFall.Structural.ShippedPrefabsStand called all 121 deck blocks
            # unsupported with no path to the ground. A floating slab.
            if ($z -le 6) { return 2 }                                                 # hull plate
            return 0
        }

        if ($z -eq 6 -and $x -ge 8 -and $x -le 18) { return 3 }                          # what is left of the deck
        if ($z -le 2 -and $x -ge 21) { return 4 }                                       # silt in the bow
        if (($x % 5) -eq 0 -and $z -le 1 -and $dy -le 2) { return 3 }                    # cargo stacks
        if ($z -le 6) { return 1 }
        return 0
    }
