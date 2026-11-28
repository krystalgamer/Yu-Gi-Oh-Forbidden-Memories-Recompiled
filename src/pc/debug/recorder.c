/* The input recorder and player (recorder.h). */
#include "pc/compat/fs.h"
#include "recorder.h"
#include "replay_file.h"
#include "control.h"
#include "control_protocol.h"
#include "pc/debug/monitor.h"
#include "pc/guest/state.h"
#include "pc/platform/paths.h"
#include "pc/platform/platform.h"
#include "pc/sdk/display.h"
#include "pc/tower/tower.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    uint32_t index;
    uint16_t bits[2], fixed[2];
    uint8_t pad2;
} PadEvent;

#define PENDING_MAX 1024 /* changes between two safe points; a frame has a few VBlanks */

static enum { WAITING, RUNNING } phase;
static int recording, playing, started;
static unsigned start_vblank;        /* the VBlank count the indices count from */
static unsigned start_frame;         /* and the presented frame the frames count from (a state does not carry it) */
static int at_exit;
/* The player's format (replay_file.h): a recording started at run time, or
 * a .yfmreplay playing. Pads only: a host action or a client's command
 * ends it (Recorder_HostAction). */
static int player_record, player_play;
static char zip_path[600];           /* recording: the .yfmreplay it becomes ("" for a plain one) */
static volatile int start_asked;     /* Game > Record replay: at the next point */
static char notice_title[64], notice_text[700];
static volatile int notice_pending;
static FILE *out;
static char out_path[600];
static unsigned states_every, last_state;
/* Recording: changes noted at the VBlank, written at the next safe point. */
static PadEvent pending[PENDING_MAX], last_written;
static volatile unsigned pending_count, dropped;
static int have_last;
/* Playing: the file's changes, in order. */
static PadEvent *script;
static size_t script_count, script_at;
static unsigned play_end;
static int play_has_end;
static unsigned play_end_frame;
static int clock_written;
/* The safe point Recorder_Point last passed, where a client's commands are taken. */
static unsigned point_index, point_frame;
/* Playing: the C lines, in order. */
typedef struct {
    unsigned index, frame;
    char *line;
} Command;
static Command *commands;
static size_t command_count, command_at;
static volatile uint16_t played[2]; /* the bits given at the last VBlank, played or recorded */

static void begin(void)
{
    started = 1;
    phase = RUNNING;
    start_vblank = Platform_VBlankCount();
    start_frame = Memories_PresentedFrames();
}

static size_t command_room;

static void add_command(unsigned index, unsigned frame, const char *text)
{
    size_t length = strcspn(text, "\r\n");
    char *copy = malloc(length + 1);
    if (!copy) return;
    if (command_count == command_room) {
        Command *grown;
        size_t room = command_room ? command_room * 2 : 64;
        grown = realloc(commands, room * sizeof(*commands));
        if (!grown) {
            free(copy);
            return;
        }
        commands = grown;
        command_room = room;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    commands[command_count].index = index;
    commands[command_count].frame = frame;
    commands[command_count++].line = copy;
}

/* What read_script took, given back: a file refused is no script. */
static void drop_script(void)
{
    size_t i;
    for (i = 0; i < command_count; i++) free(commands[i].line);
    free(commands);
    free(script);
    commands = NULL;
    script = NULL;
    script_count = command_count = command_room = 0;
    command_at = script_at = 0;
    play_has_end = 0;
}

static void read_script(const char *path)
{
    FILE *file = fopen(path, "r");
    /* A C line holds a poke of up to CONTROL_DATA_MAX bytes in hex. */
    static char line[CONTROL_LINE_MAX + 64];
    size_t room = 0;
    if (!file) {
        fprintf(stderr, "memories-pc: replay: cannot read %s; the pads stay idle\n", path);
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        unsigned index, b1, f1, b2, f2, c2, frame;
        int at = 0;
        if (sscanf(line, "C %u %u %n", &index, &frame, &at) == 2 && at) {
            add_command(index, frame, line + at);
        } else if (sscanf(line, "I %u %x %x %x %x %u", &index, &b1, &f1, &b2, &f2, &c2) == 6) {
            if (script_count == room) {
                PadEvent *grown;
                room = room ? room * 2 : 1024;
                grown = realloc(script, room * sizeof(*script));
                if (!grown) break;
                script = grown;
            }
            script[script_count].index = index;
            script[script_count].bits[0] = (uint16_t)b1;
            script[script_count].fixed[0] = (uint16_t)f1;
            script[script_count].bits[1] = (uint16_t)b2;
            script[script_count].fixed[1] = (uint16_t)f2;
            script[script_count].pad2 = (uint8_t)(c2 != 0);
            script_count++;
        } else if (sscanf(line, "E %u %u", &index, &frame) == 2) {
            play_end = index;
            play_end_frame = frame;
            play_has_end = 1;
        }
    }
    fclose(file);
    fprintf(stderr, "memories-pc: replay: %lu pad changes and %lu client commands from %s%s\n",
            (unsigned long)script_count, (unsigned long)command_count, path, play_has_end ? "" : " (no end: it runs on)");
}

/* What the run was made with: the facts every crash report starts with
 * (monitor.h) that decide what a replay sees, as "F key: value" lines. */
static void write_facts(void)
{
    static const char *const keys[] = {"build: ", "os: ", "settings: ", "mods: "};
    static char facts[4096];
    char *line, *next;
    size_t i;
    Monitor_Facts(facts, sizeof(facts));
    for (line = facts; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (!strncmp(line, keys[i], strlen(keys[i]))) fprintf(out, "F %s\n", line);
        }
    }
}

