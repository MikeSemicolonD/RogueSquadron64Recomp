Rogue Squadron 64 Recompiled
============================

This package does not include the game. You need your own legally obtained copy of
Star Wars: Rogue Squadron (N64, USA v1.0) as a .z64 ROM.

1. Put the ROM in this folder and name it rogue_squadron.z64.
2. Run RogueSquadron64Recomp (Windows) or ./run.sh (Linux) from this folder.
   The game looks for the ROM in the working directory, not next to the executable.

The ROM is checked on load; other regions or revisions are rejected.

Bundled mods are in mods/; turn them on and off in the Mods panel (F6).
Online co-op comes from the Multiplayer mod (multiplayer-native), which is on by default and must stay enabled.

Co-op over Steam (Steam Relay mod, on by default): with Steam running, HOST GAME shows a 6-digit join code
and the other player enters it under JOIN GAME; no port forwarding is needed. Steam friends can also join from
the Steam friends list while the game is running. Steam shows the game as Spacewar (app id 480).
The Steam API library is not included. Download the Steamworks SDK version 1.65
(https://partner.steamgames.com/downloads/list, any Steam login) and copy into mods/steam-relay/:
  Windows: redistributable_bin/win64/steam_api64.dll
  Linux:   redistributable_bin/linux64/libsteam_api.so   (native Steam package; Flatpak/Snap Steam is not supported)
  macOS:   redistributable_bin/osx/libsteam_api.dylib    (then: xattr -d com.apple.quarantine mods/steam-relay/libsteam_api.dylib)
On Steam Deck, add the Linux build as a non-Steam game. Without the library, or with Steam closed, the
MULTIPLAYER page says why and co-op uses direct connections. Steam on Linux, Steam Deck and macOS is untested.

Linux needs a working Vulkan driver and glibc 2.39 or newer (Ubuntu 24.04 or equivalent).

Source and issue tracker: https://github.com/MikeSemicolonD/RogueSquadron64Recomp
