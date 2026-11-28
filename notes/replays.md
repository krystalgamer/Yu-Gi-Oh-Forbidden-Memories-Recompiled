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

When a recording stops, its file is made **on a thread of its own**, and the
game goes on: see "Packing" below. The notice ("Replay saved", or why not)
comes when the file is whole.

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
  earlier or later changed the game. At 100 % the queue's fill paces the
  game (`pace()`, which calls `AudioFifo_PaceGrid`, the one function the
  pacer test runs too): each VBlank's period is nudged
  by at most 1/128 toward about 70 ms queued (3072 frames, ±512), and a
  frame late after a stall does not catch up while the queue is above
  that, so the latency neither creeps up after stalls nor drifts with a
  device whose clock is a little off. Only when the host sleeps changes:
  the game sees the same. It is off with no device (the silent sink) and
  under a control client, which steps the game itself. At Turbo the device
  skips what it cannot play (past 139 ms); while paused it plays silence.
  Where the queue runs dry (a stall, a pause) the last frame fades out over
  64 frames (1.5 ms), and the sound fades in as long when it comes back,
  instead of cutting to zero and back (`AudioFifo_Fade`: the device's side
  only). The other jumps are crossfaded over the same 64 frames from the
  last frame the device heard (`AudioFifo_Join`): the FIFO's switch on (the
  device's own mixer's last frame fades out as the queue's first come up),
  its switch off (the device's own mixer fades in from the FIFO's last
  frame, also after a silence) and a skip past 139 ms at Turbo (also from
  digital zero at full level). A stop and a start between two calls of the
  device (the gate reads "on" both times, the FIFO was reset in between) is
  joined too: every switch counts in the gate's one atomic word, and a call
  that finds the count moved is handed over from the last frame it gave. All of
  it is the consumer's alone (the gate's ordering is unchanged), so the game
  sees the same. The
  HUD's audio line then shows the queue's own numbers: the
  fill in ms, its underruns and the frames skipped.
- A **start state** is taken and loaded at once (a short hitch), so the
  recording begins exactly as its playback will, with only what a state
  holds.
- The player's **progress** is copied into the replay: the save slots
  (`saves/`), what they keep beside them (`cards/`, `packs/`, `duelists/`,
  `decks/`, `tower/`) and the memory card images (`memcard1.mcd`,
  `memcard2.mcd`), as they are at the start.

