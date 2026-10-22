# Builds every release package locally into <OutDir>/<version>/ (default build/packages): Windows x64 and x86 zips, Linux x64, arm64, armhf and i386 tarballs plus AppImages and Flatpaks (WSL) and the Android APK.
# Usage: .\tools\bundle-release.ps1 [-Version v1.2] [-OutDir %USERPROFILE%\Desktop\rs64-release] [-Only windows-x64,windows-x86,linux-x64,linux-arm64,linux-armhf,linux-i386,android] [-WslDistro Ubuntu-24.04]
# Windows zips are staged fresh (exe, DLLs, mods from mods/platforms.json), so ROMs, saves and stale mods in build*/dist never ship.
param(
    [string]$Version = "",
    [string]$OutDir = "",
    [string[]]$Only = @("windows-x64", "windows-x86", "linux-x64", "linux-arm64", "linux-armhf", "linux-i386", "android"),
    [string]$WslDistro = "Ubuntu-24.04"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
if ($Version -eq "") { $Version = "v" + (Get-Content VERSION -TotalCount 1).Trim() }
# A single comma-separated string from a VS Code prompt arrives as one element.
$Only = $Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
if ($OutDir -eq "") { $OutDir = Join-Path $repo "build\packages" }
$out = Join-Path ([Environment]::ExpandEnvironmentVariables($OutDir)) $Version
New-Item -ItemType Directory -Force -Path $out | Out-Null
$results = @()
$shaderCache = Join-Path $env:TEMP "rs64-bundle-shader-cache"
if (Test-Path $shaderCache) { Remove-Item -Recurse -Force $shaderCache }

function Invoke-Checked([string]$what, [scriptblock]$block) {
    # Windows PowerShell turns native stderr (progress, warnings) into errors under Stop, so only the exit code decides; Out-Host keeps tool output out of callers' return values.
    $ErrorActionPreference = "Continue"
    & $block 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

function Invoke-Wsl([string]$script) {
    # Windows PowerShell mangles quotes in native arguments, so the script travels base64-encoded.
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($script))
    wsl.exe -d $WslDistro -e bash -lc "echo $b64 | base64 -d | bash -l"
}

function Build-Windows([string]$arch, [string]$buildDir, [string]$cmakeArch) {
    if (-not (Test-Path "$buildDir\CMakeCache.txt")) {
        Invoke-Checked "configure $arch" { cmake -B $buildDir -G "Visual Studio 17 2022" -A $cmakeArch -T ClangCL "-DN64RECOMP_EXE=$repo/build/Debug/N64Recomp.exe" }
    }
    Invoke-Checked "build $arch" { cmake --build $buildDir --config Release --target RogueSquadron64Recomp }
    $stage = Join-Path $env:TEMP "rs64-bundle-$arch"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Path $stage | Out-Null
    foreach ($f in "RogueSquadron64Recomp.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll") { Copy-Item "$buildDir\dist\$f" $stage }
    Invoke-Checked "stage mods $arch" { cmake -DSRC="$repo/mods" -DDST="$stage/mods" -DPLATFORM=desktop -DBUILT="$repo/$buildDir/built_mods/Release" -P "$repo/tools/mods/stage_mods.cmake" }
    # Shader bytecode doesn't depend on the CPU architecture, so the first Windows build of a run compiles the cache and the other reuses it.
    if (-not (Test-Path "$shaderCache\rt64_combos.txt")) {
        $env:PLUME_D3D11_SHADER_CACHE = $shaderCache
        Invoke-Checked "D3D11 shader cache $arch" { & "$stage\RogueSquadron64Recomp.exe" --precompile-d3d11-shaders "$repo\tools\d3d11\shader_combos.txt" 2>&1 | Select-Object -Last 1 }
        Remove-Item Env:PLUME_D3D11_SHADER_CACHE
    }

    Copy-Item -Path $shaderCache -Destination "$stage\d3d11_shader_cache" -Recurse
    Copy-Item ci\release-readme.txt "$stage\README.txt"
    $zip = Join-Path $out "RogueSquadron64Recomp-windows-$arch-$Version.zip"
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
    Remove-Item -Recurse -Force $stage
    return $zip
}

foreach ($target in $Only) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        switch ($target) {
            "windows-x64" { $file = Build-Windows "x64" "build" "x64" }
            "windows-x86" { $file = Build-Windows "x86" "build-x86" "Win32" }
            "linux-x64" {
                $file = Join-Path $out "RogueSquadron64Recomp-linux-x64-$Version.tar.xz"
                $wslOut = (wsl.exe -d $WslDistro -e wslpath -a ($file -replace '\\', '/')).Trim()
                $script = 'set -e; cd "$(wslpath -a ''' + ($repo -replace '\\', '/') + ''')"; ' +
                    'cmake -S . -B "$HOME/rs64-build-release" -G Ninja -DCMAKE_BUILD_TYPE=Release; ' +
                    'cmake --build "$HOME/rs64-build-release" --target RogueSquadron64Recomp -j"$(nproc)"; ' +
                    "bash tools/package-linux.sh '$wslOut' `"`$HOME/rs64-build-release`"; " +
                    "bash tools/linux/make-appimage.sh '$wslOut' x86_64 '$($wslOut.Substring(0, $wslOut.LastIndexOf('/')))/RogueSquadron64Recomp-$Version-x86_64.AppImage'; " +
                    "if command -v flatpak-builder >/dev/null; then bash tools/linux/make-flatpak.sh '$wslOut' x86_64 '$($wslOut.Substring(0, $wslOut.LastIndexOf('/')))/RogueSquadron64Recomp-$Version-x86_64.flatpak'; fi"
                Invoke-Checked "linux-x64" { Invoke-Wsl $script }
            }
            { $_ -in "linux-arm64", "linux-armhf", "linux-i386" } {
                $arch = $target -replace '^linux-', ''
                $file = Join-Path $out "RogueSquadron64Recomp-linux-$arch-$Version.tar.xz"
                $wslOutDir = (wsl.exe -d $WslDistro -e wslpath -a ($out -replace '\\', '/')).Trim()
                $script = "RS64_VERSION='$Version' " + 'bash "$(wslpath -a ''' + ($repo -replace '\\', '/') + ''')/tools/linux/cross-build.sh" ' + "$arch Release '$wslOutDir'"
                Invoke-Checked $target { Invoke-Wsl $script }
            }
            "android" {
                Push-Location android
                try { Invoke-Checked "android" { .\gradlew.bat assembleRelease --console=plain } } finally { Pop-Location }
                $apk = Get-ChildItem android\app\build\outputs\apk\release\*.apk | Select-Object -First 1
                $file = Join-Path $out "RogueSquadron64Recomp-android-arm64-$Version.apk"
                Copy-Item $apk.FullName $file
            }
            default { throw "unknown target '$target'" }
        }
        $results += [PSCustomObject]@{ Target = $target; Outcome = "OK"; File = (Split-Path $file -Leaf); MB = [Math]::Round((Get-Item $file).Length / 1MB, 1); Min = [Math]::Round($sw.Elapsed.TotalMinutes, 1) }
    }
    catch {
        $results += [PSCustomObject]@{ Target = $target; Outcome = "FAILED"; File = $_.Exception.Message; MB = 0; Min = [Math]::Round($sw.Elapsed.TotalMinutes, 1) }
    }
}
$results | Format-Table -AutoSize
Write-Host "Packages in $out"
if ($results | Where-Object { $_.Outcome -ne "OK" }) { exit 1 }
