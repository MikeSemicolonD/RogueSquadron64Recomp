# RS64 Explorer design rules

The explorer is for modders who don't write code. Every feature is judged by whether it lets a modder do something without code, JSON or knowledge of the game's internals. Keep these rules when adding or changing anything here, and update this file when a new rule is agreed.

## Language

- Speak the modder's language. No addresses, offsets, byte sizes, flag or counter numbers, internal ids or code names in labels, tooltips, errors or dialogs. Show one only when the modder needs it, such as a real limit (a name length, a size cap).
- Pick, don't type. Things are chosen from named lists (songs by name, voice lines by their text, paths by what flies them) or done in the view; the explorer picks the bookkeeping (free flags, counters, unique names) for the modder.
- When the explorer refuses something, say why in plain words (for example, the mission script needs that object).

## Controls

- Few, compact controls. Use an icon with a tooltip instead of a text label wherever the action reads from an icon; the label goes in `title` and `aria-label`. Exception: the edit panel's Global/Local axis toggle stays text.
- Keep a toggle away from anything destructive, so a missed click can't delete. A section's on/off box has no label of its own (the heading is its label) and sits right of the heading; when the heading row also has a delete icon, the box moves left of the heading text and the delete stays alone at the far right. Headings get a divider line above.
- Icons come from `EDIT_ICONS` / `setEditIcon` (levels.js) or `iconButton` (lights.js).
- A big editor gets its own window instead of growing a cramped popup (the Level events window replaced rules inside Level settings).

## Safe editing

- A stray click never adds or changes data. Dragging into the scene or timeline starts only from a row's grip icon (clicking the name still opens it), the drop target is highlighted, and Esc or dropping elsewhere cancels.
- Every change is one undo step. Edits, including WebMCP tools, go through the page's own functions so they land in the undo history.
- Every view of the same data re-renders after any write, so no box keeps a stale copy that overwrites another edit.
- The explorer keeps the game from crashing on what the modder makes: it refuses or repairs data the game can't handle (deleting objects the mission script uses, paths that just end, JPEGs the game can't decode, terrain past the tile budget) instead of exporting it.

## Mods and files

- The modder supplies the ROM; nothing from the game is bundled. Mods built from game files ship patches, not game data.
- Export is always the modder's click. Changes that need each other (a level and the custom ship it places) export together, and the export dialog says so.
- The page runs from `file://` with no build step: plain scripts sharing one scope, loaded in order, with three.js pinned to one version.
