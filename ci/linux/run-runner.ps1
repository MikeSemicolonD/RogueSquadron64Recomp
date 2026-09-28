# Run the Linux release runner: one fresh container per job, looping until Ctrl+C.
# Secrets come from ci/linux/runner.env (gitignored; copy runner.env.example). The ELF and ucode sources are mounted read-only.
param(
    [string]$ElfPath  = "E:\Projects\rogue_squadron64\build\roguesquadron.elf",
    [string]$UcodeDir = "E:\Projects\RogueSquadron64Recomp\build\factor5_ucode",
    [string]$EnvFile  = "$PSScriptRoot\runner.env",
    [string]$Image    = "rs64-linux-runner",
    [switch]$Rebuild,
    [switch]$Once
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $EnvFile)) { throw "Missing $EnvFile (copy runner.env.example and fill it in)" }
if (-not (Test-Path $ElfPath)) { throw "Decomp ELF not found: $ElfPath" }
foreach ($f in "factor5_ucode_recompiled.c", "factor5_boot_recompiled.c", "musyx_audio_recompiled.c") {
    if (-not (Test-Path (Join-Path $UcodeDir $f))) { throw "Missing ucode source: $UcodeDir\$f" }
}

docker image inspect $Image *> $null
if ($Rebuild -or $LASTEXITCODE -ne 0) {
    docker build --pull -t $Image $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { throw "docker build failed" }
}

do {
    docker run --rm --name "rs64-runner-$([guid]::NewGuid().ToString('N').Substring(0, 8))" `
        --env-file $EnvFile `
        -v "${ElfPath}:/rs64-inputs/roguesquadron.elf:ro" `
        -v "${UcodeDir}:/rs64-inputs/factor5_ucode:ro" `
        $Image
    Write-Host "runner container exited ($LASTEXITCODE); starting a fresh one"
} while (-not $Once)