static void finish(void)
{
    if (!out) return;
    fprintf(out, "E %u %u\n", started ? Platform_VBlankCount() - start_vblank : 0,
            started ? Memories_PresentedFrames() - start_frame : 0);
    fclose(out);
    out = NULL;
}

static void notify(const char *title, const char *format, const char *a, const char *b)
{
    snprintf(notice_title, sizeof(notice_title), "%s", title);
    snprintf(notice_text, sizeof(notice_text), format, a ? a : "", b ? b : "");
    notice_pending = 1;
    fprintf(stderr, "memories-pc: replay: %s: %s\n", notice_title, notice_text);
}

/* How a pack ended, told to the player: "Replay saved" (unless `quiet`: the
 * caller said something already, as Recorder_HostAction does) or why not.
 * The game's thread only: notify's buffers are not thread-safe, which is why
 * the pack's own thread never calls it (replay_file.h). */
static void report_pack(const char *file, int failed, const char *why, int quiet)
{
    if (failed) notify("Replay not saved", "The recording could not be packed into %s: %s", file, why);
    else if (!quiet) notify("Replay saved", "%s%s", file, NULL);
    else fprintf(stderr, "memories-pc: replay: saved %s\n", file);
}

/* The packs that ended since the last call, each told once. */
static void packs_ended(void)
{
    char file[1100], why[400];
    int quiet, got;
    while ((got = ReplayFile_PackTake(file, sizeof(file), &quiet, why, sizeof(why))) != 0)
        report_pack(file, got < 0, why, quiet);
}

#define PACK_WAIT_MS 30000 /* about 150 ms on a disk that answers; a stuck one does not hold the exit for ever */

void Recorder_FinishPacking(void)
{
    if (!ReplayFile_PackWait(NULL, PACK_WAIT_MS))
        fprintf(stderr, "memories-pc: replay: a replay is still being packed after %d s; going on without it\n",
                PACK_WAIT_MS / 1000);
    packs_ended();
}

/* The E line and, for a player's recording, the start of its .yfmreplay's
 * packing: the game's thread closes the recording, notes how long it is and
 * hands the folder to a thread of its own (replay_file.h), which writes the
 * zip while the game goes on; packs_ended tells how it went. Only when no
 * thread can be had, or `here` (the game is already ending, inside exit():
 * a thread started there gains nothing), is it packed here, as it always was.
 * 0, or -1 with a notice when that failed. */
static int end_recording(int quiet, int here)
{
    char why[400];
    int failed = 0;
    finish();
    recording = 0;
    started = 0;
    if (zip_path[0]) {
        struct stat info;
        unsigned long long size = !stat(out_path, &info) ? (unsigned long long)info.st_size : 0;
        if (here || ReplayFile_PackStart(zip_path, size, quiet, why, sizeof(why))) {
            if (!here) fprintf(stderr, "memories-pc: replay: %s Packing %s on the game's thread.\n", why, zip_path);
            failed = ReplayFile_Finish(zip_path, why, sizeof(why)) != 0;
            report_pack(zip_path, failed, why, quiet);
        }
        zip_path[0] = '\0';
    }
    return failed;
}

