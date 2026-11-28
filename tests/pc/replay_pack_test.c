/* A recording's .yfmreplay packed on a thread of its own (replay_file.h,
 * ReplayFile_PackStart): the working folder in, the zip out, the game's
 * thread free in between.
 *
 *   - the file appears whole and at once (its .partial is renamed), the
 *     working folder goes, and the zip holds what the recorder left;
 *   - a pack that fails leaves no file and no .partial, and keeps the folder;
 *   - while a pack runs, its folder is not touched: a new recording to the
 *     same file is refused, Abandon leaves it, a new name is not its name;
 *   - several packs run at once, up to the slots there are; one past them is
 *     refused (the recorder then packs on its own thread);
 *   - the waits are bounded, and a pack ended is taken once;
 *   - "the same file" is the file, not its spelling: other spellings of one
 *     path are refused while it is packed too;
 *   - the leftovers of games that did not finish go at startup, and nothing
 *     else (ReplayFile_RemoveLeftovers);
 *   - the recorder's notices queue (notice_queue.h): a failure is never taken
 *     by a later notice.
 *
 * The thread is held at its start by the test's hook, so none of this
 * depends on how fast the zip is made. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "pc/debug/notice_queue.h"
#include "pc/debug/replay_file.h"
#include "pc/mods/import.h"
#include "pc/platform/paths.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "scratch.h"

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition);                             \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char root[1024];
static int gate; /* atomic: the held threads go on when it is 1 */
static int started; /* atomic: how many threads reached the hook */

static void nap(long milliseconds)
{
    struct timespec span = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&span, NULL);
}

static void hold(const char *file)
{
    (void)file;
    __atomic_add_fetch(&started, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&gate, __ATOMIC_ACQUIRE)) nap(1);
}

static void hold_threads(void)
{
    __atomic_store_n(&gate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
}

static void release_threads(void) { __atomic_store_n(&gate, 1, __ATOMIC_RELEASE); }

static int exists(const char *path)
{
    struct stat info;
    return !stat(path, &info);
}

static int is_directory(const char *path)
{
    struct stat info;
    return !stat(path, &info) && S_ISDIR(info.st_mode);
}

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file || fwrite(data, 1, size, file) != size || fclose(file)) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
}

/* What a recording leaves in its working folder: recording.txt (from a state
 * or from boot), start.state when from a state, and the progress copied in. */
static const char RECORDING[] =
    "yfm-recording 1\nbuild 0badf00d\nstart state start.state\nF build: test\nF os: test\n"
    "F settings: language=2 a=b\nF mods: none\nF clock: virtual\nI 3 0040 0000 0000 0000 0\nH 3 3 0123456789abcdef\n"
    "E 4 4\n";
static const char BOOT_RECORDING[] =
    "yfm-recording 1\nbuild 0badf00d\nstart boot\nF build: test\nF os: test\nF settings: \nF mods: none\n"
    "F clock: virtual\nH 0 0 0123456789abcdef\nE 1 1\n";
#define STATE_SIZE (3u << 20)
static unsigned char state_bytes[STATE_SIZE];

/* A working folder as ReplayFile_Begin and the recorder leave it. `from_state`
 * 0 is a recording from boot. 0 or -1 */
static int make_recording(const char *file, int from_state, size_t *recording_size)
{
    char recording[1100], state[1100], why[300];
    const char *text = from_state ? RECORDING : BOOT_RECORDING;
    if (ReplayFile_Begin(file, recording, sizeof(recording), state, sizeof(state), why, sizeof(why))) {
        fprintf(stderr, "begin: %s\n", why);
        return -1;
    }
    write_file(recording, text, strlen(text));
    if (from_state) write_file(state, state_bytes, sizeof(state_bytes));
    *recording_size = strlen(text);
    return 0;
}