**Game > Record replay from boot...** is the other kind. Where Record replay
starts from the game as it is (a start state), this one restarts the game and
records from its first frame, so the file holds **no start state**: nothing of
the game's RAM, which is the disc's code and data, only what the player
pressed and their own progress. Choose it when the replay is to be shared.
It is menu-only (no key, so the Controls list does not grow) and not on
Android, like the other replay items. It asks first, as Game > Restart game
and File > Stop replay do ("Restart the game and record a replay from its
first frame? Unsaved progress is lost."; Cancel is the default): a restart
drops what is not saved; File > Play replay... asks the same once its file is
chosen.
The restart is the one that item uses (`Platform_RestartGame`); the name of
the new file is left in `replays/next-record.txt`, as Play replay leaves its
file's in `next.txt`, and the next start takes it (`Recorder_Init`) and
records from the first VSync, on the virtual clock from frame 0. A marker is
two lines, the time it was made and the file, and is an order only while
fresh (under 120 s, so a slow start or a scan still counts; a clock set back
by up to 10 s does not spoil it) and once removed: the leftover of a restart
that never came (the monitor could not start the game, the new process died
first), a marker of the older format and one that cannot be removed (a
read-only folder, an antivirus) are dropped with a line on stderr and never
honoured, so a later ordinary start does not record or play by itself
(`take_marker`, replay_file.c, for both markers). Both are taken at every
start, `MEMORIES_PLAY` or not, so none waits for a later one. One that was
there and was dropped is told to the player too (a notice, as the other
refusals are): they asked, and nothing happened. The markers are taken in
`Recorder_Init` and not earlier in `main()`: that keeps the age that of the
whole startup, and a take in `main()` would meet the Windows monitor
process, which must leave it to the game. When the restart itself fails,
the request is taken back (`ReplayFile_DropNextRecord`, `ReplayFile_DropNext`)
and the player is told: the menu with a notice, the control channel by
detaching (its `ok` went out before: a restart that works never returns,
and an `err` after the `ok` would be read as the reply to the next command;
the client sees EOF either way). Under the Windows monitor the game ends
before the restart is tried (`Win32_Restart` only tells the monitor), so it
cannot take anything back: the monitor does when its `start_game` fails
(`restart_failed`, monitor.c: both markers, a line in the session log and a
message box). A restart that fails without the monitor has already dropped
`MEMORIES_CONTROL` and applied `MEMORIES_RESTART_ENV` in this game's
environment (`Platform_RestartGame`, before `execv` or `CreateProcessW`):
a client may connect again, since the listener was set up at start, but the
end-of-replay rule below reads the variable. The game that
comes up is an ordinary boot (the logos, then the title), with the player's
saves as they were when the game restarted. Stop and a restart
end it as for a recording from here. The item is off while a recording or a
replay is on.

### Packing

Stopping a recording used to pack the zip on the game's thread: 108 ms
measured (a 2.5-minute recording from here: 87 ms deflating the 7.6 MB start
state at level 6, 9 ms the recording's 8400 frames, 4 ms creating the file,
3 ms removing the folder, 3 ms the progress, 2 ms the header; a recording from
boot has no state to deflate), during which the game did not run, the sound
FIFO ran dry and the player heard a gap. Now `Recorder_Stop` (F8, the menu, a host action
that ends a recording, a restart) does only what has to be the game's: it
writes the `E` line, closes `recording.txt`, notes its length and hands the
working folder (`<file>.parts`) to a thread of its own
(`ReplayFile_PackStart`, replay_file.c; 0.6 ms measured, logged as
`record: stopped, N ms on the game's thread`). The thread reads and writes
files and nothing else: no guest memory, none of the game's clocks and no notice, so the game
sees nothing different (`MEMORIES_CLOCK=interrupt`'s signal is blocked in the
thread that starts it, so the new thread starts with it blocked). It prints
nothing to a stream either: a thread that printed a number while the process
was in `exit()` hung the Windows process (`pack_thread`).
It writes `<file>.partial` and renames it to `<file>` when the zip is whole
(on Windows over a file that is there, written through): the `.yfmreplay` is
there whole or not at all, and a failure removes the `.partial` and keeps the
folder, as before. How it went is told on the game's thread, by the same
notice as before, when the recorder takes the result (`packs_ended`, from
`Recorder_Point` and from the notice poll): the pack's own thread never
calls `notify`.

- **A new recording while a pack runs** does not collide: every recording has
  its own folder, so a new one to another name starts at once, and up to four
  packs run together (a fifth is packed on the game's thread, as it was
  before). One to the **same** name is refused ("An earlier recording to this
  file is still being packed: try again in a moment"), since the folder is
  the pack's; a name made for a new file (`ReplayFile_NewName`) skips a name
  whose pack still runs, so F8 twice in a second does not.
- **Quitting or restarting while a pack runs** waits for it, bounded to 30 s:
  `atexit` (closing the window, the control channel's `quit`, the end of a
  replay) and `Recorder_BeforeRestart` (Platform_RestartGame: a restart, a
  `record_boot`, a language change, which end the process with `execv` or
  `TerminateProcess` and so would cut the thread) call
  `Recorder_FinishPacking`. A script that does `record stop` and then `quit`
  finds a whole `.yfmreplay` after the game has gone. A game that is killed
  or crashes leaves the `.partial` and the folder, never a half file.
- **The control channel's `record stop`** stays synchronous in what it
  promises: it reports `ok` only after the file is whole (it waits for the
  packs inside the command, where the game is held anyway), so a client that
  gets the `ok` can read the file. A failed pack is told as before: the
  notice and the log, `ok` all the same. A recording ended by a host action
  (`load`, `poke`, `jump`...) is not waited for: the file is there by the
  time the game quits, or in a moment.

What is recorded is the pad bits the game reads at every VBlank, from every
source (keyboard, controllers, the mouse's and the touch controls' buttons),
with a frame hash at every frame, and a header: the build, the OS, the
settings and the mods that were on.

## Playing

**File > Play replay...** picks a `.yfmreplay` and restarts the game into
it, once asked ("Restart the game and play the replay? Unsaved progress is
lost."; Cancel is the default), as the other restarts are; in a tower's duel
it is refused with the tower's notice, as they are. The window and the sound
are on; the player's pads and keys do not reach the game (the replay's do).
Pause (P), Frame step (`.`) and Turbo (Tab) work. At its end the game pauses
and says so; **File > Stop replay** goes back to the title (the game
restarts). Agents and tests play one with `MEMORIES_PLAY=file.yfmreplay`,
which ends the game at the replay's end when headless or when
`MEMORIES_CONTROL` is set (a control client gets no answer from a pause). The
menu's restart drops `MEMORIES_CONTROL`, so a menu play keeps its pause,
unless a scripted check puts the variable back through
`MEMORIES_RESTART_ENV`: then the play ends the game, as one with a client
does. The rule keys on the variable, not on an attached client: it is there
from the first frame, and the game waits for the client at start, so a
client that is late or has left does not turn the end into a pause a script
waits on. The control channel has no play command.

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
`start state start.state`, or `start boot`), `start.state` (not in a
recording from boot, whose `replay.json` says `"start": {"kind": "boot"}`) and
`progress/` (the save slots and the memory card images, in both kinds). `replay.py play
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

## Which kind to share

A start state holds the game's 2 MiB of RAM, which is the disc's code and
data as the game unpacked it (and the SPU's 512 KiB of sound RAM, among the
rest; a start state is about 7.6 MB, 1.2 MB once zipped):
a shared replay from here carries part of the game with it. The repository's
own replays (`tests/pc/replays`) start from boot for that reason, and a player
who does not want to share that records **from boot** (above): slower to
reach the moment, and the progress copy then decides everything. Record
replay (from here) stays the default.

Tests: `tests/pc/player_replay_test.py` (CTest `pc_player_replay`) records
through the control channel's `record_boot PATH` (what the menu item does after
its question), checks the package has no state file and a boot start, and
plays it back frame for frame (through `MEMORIES_PLAY`, headless: the unpack,
the scratch progress and the playback File > Play replay... shares; not the
menu item and its restart through `next.txt`, a window's clock, the audio
FIFO or the tower guards). It also checks that a leftover marker is dropped
and starts nothing, and that packing is off the game's thread: a recording that
a host action ends, whose game is told to quit at once, and ones started at
once to the same name and to another, are whole files after the exit, and the
game's thread spent a small part of the pack's time at each stop. The pack
itself (whole and at once, nothing left on a failure, the folder not touched
while it runs, several at once, bounded waits) is `tests/pc/replay_pack_test.c`
(CTest `pc_replay_pack`), with the thread held at its start so that none of it
depends on speed. The FIFO's fades and joins, and the pacer's decision
(`AudioFifo_PaceGrid`), are `tests/pc/audio_fifo_test.c`'s (CTest
`pc_audio_fifo`). A recording's package says "from boot" by its header's
`start boot` line, not by the folder holding no state: a recording from a
state that lost its state is refused (the folder is kept), not packed as one
from boot.
