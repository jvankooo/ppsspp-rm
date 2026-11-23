PPSSPP for Nintendo Switch, with rumble
=======================================

Based on PPSSPP-nx 1.1.1 (NaGaa95). The PSP has no rumble, so this build infers it:
from the game's sound (impacts, explosions), and, for games with a profile, from what
happens in the game (God of War: Chains of Olympus: hits Kratos takes, blows he lands).

INSTALL
-------
Copy the "switch" folder from this package to the root of the SD card. It contains:

  switch/ppsspp-nx/PPSSPP.nro                      the emulator (replaces the old one)
  switch/ppsspp-nx/PSP/RUMBLE/UCUS98653.ini        rumble profile: God of War: Chains of Olympus (USA)

Your settings, saves and games are kept.

Rumble only runs for a game that has a profile in sdmc:/switch/ppsspp-nx/PSP/RUMBLE/
(named after the game's ID, like UCUS98653.ini). Without one, rumble is off for that game.

SETTINGS
--------
In a game: open the pause menu > Rumble. Or Settings > Controls > Rumble.

  Rumble                         on / off
  Rumble strength                None, Light, Medium, Strong (default), Hard - for all games
  Rumble strength for this game  "Use the global setting", or its own
  Sound rumble sensitivity       Off, 1-10 (default 5), for this game. Higher: more impacts rumble
  Hits taken / Blows landed      each kind of rumble on or off, for this game
  Test buttons                   feel the three kinds of rumble

Per-game settings are saved in sdmc:/switch/ppsspp-nx/PSP/RUMBLE/settings/.

No rumble while the game is paused, in its menus, during movies, or in the HOME menu.
Joy-Cons (handheld or detached) and the Pro Controller (player 1) rumble.

MAKING PROFILES
---------------
Settings > Controls > Rumble > "Save RAM snapshot..." (or the "Rumble RAM snapshot" hotkey,
mappable in the controls) saves the game's memory with a label to .../RUMBLE/snapshots/.
The profile format and how to make one for another game: docs/rumble.md in the source.

GRAPHICS BACKEND
----------------
The rendering backend is one setting for all games (it restarts PPSSPP), so the launcher's
per-game settings don't list it. Change it in the launcher: + (Settings) > Graphics >
"Rendering backend".
