# Record controller input into tools/recordings/<id>-<mission>/, creating the mission folder if needed.
# Usage: .\tools\recordings\record.ps1 -Level 5 [-Craft 0] [-Name jade_moon_strafe] [-Kind mission|session] [-Config Release]
# Mission recordings boot straight into the level (ROGUESQ_BOOT_TARGET) and write <name>.rec plus the <name>.rec.hash baseline.
# Session recordings start at launch; drive the menus yourself and close the window to stop.
param(
    [Parameter(Mandatory = $true)][ValidateRange(0, 18)][int]$Level,
    [string]$Craft = "",
    [string]$Name = "",
    [ValidateSet("mission", "session")][string]$Kind = "mission",
    [string]$Config = "Release"
)

$missions = @(
    "ambush-at-mos-eisley", "rendezvous-on-barkhesh", "the-search-for-the-nonnah", "defection-at-corellia",
    "liberation-of-gerrard-v", "the-jade-moon", "imperial-construction-yards", "assault-on-kile-ii",
    "rescue-on-kessel", "prisons-of-kessel", "battle-above-taloraan", "escape-from-fest",
    "blockade-on-chandrila", "raid-on-sullust", "moff-seerdons-revenge", "the-battle-of-calamari",
    "beggars-canyon", "the-death-star-trench-run", "the-battle-of-hoth"
)

$root = $PSScriptRoot
$prefix = "{0:D2}-" -f $Level
$folder = Get-ChildItem -Path $root -Directory -Filter "$prefix*" | Select-Object -First 1
if ($folder) {
    $dir = $folder.FullName
} else {
    $dir = Join-Path $root ($prefix + $missions[$Level])
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Write-Host "Created $dir (add a row for it in tools\recordings\README.md)"
}

if ($Name -eq "") {
    $slug = (Split-Path $dir -Leaf).Substring(3) -replace '-', '_'
    $Name = "{0}_{1}" -f $slug, (Get-Date -Format "yyyyMMdd_HHmmss")
}
if ($Name -notlike "*.rec") { $Name += ".rec" }
$rec = Join-Path $dir $Name
if (Test-Path $rec) {
    Write-Error "$rec already exists; pick another name or delete it first."
    exit 1
}

$exeDir = Join-Path (Split-Path (Split-Path $root)) "build\$Config"
$exe = Join-Path $exeDir "RogueSquadron64Recomp.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Binary not found: $exe"
    exit 1
}
if ($Config -ne "Release") { Write-Warning "Recording in $Config; README says record and replay in Release (Debug changes frame timing)." }

$vars = "ROGUESQ_LS_RECORD", "ROGUESQ_INPUT_RECORD", "ROGUESQ_BOOT_TARGET", "ROGUESQ_FORCE_LEVEL", "ROGUESQ_FORCE_CRAFT"
$saved = @{}
foreach ($v in $vars) { $saved[$v] = [Environment]::GetEnvironmentVariable($v); [Environment]::SetEnvironmentVariable($v, $null) }

try {
    if ($Kind -eq "mission") {
        $env:ROGUESQ_LS_RECORD = $rec
        $env:ROGUESQ_BOOT_TARGET = if ($Craft -ne "") { "level:$Level,$Craft" } else { "level:$Level" }
        Write-Host "Mission recording -> $rec (boot target $env:ROGUESQ_BOOT_TARGET). Finish or fail the mission, then close the game."
    } else {
        $env:ROGUESQ_INPUT_RECORD = $rec
        $env:ROGUESQ_FORCE_LEVEL = "$Level"
        if ($Craft -ne "") { $env:ROGUESQ_FORCE_CRAFT = $Craft }
        Write-Host "Session recording -> $rec (forced level $Level). Drive the menus from the title, play, then close the window."
    }
    Push-Location $exeDir
    try { & $exe } finally { Pop-Location }
} finally {
    foreach ($v in $vars) { [Environment]::SetEnvironmentVariable($v, $saved[$v]) }
}

if (Test-Path $rec) {
    Write-Host "Wrote $rec"
    if (Test-Path "$rec.hash") { Write-Host "Wrote $rec.hash" }
} else {
    Write-Warning "No recording written (a mission recording only starts at mission frame 0)."
}
