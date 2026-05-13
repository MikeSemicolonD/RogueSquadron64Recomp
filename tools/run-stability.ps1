# Run the recompile N times with a timeout, classify outcomes by stderr markers,
# print a summary table. Captures per-run logs to logs/stability/<tag>/run_N.log.
# When -Memory is on (default), polls Working Set / Private Bytes once per second
# and writes per-run memory CSVs + peak stats into the summary.
#
# Usage:
#   .\tools\run-stability.ps1                          # 5 runs, 90s, tag=baseline
#   .\tools\run-stability.ps1 -Runs 10 -Timeout 60     # 10 runs, 60s
#   .\tools\run-stability.ps1 -Tag post-pq-fix         # label runs for diff
#   .\tools\run-stability.ps1 -Memory:$false           # skip memory polling

param(
    [int]$Runs = 5,
    [int]$Timeout = 90,
    [string]$Tag = "baseline",
    [string]$Binary = ".\build\Debug\RogueSquadron64Recomp.exe",
    [string]$EnvVars = "",  # e.g. "ROGUESQ_LOG_DPC=1;ROGUESQ_NO_SYNTH_FULLSYNC=1"
    [bool]$Memory = $true   # poll process memory once per second
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

    # Memory poll loop. Polls Working Set / Private Bytes / Virtual once per
    # second, tracks peaks, writes per-run CSV. Falls back to plain WaitForExit
    # if -Memory:$false. Stores per-second samples so we can trim the crash-
    # window spike (minidump writes inflate WS by ~30MB right before exit).
    $samples = @()
    $exited = $false
    if ($Memory) {
        $memCsv = "$log.memory.csv"
        "second,working_set_mb,private_bytes_mb,virt_mb" | Out-File -FilePath $memCsv -Encoding utf8
        $lastSec = -1
        while ((-not $proc.HasExited) -and ($sw.Elapsed.TotalSeconds -lt $Timeout)) {
            $sec = [int]$sw.Elapsed.TotalSeconds
            if ($sec -gt $lastSec) {
                $lastSec = $sec
                try {
                    $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
                    if ($p) {
                        $ws = [math]::Round($p.WorkingSet64 / 1MB, 1)
                        $pb = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
                        $vm = [math]::Round($p.VirtualMemorySize64 / 1MB, 1)
                        $samples += [PSCustomObject]@{ Sec=$sec; WS=$ws; PB=$pb; VM=$vm }
                        "$sec,$ws,$pb,$vm" | Out-File -FilePath $memCsv -Append -Encoding utf8
                    }
                } catch {}
            }
            Start-Sleep -Milliseconds 200
        }
        if ($proc.HasExited) { $exited = $true }
    } else {
        $exited = $proc.WaitForExit($Timeout * 1000)
    }
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
    $sehCount = 0
    $maxSendDl = 0
    $maxUpdateScreen = 0
    $lastPqWc = -1
    $lastWqWc = -1

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
            elseif ($line -match '\[hle send_dl #(\d+)\]') {
                $n = [int]$Matches[1]
                if ($n -gt $maxSendDl) { $maxSendDl = $n }
            }
            elseif ($line -match '\[vi\] update_screen #(\d+).*pq\.wc=(-?\d+) wq\.wc=(-?\d+)') {
                $n = [int]$Matches[1]
                if ($n -gt $maxUpdateScreen) { $maxUpdateScreen = $n }
                $lastPqWc = [int]$Matches[2]
                $lastWqWc = [int]$Matches[3]
            }
            elseif ($line -match '\[vi\] update_screen #(\d+)') {
                $n = [int]$Matches[1]
                if ($n -gt $maxUpdateScreen) { $maxUpdateScreen = $n }
            }
            elseif ($line -match '\[recomp\] SEH caught') { $sehCount++ }
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

    # Compute peaks from samples, trimming the crash-window (last 2 samples)
    # when this run crashed — minidump writing inflates working set by ~30MB.
    $peakWS = 0; $peakPB = 0; $peakVM = 0
    if ($Memory -and $samples.Count -gt 0) {
        $trimmed = if ($outcome -eq "crash" -and $samples.Count -gt 2) {
            $samples[0..($samples.Count - 3)]
        } else { $samples }
        $peakWS = ($trimmed | Measure-Object WS -Maximum).Maximum
        $peakPB = ($trimmed | Measure-Object PB -Maximum).Maximum
        $peakVM = ($trimmed | Measure-Object VM -Maximum).Maximum
    }
    $row = [PSCustomObject]@{
        Run       = $i
        Outcome   = $outcome
        WallSec   = $wallSec
        MaxIter   = $maxIter
        MaxFc     = $maxFc
        SendDls   = $maxSendDl
        VIs       = $maxUpdateScreen
        SEHs      = $sehCount
        PqWc      = $lastPqWc
        WqWc      = $lastWqWc
        Natural   = if ($reachedNaturalExit) { "Y" } else { "" }
        Menu      = if ($reachedMenuInit)    { "Y" } else { "" }
        Guards    = $guardCount
        CrashAt   = $crashSite
        PeakWS_MB = $peakWS
        PeakPB_MB = $peakPB
        PeakVM_MB = $peakVM
    }
    $results += $row
    $memSummary = if ($Memory) { " ws=${peakWS}MB pb=${peakPB}MB" } else { "" }
    Write-Host "  -> $outcome (iter=$maxIter, fc=$maxFc, dls=$maxSendDl, vi=$maxUpdateScreen, seh=$sehCount, ${wallSec}s${memSummary})"
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

$avgSendDls       = if ($totalRuns -gt 0) { [math]::Round((($results | Measure-Object SendDls -Average).Average), 1) } else { 0 }
$avgVIs           = if ($totalRuns -gt 0) { [math]::Round((($results | Measure-Object VIs -Average).Average), 1) } else { 0 }
$totalSEHs        = ($results | Measure-Object SEHs -Sum).Sum

Write-Host "natural-exit: $naturalExitRuns / $totalRuns" -ForegroundColor Green
Write-Host "menu-init:    $menuInitRuns / $totalRuns"   -ForegroundColor Green
Write-Host "crash:        $crashRuns / $totalRuns"      -ForegroundColor Yellow
Write-Host "timeout:      $timeoutRuns / $totalRuns"    -ForegroundColor Yellow
Write-Host "avg max fc:   $avgMaxFc"
Write-Host "avg send_dls: $avgSendDls"
Write-Host "avg vi:       $avgVIs"
Write-Host "total SEH:    $totalSEHs (across $totalRuns runs)"
if ($Memory) {
    $avgPeakWS = [math]::Round((($results | Measure-Object PeakWS_MB -Average).Average), 1)
    $maxPeakWS = ($results | Measure-Object PeakWS_MB -Maximum).Maximum
    $avgPeakPB = [math]::Round((($results | Measure-Object PeakPB_MB -Average).Average), 1)
    $maxPeakPB = ($results | Measure-Object PeakPB_MB -Maximum).Maximum
    Write-Host "peak WS:      avg=${avgPeakWS}MB max=${maxPeakWS}MB"
    Write-Host "peak PB:      avg=${avgPeakPB}MB max=${maxPeakPB}MB"
}

# Save summary CSV for diffing across tags
$csv = Join-Path $logDir "summary.csv"
$results | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8
Write-Host ""
Write-Host "Logs: $logDir"
Write-Host "CSV:  $csv"
