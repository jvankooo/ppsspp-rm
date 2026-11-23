# Rumble for PSP games

The PSP has no rumble motor, so no PSP game ever asks for vibration. This build infers it, in two layers:

1. **Sound** (any game with a profile): PPSSPP mixes the game's audio itself. A detector on the mix of the
   sound effects fires on loud, sudden bursts below about 150 Hz (impacts, explosions, heavy landings),
   measured against their recent average, so steady sound (an engine, a drone) doesn't trigger it. Music is
   left out: a sceAudio channel that plays what sceAtrac or sceMp3 decoded counts as music. The rumble is
   timed with when those samples play, not when they are mixed.
2. **Game state** (per game): values read from the game's memory once per vblank (health, a hit counter,
   speed, a pause flag), and rules that turn their changes into rumble: health dropped, rumble scaled by the
   loss; hit counter rose, a short tap; and so on.

Rumble only runs for a game that has a **profile**: `PSP/RUMBLE/<DISC_ID>.ini` on the memory stick. On the
Switch that is `sdmc:/switch/ppsspp-nx/PSP/RUMBLE/`. Without a profile, the game gets no rumble and the
per-game settings are disabled.

The code: `Core/Rumble/Rumble.cpp` (profiles, rules, sound detector, effect mixer, research tools),
`SDL/SwitchRumble.cpp` (HD Rumble output through libnx, handheld and player 1), hooks in
`Core/HLE/__sceAudio.cpp`, `sceAtrac.cpp`, `sceMp3.cpp`, `sceMpeg.cpp`, `scePsmf.cpp`, `sceDisplay.cpp`, and
the settings in `UI/Settings/ControlsPage.cpp`.

## Settings

Settings > Controls > **Rumble**, or **Rumble** in the pause menu (shown for games with a profile).

| Setting | Where it's kept | What it does |
|---|---|---|
| Rumble | ppsspp.ini | Master switch. |
| Rumble strength | ppsspp.ini | None, Light, Medium, Strong (default), Hard. For all games. |
| Rumble strength for this game | `PSP/RUMBLE/settings/<ID>.ini` | "Use the global setting", or a strength of its own. |
| Sound rumble sensitivity | `PSP/RUMBLE/settings/<ID>.ini` | Off, 1 to 10. Higher: smaller bursts count. The default comes from the profile (5 = the gowco-sw tuning). |
| One switch per rule | `PSP/RUMBLE/settings/<ID>.ini` | For God of War: "Hits taken", "Blows landed". |
| Test buttons | | A heavy hit, a sharp tap, an impact. |
| Rumble log (research) | ppsspp.ini | A line per rumble in `PSP/RUMBLE/logs/<ID>_<date>.log`. |
| Save RAM snapshot... | | Writes RAM and the profile's values to `PSP/RUMBLE/snapshots`, with a label you type. |
| Reload rumble profile | | After editing the profile on the SD card. |

The page also shows the profile's values live (`health = 87 @ 09E3CA44`), and how many rumbles have fired.

If there is no profile for the running game but the NRO carries one (it does for God of War: Chains of
Olympus), the page offers **Install the bundled rumble profile**, which copies it to the SD card.

A controller hotkey, **Rumble RAM snapshot**, can be mapped in Control mapping (Emulator shortcuts in the
Switch launcher) to save a snapshot without leaving the game.

No rumble plays while the game isn't running (PPSSPP's pause menu), while it is paused (the profile's
`active_when`), during movies, or while another applet (the HOME menu) has the focus.

## Profile format

An INI file named after the disc ID (`UCUS98653.ini`). Comments start with `;`.

```ini
[Profile]
name = God of War: Chains of Olympus
; Game-state rules (and the sound rumble) only run while all of these hold. Values that can't be read
; make a condition false.
active_when = health > 0, paused == 0
sound = on                 ; the sound rumble
sound_sensitivity = 5      ; its default, 0 (off) to 10
mute_during_video = on     ; nothing while a movie plays

; A value read from memory: start at address, follow the path, read the type.
[Value.health]
address = 0x08B3655C
path = * +0x84 * +0x24 * find(64, 0x0C, 0x08B3E248) +0x64
type = float               ; u8 u16 u32 s8 s16 s32 float
; mask = 0x80              ; for flags: the value is (raw & mask)

[Rule.hit_taken]
label = Hits taken         ; shown in the settings
value = health
event = drop               ; drop, rise, change (either way), level (continuous)
relative_to = health_max   ; the change is divided by this value (or a number)
min = 0.0005               ; smaller changes are ignored
max = 1.0                  ; and bigger ones too (resets, loading)
strength = 0.35            ; strength = strength + gain * change, up to strength_max
gain = 2.6
strength_max = 1.0
duration = 200             ; ms
big = 0.25                 ; a change above this uses big_strength and big_duration instead
big_strength = 1.0
big_duration = 450
cooldown = 0               ; ms before this rule can fire again
per_second = off           ; on: the change per second (speed lost faster than braking can)
feel = heavy               ; heavy, sharp, thump, soft, engine, buzz
```

