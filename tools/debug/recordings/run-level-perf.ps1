# Replay each level's mission recording (the dated one, or mos_eisley.rec for level 0) in its own launch and tabulate game fps, DL walk time, hitches and RAM.
# Usage: .\tools\debug\recordings\run-level-perf.ps1 -Tag current [-Levels 1,10] [-Binary .\build\Release\RogueSquadron64Recomp.exe] [-Speed 4] [-Timeout 600] [-ReportTag current] [-Platform linux]
# -Platform linux replays in WSL from the smoke test's staged build\SmokeLinux (run smoke.py run --platforms linux once to stage it), on a hidden Xvfb display, with RAM as the process's resident memory.
# Game fps is recorded frames over the mission's wall time, so at -Speed 4 a level that keeps up reads 120. Walk and hitches come from ROGUESQ_LOG_FRAME_PROFILE; RAM is the process working set at mission start and end.
# The table also goes to dumps\smoke\<ReportTag>\<Platform>\levelperf.js, which the smoke report page (tools\debug\smoke\report) shows for that run.
param(
    [string]$Tag = "levelperf",
    [string]$ReportTag = "current",
    [int[]]$Levels = 0..18,
    [string]$Binary = ".\build\Release\RogueSquadron64Recomp.exe",
    [int]$Speed = 4,
    [int]$Timeout = 600,
    [ValidateSet("windows", "linux")]
    [string]$Platform = "windows"
)

$linux = $Platform -eq "linux"

if ($linux) {
    $Binary = ".\build\SmokeLinux\RogueSquadron64Recomp"
}

if (-not (Test-Path $Binary)) {
    Write-Error "Binary not found: $Binary"
    exit 1
}

$exe = (Resolve-Path $Binary).Path

