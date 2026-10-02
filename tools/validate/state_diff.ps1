# State-matched decomp-vs-PJ64 memory-diff harness.
#
# Captures the recomp's RDRAM at several game states IN ONE run (ROGUESQ_DUMP_RDRAM_ON_STATE, keyed
# to the game-state classifier), then diffs each state's dump against a matching PJ64 golden and
# prints a per-state pass/diff table. This is the "compare in parallel with PJ64" harness: the two
# emulations never need to run wall-clock-simultaneously -- each dumps at the same *logical* state
# and we diff by state id.
#
#   pwsh tools/validate/state_diff.ps1 -States cinematic,menu,mission
#
# Goldens: dumps/pj64/rdram_state_<id>.bin (capture with PJ64 dumping at the same logical states;
# see tools/validate/pj64_rs64_dump.js -- add state-tagged dumps, or symlink existing goldens to
# these names). Missing goldens are reported, not fatal, so the recomp side still runs + self-checks.
param(
    [string[]]$States = @("cinematic", "menu", "mission"),
    [int]$Timeout = 60,
    [string]$Level = "5",
    [switch]$NoLaunch                       # skip the recomp run; diff existing dumps only
)
$ErrorActionPreference = "Stop"
$States = @($States | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne "" })
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$exe  = Join-Path $root "build\Debug\RogueSquadron64Recomp.exe"
$exeDumps = Join-Path $root "build\Debug\dumps"          # ON_STATE dump path is cwd-relative
$pj64Dir  = Join-Path $root "dumps\pj64"
$py = "python"
$list = ($States -join ",")

if (-not $NoLaunch) {
    New-Item -ItemType Directory -Force $exeDumps | Out-Null
    Get-ChildItem $exeDumps -Filter "rdram_state_*.bin" -ErrorAction SilentlyContinue | Remove-Item -Force
    # One recomp run, hidden window, dumping every requested state. BOOT_TARGET=level drives into a
    # mission (so mission-state dumps are reachable); menu/cinematic states are hit en route.
    Write-Host "[state_diff] launching recomp (hidden), states: $list ..."
    $job = Start-Job -ScriptBlock {
        param($exe, $dir, $list, $lvl)
        Set-Location $dir
        & $exe --fake-controller --hide-window `
            --set "ROGUESQ_BOOT_TARGET=level:$lvl" `
            --set "ROGUESQ_DUMP_RDRAM_ON_STATE=$list" `
            --set "ROGUESQ_NO_AUDIO_UCODE=1" *>&1
    } -ArgumentList $exe, (Split-Path $exe), $list, $Level
    Start-Sleep -Seconds $Timeout
    Stop-Job $job; Receive-Job $job | Out-Null; Remove-Job $job
}

$rows = @()
foreach ($s in $States) {
    $cand = Join-Path $exeDumps "rdram_state_$s.bin"
    $gold = Join-Path $pj64Dir  "rdram_state_$s.bin"
    if (-not (Test-Path $cand)) { $rows += [pscustomobject]@{ State = $s; Result = "NO CANDIDATE (state not reached)" }; continue }
    if (-not (Test-Path $gold)) { $rows += [pscustomobject]@{ State = $s; Result = "NO GOLDEN (capture PJ64 rdram_state_$s.bin)" }; continue }
    $focus = & $py (Join-Path $PSScriptRoot "focus_of.py") (Join-Path $root "tools/state/state_model.toml") $s
    $args  = @($gold, $cand) + $focus
    $out = & $py (Join-Path $PSScriptRoot "rdram_golden_diff.py") @args 2>&1 | Out-String
    $diffLine = ($out -split "`n" | Select-String -Pattern "differing|identical|FOCUS|mismatch" | Select-Object -First 1)
    $rows += [pscustomobject]@{ State = $s; Result = ($diffLine -as [string]).Trim() }
}
Write-Host ""
$rows | Format-Table -AutoSize
