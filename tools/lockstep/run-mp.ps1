# Two-instance ghost co-op run on localhost: the host flies $HostPad, the client $ClientPad.
# Usage: .\tools\lockstep\run-mp.ps1 -Tag mp-a [-HostPad <rec>] [-ClientPad <rec>] [-Latency 0] [-Loss 0] [-Jitter 0] [-Speed 4] [-Timeout 900] [-TestEnd f:r] [-TestPause f] [-TestLives n] [-TestVersion n] [-TestQuit f] [-TestUnlock n] [-TestUpgrade f:hex] [-TestPickup f] [-Lobby [-Level n]]
# Each instance gets its own working dir (ROM, configs and saves live in the working dir), so the pair never shares a save.
param(
    [string]$Tag = "mp",
    [string]$HostPad = "tools\recordings\00-ambush-at-mos-eisley\mos_eisley.rec",
    [string]$ClientPad = "tools\recordings\00-ambush-at-mos-eisley\mos_eisley.rec",
    [string]$Binary = ".\build\Release\RogueSquadron64Recomp.exe",
    # Optional different build for the client (e.g. Debug, for symbolized crashes).
    [string]$ClientBinary = "",
    [int]$Latency = 0,
    [int]$Loss = 0,
    [int]$Jitter = 0,
    [int]$Speed = 4,
    [int]$Port = 27064,
    [int]$Timeout = 900,
    [double]$P95 = 1.0,
    [string]$TestEnd = "",
    [int]$TestPause = -1,
    [int]$TestLives = -1,
    [int]$TestVersion = -1,
    # Connect through the MULTIPLAYER menu (BOOT_TARGET lobby:host|join) instead of ROGUESQ_MP.
    [switch]$Lobby,
    [int]$TestQuit = -1,
    # With -TestQuit: quit through the menu path (sends BYE) instead of dying silently.
    [switch]$TestQuitClean,
    [int]$TestUnlock = -1,
    # Host ORs power-up bits on that mission frame, as a pickup does (f:hex, e.g. 300:200).
    [string]$TestUpgrade = "",
    # Host collects the first power-up that ticks from that mission frame on, as if it flew through it.
    [int]$TestPickup = -1,
    # Lobby runs: the mission the host picks.
    [int]$Level = 0,
    # Lobby runs: each side's craft (-1 = the level's default) and the all-crafts unlock for both.
    [int]$HostCraft = -1,
    [int]$ClientCraft = -1,
    [switch]$TestAllCraft,
    # Lobby runs: the client finds the host by LAN discovery instead of ROGUESQ_MP_ADDR.
    [switch]$Discover,
    # Lobby runs: the client sits through every cutscene and ignores the host's skips, so it starts the mission late.
    [switch]$ClientLate,
    # Lobby runs: the client never skips a cutscene itself, so only the host's shared skips move it on.
    [switch]$ClientWatch
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$exe = (Resolve-Path (Join-Path $root $Binary)).Path
$exeDir = Split-Path $exe
$out = Join-Path $root "logs\mp\$Tag"
$vars = "ROGUESQ_MP", "ROGUESQ_MP_ADDR", "ROGUESQ_MP_PORT", "ROGUESQ_MP_PAD", "ROGUESQ_MP_SIM_LATENCY_MS", "ROGUESQ_MP_SIM_LOSS_PCT", "ROGUESQ_MP_SIM_JITTER_MS", "ROGUESQ_BOOT_TARGET", "ROGUESQ_FAKE_CONTROLLER", "ROGUESQ_AUDIO_GAIN", "ROGUESQ_SPEED", "ROGUESQ_NAV_STEP_BUDGET", "ROGUESQ_COOP_LOCAL", "ROGUESQ_LS_PAD2_REPLAY", "ROGUESQ_LS_REPLAY", "ROGUESQ_LS_RECORD", "ROGUESQ_GHOST_TRACE", "ROGUESQ_MP_TEST_END", "ROGUESQ_WINDOW_POS", "ROGUESQ_MP_TEST_PAUSE", "ROGUESQ_MP_TEST_LIVES", "ROGUESQ_MP_TEST_VERSION", "ROGUESQ_NET_CONFIG", "ROGUESQ_MP_TEST_QUIT", "ROGUESQ_MP_TEST_UNLOCK", "ROGUESQ_MP_TEST_UPGRADE", "ROGUESQ_MP_TEST_PICKUP", "ROGUESQ_MP_TEST_ALLCRAFT", "ROGUESQ_NAV_WATCH_CUTSCENES", "ROGUESQ_MP_TEST_NO_SKIP_SHARE", "ROGUESQ_MP_UPNP"
$saved = @{}
foreach ($v in $vars) { $saved[$v] = [Environment]::GetEnvironmentVariable($v, "Process") }
$procs = @{}
$start = Get-Date
try {
    foreach ($v in "ROGUESQ_COOP_LOCAL", "ROGUESQ_LS_PAD2_REPLAY", "ROGUESQ_LS_REPLAY", "ROGUESQ_LS_RECORD") { [Environment]::SetEnvironmentVariable($v, $null, "Process") }
    foreach ($role in "host", "client") {
        $dir = Join-Path $out $role
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        New-Item -ItemType Directory -Force $dir | Out-Null
        Copy-Item (Join-Path $exeDir "rogue_squadron.z64") $dir
        # The already-imported ROM too, so neither instance imports at startup (an import can fail to reopen its fresh copy).
        if (Test-Path (Join-Path $exeDir "rs64.n64.us.1.0.z64")) { Copy-Item (Join-Path $exeDir "rs64.n64.us.1.0.z64") $dir }
        Get-ChildItem $exeDir -Filter "roguesq_*.json" | Copy-Item -Destination $dir
        # The menu driver needs an existing pilot: each instance gets its own copy of the saves.
        foreach ($sub in "mods", "mod_config", "saves") {
            if (Test-Path (Join-Path $exeDir $sub)) { Copy-Item -Recurse (Join-Path $exeDir $sub) $dir }
        }
        $pad = if ($role -eq "host") { $HostPad } else { $ClientPad }
        $env:ROGUESQ_MP = if ($Lobby) { $null } elseif ($role -eq "host") { "host" } else { "join" }
        # Side by side, so either window can be watched: host on the left, client on the right.
        $env:ROGUESQ_WINDOW_POS = if ($role -eq "host") { "left" } else { "right" }
        $env:ROGUESQ_MP_TEST_END = if ($role -eq "host" -and $TestEnd) { $TestEnd } else { $null }
        $env:ROGUESQ_MP_TEST_PAUSE = if ($role -eq "host" -and $TestPause -ge 0) { "$TestPause" } else { $null }
        $env:ROGUESQ_MP_TEST_LIVES = if ($role -eq "host" -and $TestLives -ge 0) { "$TestLives" } else { $null }
        $env:ROGUESQ_MP_TEST_VERSION = if ($role -eq "client" -and $TestVersion -ge 0) { "$TestVersion" } else { $null }
        $env:ROGUESQ_MP_TEST_QUIT = if ($role -eq "host" -and $TestQuit -ge 0) { "$TestQuit" } else { $null }
        $env:ROGUESQ_MP_TEST_QUIT_CLEAN = if ($TestQuitClean) { "1" } else { $null }
        $env:ROGUESQ_MP_TEST_UNLOCK = if ($role -eq "host" -and $TestUnlock -ge 0) { "$TestUnlock" } else { $null }
        $env:ROGUESQ_MP_TEST_UPGRADE = if ($role -eq "host" -and $TestUpgrade) { $TestUpgrade } else { $null }
        $env:ROGUESQ_MP_TEST_PICKUP = if ($role -eq "host" -and $TestPickup -ge 0) { "$TestPickup" } else { $null }
        $env:ROGUESQ_MP_TEST_ALLCRAFT = if ($TestAllCraft) { "1" } else { $null }
        $env:ROGUESQ_NAV_WATCH_CUTSCENES = if (($ClientLate -or $ClientWatch) -and $role -eq "client") { "1" } else { $null }
        $env:ROGUESQ_MP_TEST_NO_SKIP_SHARE = if ($ClientLate -and $role -eq "client") { "1" } else { $null }
        $env:ROGUESQ_GHOST_TRACE = Join-Path $dir "ghost.csv"
        $env:ROGUESQ_MP_ADDR = if ($Discover) { $null } else { "127.0.0.1" }
        $env:ROGUESQ_MP_UPNP = "0"
        $env:ROGUESQ_MP_PORT = "$Port"
        $env:ROGUESQ_MP_PAD = (Resolve-Path (Join-Path $root $pad)).Path
        $env:ROGUESQ_MP_SIM_LATENCY_MS = "$Latency"
        $env:ROGUESQ_MP_SIM_LOSS_PCT = "$Loss"
        $env:ROGUESQ_MP_SIM_JITTER_MS = "$Jitter"
        # Without the lobby, boot the level and craft named in the recording header.
        $hdr = Get-Content (Join-Path $root $pad) -TotalCount 12
        $recLevel = (($hdr | Select-String "^level=(\d+)").Matches | Select-Object -First 1).Groups[1].Value
        $recCraft = (($hdr | Select-String "^craft=(\d+)").Matches | Select-Object -First 1).Groups[1].Value
        if (-not $recLevel) { $recLevel = "0" }
        if (-not $recCraft) { $recCraft = "0" }
        $env:ROGUESQ_BOOT_TARGET = if (-not $Lobby) { "level:$recLevel,$recCraft" } elseif ($role -eq "host") { "lobby:host,$Level,$HostCraft" } else { "lobby:join,$Level,$ClientCraft" }
        $env:ROGUESQ_NET_CONFIG = if ($Lobby) { Join-Path $dir "roguesq_net.json" } else { $null }
        if ($Lobby) { Set-Content -Encoding ascii -Path $env:ROGUESQ_NET_CONFIG -Value "{ `"last_address`": `"127.0.0.1`", `"port`": $Port }" }
        $env:ROGUESQ_FAKE_CONTROLLER = "1"
        $env:ROGUESQ_AUDIO_GAIN = "0"
        $env:ROGUESQ_SPEED = "$Speed"
        # The menu driver's watchdog counts presents, which run Speed times faster.
        $env:ROGUESQ_NAV_STEP_BUDGET = "$(1200 * [Math]::Max(1, $Speed))"
        $bin = if ($role -eq "client" -and $ClientBinary) { (Resolve-Path (Join-Path $root $ClientBinary)).Path } else { $exe }
        $procs[$role] = Start-Process -FilePath $bin -WorkingDirectory $dir -PassThru -WindowStyle Normal -RedirectStandardError (Join-Path $dir "run.log") -RedirectStandardOutput (Join-Path $dir "run.stdout.log")
    }
    # Done when both hash logs have stopped growing for 20 s (mission over or session ended), or on timeout.
    $deadline = (Get-Date).AddSeconds($Timeout)
    $last = @{ host = -1; client = -1 }
    $still = 0
    while ((Get-Date) -lt $deadline) {
        Start-Sleep 5
        $grew = $false
        foreach ($role in "host", "client") {
            $h = Join-Path $out "$role\ghost.csv"
            $n = if (Test-Path $h) { (Get-Item $h).Length } else { 0 }
            if ($n -ne $last[$role]) { $grew = $true; $last[$role] = $n }
        }
        $still = if ($grew -or $last.host -le 0 -or $last.client -le 0) { 0 } else { $still + 5 }
        if ($still -ge 20) { break }
        if ($procs.host.HasExited -and $procs.client.HasExited) { break }
    }
} finally {
    foreach ($p in $procs.Values) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
    foreach ($v in $vars) { [Environment]::SetEnvironmentVariable($v, $saved[$v], "Process") }
}
"wall time: $([int]((Get-Date) - $start).TotalSeconds) s"
foreach ($role in "host", "client") {
    $log = Join-Path $out "$role\run.log"
    Select-String -Path $log -Pattern "\[mp\]|\[ghost\]|\[coop\] imposter slot" | Select-Object -Last 6 | ForEach-Object { "$role  $($_.Line)" }
}
python (Join-Path $root "tools\lockstep\ghost_error.py") (Join-Path $out "host\ghost.csv") (Join-Path $out "client\ghost.csv") --p95 $P95