static int stop_recording(int quiet);

/* Closing the game: a recording is ended, and the packs under way are waited
 * for (bounded), so that the file is whole when the process is gone. */
static void at_exit_end(void)
{
    if (recording) end_recording(1, 1);
    Recorder_FinishPacking();
}

/* The header: what made it, and where it starts. */
static void write_header(const char *state)
{
    fprintf(out, "yfm-recording 1\nbuild %08x\n", (unsigned)Memories_StateBuildId());
    if (state && *state) fprintf(out, "start state %s\n", state);
    else fprintf(out, "start boot\n");
    write_facts();
    fflush(out);
    if (!at_exit) {
        at_exit = 1;
        atexit(at_exit_end);
    }
}

/* A .yfmreplay to play (MEMORIES_PLAY, or the one File > Play replay...
 * left for this start): unpacked, its progress made the game's for this
 * run, its start state the one loaded at startup. 0 with the recording's
 * path in `recording`, -1 when it cannot be played (with a notice). */
static int open_player_replay(const char *file, char *recording, size_t size)
{
    static char state[1200], progress[1200];
    char why[400];
    if (ReplayFile_Open(file, recording, size, state, sizeof(state), progress, sizeof(progress), why, sizeof(why))) {
        notify("Replay not played", "%s could not be read: %s", file, why);
        return -1;
    }
    read_script(recording);
    /* Players' replays hold the pads only: a poke or a jump is the control
     * channel's, which a stranger's file has no business running. */
    if (command_count) {
        notify("Replay not played",
               "%s holds control commands (a poke or a jump), which a player's replay never has; it was not played.",
               file, NULL);
        drop_script();
        return -1;
    }
    Paths_SetProgressDir(progress);
    if (state[0]) setenv("MEMORIES_LOAD_STATE", state, 1);
    else unsetenv("MEMORIES_LOAD_STATE");
    player_play = 1;
    fprintf(stderr, "memories-pc: replay: playing %s (the saves it writes go to %s)\n", file, progress);
    return 0;
}

/* A .yfmreplay to record from the game's first frame (Game > Record replay
 * from boot..., which restarted the game for it): the same working folder
 * and header as Recorder_Start's, with no start state, and the progress
 * copied before the game reads any of it. 0, or -1 with a notice. */
static int record_from_boot(const char *file)
{
    char recording_path[sizeof(out_path)], state[sizeof(out_path) + 16], why[300];
    if (strlen(file) >= sizeof(zip_path)) {
        notify("Recording not started", "The path is too long: %s", file, NULL);
        return -1;
    }
    /* `state` names where a start state would go; none is ever written. */
    if (ReplayFile_Begin(file, recording_path, sizeof(recording_path), state, sizeof(state), why, sizeof(why))) {
        ReplayFile_Abandon(file);
        notify("Recording not started", "%s", why, NULL);
        return -1;
    }
    snprintf(out_path, sizeof(out_path), "%s", recording_path);
    if (!(out = fopen(recording_path, "w"))) {
        ReplayFile_Abandon(file);
        notify("Recording not started", "Could not write %s.", recording_path, NULL);
        return -1;
    }
    write_header(NULL);
    snprintf(zip_path, sizeof(zip_path), "%s", file);
    recording = 1;
    player_record = 1;
    fprintf(stderr, "memories-pc: record: recording to %s from boot\n", file);
    return 0;
}

