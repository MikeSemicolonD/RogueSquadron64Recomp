# Setting up online co-op

Co-op is for two players, and both need the same version of the game. Start it from the main menu: MULTIPLAYER, pick a pilot, then HOST GAME or JOIN GAME.

Playing on the same network needs no setup. Playing over the internet depends on the host's router, and there is no official server, so one of the routes below has to work for you.

| Route | What it needs | Try it when |
| --- | --- | --- |
| [Same network](#same-network) | Both players on one network | You are in the same building |
| [Direct](#direct-over-the-internet) | A router that opens the port by itself, or a port you forward | First choice over the internet |
| [VPN](#vpn) | Both players install the same VPN app | The port cannot be opened |
| [Steam](#steam) | Both players on desktop, with Steam and a downloaded library | Both of you use Steam |
| [Relay server](#relay-server) | Someone runs `rs64_relay` at a public address | You play with a group, or nothing else works |

**HOST GAME picks the route by itself:** Steam if it is ready, else the relay server if the address row holds one that answers, else direct. The line under the status shows which one you got. A friend joining by typed address connects directly or to a relay server, so if your game is on Steam they need Steam too.

To stop using a relay server, put any other address in the address row. To stop using Steam, close Steam or turn the mod off.

## The address row

Press A on the address row to edit it. Type on a keyboard, or use the pad: left and right pick a number, up and down change it. A or B finishes. Names and `:port` need a keyboard.

## Same network

One player presses HOST GAME, the other JOIN GAME. JOIN GAME finds the host by itself, unless an address has been set in the address row.

If it does not, type the host's `YOUR LAN ADDRESS` into the address row and press JOIN GAME again. Guest Wi-Fi networks often keep devices from seeing each other.

## Direct over the internet

The host presses HOST GAME and waits a few seconds, then reads the line under the status:

- `YOUR NET ADDRESS`: the router opened the port. Send that address to your friend, who types it into the address row and presses JOIN GAME.
- `YOUR LAN ADDRESS`: the game could not open the port. The router may have UPnP turned off or may have refused, or your internet provider may share your address with other customers. Forward the port by hand, below.

`YOUR NET ADDRESS` does not prove your friend can get in. If they see COULD NOT CONNECT TO HOST, check the firewall step below.

### Forward the port by hand

1. In your router's settings, forward **UDP port 27064** to the address the game shows as `YOUR LAN ADDRESS`.
2. Allow the game through your firewall. Windows usually asks the first time you host.
3. Look up your public address on any "what is my IP" site and send it to your friend. The game keeps showing `YOUR LAN ADDRESS`, which is expected when you forward by hand.

**If it still fails,** compare the internet (WAN) address on your router's status page with the one from that site. If they differ, or the router's starts with `100.64` to `100.127`, your provider shares your address and forwarding cannot work. Ask the provider for a public address, or use another route. Having your friend host may also work.

To use a different port, add it to the address on both sides (`203.0.113.5:27100`) and forward that port instead.

## VPN

Both players install the same VPN (Tailscale, ZeroTier or Hamachi) and join one network. Neither has to touch a router.

1. The host types their VPN address into the address row and presses HOST GAME.
2. The friend types the same address and presses JOIN GAME.

## Steam

Desktop only. Each player needs to:

1. Copy the Steam API library into `mods/steam-relay/` (which file and where to get it are in the [README](../../README.md#multiplayer-experimental)).
2. Press F6 in the game, turn on **Steam Relay** in the Mods window, and restart the game. It is off by default.
3. Keep Steam running.

The host presses HOST GAME. The friend presses JOIN GAME and picks the game from the list. Steam shows you as playing Spacewar, which is expected.

## Relay server

A relay server lists games and passes traffic between the two players, so neither of them forwards a port. Someone has to run one on a machine players can reach, such as a rented server or a home PC with UDP port 27070 forwarded: see [relay-server.md](relay-server.md).

Each player types the server's address into the address row. While it is in the row, HOST GAME opens a game there and JOIN GAME lists its games. The row keeps the last address you used, so the server is still there next launch unless you used another address since.

## On-screen messages

| Message | What to do |
| --- | --- |
| COULD NOT CONNECT TO HOST | Check the address, and that the host is still on HOST GAME with the port open and the firewall allowing the game |
| COULD NOT CONNECT TO SERVER | The relay server is down, or its address is wrong |
| VERSION MISMATCH | Both players need the same version |
| CANNOT HOST | The game could not open its port. Close any other copy that is hosting |
| NO NETWORK ADDRESS | This device is not connected to a network |
| PASSWORD NEEDED, WRONG PASSWORD | The address row now takes the 6-digit password. Enter it to join again |
| GAME HAS NO PASSWORD | A password was sent to a game without one. Press JOIN GAME again to join without it |
| GAME FULL | That game already has two players |
| RELAY SERVER FULL, TOO MANY CONNECTIONS | The server is at its limit. Try again later |
| STEAM API DLL MISSING | The library is not in `mods/steam-relay/` |
| STEAM API DLL WRONG VERSION | Use the SDK version named in the README, and update Steam |
| STEAM API DLL BLOCKED | The system refused to load it. On macOS, remove the download quarantine (see the README) |
| STEAM IS NOT RUNNING | Start Steam, then restart the game |
| STEAM FLATPAK NOT SUPPORTED | Use the native Steam package on Linux |
