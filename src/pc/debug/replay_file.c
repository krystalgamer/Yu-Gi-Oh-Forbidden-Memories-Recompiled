/* A player's replay as one file (replay_file.h). */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE   /* realpath, under -std=c11 on glibc (mods.c asks the same) */
#include "pc/compat/fs.h"
#include "replay_file.h"
#include "pc/mods/import.h"
#include "pc/platform/paths.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h> /* access, F_OK (compat/fs.h has it on Windows) */

#define RECORDING_MAX (256ul << 20) /* about 50 hours of pad changes and frame hashes */
#define STATE_MAX (64ul << 20)      /* a save state is under 8 MB */
#define PROGRESS_MAX (16ul << 20)   /* one progress file: a slot is 8 KB, a card 128 KB */

static int copy_file(const char *from, const char *to)
{
    static char buffer[65536];
    FILE *in = fopen(from, "rb"), *out;
    size_t got;
    int ok = 1;
    if (!in) return -1;
    if (!(out = fopen(to, "wb"))) {
        fclose(in);
        return -1;
    }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out)) ok = 0;
    if (!ok) remove(to);
    return ok ? 0 : -1;
}

static int is_directory(const char *path)
{
    struct stat info;
    return !stat(path, &info) && S_ISDIR(info.st_mode);
}

static int is_file(const char *path)
{
    struct stat info;
    return !stat(path, &info) && S_ISREG(info.st_mode);
}

/* Calls `each` for every plain file right in `directory` (no deeper: the
 * progress folders are flat). */
static int each_file(const char *directory, int (*each)(const char *path, const char *name, void *context),
                     void *context)
{
    DIR *listing = opendir(directory);
    struct dirent *entry;
    int failed = 0;
    if (!listing) return 0;
    while (!failed && (entry = readdir(listing))) {
        char path[1200];
        if (entry->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path)) continue;
        if (Mods_ImportIsLink(path) || !is_file(path)) continue;
        failed = each(path, entry->d_name, context);
    }
    closedir(listing);
    return failed;
}

static int work_folder(const char *file, char *out, size_t size)
{
    return snprintf(out, size, "%s.parts", file) >= (int)size ? -1 : 0;
}

/* --- one file, however it is spelled ------------------------------------ */

#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetFullPathNameW(const wchar_t *, unsigned long, wchar_t *, wchar_t **);
__declspec(dllimport) unsigned long __stdcall GetLongPathNameW(const wchar_t *, wchar_t *, unsigned long);
#endif
#define CANON_MAX 1300

#ifndef _WIN32
/* "/a//b/./c/../d" as "/a/b/d": a path that starts with "/", spelled one way. */
static void flatten(char *path)
{
    char out[CANON_MAX];
    size_t used = 0;
    const char *at = path;
    while (*at) {
        size_t length;
        while (*at == '/') at++;
        length = strcspn(at, "/");
        if (!length) break;
        if (length == 1 && at[0] == '.') {
            /* here */
        } else if (length == 2 && at[0] == '.' && at[1] == '.') {
            while (used && out[used - 1] != '/') used--;
            if (used) used--;
        } else {
            out[used++] = '/';
            memcpy(out + used, at, length);
            used += length;
        }
        at += length;
    }
    if (!used) out[used++] = '/';
    out[used] = '\0';
    memcpy(path, out, used + 1);
}
#endif

/* The file `path` names, as one string for comparing two names of it: absolute,
 * without "." and "..", one kind of slash, and where it exists with its links
 * followed (the longest part that exists is resolved: the file itself need not
 * be there yet). Where the file system does not tell "A" from "a" (Windows,
 * macOS) also in one case. On Windows the long name of an 8.3 short one, and
 * the extended-length prefix taken off; a junction is not followed there (stat follows it, which is
 * why nothing below removes through one). 0, or -1 when it is not a name or
 * does not fit. */
