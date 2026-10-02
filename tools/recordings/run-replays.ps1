# Replay every mission-start (rs64-ls) recording under a folder and compare each against its .hash baseline; skips recordings without a baseline and co-op baselines (*_coop.rec).
# Usage: .\tools\recordings\run-replays.ps1 -Tag release-a [-Recordings tools\recordings\00-ambush-at-mos-eisley] [-Binary .\build\Release\RogueSquadron64Recomp.exe] [-Timeout 900] [-Speed 4]
# -Speed sets ROGUESQ_SPEED (game clock multiplier): 4 replays ~3.5x faster and matched 1x; 8 is too fast for the menu driver.
param(
    [string]$Tag = "replays",
    [string]$Recordings = "tools\recordings",
    [string]$Binary = ".\build\Release\RogueSquadron64Recomp.exe",
    [int]$Timeout = 900,
    [int]$Speed = 4
)

if (-not (Test-Path $Binary)) {
    Write-Error "Binary not found: $Binary"
    exit 1
}
$exe = (Resolve-Path $Binary).Path
$outDir = Join-Path (Get-Location) "logs\lockstep\$Tag"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$compare = Join-Path $PSScriptRoot "compare_hashes.py"
$vars = "ROGUESQ_BOOT_TARGET", "ROGUESQ_FAKE_CONTROLLER", "ROGUESQ_AUDIO_GAIN", "ROGUESQ_LS_REPLAY", "ROGUESQ_LS_HASH_OUT", "ROGUESQ_SPEED", "ROGUESQ_NAV_STEP_BUDGET"
$saved = @{}
foreach ($v in $vars) { $saved[$v] = [Environment]::GetEnvironmentVariable($v) }

$results = @()
foreach ($rec in Get-ChildItem -Path $Recordings -Filter *.rec -Recurse) {
    if ((Get-Content $rec.FullName -TotalCount 1) -notmatch "^rs64-ls ") { continue }
    if ($rec.BaseName -like "*_coop" -or -not (Test-Path "$($rec.FullName).hash")) { continue }
    $head = Get-Content $rec.FullName -TotalCount 20
    $level = ($head | Where-Object { $_ -match '^level=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1
    $craft = ($head | Where-Object { $_ -match '^craft=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1
    $name = $rec.BaseName
    $hashOut = Join-Path $outDir "$name.replay.hash"
    $log = Join-Path $outDir "$name.log"
    if ($null -eq $level -or $null -eq $craft) {
        $results += [PSCustomObject]@{ Recording = $name; Outcome = "BAD-HEADER"; Detail = ""; WallSec = 0 }
        continue
    }
    if (Test-Path $hashOut) { Remove-Item $hashOut }
    $env:ROGUESQ_BOOT_TARGET = "level:$level,$craft"
    $env:ROGUESQ_FAKE_CONTROLLER = "1"
    $env:ROGUESQ_AUDIO_GAIN = "0"
    $env:ROGUESQ_SPEED = "$Speed"
    # The menu driver's watchdog counts presents, which run Speed times faster.
    $env:ROGUESQ_NAV_STEP_BUDGET = "$(1200 * [Math]::Max(1, $Speed))"
    $env:ROGUESQ_LS_REPLAY = $rec.FullName
    $env:ROGUESQ_LS_HASH_OUT = $hashOut
    Write-Host "[$name] level $level craft $craft ..." -NoNewline
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Hidden -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
    $exited = $p.WaitForExit($Timeout * 1000)
    if (-not $exited) {
        try { $p.Kill() } catch {}
        $p.WaitForExit(5000) | Out-Null
    }
    $sw.Stop()
    $outcome = "TIMEOUT"
    $detail = ""
    if ($exited) {
        $line = & python $compare "$($rec.FullName).hash" $hashOut
        $code = $LASTEXITCODE
        $detail = "$line"
        $outcome = switch ($code) { 0 { "PASS" } 1 { "DIVERGED" } default { "NO-OUTPUT" } }
    }
    $wall = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    Write-Host " $outcome $detail"
    $results += [PSCustomObject]@{ Recording = $name; Outcome = $outcome; Detail = $detail; WallSec = $wall }
}

foreach ($v in $vars) {
    if ($null -eq $saved[$v]) { Remove-Item "env:$v" -ErrorAction SilentlyContinue } else { Set-Item "env:$v" $saved[$v] }
}
$results | Format-Table -AutoSize
$csv = Join-Path $outDir "summary.csv"
$results | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8
$pass = @($results | Where-Object { $_.Outcome -eq "PASS" }).Count
Write-Host "PASS $pass / $($results.Count)   CSV: $csv"
exit ($(if ($pass -eq $results.Count) { 0 } else { 1 }))
