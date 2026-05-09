param(
    [int]$Timeout = 30,
    [string]$Tag = "leak-baseline",
    [string]$EnvVars = ""
)
$Binary = ".\build\Debug\RogueSquadron64Recomp.exe"
$logDir = Join-Path (Get-Location) "logs\stability\$Tag"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if ($EnvVars) {
    foreach ($kv in $EnvVars.Split(';')) {
        if ($kv -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

$log = Join-Path $logDir "run.log"
$memCsv = Join-Path $logDir "memory.csv"

"second,working_set_mb,private_bytes_mb,virt_mb" | Out-File -FilePath $memCsv -Encoding utf8

$proc = Start-Process -FilePath $Binary `
    -RedirectStandardError $log `
    -RedirectStandardOutput "$log.stdout" `
    -PassThru -WindowStyle Hidden

$start = Get-Date
$lastSec = -1
while ((-not $proc.HasExited) -and ((New-TimeSpan -Start $start -End (Get-Date)).TotalSeconds -lt $Timeout)) {
    $sec = [int]((New-TimeSpan -Start $start -End (Get-Date)).TotalSeconds)
    if ($sec -gt $lastSec) {
        $lastSec = $sec
        try {
            $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
            if ($p) {
                $ws = [math]::Round($p.WorkingSet64 / 1MB, 1)
                $pb = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
                $vm = [math]::Round($p.VirtualMemorySize64 / 1MB, 1)
                "$sec,$ws,$pb,$vm" | Out-File -FilePath $memCsv -Append -Encoding utf8
                Write-Host "  t=${sec}s ws=${ws}MB pb=${pb}MB vm=${vm}MB"
            }
        } catch {}
    }
    Start-Sleep -Milliseconds 250
}

if (-not $proc.HasExited) {
    try { $proc.Kill() } catch {}
    $proc.WaitForExit(5000) | Out-Null
}

Write-Host "Memory CSV: $memCsv"
Write-Host "Log: $log"
