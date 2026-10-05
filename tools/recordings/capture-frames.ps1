# Replay a recording and screenshot the game window at exact replay frames (frame-matched A/B for render changes).
# Usage: .\tools\recordings\capture-frames.ps1 -Recording tools\recordings\06-imperial-construction-yards\construction_yards_bonus.rec -Frames 2900,3000 -Tag pickup [-Env "ROGUESQ_RT_LIGHTS=1;ROGUESQ_RT_LIGHTS_DEBUG=2"] [-Speed 4]
# The replay pauses before each listed frame (ROGUESQ_LS_PAUSE_AT) until the capture is taken, so two runs with different -Env give identical frames.
# Level and craft come from the .rec header; the hash log and PNGs go to dumps\capture\<Tag>\ (never next to the recording).
# -RenderDoc also writes GPU captures (.rdc) of each listed frame to dumps\capture\<Tag>\rdc\ (open them with the renderdoc MCP or qrenderdoc).
param(
    [Parameter(Mandatory = $true)][string]$Recording,
    [Parameter(Mandatory = $true)][int[]]$Frames,
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Env = "",
    [string]$Binary = ".\build\Release\RogueSquadron64Recomp.exe",
    [int]$Speed = 4,
    [int]$Timeout = 600,
    # Time for the renderer to present the paused frame before the capture.
    [int]$SettleMs = 250,
    [switch]$RenderDoc,
    # renderdoc.dll to load; default: the local 1.47 build the renderdoc MCP replays with, else the installed RenderDoc.
    [string]$RenderDocDll = ""
)

