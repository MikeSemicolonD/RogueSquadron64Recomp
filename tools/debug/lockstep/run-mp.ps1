# Two-instance ghost co-op run on localhost: the host flies $HostPad, the client $ClientPad.
# Usage: .\tools\debug\lockstep\run-mp.ps1 -Tag mp-a [-HostPad <rec>] [-ClientPad <rec>] [-Latency 0] [-Loss 0] [-Jitter 0] [-Speed 4] [-Timeout 900] [-TestEnd f:r] [-TestPause f] [-TestLives n] [-TestVersion n] [-TestQuit f] [-TestUnlock n] [-TestUpgrade f:hex] [-TestPickup f] [-TestTriggerSolo] [-Lobby [-Level n]] [-ClientLive] [-ClientPage] [-HostPage] [-Sound] [-HostPassword 123456] [-Split] [-ClientRecord <rec>] [-KeepOpen] [-NavBudget n] [-Relay port [-RelayArgs flags]] [-ClientInput seq [-ClientInputDelay ms]] [-HostContinue n] [-ClientContinue n] [-HostSkipCrawl] [-HostDie f,f] [-ClientDie f,f]
# Both windows come up windowed with ray tracing off; a boot stall (log silent 15 s before the lobby) is dumped (dump-game.ps1) into the run folder and stops the pair.
# Each instance gets its own working dir (ROM, configs and saves live in the working dir), so the pair never shares a save.
param(
    [string]$Tag = "mp",
    [string]$HostPad = "tools\debug\recordings\00-ambush-at-mos-eisley\mos_eisley.rec",
    [string]$ClientPad = "tools\debug\recordings\00-ambush-at-mos-eisley\mos_eisley.rec",
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
    # The host drops its own player trigger-volume edges, so only the client's forwarded ones can advance the mission.
    [switch]$TestTriggerSolo,
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
    [switch]$ClientWatch,
    # The client is flown live (the user's controller or keyboard) instead of from $ClientPad; both sides run at 1x. The host still flies $HostPad.
    [switch]$ClientLive,
    # Lobby runs: the client stops on the MULTIPLAYER page and is driven live from there (implies -ClientLive).
    [switch]$ClientPage,
    # Lobby runs: the host also stops on the MULTIPLAYER page and is driven live (implies -ClientLive).
    [switch]$HostPage,
    # Play the host's audio (every window is muted otherwise).
    [switch]$Sound,
    # Lobby runs: the host's game password (6 digits), so it lists as locked.
    [string]$HostPassword = "",
    # Split-screen test pair: both sides flown live in borderless widescreen halves (host top, client bottom); each player's device is the first one they press.
    [switch]$Split,
    # Writes the client's input to this .rec (relative to the repo root), replayable later as -ClientPad.
    [string]$ClientRecord = "",
    # Keep both instances running after the logs go quiet (for screenshots after a mission ends); -Timeout still applies.
    [switch]$KeepOpen,
    # Menu driver presents per step before it gives up (0 = 1200 x speed); lobby runs need more to outlast the hangar launch.
    [int]$NavBudget = 0,
    # Lobby runs: connect through a local rs64_relay on this port (0 = direct); the client joins the game id the relay logs for the host.
    [int]$Relay = 0,
    # Extra rs64_relay flags (e.g. "--max-per-ip 64" for many local test hosts).
    [string]$RelayArgs = "",
    # Scripted presses for the client only (ROGUESQ_INPUT_SEQ, started $ClientInputDelay ms after launch).
    [string]$ClientInput = "",
    [int]$ClientInputDelay = 0,
    # Each side presses continue that many debrief polls in (ROGUESQ_MP_TEST_CONTINUE; -1 = never).
    [int]$HostContinue = -1,
    [int]$ClientContinue = -1,
    # Each side's craft takes a lethal hit on these mission frames (e.g. "300,450").
    [string]$HostDie = "",
    [string]$ClientDie = "",
    # Lobby runs: the host skips the opening crawl (START 60 polls in); the client follows its skip.
    [switch]$HostSkipCrawl
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$exe = (Resolve-Path (Join-Path $root $Binary)).Path
$exeDir = Split-Path $exe
$out = Join-Path $root "logs\mp\$Tag"
if ($Split -or $ClientPage -or $HostPage) { $ClientLive = $true }
$claim = Join-Path $out "claim"
$vars = "ROGUESQ_WIDESCREEN", "ROGUESQ_INPUT_CLAIM", "ROGUESQ_MP","ROGUESQ_MP_ADDR", "ROGUESQ_MP_PORT", "ROGUESQ_MP_PAD", "ROGUESQ_MP_SIM_LATENCY_MS", "ROGUESQ_MP_SIM_LOSS_PCT", "ROGUESQ_MP_SIM_JITTER_MS", "ROGUESQ_BOOT_TARGET", "ROGUESQ_FAKE_CONTROLLER", "ROGUESQ_AUDIO_GAIN", "ROGUESQ_SPEED", "ROGUESQ_NAV_STEP_BUDGET", "ROGUESQ_COOP_LOCAL", "ROGUESQ_LS_PAD2_REPLAY", "ROGUESQ_LS_REPLAY", "ROGUESQ_LS_RECORD", "ROGUESQ_GHOST_TRACE", "ROGUESQ_MP_TEST","ROGUESQ_WINDOW_POS", "ROGUESQ_MP_TEST_LIVES", "ROGUESQ_MP_TEST_VERSION", "ROGUESQ_NET_CONFIG", "ROGUESQ_MP_TEST_UNLOCK", "ROGUESQ_MP_TEST_TRIGGER_SOLO", "ROGUESQ_MP_TEST_ALLCRAFT", "ROGUESQ_NAV_WATCH_CUTSCENES", "ROGUESQ_MP_TEST_NO_SKIP_SHARE", "ROGUESQ_MP_UPNP", "ROGUESQ_FULLSCREEN", "ROGUESQ_RT_LIGHTS", "ROGUESQ_RT_SHADOWS", "ROGUESQ_RT_FOG_SHAFTS", "ROGUESQ_MP_RELAY", "ROGUESQ_INPUT_SEQ", "ROGUESQ_INPUT_DELAY", "ROGUESQ_MP_TEST_SKIP_CRAWL", "ROGUESQ_MP_TEST_CONTINUE"
$saved = @{}
foreach ($v in $vars) { $saved[$v] = [Environment]::GetEnvironmentVariable($v, "Process") }
$procs = @{}
$start = Get-Date
try {
    foreach ($v in "ROGUESQ_COOP_LOCAL", "ROGUESQ_LS_PAD2_REPLAY", "ROGUESQ_LS_REPLAY", "ROGUESQ_LS_RECORD") { [Environment]::SetEnvironmentVariable($v, $null, "Process") }
    Remove-Item "$claim.*" -ErrorAction SilentlyContinue
    $speedNow = if ($ClientLive) { 1 } else { $Speed }
    $relayLog = Join-Path $out "relay.log"
    if ($Relay -gt 0) {
        New-Item -ItemType Directory -Force $out | Out-Null
        Remove-Item $relayLog -ErrorAction SilentlyContinue
        # Its own console window (type help there), copied to relay.log for the game id below.
        $relayExe = Join-Path (Resolve-Path $exeDir) "rs64_relay.exe"
        $relayCmd = "& '$relayExe' --port $Relay --config none.cfg --rate $(120 * [Math]::Max(1, $speedNow)) $RelayArgs 2>&1 | Tee-Object -FilePath '$relayLog'"
        $procs["relay"] = Start-Process -FilePath powershell -ArgumentList "-NoProfile", "-Command", $relayCmd -WorkingDirectory $out -PassThru
        $env:ROGUESQ_MP_RELAY = "127.0.0.1:$Relay"
    }
    foreach ($role in "host", "client") {
        # The client starts once the relay lists the host's game.
        if ($Relay -gt 0 -and $role -eq "client") {
            $code = $null
            while (-not $code -and (Get-Date) -lt $start.AddSeconds($Timeout)) {
                Start-Sleep 1
                $m = Select-String -Path $relayLog -Pattern "game (\d+) .* opened" -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($m) { $code = $m.Matches[0].Groups[1].Value }
            }
            "relay game $code"
        }
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
        $env:ROGUESQ_WINDOW_POS = if ($Split) { if ($role -eq "host") { "top" } else { "bottom" } } elseif ($role -eq "host") { "left" } else { "right" }
        $env:ROGUESQ_WIDESCREEN = if ($Split) { "1" } else { $null }
        $env:ROGUESQ_INPUT_CLAIM = if ($Split) { "$(if ($role -eq 'host') { 1 } else { 2 }),$claim" } else { $null }
        # Windowed with ray tracing off whatever the copied roguesq_video.json says; neither is written back to it.
        $seqIn = if ($role -eq "client") { $ClientInput } else { "" }
        $cont = if ($role -eq "client") { $ClientContinue } else { $HostContinue }
        $env:ROGUESQ_MP_TEST_CONTINUE = if ($cont -ge 0) { "$cont" } else { $null }
        # Scripted mission-frame steps (ROGUESQ_MP_TEST): deaths on either side, the rest on the host.
        $steps = @()
        $die = if ($role -eq "client") { $ClientDie } else { $HostDie }
        if ($die) { $steps += $die -split "," | ForEach-Object { "die@$_" } }
        if ($role -eq "host") {
            if ($TestEnd) { $steps += "end@$TestEnd" }
            if ($TestPause -ge 0) { $steps += "pause@$TestPause" }
            if ($TestQuit -ge 0) { $steps += "quit@$TestQuit$(if ($TestQuitClean) { ':1' })" }
            if ($TestUpgrade) { $f, $b = $TestUpgrade -split ":"; $steps += "upgrade@${f}:0x$b" }
            if ($TestPickup -ge 0) { $steps += "pickup@$TestPickup" }
        }
        $env:ROGUESQ_MP_TEST = if ($steps) { $steps -join "," } else { $null }
        $env:ROGUESQ_INPUT_SEQ = if ($seqIn) { $seqIn } else { $null }
        $env:ROGUESQ_INPUT_DELAY = if ($seqIn) { "$ClientInputDelay" } else { $null }
        $env:ROGUESQ_MP_TEST_SKIP_CRAWL = if ($role -eq "host" -and $HostSkipCrawl) { "1" } else { $null }
        $env:ROGUESQ_FULLSCREEN = "0"
        $env:ROGUESQ_RT_LIGHTS = "0"
        $env:ROGUESQ_RT_SHADOWS = "0"
        $env:ROGUESQ_RT_FOG_SHAFTS = "0"
        $env:ROGUESQ_MP_TEST_LIVES = if ($role -eq "host" -and $TestLives -ge 0) { "$TestLives" } else { $null }
        $env:ROGUESQ_MP_TEST_VERSION = if ($role -eq "client" -and $TestVersion -ge 0) { "$TestVersion" } else { $null }
        $env:ROGUESQ_MP_TEST_UNLOCK = if ($role -eq "host" -and $TestUnlock -ge 0) { "$TestUnlock" } else { $null }
        $env:ROGUESQ_MP_TEST_TRIGGER_SOLO = if ($role -eq "host" -and $TestTriggerSolo) { "1" } else { $null }
        $env:ROGUESQ_MP_TEST_ALLCRAFT = if ($TestAllCraft) { "1" } else { $null }
        $env:ROGUESQ_NAV_WATCH_CUTSCENES = if (($ClientLate -or $ClientWatch) -and $role -eq "client") { "1" } else { $null }
        $env:ROGUESQ_MP_TEST_NO_SKIP_SHARE = if ($ClientLate -and $role -eq "client") { "1" } else { $null }
        $env:ROGUESQ_GHOST_TRACE = Join-Path $dir "ghost.csv"
        $env:ROGUESQ_MP_ADDR = if ($Discover) { $null } else { "127.0.0.1" }
        $env:ROGUESQ_MP_UPNP = "0"
        $env:ROGUESQ_MP_PORT = "$Port"
        $live = $Split -or ($ClientLive -and $role -eq "client") -or ($HostPage -and $role -eq "host")
        $env:ROGUESQ_MP_PAD = if ($live) { $null } else { (Resolve-Path (Join-Path $root $pad)).Path }
        $env:ROGUESQ_LS_RECORD = if ($role -eq "client" -and $ClientRecord) { [IO.Path]::GetFullPath((Join-Path $root $ClientRecord)) } else { $null }
        $env:ROGUESQ_MP_SIM_LATENCY_MS = "$Latency"
        $env:ROGUESQ_MP_SIM_LOSS_PCT = "$Loss"
        $env:ROGUESQ_MP_SIM_JITTER_MS = "$Jitter"
        # Without the lobby, boot the level and craft named in the recording header.
        $hdrPad = if ($live) { $HostPad } else { $pad }
        $hdr = Get-Content (Join-Path $root $hdrPad) -TotalCount 12
        $recLevel = (($hdr | Select-String "^level=(\d+)").Matches | Select-Object -First 1).Groups[1].Value
        $recCraft = (($hdr | Select-String "^craft=(\d+)").Matches | Select-Object -First 1).Groups[1].Value
        if (-not $recLevel) { $recLevel = "0" }
        if (-not $recCraft) { $recCraft = "0" }
        $roleCraft = if ($role -eq "host") { $HostCraft } else { $ClientCraft }
        if ($live -and $roleCraft -ge 0) { $recCraft = "$roleCraft" }
        # Split runs boot -Level with each side's craft (the level's own when -1), not the recording's.
        $target = if ($Split) { "level:$Level$(if ($roleCraft -ge 0) { ",$roleCraft" })" } else { "level:$recLevel,$recCraft" }
        $env:ROGUESQ_BOOT_TARGET = if (-not $Lobby) { $target } elseif ($role -eq "host") { if ($HostPage) { "lobby:page" } else { "lobby:host,$Level,$HostCraft" } } elseif ($ClientPage) { "lobby:page" } else { "lobby:join,$Level,$ClientCraft" }
        $env:ROGUESQ_NET_CONFIG = if ($Lobby) { Join-Path $dir "roguesq_net.json" } else { $null }
        $pinField = if ($HostPassword -and $role -eq "host") { ", `"host_password`": `"$HostPassword`"" } else { "" }
        if ($Lobby) { Set-Content -Encoding ascii -Path $env:ROGUESQ_NET_CONFIG -Value "{ `"last_address`": `"127.0.0.1`", `"port`": $Port$pinField }" }
        $env:ROGUESQ_FAKE_CONTROLLER = if ($live) { $null } else { "1" }
        # Muted unless -Sound, which plays the host's audio only.
        $env:ROGUESQ_AUDIO_GAIN = if ($Sound -and $role -eq "host") { $null } else { "0" }
        $env:ROGUESQ_SPEED = "$speedNow"
        # The menu driver's watchdog counts presents, which run Speed times faster.
        $env:ROGUESQ_NAV_STEP_BUDGET = if ($NavBudget -gt 0) { "$NavBudget" } else { "$(1200 * [Math]::Max(1, $speedNow))" }
        $bin = if ($role -eq "client" -and $ClientBinary) { (Resolve-Path (Join-Path $root $ClientBinary)).Path } else { $exe }
        $procs[$role] = Start-Process -FilePath $bin -WorkingDirectory $dir -PassThru -WindowStyle Normal -RedirectStandardError (Join-Path $dir "run.log") -RedirectStandardOutput (Join-Path $dir "run.stdout.log")
    }
    if ($Split) {
        foreach ($p in @(@{ n = 1; win = "top"; role = "host" }, @{ n = 2; win = "bottom"; role = "client" })) {
            "Player $($p.n) ($($p.win) window): press a button on your controller, or click the $($p.win) window and press a key."
            while (-not (Test-Path "$claim.$($p.n)") -and -not $procs[$p.role].HasExited) { Start-Sleep -Milliseconds 200 }
            if (Test-Path "$claim.$($p.n)") { "Player $($p.n): $(Get-Content "$claim.$($p.n)")" }
        }
        "The keyboard player's window must stay focused; controllers work either way."
    }
    # Done when both hash logs have stopped growing for 20 s (mission over or session ended), or on timeout.
    $deadline = (Get-Date).AddSeconds($Timeout)
    $last = @{ host = -1; client = -1 }
    $still = 0
    $logLen = @{ host = -1; client = -1 }
    $logStill = @{ host = 0; client = 0 }
    $stalled = $false
    while ((Get-Date) -lt $deadline -and -not $stalled) {
        Start-Sleep 5
        # A boot stall (log silent 15 s before the instance reached the lobby; the crawl and lobby waits are quiet too): full dump of that instance, then stop.
        foreach ($role in "host", "client") {
            $log = Join-Path $out "$role\run.log"
            $len = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
            $logStill[$role] = if ($len -ne $logLen[$role]) { 0 } else { $logStill[$role] + 5 }
            $logLen[$role] = $len
            $booting = $len -gt 0 -and -not (Select-String -Path $log -Pattern "\[lobby\]|\[ghost\]|lobby: page reached" -Quiet)
            if ($logStill[$role] -ge 15 -and $booting -and -not $procs[$role].HasExited) {
                "$role stalled (log silent $($logStill[$role]) s); dumping pid $($procs[$role].Id)"
                & (Join-Path $root "tools\debug\dump-game.ps1") -Id $procs[$role].Id -OutDir (Join-Path $out "$role")
                $stalled = $true
            }
        }
        if ($stalled) { break }
        $grew = $false
        foreach ($role in "host", "client") {
            $h = Join-Path $out "$role\ghost.csv"
            $n = if (Test-Path $h) { (Get-Item $h).Length } else { 0 }
            if ($n -ne $last[$role]) { $grew = $true; $last[$role] = $n }
        }
        $still = if ($grew -or $last.host -le 0 -or $last.client -le 0) { 0 } else { $still + 5 }
        # A live run ends when the user closes the client window, not when the logs go quiet (they may sit in the pause menu).
        if (-not ($ClientLive -or $KeepOpen) -and $still -ge 20) { break }
        if ($ClientLive -and $procs.client.HasExited) { break }
        if ($procs.host.HasExited -and $procs.client.HasExited) { break }
    }
} finally {
    foreach ($p in $procs.Values) { if (-not $p.HasExited) { taskkill /PID $p.Id /T /F 2>&1 | Out-Null } }
    foreach ($v in $vars) { [Environment]::SetEnvironmentVariable($v, $saved[$v], "Process") }
}
"wall time: $([int]((Get-Date) - $start).TotalSeconds) s"
foreach ($role in "host", "client") {
    $log = Join-Path $out "$role\run.log"
    Select-String -Path $log -Pattern "\[mp\]|\[ghost\]|\[coop\] imposter slot" | Select-Object -Last 6 | ForEach-Object { "$role  $($_.Line)" }
}
python (Join-Path $root "tools\debug\lockstep\ghost_error.py") (Join-Path $out "host\ghost.csv") (Join-Path $out "client\ghost.csv") --p95 $P95