void Recorder_Init(void)
{
    static char next[1024], unpacked[1200], boot[1024];
    const char *record = getenv("MEMORIES_RECORD"), *play = getenv("MEMORIES_PLAY");
    const char *state, *every = getenv("MEMORIES_RECORD_STATES");
    /* Both taken at every start, MEMORIES_PLAY or not, so a request that is
     * not honoured does not come back. One that was there and was refused
     * (too old: the restart took long, or never came; or it could not be
     * removed) is told to the player, who asked for something. */
    int boot_taken = ReplayFile_TakeNextRecord(boot, sizeof(boot));
    int next_taken = ReplayFile_TakeNext(next, sizeof(next));
    int boot_asked = !boot_taken;
    if (next_taken == 0) {
        if (play && *play) fprintf(stderr, "memories-pc: replay: MEMORIES_PLAY is set: replays/next.txt is not played\n");
        else play = next;
    } else if (next_taken == REPLAY_MARKER_DROPPED) {
        notify("Replay not played",
               "The request to play a replay (replays/next.txt) was too old to be honoured, or could not be removed. "
               "File > Play replay... asks again.", NULL, NULL);
    }
    if (boot_taken == REPLAY_MARKER_DROPPED) {
        notify("Recording not started",
               "The request to record from boot (replays/next-record.txt) was too old to be honoured, or could not "
               "be removed: the game took too long to restart, or did not restart. Game > Record replay from boot... "
               "asks again.", NULL, NULL);
    }
    if (play && *play && ReplayFile_Is(play)) {
        if (!open_player_replay(play, unpacked, sizeof(unpacked))) playing = 1;
    } else if (play && *play) {
        playing = 1;
        read_script(play);
    }
    state = getenv("MEMORIES_LOAD_STATE");
    if (record && *record) {
        snprintf(out_path, sizeof(out_path), "%s", record);
        out = fopen(record, "w");
        if (!out) {
            fprintf(stderr, "memories-pc: record: cannot write %s\n", record);
        } else {
            recording = 1;
            write_header(state);
        }
    }
    if (boot_asked) {
        if ((record && *record) || playing || (state && *state)) {
            notify("Recording not started",
                   "The marker replays/next-record.txt asked for a recording from boot, but this start was already "
                   "to record or play something else (MEMORIES_RECORD, MEMORIES_PLAY, MEMORIES_LOAD_STATE or "
                   "the marker replays/next.txt).", NULL, NULL);
        } else {
            record_from_boot(boot);
        }
    }
    states_every = every ? (unsigned)strtoul(every, NULL, 10) : 0;
    if (!recording && !playing) return;
    /* Only the virtual clock replays, with the sound mixed as it steps. */
    Platform_SetVirtualClock(1);
    Platform_SetSteppedAudio(1);
    /* From boot the indices are the VBlank count; from a state they start
     * once the state is in (Recorder_Point). */
    if (state && *state) phase = WAITING;
    else begin();
}

void Recorder_Pads(uint16_t bits[2], uint16_t fixed[2], int *pad2_connected)
{
    unsigned index;
    if (!recording && !playing) return;
    if (phase != RUNNING) {
        /* Until the start state is in: the same nothing, recording or playing. */
        bits[0] = bits[1] = fixed[0] = fixed[1] = 0, *pad2_connected = 0;
        return;
    }
    index = Platform_VBlankCount() - start_vblank;
    if (playing) {
        while (script_at < script_count && script[script_at].index <= index) script_at++;
        if (script_at) {
            const PadEvent *event = &script[script_at - 1];
            bits[0] = event->bits[0];
            bits[1] = event->bits[1];
            fixed[0] = event->fixed[0];
            fixed[1] = event->fixed[1];
            *pad2_connected = event->pad2;
        } else {
            bits[0] = bits[1] = fixed[0] = fixed[1] = 0;
            *pad2_connected = 0;
        }
    }
    /* A mod's host->pad reads these, recording or playing, so both see the
     * same bits (Recorder_HostPad). */
    played[0] = bits[0];
    played[1] = bits[1];
    if (recording) {
        PadEvent event;
        event.index = index;
        event.bits[0] = bits[0];
        event.bits[1] = bits[1];
        event.fixed[0] = fixed[0];
        event.fixed[1] = fixed[1];
        event.pad2 = (uint8_t)(*pad2_connected != 0);
        if (pending_count < PENDING_MAX) pending[pending_count++] = event;
        else dropped++;
    }
}

void Recorder_Command(const char *line)
{
    if (player_record || player_play) {
        Recorder_HostAction("A control client's command");
        return;
    }
    if (!recording || !out || phase != RUNNING) return;
    fprintf(out, "C %u %u %s\n", point_index, point_frame, line);
    fflush(out);
}
uint16_t Recorder_HostPad(int port, uint16_t live)
{
    return playing || (recording && phase == RUNNING) ? played[port & 1] : live;
}

