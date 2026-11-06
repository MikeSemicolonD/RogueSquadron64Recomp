# Make a level mod

A level mod changes one of the game's missions: the ground, what's placed in it, where enemies fly, and what happens when.

To build a new mission instead, see [Make a custom level](Make-a-Custom-Level). It uses all the tools on this page.

## Open the editor

1. Open the **Levels** tab and pick a level.
2. Click the **pencil** button at the top of the view to start editing. Changes go into your project; nothing touches the game until you export.
3. Pick one of the three edit modes (Tab cycles through them):
   - **Terrain**: reshape and repaint the ground.
   - **Objects**: buildings, turrets, enemies, wingmen and everything else placed in the level.
   - **Events**: flight paths, trigger areas, music regions and the level's rules.

![Ambush at Mos Eisley open in the level editor, in Objects mode](images/level-objects.png)

## Reshape the ground

In Terrain mode, pick a **Tool** and drag on the ground:

![The Terrain mode panel](images/level-terrain.png)

- **Raise**, **Lower**, **Flatten**, **Smooth** reshape it. **Radius** sets the brush size and **Strength** sets how much each stroke changes.
- **Paint texture** paints ground tiles, and **Pick texture** picks up the tile under the cursor to paint with.
- With **Objects follow ground** ticked, things on the ground keep their height above it as you reshape it.

You can also work on a heightmap in an image editor. The download button saves the ground as a grey PNG (white is high), and the upload button loads any image back, scaled to fit the level.

> TODO: the ground tile limit and what the explorer says when you reach it.

## Move, add and remove things

In Objects or Events mode:

- **Click** something to select it, and **right-click** to pick from everything under the cursor.
- **W**, **E**, **R** move, rotate and scale it with the handles. **X** switches between world and local axes. The panel also has number boxes for exact values.
- **Ctrl+D** duplicates it and **Delete** removes it. The explorer won't let you delete something the mission needs (for example the target of an objective), and tells you why.

To add something, use **Add to level** at the bottom of the panel. Search the list, pick an item, then click where it goes (Esc cancels). Items marked *from another level* are copied from that mission. You can also drag a row in by its grip icon.

## Enemies and waves

An enemy can belong to a **wave**: it waits off-stage until something releases it, such as the player entering an area or a target being destroyed. Pick the wave from the enemy's panel, or **New wave…** to start one. TIE fighters and TIE interceptors can be added from other levels.

> TODO: how to release a wave with a rule, and a short example.

## Flight paths

Enemies and wingmen fly along paths. Pick an existing path from the unit's panel. Each path is named after what flies it, and *(then attacks the player)* marks the ones that end in an attack run. **Copy from another level…** brings in a path from another mission. Drag a path's points to reshape it.

## Trigger areas, music and camera shots

In Events mode:

- A **trigger area** fires when something enters it. Check what it's set to react to, especially on triggers copied from another level.
- A **music region** changes the song when the player flies into it. Pick the song by name.
- A trigger can **Play camera shot**: the game cuts to one of the level's cameras the first time the trigger fires, then hands control back while the fight goes on. In online co-op, both players see the shot unless you set it to show only for the player who fired it.

## Rules: "when this happens, do that"

Rules let a level react to the player without any code. Select the thing a rule is about (a trigger area or path in Events mode, a unit in Objects mode) and click **+ Add rule**. Pick **When…** and then what to do.

A rule can start when:

- something enters or leaves an area
- a unit passes a point on a path
- a unit is destroyed, or its health drops below a set amount
- the mission starts, or some time after it starts
- an objective is done or failed

It then runs its actions in order:

- play a voice line or your own recording
- show a message on screen
- change the music
- release an enemy wave
- cut to a camera, or destroy a unit
- win or fail the mission, or mark an objective done or failed
- wait a few seconds before the next action

A rule fires once unless you make it repeat. **Level events…** opens a window with the level-wide rules (mission start, timers, objectives) and a list of every rule in the level. A rule whose object you deleted shows red there, and Export mod warns you about it.

![The Level events window](images/level-events.png)

> TODO: a worked example: "Hold the bridge" message, wave release, win on a destroyed target.

## Lights

Right-click the ground and pick **Add light here** for a ray-traced light in the level. Drag the bulb to move it.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing).

## Not yet possible

- **Drawing a new flight path**, or adding and removing points on one. You can copy a stock path and move its points.
- **Renaming** objects and areas. Copied areas keep their original name and size.
- Adding enemy craft other than TIE fighters and TIE interceptors from other levels.
- Adding new camera positions for camera shots: you can only use the cameras already in the level.
- Making a map larger than the level it started from.
