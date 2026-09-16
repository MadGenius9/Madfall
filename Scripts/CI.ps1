# Copyright MadFall. All Rights Reserved.
#
# MadFall CI entry point.
#
#   .\Scripts\CI.ps1                  # standard gate (single-player scope)
#   .\Scripts\CI.ps1 -RequireServer   # also enforce the dedicated server build
#
# Gates, in order. Any one failing fails the run:
#   1. Editor target builds            (needed to run automation tests at all)
#   2. Game (client) target builds
#   3. Dedicated server target builds  (opt-in: -RequireServer, see below)
#   4. Zero compiler warnings anywhere in the build output
#   5. The test map exists or can be regenerated
#   6. Every MadFall.* automation test passes
#   7. Acceptance: a voxel edit survives save, unload and reload end to end
#   8. Acceptance: a damaged structure collapses through the live subsystems
#   9. Acceptance: the survival loop plays headless in -game (craft, mine, build, loot, die, respawn),
#      then a second session restores the survivor from gameplay.json
#  10. Acceptance: zombies reach and hit the player, undermine a pillar the player stands on, dig through a wall,
#      and horde night starts on day 7
#  11. Package: the cooked game and every example mod build; the packaged exe runs and a content mod's
#      cooked material renders (skip with -SkipPackage)
#  12. Frame budget: rendered horde, streaming and collapse keep MadFall under 2 ms per frame (mad.perf)
#      (runs before 11)
#
# On gate 3: MadFall is single-player for now, so the dedicated server build is
# reported as SKIPPED rather than FAILED by default. Turn it back into a hard
# gate with -RequireServer when Phase 6 (multiplayer) starts. Note that gate 2
# still catches editor-only includes leaking into runtime code, because the
# client target excludes MadFallEditor - that protection does not depend on a
# server binary.
#
# On warnings: each module sets bWarningsAsErrors, so a C++ warning is already a
# hard compile error. Gate 4 exists to catch the warnings that flag cannot reach
# - C# warnings from Target.cs / Build.cs rules assemblies, and UHT warnings -
# which otherwise scroll past a green build forever.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'DebugGame', 'Development', 'Test', 'Shipping')]
    [string] $Configuration = 'Development',

    [string] $EngineRoot,

    # Promote the dedicated server build from an informational SKIP to a hard
    # gate. Set this when multiplayer work starts (Phase 6). It requires a
    # SOURCE build of Unreal Engine - a Launcher install cannot build Server
    # targets at all.
    [switch] $RequireServer,

    [switch] $SkipTests,

    # Skip gate 11 (package the game and mods, run the packaged build).
    [switch] $SkipPackage
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $RepoRoot 'MadFall.uproject'
$LogDir = Join-Path $RepoRoot 'Saved\CI'
$AutomationDir = Join-Path $RepoRoot 'Saved\Automation'

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$script:Failures = @()
$script:Skipped = @()

function Resolve-EngineRoot {
    param([string] $Explicit)

    $candidates = @($Explicit, $env:MADFALL_UE_ROOT, 'C:\Program Files\Epic Games\UE_5.8') |
        Where-Object { $_ }

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'Engine\Build\BatchFiles\Build.bat')) {
            return $candidate
        }
    }

    throw "Could not find an Unreal Engine install. Set MADFALL_UE_ROOT or pass -EngineRoot."
}

function Write-Section {
    param([string] $Text)
    Write-Host ''
    Write-Host "=== $Text " -ForegroundColor Cyan -NoNewline
    Write-Host ('=' * [Math]::Max(0, 66 - $Text.Length)) -ForegroundColor Cyan
}

$Engine = Resolve-EngineRoot -Explicit $EngineRoot
$BuildBat = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat'
$EditorCmd = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$IsInstalledEngine = Test-Path (Join-Path $Engine 'Engine\Build\InstalledBuild.txt')

Write-Host "MadFall CI" -ForegroundColor White
Write-Host "  Engine:        $Engine"
Write-Host "  Installed:     $IsInstalledEngine"
Write-Host "  Configuration: $Configuration"

# ---------------------------------------------------------------------------
# Build gates
# ---------------------------------------------------------------------------

function Invoke-BuildTarget {
    param(
        [string] $Target,
        [string] $Platform = 'Win64'
    )

    Write-Section "BUILD $Target ($Platform, $Configuration)"

    $logFile = Join-Path $LogDir "build-$Target-$Platform.log"
    & $BuildBat $Target $Platform $Configuration -Project="$ProjectFile" -WaitMutex -FromMsBuild 2>&1 |
        Tee-Object -FilePath $logFile
    $code = $LASTEXITCODE

    if ($code -ne 0) {
        $script:Failures += "build:$Target($Platform)"
        Write-Host "FAILED: $Target build exited $code" -ForegroundColor Red
        return $false
    }

    Write-Host "OK: $Target" -ForegroundColor Green
    return $true
}

Invoke-BuildTarget -Target 'MadFallEditor' | Out-Null
Invoke-BuildTarget -Target 'MadFall' | Out-Null

Write-Section 'BUILD MadFallServer (Win64)'
if (-not $RequireServer) {
    Write-Host 'SKIPPED: MadFall is single-player for now; the dedicated server is a Phase 6 concern.' -ForegroundColor Yellow
    Write-Host '         Source/MadFallServer.Target.cs is kept so Phase 6 does not start from scratch.' -ForegroundColor Yellow
    Write-Host '         Pass -RequireServer to make this a hard gate (needs a SOURCE engine build).' -ForegroundColor Yellow
    $script:Skipped += 'build:MadFallServer'
}
elseif ($IsInstalledEngine) {
    Write-Host @"
FAILED: -RequireServer was passed, but $Engine is a Launcher (installed) engine
and ships no Server target libraries. A dedicated server requires a source build
of Unreal Engine (github.com/EpicGames/UnrealEngine, branch 5.8).
"@ -ForegroundColor Red
    $script:Failures += 'build:MadFallServer'
}
else {
    Invoke-BuildTarget -Target 'MadFallServer' | Out-Null
    # Linux server is built from Windows via Epic's clang cross-toolchain
    # (-v25 for 5.8). Absent toolchain is a skip, not a silent pass.
    if ($env:LINUX_MULTIARCH_ROOT) {
        Invoke-BuildTarget -Target 'MadFallServer' -Platform 'Linux' | Out-Null
    }
    else {
        Write-Host 'SKIPPED Linux server: LINUX_MULTIARCH_ROOT is not set (cross-toolchain not installed).' -ForegroundColor Yellow
        $script:Skipped += 'build:MadFallServer(Linux)'
    }
}

# ---------------------------------------------------------------------------
# Warning gate
# ---------------------------------------------------------------------------

Write-Section 'WARNINGS'

# Compiler-style diagnostics only: "file(line,col): warning XNNNN: text".
# Matching bare "warning" would trip on UBT progress chatter.
$warningPattern = '\(\d+(,\d+)?\):\s*warning\s'
$warnings = Get-ChildItem -Path $LogDir -Filter 'build-*.log' |
    Select-String -Pattern $warningPattern |
    ForEach-Object { $_.Line.Trim() } |
    Sort-Object -Unique