static int canonical_file(const char *path, char *out, size_t size)
{
    char work[CANON_MAX];
#ifdef _WIN32
    wchar_t *wide, full[CANON_MAX], head[CANON_MAX], longer[CANON_MAX];
    char plain[CANON_MAX], *narrow;
    unsigned long length;
    size_t cut, i;
    if (snprintf(plain, sizeof(plain), "%s", path) >= (int)sizeof(plain)) return -1;
    for (i = 0; plain[i]; i++)
        if (plain[i] == '\\') plain[i] = '/';
    if (!strncmp(plain, "//?/UNC/", 8)) memmove(plain + 2, plain + 8, strlen(plain + 8) + 1);
    else if (!strncmp(plain, "//?/", 4) || !strncmp(plain, "//./", 4)) memmove(plain, plain + 4, strlen(plain + 4) + 1);
    if (!(wide = Memories_Utf8ToWide(plain))) return -1;
    length = GetFullPathNameW(wide, CANON_MAX, full, NULL);
    free(wide);
    if (!length || length >= CANON_MAX) return -1;
    /* A short name ("PROGRA~1") spells the same file as the long one: the longest part that exists. */
    for (cut = wcslen(full); cut > 3;) {
        memcpy(head, full, cut * sizeof(wchar_t));
        head[cut] = 0;
        length = GetLongPathNameW(head, longer, CANON_MAX);
        if (length && length < CANON_MAX && length + wcslen(full + cut) < CANON_MAX) {
            memcpy(longer + length, full + cut, (wcslen(full + cut) + 1) * sizeof(wchar_t));
            memcpy(full, longer, (wcslen(longer) + 1) * sizeof(wchar_t));
            break;
        }
        while (cut > 3 && full[cut - 1] != L'\\') cut--;
        if (cut > 3) cut--;
    }
    for (i = 0; full[i]; i++) full[i] = (wchar_t)towlower(full[i] == L'\\' ? L'/' : full[i]);
    if (!(narrow = Memories_WideToUtf8(full))) return -1;
    length = (unsigned long)snprintf(work, sizeof(work), "%s", narrow);
    free(narrow);
    if (length >= sizeof(work)) return -1;
#else
    char whole[CANON_MAX];
    size_t cut;
    if (path[0] == '/') {
        if (snprintf(whole, sizeof(whole), "%s", path) >= (int)sizeof(whole)) return -1;
    } else {
        char here[CANON_MAX];
        if (!getcwd(here, sizeof(here)) || snprintf(whole, sizeof(whole), "%s/%s", here, path) >= (int)sizeof(whole))
            return -1;
    }
    for (cut = strlen(whole);;) {
        char head[CANON_MAX], *real;
        memcpy(head, whole, cut);
        head[cut] = '\0';
        real = realpath(cut ? head : "/", NULL);
        if (real) {
            int fits = snprintf(work, sizeof(work), "%s/%s", real, whole + cut) < (int)sizeof(work);
            free(real);
            if (!fits) return -1;
            break;
        }
        while (cut > 0 && whole[cut - 1] != '/') cut--;
        while (cut > 0 && whole[cut - 1] == '/') cut--;
    }
    flatten(work);
#ifdef __APPLE__
    {
        size_t i;
        for (i = 0; work[i]; i++) work[i] = (char)tolower((unsigned char)work[i]);
    }
#endif
#endif
    return (size_t)snprintf(out, size, "%s", work) >= size ? -1 : 0;
}

/* The name to compare: the canonical one, or the text itself when it has none. */
static void identity_of(const char *file, char *out, size_t size)
{
    if (canonical_file(file, out, size)) snprintf(out, size, "%s", file);
}

/* --- recording --------------------------------------------------------- */

typedef struct {
    const char *to;
} CopyInto;

static int copy_into(const char *path, const char *name, void *context)
{
    const CopyInto *into = context;
    char target[1200];
    if (snprintf(target, sizeof(target), "%s/%s", into->to, name) >= (int)sizeof(target)) return 0;
    return copy_file(path, target);
}

int ReplayFile_Begin(const char *file, char *recording, size_t recording_size, char *state, size_t state_size,
                     char *why, size_t why_size)
{
    char work[1100], from[1200], to[1200];
    const char *name;
    int i;
    if (work_folder(file, work, sizeof(work)) ||
        snprintf(recording, recording_size, "%s/recording.txt", work) >= (int)recording_size ||
        snprintf(state, state_size, "%s/start.state", work) >= (int)state_size) {
        snprintf(why, why_size, "The replay's path is too long.");
        return -1;
    }
    /* The folder of a recording being packed is the pack's (ReplayFile_PackStart): a new
     * recording to the same file waits for it, and the player is told why. */
    if (ReplayFile_PackRunning(file)) {
        snprintf(why, why_size, "An earlier recording to this file is still being packed: try again in a moment.");
        return -1;
    }
    if (!access(work, F_OK)) Mods_ImportRemoveTree(work);
    snprintf(to, sizeof(to), "%s/progress", work);
    if (Paths_MakeDirs(to)) {
        snprintf(why, why_size, "Could not make %s: %s.", to, strerror(errno));
        return -1;
    }
    /* The player's progress as it is now: what the start state's game reads
     * of the saves when the replay plays. */
    for (i = 0; (name = Paths_ProgressName(i)) != NULL; i++) {
        snprintf(from, sizeof(from), "%s/%s", Paths_UserDir(), name);
        snprintf(to, sizeof(to), "%s/progress/%s", work, name);
        if (Mods_ImportIsLink(from)) continue;
        if (is_directory(from)) {
            CopyInto into = {to};
            if (Paths_MakeDirs(to) || each_file(from, copy_into, &into)) {
                snprintf(why, why_size, "Could not copy %s into the replay.", from);
                return -1;
            }
        } else if (is_file(from) && copy_file(from, to)) {
            snprintf(why, why_size, "Could not copy %s into the replay.", from);
            return -1;
        }
    }
    return 0;
}

