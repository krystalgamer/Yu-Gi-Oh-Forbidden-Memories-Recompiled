# Replays for players (#523)

A replay is a play session kept as one file, a `.yfmreplay`, that the game
plays back exactly: the same frames, the same sound, the same duel. This is
the desktop's first part; [agent control](agent-control.md) has the
recorder underneath and the agents' tools.

## Recording

**Game > Record replay** (or **F8**, bindable in Game > Controls) starts at
the game's next frame; **Game > Stop recording** (F8 again) ends it. The
file goes to the user folder's `replays/` (on Windows
`Documents\My Games\YFM Re-Decomp\replays\replay-YYYYMMDD-HHMMSS.yfmreplay`),
and a notice names it. Closing the game, or anything that restarts it
(Apply & restart in Mods, a language change, Stop replay), saves it too.

When a recording starts:

- The game switches to the **virtual clock** (the one the agents' replays
  have always run on) from that frame until the recording stops. In a
  window it is paced to the game speed; Pause, Frame step and Turbo work as
  before. A host too slow for a frame now slows the game instead of letting
  a VBlank go by.
- The **sound** is mixed on the game's thread as the virtual clock steps,
  and the sound device plays those frames from a queue (`src/pc/audio/fifo.h`)
  instead of mixing them itself. The game's sound driver reads the voices'
  levels, so where the device's thread mixed, a device that asked a little
  earlier or later changed the game. The queue adds 46 to 139 ms of
  latency; at Turbo the device skips what it cannot play; while paused it
  plays silence.
- A **start state** is taken and loaded at once (a short hitch), so the
  recording begins exactly as its playback will, with only what a state
  holds.
- The player's **progress** is copied into the replay: the save slots
  (`saves/`), what they keep beside them (`cards/`, `packs/`, `duelists/`,
  `decks/`, `tower/`) and the memory card images (`memcard1.mcd`,
  `memcard2.mcd`), as they are at the start.

What is recorded is the pad bits the game reads at every VBlank, from every
source (keyboard, controllers, the mouse's and the touch controls' buttons),
with a frame hash at every frame, and a header: the build, the OS, the
settings and the mods that were on.

## Playing

**File > Play replay...** picks a `.yfmreplay` and restarts the game into
it. The window and the sound are on; the player's pads and keys do not reach
the game (the replay's do). Pause (P), Frame step (`.`) and Turbo (Tab) work.
At its end the game pauses and says so; **File > Stop replay** goes back to
the title (the game restarts). Agents and tests play one with
`MEMORIES_PLAY=file.yfmreplay`, which ends the game at the replay's end when
headless.

**A replay never writes the player's saves.** It is unpacked into the user
folder's `replays/.playing/`, and for as long as the game runs its
`progress/` copy is the player's progress (`Paths_SetProgressDir`, paths.h):
a replay that saves at the shop saves there. `MEMORIES_MEMCARD1/2` are not
used then. `tests/pc/player_replay_test.py` checks it: the player's
progress hashes the same before and after a replay that saves.

## A replay from someone else

- The file is read with the Mods window's hardened zip reader
  (`src/pc/mods/import.c`): no name that leaves its folder, no link,
  sizes and CRCs checked, and only the progress names above are unpacked.
- **The start state is loaded as a save state is** (File > Load state), with
  the same checks (`state.c`: the build it was made by, its chunks), and a
  save state restores the whole game as it was, its stack included: as with
  a save state from someone else, open replays from people you trust. A
  start state that does not load (another version of the game, a damaged
  file) is not played onto a game booting from scratch: the replay stops
  with a notice.
- A replay with a control command in it (a `C` line: a `poke` into memory or
  a `jump` to a screen, which only the agents' recordings have) is refused
  and not played.

## The format

A `.yfmreplay` is the zip `tools/pc/replay.py` reads (format 1, kind
`recorded`, `replay_file.h`): `replay.json` (the header, `"player": true`,
`"progress": "progress"`), `recording.txt` (`recorder.h`'s lines, from
`start state start.state`), `start.state` and `progress/`. `replay.py play
file.yfmreplay --check` plays one headless and compares its frames; it puts
the progress in the play's user folder.

## What is not in a replay (yet)

These end a recording before they happen, with a notice (the replay is kept
up to there), and stop a replay that is playing: **Load state** (F7, the
menu, the channel's `load`), **the deck slots** (F6), **a cheat**, **Debug >
Jump to** and Restart game, **turning a mod on or off**, and a control
client's `poke` or `jump`. Save state (F5) is fine.

Not checked yet (the next part, PR 2): the settings that change the game
(Card drops, Use deck slots, the cheats' settings, ...) are played with the
player's own, not the replay's; the mods are not compared (a replay made
with a mod played without it, or another version of it, parts company); a
replay made by another build may not play the same. Nothing tells the player
when a play has parted from its recording (the frame hashes are there for
it). A mod's own stored data (`mod-data/`) is not copied.

Not on Android yet: there are no menu items, and the phone's mixer stays
on its own thread.

## Open question

A start state holds the game's 2 MiB of RAM, which is the disc's code and
data as the game unpacked it (and the SPU's 512 KiB of sound RAM, among the
rest; a start state is about 7.6 MB, 1.2 MB once zipped):
a shared replay carries part of the game with it. The repository's own
replays (`tests/pc/replays`) start from boot for that reason. Should a
player's replay start from boot instead (slower to reach the moment, and
the progress copy then decides everything), or is a start state in a file
players share acceptable?
