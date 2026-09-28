# Input recordings

Recorded controller-0 input for repeatable test runs. A recording starts at the first input poll after launch, so it includes the menu navigation; launch without `ROGUESQ_BOOT_TARGET`.

| File | Contents |
|---|---|
| `kessel.rec` | Boot, menus, Prisons of Kessel: flying past the searchlight towers and fighting (frame-interpolation checks) |

## Record

```powershell
$env:ROGUESQ_INPUT_RECORD = 'E:\Projects\RogueSquadron64Recomp\tools\input-recordings\kessel.rec'
.\build\Release\RogueSquadron64Recomp.exe
```

Play from the title screen, then close the window.

## Replay

```powershell
$env:ROGUESQ_INPUT_REPLAY = 'E:\Projects\RogueSquadron64Recomp\tools\input-recordings\kessel.rec'
.\build\Release\RogueSquadron64Recomp.exe
```

Replay overrides all controller-0 input from launch; the last recorded input holds after the file ends. Use the same build (Release) and the same env settings as the recording.

## Format

Lines are `<ms> <vi> <poll> <buttons hex> <stick x> <stick y>` after a version header.

- **v4** (written now): one line per input poll, full float precision. Replay serves the recorded sample nearest the current wall-clock time, so every input (mouse steering included) lands within half a poll (~8 ms) of when it was recorded and no lag builds up. The recorded stick values already include the mouse smoothing, so replay matches the live feel without changing controls.
- **v3** (older files, e.g. the current `kessel.rec`): written only on change and replayed by wall-clock `ms`, with the stick blended between close samples. Mouse steering drifts slightly between runs.

Replay logs `[input-rec] ms=… vi=… recorded_vi=… poll=… recorded_poll=…` every 10 s and closes the game when the recording ends. The game is not fully deterministic, so a long replay can still diverge from the recorded run.
