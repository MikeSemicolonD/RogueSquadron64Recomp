# Run the recomp headless until it reaches the main menu, dump RDRAM (big-endian,
# PJ64 layout) for tools/debug/validate/rdram_golden_diff.py, then kill the process.
#
#   .\tools\debug\validate\capture_menu_rdram.ps1                      # -> dumps\rdram_menu_recomp.bin
#   .\tools\debug\validate\capture_menu_rdram.ps1 -Out x.bin -Timeout 240 -Screen 3
param(
    [string]$Out = "dumps\rdram_menu_recomp.bin",
    [string]$Screen = "menu",          # "menu" or a numeric screenState value (ignored when -Scene set)
    [string]$Scene = "",               # host scene id, e.g. "9" = LucasArts attribution (reached in ~10 s)
    [string]$CineIter = "",            # cinematic loop iteration n (matches PJ64 cine_frame<n> goldens)
    [int]$Timeout = 540,
    [string]$FastFwd = "0",           # ROGUESQ_CINE_FASTFWD; 0 = natural timeline (default since 2026-09-07)
    [string]$Binary = "build\Debug\RogueSquadron64Recomp.exe",
    [string]$Log = "logs\capture_menu_rdram.log"
)
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$outAbs = Join-Path $root $Out
$logAbs = Join-Path $root $Log
Remove-Item -ErrorAction SilentlyContinue $outAbs
$env:ROGUESQ_FAKE_CONTROLLER = "1"
$env:ROGUESQ_CINE_FASTFWD = $FastFwd      # complete the intro timeline naturally, faster
$env:ROGUESQ_LOG_GAMESTATE = "1"
if ($CineIter) { $env:ROGUESQ_DUMP_RDRAM_ON_CINE_ITER = $CineIter } elseif ($Scene) { $env:ROGUESQ_DUMP_RDRAM_ON_SCENE = $Scene } else { $env:ROGUESQ_DUMP_RDRAM_ON_SCREEN = $Screen }
$env:ROGUESQ_DUMP_RDRAM_PATH = $outAbs
$p = Start-Process -FilePath (Join-Path $root $Binary) -WorkingDirectory (Split-Path (Join-Path $root $Binary)) `
        -RedirectStandardError $logAbs -RedirectStandardOutput "$logAbs.out" -PassThru
$deadline = (Get-Date).AddSeconds($Timeout)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (Test-Path $outAbs) { Start-Sleep -Seconds 2; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$ok = Test-Path $outAbs
Write-Output ("result: " + $(if ($ok) { "DUMPED $Out" } else { "NO DUMP (exited=$($p.HasExited))" }))
Get-Content $logAbs | Select-String -SimpleMatch -Pattern "[rdram-dump]", "[CRASH]", "[ABORT]" | ForEach-Object { $_.Line } | Select-Object -Last 5
Get-Content $logAbs | Select-String -SimpleMatch -Pattern "[gamestate" | Select-Object -Last 3 | ForEach-Object { $_.Line }