if ($warnings.Count -gt 0) {
    Write-Host "FAILED: $($warnings.Count) compiler warning(s):" -ForegroundColor Red
    $warnings | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $script:Failures += 'warnings'
}
else {
    Write-Host 'OK: no compiler warnings.' -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Test map
# ---------------------------------------------------------------------------

Write-Section 'TEST MAP'

$MapFile = Join-Path $RepoRoot 'Content\Maps\L_MadFall_Test.umap'
if (-not (Test-Path $MapFile)) {
    Write-Host 'Test map missing; regenerating from Scripts/make_test_map.py ...' -ForegroundColor Yellow
    & $EditorCmd $ProjectFile `
        -ExecutePythonScript="$(Join-Path $RepoRoot 'Scripts\make_test_map.py')" `
        -unattended -nopause -nosplash -stdout -NoLogTimes |
        Tee-Object -FilePath (Join-Path $LogDir 'make-test-map.log') | Out-Null
}

if (Test-Path $MapFile) {
    Write-Host "OK: $MapFile" -ForegroundColor Green
}
else {
    Write-Host 'FAILED: test map could not be created.' -ForegroundColor Red
    $script:Failures += 'test-map'
}

# ---------------------------------------------------------------------------
# Automation tests
# ---------------------------------------------------------------------------

if ($SkipTests) {
    Write-Section 'TESTS (skipped)'
    $script:Skipped += 'tests'
}
else {
    Write-Section 'AUTOMATION TESTS'

    if (Test-Path $AutomationDir) {
        Remove-Item -Recurse -Force $AutomationDir
    }

    & $EditorCmd $ProjectFile `
        -ExecCmds='Automation RunTests MadFall' `
        -TestExit='Automation Test Queue Empty' `
        -ReportExportPath="$AutomationDir" `
        -unattended -nopause -nosplash -nullrhi -stdout -NoLogTimes |
        Tee-Object -FilePath (Join-Path $LogDir 'automation.log') | Out-Null

    $reportFile = Join-Path $AutomationDir 'index.json'
    if (-not (Test-Path $reportFile)) {
        Write-Host 'FAILED: no automation report was written - the editor likely crashed.' -ForegroundColor Red
        $script:Failures += 'tests:no-report'
    }
    else {
        $report = Get-Content -Raw $reportFile | ConvertFrom-Json

        Write-Host ("  passed: {0}  warned: {1}  failed: {2}  notRun: {3}" -f `
            $report.succeeded, $report.succeededWithWarnings, $report.failed, $report.notRun)

        foreach ($test in $report.tests) {
            $colour = if ($test.state -eq 'Success') { 'Green' } else { 'Red' }
            Write-Host ("  [{0}] {1}" -f $test.state, $test.fullTestPath) -ForegroundColor $colour

            if ($test.state -ne 'Success') {
                foreach ($entry in $test.entries) {
                    if ($entry.event.type -ne 'Info') {
                        Write-Host ("      {0}" -f $entry.event.message) -ForegroundColor Red
                    }
                }
            }
        }

        # notRun counts as failure: a test that silently stopped being discovered
        # is indistinguishable from a test that was deleted.
        if ($report.failed -gt 0 -or $report.notRun -gt 0) {
            $script:Failures += 'tests'
        }
        elseif ($report.succeeded -eq 0) {
            Write-Host 'FAILED: zero tests ran. Expected at least one MadFall.* test.' -ForegroundColor Red
            $script:Failures += 'tests:none-ran'
        }
        else {
            Write-Host 'OK: all automation tests passed.' -ForegroundColor Green
        }
    }
}

# ---------------------------------------------------------------------------
# Phase 1 acceptance: edit voxels, save, reload, read them back
# ---------------------------------------------------------------------------
#
# The unit tests cover serialization in isolation. This drives the real console
# commands against the real subsystem end to end, which is the only way to catch
# a break in the wiring between them - the registry, the world subsystem, the
# region file and the save path all have to agree for the readback to match.

if ($SkipTests) {
    Write-Section 'ACCEPTANCE (skipped)'
    $script:Skipped += 'acceptance'
}
else {
    Write-Section 'ACCEPTANCE: voxel edit / save / reload'

    $AcceptanceWorld = Join-Path $RepoRoot 'Saved\MadFallWorlds\CIWorld'
    if (Test-Path $AcceptanceWorld) {
        Remove-Item -Recurse -Force $AcceptanceWorld
    }

    $commands = @(
        'mad.chunk.load 0 0 0'
        'mad.chunk.fill 0 0 0 madfall:stone'
        'mad.voxel.set 5 6 7 madfall:rebar_concrete 255 19 5'
        'mad.world.save'
        'mad.chunk.unload 0 0 0'
        'mad.chunk.load 0 0 0'
        'mad.voxel.get 5 6 7'
        'QUIT_EDITOR'
    ) -join ','

    $acceptanceLog = Join-Path $LogDir 'acceptance.log'
    & $EditorCmd $ProjectFile `
        -ExecCmds="$commands" `
        -unattended -nopause -nosplash -nullrhi -stdout -NoLogTimes |
        Tee-Object -FilePath $acceptanceLog | Out-Null

    # The voxel must come back with its block id, density, orientation AND shape
    # variant intact. Checking only the block id would pass even if the rotation
    # byte's bit packing were broken.
    $expected = 'madfall:rebar_concrete  density=255 damage=0 orientation=19 variant=5'
    $readback = Select-String -Path $acceptanceLog -SimpleMatch $expected -Quiet

    if ($readback) {
        Write-Host "OK: (5,6,7) survived save + unload + reload as $expected" -ForegroundColor Green
    }
    else {
        Write-Host 'FAILED: the edited voxel did not come back correctly after a save/reload cycle.' -ForegroundColor Red
        Write-Host "  expected a log line containing: $expected" -ForegroundColor Red
        Select-String -Path $acceptanceLog -Pattern 'LogMadFallVoxel' |
            Select-Object -Last 20 |
            ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
        $script:Failures += 'acceptance'
    }

    # The autosave's world write survives a hard kill: no shutdown save, no
    # region close. An edit made after the autosave must NOT survive, which
    # proves the process really died before saving anything else.
    if (Test-Path $AcceptanceWorld) {
        Remove-Item -Recurse -Force $AcceptanceWorld
    }
    $crashCommands = @(
        'mad.chunk.load 0 0 0'
        'mad.chunk.fill 0 0 0 madfall:stone'
        'mad.voxel.set 5 6 7 madfall:rebar_concrete 255 19 5'
        'mad.world.autosave wait'
        'mad.voxel.set 6 6 7 madfall:wood_frame'
        'mad.world.crash'
    ) -join ','
    $crashLog = Join-Path $LogDir 'autosave-crash.log'
    & $EditorCmd $ProjectFile -ExecCmds="$crashCommands" -unattended -nopause -nosplash -nullrhi -stdout -NoLogTimes |
        Tee-Object -FilePath $crashLog | Out-Null
    $reloadCommands = @(
        'mad.chunk.load 0 0 0'
        'mad.voxel.get 5 6 7'
        'mad.voxel.get 6 6 7'
        'QUIT_EDITOR'
    ) -join ','
    $crashReloadLog = Join-Path $LogDir 'autosave-reload.log'
    & $EditorCmd $ProjectFile -ExecCmds="$reloadCommands" -unattended -nopause -nosplash -nullrhi -stdout -NoLogTimes |
        Tee-Object -FilePath $crashReloadLog | Out-Null

    $crashChecks = @(
        @{ Log = $crashLog;       Pattern = 'World autosave: 1 chunk\(s\) copied .* 1 written to 1 region\(s\) and flushed to disk'; Why = 'the autosave wrote the dirty chunk on a worker' },
        @{ Log = $crashLog;       Pattern = 'Terminating without saving';                                        Why = 'the process was killed without a shutdown save' },
        @{ Log = $crashReloadLog; Pattern = '\(5, 6, 7\) = madfall:rebar_concrete  density=255 damage=0 orientation=19 variant=5'; Why = 'the autosaved edit survived the kill' },
        @{ Log = $crashReloadLog; Pattern = '\(6, 6, 7\) = madfall:stone';                                       Why = 'the edit after the autosave did not (the kill was real)' }
    )
    foreach ($check in $crashChecks) {
        if (Select-String -Path $check.Log -Pattern $check.Pattern -Quiet) {
            Write-Host "OK: $($check.Why)" -ForegroundColor Green
        }
        else {
            Write-Host "FAILED: $($check.Why) - no line matching '$($check.Pattern)'" -ForegroundColor Red
            $script:Failures += 'autosave-crash'
        }
    }
}

# ---------------------------------------------------------------------------
# Phase 4 acceptance: a structure takes damage and actually collapses
# ---------------------------------------------------------------------------
#
# The solver's unit tests run against an in-memory world. This drives the real
# chain - console edit -> SetVoxel -> OnVoxelChanged -> structural subsystem ->
# collapse written back through SetVoxel - which is what the brief means by "the
# base actually takes structural damage".
#
# Scenario: a 4-high concrete_frame column with a 7-block cantilever. Concrete
# spans 6, so the 7th block must fall and the 6th must stay. Then an explosive
# hit destroys the column's second block, and everything above it must come
# down with it.

if ($SkipTests) {
    Write-Section 'STRUCTURAL ACCEPTANCE (skipped)'
    $script:Skipped += 'structural-acceptance'
}
else {
    Write-Section 'ACCEPTANCE: structural collapse'

    $StructuralWorld = Join-Path $RepoRoot 'Saved\MadFallWorlds\CIWorld'
    if (Test-Path $StructuralWorld) {
        Remove-Item -Recurse -Force $StructuralWorld
    }

    $commands = @(
        'mad.chunk.load 0 0 0'
        'mad.chunk.load 0 0 -1'
        'mad.chunk.fill 0 0 0 madfall:air'
        'mad.chunk.fill 0 0 -1 madfall:stone'
    )
    foreach ($z in 0..3) { $commands += "mad.voxel.set 4 4 $z madfall:concrete_frame" }
    foreach ($x in 5..11) { $commands += "mad.voxel.set $x 4 3 madfall:concrete_frame" }
    $commands += @(
        'mad.debris.flush'
        'mad.si.status'
        'mad.voxel.get 10 4 3'
        'mad.voxel.get 11 4 3'
        'mad.damage 4 4 1 5000 madfall:explosive'
        'mad.debris.flush'
        'mad.voxel.get 4 4 3'
        'mad.voxel.get 9 4 3'
        'QUIT_EDITOR'
    )

    $structuralLog = Join-Path $LogDir 'structural-acceptance.log'
    & $EditorCmd $ProjectFile `
        -ExecCmds="$($commands -join ',')" `
        -unattended -nopause -nosplash -nullrhi -stdout -NoLogTimes |
        Tee-Object -FilePath $structuralLog | Out-Null

    $checks = @(
        @{ Text = '(10, 4, 3) = madfall:concrete_frame'; Why = 'the 6th cantilever block (at span) stood' },
        @{ Text = '(11, 4, 3) = madfall:air';            Why = 'the 7th cantilever block (past span) fell' },
        @{ Pattern = 'Creak: madfall:concrete_frame at .* is at (8[5-9]|9[0-9]|1[0-9][0-9])% of its limit'; Why = 'the cantilever at its limit creaked' },
        @{ Pattern = '[1-9][0-9]* creak\(s\)';          Why = 'creaks are counted in mad.si.status' },
        @{ Text = 'destroyed';                            Why = 'the explosive hit destroyed the column block' },
        @{ Text = '(4, 4, 3) = madfall:air';             Why = 'the column above the destroyed block fell' },
        @{ Text = '(9, 4, 3) = madfall:air';             Why = 'the cantilever fell with its column' },
        @{ Pattern = '[1-9][0-9]* impacts, [0-9.]+ kJ';  Why = 'falling debris landed and dealt impact energy' }
    )

    $structuralOk = $true
    foreach ($check in $checks) {
        $found = if ($check.Pattern) { Select-String -Path $structuralLog -Pattern $check.Pattern -Quiet }
                 else { Select-String -Path $structuralLog -SimpleMatch $check.Text -Quiet }
        if ($found) {
            Write-Host "OK: $($check.Why)" -ForegroundColor Green
        }
        else {
            Write-Host "FAILED: $($check.Why) - no log line matching '$($check.Text)$($check.Pattern)'" -ForegroundColor Red
            $structuralOk = $false
        }
    }

    if (-not $structuralOk) {
        Select-String -Path $structuralLog -Pattern 'LogMadFallStructural|LogMadFallVoxel' |
            Select-Object -Last 25 |
            ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
        $script:Failures += 'structural-acceptance'
    }
}

# ---------------------------------------------------------------------------
# Phase 4 acceptance: play the survival loop headless
# ---------------------------------------------------------------------------
#
# Runs the real game (-game, MadGameMode, streaming, the survivor pawn) with no
# renderer, and plays it through mad.onspawn: craft a tool, mine with it, build,
# place and loot a container, eat, die and respawn. Every step goes through the
# same verbs the mouse and keyboard call. What it cannot see - that it FEELS
# right - is for a human with a controller; what it catches is the loop being
# broken.

if ($SkipTests) {
    Write-Section 'GAMEPLAY ACCEPTANCE (skipped)'
    $script:Skipped += 'gameplay-acceptance'
}
else {
    Write-Section 'ACCEPTANCE: survival loop (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $playScript = @(
        'mad.scene.anchor'
        'mad.scene.pad 12'
        # Collision for the new ground cooks a few frames behind it: spawning or
        # interacting in the same frame fails on ground that is not there yet.
        'wait 2'
        'mad.player.xp 300'
        'mad.player.perk madfall:miner'
        'mad.player.craft madfall:stone_pickaxe'
        'wait 4'
        'mad.player.hold madfall:stone_pickaxe'
        'mad.player.aimrel 1 0 -1'
        'mad.player.use 40'
        'mad.player.status'
        'mad.player.give madfall:rock 2'
        'mad.player.repair'
        'mad.player.give madfall:wood_frame 5'
        'mad.player.hold madfall:wood_frame'
        'mad.player.aimrel 2 0 -1'
        'mad.player.place'
        'mad.player.aimrel 2 0 0'
        'wait 1'
        'mad.player.stress'
        'wait 1'
        'mad.player.stress'
        'mad.player.give madfall:loot_crate 1'
        'mad.player.hold madfall:loot_crate'
        'mad.player.aimrel -2 0 -1'
        'mad.player.place'
        'mad.player.aimrel -2 0 0'
        'mad.player.interact'
        'mad.player.store madfall:wood_frame'
        'mad.player.container'
        'mad.player.takeall'
        'mad.player.closeinventory'
        'mad.player.hold madfall:canned_food'
        'mad.player.place'
        'mad.player.overhead madfall:wood_frame 4 1'
        'wait 3'
        'mad.debris.status'
        'mad.player.give madfall:storage_barrel 1'
        'mad.player.hold madfall:storage_barrel'
        'mad.player.aimrel -3 3 -1'
        'mad.player.place'
        'wait 1'
        'mad.models.stats'
        'mad.player.walk 2 0 -1'
        'wait 2.5'
        'mad.audio.stats'
        # Far out and high up, ahead of streaming: the survivor must hang until the
        # ground below has collision, then land on it rather than fall through.
        'mad.player.tp 900 900 80'
        'wait 25'
        'mad.player.status'
        # Out here, away from the walk: every collapsed block becomes rubble, so
        # the slab's rubble would fill the survivor's own voxels and must be kept
        # out (dropped as salvage) instead. At home it walled the survivor in and
        # the footstep walk above went nowhere.
        'mad.debris.RubbleKeepOneIn 1'
        'mad.player.overhead madfall:wood_frame 4 1'
        'wait 3'
        'mad.debris.status'
        'mad.player.buried'
        'mad.debris.RubbleKeepOneIn 3'
        # Back where they spawned: the death backpack must drop at the respawn
        # point, or the reload session never picks the pickaxe back up.
        'mad.scene.tp 0 0 0'
        'wait 10'
        'wait 1'
        'mad.player.damage 1000'
        'wait 6'
        'mad.player.status'
        'quit'
    ) -join '; '

    $gameplayLog = Join-Path $LogDir 'gameplay-acceptance.log'
    $gameProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $gameplayLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $playScript`"")

    # A hung game must fail the gate, not hang CI.
    if (-not $gameProcess.WaitForExit(300000)) {
        $gameProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the script within 300 s.' -ForegroundColor Red
        $script:Failures += 'gameplay-acceptance'
    }
    else {
        $checks = @(
            @{ Pattern = 'Player spawned at';                                  Why = 'the survivor spawned once the world streamed in' },
            @{ Pattern = 'Craft finished: 1 x madfall:stone_pickaxe';          Why = 'crafted a stone pickaxe from the starting kit (timed)' },
            @{ Pattern = 'Perk madfall:miner: ok';                            Why = 'levelled up and bought a perk rank' },
            @{ Pattern = 'Repair: ok';                                         Why = 'repaired the worn pickaxe' },
            @{ Pattern = 'Dropped a backpack';                                 Why = 'death dropped the backpack' },
            @{ Pattern = '[1-9][0-9]* of 40 swing\(s\) landed';                Why = 'mining swings landed on the targeted voxel' },
            @{ Pattern = 'stone_pickaxe x1 \(durability (1[0-9]{2}|2[0-4][0-9])\)'; Why = 'the pickaxe lost durability' },
            @{ Pattern = 'Interact succeeded';                                 Why = 'looted a placed container' },
            @{ Pattern = 'Stress at .*: [0-9]+% \(carrying';                   Why = 'the HUD load readout solved the placed block in the background' },
            @{ Pattern = 'Stored [1-9][0-9]* x madfall:wood_frame in the container'; Why = 'put items into a crate through the inventory screen' },
            @{ Pattern = 'Took [1-9][0-9]* item\(s\) from the container';     Why = 'and took them back out' },
            @{ Pattern = 'Models: [1-9][0-9]* instances';                     Why = 'a placed storage barrel drew as a model instance' },
            @{ Pattern = 'Debris hit MadPlayerCharacter';                      Why = 'a collapsing slab fell on the survivor and hurt them' },
            @{ Pattern = 'pawn hit\(s\), [1-9][0-9]* item\(s\) dropped'; Why = 'the collapse left salvage behind' },
            @{ Pattern = '[1-9][0-9]* kept out of a pawn''s space'; Why = 'rubble that would have filled the survivor''s space was kept out' },
            @{ Pattern = 'Buried: no';                                   Why = 'and the survivor was hurt, not entombed' },
            @{ Pattern = 'Holding the survivor at X=900 Y=900';         Why = 'teleported ahead of streaming, the survivor was held in the air' },
            @{ Ok = [bool]((Select-String -Path $gameplayLog -Pattern 'Holding the survivor' | Select-Object -First 1).Line -match 'X=900 Y=900'); Why = 'and never held before that: building and digging at home only rebuild chunks, which keep their collision' },
            @{ Pattern = 'Player at voxel X=900 Y=900 Z=[1-9][0-9]?,';  Why = 'and landed on the ground, not under it' },
            @{ Pattern = 'Player died of';                                     Why = 'lethal damage killed the survivor' },
            @{ Pattern = 'Player respawned at';                                Why = 'the survivor respawned' },
            @{ Pattern = 'Audio: .*hit_\w+=[1-9]';                             Why = 'mining made impact sounds' },
            @{ Pattern = 'Audio: .*place=[1-9]';                               Why = 'placing blocks made a sound' },
            @{ Pattern = 'Audio: .*step_\w+=[1-9]';                            Why = 'walking made footsteps' },
            @{ Pattern = 'Audio: .*craft_done=[1-9]';                          Why = 'finishing a craft chimed' },
            @{ Pattern = 'Audio: .*collapse=[1-9]';                            Why = 'the collapsing slab crashed' }
        )

        $gameplayOk = $true
        foreach ($check in $checks) {
            # Most checks are a pattern that must appear; a few carry a computed Ok.
            $passed = if ($check.ContainsKey('Ok')) { $check.Ok } else { [bool](Select-String -Path $gameplayLog -Pattern $check.Pattern -Quiet) }
            if ($passed) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why) - no log line matching '$($check.Pattern)'" -ForegroundColor Red
                $gameplayOk = $false
            }
        }

        $placed = @(Select-String -Path $gameplayLog -Pattern 'Secondary use succeeded').Count
        if ($placed -ge 3) {
            Write-Host "OK: placed a block, placed a crate and ate ($placed secondary uses)" -ForegroundColor Green
        }
        else {
            Write-Host "FAILED: expected 3 successful secondary uses (place, place, eat), saw $placed" -ForegroundColor Red
            $gameplayOk = $false
        }

        if (-not $gameplayOk) {
            Select-String -Path $gameplayLog -Pattern 'LogMadFallGameplay' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'gameplay-acceptance'
        }

        # --- save and reload ------------------------------------------------------
        # Quitting saved gameplay.json. A second session must restore the survivor
        # instead of handing out a fresh starting kit: the worn pickaxe from the
        # first session is the proof, because a new survivor has no pickaxe at all.
        $reloadLog = Join-Path $LogDir 'gameplay-reload.log'
        $reloadProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
            -RedirectStandardOutput $reloadLog `
            -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                            "-ExecCmds=`"mad.onspawn mad.player.status; mad.perks; mad.Language de; mad.strings @items.wood_plank; quit`"")

        if (-not $reloadProcess.WaitForExit(240000)) {
            $reloadProcess | Stop-Process -Force
            Write-Host 'FAILED: the reload session did not finish within 240 s.' -ForegroundColor Red
            $script:Failures += 'gameplay-reload'
        }
        else {
            $reloadOk = $true
            foreach ($check in @(
                @{ Pattern = 'Gameplay saved to .*gameplay\.json'; Log = $gameplayLog; Why = 'quitting saved gameplay state' },
                @{ Pattern = 'Restored the survivor from the save'; Log = $reloadLog; Why = 'the next session restored the survivor' },
                @{ Pattern = 'madfall:miner +1/'; Log = $reloadLog; Why = 'the bought perk rank survived the reload' },
                @{ Pattern = '@items.wood_plank -> "Holzbrett" \(de\)'; Log = $reloadLog; Why = 'switching language at runtime picks up the example_german translation mod' },
                @{ Pattern = 'stone_pickaxe x1 \(durability [0-9]+\)'; Log = $reloadLog; Why = 'the pickaxe, recovered from the death backpack, came back in the next session' }
            )) {
                if (Select-String -Path $check.Log -Pattern $check.Pattern -Quiet) {
                    Write-Host "OK: $($check.Why)" -ForegroundColor Green
                }
                else {
                    Write-Host "FAILED: $($check.Why) - no line matching '$($check.Pattern)'" -ForegroundColor Red
                    $reloadOk = $false
                }
            }
            if (-not $reloadOk) {
                $script:Failures += 'gameplay-reload'
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Phase 4 acceptance: zombies hunt, dig, and a horde night starts on schedule
# ---------------------------------------------------------------------------
#
#   1. A civilian spawned 4 voxels away must reach the player and hit them.
#   2. The player walls themselves in with wood frames; a horde zombie outside
#      must path through the wall, break blocks, and hit the player again.
#   3. The clock is set to just before dusk on day 7; the horde director must
#      announce horde night and spawn a wave.

if ($SkipTests) {
    Write-Section 'ZOMBIE ACCEPTANCE (skipped)'
    $script:Skipped += 'zombie-acceptance'
}
else {
    Write-Section 'ACCEPTANCE: zombies and horde night (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $zombieScript = @(
        'mad.ai.Sleepers 0'
        'mad.ai.spawn madfall:zombie_civilian 4 0'
        'wait 10'
        'mad.ai.status'
        'mad.ai.killall'
        'wait 1'
        'mad.player.damage -100'
        'mad.player.pillar 3 madfall:wood_frame'
        'wait 1'
        'mad.player.status'
        'mad.ai.spawn madfall:zombie_civilian 4 0 1'
        'wait 20'
        'mad.player.status'
        'mad.ai.status'
        'mad.ai.killall'
        'wait 1'
        'mad.player.damage -100'
        'mad.player.shelter madfall:wood_frame'
        'mad.ai.spawn madfall:zombie_civilian 7 0 1'
        'wait 40'
        'mad.ai.status'
        'mad.ai.killall'
        'mad.player.damage -100'
        'mad.clock.set 21.98 7'
        'wait 4'
        'mad.ai.status'
        'mad.audio.stats'
        'mad.music.status'
        'quit'
    ) -join '; '

    $zombieLog = Join-Path $LogDir 'zombie-acceptance.log'
    $zombieProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $zombieLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $zombieScript`"")

    if (-not $zombieProcess.WaitForExit(300000)) {
        $zombieProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the zombie script within 300 s.' -ForegroundColor Red
        $script:Failures += 'zombie-acceptance'
    }
    else {
        $statusLines = @(Select-String -Path $zombieLog -Pattern 'lifetime: (\d+) paths, (\d+) block hits, (\d+) player hits, (\d+) kills, (\d+) undermines')
        $playerZ = @(Select-String -Path $zombieLog -Pattern 'Player at voxel X=-?\d+ Y=-?\d+ Z=(-?\d+)')
        $zombieOk = $true

        if ($statusLines.Count -ge 3 -and $playerZ.Count -ge 2) {
            $first = $statusLines[0].Matches[0].Groups
            $pillar = $statusLines[1].Matches[0].Groups
            $second = $statusLines[2].Matches[0].Groups
            $onPillarZ = [int]$playerZ[0].Matches[0].Groups[1].Value
            $afterZ = [int]$playerZ[1].Matches[0].Groups[1].Value
            $checks = @(
                @{ Ok = [int]$first[3].Value -gt 0;                               Why = "a nearby zombie reached and hit the player ($($first[3].Value) hits)" },
                @{ Ok = [int]$pillar[5].Value -gt 0;                              Why = "a zombie that could not reach the player on a pillar attacked its support ($($pillar[5].Value) undermines)" },
                @{ Ok = $afterZ -lt $onPillarZ;                                   Why = "the pillar collapsed and the player came down (feet Z $onPillarZ -> $afterZ)" },
                @{ Ok = [int]$second[2].Value -gt 0;                              Why = "a horde zombie broke blocks of the shelter wall ($($second[2].Value) block hits)" },
                @{ Ok = [int]$second[3].Value -gt [int]$first[3].Value;           Why = "and got through to hit the player again ($($second[3].Value) total hits)" }
            )
            foreach ($check in $checks) {
                if ($check.Ok) { Write-Host "OK: $($check.Why)" -ForegroundColor Green }
                else { Write-Host "FAILED: $($check.Why)" -ForegroundColor Red; $zombieOk = $false }
            }
        }
        else {
            Write-Host "FAILED: expected 3 mad.ai.status and 2 player status reports, found $($statusLines.Count) and $($playerZ.Count)" -ForegroundColor Red
            $zombieOk = $false
        }

        # Sounds are counted even with no audio device, so this proves the events
        # reach the audio system; MadFall.Audio.Synth proves what they sound like.
        foreach ($pattern in @('Horde night 7 begins', 'Spawned [1-9][0-9]* of [0-9]+ horde zombie',
                               'Audio: .*zombie_groan=[1-9]', 'Audio: .*zombie_attack=[1-9]', 'Audio: .*horde_horn=1', 'Audio: .*player_hurt=[1-9]',
                               'Music: horde, .*horde=[1-9]')) {
            if (Select-String -Path $zombieLog -Pattern $pattern -Quiet) {
                Write-Host "OK: log contains '$pattern'" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: no log line matching '$pattern'" -ForegroundColor Red
                $zombieOk = $false
            }
        }

        if (-not $zombieOk) {
            Select-String -Path $zombieLog -Pattern 'LogMadFallGameplay' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'zombie-acceptance'
        }
    }

    # A sealed base: a 13 x 13 hollow concrete shell around the survivor, horde
    # zombies outside. No path reaches the survivor, so they must break through
    # (MadFall::Pathfinding::FindBreachTarget) - before that, they stood at the
    # walls re-planning and never got in.
    $breachSpawns = ('10 0', '-10 0', '0 10', '0 -10', '10 6', '-10 -6', '6 10', '-6 -10', '9 9', '-9 9', '9 -9', '-9 -9' |
        ForEach-Object { "mad.ai.spawn madfall:zombie_civilian $_ 1" }) -join '; '
    $breachScript = "mad.ai.Sleepers 0; mad.scene.anchor; mad.scene.pad 14; wait 3; mad.scene.box madfall:concrete_frame -6 -6 -1 6 6 4 hollow; wait 2; $breachSpawns; wait 100; mad.ai.status; quit"
    $breachLog = Join-Path $LogDir 'horde-breach.log'
    $breachProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow -RedirectStandardOutput $breachLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes', '-MadWorld=CIBreach',
                        "-ExecCmds=`"mad.onspawn $breachScript`"")
    if (-not $breachProcess.WaitForExit(240000)) {
        $breachProcess | Stop-Process -Force
        Write-Host 'FAILED: the sealed-base session did not finish within 240 s.' -ForegroundColor Red
        $script:Failures += 'horde-breach'
    }
    else {
        $breach = Select-String -Path $breachLog -Pattern 'lifetime: \d+ paths, (\d+) block hits, (\d+) player hits, \d+ kills, \d+ undermines, (\d+) breaches' | Select-Object -Last 1
        $breachOk = $false
        if ($null -ne $breach) {
            $groups = $breach.Matches[0].Groups
            $breachOk = [int]$groups[3].Value -gt 0 -and [int]$groups[2].Value -gt 0
            Write-Host "$(if ($breachOk) { 'OK' } else { 'FAILED' }): horde zombies broke into a sealed concrete base ($($groups[3].Value) breaches, $($groups[1].Value) block hits) and reached the survivor ($($groups[2].Value) hits)" `
                -ForegroundColor $(if ($breachOk) { 'Green' } else { 'Red' })
        }
        else {
            Write-Host 'FAILED: no mad.ai.status report from the sealed-base session' -ForegroundColor Red
        }
        if (-not $breachOk) {
            $script:Failures += 'horde-breach'
        }
    }
}

# ---------------------------------------------------------------------------
# Title screen, new world, pause, save and quit, load
# ---------------------------------------------------------------------------
#
# The menus' buttons call UMadMenuSubsystem functions, and mad.menu.* calls the
# same functions, so this walks the exact path a player clicks: start at the
# title (forced with -MadTitle; -unattended alone starts in a world), create a
# seeded world, pause, save and quit to the title, and load the world again -
# checking the seed survived in world.json and the title never saved anything.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: title screen and worlds (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $menuScript = @(
        'wait 5'
        'mad.menu.status'
        'mad.menu.newworld CIMenu 4242 hard'
        'wait 10'
        'mad.menu.status'
        'mad.menu.pause'
        'wait 1'
        'mad.menu.status'
        'mad.menu.resume'
        'mad.menu.title'
        'wait 5'
        'mad.menu.status'
        'mad.menu.worlds'
        'mad.menu.load CIMenu'
        'wait 10'
        'mad.menu.status'
        'mad.world.backups CIMenu'
        'mad.world.restore CIMenu'
        'quit'
    ) -join '; '

    $menuLog = Join-Path $LogDir 'menu-acceptance.log'
    $menuProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $menuLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-MadTitle', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $menuScript`"")

    if (-not $menuProcess.WaitForExit(240000)) {
        $menuProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the menu script within 240 s.' -ForegroundColor Red
        $script:Failures += 'menu-acceptance'
    }
    else {
        $titleStatus = @(Select-String -Path $menuLog -Pattern 'Menu: title, page 1, paused no, world _Title').Count
        $gameStatus = @(Select-String -Path $menuLog -Pattern 'Menu: game, page 0, paused no, world CIMenu \(seed 4242\)').Count
        $worldJson = Join-Path $GameWorlds 'CIMenu\world.json'
        $checks = @(
            @{ Ok = $titleStatus -ge 2; Why = "the game started at the title screen and came back to it ($titleStatus title statuses)" },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'Creating world CIMenu \(CIMenu\), seed 4242, hard' -Quiet); Why = 'New World created a seeded, hard world' },
            @{ Ok = $gameStatus -ge 2; Why = "the survivor played it, and again after loading it from the title ($gameStatus game statuses)" },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'Menu: game, page 5, paused yes' -Quiet); Why = 'the pause menu paused the game' },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'Saved; returning to the title screen' -Quiet); Why = 'save and quit to title saved first' },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'CIMenu \(CIMenu\): seed 4242, day \d+' -Quiet); Why = 'the world list shows the world with its seed' },
            @{ Ok = (Test-Path $worldJson) -and ((Get-Content $worldJson -Raw) -match '"seed"\s*:\s*"4242"'); Why = 'world.json recorded the seed' },
            @{ Ok = (Test-Path $worldJson) -and ((Get-Content $worldJson -Raw) -match '"difficulty"\s*:\s*"hard"'); Why = 'and the difficulty' },
            @{ Ok = -not (Test-Path (Join-Path $GameWorlds '_Title\world.json')); Why = 'the title backdrop world was never saved' },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'World backup [0-9_-]+ of CIMenu made in' -Quiet); Why = 'loading the saved world backed it up first' },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'CIMenu: 1 backup' -Quiet); Why = 'and the backup is listed' },
            @{ Ok = [bool](Select-String -Path $menuLog -Pattern 'Restore failed: cannot restore the world being played' -Quiet); Why = 'restoring the world being played is refused' }
        )

        $menuOk = $true
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $menuOk = $false
            }
        }

        if (-not $menuOk) {
            Select-String -Path $menuLog -Pattern 'LogMadFallGameplay|LogMadFallVoxel' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'menu-acceptance'
        }
    }
}

