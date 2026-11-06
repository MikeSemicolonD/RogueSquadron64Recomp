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

> TODO: tips for palette textures, textures shared by several models.

## Replace a player craft with your own model

1. In the Models tab, open a player craft (for example the X-wing).
2. Click **Import craft model (.glb)…** and pick your model. Blender exports `.glb` with *File > Export > glTF 2.0*.

What happens:

- Your model is scaled to the size of the craft it replaces.
- The craft's engines, engine glows and canopy stay.
- To keep a part moving (wings, landing gear), name the mesh in your model after the craft's part, for example `wngrot_a` for an X-wing wing. Hover the import button for the tooltip.
- Each of your textures takes one of the craft's free texture slots and is scaled to that slot's size.

> TODO: how to find a craft's part names, recommended polygon and texture budgets, a worked Blender example.

## Use a game model in Blender

**Export .glb** saves what is shown (the current object, LOD and markers) as a `.glb` you can open in Blender, for reference or as a starting point for your own model.

## Ray-traced lights on a model

Right-click a part of the model and pick **Add light here** to give it a light in the game's ray-traced mode, such as a lamp or an engine glow. Drag the bulb to move it, and change its colour and size in the lights panel.

> TODO: the lights panel fields, previewing lights, how lights export.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing).

## Not yet possible

- Adding your model as a **new** ship to pick, with its own hangar shot. An imported model replaces an existing player craft.
- Importing a model for anything other than a player craft (enemies, buildings). Their textures can still be replaced.