static void path_in(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", root, name); }

/* The zip's entry `name` read back: its bytes in a new buffer, or NULL. */
static unsigned char *entry(const char *zip, const char *name, size_t *size)
{
    char why[300], path[1100];
    ModsImport *import = Mods_ImportOpen(zip, why, sizeof(why));
    unsigned char *bytes = NULL;
    FILE *file;
    long length;
    snprintf(path, sizeof(path), "%s/entry.tmp", root);
    if (!import) return NULL;
    if (Mods_ImportExtract(import, name, path, 64ul << 20, why, sizeof(why)) > 0 && (file = fopen(path, "rb")) != NULL) {
        fseek(file, 0, SEEK_END);
        length = ftell(file);
        fseek(file, 0, SEEK_SET);
        bytes = malloc((size_t)length + 1);
        if (bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length) {
            *size = (size_t)length;
            bytes[length] = 0;
        } else {
            free(bytes);
            bytes = NULL;
        }
        fclose(file);
    }
    Mods_ImportClose(import);
    remove(path);
    return bytes;
}

static int has_entry(const char *zip, const char *name)
{
    size_t size;
    unsigned char *bytes = entry(zip, name, &size);
    free(bytes);
    return bytes != NULL;
}

static int wait_for_all(unsigned milliseconds) { return ReplayFile_PackWait(NULL, milliseconds); }

/* Takes what ended until nothing has: how many were saved, and how many failed. */
static void take_all(int *saved, int *failed, char *last_why, size_t why_size)
{
    char file[1100], why[400];
    int quiet, got;
    *saved = *failed = 0;
    while ((got = ReplayFile_PackTake(file, sizeof(file), &quiet, why, sizeof(why))) != 0) {
        if (got > 0) (*saved)++;
        else {
            (*failed)++;
            if (last_why) snprintf(last_why, why_size, "%s", why);
        }
    }
}

static void whole_and_at_once(void)
{
    char file[1100], partial[1200], parts[1200], why[400], taken[1100];
    size_t recording_size, size = 0;
    unsigned char *bytes;
    int quiet = 7, got;
    CHECK(!ReplayFile_NewName(file, sizeof(file)));
    snprintf(partial, sizeof(partial), "%s.partial", file);
    snprintf(parts, sizeof(parts), "%s.parts", file);
    CHECK(!make_recording(file, 1, &recording_size));
    write_file(partial, "junk of a game that was killed", 30); /* a leftover is no obstacle */
    hold_threads();
    ReplayFile_SetPackHook(hold);
    CHECK(!ReplayFile_PackStart(file, recording_size, 1, why, sizeof(why)));
    CHECK(ReplayFile_PackRunning(file) == 1 && ReplayFile_PackRunning(NULL) == 1);
    CHECK(!ReplayFile_PackRunning("elsewhere.yfmreplay"));
    CHECK(!exists(file)); /* not there until it is whole */
    CHECK(ReplayFile_PackWait(file, 30) == 0); /* bounded: the thread is held */
    CHECK(ReplayFile_PackTake(taken, sizeof(taken), &quiet, why, sizeof(why)) == 0);
    /* The folder is the job's: no new recording to this file, no Abandon... */
    {
        char recording[1100], state[1100];
        CHECK(ReplayFile_Begin(file, recording, sizeof(recording), state, sizeof(state), why, sizeof(why)) == -1);
        CHECK(strstr(why, "still being packed") != NULL);
    }
    ReplayFile_Abandon(file);
    CHECK(is_directory(parts));
    /* ... and a new name is not this one's, though its file does not exist yet. */
    {
        char second[1100];
        CHECK(!ReplayFile_NewName(second, sizeof(second)));
        CHECK(strcmp(file, second) != 0);
    }
    release_threads();
    CHECK(ReplayFile_PackWait(file, 20000) == 1);
    got = ReplayFile_PackTake(taken, sizeof(taken), &quiet, why, sizeof(why));
    CHECK(got == 1 && !strcmp(taken, file) && quiet == 1);
    CHECK(ReplayFile_PackTake(taken, sizeof(taken), &quiet, why, sizeof(why)) == 0); /* taken once */
    CHECK(exists(file) && !exists(partial) && !exists(parts));
    CHECK(has_entry(file, "replay.json") && has_entry(file, "recording.txt") && has_entry(file, "start.state"));
    CHECK(has_entry(file, "progress/saves/slot01.sav") && has_entry(file, "progress/memcard1.mcd"));
    bytes = entry(file, "recording.txt", &size);
    CHECK(bytes && size == recording_size && !memcmp(bytes, RECORDING, size));
    free(bytes);
    bytes = entry(file, "start.state", &size);
    CHECK(bytes && size == sizeof(state_bytes) && !memcmp(bytes, state_bytes, size));
    free(bytes);
    bytes = entry(file, "replay.json", &size);
    CHECK(bytes && strstr((char *)bytes, "\"kind\": \"state\"") && strstr((char *)bytes, "\"language\": \"2\""));
    free(bytes);
    ReplayFile_SetPackHook(NULL);
}

static void replaces_an_old_file_and_boot_has_no_state(void)
{
    char file[1100], why[400];
    size_t recording_size, size = 0;
    unsigned char *bytes;
    path_in(file, sizeof(file), "boot.yfmreplay");
    write_file(file, "an older replay of the same name", 32);
    CHECK(!make_recording(file, 0, &recording_size));
    CHECK(!ReplayFile_PackStart(file, recording_size, 0, why, sizeof(why)));
    CHECK(ReplayFile_PackWait(file, 20000) == 1);
    {
        int saved, failed;
        take_all(&saved, &failed, NULL, 0);
        CHECK(saved == 1 && !failed);
    }
    CHECK(has_entry(file, "replay.json") && !has_entry(file, "start.state") && has_entry(file, "recording.txt"));
    bytes = entry(file, "replay.json", &size);
    CHECK(bytes && strstr((char *)bytes, "\"kind\": \"boot\""));
    free(bytes);
}

static void failures_leave_no_file(void)
{
    char file[1100], partial[1200], parts[1200], state[1300], recording[1300], why[400], last[400] = "";
    size_t recording_size;
    int saved, failed;
    path_in(file, sizeof(file), "lost.yfmreplay");
    snprintf(partial, sizeof(partial), "%s.partial", file);
    snprintf(parts, sizeof(parts), "%s.parts", file);
    /* A recording from a state whose state is gone. */
    CHECK(!make_recording(file, 1, &recording_size));
    snprintf(state, sizeof(state), "%s/start.state", parts);
    CHECK(!remove(state));
    CHECK(!ReplayFile_PackStart(file, recording_size, 0, why, sizeof(why)));
    CHECK(ReplayFile_PackWait(file, 20000) == 1);
    take_all(&saved, &failed, last, sizeof(last));
    CHECK(!saved && failed == 1 && strstr(last, "start state") != NULL);
    CHECK(!exists(file) && !exists(partial) && is_directory(parts));
    /* A recording that is not the length it was at the stop. */
    path_in(file, sizeof(file), "grown.yfmreplay");
    snprintf(partial, sizeof(partial), "%s.partial", file);
    snprintf(parts, sizeof(parts), "%s.parts", file);
    CHECK(!make_recording(file, 0, &recording_size));
    snprintf(recording, sizeof(recording), "%s/recording.txt", parts);
    CHECK(!ReplayFile_PackStart(file, recording_size + 1, 0, why, sizeof(why)));
    CHECK(ReplayFile_PackWait(file, 20000) == 1);
    last[0] = '\0';
    take_all(&saved, &failed, last, sizeof(last));
    CHECK(!saved && failed == 1 && strstr(last, "changed") != NULL);
    CHECK(!exists(file) && !exists(partial) && is_directory(parts));
    /* A folder that is not there. */
    path_in(file, sizeof(file), "never.yfmreplay");
    CHECK(!ReplayFile_PackStart(file, 10, 0, why, sizeof(why)));
    CHECK(ReplayFile_PackWait(file, 20000) == 1);
    last[0] = '\0';
    take_all(&saved, &failed, last, sizeof(last));
    CHECK(!saved && failed == 1 && strstr(last, "recording.txt") != NULL);
}

static void several_at_once_up_to_the_slots(void)
{
    char names[8][1100], why[400];
    size_t recording_size;
    int i, started_ok = 0, refused = 0, saved, failed;
    hold_threads();
    ReplayFile_SetPackHook(hold);
    for (i = 0; i < 8; i++) {
        char name[64];
        snprintf(name, sizeof(name), "many-%d.yfmreplay", i);
        path_in(names[i], sizeof(names[i]), name);
        CHECK(!make_recording(names[i], 0, &recording_size));
    }
    for (i = 0; i < 8; i++) {
        if (!ReplayFile_PackStart(names[i], recording_size, 0, why, sizeof(why))) started_ok++;
        else {
            refused++;
            CHECK(strstr(why, "at once") != NULL);
        }
    }
    /* Held, none ends: the slots there are are the packs that start, the rest are refused. */
    CHECK(started_ok == REPLAY_PACK_SLOTS && refused == 8 - REPLAY_PACK_SLOTS);
    CHECK(ReplayFile_PackRunning(NULL) == started_ok);
    CHECK(wait_for_all(30) == 0);
    release_threads();
    CHECK(wait_for_all(20000) == 1);
    take_all(&saved, &failed, NULL, 0);
    CHECK(saved == started_ok && !failed);
    /* The refused ones are packed as they always were: on the caller's thread. */
    for (i = 0; i < 8; i++) {
        if (!exists(names[i])) {
            CHECK(!ReplayFile_Finish(names[i], why, sizeof(why)));
            CHECK(exists(names[i]));
        }
    }
    ReplayFile_SetPackHook(NULL);
    CHECK(wait_for_all(10) == 1); /* nothing runs: no wait */
}


/* The same file under other names: while its pack runs, none of them gets its folder. */
static void other_spellings_of_a_file(void)
{
    char file[1100], other[1100], here[1100], why[400], recording[1100], state[1100], taken[1100];
    char spellings[8][1300];
    size_t recording_size, n = 0, i;
    int quiet, in_here;
    path_in(file, sizeof(file), "spell.yfmreplay");
    CHECK(!make_recording(file, 0, &recording_size));
    hold_threads();
    ReplayFile_SetPackHook(hold);
    CHECK(!ReplayFile_PackStart(file, recording_size, 0, why, sizeof(why)));
    snprintf(spellings[n++], sizeof(spellings[0]), "%s/./spell.yfmreplay", root);
    snprintf(spellings[n++], sizeof(spellings[0]), "%s//spell.yfmreplay", root);
    snprintf(spellings[n++], sizeof(spellings[0]), "%s/nowhere/../spell.yfmreplay", root);
#ifdef _WIN32
    /* Backslashes, and Windows does not tell A from a. */
    snprintf(spellings[n], sizeof(spellings[0]), "%s/spell.yfmreplay", root);
    for (i = 0; spellings[n][i]; i++)
        spellings[n][i] = spellings[n][i] == '/' ? '\\' : (char)toupper((unsigned char)spellings[n][i]);
    n++;
#elif defined(__APPLE__)
    snprintf(spellings[n++], sizeof(spellings[0]), "%s/SPELL.YFMREPLAY", root);
#else
    /* Another name for the folder: a link to it. */
    snprintf(other, sizeof(other), "%s/linked", root);
    if (!symlink(root, other)) snprintf(spellings[n++], sizeof(spellings[0]), "%s/linked/spell.yfmreplay", root);
    CHECK(n == 4);
#endif
    /* Names relative to where the game runs: from root. */
    in_here = n;
    snprintf(spellings[n++], sizeof(spellings[0]), "spell.yfmreplay");
    snprintf(spellings[n++], sizeof(spellings[0]), "./spell.yfmreplay");
    CHECK(getcwd(here, sizeof(here)) != NULL);
    for (i = 0; i < n; i++) {
        const char *name = spellings[i];
        if ((int)i >= in_here) CHECK(!chdir(root));
        CHECK(ReplayFile_PackRunning(name) == 1);
        CHECK(ReplayFile_Begin(name, recording, sizeof(recording), state, sizeof(state), why, sizeof(why)) == -1);
        CHECK(strstr(why, "still being packed") != NULL);
        CHECK(ReplayFile_PackStart(name, recording_size, 0, why, sizeof(why)) == -1);
        CHECK(ReplayFile_Finish(name, why, sizeof(why)) == -1);
        ReplayFile_Abandon(name);
        if ((int)i >= in_here) CHECK(!chdir(here));
    }
    {
        char parts[1200];
        snprintf(parts, sizeof(parts), "%s.parts", file);
        CHECK(is_directory(parts)); /* none of it touched the folder of the pack */
        snprintf(other, sizeof(other), "%s/spell2.yfmreplay", root);
        CHECK(!ReplayFile_PackRunning(other)); /* a name of its own is another file */
    }
    release_threads();
    CHECK(ReplayFile_PackWait(file, 20000) == 1);
    CHECK(ReplayFile_PackTake(taken, sizeof(taken), &quiet, why, sizeof(why)) == 1);
    CHECK(exists(file));
    CHECK(!ReplayFile_PackRunning(spellings[0])); /* ended: no spelling is refused */
#if !defined(_WIN32) && !defined(__APPLE__)
    snprintf(other, sizeof(other), "%s/linked", root);
    unlink(other);
#endif
    ReplayFile_SetPackHook(NULL);
}

/* --- the leftovers of games that did not finish --------------------------- */

static void age(const char *path, long seconds)
{
    struct utimbuf times;
    times.actime = times.modtime = time(NULL) - seconds;
    if (utime(path, &times)) {
        fprintf(stderr, "cannot age %s\n", path);
        failures++;
    }
}

#define DAY (24L * 3600L)

/* A pack folder as a cut-off game leaves it: recording.txt and progress/saves/slot01.sav in it. */
static void make_parts(const char *folder, long recording_age, long other_age)
{
    char path[1400], progress[1400];
    snprintf(progress, sizeof(progress), "%s/progress/saves", folder);
    CHECK(!Paths_MakeDirs(progress));
    snprintf(path, sizeof(path), "%s/recording.txt", folder);
    write_file(path, "yfm-recording 1\n", 16);
    age(path, recording_age);
    snprintf(path, sizeof(path), "%s/slot01.sav", progress);
    write_file(path, "slot", 4);
    age(path, other_age);
}

static void make_old_file(const char *path)
{
    write_file(path, "a pack cut off", 14);
    age(path, 2 * DAY);
}

static void drop_link(const char *link)
{
#ifdef _WIN32
    rmdir(link); /* a junction: the link only */
#else
    unlink(link);
#endif
}

/* A link to a folder, as the system makes it (a junction on Windows): 1 when there is one now. */
static int make_link(const char *link, const char *target)
{
#ifdef _WIN32
    char command[3000], from[1400], to[1400];
    size_t i;
    snprintf(from, sizeof(from), "%s", link);
    snprintf(to, sizeof(to), "%s", target);
    for (i = 0; from[i]; i++)
        if (from[i] == '/') from[i] = '\\';
    for (i = 0; to[i]; i++)
        if (to[i] == '/') to[i] = '\\';
    snprintf(command, sizeof(command), "cmd /c mklink /J \"%s\" \"%s\" > NUL 2>&1", from, to);
    (void)!system(command);
#else
    (void)!symlink(target, link);
#endif
    return Mods_ImportIsLink(link);
}

static void leftovers_go_and_nothing_else(void)
{
    char replays[1100], p[1500], q[1500], keep[1500], why[400], outside[1100], target[1100];
    size_t recording_size;
    int removed, links;
    snprintf(replays, sizeof(replays), "%s/user/replays", root);
    CHECK(!Paths_MakeDirs(replays));
    /* What goes: old, and of a name this game makes. */
    snprintf(p, sizeof(p), "%s/old.yfmreplay.partial", replays);
    make_old_file(p);
    snprintf(p, sizeof(p), "%s/Upper.YFMREPLAY.PARTIAL", replays);
    make_old_file(p);
    snprintf(p, sizeof(p), "%s/old.yfmreplay.parts", replays);
    make_parts(p, 2 * DAY, 2 * DAY);
    /* What stays: the replay itself, however old. */
    snprintf(p, sizeof(p), "%s/done.yfmreplay", replays);
    make_old_file(p);
    /* Fresh: another game's pack under way, and a recording another game is still writing (its
     * recording.txt is the newest file, its start state is old). */
    snprintf(p, sizeof(p), "%s/fresh.yfmreplay.partial", replays);
    write_file(p, "being written", 13);
    snprintf(p, sizeof(p), "%s/live.yfmreplay.parts", replays);
    make_parts(p, 5, 2 * DAY);
    /* Not ours by name or by kind. */
    snprintf(p, sizeof(p), "%s/note.txt", replays);
    make_old_file(p);
    snprintf(p, sizeof(p), "%s/x.partial", replays);
    make_old_file(p);
    snprintf(p, sizeof(p), "%s/x.parts", replays);
    make_parts(p, 2 * DAY, 2 * DAY);
    snprintf(p, sizeof(p), "%s/odd.yfmreplay.parts", replays); /* a file, not a folder */
    make_old_file(p);
    snprintf(p, sizeof(p), "%s/odd2.yfmreplay.partial", replays); /* a folder, not a file */
    make_parts(p, 2 * DAY, 2 * DAY);
    /* The recording this start takes over, named another way than its folder is. */
    snprintf(p, sizeof(p), "%s/take.yfmreplay.partial", replays);
    make_old_file(p);
    snprintf(p, sizeof(p), "%s/take.yfmreplay.parts", replays);
    make_parts(p, 2 * DAY, 2 * DAY);
    snprintf(keep, sizeof(keep), "%s/./take.yfmreplay", replays);
    /* A pack of this process that is under way, its folder old-looking notwithstanding. */
    snprintf(p, sizeof(p), "%s/busy.yfmreplay", replays);
    CHECK(!make_recording(p, 0, &recording_size));
    snprintf(q, sizeof(q), "%s.parts/recording.txt", p);
    age(q, 2 * DAY);
    snprintf(q, sizeof(q), "%s.parts/progress/saves/slot01.sav", p);
    age(q, 2 * DAY);
    snprintf(q, sizeof(q), "%s.parts/progress/memcard1.mcd", p);
    age(q, 2 * DAY);
    hold_threads();
    ReplayFile_SetPackHook(hold);
    CHECK(!ReplayFile_PackStart(p, recording_size, 0, why, sizeof(why)));
    /* Outside replays/, and in it but not a pack's. */
    snprintf(outside, sizeof(outside), "%s/outside.yfmreplay.parts", root);
    make_parts(outside, 2 * DAY, 2 * DAY);
    snprintf(q, sizeof(q), "%s/.playing", replays);
    make_parts(q, 2 * DAY, 2 * DAY);
    /* Links are not followed: one by a pack's name, and one inside a pack's folder. What they lead
     * to holds old files that must not go, and the folder that holds a link is left whole. */
    snprintf(target, sizeof(target), "%s/link-target", root);
    make_parts(target, 2 * DAY, 2 * DAY);
    snprintf(q, sizeof(q), "%s/link.yfmreplay.parts", replays);
    links = make_link(q, target);
    if (links) {
        snprintf(q, sizeof(q), "%s/inner.yfmreplay.parts", replays);
        CHECK(!Paths_MakeDirs(q));
        snprintf(why, sizeof(why), "%s/inner.yfmreplay.parts/a.txt", replays);
        make_old_file(why);
        snprintf(why, sizeof(why), "%s/inner.yfmreplay.parts/linked", replays);
        CHECK(make_link(why, target));
    } else {
        fprintf(stderr, "replay_pack_test: no link could be made here: the link cases are skipped\n");
    }

    removed = ReplayFile_RemoveLeftovers(keep, 3600);

    snprintf(p, sizeof(p), "%s/old.yfmreplay.partial", replays);
    CHECK(!exists(p));
    snprintf(p, sizeof(p), "%s/Upper.YFMREPLAY.PARTIAL", replays);
    CHECK(!exists(p));
    snprintf(p, sizeof(p), "%s/old.yfmreplay.parts", replays);
    CHECK(!exists(p));
    CHECK(removed == 3);
    snprintf(p, sizeof(p), "%s/done.yfmreplay", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/fresh.yfmreplay.partial", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/live.yfmreplay.parts/progress/saves/slot01.sav", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/note.txt", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/x.partial", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/x.parts/recording.txt", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/odd.yfmreplay.parts", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/odd2.yfmreplay.partial/recording.txt", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/take.yfmreplay.partial", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/take.yfmreplay.parts/recording.txt", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/busy.yfmreplay.parts/recording.txt", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/recording.txt", outside);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/.playing/recording.txt", replays);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/recording.txt", target);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/progress/saves/slot01.sav", target);
    CHECK(exists(p));
    if (links) {
        snprintf(p, sizeof(p), "%s/link.yfmreplay.parts", replays);
        CHECK(Mods_ImportIsLink(p));
        snprintf(p, sizeof(p), "%s/inner.yfmreplay.parts/a.txt", replays);
        CHECK(exists(p));
    }
    /* A second pass has nothing left to remove; without the kept name, the one that was kept goes
     * (the pack of this process, the fresh and the linked stay). */
    CHECK(ReplayFile_RemoveLeftovers(keep, 3600) == 0);
    CHECK(ReplayFile_RemoveLeftovers(NULL, 3600) == 2); /* take.partial and take.parts */
    snprintf(p, sizeof(p), "%s/take.yfmreplay.parts", replays);
    CHECK(!exists(p));
    snprintf(p, sizeof(p), "%s/busy.yfmreplay.parts/recording.txt", replays);
    CHECK(exists(p)); /* its pack still runs */
    release_threads();
    CHECK(ReplayFile_PackWait(NULL, 20000) == 1);
    {
        int saved, failed;
        take_all(&saved, &failed, NULL, 0);
        CHECK(saved == 1 && !failed);
    }
    ReplayFile_SetPackHook(NULL);
    /* The pack's own folder went with it; nothing is left of the ones the pass kept by mistake. */
    snprintf(p, sizeof(p), "%s/busy.yfmreplay.parts", replays);
    CHECK(!exists(p));
    if (links) {
        snprintf(p, sizeof(p), "%s/link.yfmreplay.parts", replays);
        drop_link(p);
        snprintf(p, sizeof(p), "%s/inner.yfmreplay.parts/linked", replays);
        drop_link(p);
    }
    snprintf(p, sizeof(p), "%s/recording.txt", target);
    CHECK(exists(p)); /* still there after the links went */
}