# ---------------------------------------------------------------------------
# Base building: doors, water, a ladder and a bed
# ---------------------------------------------------------------------------
#
# The survival verbs beyond mining and placing: a fur coat is worn (and kept
# through death), a two-high door opens and closes as one, an empty bottle fills at water, murky water boils at a campfire, a
# drink gives its bottle back, a ladder climbs a wall, and a bed becomes where
# the survivor wakes up. Geometry is built with mad.voxel.set around the fixed
# spawn at (0, 0, 22), then left three seconds for its collision to cook.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: base building (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $baseScript = @(
        'mad.scene.anchor'
        'mad.scene.pad 14'
        # Collision for the new ground cooks a few frames behind it: spawning or
        # interacting in the same frame fails on ground that is not there yet.
        'wait 2'
        'mad.player.give madfall:fur_coat 1'
        'mad.player.hold madfall:fur_coat'
        'mad.player.place'
        'mad.player.status'
        'mad.player.give madfall:wood_door 2'
        'mad.player.hold madfall:wood_door'
        'mad.player.aimrel -3 -3 -1'
        'mad.player.place'
        'mad.player.aimrel -3 -3 0'
        'mad.player.place'
        'mad.player.aimrel -3 -3 0'
        'mad.player.interact'
        'mad.player.interact'
        'mad.scene.set 0 2 1 madfall:water'
        'mad.player.give madfall:empty_bottle 1'
        'mad.player.hold madfall:empty_bottle'
        'mad.scene.aim 0 2 1'
        'mad.player.place'
        'mad.scene.set -1 -2 0 madfall:campfire'
        'wait 1'
        'mad.player.craft madfall:water_bottle_boiled'
        'wait 8'
        'mad.player.hold madfall:water_bottle'
        'mad.player.place'
        'mad.player.status'
        'mad.scene.set 3 -6 0 madfall:stone'
        'mad.scene.set 3 -6 1 madfall:stone'
        'mad.scene.set 3 -6 2 madfall:stone'
        'mad.scene.set 3 -6 3 madfall:stone'
        'mad.scene.set 3 -6 4 madfall:stone'
        'mad.scene.set 3 -6 5 madfall:stone'
        'mad.scene.set 2 -6 0 madfall:ladder'
        'mad.scene.set 2 -6 1 madfall:ladder'
        'mad.scene.set 2 -6 2 madfall:ladder'
        'mad.scene.set 2 -6 3 madfall:ladder'
        'mad.scene.set 2 -6 4 madfall:ladder'
        'wait 3'
        'mad.scene.tp 2 -6 1'
        'wait 1'
        'mad.player.walk 2 1 0'
        'wait 2'
        'mad.player.status'
        'mad.scene.set -4 5 0 madfall:bedroll'
        'mad.scene.tp -4 3 1'
        'wait 1'
        'mad.scene.aim -4 5 0'
        'mad.player.interact'
        'mad.player.damage 1000'
        'wait 6'
        'mad.player.status'
        'quit'
    ) -join '; '

    $baseLog = Join-Path $LogDir 'base-acceptance.log'
    $baseProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $baseLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $baseScript`"")

    if (-not $baseProcess.WaitForExit(240000)) {
        $baseProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the base-building script within 240 s.' -ForegroundColor Red
        $script:Failures += 'base-acceptance'
    }
    else {
        $climbZ = @(Select-String -Path $baseLog -Pattern 'Player at voxel X=-?\d+ Y=-6 Z=(\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
        $wornLines = @(Select-String -Path $baseLog -Pattern 'worn: armor 10%  cold \+12 C')
        $checks = @(
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Toggled 2 x madfall:wood_door to madfall:wood_door_open' -Quiet); Why = 'a two-high door opened as one' },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Toggled 2 x madfall:wood_door_open to madfall:wood_door' -Quiet); Why = 'and closed again' },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Filled madfall:murky_water from water' -Quiet); Why = 'an empty bottle filled at water' },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Craft finished: 1 x madfall:water_bottle' -Quiet); Why = 'murky water boiled at the campfire' },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'madfall:empty_bottle x1' -Quiet); Why = 'drinking gave the bottle back' },
            @{ Ok = $climbZ.Count -ge 1 -and $climbZ[-1] -ge 27; Why = "the ladder climbed the wall (feet Z $($climbZ -join ', '))" },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Respawn point set at X=-4 Y=5 Z=22' -Quiet); Why = 'the bedroll became the respawn point' },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Player respawned at X=-4 Y=5 Z=23 \(bed\)' -Quiet); Why = 'and the survivor woke up on it' },
            @{ Ok = [bool](Select-String -Path $baseLog -Pattern 'Wearing madfall:fur_coat on the body' -Quiet); Why = 'a fur coat was put on' },
            @{ Ok = $wornLines.Count -ge 4; Why = "it gave warmth and armour, and was still worn after dying ($($wornLines.Count) status reports)" }
        )

        $baseOk = $true
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $baseOk = $false
            }
        }
        if (-not $baseOk) {
            Select-String -Path $baseLog -Pattern 'LogMadFallGameplay' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'base-acceptance'
        }
    }
}

# ---------------------------------------------------------------------------
# Farming: till, plant, grow, harvest
# ---------------------------------------------------------------------------
#
# A hoe tills grass into farmland, a corn seed planted on it is tracked, a seed
# aimed at stone is refused, the plant survives a save, fast-forwarding 30 hours
# grows it through both 12-hour stages, and harvesting the ripe stalk gives corn
# and seeds back. The plot is cleared with mad.voxel.set two voxels in front of
# the spawn so the aim ray has a known path.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: farming (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $farmScript = @(
        'mad.scene.anchor'
        'mad.scene.pad 12'
        # Collision for the new ground cooks a few frames behind it: spawning or
        # interacting in the same frame fails on ground that is not there yet.
        'wait 2'
        'mad.scene.set 2 0 -1 madfall:grass'
        'mad.scene.set 2 -1 -1 madfall:stone'
        'mad.scene.set 1 0 0 madfall:air'
        'mad.scene.set 2 0 0 madfall:air'
        'mad.scene.set 1 0 1 madfall:air'
        'mad.scene.set 2 0 1 madfall:air'
        'mad.scene.set 1 -1 0 madfall:air'
        'mad.scene.set 2 -1 0 madfall:air'
        'mad.scene.set 1 -1 1 madfall:air'
        'mad.scene.set 2 -1 1 madfall:air'
        'wait 2'
        'mad.scene.tp 0 0 0'
        'wait 1'
        'mad.player.give madfall:stone_hoe 1'
        'mad.player.hold madfall:stone_hoe'
        'mad.scene.aim 2 0 -1'
        'mad.player.place'
        'mad.player.give madfall:corn_seed 3'
        'mad.player.hold madfall:corn_seed'
        'mad.scene.aim 2 0 -1'
        'mad.player.place'
        'mad.scene.aim 2 -1 -1'
        'mad.player.place'
        'mad.farm.status'
        'mad.save'
        'mad.farm.advance 30'
        'wait 3'
        'mad.farm.status'
        'mad.voxel.get 2 0 22'
        'mad.player.hold madfall:stone_hoe'
        'mad.scene.aim 2 0 0'
        'mad.player.use 3'
        'mad.player.status'
        'mad.clock.set 15'
        'mad.weather.set storm'
        # Thunder follows its flash at the speed of sound, up to about 2.6 s for a strike 900 m off.
        'wait 12'
        'mad.weather.status'
        'mad.player.status'
        'mad.audio.stats'
        'quit'
    ) -join '; '

    $farmLog = Join-Path $LogDir 'farm-acceptance.log'
    $farmProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $farmLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $farmScript`"")

    if (-not $farmProcess.WaitForExit(240000)) {
        $farmProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the farming script within 240 s.' -ForegroundColor Red
        $script:Failures += 'farm-acceptance'
    }
    else {
        $checks = @(
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'Tilled madfall:grass into madfall:farmland at X=2 Y=0 Z=21' -Quiet); Why = 'the hoe tilled grass into farmland' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'Farming: 1 plant\(s\), 0 due, next stage in 12\.0 h' -Quiet); Why = 'one seed planted on farmland, the one aimed at stone refused' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'Gameplay saved to .*1 plant\(s\)' -Quiet); Why = 'the growing plant was saved' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern '\(2, 0, 22\) = madfall:corn_plant' -Quiet); Why = '30 hours grew it through both stages' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'madfall:corn x[2-3]' -Quiet); Why = 'harvesting gave corn' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'madfall:corn_seed x[3-4]' -Quiet); Why = 'and seeds back' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'Weather: storm \(forced\).*-6\.0 C.*thunder [1-9]' -Quiet); Why = 'a forced storm blew in, cold, with thunder' },
            @{ Ok = [bool](Select-String -Path $farmLog -Pattern 'Audio: .*rain_loop=[1-9].*wind_loop=[1-9].*thunder=[1-9]' -Quiet); Why = 'and sounded like one' }
        )

        $farmOk = $true
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $farmOk = $false
            }
        }
        if (-not $farmOk) {
            Select-String -Path $farmLog -Pattern 'LogMadFall' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'farm-acceptance'
        }
    }
}