/* JSON text: quotes, backslashes and control characters escaped. */
static void json_string(char **at, char *end, const char *text, size_t length)
{
    size_t i;
    if (*at < end) *(*at)++ = '"';
    for (i = 0; i < length && *at + 7 < end; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            *(*at)++ = '\\';
            *(*at)++ = (char)c;
        } else if (c < 0x20) {
            *at += snprintf(*at, (size_t)(end - *at), "\\u%04x", c);
        } else {
            *(*at)++ = (char)c;
        }
    }
    if (*at < end) *(*at)++ = '"';
}

static void json_raw(char **at, char *end, const char *text)
{
    size_t length = strlen(text);
    if ((size_t)(end - *at) > length) {
        memcpy(*at, text, length);
        *at += length;
    }
}

/* Space-separated words as a JSON list, or k=v words as an object. */
static void json_words(char **at, char *end, const char *words, int pairs)
{
    int first = 1;
    json_raw(at, end, pairs ? "{" : "[");
    while (*words) {
        size_t length = strcspn(words, " \t"), key;
        if (length && (!pairs || memchr(words, '=', length))) {
            if (!first) json_raw(at, end, ", ");
            first = 0;
            if (pairs) {
                key = (size_t)((const char *)memchr(words, '=', length) - words);
                json_string(at, end, words, key);
                json_raw(at, end, ": ");
                json_string(at, end, words + key + 1, length - key - 1);
            } else {
                json_string(at, end, words, length);
            }
        }
        words += length;
        while (*words == ' ' || *words == '\t') words++;
    }
    json_raw(at, end, pairs ? "}" : "]");
}

/* replay.json from the recording's own header lines (replay.py's format 1). */
static char *header_json(const char *recording, size_t *size, int *from_boot)
{
    static const char *const keys[] = {"build: ", "os: ", "settings: ", "mods: ", "clock: "};
    /* On the heap and not static: it runs on a pack's thread, and two packs may run at once. */
    char (*values)[4096] = calloc(5, sizeof(*values)), *line = malloc(8192);
    char build[64] = "", language[16] = "0";
    int boot = 0; /* "start boot": no start state in the file */
    char *json = malloc(65536), *at = json, *end;
    FILE *file = fopen(recording, "r");
    const char *found;
    size_t i;
    if (!json || !values || !line) {
        if (file) fclose(file);
        free(json);
        free(values);
        free(line);
        return NULL;
    }
    end = json + 65536 - 1;
    while (file && fgets(line, 8192, file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "build ", 6)) snprintf(build, sizeof(build), "%s", line + 6);
        if (!strcmp(line, "start boot")) boot = 1;
        if (strncmp(line, "F ", 2)) continue;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (!strncmp(line + 2, keys[i], strlen(keys[i]))) snprintf(values[i], sizeof(values[i]), "%s", line + 2 + strlen(keys[i]));
        }
    }
    if (file) fclose(file);
    if ((found = strstr(values[2], "language=")) && (found == values[2] || found[-1] == ' '))
        snprintf(language, sizeof(language), "%.*s", (int)strcspn(found + 9, " "), found + 9);
    json_raw(&at, end, "{\n \"format\": 1,\n \"kind\": \"recorded\",\n \"player\": true,\n \"header\": {\n  \"build\": ");
    json_string(&at, end, build, strlen(build));
    json_raw(&at, end, ",\n  \"clock\": ");
    json_string(&at, end, values[4][0] ? values[4] : "virtual", strlen(values[4][0] ? values[4] : "virtual"));
    json_raw(&at, end, ",\n  \"os\": ");
    json_string(&at, end, values[1], strlen(values[1]));
    json_raw(&at, end, ",\n  \"commit\": ");
    json_string(&at, end, values[0], strlen(values[0]));
    json_raw(&at, end, ",\n  \"settings\": ");
    json_words(&at, end, values[2], 1);
    json_raw(&at, end, ",\n  \"mods\": ");
    json_words(&at, end, strcmp(values[3], "none") ? values[3] : "", 0);
    json_raw(&at, end, ",\n  \"language\": ");
    json_string(&at, end, language, strlen(language));
    json_raw(&at, end, boot ? "\n },\n \"start\": {\"kind\": \"boot\"},\n"
                            : "\n },\n \"start\": {\"kind\": \"state\", \"file\": \"start.state\"},\n");
    json_raw(&at, end, " \"recording\": \"recording.txt\",\n \"progress\": \"progress\",\n \"states_every\": 0,\n");
    json_raw(&at, end, boot ? " \"notes\": \"Game > Record replay from boot\"\n}\n" : " \"notes\": \"Game > Record replay\"\n}\n");
    free(values);
    free(line);
    *at = '\0';
    *size = (size_t)(at - json);
    *from_boot = boot;
    return json;
}