/* --- the notices ---------------------------------------------------------- */

static void notices_queue_and_failures_stay(void)
{
    NoticeQueue queue;
    char title[64], text[700];
    int i;
    memset(&queue, 0, sizeof(queue));
    /* One poll: a pack that failed, then a recording that did not start, then another pack saved. */
    CHECK(!NoticeQueue_Push(&queue, "Replay not saved", "A could not be packed", 1));
    CHECK(!NoticeQueue_Push(&queue, "Recording not started", "no state point", 1));
    CHECK(!NoticeQueue_Push(&queue, "Replay saved", "B", 0));
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(title, "Replay not saved") &&
          !strcmp(text, "A could not be packed"));
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(title, "Recording not started"));
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(title, "Replay saved"));
    CHECK(!NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)));
    /* Full: a failure, then saved ones fill it; a newer failure takes the room of the oldest success. */
    CHECK(!NoticeQueue_Push(&queue, "Replay not saved", "first", 1));
    for (i = 1; i < NOTICE_QUEUE_MAX; i++) {
        snprintf(text, sizeof(text), "saved %d", i);
        CHECK(!NoticeQueue_Push(&queue, "Replay saved", text, 0));
    }
    CHECK(NoticeQueue_Push(&queue, "Replay not saved", "second", 1) == 1);
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(text, "first"));
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(text, "saved 2")); /* "saved 1" went */
    for (i = 0; i < NOTICE_QUEUE_MAX - 3; i++) CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)));
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(text, "second"));
    CHECK(!NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)));
    /* All failures: the oldest goes, the newest stays. */
    for (i = 0; i < NOTICE_QUEUE_MAX; i++) {
        snprintf(text, sizeof(text), "failure %d", i);
        CHECK(!NoticeQueue_Push(&queue, "Replay not saved", text, 1));
    }
    CHECK(NoticeQueue_Push(&queue, "Replay not saved", "newest", 1) == 1);
    CHECK(NoticeQueue_Take(&queue, title, sizeof(title), text, sizeof(text)) && !strcmp(text, "failure 1"));
}

