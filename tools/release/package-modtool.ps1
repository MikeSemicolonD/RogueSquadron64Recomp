# Builds RecompModTool from the N64Recomp fork for Windows x64 and Linux x64 (WSL) and packages it into <OutDir>/<version>/ with a SHA256SUMS.txt.
# Usage: .\tools\release\package-modtool.ps1 [-Version modtool-v1] [-OutDir build\packages] [-Only windows-x64,linux-x64] [-WslDistro Ubuntu-24.04]
param(
    [string]$Version = "",
    [string]$OutDir = "",
    [string[]]$Only = @("windows-x64", "linux-x64"),
    [string]$WslDistro = "Ubuntu-24.04"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $repo
$src = "lib/N64ModernRuntime/N64Recomp"
if ($Version -eq "") { $Version = "modtool-" + (git -C lib/N64ModernRuntime rev-parse --short HEAD).Trim() }
$Only = $Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
if ($OutDir -eq "") { $OutDir = Join-Path $repo "build\packages" }
$out = Join-Path ([Environment]::ExpandEnvironmentVariables($OutDir)) $Version
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Invoke-Checked([string]$what, [scriptblock]$block) {
    # Windows PowerShell turns native stderr into errors under Stop, so only the exit code decides.
    $ErrorActionPreference = "Continue"
    & $block 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

foreach ($target in $Only) {
    switch ($target) {
        "windows-x64" {
            Invoke-Checked "configure" { cmake -S $src -B build-modtool -G "Visual Studio 17 2022" -A x64 }
            Invoke-Checked "build" { cmake --build build-modtool --config Release --target RecompModTool }
            $stage = Join-Path $env:TEMP "rs64-modtool-stage"
            if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
            New-Item -ItemType Directory -Path $stage | Out-Null
            Copy-Item build-modtool\Release\RecompModTool.exe, "$src\LICENSE" $stage
            Compress-Archive -Path "$stage\*" -DestinationPath (Join-Path $out "RecompModTool-windows-x64-$Version.zip") -Force
            Remove-Item -Recurse -Force $stage
        }
        "linux-x64" {
            $wslOut = (wsl.exe -d $WslDistro -e wslpath -a ($out -replace '\\', '/')).Trim()
            $script = 'set -e; cd "$(wslpath -a ''' + ($repo -replace '\\', '/') + ''')"; ' +
                "cmake -S $src -B `"`$HOME/rs64-build-modtool`" -DCMAKE_BUILD_TYPE=Release; " +
                'cmake --build "$HOME/rs64-build-modtool" --target RecompModTool -j"$(nproc)"; ' +
                'stage="$(mktemp -d)"; cp "$HOME/rs64-build-modtool/RecompModTool" ' + "$src/LICENSE `"`$stage`"; " +
                "tar -cJf '$wslOut/RecompModTool-linux-x64-$Version.tar.xz' -C `"`$stage`" .; rm -rf `"`$stage`""
            # Windows PowerShell mangles quotes in native arguments, so the script travels base64-encoded.
            $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($script))
            Invoke-Checked "linux-x64" { wsl.exe -d $WslDistro -e bash -lc "echo $b64 | base64 -d | bash -l" }
        }
        default { throw "unknown target '$target'" }
    }
}

# Same format as `sha256sum`, so `sha256sum -c SHA256SUMS.txt` verifies it.
Get-ChildItem $out -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | Sort-Object Name |
    ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" } |
    Set-Content (Join-Path $out "SHA256SUMS.txt") -Encoding ascii

Get-ChildItem $out | Format-Table Name, Length -AutoSize
