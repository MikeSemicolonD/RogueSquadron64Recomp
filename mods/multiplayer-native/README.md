# Multiplayer/Co-op mod

This mod is a work in progress

## Issues

- The other player's tow cable isn't drawn: any AT-AT the client trips WILL falls in both games, but only the player doing the tow cable winding will see the cable and the 'trip' cutscene sequence.

- There are some edge cases where Client and Host can get out of sync if cutscene skips aren't respected across client and host. Like if a host triggers a cutscene skip that the client doesn't recieve or vice versa.

- Selecting a craft that's not allowed in the hangar counts as a 'ship selected' in multiplayer even if the selection of that ship is denied later. The main issue is that this condition is not checked until both players have selected a ship. This immediately causing a desync where the player with an approved craft continues onto the mission while the other player stays in the hangar until they select an approved ship. Message of selection need to be sent after this condition check.
