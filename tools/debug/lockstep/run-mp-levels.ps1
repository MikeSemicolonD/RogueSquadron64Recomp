# Ghost co-op pair (run-mp.ps1) on every level's solo recording, both sides flying it, then a summary of how each mission ended.
# Usage: .\tools\debug\lockstep\run-mp-levels.ps1 [-Mode sweep|deaths|complete] [-Levels 0,16] [-Tag mplv] [-Speed 4] [-Binary .\build\Release\RogueSquadron64Recomp.exe]
#   sweep:    both sides invincible; the host ends the mission (success) when its recording runs out, in case co-op drift kept it from finishing.
#   deaths:   the host invincible, the client shot down at 20/35/50% of the recording (the host's puppet dies and respawns each time), then the host fails the mission at 65%.
#   complete: the host replays its recording as a solo replay does and ignores the client's world messages (run-mp.ps1 -HostReplay -HostSolo), the client invincible, no scripted end;
#             passes only when the host's mission ends in success, so a level that fails shows where the puppet alone still changes the mission.
# Otherwise a side passes when its mission end reaches a front-end screen; writes logs\mp\<tag>-<mode>-summary.csv and exits 1 if any level fails.
param(
    [ValidateSet("sweep", "deaths", "complete")][string]$Mode = "sweep",
    [int[]]$Levels = 0..18,
    [string]$Tag = "mplv",
    [int]$Speed = 4,
    [string]$Binary = ".\build\Release\RogueSquadron64Recomp.exe",
    [int]$Timeout = 900
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$recs = Join-Path $root "tools\debug\recordings"
$rows = @()
foreach ($level in $Levels) {
    $folder = Get-ChildItem $recs -Directory | Where-Object { $_.Name -match ("^{0:D2}-" -f $level) } | Select-Object -First 1
    # The level's solo recording: the newest <mission>_<yyyyMMdd_HHmmss>.rec, else one with a .hash baseline.
    $rec = Get-ChildItem $folder.FullName -Filter "*.rec" | Where-Object { $_.Name -match "_\d{8}_\d{6}\.rec$" -and $_.Name -notmatch "_coop_client_" } | Sort-Object Name | Select-Object -Last 1
    if (-not $rec) { $rec = Get-ChildItem $folder.FullName -Filter "*.rec" | Where-Object { Test-Path "$($_.FullName).hash" } | Select-Object -First 1 }
    $frames = [int]((Get-Content $rec.FullName -Tail 1) -split " ")[0] + 1
    $pad = $rec.FullName.Substring($root.Length + 1)
    $run = "$Tag-$Mode-{0:D2}" -f $level
    $runArgs = @{ Tag = $run; HostPad = $pad; ClientPad = $pad; Speed = $Speed; Binary = $Binary; Timeout = $Timeout; HostInvincible = $true; TestLives = 9 }
    if ($Mode -eq "complete") {
        $runArgs.Remove("HostInvincible")
        $runArgs.ClientInvincible = $true
        $runArgs.HostReplay = $true
        $runArgs.HostSolo = $true
    } elseif ($Mode -eq "sweep") {
        $runArgs.ClientInvincible = $true
        $runArgs.TestEnd = "${frames}:1"
    } else {
        $runArgs.ClientDie = (20, 35, 50 | ForEach-Object { [int]($frames * $_ / 100) }) -join ","
        $runArgs.TestEnd = "$([int]($frames * 0.65)):2"
    }
    "== level $level ($($rec.Name), $frames frames)"
    $lines = & (Join-Path $PSScriptRoot "run-mp.ps1") @runArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $lines | Select-Object -Last 12
    $end = @{}
    foreach ($role in "host", "client") {
        $m = $lines | Select-String "^$role mission end: (.*)$" | Select-Object -Last 1
        $end[$role] = if ($m) { $m.Matches[0].Groups[1].Value } else { "no result" }
    }
    $ended = Select-String -Path (Join-Path $root "logs\mp\$run\host\run.log") -Pattern "\[ghost\] mission ended: result (\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    $hostResult = if ($ended) { [int]$ended.Matches[0].Groups[1].Value } else { -1 }
    $ok =if ($Mode -eq "complete") { $code -ne 2 -and $hostResult -eq 1 -and $end.client -like "menu.*" } else { $code -ne 2 -and $end.host -like "menu.*" -and $end.client -like "menu.*" }
    $rows += [pscustomobject]@{ level = $level; recording = $rec.Name; host_result = $hostResult; host = $end.host; client = $end.client; result = $(if ($ok) { "ok" } else { "FAIL" }) }
}
$summary = Join-Path $root "logs\mp\$Tag-$Mode-summary.csv"
$rows | Export-Csv -NoTypeInformation -Path $summary
$rows | Format-Table -AutoSize | Out-String
"summary: $summary"
if ($rows | Where-Object { $_.result -ne "ok" }) { exit 1 }
exit 0