void Recorder_Point(unsigned frame)
{
    unsigned index, i;
    packs_ended();
    if (start_asked) {
        /* Game > Record replay, taken at this state point. Does not return
         * when it starts (the start state is loaded). */
        char file[600], why[300];
        start_asked = 0;
        if (ReplayFile_NewName(file, sizeof(file))) {
            notify("Recording not started", "No name could be made for the replay in %s.", Paths_UserDir(), NULL);
        } else if (Recorder_Start(file, why, sizeof(why))) {
            notify("Recording not started", "%s", why, NULL);
        }
    }
    if (!recording && !playing) return;
    if (phase == WAITING) {
        if (!Memories_StateStartupDone()) return;
        if (player_play && Memories_StateStartupFailed()) {
            /* Played on a game booting from scratch, the pads would mean nothing. */
            playing = 0;
            notify("Replay not played",
                   "Its start state could not be loaded (made by another version of the game, or damaged): "
                   "the reason is in the log.%s%s", NULL, NULL);
            return;
        }
        begin();
    }
    index = Platform_VBlankCount() - start_vblank;
    frame -= start_frame;
    if (recording && out) {
        /* The VBlank's buffer, taken whole: with MEMORIES_CLOCK=interrupt
         * the timer may append to it at any moment. */
        static PadEvent taken[PENDING_MAX];
        unsigned count;
        sigset_t set, previous;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, &previous);
        count = pending_count;
        memcpy(taken, pending, count * sizeof(*taken));
        pending_count = 0;
        sigprocmask(SIG_SETMASK, &previous, NULL);
        if (!clock_written) {
            /* Only the virtual clock replays: a run on the real-time clock
             * met each VBlank at its own moment (notes/agent-control.md). */
            clock_written = 1;
            fprintf(out, "F clock: %s\n", Platform_VirtualClock() ? "virtual" : "real");
            if (!Platform_VirtualClock()) {
                fprintf(stderr, "memories-pc: record: the real-time clock runs: this recording will not play back "
                                "(MEMORIES_DETERMINISTIC=1 makes one that does)\n");
            }
        }
        for (i = 0; i < count; i++) {
            const PadEvent *event = &taken[i];
            if (have_last && !memcmp(event->bits, last_written.bits, sizeof(event->bits)) &&
                !memcmp(event->fixed, last_written.fixed, sizeof(event->fixed)) && event->pad2 == last_written.pad2) {
                continue;
            }
            fprintf(out, "I %u %04x %04x %04x %04x %u\n", event->index, event->bits[0], event->fixed[0],
                    event->bits[1], event->fixed[1], event->pad2);
            last_written = *event;
            have_last = 1;
        }
        if (dropped) {
            fprintf(stderr, "memories-pc: record: %u VBlanks of input were dropped (too many between two frames)\n",
                    dropped);
            dropped = 0;
        }
        fprintf(out, "H %u %u %016llx\n", index, frame, Memories_VramHash());
        /* A .yfmreplay carries no checkpoint (its parts folder goes when it is packed). */
        if (states_every && !zip_path[0] && index - last_state >= states_every) {
            char path[700];
            snprintf(path, sizeof(path), "%s.%u.state", out_path, index);
            if (!Memories_StateSaveHere(path)) {
                fprintf(out, "S %u %u %s\n", index, frame, path);
                last_state = index;
            }
        }
        fflush(out);
    }
    point_index = index;
    point_frame = frame;
    /* The client's commands taken at this point, or at one passed (a play
     * that took another path, which the check reports by its frames). */
    while (playing && command_at < command_count &&
           (commands[command_at].index < index ||
            (commands[command_at].index == index && commands[command_at].frame <= frame))) {
        Command *command = &commands[command_at++];
        if (command->index != index || command->frame != frame) {
            fprintf(stderr, "memories-pc: replay: the command of VBlank %u frame %u comes at VBlank %u frame %u\n",
                    command->index, command->frame, index, frame);
        }
        if (recording && out) {
            fprintf(out, "C %u %u %s\n", command->index, command->frame, command->line);
            fflush(out);
        }
        if (Control_Apply(command->line)) {
            fprintf(stderr, "memories-pc: replay: the game refused the command of VBlank %u: %.80s\n",
                    command->index, command->line);
        }
    }
    /* The end: its VBlank and its frame (a game closed between two VBlanks
     * presents twice in the last one). A player's replay in a window pauses
     * there, and offers the way back; any other play ends the game, as does
     * one with a control client (MEMORIES_CONTROL): a script that plays a
     * replay waits for the game to end, and a pause (which answers a client's
     * info and quit, control.c) is no end. A restart drops MEMORIES_CONTROL (Platform_RestartGame),
     * so File > Play replay... keeps its pause, unless a scripted check
     * puts the variable back through MEMORIES_RESTART_ENV: then it expects
     * a client and the play ends the game. The rule keys on the variable and
     * not on an attached client: the variable is there from the game's
     * start, and the game waits for the client at start
     * (MEMORIES_CONTROL_WAIT), so a client that is late, or has left, does
     * not turn the end into a pause that a script then waits on forever. */
    if (playing && play_has_end && (index > play_end || (index == play_end && frame >= play_end_frame))) {
        const char *control = getenv("MEMORIES_CONTROL");
        fprintf(stderr, "memories-pc: replay: the recording ends at VBlank %u; done\n", play_end);
        playing = 0;
        if (player_play && !getenv("MEMORIES_HEADLESS") && !(control && *control)) {
            Platform_SetClockRate(0);
            notify("Replay finished", "The replay is over. File > Stop replay goes back to the title.", NULL, NULL);
        } else {
            Platform_RequestQuit();
        }
    }
}

