# Regions

Part of the [Level reference](Level-Events).

Only a region's footprint on the ground counts, not its height.

## High-detail regions

Changes the ground only. Objects are not affected, and the player's position doesn't matter.

- When the level loads, small bumps and dips in the ground here are evened out. Spots rise or fall slightly, which also changes collision.
- On legacy terrain the ground stays at medium detail, near or far. Up close that is less than the finest detail, so "high" is relative. The default terrain ignores this.
- Where a high and a low region overlap, high wins.

## Low-detail regions

Changes the ground only. Objects are not affected, and the player's position doesn't matter.

- When the level loads, bumps and dips here are flattened into straight slopes. Spots can rise or fall by up to a few units, which also changes collision.
- On legacy terrain the ground stays at the lowest detail, near or far. The default terrain ignores this.

Removing either kind of region changes the ground shape, so a level's old recordings stop matching.

## Music region triggers

Changes the music while the player is in this area, using two boxes that act as one item:

- It **starts** when the player enters the inner box.
- It **ends** when they leave the outer box. The gap stops the music flickering at the border.

Most stock regions leave the song alone on entry and switch to an action song once an objective calls for it.

In the panel:

- **On enter** is what happens on entering: a song, **Keep current song** or **Stop music**.
- **Action song** replaces it while **When** and **When count** hold, checked all the time the player is inside. With both set, both must hold. With neither, it never plays.
- **Linger** keeps the action song playing for some seconds after the player leaves.
- **On leave** is what plays on leaving. **Keep current song** brings back the song from before they entered.
- **Exit margin** sets how far past the inner box the player must fly before the region ends.
- **Side offsets** add to the margin on one side only. Stock regions often use them.

Under **More**:

- **Entry song plays** ends the entry song after some seconds (the earlier song returns) or some repeats (**Then** plays next).
- **Fades** set the seconds to fade the old song out and the new one in, separately for entering and leaving.
- **Re-entry delay** makes the region wait after the player leaves before it responds again.
- **Works for** removes the region after the player has left it that many times.
- **Cancels running timers** stops another region's **Entry song plays** timer on entering.
- **Active when** and **Active when count** switch the whole region off until they hold. Two regions in one spot can use this to swap songs as the mission goes on.

Conditions are picked by what sets them: a unit being destroyed, a trigger area, a wave. Some are set only by a stock mission's script, so they show as a number, and **Other number…** enters one directly.

A region with no inner box, usually off the map and named `LevelSettings`, holds songs the mission script plays. Its panel shows just those.

Clicking either box selects both as one object, and moving it moves both. In Scale mode each box has its own corner handles. Resizing the inner box keeps the gaps. Resizing the outer box leaves the inner one alone, and it can't shrink past the inner box. A new one is made with both boxes, and any side close to the map edge reaches past it, so flying along the edge doesn't end the song.

## Ground-unit areas

Troopers and civilians appear and walk around inside this area.

## Roam areas

A unit wanders inside this area and turns back toward the middle when it strays out. The ring in the view is drawn flat, but the area is really a ball, so height counts:

- **Ground units** walk over the terrain, so they only get the part of the ball at ground level. Keep the centre at ground level to use the full radius. If the ground is further from the centre than the radius, they keep turning back, and the panel warns about this.
- **Flying units** use the whole ball.
- **Probe droids** pick random points inside the ball and head for them.

## Other regions

Regions whose purpose isn't known yet.
