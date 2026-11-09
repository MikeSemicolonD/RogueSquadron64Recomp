# WebMCP tools for AI agents

[WebMCP](https://developer.chrome.com/docs/ai/webmcp) lets a page offer tools to an AI agent in the browser.

With the RS64 Explorer's tools, an agent can see what's open, capture the view, and make the same edits you can.

The save editor has its own tools for reading and changing saves.

Each agent edit is one undo step and saves with your project. Exporting stays your click.

## Turning it on

Only Chrome has it so far, behind a flag (tested in Chrome 156):

1. In `chrome://flags`, set **WebMCP** to **Enabled** and relaunch.
2. Open the explorer and load your ROM.
3. Use an agent or extension that calls WebMCP tools. The **Model Context Tool Inspector** extension lists and calls them by hand.

## Available tools

| Area | Tools | What an agent can do |
| --- | --- | --- |
| Looking around | `rs64_status`, `rs64_list`, `rs64_search`, `rs64_open`, `rs64_selection`, `rs64_view`, `rs64_view_capture` | See what's open, list and search content, open anything, move and capture the view |
| Levels | `rs64_level_objects`, `rs64_level_add_object`, `rs64_level_add_enemy`, `rs64_level_set_object`, `rs64_level_delete_object`, `rs64_level_rename_object`, `rs64_level_links`, `rs64_level_set_link`, `rs64_level_set_path_points` | Place, move, rename and delete objects and enemies; set waves, paths, camera shots and songs |
| Terrain | `rs64_terrain_get`, `rs64_terrain_brush` | Read and brush the ground |
| Rules | `rs64_rules_get`, `rs64_rules_set` | Read and write level rules |
| Custom levels | `rs64_mod_level_new`, `rs64_mod_level_get`, `rs64_mod_level_set`, `rs64_crawl` | Make a level, change its settings and crawl |
| Cutscenes | `rs64_cutscene`, `rs64_cutscene_play`, `rs64_cutscene_keys`, `rs64_cutscene_set_key`, `rs64_cutscene_delete_key`, `rs64_cutscene_add_actor`, `rs64_cutscene_add_event`, `rs64_cutscene_delete_event`, `rs64_cutscene_edit`, `rs64_cutscene_adopt_object` | Play, add actors and events, key cameras and actors |
| Menus | `rs64_menu`, `rs64_menu_add_button`, `rs64_menu_set_button`, `rs64_menu_delete_button`, `rs64_menu_title`, `rs64_menu_text` | Read menus, change buttons, titles and text |
| Lights | `rs64_lights`, `rs64_lights_set_preset` | Read and set light presets |
| Project | `rs64_history`, `rs64_export_plan` | Undo, redo, preview the export |
| Save editor | `rs64save_status`, `rs64save_read`, `rs64save_bytes`, `rs64save_load`, `rs64save_set_pilot`, `rs64save_pilot_slot`, `rs64save_set_elite`, `rs64save_set_settings`, `rs64save_set_unlocks` | Read a save; change pilots, Elite Rogues, settings and unlocks |

Editing tools are marked as edits, so an agent can ask first. A refused tool says why.

## Example prompts

- *"Open Ambush at Mos Eisley and show me the level from above."*
- *"Make the trigger area `bridge_entry` send in a wave of four TIE fighters."*
- *"Raise a low hill around the player start, then show me."*
- *"Find every voice line that mentions Wedge."*
- *"What would Export mod make right now?"*
