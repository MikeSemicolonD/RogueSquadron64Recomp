# Builds every native mod (each mods/<name>/ with a CMakeLists.txt) with its own standalone build.
# Each DLL lands beside its mod.json; the next game build copies the mods folder next to the exe.
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$root = Split-Path -Parent $PSScriptRoot
$failed = @()
$built = @()

foreach ($cml in Get-ChildItem -Path (Join-Path $root "mods") -Filter CMakeLists.txt -Recurse -Depth 1) {
    $dir = $cml.DirectoryName
    if ($dir -match "[\\/]build([\\/]|$)") {
        continue
    }

    $name = Split-Path -Leaf $dir
    Write-Host "== $name ($Config)"
    cmake -S $dir -B (Join-Path $dir "build")
    if ($LASTEXITCODE -eq 0) {
        cmake --build (Join-Path $dir "build") --config $Config
    }

    if ($LASTEXITCODE -eq 0) {
        $built += $name
    }
    else {
        $failed += $name
    }
}

Write-Host ""
Write-Host "Built: $($built -join ', ')"
if ($failed.Count -gt 0) {
    Write-Host "Failed: $($failed -join ', ')"
    exit 1
}