typedef struct {
    ModsZipOut *zip;
    const char *prefix;
    char *why;
    size_t why_size;
} AddTo;

static int add_one(const char *path, const char *name, void *context)
{
    AddTo *add = context;
    char entry[400];
    if (snprintf(entry, sizeof(entry), "%s/%s", add->prefix, name) >= (int)sizeof(entry)) return 0;
    return Mods_ZipAddFile(add->zip, entry, path, add->why, add->why_size) ? 0 : -1;
}

/* The packing itself, on whichever thread: the zip is written beside the
 * file as "<file>.partial" and renamed over it when it is whole, so that the
 * file is never half a zip (a game that is killed leaves the .partial and the
 * folder). `recording_size` is the recording's length when it was stopped,
 * 0 when not known. */
static int pack_files(const char *file, unsigned long long recording_size, char *why, size_t why_size)
{
    char work[1100], path[1200], prefix[300], state[1200], partial[1200];
    const char *name;
    char *json;
    size_t json_size = 0;
    ModsZipOut *zip;
    struct stat info;
    int i, ok, boot = 0;
    if (work_folder(file, work, sizeof(work)) || snprintf(partial, sizeof(partial), "%s.partial", file) >= (int)sizeof(partial)) {
        snprintf(why, why_size, "The replay's path is too long.");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/recording.txt", work);
    /* header_json reads "start boot" from it: unread, it would say "from a
     * state" and the refusal below would blame the state, not the recording. */
    if (!is_file(path)) {
        snprintf(why, why_size, "The recording (recording.txt) is missing or cannot be read.");
        return -1;
    }
    /* What the game thread handed over is what is packed: a recording that is
     * not the length it had at the stop was touched by something else. */
    if (recording_size && (stat(path, &info) || (unsigned long long)info.st_size != recording_size)) {
        snprintf(why, why_size, "The recording changed after it was stopped.");
        return -1;
    }
    if (!(json = header_json(path, &json_size, &boot))) {
        snprintf(why, why_size, "Out of memory.");
        return -1;
    }
    /* What the recording says it starts from decides, not what the folder
     * holds: a recording from a state that lost its state is refused (the
     * folder is kept), not packed as a recording from boot. */
    snprintf(state, sizeof(state), "%s/start.state", work);
    if (!boot && !is_file(state)) {
        free(json);
        snprintf(why, why_size, "The recording starts from a state, and its start state is missing.");
        return -1;
    }
    if (!(zip = Mods_ZipCreate(partial, why, why_size))) {
        free(json);
        return -1;
    }
    ok = Mods_ZipAddData(zip, "replay.json", json, json_size, why, why_size) &&
         Mods_ZipAddFile(zip, "recording.txt", path, why, why_size);
    free(json);
    /* A recording from boot never made a state: the file holds none. */
    if (ok && !boot) ok = Mods_ZipAddFile(zip, "start.state", state, why, why_size);
    for (i = 0; ok && (name = Paths_ProgressName(i)) != NULL; i++) {
        snprintf(path, sizeof(path), "%s/progress/%s", work, name);
        snprintf(prefix, sizeof(prefix), "progress/%s", name);
        if (is_directory(path)) {
            AddTo add = {zip, prefix, why, why_size};
            ok = !each_file(path, add_one, &add);
        } else if (is_file(path)) {
            ok = Mods_ZipAddFile(zip, prefix, path, why, why_size);
        }
    }
    if (!ok) Mods_ZipAbandon(zip);
    if (!Mods_ZipFinish(zip, why, why_size)) return -1; /* it removes the .partial */
    if (rename(partial, file)) {
        snprintf(why, why_size, "Could not name the replay %s: %s.", file, strerror(errno));
        remove(partial);
        return -1;
    }
    Mods_ImportRemoveTree(work);
    return 0;
}

int ReplayFile_Finish(const char *file, char *why, size_t why_size)
{
    if (ReplayFile_PackRunning(file)) {
        snprintf(why, why_size, "It is being packed on its own thread.");
        return -1;
    }
    return pack_files(file, 0, why, why_size);
}

/* --- packing off the game's thread -------------------------------------
 *
 * A few slots, each a thread that runs pack_files for one recording. The game
 * thread owns a slot while it is FREE and after it is taken; the thread owns
 * it while it is RUNNING and publishes the end in `state` (atomic, the last
 * thing it writes, as import_job.c does). The thread touches only the files
 * of its recording: no guest memory, no clock, no notice (the recorder
 * tells the player, on the game's thread, when it takes the result). */

#define PACK_SLOTS REPLAY_PACK_SLOTS
enum { PACK_FREE, PACK_RUNNING, PACK_ENDED };

static struct PackJob {
    int state; /* atomic */
    pthread_t thread;
    int quiet, failed;
    unsigned tenths; /* how long it took, in tenths of a millisecond */
    unsigned long long recording_size;
    char file[1100];
    char canon[CANON_MAX]; /* identity_of(file): the name that decides which recording a pack is of */
    char why[400];
} jobs[PACK_SLOTS];
static void (*pack_hook)(const char *file);

static double milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

static void *pack_thread(void *argument)
{
    struct PackJob *job = argument;
    double began;
    if (pack_hook) pack_hook(job->file);
    began = milliseconds();
    job->failed = pack_files(job->file, job->recording_size, job->why, sizeof(job->why)) != 0;
    job->tenths = (unsigned)((milliseconds() - began) * 10.0);
    /* No fprintf from here, to stderr or anywhere. A thread that prints a number while the process is in
     * exit() hung it on the Windows C runtime (seen with "%.1f" here, and in a bare test program with
     * "%ld"; it did not when the main thread had printed numbers before, so it is a first use of
     * something, which snprintf and the file calls this thread makes do not trip). PackTake says how it
     * went, on the caller's thread. */
    __atomic_store_n(&job->state, PACK_ENDED, __ATOMIC_RELEASE);
    return NULL;
}

void ReplayFile_SetPackHook(void (*hook)(const char *file)) { pack_hook = hook; }

int ReplayFile_PackStart(const char *file, unsigned long long recording_size, int quiet, char *why, size_t why_size)
{
    struct PackJob *job = NULL;
    int i, running = 0, created;
    if (strlen(file) >= sizeof(job->file)) {
        snprintf(why, why_size, "The replay's path is too long.");
        return -1;
    }
    if (ReplayFile_PackRunning(file)) {
        snprintf(why, why_size, "It is already being packed.");
        return -1;
    }
    for (i = 0; i < PACK_SLOTS; i++) {
        if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) == PACK_FREE) {
            if (!job) job = &jobs[i];
        } else if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) == PACK_RUNNING) {
            running++;
        }
    }
    if (!job) {
        snprintf(why, why_size, "Too many replays are being packed at once.");
        return -1;
    }
    memset(job->why, 0, sizeof(job->why));
    snprintf(job->file, sizeof(job->file), "%s", file);
    identity_of(file, job->canon, sizeof(job->canon));
    job->recording_size = recording_size;
    job->quiet = quiet;
    job->failed = 0;
    __atomic_store_n(&job->state, PACK_RUNNING, __ATOMIC_RELEASE);