# ---------------------------------------------------------------------------
# HUD layout: the same HUD in a small window and a large one
# ---------------------------------------------------------------------------
#
# The panels are placed by FMadHudLayout and swept across window sizes by
# MadFall.UI.Layout. This gate runs the real HUD in the real game at two window
# sizes and at a doubled HUD size, and asks it (mad.hud.layout) whether what it
# just drew is valid: the unit test proves the arithmetic, this proves the HUD
# is actually using it.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: HUD layout (headless -game)'

    $hudSizes = @(
        @{ W = 854; H = 480; Scale = 1.0; Why = 'a small window' },
        @{ W = 1280; H = 720; Scale = 2.0; Why = 'a doubled HUD, backed off to fit' }
    )
    $hudOk = $true
    foreach ($size in $hudSizes) {
        $hudScript = @(
            "mad.ui.Scale $($size.Scale)"
            'mad.player.give madfall:wood_plank 40'
            'mad.player.craft madfall:ladder 3'
            'wait 1'
            'mad.hud.layout'
            'mad.player.openinventory'
            'wait 1'
            'mad.hud.layout'
            'mad.hud.click inv.b.9'
            'mad.player.closeinventory'
            'quit'
        ) -join '; '
        $hudLog = Join-Path $LogDir "hud-$($size.W)x$($size.H).log"
        $hudProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
            -RedirectStandardOutput $hudLog `
            -ArgumentList @("`"$ProjectFile`"", '-game', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                            '-RenderOffScreen', "-ResX=$($size.W)", "-ResY=$($size.H)", '-windowed', '-MadDefaultSettings',
                            "-MadWorld=hud$($size.W)", "-ExecCmds=`"mad.onspawn $hudScript`"")

        if (-not $hudProcess.WaitForExit(240000)) {
            $hudProcess | Stop-Process -Force
            Write-Host "FAILED: the game did not finish the HUD script for $($size.Why) within 240 s." -ForegroundColor Red
            $hudOk = $false
            continue
        }

        $reports = @(Select-String -Path $hudLog -Pattern 'HUD layout: .*')
        if ($reports.Count -lt 2) {
            Write-Host "FAILED: $($size.Why) did not report its layout twice (got $($reports.Count))." -ForegroundColor Red
            $hudOk = $false
            continue
        }
        $invalid = @($reports | Where-Object { $_.Line -match 'INVALID' })
        if ($invalid.Count -gt 0) {
            Write-Host "FAILED: $($size.Why) drew an invalid layout: $($invalid[0].Line)" -ForegroundColor Red
            $hudOk = $false
            continue
        }
        Write-Host "OK: $($size.Why) placed every panel validly" -ForegroundColor Green

        # The HUD is sized to the window, not to a fixed number of pixels.
        if ($reports[0].Line -match 'HUD layout: (\d+)x(\d+), scale ([0-9.]+)') {
            $drawnW = [int]$Matches[1]
            $scale = [double]$Matches[3]
            $wanted = [Math]::Round([Math]::Min($drawnW / 1600.0, [int]$Matches[2] / 900.0), 2)
            if ($scale -le 0.0) {
                Write-Host "FAILED: $($size.Why) reported no scale." -ForegroundColor Red
                $hudOk = $false
            }
            elseif ($size.Scale -eq 1.0 -and [Math]::Abs($scale - [Math]::Max($wanted, 0.5)) -gt 0.03) {
                Write-Host "FAILED: $($size.Why) scaled to $scale, not the $wanted its window implies." -ForegroundColor Red
                $hudOk = $false
            }
            else {
                Write-Host "OK: $($size.Why) scaled the HUD to $scale" -ForegroundColor Green
            }
        }

        # The inventory screen's slots are still where a click finds them.
        if (Select-String -Path $hudLog -Pattern 'inventory|Holding|backpack' -Quiet) {
            Write-Host "OK: the inventory screen took a click at $($size.W)x$($size.H)" -ForegroundColor Green
        }
    }
    if (-not $hudOk) {
        $script:Failures += 'hud-layout'
    }
}

# ---------------------------------------------------------------------------
# Scripts: the Lua example mod in a real game
# ---------------------------------------------------------------------------
#
# Mods/example_scripted loads, passes its own sandbox self-test, builds a beacon
# through madfall.set_block, counts a block the survivor chops down (a real
# block_broken event), pays its bounty after 25 kill events, and hands out
# supplies at the real dawn after a real horde night. A second launch of the
# same world must find the tallies in the save.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: scripts (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $killEvents = @(1..25 | ForEach-Object { 'mad.scripts.event zombie_killed zombie=madfall:zombie_civilian count=1' })
    $scriptScript = (@(
        'mad.scene.anchor'
        'mad.scene.pad 12'
        # Collision for the new ground cooks a few frames behind it: spawning or
        # interacting in the same frame fails on ground that is not there yet.
        'wait 2'
        'wait 2'
        'mad.scene.set 1 0 0 madfall:air'
        'mad.scene.set 1 0 1 madfall:air'
        'mad.scene.set 1 0 2 madfall:air'
        'mad.scene.tp 0 0 0'
        'wait 1'
        'mad.scripts'
        'mod.example_scripted.selftest'
        'mod.example_scripted.beacon 4'
        'wait 1'
        'mad.voxel.get 2 0 26'
        'mad.player.give madfall:stone_axe 1'
        'mad.player.hold madfall:stone_axe'
        'mad.scene.aim 2 0 1'
        'mad.player.use 40'
        'wait 1'
    ) + $killEvents + @(
        'wait 1'
        'mad.clock.set 21.98 7'
        'wait 4'
        'mad.clock.set 5.98 8'
        'wait 4'
        'mod.example_scripted.stats'
        'mad.scripts'
        'mad.save'
        'quit'
    )) -join '; '

    $scriptLog = Join-Path $LogDir 'scripts-acceptance.log'
    $scriptProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $scriptLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $scriptScript`"")

    $reloadLog = Join-Path $LogDir 'scripts-reload.log'
    $scriptsOk = $true
    if (-not $scriptProcess.WaitForExit(240000)) {
        $scriptProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the scripts script within 240 s.' -ForegroundColor Red
        $scriptsOk = $false
    }
    else {
        $reloadProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
            -RedirectStandardOutput $reloadLog `
            -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                            "-ExecCmds=`"mad.onspawn wait 1; mod.example_scripted.stats; quit`"")
        if (-not $reloadProcess.WaitForExit(180000)) {
            $reloadProcess | Stop-Process -Force
            Write-Host 'FAILED: the reload did not finish within 180 s.' -ForegroundColor Red
            $scriptsOk = $false
        }
        else {
            $checks = @(
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'Script \[example_scripted\]: loaded scripts/main\.lua' -Quiet); Why = 'the example script mod loaded' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'Script \[example_scripted\]: selftest passed' -Quiet); Why = 'its sandbox self-test passed inside the game' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern '\(2, 0, 26\) = madfall:torch' -Quiet); Why = 'the beacon command built through madfall.set_block' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'Night Watch bounty: 25 kills' -Quiet); Why = 'the bounty was paid after 25 kill events' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'Script message \[example_scripted\]: Horde night' -Quiet); Why = 'the real horde dusk reached the script' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'Script message \[example_scripted\]: You held the line\. Supplies have arrived' -Quiet); Why = 'supplies at the real dawn after the horde night' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'Night Watch: 25 kills, 1 blocks mined, 1 nights, 1 sessions' -Quiet); Why = 'a block the survivor chopped down counted (block_broken)' },
                @{ Ok = [bool](Select-String -Path $scriptLog -Pattern 'example_scripted: .* 0 error\(s\)' -Quiet); Why = 'no script errors' },
                @{ Ok = -not [bool](Select-String -Path $scriptLog -Pattern 'Script error' -Quiet); Why = 'nothing reported as a script error' },
                @{ Ok = [bool](Select-String -Path $reloadLog -Pattern 'Night Watch: 25 kills, 1 blocks mined, 1 nights, 2 sessions' -Quiet); Why = 'the store came back from the save on the next launch' }
            )
            foreach ($check in $checks) {
                if ($check.Ok) {
                    Write-Host "OK: $($check.Why)" -ForegroundColor Green
                }
                else {
                    Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                    $scriptsOk = $false
                }
            }
        }
    }
    if (-not $scriptsOk) {
        Select-String -Path $scriptLog, $reloadLog -Pattern 'LogMadFall' -ErrorAction SilentlyContinue |
            Select-Object -Last 30 |
            ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
        $script:Failures += 'scripts-acceptance'
    }
}