$exe = (Resolve-Path $Binary).Path
$rec = (Resolve-Path $Recording).Path
$head = Get-Content $rec -TotalCount 20
if ($head[0] -notmatch "^rs64-ls ") { Write-Error "Not an rs64-ls recording: $rec"; exit 1 }
$level = ($head | Where-Object { $_ -match '^level=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1
$craft = ($head | Where-Object { $_ -match '^craft=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1

$outDir = Join-Path (Get-Location) "dumps\capture\$Tag"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Get-ChildItem $outDir -Filter "f*.png" -ErrorAction SilentlyContinue | Remove-Item
$hashOut = Join-Path $outDir "replay.hash"
$pausedFile = "$hashOut.paused"
$resumeFile = "$hashOut.resume"
foreach ($f in @($hashOut, $pausedFile, $resumeFile)) { if (Test-Path $f) { Remove-Item $f } }
$log = Join-Path $outDir "game.log"
$sorted = @($Frames | Sort-Object -Unique)

$vars = @{
    ROGUESQ_BOOT_TARGET = "level:$level,$craft"; ROGUESQ_FAKE_CONTROLLER = "1"; ROGUESQ_AUDIO_GAIN = "0"; ROGUESQ_UNFOCUSED = "1"
    ROGUESQ_SPEED = "$Speed"; ROGUESQ_NAV_STEP_BUDGET = "$(1200 * [Math]::Max(1, $Speed))"
    ROGUESQ_LS_REPLAY = $rec; ROGUESQ_LS_HASH_OUT = $hashOut; ROGUESQ_LS_PAUSE_AT = ($sorted -join ',')
}
if ($RenderDoc) {
    if (-not $RenderDocDll) {
        $RenderDocDll = @("E:\Projects\renderdoc\x64\Release\renderdoc.dll", "$env:ProgramFiles\RenderDoc\renderdoc.dll") | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $RenderDocDll) { Write-Error "renderdoc.dll not found; pass -RenderDocDll"; exit 1 }
    $rdcDir = Join-Path $outDir "rdc"
    New-Item -ItemType Directory -Force -Path $rdcDir | Out-Null
    Get-ChildItem $rdcDir -Filter "*.rdc" -ErrorAction SilentlyContinue | Remove-Item
    $vars["ROGUESQ_RENDERDOC"] = $RenderDocDll
    $vars["ROGUESQ_RENDERDOC_OUT"] = Join-Path $rdcDir "rs64"
}
foreach ($kv in ($Env -split ';')) {
    $m = [regex]::Match($kv, '^\s*(\w+)=(.*)$')
    if ($m.Success) { $vars[$m.Groups[1].Value] = $m.Groups[2].Value }
}
$saved = @{}
foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); Set-Item "env:$k" $vars[$k] }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class RS64Cap {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@

function Get-PausedFrame {
    if (-not (Test-Path $pausedFile -ErrorAction SilentlyContinue)) { return -1 }
    # The file can vanish or be empty between the test and the read (the game is releasing it).
    $line = Get-Content $pausedFile -TotalCount 1 -ErrorAction SilentlyContinue
    if ($null -eq $line) { return -1 }
    $m = [regex]::Match("$line", '^(\d+)')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return -1
}

Get-Process RogueSquadron64Recomp -ErrorAction SilentlyContinue | Stop-Process
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$captured = 0
# A stalled game (nav sequencer stuck, boot freeze, replay not advancing) is killed instead of waiting out the timeout.
$StallSeconds = 30
$lastSize = -1
$lastProgress = [System.Diagnostics.Stopwatch]::StartNew()
function Test-Stalled {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern "\[nav\] stuck|aborting sequence" -Quiet)) { return "nav sequencer stuck" }
    $size = if (Test-Path $hashOut) { (Get-Item $hashOut).Length } else { 0 }
    if ($size -eq 0) {
        if ($sw.Elapsed.TotalSeconds -gt 120) { return "replay never started (boot/menus over 120 s)" }
        return $null
    }
    if ($size -ne $script:lastSize) { $script:lastSize = $size; $script:lastProgress.Restart(); return $null }
    if ($script:lastProgress.Elapsed.TotalSeconds -gt $StallSeconds) { return "no replay progress for $StallSeconds s" }
    return $null
}
$stalled = $null
try {
    foreach ($f in $sorted) {
        $poll = 0
        while ((Get-PausedFrame) -ne $f) {
            if ($p.HasExited) { break }
            if ($sw.Elapsed.TotalSeconds -gt $Timeout) { break }
            if (((++$poll % 50) -eq 0) -and ($stalled = Test-Stalled)) { break }
            Start-Sleep -Milliseconds 10
        }
        if ($stalled) { Write-Host "stalled before frame ${f}: $stalled (see $log); killed"; break }
        if ($p.HasExited) { Write-Host "game exited before frame $f (see $log)"; break }
        if ($sw.Elapsed.TotalSeconds -gt $Timeout) { Write-Host "timeout before frame $f"; break }
        Start-Sleep -Milliseconds $SettleMs
        $p.Refresh()
        $h = $p.MainWindowHandle
        $r = New-Object RS64Cap+RECT
        [RS64Cap]::GetWindowRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        if (($w -gt 0) -and ($ht -gt 0)) {
            $b = New-Object System.Drawing.Bitmap $w, $ht
            $g = [System.Drawing.Graphics]::FromImage($b)
            $hdc = $g.GetHdc(); [RS64Cap]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
            $png = Join-Path $outDir "f$f.png"
            $b.Save($png); $b.Dispose()
            $captured++
            Write-Host "frame $f -> $png"
        }
        else {
            Write-Host "no window at frame $f"
        }
        # Atomic, frame-named release: written aside then renamed, so the game never sees a half-written or stale file.
        $tmp = "$resumeFile.tmp"
        [System.IO.File]::WriteAllText($tmp, "$f")
        Move-Item -Path $tmp -Destination $resumeFile -Force
        while (((Get-PausedFrame) -eq $f) -and -not $p.HasExited) { Start-Sleep -Milliseconds 5 }
        $lastProgress.Restart()
    }
}
finally {
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit(5000) | Out-Null }
    foreach ($k in $saved.Keys) { if ($null -eq $saved[$k]) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } else { Set-Item "env:$k" $saved[$k] } }
}
Write-Host "captured $captured / $($sorted.Count) (level $level craft $craft, speed $Speed) in $outDir"
if ($RenderDoc) {
    $rdcs = @(Get-ChildItem (Join-Path $outDir "rdc") -Filter "*.rdc" -ErrorAction SilentlyContinue)
    Write-Host "renderdoc: $($rdcs.Count) capture(s)"
    $rdcs | ForEach-Object { Write-Host "  $($_.FullName)" }
}