/* Recording from here: the virtual clock and the stepped mixer from this
 * VSync on, a start state taken here and loaded at once, so that the
 * recording begins as a play of it does: at the first VSync after a load,
 * with only what the state holds. `path` is a .yfmreplay, packed when the
 * recording stops (replay_file.h), or a plain recording, its start state
 * beside it as `path`.start.state. */
int Recorder_Start(const char *path, char *why, size_t why_size)
{
    char recording_path[sizeof(out_path)], state[sizeof(out_path) + 16];
    int packed = ReplayFile_Is(path);
    if (recording || playing || player_play) {
        snprintf(why, why_size, recording ? "Already recording." :
                 playing ? "A replay is playing." : "A replay was played: File > Stop replay goes back to the title first.");
        return -1;
    }
    if (strlen(path) >= sizeof(zip_path)) {
        snprintf(why, why_size, "The path is too long.");
        return -1;
    }
    /* The start state is a save: a tower whose mod turns save states off
     * refuses it (state.c's save does too, but says only "not saved"). Asked
     * first, so nothing is set up and the player is told why. */
    if (Tower_Guard(TOWER_GUARD_SAVE_STATE, 0, why, why_size)) return -1;
    if (packed) {
        if (ReplayFile_Begin(path, recording_path, sizeof(recording_path), state, sizeof(state), why, why_size)) {
            ReplayFile_Abandon(path); /* a copy that failed halfway left its folder */
            return -1;
        }
    } else {
        snprintf(recording_path, sizeof(recording_path), "%s", path);
        snprintf(state, sizeof(state), "%s.start.state", path);
    }
    snprintf(out_path, sizeof(out_path), "%s", recording_path);
    if (!(out = fopen(recording_path, "w"))) {
        snprintf(why, why_size, "Could not write %.200s.", recording_path);
        if (packed) ReplayFile_Abandon(path);
        return -1;
    }
    Platform_SetSteppedAudio(1);
    Platform_SetVirtualClock(1);
    if (Memories_StateSaveHere(state)) {
        snprintf(why, why_size, "The start state was not saved (not at a state point: try again).");
        fclose(out);
        out = NULL;
        remove(recording_path);
        Platform_SetVirtualClock(0);
        Platform_SetSteppedAudio(0);
        if (packed) ReplayFile_Abandon(path);
        return -1;
    }
    write_header(packed ? "start.state" : state);
    snprintf(zip_path, sizeof(zip_path), "%s", packed ? path : "");
    recording = 1;
    player_record = 1;
    started = 0;
    phase = WAITING;
    have_last = 0;
    pending_count = dropped = 0;
    clock_written = 0;
    last_state = 0;
    fprintf(stderr, "memories-pc: record: recording to %s from a state taken at VBlank %u\n", path,
            Platform_VBlankCount());
    /* Returns only when the load was refused. */
    Memories_StateLoadHere(state, why, why_size);
    zip_path[0] = '\0';
    stop_recording(1);
    remove(recording_path);
    if (packed) ReplayFile_Abandon(path);
    return -1;
}