#ifndef _WIN32
    /* The interrupt clock's signal (MEMORIES_CLOCK=interrupt) belongs to the game's thread: blocked here,
     * the new thread starts with it blocked, and no tick can land on it before it could mask it. (On
     * Windows the clock interrupts the main thread only: compat/signal.h.) */
    sigset_t alarm, before;
    sigemptyset(&alarm);
    sigaddset(&alarm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &alarm, &before);
    created = pthread_create(&job->thread, NULL, pack_thread, job);
    pthread_sigmask(SIG_SETMASK, &before, NULL);
#else
    created = pthread_create(&job->thread, NULL, pack_thread, job);
#endif
    if (created) {
        __atomic_store_n(&job->state, PACK_FREE, __ATOMIC_RELEASE);
        snprintf(why, why_size, "The packing could not get a thread of its own.");
        return -1;
    }
    fprintf(stderr, "memories-pc: replay: packing %s on a thread of its own (%d other%s running)\n", file, running,
            running == 1 ? "" : "s");
    return 0;
}

int ReplayFile_PackTake(char *file, size_t file_size, int *quiet, char *why, size_t why_size)
{
    int i;
    for (i = 0; i < PACK_SLOTS; i++) {
        struct PackJob *job = &jobs[i];
        int failed;
        if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != PACK_ENDED) continue;
        pthread_join(job->thread, NULL);
        snprintf(file, file_size, "%s", job->file);
        snprintf(why, why_size, "%s", job->why);
        *quiet = job->quiet;
        failed = job->failed;
        fprintf(stderr, "memories-pc: replay: packed %s in %u.%u ms on a thread of its own%s%s\n", job->file,
                job->tenths / 10, job->tenths % 10, failed ? ": " : "", failed ? job->why : "");
        __atomic_store_n(&job->state, PACK_FREE, __ATOMIC_RELEASE);
        return failed ? -1 : 1;
    }
    return 0;
}

