# Paths

Part of the [Level reference](Level-Events).

## Flight paths

The route a unit flies. The legend lists one row per kind of unit (AT-AT, TIE fighter, Wingmen and so on), but they are all the same thing: only who flies it differs.

- Pick a path in the unit's panel.
- Craft fly from the second point to the second-last.
- A path must end in an attack run, since a path leading nowhere crashes the game.
- Cloud cars and trains must have a path. When placing one, click a path to put it on that path, or the ground to make a new path there.
- Wave skimmers and World Devastators each get their own path, which ends in a stop order. Removing that order crashes the game when they reach the end.
- A train is placed and deleted as a whole.

## Unassigned paths

Paths nothing flies yet. A mission script may switch a unit onto one. The cross marks the start.

## Path events

These sit on a path and act on the unit flying it as it passes.

### Speed changes

The unit eases to a new speed.

### Hold orders

The unit stops until a timer runs out and an objective lets it go on.

### Attack orders

The unit picks a new attack target (red dashed arrow). Set it under **Target** in the selection panel, or click the place button there and then the unit in the view.

### Craft actions

TIE bombers start a bombing run, shuttles land or fold their wings, wingmen play an effect.

### Fly-to orders

The unit leaves its path and flies toward a region, an area or the player. Set it under **Target** the same way as an attack order. TIE fighters and the boss shuttle only.

### Weave changes

Changes how much the craft sways side to side from here on.

### Landing pitch

The craft tilts up to land, or levels out to take off, over a few seconds.
