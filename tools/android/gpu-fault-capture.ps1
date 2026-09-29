# Capture system-wide logs around an Android GPU fault, optionally while screen recording.
#   powershell -File tools/android/gpu-fault-capture.ps1 -Tag rec-none -Record none -Seconds 120
#   -Record adb-full | adb-low records with screenrecord; -Record none leaves recording to the user (built-in recorder).
param(
    [Parameter(Mandatory)] [string]$Tag,
    [ValidateSet('none', 'adb-full', 'adb-low')] [string]$Record = 'none',
    [int]$Seconds = 120,
    [int]$RecordDelay = 20,
    [string]$EnvLines = '',
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Continue'
$adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root "dumps\android\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$files = '/sdcard/Android/data/com.rs64recomp.app/files'
$pkg = 'com.rs64recomp.app'

& $adb pull "$files/roguesq_env.txt" (Join-Path $out 'env.before.txt') 2>&1 | Out-Null
if ($EnvLines) {
    $tmp = Join-Path $out 'env.pushed.txt'
    $before = Get-Content (Join-Path $out 'env.before.txt') -ErrorAction SilentlyContinue
    (@($before) + ($EnvLines -split ';')) -join "`n" | Set-Content -Encoding ascii $tmp
    & $adb push $tmp "$files/roguesq_env.txt" 2>&1 | Out-Null
}

& $adb shell screenrecord --help 2>&1 | Out-File -Encoding utf8 (Join-Path $out 'screenrecord_help.txt')
& $adb logcat -c
$logFile = Join-Path $out 'logcat_all.txt'
$log = Start-Process -PassThru -WindowStyle Hidden -FilePath $adb -ArgumentList 'logcat', '-b', 'all', '-v', 'threadtime' -RedirectStandardOutput $logFile

if (-not $NoLaunch) {
    & $adb shell am force-stop $pkg
    & $adb shell monkey -p $pkg -c android.intent.category.LAUNCHER 1 2>&1 | Out-Null
}

$rec = $null
try {
    if ($Record -ne 'none') {
        Start-Sleep -Seconds $RecordDelay
        $recArgs = if ($Record -eq 'adb-low') { '--size 1170x540 --bit-rate 4000000' } else { '--bit-rate 20000000' }
        $recLimit = [Math]::Max(10, $Seconds - $RecordDelay)
        $rec = Start-Process -PassThru -WindowStyle Hidden -FilePath $adb -ArgumentList "shell screenrecord --verbose --time-limit $recLimit $recArgs /sdcard/rs64_capture.mp4" -RedirectStandardOutput (Join-Path $out 'screenrecord_verbose.txt')
        Start-Sleep -Seconds 5
        & $adb shell dumpsys display 2>&1 | Select-String -Pattern 'mActiveModeId|mActiveSfDisplayMode|refreshRate|mDisplayModeSpecs|renderFrameRate' | ForEach-Object { $_.Line.Trim() } | Out-File -Encoding utf8 (Join-Path $out 'refresh_recording.txt')
        Start-Sleep -Seconds ([Math]::Max(1, $Seconds - $RecordDelay - 5))
    }
    else {
        Start-Sleep -Seconds $Seconds
        & $adb shell dumpsys display 2>&1 | Select-String -Pattern 'mActiveModeId|mActiveSfDisplayMode|refreshRate|mDisplayModeSpecs|renderFrameRate' | ForEach-Object { $_.Line.Trim() } | Out-File -Encoding utf8 (Join-Path $out 'refresh.txt')
    }

    & $adb shell dumpsys SurfaceFlinger 2>&1 | Out-File -Encoding utf8 (Join-Path $out 'surfaceflinger.txt')
    & $adb shell dumpsys gpu 2>&1 | Out-File -Encoding utf8 (Join-Path $out 'gpu.txt')
}
finally {
    if ($rec) {
        & $adb shell pkill -INT screenrecord 2>&1 | Out-Null
        Start-Sleep -Seconds 3
        & $adb pull /sdcard/rs64_capture.mp4 (Join-Path $out 'capture.mp4') 2>&1 | Out-Null
        & $adb shell rm -f /sdcard/rs64_capture.mp4 2>&1 | Out-Null
    }
    if ($log -and -not $log.HasExited) {
        Stop-Process -Id $log.Id -Force
    }
    if ($EnvLines) {
        & $adb push (Join-Path $out 'env.before.txt') "$files/roguesq_env.txt" 2>&1 | Out-Null
    }
}

Start-Sleep -Seconds 1
Select-String -Path $logFile -Pattern ' RS64 ' | ForEach-Object { $_.Line } | Out-File -Encoding utf8 (Join-Path $out 'rs64.txt')
$pat = 'kgsl|adreno|GPU fault|gpu hang|GPU HANG|guilty|hard recovery|stuck fence|snapshot|pagefault|page fault|DEVICE_LOST|0xFFFFFFFC|lowmemorykiller|lmkd|vkQueueSubmit|SurfaceFlinger.*(error|fail)|MediaCodec.*(error|fail)|ANativeWindow'
Select-String -Path $logFile -Pattern $pat | ForEach-Object { $_.Line } | Out-File -Encoding utf8 (Join-Path $out 'markers.txt')
$lost = (Select-String -Path $logFile -Pattern '0xFFFFFFFC' | Measure-Object).Count
"tag=$Tag record=$Record markers=$((Get-Content (Join-Path $out 'markers.txt')).Count) device_lost_lines=$lost -> $out"