/* By identity (canonical_file), not by the text: "replays\A.yfmreplay" and "C:/u/replays/a.yfmreplay" are one
 * file, and its folder is the pack's whichever way a new recording names it. */
int ReplayFile_PackRunning(const char *file)
{
    char identity[CANON_MAX];
    int i, count = 0;
    if (file) identity_of(file, identity, sizeof(identity));
    for (i = 0; i < PACK_SLOTS; i++) {
        if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) == PACK_RUNNING && (!file || !strcmp(identity, jobs[i].canon)))
            count++;
    }
    return count;
}

int ReplayFile_PackWait(const char *file, unsigned timeout_ms)
{
    double deadline = milliseconds() + timeout_ms;
    while (ReplayFile_PackRunning(file)) {
        struct timespec nap = {0, 2000000L};
        if (milliseconds() >= deadline) return 0;
        nanosleep(&nap, NULL);
    }
    return 1;
}

void ReplayFile_Abandon(const char *file)
{
    char work[1100];
    if (ReplayFile_PackRunning(file)) return; /* its folder is the pack's */
    if (!work_folder(file, work, sizeof(work)) && !access(work, F_OK)) Mods_ImportRemoveTree(work);
}

/* --- what a game that did not finish left -------------------------------- */

/* The newest modification time of the files in `path` and below it (of `path` itself when it is a
 * file, or a folder with none), or LLONG_MAX when it must be left alone: a link anywhere (nothing
 * below follows one; Mods_ImportRemoveTree does, on Windows), something unreadable, or a tree deeper
 * than a replay's folders ever are. A folder's own time says only when a name came or went in it:
 * the files say when the recording or the zip was last written. */
static long long newest_time(const char *path, int depth)
{
    struct stat info;
    long long newest = -1;
    DIR *listing;
    struct dirent *entry;
    if (depth > 5 || Mods_ImportIsLink(path) || stat(path, &info)) return LLONG_MAX;
    if (!S_ISDIR(info.st_mode)) return (long long)info.st_mtime;
    if (!(listing = opendir(path))) return LLONG_MAX;
    while (newest != LLONG_MAX && (entry = readdir(listing))) {
        char inner[1500];
        long long other;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (snprintf(inner, sizeof(inner), "%s/%s", path, entry->d_name) >= (int)sizeof(inner)) {
            newest = LLONG_MAX;
            break;
        }
        other = newest_time(inner, depth + 1);
        if (other > newest) newest = other;
    }
    closedir(listing);
    return newest < 0 ? (long long)info.st_mtime : newest;
}

/* Whether `name` ends in `suffix` (lower case), in any case: ReplayFile_Is does the same. */
static int ends_with(const char *name, const char *suffix)
{
    size_t length = strlen(name), n = strlen(suffix), i;
    if (length < n) return 0;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)name[length - n + i]) != suffix[i]) return 0;
    }
    return 1;
}

