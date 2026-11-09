# Frame rate of a mission recording replayed on Android emulator profiles (AVDs) or a connected device.
# The replay flies the same route every run and quits at its end, so runs compare build against build and profile against profile.
# Emulator numbers include gfxstream's Vulkan translation to the host GPU: compare profiles and builds, not phones.
# The emulator needs an x86_64 APK (gradlew assembleRelease -Prs64Abis=x86_64); a phone needs the default arm64 one.
param(
    [string[]]$Avds,
    [string]$Serial,
    [string]$Recording = "$PSScriptRoot\recordings\00-ambush-at-mos-eisley\mos_eisley.rec",
    # Extra NAME=VALUE lines for the device's roguesq_env.txt, e.g. ROGUESQ_F5_STATIC_TERRAIN=1.
    [string[]]$Set = @(),
    [int]$TimeoutS = 900,
    [string]$Apk = "$PSScriptRoot\..\..\android\app\build\outputs\apk\release\RogueSquadron64Recomp-release.apk",
    [string]$Sdk = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { 'A:\Android\Sdk' }),
    [string]$AvdHome = $(if ($env:ANDROID_AVD_HOME) { $env:ANDROID_AVD_HOME } else { 'A:\Android' })
)

# Not Stop: Windows PowerShell turns adb's stderr progress lines into errors.
$ErrorActionPreference = 'Continue'
$adb = Join-Path $Sdk 'platform-tools\adb.exe'
$pkg = 'com.rs64recomp.app'
$files = "/sdcard/Android/data/$pkg/files"
$rec = "$files/perf_test.rec"
$port = 5560

