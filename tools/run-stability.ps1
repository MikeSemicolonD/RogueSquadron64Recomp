# Run the recompile N times with a timeout, classify outcomes by stderr markers,
# print a summary table. Captures per-run logs to logs/stability/<tag>/run_N.log.
#
# Usage:
#   .\tools\run-stability.ps1                          # 5 runs, 90s, tag=baseline
#   .\tools\run-stability.ps1 -Runs 10 -Timeout 60     # 10 runs, 60s
#   .\tools\run-stability.ps1 -Tag post-pq-fix         # label runs for diff

param(
    [int]$Runs = 5,
    [int]$Timeout = 90,
    [string]$Tag = "baseline",
    [string]$Binary = ".\build\Debug\RogueSquadron64Recomp.exe",
    [string]$EnvVars = ""  # e.g. "ROGUESQ_LOG_DPC=1;ROGUESQ_NO_SYNTH_FULLSYNC=1"
)

if (-not (Test-Path $Binary)) {
    Write-Error "Binary not found: $Binary"
    exit 1
}

$logDir = Join-Path (Get-Location) "logs\stability\$Tag"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

# Apply env vars (semicolon-delimited NAME=VALUE pairs)
$savedEnv = @{}
if ($EnvVars) {
    foreach ($kv in $EnvVars.Split(';')) {
        if ($kv -match '^([^=]+)=(.*)$') {
            $name = $Matches[1]
            $val  = $Matches[2]
            $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name)
            Set-Item -Path "env:$name" -Value $val
        }
    }
}

$results = @()
for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $logDir "run_$i.log"
    Write-Host "[$i/$Runs] running... (timeout ${Timeout}s)" -NoNewline

    # Start the process with stderr redirected. PowerShell's Start-Process can't merge
    # stderr cleanly with a wait+kill on timeout, so we use cmd's redirect via & call.
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $Binary `
        -RedirectStandardError $log `
        -RedirectStandardOutput "$log.stdout" `
        -PassThru -WindowStyle Hidden

    $exited = $proc.WaitForExit($Timeout * 1000)
    $sw.Stop()
    $wallSec = [math]::Round($sw.Elapsed.TotalSeconds, 1)

    $timedOut = $false
    if (-not $exited) {
        $timedOut = $true
        try { $proc.Kill() } catch {}
        $proc.WaitForExit(5000) | Out-Null
    }

    # Classify outcome by scanning the log
    $outcome = "unknown"
    $crashSite = ""
    $maxIter = 0
    $maxFc = 0
    $reachedNaturalExit = $false
    $reachedMenuInit = $false
    $guardCount = 0

    if (Test-Path $log) {
        $lines = Get-Content $log -ErrorAction SilentlyContinue
        foreach ($line in $lines) {
            if ($line -match '\[CRASH\]') {
                $outcome = "crash"
                if ($line -match '0x[0-9A-Fa-f]+') { $crashSite = $Matches[0] }
            }
            elseif ($line -match '\[ABORT\]')   { if ($outcome -eq "unknown") { $outcome = "abort" } }
            elseif ($line -match '\[L_627C-FIRST\]') { $reachedNaturalExit = $true }
            elseif ($line -match '\[cine-tick\] iter=(\d+).*fc=(\d+)') {
                $iter = [int]$Matches[1]
                $fc   = [int]$Matches[2]
                if ($iter -gt $maxIter) { $maxIter = $iter }
                if ($fc -gt $maxFc)     { $maxFc   = $fc }
            }
            elseif ($line -match 'menu_overlay_init|func_800C58A0') { $reachedMenuInit = $true }
            elseif ($line -match '\[guard\]') { $guardCount++ }
        }
    }

    if ($outcome -eq "unknown") {
        if ($timedOut)              { $outcome = "timeout" }
        elseif ($reachedMenuInit)   { $outcome = "menu-init" }
        elseif ($reachedNaturalExit){ $outcome = "natural-exit" }
        elseif ($maxIter -gt 0)     { $outcome = "exited-mid-cine" }
        else                        { $outcome = "exited-pre-cine" }
    }

    $row = [PSCustomObject]@{
        Run       = $i
        Outcome   = $outcome
        WallSec   = $wallSec
        MaxIter   = $maxIter
        MaxFc     = $maxFc
        Natural   = if ($reachedNaturalExit) { "Y" } else { "" }
        Menu      = if ($reachedMenuInit)    { "Y" } else { "" }
        Guards    = $guardCount
        CrashAt   = $crashSite
    }
    $results += $row
    Write-Host "  -> $outcome (iter=$maxIter, fc=$maxFc, ${wallSec}s)"
}

# Restore env vars
foreach ($name in $savedEnv.Keys) {
    if ($null -eq $savedEnv[$name]) {
        Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue
    } else {
        Set-Item -Path "env:$name" -Value $savedEnv[$name]
    }
}

Write-Host ""
Write-Host "==== Summary [tag=$Tag] ====" -ForegroundColor Cyan
$results | Format-Table -AutoSize

# Aggregate stats
$totalRuns         = $results.Count
$naturalExitRuns   = ($results | Where-Object { $_.Natural -eq "Y" }).Count
$menuInitRuns      = ($results | Where-Object { $_.Menu    -eq "Y" }).Count
$crashRuns         = ($results | Where-Object { $_.Outcome -eq "crash" }).Count
$timeoutRuns       = ($results | Where-Object { $_.Outcome -eq "timeout" }).Count
$avgMaxFc          = if ($totalRuns -gt 0) { [math]::Round((($results | Measure-Object MaxFc -Average).Average), 1) } else { 0 }

Write-Host "natural-exit: $naturalExitRuns / $totalRuns" -ForegroundColor Green
Write-Host "menu-init:    $menuInitRuns / $totalRuns"   -ForegroundColor Green
Write-Host "crash:        $crashRuns / $totalRuns"      -ForegroundColor Yellow
Write-Host "timeout:      $timeoutRuns / $totalRuns"    -ForegroundColor Yellow
Write-Host "avg max fc:   $avgMaxFc"

# Save summary CSV for diffing across tags
$csv = Join-Path $logDir "summary.csv"
$results | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8
Write-Host ""
Write-Host "Logs: $logDir"
Write-Host "CSV:  $csv"
