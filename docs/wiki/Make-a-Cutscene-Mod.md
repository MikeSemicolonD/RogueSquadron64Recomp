# Make a cutscene mod

Each level has an intro and an ending cutscene, and the chapter levels open with a scrolling crawl. You can edit all of them.

## Open a cutscene

1. Open the **Levels** tab and pick a level.
2. Pick from the **Cutscene** list: the intro, the ending, the medal ceremony, or the camera shots the mission cuts to.
3. It plays with its voices, music, sound and fog. The checkboxes under the timeline turn each one off. **Camera** off lets you fly around freely while it plays.

**Player craft** shows the intro with a different ship, and **Medal tier** shows the ending ceremony for each medal.

## Edit it

Click the **pencil** on the timeline to start editing. A separate window (the inspector) opens for the parts of the cutscene.

### The timeline

- Drag a clip to move it, and drag its ends to trim it.
- Drag voice lines, sounds and music onto the timeline. Voice lines are listed by what they say.
- **Event…** adds an event of the chosen kind at the playhead.
- **Duration** makes the cutscene longer (empty time at the end) or shorter (cuts the end).

### Actors

- **Actor…** adds a model: any model from the game's cutscene list, or a player craft.
- **Model** swaps the picked actor's model.
- **Shoot at…** makes the actor turn to a target and fire. The target blows up when the shot hits.

### Cameras and movement

1. Pick a camera or an actor from **Pose…**.
2. Move the playhead to a time, then drag the handles (W move, E rotate, R scale). That saves its pose at that time as a *key*.
3. Keys at different times become a smooth path. They show on the **Keys** row, where you can drag or delete them.

**V** snaps a camera to your current view. Untick **Camera**, fly to the shot you want, and press V. A camera's **Mode** can follow an actor, aim at one, or ride inside one. A cutscene can have up to 6 cameras.

**Start from blank** removes every actor, camera move, sound and voice line, keeping one still camera. Undo brings everything back.

> TODO: screenshots of the timeline and inspector, a worked example (a new fly-by shot).

## Opening crawls

Chapter levels (the first level of each chapter) and custom levels have **Opening crawl** in their Cutscene list. It plays like a cutscene and opens a crawl window:

- Write the **chapter line**, **title** and **story text**. A blank line starts a new paragraph. The crawl's font has letters, digits and common punctuation; anything else is left out.
- Pick the **planet** the camera pans down to, or right-click the backdrop to use your own image. The explorer converts it to the format the game can read.
- With the pencil on, pick **Camera** to pose the camera like a cutscene camera, and drag the fly-by ships' route points in the view.

For a stock chapter, your crawl replaces that chapter's crawl.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing). **Play in game** with a cutscene open starts the game straight at that cutscene.

## Not yet possible

- In the crawl preview, the fly-by ships don't change with the planet the way they do in the game.
- Building a full intro from a blank cutscene hasn't been tested end to end yet. Expect rough edges.