**Paths.** `*` reads a 32-bit pointer at the current address and goes there. `+N` / `-N` add an offset.
`find(count, key_offset, key)` treats the current address as an array of `count` pointers and goes to the
first one whose 32-bit word at `key_offset` equals `key` (C++ objects: a component with a given vtable). If
any pointer is invalid or nothing is found, the value is "not found" for that frame and its rules wait. When
the address a value resolves to changes (a new level, a respawned object), its rules start afresh instead
of seeing a jump.

**Level rules** (`event = level`) rumble continuously: the value mapped from `level_min`..`level_max` to
0..`strength_max`, for engine rumble rising with speed, say. Use `feel = engine` or `buzz`.

**Feels.** `heavy`: low and strong, holds then fades (a hit taken, a crash). `sharp`: high and short (a blow
landed). `thump`: the sound rumble's. `soft`: gentle. `engine` / `buzz`: for level rules.

A racing game, for example:

```ini
[Value.speed]
address = 0x08A12340
type = float

[Rule.crash]
label = Crashes
value = speed
event = drop
per_second = on
min = 60          ; speed lost per second: more than braking can do
strength = 0.6
gain = 0.004
duration = 350
feel = heavy

[Rule.engine]
label = Engine
value = speed
event = level
level_min = 20
level_max = 250
strength_max = 0.35
feel = engine
```

## Making a profile for a new game

Values are found on the running game, not from the ISO.

1. **CWCheat codes** often give a health address directly: `python Tools/rumble/rumble_research.py cheats
   cheat.db ULUS12345` lists a game's codes from PPSSPP's `cheat.db`.
2. **RAM diffs** between known moments. On the PC, start PPSSPP (or PPSSPPHeadless) with `--debugger=0`
   and the game, and drive it with `Tools/rumble/rumble_research.py PORT script.txt` (commands: `run`,
   `press`, `hold`, `stick`, `shot`, `dump`, `read`, `profile`). Dump RAM at full health, after a hit,
   after another; then `rumble_research.py diff a.bin b.bin c.bin --type float` lists the addresses whose
   value changed. On the Switch, map **Rumble RAM snapshot** to a button combo or use **Save RAM snapshot...**
   in the pause menu, and copy `PSP/RUMBLE/snapshots` to the PC: the dumps have the same layout.
3. **Pointer paths** that survive level loads: set a memory breakpoint on the value (PPSSPP's debugger, on
   the interpreter) to find the code that writes it, and read that code in Ghidra on the decrypted EBOOT
   to see which object it belongs to and how the game reaches it.
4. Write the profile, then check it: `rumble_research.py PORT -c "profile UCUS98653.ini"` on the PC, or
   `rumble_research.py evaldump UCUS98653.ini snapshot.bin` on a Switch snapshot, or the values on the
   Rumble settings page while playing.
5. Turn on the **Rumble log**, play, and tune `strength`, `gain`, `min` and `sound_sensitivity` from what it
   shows.

Keep game content (ISOs, decrypted EBOOTs, RAM dumps, screenshots) out of the repository.

## God of War: Chains of Olympus (UCUS98653)

`assets/rumble/UCUS98653.ini`, from the gowco-sw research (`runtime/rt_debug.c`, `rt_audio.c`), checked in
PPSSPP on the US ISO:

- Kratos's health: the component with vtable `0x08B3E248` in his component list; float at `+0x64`, its
  maximum at `+0x70`. Read 100/100 at the start; 100, 97, 94, 91 as a legionnaire hit him (3 per blow,
  in two steps of 1 and 2).
- The combo counter ("HITS"): the component with vtable `0x08B485EC`, u32 at `+0xB8`. Rose 1, 2, 3, 7, 8 ...
  27 during a fight, back to 0 after it.
- Pause flag: byte `0x08B2DAC3`, `0x80` in the pause menu and while a tutorial message is up.
- The music is on its own sceAudio channel (channel 5 in the first level), fed by sceAtrac, so the sound
  rumble only follows the sound effects. 8 impacts in 12 seconds of fighting at sensitivity 5.
