# Make a cutscene mod

Edit a level's intro and ending, and a chapter's opening crawl.

## Open a cutscene

1. In **Levels**, pick a level.
2. Pick from **Cutscene**: intro, ending, medal ceremony, or a mission camera shot.
3. It plays with voices, music, sound and fog; the checkboxes under the timeline turn each off. **Camera** off lets you fly freely.

**Player craft** swaps the intro's ship; **Medal tier** picks the ceremony.

## Edit it

Click the **pencil** on the timeline. The inspector window opens.

![The intro of Ambush at Mos Eisley in the cutscene editor, with the timeline at the top and the inspector on the right](images/cutscene-editor.png)

### The timeline

- Drag a clip to move it, its ends to trim it.
- Drag voice lines, sounds and music onto it.
- **Event…** adds an event at the playhead.
- **Duration** lengthens or cuts the end.

### Actors

- **Actor…** adds a cutscene model or player craft.
- **Model** swaps the picked actor's model.
- **Shoot at…** makes it turn and fire; the target blows up on hit.

### Cameras and movement

1. Pick a camera or actor from **Pose…**.
2. Move the playhead and drag the handles (W move, E rotate, R scale) to set a *key*.
3. Keys make a smooth path; drag or delete them on the **Keys** row.

**V** snaps a camera to your view. A camera's **Mode** can follow, aim at, or ride an actor. Up to 6 cameras.

**Start from blank** keeps one still camera and removes the rest (undoable).

### Example: an X-wing flies past the camera

1. Open the Ambush at Mos Eisley intro and click the pencil.
2. **Actor…** → the X-wing player craft.
3. Pick it in **Pose…**. At 0.5 s, move it to one side (W) and turn its nose along its route (E).
4. At 1.5 s, move it to the other side. Keep the turn, or it rolls.
5. Tick **Camera** and play. Closer keys fly faster.

   ![The new X-wing crossing the opening shot of the Mos Eisley intro, seen through the cutscene camera](images/cutscene-flyby.png)

6. Optional: drop an engine sound at 0.5 s.

To track it, set a camera's **Mode** to aim at the X-wing.

## Opening crawls

Chapter and custom levels have **Opening crawl** in the Cutscene list:

- Write the **chapter line**, **title** and **story text**; a blank line starts a paragraph. Unsupported characters are left out.
- Pick the **planet**, or right-click the backdrop to use your own image.
- With the pencil on, **Camera** poses the camera; drag the fly-by ships' route points.

A stock chapter's crawl is replaced by yours.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing). **Play in game** starts at the open cutscene.

## Not yet possible

- The crawl preview's fly-by ships don't change with the planet.
- A full intro built from blank is untested.