$head = Get-Content $Recording -TotalCount 12
$level = ($head | Where-Object { $_ -match '^level=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1
$craft = ($head | Where-Object { $_ -match '^craft=(\d+)$' } | ForEach-Object { $Matches[1] }) | Select-Object -First 1

if ($null -eq $level -or $null -eq $craft) {
    throw "Not a mission recording (no level/craft header): $Recording"
}

$out = Join-Path $PSScriptRoot "..\..\dumps\android-perf\$('{0:yyyyMMdd_HHmmss}' -f (Get-Date))"
New-Item -ItemType Directory -Force $out | Out-Null

function Measure-Target($s, $name)
{
    # Shared storage mounts some time after sys.boot_completed; reading the env file before then looks like it is absent.
    $t0 = Get-Date

    while ((& $adb -s $s shell "test -d $files && echo y") -ne 'y') {
        if (((Get-Date) - $t0).TotalSeconds -gt 120) {
            return [pscustomobject]@{ Target = $name; Fps = 'no app storage (run the app once, with the ROM in place)' }
        }

        Start-Sleep 3
    }

    # Right after boot the package manager can still be scanning and reject the install as a duplicate pending request.
    for ($i = 0; $i -lt 10; $i++) {
        if ((& $adb -s $s install -r $Apk 2>&1) -match 'Success') {
            break
        }

        Start-Sleep 10
    }

    if ($i -eq 10) {
        return [pscustomobject]@{ Target = $name; Fps = 'install failed' }
    }

    & $adb -s $s push $Recording $rec 2>&1 | Out-Null
    & $adb -s $s shell "am force-stop $pkg; settings put secure immersive_mode_confirmations confirmed; settings put system accelerometer_rotation 0; settings put system user_rotation 1"

    $hadEnv = (& $adb -s $s shell "test -f $files/roguesq_env.txt && echo y") -eq 'y'
    $saved = & $adb -s $s shell "cat $files/roguesq_env.txt 2>/dev/null"
    $names = @('ROGUESQ_BOOT_TARGET', 'ROGUESQ_FAKE_CONTROLLER', 'ROGUESQ_LOG_FRAME_PROFILE', 'ROGUESQ_LOG_GAMESTATE', 'ROGUESQ_LS_REPLAY', 'ROGUESQ_NAV_STEP_BUDGET') + @($Set | ForEach-Object { ($_ -split '=', 2)[0] })
    $kept = @($saved | Where-Object { $_ -and ($names -notcontains ($_ -split '=', 2)[0]) })
    $lines = $kept + $Set + @("ROGUESQ_BOOT_TARGET=level:$level,$craft", "ROGUESQ_LS_REPLAY=$rec", 'ROGUESQ_NAV_STEP_BUDGET=19200', 'ROGUESQ_FAKE_CONTROLLER=1', 'ROGUESQ_LOG_FRAME_PROFILE=1', 'ROGUESQ_LOG_GAMESTATE=1')

    try {
        & $adb -s $s shell "printf '%s\n' $(($lines | ForEach-Object { "'$_'" }) -join ' ') > $files/roguesq_env.txt"
        & $adb -s $s logcat -c
        & $adb -s $s shell am start -n "$pkg/.MainActivity" | Out-Null

        # The log is read and cleared every few seconds, since a whole run's hitch lines can overflow logcat's ring.
        $fps = @()
        $inMission = $false
        $ended = $false
        $seen = $false
        $replayDone = $false
        $t0 = Get-Date

        while (-not $ended -and (((Get-Date) - $t0).TotalSeconds -lt $TimeoutS)) {
            Start-Sleep 5
            $log = @(& $adb -s $s logcat -d -s RS64)
            & $adb -s $s logcat -c
            $log | Add-Content -Encoding utf8 (Join-Path $out "$name.log")

            foreach ($line in $log) {
                # Anchored after the tag: an unanchored, case-insensitive state= also matches the line's later cineState= field.
                if ($line -match '\[gamestate[^\]]*\] (?:-> |state=)(\S+)') {
                    $inMission = ($Matches[1] -eq 'mission')
                }
                elseif ($inMission -and ($line -match '\[frameprof\] present=[\d.]+ fps display=([\d.]+) fps.*period ms min/avg/max=[\d.]+/[\d.]+/([\d.]+)')) {
                    # display = frames RT64 shows (the F7 Display line); the period max is the slowest game frame in that second.
                    $fps += [pscustomobject]@{ Fps = [double]$Matches[1]; Max = [double]$Matches[2] }
                }
                elseif ($line -match '\[ls\] session ended') {
                    $ended = $true
                    $replayDone = $true
                }
            }

            # The process exiting ends the run only once it has been seen: it takes a few seconds to start.
            $running = [bool](& $adb -s $s shell "pidof $pkg")
            $seen = $seen -or $running

            if ($seen -and -not $running) {
                $ended = $true
            }
        }

        if ($fps.Count -eq 0) {
            return [pscustomobject]@{ Target = $name; Fps = 'no samples'; Finished = $replayDone }
        }

        $stats = $fps.Fps | Measure-Object -Average -Minimum

        return [pscustomobject]@{
            Target = $name
            Fps = [math]::Round($stats.Average, 1)
            MinFps = [math]::Round($stats.Minimum, 1)
            WorstFrameMs = ($fps.Max | Measure-Object -Maximum).Maximum
            Seconds = $fps.Count
            # False: the game exited (a crash or a kill) or the timeout hit before the replay's end.
            Finished = $replayDone
        }
    }
    finally {
        & $adb -s $s shell "am force-stop $pkg; rm -f $rec $rec.replay.hash"

        if ($hadEnv) {
            & $adb -s $s shell "printf '%s\n' $(($saved | ForEach-Object { "'$_'" }) -join ' ') > $files/roguesq_env.txt"
        }
        else {
            & $adb -s $s shell "rm -f $files/roguesq_env.txt"
        }
    }
}

if (-not (Test-Path $Apk)) {
    throw "APK not found: $Apk"
}

$results = @()

if ($Serial) {
    $results += Measure-Target $Serial $Serial
}
else {
    # Default: every AVD on API 29+ (the API 26/28 images have no Vulkan driver).
    if (-not $Avds) {
        $Avds = Get-ChildItem $AvdHome -Filter *.avd -Directory | Where-Object {
            [int](Get-Content (Join-Path $_.FullName 'config.ini') | Select-String 'image.sysdir.1=.*android-(\d+)').Matches[0].Groups[1].Value -ge 29
        } | ForEach-Object { $_.Name -replace '\.avd$', '' }
    }

    $env:ANDROID_AVD_HOME = $AvdHome

    foreach ($avd in $Avds) {
        # A fresh port each time: the previous emulator can still hold its port while it shuts down, and a second one on it exits at once.
        $port += 2
        $s = "emulator-$port"
        $emu = Start-Process -FilePath (Join-Path $Sdk 'emulator\emulator.exe') -ArgumentList '-avd', $avd, '-port', $port, '-no-snapshot', '-no-audio' -PassThru

        try {
            $t0 = Get-Date

            while (((& $adb -s $s shell getprop sys.boot_completed 2>$null) -ne '1') -and (-not $emu.HasExited) -and (((Get-Date) - $t0).TotalSeconds -lt 300)) {
                Start-Sleep 3
            }

            if ((& $adb -s $s shell getprop sys.boot_completed 2>$null) -ne '1') {
                $results += [pscustomobject]@{ Target = $avd; Fps = 'emulator did not boot' }
                continue
            }

            $results += Measure-Target $s $avd
        }
        finally {
            & $adb -s $s emu kill | Out-Null
            $emu.WaitForExit(30000) | Out-Null
        }
    }
}

$csv = Join-Path $out 'results.csv'
# Fixed columns: Export-Csv takes them from the first row, which can be an error row with only two.
$results = $results | Select-Object Target, Fps, MinFps, WorstFrameMs, Seconds, Finished
$results | Export-Csv -NoTypeInformation $csv
$results | Format-Table -AutoSize
"Saved $csv"