/* Ends a recording: 0 when it is on its way to a file (or needs none), -1
 * when it was not saved (with a notice). The time it takes the game's thread
 * is in the log: the pack itself is not part of it. */
static int stop_recording(int quiet)
{
    struct timespec began, ended;
    int failed;
    clock_gettime(CLOCK_MONOTONIC, &began);
    failed = end_recording(quiet, 0);
    clock_gettime(CLOCK_MONOTONIC, &ended);
    fprintf(stderr, "memories-pc: record: stopped, %.2f ms on the game's thread\n",
            (double)(ended.tv_sec - began.tv_sec) * 1000.0 + (double)(ended.tv_nsec - began.tv_nsec) / 1e6);
    player_record = 0;
    if (!playing) {
        Platform_SetVirtualClock(0);
        Platform_SetSteppedAudio(0);
    }
    return failed;
}

int Recorder_Stop(void)
{
    if (!recording) return -1;
    stop_recording(0); /* "Replay saved" comes when the pack is done (packs_ended) */
    return 0;
}

int Recorder_Recording(void) { return recording; }
int Recorder_PlayingReplay(void) { return player_play; }

void Recorder_RequestStart(void)
{
#ifdef __ANDROID__
    /* The phone keeps its mixer thread (platform_common.c): no replay there yet. */
    notify("Recording not started", "Replays are not on Android yet.", NULL, NULL);
#else
    if (player_play) {
        notify("Recording not started", "A replay was played: File > Stop replay goes back to the title first.",
               NULL, NULL);
    } else if (!recording && !playing) {
        start_asked = 1;
    }
#endif
}

int Recorder_BootRequest(const char *path, char *why, size_t why_size)
{
#ifdef __ANDROID__
    (void)path;
    snprintf(why, why_size, "Replays are not on Android yet.");
    return -1;
#else
    char file[600];
    if (recording || playing || player_play) {
        snprintf(why, why_size, recording ? "Already recording." :
                 playing ? "A replay is playing." : "A replay was played: File > Stop replay goes back to the title first.");
        return -1;
    }
    if (path) {
        if (!ReplayFile_Is(path)) {
            snprintf(why, why_size, "A recording from boot is a .yfmreplay.");
            return -1;
        }
        if (strlen(path) >= sizeof(file)) {
            snprintf(why, why_size, "The path is too long.");
            return -1;
        }
        snprintf(file, sizeof(file), "%s", path);
    } else if (ReplayFile_NewName(file, sizeof(file))) {
        snprintf(why, why_size, "No name could be made for the replay in %s.", Paths_UserDir());
        return -1;
    }
    if (ReplayFile_SetNextRecord(file)) {
        snprintf(why, why_size, "The replay could not be named for the restart in %s.", Paths_UserDir());
        return -1;
    }
    return 0;
#endif
}

int Recorder_BootRestart(char *why, size_t why_size)
{
#ifdef __ANDROID__
    snprintf(why, why_size, "Replays are not on Android yet.");
    return -1;
#else
    /* Does not return when the game restarts: the next start takes the name
     * (Recorder_Init) and records from its first frame. */
    if (Platform_RestartGame() >= 0) return 0;
    ReplayFile_DropNextRecord(); /* the request is taken back: no later start may record from it */
    snprintf(why, why_size, "The game could not restart itself.");
    return -1;
#endif
}

void Recorder_HostAction(const char *what)
{
    if (recording && player_record) {
        char saved[sizeof(zip_path)];
        snprintf(saved, sizeof(saved), "%s", zip_path);
        if (!stop_recording(1)) {
            notify("Recording stopped",
                   "%s is not part of a replay, so the recording ended just before it. The replay is being saved to %s", what,
                   saved);
        }
    } else if (playing && player_play) {
        playing = 0; /* the pads are the player's again */
        notify("Replay stopped", "%s ended the replay. File > Stop replay goes back to the title.", what, NULL);
    }
}

void Recorder_BeforeRestart(void)
{
    if (recording) Recorder_Stop();
    Recorder_FinishPacking(); /* the restart ends this process, threads and all */
}

int Recorder_TakeNotice(char *title, size_t title_size, char *text, size_t text_size)
{
    packs_ended();
    if (!notice_pending) return 0;
    notice_pending = 0;
    snprintf(title, title_size, "%s", notice_title);
    snprintf(text, text_size, "%s", notice_text);
    return 1;
}