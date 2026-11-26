#ifndef MEMORIES_PC_DEBUG_RECORDER_H
#define MEMORIES_PC_DEBUG_RECORDER_H
/* The input recorder and player (notes/agent-control.md, step 3).
 *
 * MEMORIES_RECORD=<path> writes, as text, the pad bits the game is given at
 * every VBlank from every source (the keyboard, the controllers, the mouse,
 * MEMORIES_INPUT and a control client, already ORed), at the one point they
 * all pass (run_vblank, libetc.c), before the mods' INPUT hooks and View >
 * Japanese buttons, which act on them again when they are played back.
 * Lines, one kind a letter:
 *
 *     yfm-recording 1
 *     build <id>                      the build that made it
 *     F <key>: <value>                build, os, settings and mods, as the
 *                                     crash reports' facts give them
 *     start boot | start state <path> (MEMORIES_LOAD_STATE: the recording
 *                                     begins at the first safe point after
 *                                     the state is loaded)
 *     I <index> <p1> <f1> <p2> <f2> <c2>
 *                                     from VBlank <index> on (counted from
 *                                     the start) pad 1 and pad 2 hold these
 *                                     bits, of which f1/f2 are exempt from
 *                                     the Japanese buttons' exchange; c2 is
 *                                     1 when pad 2 is connected. Written only
 *                                     when something changed.
 *     H <index> <frame> <hash>        at every safe point (the end of a
 *                                     VSync(0)): the VBlank, the presented
 *                                     frame (counted from the start too: a
 *                                     state does not carry it) and
 *                                     Memories_VramHash
 *     S <index> <frame> <path>        a save state taken there, every
 *                                     MEMORIES_RECORD_STATES VBlanks
 *     C <index> <frame> <line>        a control client's `poke` or `jump`,
 *                                     taken at the safe point of that VBlank
 *                                     and frame (control.c): done again at
 *                                     the same point, after its H line, so a
 *                                     session that jumps to a screen or
 *                                     arranges a deck replays from the pads
 *                                     and these alone
 *     E <index> <frame>               the end
 *
 * MEMORIES_PLAY=<path> plays such a file back: the pads get its bits at
 * each VBlank instead of the live ones (which are ignored), from the same
 * start, does the C lines again at their points, and the game ends at its E
 * line. Recording while playing gives the lines to compare
 * (tools/pc/replay.py), C lines included. Host actions (F5/F7, the deck
 * slots, the menus) and the client's `load` are not recorded.
 *
 * Either runs the game on the virtual clock with the stepped mixer
 * (platform.h, Platform_SetVirtualClock and Platform_SetSteppedAudio):
 * only those make a run that plays back, window and sound or not.
 *
 * The player's replays (notes/replays.md, replay_file.h): Game > Record
 * replay (or the client's `record PATH`) starts recording at a VSync, from
 * a start state taken there and loaded at once, and Stop ends it with its E
 * line (so does a restart, which skips atexit on Windows); a .yfmreplay
 * holds it with the player's progress. Game > Record replay from boot...
 * restarts the game and records from its first frame instead: the same
 * file with no start state. File > Play replay... restarts into
 * one, or MEMORIES_PLAY names it: its progress is a scratch copy, and a file
 * with a C line is refused. A host action while either runs (a state load,
 * the deck slots, a cheat, Jump to, a mod turned on or off, a client's
 * command) ends it with a notice instead of being recorded or played.
 *
 * Unset, both cost one test per VBlank and one per frame. */
#include <stddef.h>
#include <stdint.h>

/* ResetCallback: read MEMORIES_PLAY's file, open MEMORIES_RECORD's. */
void Recorder_Init(void);
/* run_vblank, before anything reads the pads: the bits from every source
 * and the Japanese-buttons-exempt part of them, by pad, and whether pad 2
 * is connected. Playing, they are replaced by the file's. Signal-safe. */
void Recorder_Pads(uint16_t bits[2], uint16_t fixed[2], int *pad2_connected);
/* A mod's host->pad: while playing, the bits the last VBlank was given
 * (what the live pad read between two VBlanks when recorded); else `live`. */
uint16_t Recorder_HostPad(int port, uint16_t live);
/* The end of VSync(0), with the presented frame count. */
void Recorder_Point(unsigned frame);
/* A control client's poke or jump, done at the point Recorder_Point just
 * passed (control.c): kept as a C line. */
void Recorder_Command(const char *line);
/* At run time, from the end of a VSync(0) (a state point): start recording
 * to `path`, a .yfmreplay (packed at the stop) or a plain recording (its
 * start state beside it, `path`.start.state); or stop, which writes the E
 * line (-1 when none ran). Start does not return when it succeeds: the start
 * state is loaded at once and the game resumes in its VSync caller. -1 with
 * why. */
int Recorder_Start(const char *path, char *why, size_t why_size);
int Recorder_Stop(void);
int Recorder_Recording(void);
/* Game > Record replay: a new .yfmreplay in the user folder's replays/,
 * started at the next state point. Main thread. */
void Recorder_RequestStart(void);
/* Game > Record replay from boot... and the client's `record_boot PATH`: the
 * game restarts and records from its first frame, so the .yfmreplay holds no
 * start state (the player's progress is still copied in). Request names the
 * file (PATH, a .yfmreplay, or NULL for a new one in the user folder's
 * replays/) and leaves it for the next start: 0, or -1 with why. Restart
 * then restarts the game (Platform_RestartGame); it returns only when that
 * failed, -1 with why, the request taken back. Main thread. */
int Recorder_BootRequest(const char *path, char *why, size_t why_size);
int Recorder_BootRestart(char *why, size_t why_size);
/* A player's replay is playing (or has ended, the game paused). */
int Recorder_PlayingReplay(void);
/* Something a replay cannot hold is about to happen (`what`, a phrase that
 * starts a sentence: "Load state"): a player's recording stops before it,
 * a player's replay stops playing; both with a notice. Main thread. */
void Recorder_HostAction(const char *what);
/* Platform_RestartGame, before it ends this process: a recording ends and
 * is saved, as atexit would have it. */
void Recorder_BeforeRestart(void);
/* The notice the recorder has for the player, once: 1 with it, else 0. */
int Recorder_TakeNotice(char *title, size_t title_size, char *text, size_t text_size);

#endif
