# Video presets

Ready-made graphics settings. Pick the folder that fits your PC and copy its `roguesq_video.json` next to the game, replacing the old file. Close the game first. Settings a preset leaves out (fullscreen, language, graphics API) go back to their defaults.

On Windows the file sits next to `RogueSquadron64Recomp.exe`. On Linux and macOS it is in the saves folder listed in [building-all-platforms.md](../docs/guides/building-all-platforms.md#what-players-need).

| Preset | For | What you get |
| --- | --- | --- |
| [low](low/roguesq_video.json) | Raspberry Pi, phones, old laptops | Native N64 resolution, 30 fps, no ray tracing |
| [medium](medium/roguesq_video.json) | Integrated graphics | Window resolution, 60 fps, 2x draw distance, no ray tracing |
| [high](high/roguesq_video.json) | Mid-range graphics cards | Monitor refresh rate, 4x MSAA, 4x draw distance, ray-traced lights and shadows |
| [ultra](ultra/roguesq_video.json) | High-end graphics cards with ray tracing | 2x SSAA, max draw distance and terrain detail, every ray-traced effect |
| [original](original/roguesq_video.json) | The N64 look | 4:3, native resolution, 30 fps, original terrain and draw distance |

Every setting can still be changed in OPTIONS afterwards. If a preset runs below its frame rate, try the one below it, or lower `ssaa` and `antialiasing` first since they cost the most.