int ReplayFile_RemoveLeftovers(const char *keep, unsigned min_age_seconds)
{
    static const char *const suffixes[2] = {".yfmreplay.partial", ".yfmreplay.parts"};
    char folder[1100], kept[2][CANON_MAX];
    DIR *listing;
    struct dirent *entry;
    long long now = (long long)time(NULL);
    int removed = 0, i;
    if (snprintf(folder, sizeof(folder), "%s/replays", Paths_UserDir()) >= (int)sizeof(folder) ||
        !(listing = opendir(folder)))
        return 0;
    for (i = 0; i < 2; i++) {
        char name[1200];
        kept[i][0] = '\0';
        /* "<keep>.partial" and "<keep>.parts": the suffixes without ".yfmreplay". */
        if (keep && snprintf(name, sizeof(name), "%s%s", keep, suffixes[i] + 10) < (int)sizeof(name))
            identity_of(name, kept[i], sizeof(kept[i]));
    }
    while ((entry = readdir(listing))) {
        char path[1300], identity[CANON_MAX], file[1300];
        long long newest;
        size_t length;
        int which = ends_with(entry->d_name, suffixes[0]) ? 0 : ends_with(entry->d_name, suffixes[1]) ? 1 : -1;
        if (which < 0 || snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= (int)sizeof(path)) continue;
        /* The recording this process is about to take over, and a pack of its own. */
        identity_of(path, identity, sizeof(identity));
        if (kept[which][0] && !strcmp(identity, kept[which])) continue;
        length = strlen(path) - strlen(suffixes[which] + 10);
        memcpy(file, path, length);
        file[length] = '\0';
        if (ReplayFile_PackRunning(file)) continue;
        /* A plain file for the .partial, a folder for the .parts: nothing else of that name is touched. */
        if (Mods_ImportIsLink(path) || (which == 0 ? !is_file(path) : !is_directory(path))) continue;
        newest = newest_time(path, 0);
        if (newest == LLONG_MAX || now - newest < (long long)min_age_seconds) continue;
        if (which == 0 ? remove(path) : Mods_ImportRemoveTree(path)) {
            fprintf(stderr, "memories-pc: replay: could not remove the leftover %s\n", path);
            continue;
        }
        fprintf(stderr, "memories-pc: replay: removed %s, left by a recording that was never packed (%lld s old)\n", path,
                now - newest);
        removed++;
    }
    closedir(listing);
    return removed;
}

/* --- playing ------------------------------------------------------------ */

int ReplayFile_Open(const char *file, char *recording, size_t recording_size, char *state, size_t state_size,
                    char *progress, size_t progress_size, char *why, size_t why_size)
{
    char folder[1100], target[1300];
    const char *name;
    ModsImport *import;
    int i, got;
    if (Paths_User(folder, sizeof(folder), "replays/.playing")) {
        snprintf(why, why_size, "The user folder's path is too long.");
        return -1;
    }
    if (!access(folder, F_OK)) Mods_ImportRemoveTree(folder);
    if (snprintf(progress, progress_size, "%s/progress", folder) >= (int)progress_size ||
        snprintf(recording, recording_size, "%s/recording.txt", folder) >= (int)recording_size ||
        snprintf(state, state_size, "%s/start.state", folder) >= (int)state_size || Paths_MakeDirs(progress)) {
        snprintf(why, why_size, "Could not make %s.", folder);
        return -1;
    }
    if (!(import = Mods_ImportOpen(file, why, why_size))) return -1;
    got = Mods_ImportExtract(import, "recording.txt", recording, RECORDING_MAX, why, why_size);
    if (got == 0) snprintf(why, why_size, "It is not a replay: it has no recording.txt.");
    if (got > 0 && (got = Mods_ImportExtract(import, "start.state", state, STATE_MAX, why, why_size)) == 0) {
        state[0] = '\0'; /* from boot */
        got = 1;
    }
    /* The progress: only the names the game keeps it under, one level down. */
    for (i = 0; got > 0 && (name = Mods_ImportFile(import, i)) != NULL; i++) {
        const char *rest = name + 9, *slash;
        if (strncmp(name, "progress/", 9) || !Paths_Contained(rest) || !Paths_IsProgress(rest)) continue;
        slash = strchr(rest, '/');
        if (slash && strchr(slash + 1, '/')) continue;
        if (snprintf(target, sizeof(target), "%s/%s", progress, rest) >= (int)sizeof(target)) continue;
        if (slash) {
            char parent[1300];
            snprintf(parent, sizeof(parent), "%s/%.*s", progress, (int)(slash - rest), rest);
            if (Paths_MakeDirs(parent)) continue;
        }
        if (Mods_ImportExtract(import, name, target, PROGRESS_MAX, why, why_size) < 0) got = -1;
    }
    Mods_ImportClose(import);
    return got > 0 ? 0 : -1;
}

int ReplayFile_Is(const char *path)
{
    size_t length = strlen(path);
    static const char suffix[] = ".yfmreplay";
    size_t i, n = sizeof(suffix) - 1;
    if (length < n) return 0;
    for (i = 0; i < n; i++) {
        char c = path[length - n + i];
        if ((c >= 'A' && c <= 'Z' ? c + 32 : c) != suffix[i]) return 0;
    }
    return 1;
}