function ConvertTo-WslPath([string]$path) {
    return "/mnt/" + $path.Substring(0, 1).ToLower() + $path.Substring(2).Replace('\', '/')
}

$outDir = Join-Path (Get-Location) ("logs\levelperf\$Tag" + $(if ($linux) { "-linux" } else { "" }))
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$names = @("Mos Eisley", "Barkhesh", "Nonnah", "Corellia", "Gerrard V", "Jade Moon", "Construction Yards", "Kile II", "Rescue on Kessel", "Prisons of Kessel", "Taloraan", "Fest", "Chandrila", "Sullust", "Moff Seerdon", "Calamari", "Beggar's Canyon", "Trench Run", "Hoth")
$vars = "ROGUESQ_BOOT_TARGET", "ROGUESQ_FAKE_CONTROLLER", "ROGUESQ_AUDIO_GAIN", "ROGUESQ_UNFOCUSED", "ROGUESQ_SPEED", "ROGUESQ_LS_REPLAY", "ROGUESQ_LOG_FRAME_PROFILE"
$saved = @{}
foreach ($v in $vars) { $saved[$v] = [Environment]::GetEnvironmentVariable($v) }

$recs = @{}
foreach ($r in Get-ChildItem -Path $PSScriptRoot -Recurse -Filter *.rec) {
    if ($r.Directory.Name -notmatch '^(\d{2})-') { continue }
    $lvl = [int]$Matches[1]
    $dated = ($r.BaseName -match '_\d{8}_\d{6}$') -and ($r.BaseName -notmatch 'coop')
    if ($dated -or (($lvl -eq 0) -and ($r.BaseName -eq "mos_eisley"))) { $recs[$lvl] = $r.FullName }
}

$rows = @()
foreach ($lvl in $Levels) {
    $rec = $recs[$lvl]
    if (-not $rec) { continue }

    $head = Get-Content $rec -TotalCount 20
    $craft = ($head | Where-Object { $_ -match '^craft=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1
    $log = Join-Path $outDir ("level{0:D2}.log" -f $lvl)

    $env:ROGUESQ_BOOT_TARGET = "level:$lvl,$craft"
    $env:ROGUESQ_FAKE_CONTROLLER = "1"
    $env:ROGUESQ_AUDIO_GAIN = "0"
    $env:ROGUESQ_UNFOCUSED = "1"
    $env:ROGUESQ_SPEED = "$Speed"
    $env:ROGUESQ_LS_REPLAY = $rec
    $env:ROGUESQ_LOG_FRAME_PROFILE = "1"
    Write-Host "[$lvl $($names[$lvl])] ..." -NoNewline

    $pidFile = "$log.pid"
    Remove-Item $pidFile -ErrorAction SilentlyContinue

    if ($linux) {
        # wsl.exe passes no Windows environment, so the script exports it; Xvfb :99 is the display the smoke test's WSL runs share. No double quotes inside.
        $exports = ($vars | ForEach-Object { "export $_='$([Environment]::GetEnvironmentVariable($_))'" }) -join "; "
        $exports = $exports.Replace("ROGUESQ_LS_REPLAY='$rec'", "ROGUESQ_LS_REPLAY='$(ConvertTo-WslPath $rec)'")
        $script = "$exports; export VK_ICD_FILENAMES=`$HOME/mesa-dzn/share/vulkan/icd.d/dzn_icd.x86_64.json; " +
            "export LD_LIBRARY_PATH=/usr/lib/wsl/lib:`$HOME/mesa-dzn/lib/x86_64-linux-gnu:`$LD_LIBRARY_PATH; unset WAYLAND_DISPLAY; export DISPLAY=:99; " +
            "pgrep -f '^Xvfb :99' >/dev/null || (setsid Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 &); " +
            "for i in `$(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.1; done; " +
            "cd '$(ConvertTo-WslPath (Split-Path $exe))' && echo `$`$ > '$(ConvertTo-WslPath $pidFile)' && exec ./RogueSquadron64Recomp"
        $p = Start-Process -FilePath "wsl.exe" -ArgumentList "-e bash -lc `"$script`"" -PassThru -WindowStyle Hidden -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
    } else {
        $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Minimized -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $t0 = $null
    $t1 = $null
    $m0 = $null
    $m1 = $null
    $last = 0

    # The game quits as soon as the replay ends, so the end RAM is the last sample taken while it ran.
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
        Start-Sleep -Milliseconds 250
        $p.Refresh()

        # Under WSL the memory is the Linux process's resident set, read once a second since each read starts wsl.exe (every time until the replay
        # starts, so the start figure is fresh); between reads it is the last one.
        if ($linux) {
            $ws = $last

            if ((($null -eq $t0) -or (($sw.ElapsedMilliseconds % 1000) -lt 300)) -and (Test-Path $pidFile)) {
                $gamePid = (Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
                $rss = wsl.exe -e bash -c "grep VmRSS /proc/$gamePid/status 2>/dev/null"

                if ($rss -match '(\d+)\s*kB') { $ws = [math]::Round([double]$Matches[1] / 1024) }
            }
        } else {
            $ws = [math]::Round($p.WorkingSet64 / 1MB)
        }
        $txt = Get-Content $log -Raw -ErrorAction SilentlyContinue

        if (($null -eq $t0) -and ($txt -match '\[ls\] replaying')) {
            $t0 = $sw.Elapsed.TotalSeconds
            $m0 = $ws
        }

        if ($txt -match '\[ls\] session ended') {
            $t1 = $sw.Elapsed.TotalSeconds
            $m1 = $last
            break
        }

        if ($ws -gt 0) { $last = $ws }
    }

    if (-not $p.HasExited) { $p.WaitForExit(20000) | Out-Null }
    if (-not $p.HasExited) {
        # Stopping wsl.exe leaves the Linux game running, so it is killed in WSL by the pid its script wrote.
        if ($linux -and (Test-Path $pidFile)) { wsl.exe -e kill -9 (Get-Content $pidFile | Select-Object -First 1) }

        Stop-Process -Id $p.Id -Force
    }

    Remove-Item $pidFile -ErrorAction SilentlyContinue

    $lines = Get-Content $log
    $start = ($lines | Select-String '\[ls\] replaying' | Select-Object -First 1).LineNumber
    $end = $lines | Select-String '\[ls\] session ended at frame (\d+)' | Select-Object -First 1
    $frames = if ($end) { [int]$end.Matches[0].Groups[1].Value } else { 0 }
    $span = if ($start -and $end) { $lines[$start..($end.LineNumber - 1)] } else { @() }
    $dl = @($span | Select-String 'dl us/frame=(\d+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $walk = if ($dl.Count) { [math]::Round(($dl | Measure-Object -Average).Average / 1000, 2) } else { 0 }
    $hitches = @($span | Select-String '\[frameprof HITCH\]').Count
    $secs = if ($t0 -and $t1) { $t1 - $t0 } else { 0 }
    $fps = if ($secs) { [math]::Round($frames / $secs, 1) } else { 0 }

    $row = [PSCustomObject]@{ Level = "$lvl $($names[$lvl])"; Frames = $frames; Secs = [math]::Round($secs); GameFps = $fps; WalkMs = $walk; Hitches = $hitches; RamStartMB = $m0; RamEndMB = $m1 }
    $rows += $row
    Write-Host " $fps fps, walk $walk ms, $hitches hitches, $m0 -> $m1 MB"
}

foreach ($v in $vars) {
    if ($null -eq $saved[$v]) { Remove-Item "env:$v" -ErrorAction SilentlyContinue } else { Set-Item "env:$v" $saved[$v] }
}

$rows | Format-Table -AutoSize
$csv = Join-Path $outDir "summary.csv"
$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8
Write-Host "CSV: $csv"

$reportDir = Join-Path (Get-Location) "dumps\smoke\$ReportTag\$Platform"
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$commit = git -C $PSScriptRoot rev-parse --short HEAD
$data = [PSCustomObject]@{ made = (Get-Date -Format "yyyy-MM-dd HH:mm"); commit = "$commit"; speed = $Speed; rows = @($rows) }
"window.LEVEL_PERF = " + (ConvertTo-Json -InputObject $data -Depth 4 -Compress) + ";" | Set-Content -Path (Join-Path $reportDir "levelperf.js") -Encoding utf8

# The report page opened from disk finds runs through dumps\smoke\index.js.
& python -I (Join-Path $PSScriptRoot "..\smoke\smoke.py") index
Write-Host "Report: $(Join-Path $PSScriptRoot '..\smoke\report\report.html')?run=$ReportTag/$Platform (or smoke.py serve)"
