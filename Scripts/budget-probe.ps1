# Runs the CI frame-budget session standalone, N times, and prints the numbers.
# WHY it exists: the budget gate is the one CI stage worth A/B-ing on its own,
# and doing that through a 30-minute full run wastes most of an hour.
param([int] $Runs = 2, [string] $Label = 'probe')

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $RepoRoot 'MadFall.uproject'
$EditorCmd = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$LogDir = Join-Path $RepoRoot 'Saved\CI'
$budgetWorld = Join-Path $RepoRoot 'Saved\MadFallWorlds\CIBudget'

$spawns = (1..15 | ForEach-Object { "mad.ai.spawn madfall:zombie_civilian $((($_ % 5) - 2) * 4) $(6 + ($_ % 3) * 2) 1" }) -join '; '
$steps = (1..16 | ForEach-Object { "mad.player.tp $($_ * 16) 0 45; wait 1.5" }) -join '; '
$budgetScript = "mad.ai.Sleepers 0; mad.scene.anchor; mad.weather.set storm; wait 3; mad.perf.reset; $spawns; wait 15; mad.ai.status; mad.ai.killall; $steps; wait 3; mad.player.overhead madfall:concrete_frame 12 4; wait 8; mad.player.tpbiome madfall:highlands; wait 12; mad.player.walk 8 1 0; wait 10; mad.perf; quit"

for ($run = 1; $run -le $Runs; $run++) {
    if (Test-Path $budgetWorld) { Remove-Item -Recurse -Force $budgetWorld }
    $log = Join-Path $LogDir "budget-$Label-$run.log"
    $p = Start-Process -FilePath $EditorCmd -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -ArgumentList @("`"$ProjectFile`"", '-game', '-RenderOffScreen', '-ResX=1280', '-ResY=720', '-windowed', '-unattended',
                        '-nosplash', '-stdout', '-NoLogTimes', '-MadWorld=CIBudget', "-ExecCmds=`"mad.onspawn $budgetScript`"")
    $null = $p.WaitForExit(400000)

    $frameLine = Select-String -Path $log -Pattern 'MadFall frame budget: (\d+) frames, (\d+) with MadFall work, (\d+) over 2\.0 ms; worst frame ([0-9.]+) ms, mean working frame ([0-9.]+) ms' | Select-Object -Last 1
    $rateLine = Select-String -Path $log -Pattern 'over ([0-9.]+) s of session: ([0-9.]+) ms of MadFall work, ([0-9.]+) ms a second, ([0-9.]+) frames a second' | Select-Object -Last 1
    if ($null -eq $frameLine) { Write-Output "$Label run ${run}: no report"; continue }
    $g = $frameLine.Matches[0].Groups
    $working = [int]$g[2].Value
    $percent = [math]::Round(100.0 * [int]$g[3].Value / $working, 2)
    $fps = if ($rateLine) { $rateLine.Matches[0].Groups[4].Value } else { '?' }
    Write-Output "$Label run ${run}: tail $percent%  worst $($g[4].Value) ms  mean $($g[5].Value) ms  ($working working frames, $fps fps)"
    Select-String -Path $log -Pattern ':   (meshing|player|streaming|other|zombies|far terrain) ' | ForEach-Object { "    $($_.Line.Trim())" }
}