/* `replay_pack_test exit`, a process of its own and cold (no pack has run in it): a pack that is under
 * way when the game ends in exit(), where the recorder's atexit handler waits for it, must finish
 * there. A thread that printed a number while exit() ran hung the Windows process (replay_file.c,
 * pack_thread), and a warm test process, one that had packed before, did not show it. The pack is
 * started in main and held at its start; the handler lets it go and waits. */
static char exit_file[1100];

static void wait_at_exit(void)
{
    char file[1100], why[400];
    int quiet;
    release_threads();
    if (!ReplayFile_PackWait(exit_file, 20000) || !exists(exit_file) ||
        ReplayFile_PackTake(file, sizeof(file), &quiet, why, sizeof(why)) != 1) {
        fprintf(stderr, "replay_pack_test: the pack that was under way at exit did not end\n");
        _Exit(1);
    }
    printf("replay_pack_test: the pack that was under way at exit ended\n");
}

static int exit_case(void)
{
    size_t size;
    char why[400];
    path_in(exit_file, sizeof(exit_file), "at-exit.yfmreplay");
    CHECK(!make_recording(exit_file, 0, &size));
    hold_threads();
    ReplayFile_SetPackHook(hold);
    CHECK(!ReplayFile_PackStart(exit_file, size, 0, why, sizeof(why)));
    atexit(wait_at_exit); /* after scratch_dir's, so it runs first */
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    char progress[1200], user[1100];
    size_t i;
    CHECK(scratch_dir(root, sizeof(root), "memories-replay-pack"));
    snprintf(user, sizeof(user), "%s/user", root);
    CHECK(!setenv("MEMORIES_USER_DIR", user, 1));
    snprintf(progress, sizeof(progress), "%s/saves", user);
    CHECK(!Paths_MakeDirs(progress));
    snprintf(progress, sizeof(progress), "%s/saves/slot01.sav", user);
    write_file(progress, "slot one", 8);
    snprintf(progress, sizeof(progress), "%s/memcard1.mcd", user);
    write_file(progress, "memory card", 11);
    if (argc > 1 && !strcmp(argv[1], "exit")) return exit_case();
    for (i = 0; i < sizeof(state_bytes); i++) state_bytes[i] = (unsigned char)((i * 2654435761u) >> 13);
    whole_and_at_once();
    replaces_an_old_file_and_boot_has_no_state();
    failures_leave_no_file();
    several_at_once_up_to_the_slots();
    other_spellings_of_a_file();
    leftovers_go_and_nothing_else();
    notices_queue_and_failures_stay();
    if (failures) {
        fprintf(stderr, "replay_pack_test: %d checks failed\n", failures);
        return 1;
    }
    printf("replay_pack_test: passed\n");
    return 0;
}
