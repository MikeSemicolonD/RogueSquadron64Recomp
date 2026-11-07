# WebMCP tools for AI agents

[WebMCP](https://developer.chrome.com/docs/ai/webmcp) lets a web page offer tools to an AI agent working in the browser. The RS64 Explorer offers 44 of them: an agent can see what's open, look at the 3D view, and make the same edits you can, such as placing enemies, writing rules, shaping terrain, keying a cutscene or adding a menu button. The save editor page offers 9 more for reading and changing a save file.

Every edit an agent makes is one step in the explorer's undo history, so **Undo** takes it back like any of your own, and it is saved with your project. Exporting the mod stays your click.

## Turning it on

WebMCP is new. So far it is in Chrome behind a test flag (the explorer's tools were tested in Chrome 156):

1. In Chrome, open `chrome://flags`, search for **WebMCP**, set it to **Enabled** and relaunch Chrome.
2. Open the RS64 Explorer and load your ROM. The tools register when the page loads.
3. Use an AI agent or browser extension that can call a page's WebMCP tools. The **Model Context Tool Inspector** extension lists the tools the page offers and can call them by hand, which is a quick way to check it works.

Other browsers don't have WebMCP yet. Without it the explorer works as usual; the tools just aren't offered.

## Available tools

| Area | Tools | What an agent can do |
| --- | --- | --- |
| Looking around | `rs64_status`, `rs64_list`, `rs64_search`, `rs64_open`, `rs64_selection`, `rs64_view`, `rs64_view_capture` | See what's open, list levels, cutscenes, models, audio and menus, search voice lines and object names, open anything, move the camera and take a picture of the view |
| Levels | `rs64_level_objects`, `rs64_level_add_object`, `rs64_level_add_enemy`, `rs64_level_set_object`, `rs64_level_delete_object`, `rs64_level_rename_object`, `rs64_level_links`, `rs64_level_set_link`, `rs64_level_set_path_points` | List and place objects and enemies, move, rename and delete them, set waves, paths, camera shots and songs, and edit flight paths |
| Terrain | `rs64_terrain_get`, `rs64_terrain_brush` | Read ground heights and raise, lower, flatten, smooth or paint the ground |
| Rules | `rs64_rules_get`, `rs64_rules_set` | Read and write a level's "when this happens, do that" rules |
| Custom levels | `rs64_mod_level_new`, `rs64_mod_level_get`, `rs64_mod_level_set`, `rs64_crawl` | Start a new level and change its settings and opening crawl |
| Cutscenes | `rs64_cutscene`, `rs64_cutscene_play`, `rs64_cutscene_keys`, `rs64_cutscene_set_key`, `rs64_cutscene_delete_key`, `rs64_cutscene_add_actor`, `rs64_cutscene_add_event`, `rs64_cutscene_delete_event`, `rs64_cutscene_edit`, `rs64_cutscene_adopt_object` | Play and scrub a cutscene, add actors and events, key cameras and actors, and turn a level object into an actor |
| Menus | `rs64_menu`, `rs64_menu_add_button`, `rs64_menu_set_button`, `rs64_menu_delete_button`, `rs64_menu_title`, `rs64_menu_text` | Read a menu, add and change buttons, titles and translated text |
| Lights | `rs64_lights`, `rs64_lights_set_preset` | Read and set ray-traced light presets |
| Project | `rs64_history`, `rs64_export_plan` | Undo or redo, and see what **Export mod** would make |
| Save editor | `rs64save_status`, `rs64save_read`, `rs64save_bytes`, `rs64save_load`, `rs64save_set_pilot`, `rs64save_pilot_slot`, `rs64save_set_elite`, `rs64save_set_settings`, `rs64save_set_unlocks` | Read a save, and add, move or change pilots, the Elite Rogues list, settings and cheat unlocks |

Tools that change something are marked as edits, so an agent can ask before using them. When a tool can't do what was asked, it says why in plain words (for example, that the mission's script needs an object), and the agent can try another way.

## Example prompts

- *"Open Ambush at Mos Eisley and show me the level from above."* The agent opens the level, moves the view camera and captures a picture.
- *"In my level, make the trigger area `bridge_entry` send in a wave of four TIE fighters."* It adds the enemies, puts them in a wave and links the wave to the trigger area.
- *"Raise a low hill around the player start, then show me."* It finds the player start, makes a few brush strokes and captures the view.
- *"Find every voice line that mentions Wedge."* It searches the game's subtitles and lists the lines with their numbers.
- *"What would Export mod make right now?"* It lists each mod the export would write and the changed files in it, without saving anything.