# ---------------------------------------------------------------------------
# Inventory screen: skills, tooltips, installing a mod by drag and drop; keys
# ---------------------------------------------------------------------------
#
# Driven through mad.hud.click, the same AMadHUD::ClickBox a mouse click
# reaches: a Reinforced Grip picked up and put down on an Iron Pickaxe is
# installed (not swapped), the pickaxe's tooltip lists it, and a perk's Take
# button spends a level-up point. The crafting column filters to craftable
# tools and its Craft button queues the selected recipe. Rebinding a key another action uses swaps
# the two and remaps the survivor; Escape cannot be rebound.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: inventory screen (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $uiScript = @(
        'wait 1'
        'mad.player.give madfall:iron_pickaxe 1'
        'mad.player.give madfall:mod_reinforced_grip 2'
        'mad.player.xp 350'
        'mad.player.openinventory'
        'mad.hud.click inv.b.5'
        'mad.hud.click inv.b.4'
        'mad.player.tooltip b 4'
        'mad.player.tooltip b 5'
        'mad.hud.click inv.b.4 ctrl'
        'mad.player.slots'
        'mad.hud.click inv.b.4'
        'mad.hud.click inv.b.30'
        'mad.hud.click inv.b.1'
        'mad.player.invheld -1'
        'mad.hud.click inv.b.25'
        'mad.hud.click inv.sort.b'
        'mad.player.slots'
        'mad.hud.click perk.madfall:tough'
        'mad.hud.click perk.madfall:miner'
        'mad.hud.click perk.madfall:artisan'
        'mad.perks'
        'mad.hud.click tab.crafting'
        'mad.hud.click craft.cat.1'
        'mad.hud.click craft.only'
        'mad.hud.click craft.row.madfall:stone_axe'
        'mad.player.recipes'
        'mad.hud.click craft.make.1'
        'mad.input.bind jump J'
        'mad.input.bind interact J'
        'mad.input.bind jump Escape'
        'mad.input.bindings'
        'mad.input.reset'
        'quit'
    ) -join '; '

    $uiLog = Join-Path $LogDir 'inventory-screen.log'
    $uiProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $uiLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $uiScript`"")

    if (-not $uiProcess.WaitForExit(180000)) {
        $uiProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the inventory screen script within 180 s.' -ForegroundColor Red
        $script:Failures += 'inventory-screen'
    }
    else {
        $checks = @(
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Install madfall:mod_reinforced_grip on madfall:iron_pickaxe: ok' -Quiet); Why = 'a grip put down on the pickaxe was installed' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Tooltip: Iron Pickaxe \| .*Mods 1 / 2: Reinforced Grip' -Quiet); Why = 'the pickaxe tooltip lists the installed mod' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Tooltip: Reinforced Grip \| Item mod: durability \+50%' -Quiet); Why = 'one grip left, and its tooltip explains it' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Remove mod madfall:mod_reinforced_grip from madfall:iron_pickaxe: ok' -Quiet); Why = 'ctrl-click took the grip back off the pickaxe' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Slots: .* 4=madfall:iron_pickaxe x1 5=madfall:mod_reinforced_grip x2' -Quiet); Why = 'the pickaxe has no mod and both grips are in the backpack' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Holding 3\.' -Quiet); Why = 'the wheel took one fewer of the 4 stones' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Sorted the backpack\.' -Quiet); Why = 'the Sort button sorted the backpack' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Slots: .* 1=madfall:rock x1 .*9=madfall:iron_pickaxe x1 10=madfall:rock x3' -Quiet); Why = 'exactly 3 stones moved, and the sort packed the pickaxe then the stones from the first backpack slot' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Perk madfall:tough: ok' -Quiet); Why = 'a Take button bought a perk' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Perk madfall:artisan: no perk points' -Quiet); Why = 'and the third purchase at level 3 was refused' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'madfall:tough +1/3' -Quiet); Why = 'the rank shows in mad.perks' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Recipes: Tools, craftable only, [1-9]\d* row\(s\), selected madfall:stone_axe' -Quiet); Why = 'the crafting column filtered to craftable tools and selected the stone axe' },
            @{ Ok = -not [bool](Select-String -Path $uiLog -Pattern '^\s+- madfall:' -Quiet); Why = 'and listed nothing uncraftable' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Craft queued: 1 x madfall:stone_axe' -Quiet); Why = 'Craft 1 queued the axe' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Input: keys mapped, .*interact=J.*jump=E|Input: keys mapped, .*jump=E.*interact=J' -Quiet); Why = 'binding interact to J swapped jump onto E and remapped the survivor' },
            @{ Ok = [bool](Select-String -Path $uiLog -Pattern 'Bind jump Escape: that key is reserved' -Quiet); Why = 'Escape cannot be rebound' },
            @{ Ok = [bool]((Select-String -Path $uiLog -Pattern 'Input: keys mapped' | Select-Object -Last 1).Line -match '\(defaults\)'); Why = 'reset put every key back (the last remap is to defaults)' }
        )
        $uiOk = $true
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $uiOk = $false
            }
        }
        if (-not $uiOk) {
            Select-String -Path $uiLog -Pattern 'LogMadFall' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'inventory-screen'
        }
    }
}

# ---------------------------------------------------------------------------
# Traders: the outpost near spawn, buying, selling, shelves saved
# ---------------------------------------------------------------------------
#
# Every world places one trader outpost in the rings of cells around the spawn.
# The survivor travels in, faces Rosa, opens the trade screen with Interact,
# buys through the shelf's hit box, sells a stack with shift-click, and saves;
# a second launch must find the same shelves with the same restock day.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: traders (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $tradeScript = @(
        'wait 1'
        'mad.trader.status'
        'mad.player.compass'
        'mad.trader.goto'
        'wait 4'
        'mad.trader.aim'
        'wait 0.5'
        'mad.player.give madfall:coin 100'
        'mad.player.interact'
        'mad.hud.click trade.b.0'
        'mad.hud.click inv.b.1 shift'
        'mad.trader.status'
        'mad.save'
        'quit'
    ) -join '; '

    $tradeLog = Join-Path $LogDir 'traders.log'
    $tradeReload = Join-Path $LogDir 'traders-reload.log'
    $tradeOk = $true
    $tradeProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $tradeLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $tradeScript`"")
    if (-not $tradeProcess.WaitForExit(240000)) {
        $tradeProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the traders script within 240 s.' -ForegroundColor Red
        $tradeOk = $false
    }
    else {
        $reloadProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
            -RedirectStandardOutput $tradeReload `
            -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                            "-ExecCmds=`"mad.onspawn wait 1; mad.trader.status; quit`"")
        if (-not $reloadProcess.WaitForExit(180000)) {
            $reloadProcess | Stop-Process -Force
            Write-Host 'FAILED: the traders reload did not finish within 180 s.' -ForegroundColor Red
            $tradeOk = $false
        }
        else {
            $restockLine = Select-String -Path $tradeLog -Pattern 'madfall:quartermaster at .*restocks day (\d+)' | Select-Object -Last 1
            $checks = @(
                @{ Ok = [bool](Select-String -Path $tradeLog -Pattern 'outpost trader at X=' -Quiet); Why = 'the world has a trader outpost near the spawn' },
                @{ Ok = [bool](Select-String -Path $tradeLog -Pattern 'Display:\s+Trader: bearing' -Quiet); Why = 'the compass points to it' },
                @{ Ok = [bool](Select-String -Path $tradeLog -Pattern 'Trader madfall:quartermaster is at' -Quiet); Why = 'Rosa stands at her marker when the survivor arrives' },
                @{ Ok = [bool](Select-String -Path $tradeLog -Pattern 'Trade opened with madfall:quartermaster' -Quiet); Why = 'Interact on Rosa opened the trade screen' },
                @{ Ok = [bool](Select-String -Path $tradeLog -Pattern 'Trade: buy madfall:\S+ from slot 0: ok \(paid [1-9]' -Quiet); Why = 'a shelf click bought an item' },
                @{ Ok = [bool](Select-String -Path $tradeLog -Pattern 'Trade: sell madfall:\S+ from slot 1: ok \(earned [1-9]' -Quiet); Why = 'shift-click sold a stack' },
                @{ Ok = ($null -ne $restockLine) -and [bool](Select-String -Path $tradeReload -Pattern ("madfall:quartermaster at .*restocks day " + $restockLine.Matches[0].Groups[1].Value) -Quiet); Why = 'the shelves and their restock day came back from the save' }
            )
            foreach ($check in $checks) {
                if ($check.Ok) {
                    Write-Host "OK: $($check.Why)" -ForegroundColor Green
                }
                else {
                    Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                    $tradeOk = $false
                }
            }
        }
    }
    if (-not $tradeOk) {
        Select-String -Path $tradeLog, $tradeReload -Pattern 'LogMadFall' -ErrorAction SilentlyContinue |
            Select-Object -Last 30 |
            ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
        $script:Failures += 'traders'
    }
}