int ReplayFile_NewName(char *path, size_t size)
{
    char relative[128], stamp[32];
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    int n;
    if (!local || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local)) snprintf(stamp, sizeof(stamp), "%ld", (long)now);
    for (n = 1; n < 100; n++) {
        if (n == 1) snprintf(relative, sizeof(relative), "replays/replay-%s.yfmreplay", stamp);
        else snprintf(relative, sizeof(relative), "replays/replay-%s-%d.yfmreplay", stamp, n);
        if (Paths_User(path, size, relative)) return -1;
        /* A recording being packed has no file yet, and its name is taken. */
        if (access(path, F_OK) && !ReplayFile_PackRunning(path)) return 0;
    }
    return -1;
}

/* The name a restart leaves for the next start, in replays/`marker`: its
 * first line is the time it was made (seconds since the epoch), the second
 * the file. */
static int set_marker(const char *marker, const char *file)
{
    char relative[64], path[1100];
    FILE *out;
    snprintf(relative, sizeof(relative), "replays/%s", marker);
    if (Paths_User(path, sizeof(path), relative) || !(out = fopen(path, "w"))) return -1;
    fprintf(out, "%lld\n%s\n", (long long)time(NULL), file);
    return fclose(out) ? -1 : 0;
}

/* A restart starts the game within seconds, a slow disk or an antivirus scan
 * within a minute or two. A marker older than this is the leftover of a
 * restart that never came (the monitor's start_game failed, or the new
 * process died before Recorder_Init): not an order any more. The clock may
 * also have been set back between the Set and the Take (a time sync, summer
 * time does not count: it is the epoch): a marker from the future by up to
 * MARKER_SKEW_SECONDS is still the order it was. */
#define MARKER_FRESH_SECONDS 120
#define MARKER_SKEW_SECONDS 10

/* Takes the marker: the file it names, only when the marker is an order, that
 * is fresh and removed. A stale one, one of the older format (no time line)
 * and one that cannot be removed (an antivirus holds it, a read-only folder:
 * it would be honoured at every start) are dropped, said on stderr, and never
 * honoured. 0 with `file`; REPLAY_MARKER_NONE (no marker); REPLAY_MARKER_DROPPED
 * (one was there and was not honoured: the caller tells the player). */
static int take_marker(const char *marker, char *file, size_t size)
{
    char path[1100], stamp[40];
    FILE *in;
    char *end;
    long long made, now;
    /* Asked at every start: no folder is made for it (Paths_User would). */
    if (snprintf(path, sizeof(path), "%s/replays/%s", Paths_UserDir(), marker) >= (int)sizeof(path) ||
        !(in = fopen(path, "r")))
        return REPLAY_MARKER_NONE;
    if (!fgets(stamp, (int)sizeof(stamp), in)) stamp[0] = '\0';
    if (!fgets(file, (int)size, in)) file[0] = '\0';
    fclose(in);
    file[strcspn(file, "\r\n")] = '\0';
    /* Taken once: a replay that crashes the game does not come back at every start. */
    if (remove(path)) {
        fprintf(stderr, "memories-pc: replay: %s cannot be removed, so it is not honoured\n", path);
        return REPLAY_MARKER_DROPPED;
    }
    made = strtoll(stamp, &end, 10);
    now = (long long)time(NULL);
    if (end == stamp || *end != '\n' || made - now > MARKER_SKEW_SECONDS || now - made > MARKER_FRESH_SECONDS) {
        fprintf(stderr, "memories-pc: replay: %s is a leftover (made %lld, now %lld), dropped\n", path,
                end == stamp ? 0LL : made, now);
        return REPLAY_MARKER_DROPPED;
    }
    return file[0] ? 0 : REPLAY_MARKER_DROPPED;
}

/* A request taken back: the restart it was for did not happen. */
static void drop_marker(const char *marker)
{
    char path[1100];
    if (snprintf(path, sizeof(path), "%s/replays/%s", Paths_UserDir(), marker) < (int)sizeof(path) && remove(path) &&
        access(path, F_OK) == 0)
        fprintf(stderr, "memories-pc: replay: %s cannot be removed\n", path);
}

int ReplayFile_SetNext(const char *file) { return set_marker("next.txt", file); }
int ReplayFile_TakeNext(char *file, size_t size) { return take_marker("next.txt", file, size); }
int ReplayFile_SetNextRecord(const char *file) { return set_marker("next-record.txt", file); }
int ReplayFile_TakeNextRecord(char *file, size_t size) { return take_marker("next-record.txt", file, size); }
void ReplayFile_DropNext(void) { drop_marker("next.txt"); }
void ReplayFile_DropNextRecord(void) { drop_marker("next-record.txt"); }
