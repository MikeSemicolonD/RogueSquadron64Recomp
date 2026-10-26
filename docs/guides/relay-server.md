# Relay server (rs64_relay)

`rs64_relay` lets online co-op players find and join each other's games without forwarding ports. A host registers a game with the server, other players see it in a list and join, and the server forwards the two players' packets. Anyone can run one: on a desktop PC from a terminal, or on a rented Linux or Windows server. It needs no game files.

## Pointing the game at a server

Each player adds the server to `roguesq_net.json` next to the game (create it if missing), with the port if it is not 27070:

```json
{ "relay_server": "relay.example.com:27070" }
```

The game then lists the server's games under MULTIPLAYER > FIND GAME (after Steam's, when the steam-relay mod is running), and HOST GAME opens games on it when Steam is not in use. A game's join code is its id on the server, as `games` shows it.

When the server is down or the address is wrong, joining says COULD NOT CONNECT TO SERVER, FIND GAME says the same over any Steam games, and HOST GAME hosts directly (LAN, or the internet when the router allows it) instead.

## Download and run

Release packages (draft releases tagged `relay-v*`):

| Package | Contents |
|---|---|
| `rs64_relay-windows-x64-<ver>.zip` | `rs64_relay.exe`, this README, licenses. No runtime to install. |
| `rs64_relay-linux-x64-<ver>.tar.gz` | `rs64_relay`, this README, licenses. Needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12 and later). |

Unpack it anywhere and run it from a terminal:

```
rs64_relay --name "MIKES SERVER"
```

Players need to reach UDP port 27070 (`--port` changes it): open it in the machine's firewall, and on a home connection forward it on the router. The server listens on IPv4 and IPv6.

## Settings

Every setting is a command-line flag, a line in the config file, and (limits only) a console `set`. Defaults load first, then the config file, then the flags.

| Flag | Config key | Default | Meaning |
|---|---|---|---|
| `--port` | `port` | 27070 | UDP port (1-65535) |
| `--name` | `name` | RS64 RELAY | Server name shown in the game's list (A-Z, 0-9, space; at most 16) |
| `--max-clients` | `max_clients` | 64 | Connections at once: hosts, joiners and players browsing the list (1-4000) |
| `--max-games` | `max_games` | 32 | Registered games (1-2000) |
| `--max-per-ip` | `max_per_ip` | 4 | Connections from one address (1-4000) |
| `--rate` | `rate` | 120 | Packets per second per connection before it is dropped (10-10000); a game sends about 35 |
| `--idle-timeout` | `idle_timeout` | 60 | Seconds before a connection outside a game is closed (5-3600) |
| `--config` | | rs64_relay.cfg | Config file, relative to the current directory |

The config file is plain text, one `key = value` per line, `#` starts a comment:

```
port = 27070
name = MIKES SERVER
max_clients = 128
```

A missing file is fine. A bad line is logged and skipped. `save` rewrites the whole file, so comments you added by hand are lost.

## Console commands

Type these into the terminal while the server runs:

| Command | Effect |
|---|---|
| `help` | List the commands |
| `games` | Connection and game counts, the limits in use, and each game: id, name, password flag, host address, waiting or playing with whom |
| `kick <game id>` | End one game (both players are disconnected) |
| `limits` | Every limit and its value |
| `set <limit> <value>` | Change a limit now. Lowering one never drops anyone already connected. The first change prints `Use 'save' to keep the current configuration` |
| `save` | Write the current settings to the config file |
| `reload` | Re-read the config file (a new `port` applies after a restart) |
| `quit` | Disconnect everyone and stop (Ctrl+C does the same) |

Without a terminal (a service, `nohup`, stdin closed) the commands are off and the server keeps running; it stops on SIGTERM / Ctrl+C.

## Running unattended on Linux

A systemd unit, with the files in `/opt/rs64_relay`:

```
[Unit]
Description=RS64 relay server
After=network-online.target

[Service]
WorkingDirectory=/opt/rs64_relay
ExecStart=/opt/rs64_relay/rs64_relay
Restart=on-failure
DynamicUser=yes

[Install]
WantedBy=multi-user.target
```

The log goes to the journal (`journalctl -u rs64_relay`). Change settings in `rs64_relay.cfg` and restart; there is no console under systemd.

## What it does and does not do

- Each game is two players: the host and one joiner. A full game drops out of the list until the joiner leaves.
- The list shows only games from the same game build, so players on different versions never meet.
- A game's password is checked by the host's game, never by the server; the server only knows that a game is locked.
- The server forwards game packets without reading them, so it does not need updating when the game's protocol changes. Traffic is not encrypted.

## Building from source

Needs CMake 3.20+ and a C++20 compiler; only the `enet6` submodule is used:

```
git submodule update --init mods/multiplayer-native/lib/enet6
cmake -S relay-server -B build-relay
cmake --build build-relay --config Release --target rs64_relay
```

`relay_core_test` (also run by `rs64_unit_tests`) tests the protocol, limits, console and config handling.
