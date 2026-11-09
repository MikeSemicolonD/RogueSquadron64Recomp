# Make a level mod

Change a mission's ground, objects, enemy routes and events. For a new mission, see [Make a custom level](Make-a-Custom-Level).

## Open the editor

1. In **Levels**, pick a level.
2. Click the **pencil** at the top of the view. Nothing touches the game until you export.
3. Pick an edit mode (Tab cycles):
   - **Terrain**: shape and paint the ground.
   - **Objects**: buildings, turrets, enemies, wingmen and the rest.
   - **Events**: flight paths, trigger areas, music regions and rules.

![Ambush at Mos Eisley open in the level editor, in Objects mode](images/level-objects.png)

## Reshape the ground

In Terrain mode, pick a **Tool** and drag:

![The Terrain mode panel](images/level-terrain.png)

- **Raise**, **Lower**, **Flatten**, **Smooth**, with **Radius** and **Strength**.
- **Paint texture** paints the picked swatch. **Pick texture** or **Alt+drag** picks one up.
- **Brush**: soft or hard, round or square. Flatten's **Height** sets the level (empty: where you start).
- Keys: **1**-**6** tools, **[** **]** brush size, **Shift+drag** swaps Raise and Lower.
- **Objects follow** keeps things on the ground. **Show tiles** outlines tiles.

The download button saves a grey heightmap PNG (white is high); upload loads any image back.

A level holds 8191 different ground tiles; the bar shows use. Unused tiles are dropped near the limit, and a stroke past it is undone. Smoother shapes use fewer.

## Move, add and remove things

In Objects or Events mode:

- **Click** to select; **right-click** picks from everything under the cursor.
- **W**, **E**, **R** move, rotate, scale; **X** toggles world / local axes. The panel has exact values.
- **Name** renames it; rules, camera shots and paths follow. The player start and names a stock script uses can't change.
- **Ctrl+D** duplicates, **Delete** removes (refused, with the reason, if the mission needs it).
- **Used by** lists the objectives and rules using it.

**Add to level** (bottom of the panel): pick an item and click where it goes, or drag it in by its grip. Enemies keep placing until **Esc**. *From another level* items are copied from that mission. A **Camera** is added at your view.

## Enemies and waves

An enemy in a **wave** waits off-stage until released. Pick the wave in its panel, or **New wave…**. TIE fighters and interceptors can come from other levels.

A wave is released:

- **By a trigger area**: its **Start wave** box in Events mode.
- **By a rule**: the **Spawn** action. It starts with your last kind, count (up to 8) and path; change them or click a path in the view with the pin, then **Create wave**.

## Flight paths

Pick a path in the unit's panel. *(then attacks the player)* marks paths ending in an attack run. **Copy from another level…** or **New path** add one; both end in an attack, since a path leading nowhere crashes the game.

In Events mode, drag a path's points (**Shift+drag**: height, **Ctrl+click**: add after, **Alt+click**: delete, minimum 4). Craft fly from the second point to the second-last.

## Trigger areas, music and camera shots

In Events mode:

- A **trigger area** fires when what **Set off by** names (the player or a unit) comes within its **Radius**. Copied triggers start set off by the player, radius 10 or more.
- A **music region** changes the song when the player enters.
- **Play camera shot** cuts to a level camera the first time the trigger fires. In co-op, both players see it unless set to the one who fired it.

## Rules: "when this happens, do that"

Select a trigger area, path or unit and pick from **+ Add rule**:

- something enters or leaves an area
- a unit passes a path point
- a unit is destroyed or drops below some health
- the mission starts, or a time after
- an objective is done or failed

Pick actions from **+ Add action**. They run in order:

- play a voice line or recording
- show a message
- change the music
- release a wave (**Spawn**)
- cut to a camera, or destroy a unit
- win or fail the mission or an objective
- wait

A rule fires once unless set to repeat. **Level events…** holds level-wide rules and lists every rule; one whose object is gone shows red, and Export mod warns.

![The Level events window](images/level-events.png)

### Example: hold the bridge

On a copy of Ambush at Mos Eisley:

1. In Events mode, name a trigger area on the way in `bridge_entry`, set off by *The player*.
2. **+ Add rule**: *Something enters this area*.
3. **+ Add action**: *Show message*, `Hold the bridge!`, 4 seconds.
4. **+ Add action**: *Spawn*, pick a path, **Create wave**.
5. In Objects mode, name a probe droid `relay_droid`.
6. **+ Add rule**: *This unit is destroyed*, **+ Add action**: *Win the mission*.

![The bridge_entry rule: a message, then a wave of four TIE fighters](images/level-rules-example.png)

A custom level can use a [Destroy units objective](Make-a-Custom-Level#objectives) instead.

## Lights

Right-click the ground, **Add light here**; drag the bulb to move it.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing).

## Not yet possible

- Enemy craft other than TIE fighters and interceptors from other levels.
- A map larger than the original.

Untested in game: new and edited paths, a copied camera in a camera shot, a unit in **Set off by** and *Play recording*.
