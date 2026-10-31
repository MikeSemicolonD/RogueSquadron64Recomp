# Builds the game from a clone of this repo on Windows: checks the tools, fetches the submodules and the decomp, imports the ROM,
# generates the recompiled code and builds Release. Re-run it after a git pull; finished steps are redone or skipped as needed.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\setup\setup.ps1 [-Rom <path>] [-MipsToolchainDir <path>]
param(
    [string]$Rom = "",
    [string]$MipsToolchainDir = "E:/mips-toolchain"
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path "$PSScriptRoot/../..").Path
$Decomp = Join-Path (Split-Path $Repo -Parent) "rogue_squadron64"
$RomSha1 = "ED42EED1EE2DB646FF7EF94BA8C5421D164A4F0D"

function Step($text)
{

    Write-Host "`n== $text"
}

function Fail($text)
{

    Write-Host "`nSetup stopped: $text" -ForegroundColor Red
    exit 1
}

# Native tools don't throw, so every call checks its exit code.
function Run($failure)
{

    & $args[0] @($args | Select-Object -Skip 1)

    if ($LASTEXITCODE -ne 0)
    {
        Fail $failure
    }
}

Set-Location $Repo

Step "Checking tools"

foreach ($c in "git", "cmake") {
    if (-not (Get-Command $c -ErrorAction SilentlyContinue)) {
        Fail "$c is not installed (or not on PATH)"
    }
}

$python = if (Get-Command py -ErrorAction SilentlyContinue) { "py" } else { "python" }

& $python -c "import sys; sys.exit(sys.version_info < (3, 11))"

if ($LASTEXITCODE -ne 0)
{
    Fail "Python 3.11 or newer is needed (python.org installer, with the py launcher)"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere) -or -not (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset -property installationPath))
{
    Fail "Visual Studio 2022 with the 'C++ Clang tools for Windows' component is needed"
}

while (-not (Test-Path "$MipsToolchainDir/bin/mips64-elf-gcc.exe")) {
    Write-Host "MIPS GCC is not in $MipsToolchainDir. Download and unzip gcc-toolchain-mips64: https://github.com/n64-tools/gcc-toolchain-mips64/releases"
    $MipsToolchainDir = (Read-Host "Folder that holds bin\mips64-elf-gcc.exe").Trim().Trim('"', "'")
}

Step "Fetching submodules"
Run "a submodule could not be fetched; try again later, or report it on GitHub" git submodule update --init --recursive

Step "Fetching the decomp"

if (-not (Test-Path "$Decomp/.git"))
{
    Run "could not clone the decomp into $Decomp" git clone https://github.com/MikeSemicolonD/rogue_squadron64.git $Decomp
}

Step "Importing the ROM"
$decompRom = "$Decomp/rogue_squadron.z64"

if (-not (Test-Path rogue_squadron.z64) -or -not (Test-Path $decompRom) -or (Get-FileHash $decompRom -Algorithm SHA1).Hash -ne $RomSha1) {
    if (-not $Rom) {
        $Rom = Read-Host "Path to your Star Wars: Rogue Squadron ROM (USA 1.0; .z64, .v64 or .n64)"
    }

    Run "the ROM was not imported" $python -I tools/setup/import_rom.py $Rom
}

Step "Building the game ELF"
Run "make_elf_from_syms.py failed" $python tools/re/make_elf_from_syms.py

Step "Building N64Recomp, RSPRecomp and RecompModTool"
Run "configuring the tools failed" cmake -S lib/N64ModernRuntime/N64Recomp -B build-tools
Run "building the tools failed" cmake --build build-tools --config Release --target N64RecompCLI RSPRecomp RecompModTool

# Visual Studio puts the tools in Release/, single-config generators in the tree's root.
$tools = if (Test-Path build-tools/Release/N64Recomp.exe) { "$Repo/build-tools/Release" } else { "$Repo/build-tools" }

Step "Recompiling the game code"
Run "N64Recomp failed" "$tools/N64Recomp.exe" rogue_squadron.toml

# The build fixes these up in place, so they are only generated once.
if (-not (Test-Path build/factor5_ucode/musyx_audio_recompiled.c))
{
    Run "RSPRecomp failed on the boot microcode" "$tools/RSPRecomp.exe" rsp/factor5_boot_rsp.toml
    Run "RSPRecomp failed on the audio microcode" "$tools/RSPRecomp.exe" rsp/musyx_rsp.toml
}

Step "Building the code mods"
Run "building the code mods failed" cmake -DPLATFORM=desktop "-DRECOMP_MOD_TOOL=$tools/RecompModTool.exe" "-DMIPS_TOOLCHAIN_DIR=$MipsToolchainDir" -P tools/mods/build_code_mods.cmake

Step "Building the game (Release)"
Run "configuring the game failed" cmake -B build -T ClangCL "-DN64RECOMP_EXE=$tools/N64Recomp.exe" "-DMIPS_TOOLCHAIN_DIR=$MipsToolchainDir"
Run "building the game failed" cmake --build build --config Release --target RogueSquadron64Recomp
Copy-Item rogue_squadron.z64 build/Release/

Step "Done"
Write-Host "Play by running $Repo\build\Release\RogueSquadron64Recomp.exe"
