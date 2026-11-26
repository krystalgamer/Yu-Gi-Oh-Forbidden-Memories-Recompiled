#ifndef MEMORIES_PC_DEBUG_REPLAY_FILE_H
#define MEMORIES_PC_DEBUG_REPLAY_FILE_H
/* A player's replay as one file (notes/replays.md): a .yfmreplay, the zip
 * tools/pc/replay.py reads, holding
 *
 *     replay.json      format 1, kind "recorded", the header (build, OS,
 *                      settings, mods, clock) and where it starts
 *     recording.txt    the recorder's lines (recorder.h), starting from
 *                      "start state start.state" (or "start boot"), with no
 *                      C line
 *     start.state      the save state the recording starts from; none in a
 *                      recording from boot (replay.json: "start": boot), so
 *                      that nothing of the game's RAM is in the file
 *     progress/...     the player's progress as it was when the recording
 *                      began: the save slots and what they keep beside them,
 *                      and the memory card images (paths.h, Paths_IsProgress)
 *
 * Recording, the parts are written into a folder beside the file,
 * "<file>.parts", and packed when the recording stops. Playing, the file is
 * read with the hardened zip reader of Import mod... (src/pc/mods/import.c:
 * no name that leaves its folder, no link, sizes and CRCs checked) and
 * unpacked into the user directory's replays/.playing, whose progress/ the
 * game then uses as the player's (Paths_SetProgressDir): nothing a replay
 * saves reaches the player's own saves. A stranger's start state is loaded
 * as a save state is, with the same checks (state.c). */
#include <stddef.h>

/* The working folder for a recording that becomes `file`: the paths of its
 * recording.txt and start.state (where a recording from here writes its
 * start state; one from boot never does), and the player's progress copied
 * in. 0, or -1 with why. */
int ReplayFile_Begin(const char *file, char *recording, size_t recording_size, char *state, size_t state_size,
                     char *why, size_t why_size);
/* Packs the working folder into `file` and removes it: 0, or -1 with why
 * (the folder is then kept, for the player to look at). */
int ReplayFile_Finish(const char *file, char *why, size_t why_size);
/* Removes the working folder of a recording that did not start. */
void ReplayFile_Abandon(const char *file);
/* Unpacks `file` for playing: the paths of its recording and start state
 * (empty when it starts from boot) and of the progress folder to use.
 * 0, or -1 with why. */
int ReplayFile_Open(const char *file, char *recording, size_t recording_size, char *state, size_t state_size,
                    char *progress, size_t progress_size, char *why, size_t why_size);
/* Whether `path` names a .yfmreplay. */
int ReplayFile_Is(const char *path);
/* A new file's name in the user directory's replays/, by the time. */
int ReplayFile_NewName(char *path, size_t size);
/* File > Play replay... restarts the game into the replay: the file is
 * named in replays/next.txt with the time it was made, which the next start
 * takes and removes. A marker is an order only when it is fresh (under a
 * minute old) and could be removed: the leftover of a restart that never
 * came is dropped, with a line on stderr, and not taken. */
int ReplayFile_SetNext(const char *file);
int ReplayFile_TakeNext(char *file, size_t size);
/* Game > Record replay from boot...: the same, for the file the next start
 * records to from its first frame (replays/next-record.txt). */
int ReplayFile_SetNextRecord(const char *file);
int ReplayFile_TakeNextRecord(char *file, size_t size);
/* A restart that failed takes its marker back (its Set did not come true). */
void ReplayFile_DropNext(void);
void ReplayFile_DropNextRecord(void);
#endif
