# Make a model mod

A model mod changes how something looks: a ship's paint, a building's texture, or a whole new model for one of the player's crafts.

## Find the model

Open the **Models** tab and type in the Filter box to find a model. The left panel has its options:

- **LOD**: the less detailed versions the game shows from far away. Each one has its own textures, so check every LOD after you repaint something.
- **Animation** and **Anim speed**: play the model's animations, such as an AT-AT walking.
- **S-foils**, **Speed**: open and close an X-wing's wings, and see the engine glow at different speeds.
- **Textures**, **Wireframe**, **Filtering**, **Lighting**: how the view draws it. *Game (prelit)* matches the N64.

You can also find a model in a level: right-click a placed object and open it in the Models tab. A **Back** button takes you back to the level.

## Repaint a texture

1. Right-click a textured part of the model.
2. Pick **Download texture PNG** to get the current texture as a starting point.
3. Paint over it in any image editor and keep the same width and height.
4. Right-click the same part again and pick **Replace texture from image…**.

The explorer scales your image to the texture's size and converts it to the game's format. Many textures use a small palette of colours, so subtle gradients can come out banded. Check the result in the view before you export.

![Right-clicking the X-wing opens a card with Replace texture from image](images/model-texture-card.png)

### Texture tips

- **Check the format first.** The card shows it under **Format**. *CI4* has 16 colours and *CI8* 256, picked from a palette. *RGB16* and *RGB(A)32* hold any colour.
- **Palette textures pick their own colours.** For a CI4 or CI8 texture the explorer chooses the 16 or 256 colours that fit your image best. Flat areas and bold shapes come out cleanly, while soft gradients and photos band. Painting with few colours, or reducing the image to 16 colours yourself, gives you control over which ones survive.
- **Some textures share a palette.** When another texture in the same model uses the same colours, the explorer keeps them and paints your image with the nearest ones. Stay close to the original's colours there, or other textures would change too.
- **Transparency is kept.** Where the original has see-through pixels, make those pixels transparent in your image.
- **Keep the size and shape.** Your image is stretched to the texture's width and height, so match its proportions.
- **One file, many models.** A level's buildings and props often come from one shared package, where several objects use the same texture. Replacing it changes all of them. The same texture in another level's package is a separate copy, so change that one too if it should match.
- The Naboo Starfighter's textures are encrypted in the game and can't be replaced yet.

## Replace a player craft with your own model

1. In the Models tab, open a player craft (for example the X-wing).
2. Click **Import craft model (.glb)…** and pick your model. Blender exports `.glb` with *File > Export > glTF 2.0*.

What happens:

- Your model is scaled to the size of the craft it replaces.
- The craft's engines, engine glows and canopy stay.
- To keep a part moving (wings, landing gear), name the mesh in your model after the craft's part, for example `wngrot_a` for an X-wing wing. Hover the import button for the tooltip.
- Each of your textures takes one of the craft's free texture slots and is scaled to that slot's size.

### Find a craft's moving parts

Right-click a part of the craft. A part that moves has a **Joint** row with its name, such as `wngrot_a` and `wngrot_b` for the X-wing's two wing pairs. **Export .glb** keeps these names, so the exported craft in Blender shows which part goes with each joint.

### Budgets

The stock crafts have about 300 to 530 triangles (the X-wing 504, the Millennium Falcon 530, the V-wing 302) and 14 to 22 small textures, most under 100 pixels on a side. Stay close to that: the game runs in the N64's memory, and a much heavier model can run out of room in busy levels. The model info box shows the triangle count after an import.

Textures are given slots in order of use: the texture on the most triangles gets the first slot. When your model has more textures than the craft has slots, the extra ones share the last slot, so pack small details into one image (an atlas) instead.

### Example: a simple craft from Blender

1. In Blender, model the body and give it a material with a colour or texture. Point the nose toward the **front** view (Numpad 1), Blender's -Y.
2. Make each wing pair a separate object named after the X-wing's joints: `wngrot_a` for one pair and `wngrot_b` for the other. Their origins don't matter.
3. Select everything and export with *File > Export > glTF 2.0*, format **glTF Binary (.glb)**, **Selected Objects** ticked.
4. In the explorer, open `data/pl_crafts/xwing`, click **Import craft model (.glb)…**, pick the file and answer **+Z** for the nose.
5. Play an animation that opens the wings (or fly it in the game): your wing objects move with them, and the X-wing's engine glows stay at the back.

## Use a game model in Blender

**Export .glb** saves what is shown (the current object, LOD and markers) as a `.glb` you can open in Blender, for reference or as a starting point for your own model.

## Ray-traced lights on a model

Right-click a part of the model and pick **Add light here** to give it a light in the game's ray-traced mode, such as a lamp or an engine glow. Drag the bulb to move it, and change its colour and size in the lights panel.

The **Ray-traced lights** panel lists the lights by *preset*: a group of lights tied to what they light, here the model part they ride on (its **Mesh**). The same panel shows a level's lights in the Levels tab and an effect's in the Effects tab.

For each light:

- **Colour**, and **Strength** for how bright (above 1 is brighter than white).
- **Radius**: how far it reaches. Tick **Radius in camera units** for models the game draws at different sizes, such as laser bolts, so the reach stays the same on screen.
- **Falloff**: how quickly it fades toward the edge of its reach (the game's own lights mostly use 2).
- **X**, **Y**, **Z**: where it sits. Dragging the bulb sets them too.
- **Shadow offset**: anything closer to the light than this doesn't block it, so the light's own model doesn't shadow it.
- **Flicker**: 0 for a steady light, up to 1 for one that dims at random.
- **Cone** and **Cone inner** make it a spotlight shining along **Beam X / Y / Z**. A cone of 0 is a plain light that shines all around.

The box beside a heading switches that light or preset off, and the bin at the right deletes it. The buttons at the top of the panel:

- **Lights from hardpoints**: a light behind each of the model's glow points (engine nozzles, lamps).
- **Open lights file**: brings in the lights from another `roguesq_lights.json`, such as another mod's.
- **Game's lights** (Chrome and Edge): shows the lights already in the game folder under yours. Changing one makes your own copy that replaces it; deleting one switches it off in your mod.

The view shows the lights as you edit them, close to how the game draws them: the 32 lights nearest the camera, added on top of the model's own shading. In the game they show with ray-traced lights turned on, which needs a graphics card that supports ray tracing.

**Export mod** writes the lights to the mod's `roguesq_lights.json`. The game loads its built-in lights first, then each enabled mod's in the order of the Mods list, and a preset replaces an earlier one with the same name. A running game re-reads the files within a second of a change, so after editing lights, **Play in game** again updates them without restarting the game.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing).

## Not yet possible

- Adding your model as a **new** ship to pick, with its own hangar shot. An imported model replaces an existing player craft.
- Importing a model for anything other than a player craft (enemies, buildings). Their textures can still be replaced.
