# Rebuilds tools/web/play/rs64tint.js + rs64tint.wasm (Tint's SPIR-V reader and WGSL writer for the browser build). Offline: run it when bumping Tint, then commit the two files.
# Needs git, CMake, Ninja, Python 3 (SPIRV-Tools generates its grammar tables with it) and an activated emsdk (Emscripten 6.0.13).
# Usage: pwsh tools/web/tint/make-tint-wasm.ps1 [-Work build-tint]
param([string]$Work = "build-tint")

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$work = Join-Path $repo $Work
$dawn = Join-Path $work "dawn"

# Dawn's commit and the third-party commits (and mirrors) its git submodules pin there.
$dawnCommit = "37f80f91a029d37019995d4bfa9141fa73fe1cc6"
$deps = @(
    @{ Path = "third_party/abseil-cpp"; Url = "https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp"; Commit = "34ede0d51ae535e13886d04e7bbb557fe34acdd5" },
    @{ Path = "third_party/spirv-headers/src"; Url = "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Headers"; Commit = "6efe5ae6b7c6ad29da3f2cc3f164b9c3d72b78b1" },
    @{ Path = "third_party/spirv-tools/src"; Url = "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Tools"; Commit = "a08de8457bfb88adc077e83ac8f18df968447f0d" }
)

function Get-Commit([string]$dir, [string]$url, [string]$commit, [string[]]$sparse) {
    if (-not (Test-Path "$dir\.git")) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        git -C $dir init -q
        git -C $dir remote add origin $url
    }

    if ($sparse) {
        git -C $dir sparse-checkout set --no-cone $sparse
    }

    git -C $dir fetch -q --depth 1 origin $commit
    git -C $dir checkout -q FETCH_HEAD
}

Get-Commit $dawn "https://github.com/google/dawn.git" $dawnCommit @("/src/tint/", "/src/utils/", "/src/cmake/", "/include/", "/third_party/CMakeLists.txt")

foreach ($d in $deps) {
    Get-Commit (Join-Path $dawn $d.Path) $d.Url $d.Commit $null
}

$build = Join-Path $work "build"
emcmake cmake -S "$PSScriptRoot" -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DDAWN_DIR=$dawn"
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build $build --target rs64tint
if ($LASTEXITCODE) { throw "build failed" }

Copy-Item "$build\rs64tint.js", "$build\rs64tint.wasm" "$repo\tools\web\play\" -Force
Get-Item "$repo\tools\web\play\rs64tint.*" | ForEach-Object { "{0} {1:N0} bytes" -f $_.Name, $_.Length }
