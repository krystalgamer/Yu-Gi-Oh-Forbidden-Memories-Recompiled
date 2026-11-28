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
 *   - the waits are bounded, and a pack ended is taken once.
 *
 * The thread is held at its start by the test's hook, so none of this
 * depends on how fast the zip is made. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "pc/debug/replay_file.h"
#include "pc/mods/import.h"
#include "pc/platform/paths.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
    CHECK(started_ok >= 2 && started_ok < 8 && refused == 8 - started_ok);
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
    if (failures) {
        fprintf(stderr, "replay_pack_test: %d checks failed\n", failures);
        return 1;
    }
    printf("replay_pack_test: passed\n");
    return 0;
}