# ---------------------------------------------------------------------------
# Zombie variety: a spitter's acid and a screamer's call
# ---------------------------------------------------------------------------
#
# A spitter ten voxels away hits the survivor from range (its spit is a
# projectile, through the same code as arrows); a screamer that sees the
# survivor alerts the zombies around it and calls more from out of sight.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: zombie variety (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $varietyScript = @(
        'mad.scene.anchor'
        'mad.scene.pad 12'
        # Collision for the new ground cooks a few frames behind it: spawning or
        # interacting in the same frame fails on ground that is not there yet.
        'wait 2'
        'mad.animals.SpawnSeconds 0'
        'mad.ai.Sleepers 0'
        'mad.clock.set 12'
        'mad.ai.spawn madfall:zombie_spitter 10 0'
        'wait 10'
        'mad.ai.status'
        'mad.player.status'
        'mad.ai.killall'
        'mad.ai.spawn madfall:zombie_civilian 20 20'
        'mad.ai.spawn madfall:zombie_screamer 8 0'
        'wait 6'
        'mad.ai.status'
        'mad.audio.stats'
        'mad.ai.killall'
        'mad.scene.set 3 -1 0 madfall:wood_spikes'
        'mad.scene.set 3 0 0 madfall:wood_spikes'
        'mad.scene.set 3 1 0 madfall:wood_spikes'
        'mad.scene.set 4 -1 0 madfall:wood_spikes'
        'mad.scene.set 4 0 0 madfall:wood_spikes'
        'mad.scene.set 4 1 0 madfall:wood_spikes'
        'wait 2'
        'mad.ai.spawn madfall:zombie_civilian 9 0'
        'wait 12'
        'mad.ai.status'
        'mad.ai.killall'
        'mad.player.pillar 5 madfall:rebar_concrete'
        'wait 2'
        'mad.player.status'
        'mad.ai.spawn madfall:zombie_climber 5 0'
        'wait 15'
        'mad.ai.status'
        'mad.player.status'
        'quit'
    ) -join '; '

    $varietyLog = Join-Path $LogDir 'zombie-variety.log'
    $varietyProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow -RedirectStandardOutput $varietyLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $varietyScript`"")

    if (-not $varietyProcess.WaitForExit(180000)) {
        $varietyProcess | Stop-Process -Force
        Write-Host 'FAILED: the zombie variety script did not finish within 180 s.' -ForegroundColor Red
        $script:Failures += 'zombie-variety'
    }
    else {
        $healthLine = Select-String -Path $varietyLog -Pattern '^\s+health ([0-9.]+)/' | Select-Object -First 1
        # The player-hit count before the climber (the spikes status) and after it.
        $hitLines = @(Select-String -Path $varietyLog -Pattern 'lifetime: \d+ paths, \d+ block hits, (\d+) player hits')
        $pillarHits = if ($hitLines.Count -ge 2) { [int]$hitLines[$hitLines.Count - 2].Matches[0].Groups[1].Value } else { 0 }
        $climbHits = if ($hitLines.Count -ge 1) { [int]$hitLines[$hitLines.Count - 1].Matches[0].Groups[1].Value } else { 0 }
        $health = if ($healthLine) { [double]$healthLine.Matches[0].Groups[1].Value } else { 100.0 }
        $checks = @(
            @{ Ok = [bool](Select-String -Path $varietyLog -Pattern 'Spit hit MadPlayerCharacter' -Quiet); Why = 'a spitter hit the survivor from ten voxels' },
            @{ Ok = $health -lt 95.0; Why = "and hurt them (health $health)" },
            @{ Ok = [bool](Select-String -Path $varietyLog -Pattern 'zombie_screamer screams: [1-9]\d* zombie\(s\) alerted, [1-9]\d* called' -Quiet); Why = 'a screamer alerted the zombies near it and called more' },
            @{ Ok = [bool](Select-String -Path $varietyLog -Pattern 'Audio: .*zombie_scream=[1-9].*zombie_spit=[1-9]' -Quiet); Why = 'and both were heard' },
            @{ Ok = [bool](Select-String -Path $varietyLog -Pattern 'lifetime: .* [1-9]\d* trap hits' -Quiet); Why = 'a zombie walked into wooden spikes on its way to the survivor, and was hurt' },
            @{ Ok = [bool](Select-String -Path $varietyLog -Pattern 'lifetime: .* trap hits, ([4-9]|[1-9]\d+) climbed' -Quiet); Why = 'a climber scaled the concrete pillar the survivor stood on' },
            @{ Ok = $climbHits -gt $pillarHits; Why = "and hit them up there ($pillarHits -> $climbHits player hits)" }
        )
        $varietyOk = $true
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $varietyOk = $false
            }
        }
        if (-not $varietyOk) {
            Select-String -Path $varietyLog -Pattern 'LogMadFallGameplay' | Select-Object -Last 30 | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'zombie-variety'
        }
    }
}

# ---------------------------------------------------------------------------
# Quests: the tutorial starts, completes on a craft, pays out, and persists
# ---------------------------------------------------------------------------
#
# A fresh survivor has exactly the first tutorial quest. Crafting a stone axe
# completes it, pays its reward (4 rocks: 4 - 2 for the axe + 4 = 6) and starts
# the two quests it unlocks. A second launch of the same world restores that.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: quests (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $questLog = Join-Path $LogDir 'quest-acceptance.log'
    $questReloadLog = Join-Path $LogDir 'quest-reload.log'
    $questRuns = @(
        @{ Log = $questLog; Script = (@('mad.animals.SpawnSeconds 0', 'mad.quests', 'mad.player.craft madfall:stone_axe', 'wait 5', 'mad.quests', 'mad.player.status', 'mad.map.toggle', 'mad.player.walk 6 1 0', 'wait 7', 'mad.map.status', 'quit') -join '; ') },
        @{ Log = $questReloadLog; Script = (@('mad.quests', 'mad.map.status', 'quit') -join '; ') }
    )
    $questOk = $true
    foreach ($run in $questRuns) {
        $process = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow -RedirectStandardOutput $run.Log `
            -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                            '-MadWorld=CIQuests', "-ExecCmds=`"mad.onspawn $($run.Script)`"")
        if (-not $process.WaitForExit(180000)) {
            $process | Stop-Process -Force
            Write-Host "FAILED: a quest run did not finish within 180 s." -ForegroundColor Red
            $questOk = $false
        }
    }

    if ($questOk) {
        $checks = @(
            @{ Ok = [bool](Select-String -Path $questLog -Pattern 'Quests: 1 active, 0 complete' -Quiet); Why = 'a new survivor starts with one quest' },
            @{ Ok = [bool](Select-String -Path $questLog -Pattern 'Quest complete: madfall:quest/first_tool' -Quiet); Why = 'crafting a stone axe completed it' },
            @{ Ok = [bool](Select-String -Path $questLog -Pattern 'Quest started: madfall:quest/timber' -Quiet); Why = 'and unlocked the next' },
            @{ Ok = [bool](Select-String -Path $questLog -Pattern 'madfall:rock x6' -Quiet); Why = 'its reward was paid' },
            @{ Ok = [bool](Select-String -Path $questReloadLog -Pattern 'Quests: 2 active, 1 complete' -Quiet); Why = 'the journal came back in the next session' },
            @{ Ok = [bool](Select-String -Path $questLog -Pattern 'Map: open, zoom 1 .*[1-9]\d+ cell\(s\) explored, [1-9]\d* draw\(s\)' -Quiet); Why = 'the map opened, drew, and grew as the survivor walked' },
            @{ Ok = [bool](Select-String -Path $questReloadLog -Pattern 'Map: closed, .*[1-9]\d+ cell\(s\) explored' -Quiet); Why = 'and what was explored was saved' }
        )
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $questOk = $false
            }
        }
    }
    if (-not $questOk) {
        Select-String -Path $questLog, $questReloadLog -Pattern 'LogMadFallGameplay' |
            Select-Object -Last 30 |
            ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
        $script:Failures += 'quest-acceptance'
    }
}

# ---------------------------------------------------------------------------
# Animals: hunt, get gored, butcher; a deer bolts; herds appear by biome
# ---------------------------------------------------------------------------
#
# A stag two voxels ahead is struck with a club (through the same swing trace
# that hits zombies), charges and gores the survivor, is killed and drops meat
# and hide that the survivor picks up. A deer spawned in plain sight six voxels
# away runs. A second stag nine voxels off takes an arrow from a bow. Natural spawning is then switched on and must place a herd of some
# species in the biome around the spawn.

if (-not $SkipTests) {
    Write-Section 'ACCEPTANCE: animals (headless -game)'

    $GameWorlds = Join-Path $RepoRoot 'Saved\MadFallWorlds'
    if (Test-Path $GameWorlds) {
        Remove-Item -Recurse -Force $GameWorlds
    }

    $animalScript = @(
        'mad.scene.anchor'
        'mad.scene.pad 14'
        # Collision for the new ground cooks a few frames behind it: spawning or
        # interacting in the same frame fails on ground that is not there yet.
        'wait 2'
        'mad.animals.SpawnSeconds 0'
        # Still: on the scene's flat pad a stag wanders out of arm's reach between
        # swings, where it used to fetch up against the natural ground.
        'mad.animals.Stroll 0'
        'mad.animals.spawn madfall:stag 1 2'
        'wait 3'
        'mad.player.give madfall:wooden_club 1'
        'mad.player.hold madfall:wooden_club'
        'mad.player.aimanimal'
        'mad.player.use 1'
        'wait 3'
        'mad.animals.status'
        # A grazing stag drifts a voxel or two between swings and a club only
        # reaches so far, so keep swinging: the check is that it dies, not that
        # every swing lands.
        $(1..10 | ForEach-Object { 'mad.player.aimanimal'; 'mad.player.use 1'; 'wait 1' })
        'mad.animals.status'
        'mad.player.status'
        'mad.animals.spawn madfall:deer 1 6'
        # Straight away: a startled deer bolts, then settles to graze once it is
        # far enough off, and a status three seconds later has missed the bolt.
        'wait 1'
        'mad.animals.status'
        'wait 3'
        'mad.animals.status'
        'mad.animals.killall'
        # Standing still: a stroll between aiming and the arrow's arrival was a miss.
        'mad.animals.Stroll 0'
        'mad.animals.spawn madfall:stag 1 9'
        'wait 3'
        'mad.player.give madfall:wooden_bow 1'
        'mad.player.give madfall:arrow 10'
        'mad.player.hold madfall:wooden_bow'
        'mad.player.aimanimal'
        'mad.player.use 1'
        'wait 2'
        'mad.animals.status'
        'mad.player.status'
        'mad.animals.SpawnSeconds 1'
        'wait 8'
        'mad.animals.status'
        'quit'
    ) -join '; '

    $animalLog = Join-Path $LogDir 'animal-acceptance.log'
    $animalProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow `
        -RedirectStandardOutput $animalLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes',
                        "-ExecCmds=`"mad.onspawn $animalScript`"")

    if (-not $animalProcess.WaitForExit(240000)) {
        $animalProcess | Stop-Process -Force
        Write-Host 'FAILED: the game did not finish the animal script within 240 s.' -ForegroundColor Red
        $script:Failures += 'animal-acceptance'
    }
    else {
        $checks = @(
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'madfall:stag at .*: (chase|attack)' -Quiet); Why = 'a struck stag turned on the survivor' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'Animals: .* [1-9]\d* hit\(s\) on the survivor' -Quiet); Why = 'and gored them' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'Animal madfall:stag died' -Quiet); Why = 'the club killed it' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'madfall:raw_meat x[4-6]' -Quiet); Why = 'its meat was picked up' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'madfall:animal_hide x[1-2]' -Quiet); Why = 'and its hide' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'madfall:deer at .*: flee' -Quiet); Why = 'a deer in plain sight bolted' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'Arrow hit MadAnimal\w* for 40' -Quiet); Why = 'an arrow hit a stag nine voxels away' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'madfall:arrow x9' -Quiet); Why = 'and used up one arrow' },
            @{ Ok = [bool](Select-String -Path $animalLog -Pattern 'Spawned \d+ madfall:\w+ in madfall:\w+ at' -Quiet); Why = 'a herd spawned naturally by biome' }
        )

        $animalOk = $true
        foreach ($check in $checks) {
            if ($check.Ok) {
                Write-Host "OK: $($check.Why)" -ForegroundColor Green
            }
            else {
                Write-Host "FAILED: $($check.Why)" -ForegroundColor Red
                $animalOk = $false
            }
        }
        if (-not $animalOk) {
            Select-String -Path $animalLog -Pattern 'LogMadFall|  madfall:' |
                Select-Object -Last 30 |
                ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'animal-acceptance'
        }
    }
}

# ---------------------------------------------------------------------------
# Gate 12: frame budget
# ---------------------------------------------------------------------------
#
# The engineering standard is no game-thread stall over 2 ms. Every MadFall
# system's tick is charged to mad.perf's per-frame accounting, so this measures
# the frame, not one system. Rendered (render-state creation is part of the
# cost), after the initial load, three scenarios in one session: a zombie horde
# around the player, streaming at faster than a sprint (16 voxels every 1.5 s),
# and an 81-block concrete slab collapsing. The dev box measured 7 over-budget
# frames of 3,537 (0.2%), worst 2.6 ms. The gate allows 0.5% and fails on any
# frame over 5 ms - a real stall, not noise.

if ($SkipTests) {
    Write-Section 'FRAME BUDGET (skipped)'
    $script:Skipped += 'frame-budget'
}
else {
    Write-Section 'FRAME BUDGET: horde, streaming, collapse'

    $budgetWorld = Join-Path $RepoRoot 'Saved\MadFallWorlds\CIBudget'
    if (Test-Path $budgetWorld) { Remove-Item -Recurse -Force $budgetWorld }

    $spawns = (1..15 | ForEach-Object { "mad.ai.spawn madfall:zombie_civilian $((($_ % 5) - 2) * 4) $(6 + ($_ % 3) * 2) 1" }) -join '; '
    $steps = (1..16 | ForEach-Object { "mad.player.tp $($_ * 16) 0 45; wait 1.5" }) -join '; '
    $budgetScript = "mad.ai.Sleepers 0; mad.scene.anchor; mad.weather.set storm; wait 3; mad.perf.reset; $spawns; wait 15; mad.ai.status; mad.ai.killall; $steps; wait 3; mad.player.overhead madfall:concrete_frame 12 4; wait 8; mad.debris.status; mad.weather.status; mad.far.status; mad.perf; quit"
    $budgetLog = Join-Path $LogDir 'frame-budget.log'
    $budgetProcess = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow -RedirectStandardOutput $budgetLog `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-RenderOffScreen', '-ResX=1280', '-ResY=720', '-windowed', '-unattended',
                        '-nosplash', '-stdout', '-NoLogTimes', '-MadWorld=CIBudget', "-ExecCmds=`"mad.onspawn $budgetScript`"")

    if (-not $budgetProcess.WaitForExit(400000)) {
        $budgetProcess | Stop-Process -Force
        Write-Host 'FAILED: the frame budget session did not finish within 400 s.' -ForegroundColor Red
        $script:Failures += 'frame-budget'
    }
    else {
        $budgetOk = $true
        $frameLine = Select-String -Path $budgetLog -Pattern 'MadFall frame budget: (\d+) frames, (\d+) with MadFall work, (\d+) over 2\.0 ms; worst frame ([0-9.]+) ms' | Select-Object -Last 1
        if ($null -eq $frameLine) {
            Write-Host 'FAILED: no mad.perf report in the log' -ForegroundColor Red
            $budgetOk = $false
        }
        else {
            $groups = $frameLine.Matches[0].Groups
            $working = [int]$groups[2].Value
            $over = [int]$groups[3].Value
            $worst = [double]$groups[4].Value
            $percent = if ($working -gt 0) { 100.0 * $over / $working } else { 100.0 }

            foreach ($check in @(
                @{ Ok = (Select-String -Path $budgetLog -Pattern 'Zombies: (1[0-9]|[2-9][0-9]) alive' -Quiet); Why = 'the horde spawned (at least 10 zombies alive)' },
                @{ Ok = (Select-String -Path $budgetLog -Pattern '[1-9][0-9]* blocks fallen' -Quiet);         Why = 'the slab collapsed' },
                @{ Ok = (Select-String -Path $budgetLog -Pattern 'Weather: storm \(forced\), cloud 1\.00, precipitation 1\.00' -Quiet); Why = 'all of it in a storm' },
                @{ Ok = (Select-String -Path $budgetLog -Pattern 'Far terrain: \d+ tile\(s\) wanted, ([5-9]\d|[1-9]\d\d+) built' -Quiet); Why = 'with the far terrain built out to the horizon' },
                @{ Ok = $working -ge 1000;                                                                   Why = "the session did enough work to measure ($working working frames)" },
                @{ Ok = $percent -le 0.5;                                                                    Why = "at most 0.5% of working frames over 2 ms ($over of $working, $([math]::Round($percent, 2))%)" },
                @{ Ok = $worst -le 5.0;                                                                      Why = "no frame over 5 ms (worst $worst ms)" }
            )) {
                if ($check.Ok) { Write-Host "OK: $($check.Why)" -ForegroundColor Green }
                else { Write-Host "FAILED: $($check.Why)" -ForegroundColor Red; $budgetOk = $false }
            }
        }
        if (-not $budgetOk) {
            Select-String -Path $budgetLog -Pattern 'LogMadFallVoxel: Display:   ' | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor DarkGray }
            $script:Failures += 'frame-budget'
        }
    }
}

# ---------------------------------------------------------------------------
# Gate 11: packaged build and a content mod
# ---------------------------------------------------------------------------
#
# Everything above runs uncooked. This packages the real thing - cook, IoStore
# paks, staged Definitions - builds every example mod against that release, and
# runs the packaged exe. The content mod is the proof that Tier 2 works: its
# material exists only in its own cooked container, and the log must show the
# concrete surface rendering with it. Also catches what uncooked runs cannot:
# assets referenced only from data that the cooker dropped.

if ($SkipPackage -or $SkipTests) {
    Write-Section 'PACKAGE (skipped)'
    $script:Skipped += 'package'
}
else {
    Write-Section 'PACKAGE: cooked game + content mod'

    $packageLog = Join-Path $LogDir 'package.log'
    $packageOut = Join-Path $RepoRoot 'Saved\CIPackage'
    & (Join-Path $PSScriptRoot 'Package.ps1') -OutputDir $packageOut *>&1 | Tee-Object -FilePath $packageLog | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: Scripts/Package.ps1 exited $LASTEXITCODE (see $packageLog)" -ForegroundColor Red
        $script:Failures += 'package'
    }
    else {
        Write-Host 'OK: packaged the game and built every example mod' -ForegroundColor Green

        $packagedExe = Join-Path $packageOut 'Windows\MadFall.exe'
        $packagedLog = Join-Path $LogDir 'packaged-run.log'
        $packagedWorld = Join-Path $packageOut 'Windows\MadFall\Saved\MadFallWorlds'
        if (Test-Path $packagedWorld) { Remove-Item -Recurse -Force $packagedWorld }

        $probe = @(
            'mad.mods'
            'mad.voxel.set 3 0 22 madfall:concrete_frame'
            'wait 3'
            'mad.strings @items.wood_plank'
            'quit'
        ) -join '; '
        $packagedProcess = Start-Process -FilePath $packagedExe -PassThru -NoNewWindow -RedirectStandardOutput $packagedLog `
            -ArgumentList @('-nullrhi', '-unattended', '-nosplash', '-stdout', '-NoLogTimes', "-ExecCmds=`"mad.onspawn $probe`"")

        if (-not $packagedProcess.WaitForExit(240000)) {
            $packagedProcess | Stop-Process -Force
            Write-Host 'FAILED: the packaged game did not finish within 240 s.' -ForegroundColor Red
            $script:Failures += 'packaged-run'
        }
        else {
            $packagedOk = $true
            foreach ($check in @(
                @{ Pattern = 'Player spawned at';                                         Why = 'the packaged game streamed a world and spawned the survivor' },
                @{ Pattern = 'Mods: (\d+) found, \1 enabled';                              Why = 'it found every installed mod, all enabled' },
                @{ Pattern = 'Mounted IoStore container ".*Mods/example_paint/ExamplePaint/.*\.utoc"'; Why = "the engine mounted the content mod's container from Mods/" },
                @{ Pattern = 'Surface example_paint:paint renders with /ExamplePaint/M_Paint'; Why = "the mod's painted concrete renders with its cooked material" },
                @{ Pattern = '@items.wood_plank -> "Wood Plank"';                          Why = 'strings load from the staged Definitions' }
            )) {
                if (Select-String -Path $packagedLog -Pattern $check.Pattern -Quiet) {
                    Write-Host "OK: $($check.Why)" -ForegroundColor Green
                }
                else {
                    Write-Host "FAILED: $($check.Why) - no log line matching '$($check.Pattern)'" -ForegroundColor Red
                    $packagedOk = $false
                }
            }
            if (Select-String -Path $packagedLog -Pattern 'Could not load section material' -Quiet) {
                Write-Host 'FAILED: the packaged build has no voxel material (dropped by the cook)' -ForegroundColor Red
                $packagedOk = $false
            }
            if (-not $packagedOk) {
                $script:Failures += 'packaged-run'
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

Write-Section 'SUMMARY'

if ($script:Skipped.Count -gt 0) {
    Write-Host ("SKIPPED: {0}" -f ($script:Skipped -join ', ')) -ForegroundColor Yellow
}

if ($script:Failures.Count -gt 0) {
    Write-Host ("FAILED GATES: {0}" -f ($script:Failures -join ', ')) -ForegroundColor Red
    exit 1
}

Write-Host 'ALL GATES PASSED' -ForegroundColor Green
exit 0
