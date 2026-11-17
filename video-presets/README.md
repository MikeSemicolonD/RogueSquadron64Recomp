# Video presets

Ready-made graphics settings. Pick the folder that fits your PC and copy its `roguesq_video.json` next to the game, replacing the old file. Close the game first. Settings a preset leaves out (fullscreen, language, graphics API) go back to their defaults.

To try one without replacing your file, launch with `--preset <name>` (for example `--preset vr`). Changes made in OPTIONS during that run save into the preset.

Where the file goes:

- Windows and the Linux tarball: the game folder (next to `RogueSquadron64Recomp.exe` or `run.sh`).
- macOS: `~/Library/Application Support/RogueSquadron64Recomp`.
- Linux AppImage: `~/.local/share/RogueSquadron64Recomp`.
- Flatpak: `~/.var/app/io.github.MikeSemicolonD.RogueSquadron64Recomp/data`.

| Preset | For | What you get |
| --- | --- | --- |
| [low](low/roguesq_video.json) | Raspberry Pi, phones, old laptops | Native N64 resolution, original terrain, 30 fps, no ray tracing |
| [medium](medium/roguesq_video.json) | Integrated graphics | Window resolution, 60 fps, 2x draw distance, no ray tracing |
| [high](high/roguesq_video.json) | Mid-range graphics cards | Monitor refresh rate, 4x MSAA, 4x draw distance, ray-traced lights and shadows |
| [ultra](ultra/roguesq_video.json) | High-end graphics cards with ray tracing | 4x MSAA, max draw distance and terrain detail, every ray-traced effect |
| [original](original/roguesq_video.json) | The N64 look | 4:3, native resolution, 30 fps, original terrain and draw distance |
| [vr](vr/roguesq_video.json) | VR headsets (VR builds) | Starts in the headset, 2x MSAA, 4x draw distance, no ray tracing; set the headset to 72 or 90 Hz |

Every setting can still be changed in OPTIONS afterwards. If a preset runs below its frame rate, try the one below it, or lower `antialiasing` first since it costs the most. The game uses one anti-aliasing method: with `antialiasing` set, `ssaa` and `fxaa` are ignored. With no monitor attached (a headless Raspberry Pi), keep `vsync` at `false` (as in low) or the game runs at one frame a second.
