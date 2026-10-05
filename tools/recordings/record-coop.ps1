# Record your input as the co-op client (player 2) while the host (player 1) replays a solo mission recording of the same level.
# Usage: .\tools\recordings\record-coop.ps1 -Level 2 [-Craft 4] [-HostPad <rec>] [-Name <name>] [-Timeout 1800]
# The host recording defaults to the newest <mission>_<yyyyMMdd_HHmmss>.rec in the level's folder (record.ps1's default name), else the newest other solo mission recording there.
# Writes <mission>_coop_client_<timestamp>.rec next to it; it replays only in a pair: run-mp.ps1 -HostPad <host> -ClientPad <this>.
param(
    [Parameter(Mandatory = $true)][ValidateRange(0, 18)][int]$Level,
    [string]$Craft = "",
    [string]$HostPad = "",
    [string]$Name = "",
    [int]$Timeout = 1800
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$prefix = "{0:D2}-" -f $Level
$folder = Get-ChildItem -Path $PSScriptRoot -Directory -Filter "$prefix*" | Select-Object -First 1
if (-not $folder) {
    Write-Error "No recordings folder for level $Level ($prefix*); record a solo mission first with record.ps1."
    exit 1
}
$slug = $folder.Name.Substring(3) -replace '-', '_'

if ($HostPad -eq "") {
    # Solo mission recordings only: rs64-ls header, and not a co-op baseline or a co-op client recording.
    $solo = Get-ChildItem -Path $folder.FullName -Filter "*.rec" | Where-Object {
        $_.Name -notmatch '_coop' -and (Get-Content $_.FullName -TotalCount 1) -like "rs64-ls*"
    } | Sort-Object LastWriteTime -Descending
    # Fallback order: record.ps1's default name, then one with a .hash baseline (a real mission run, not a probe like mos_eisley_dive.rec), then any.
    $byConvention = $solo | Where-Object { $_.Name -match "^$([regex]::Escape($slug))_\d{8}_\d{6}\.rec$" } | Select-Object -First 1
    $withBaseline = $solo | Where-Object { Test-Path ($_.FullName + ".hash") } | Select-Object -First 1
    $pick = if ($byConvention) { $byConvention } elseif ($withBaseline) { $withBaseline } else { $solo | Select-Object -First 1 }
    if (-not $pick) {
        Write-Error "No solo mission recording in $($folder.FullName); record one with record.ps1 -Level $Level, or pass -HostPad."
        exit 1
    }
    $HostPad = $pick.FullName
} elseif (-not [IO.Path]::IsPathRooted($HostPad)) {
    $inFolder = Join-Path $folder.FullName $HostPad
    $HostPad = if (Test-Path $inFolder) { $inFolder } else { Join-Path $root $HostPad }
}
if (-not (Test-Path $HostPad)) {
    Write-Error "Host recording not found: $HostPad"
    exit 1
}

if ($Name -eq "") { $Name = "{0}_coop_client_{1}" -f $slug, (Get-Date -Format "yyyyMMdd_HHmmss") }
if ($Name -notlike "*.rec") { $Name += ".rec" }
$rec = Join-Path $folder.FullName $Name
if (Test-Path $rec) {
    Write-Error "$rec already exists; pick another name or delete it first."
    exit 1
}

# run-mp.ps1 joins pad paths onto the repo root, so they go in relative (Windows PowerShell 5.1 has no Path.GetRelativePath).
function Get-RepoRelative([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    if (-not $full.StartsWith($root + "\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "$full is outside the repository"
    }
    return $full.Substring($root.Length + 1)
}
$hostRel = Get-RepoRelative $HostPad
$recRel = Get-RepoRelative $rec
Write-Host "Host (left) replays $hostRel"
Write-Host "You fly the client (right); keep hands off until it is in the mission, then close its window to stop. Recording -> $recRel"
$mp = @{ Tag = "coop-rec"; HostPad = $hostRel; ClientLive = $true; ClientRecord = $recRel; Timeout = $Timeout }
if ($Craft -ne "") { $mp.ClientCraft = [int]$Craft }
& (Join-Path $root "tools\lockstep\run-mp.ps1") @mp

if (Test-Path $rec) {
    Write-Host "Wrote $recRel"
    Write-Host "Replay: .\tools\lockstep\run-mp.ps1 -HostPad $hostRel -ClientPad $recRel"
} else {
    Write-Warning "No recording written (the client never reached mission frame 0)."
}
