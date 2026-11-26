/* A player's replay as one file (replay_file.h). */
#include "pc/compat/fs.h"
#include "replay_file.h"
#include "pc/mods/import.h"
#include "pc/platform/paths.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
static char *header_json(const char *recording, size_t *size)
{
    static const char *const keys[] = {"build: ", "os: ", "settings: ", "mods: ", "clock: "};
    static char values[5][4096], line[8192]; /* not on the game's stack, which a VSync's caller may be on */
    char build[64] = "", language[16] = "0";
    char *json = malloc(65536), *at = json, *end;
    FILE *file = fopen(recording, "r");
    const char *found;
    size_t i;
    if (!json) {
        if (file) fclose(file);
        return NULL;
    }
    end = json + 65536 - 1;
    memset(values, 0, sizeof(values));
    while (file && fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "build ", 6)) snprintf(build, sizeof(build), "%s", line + 6);
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
    json_raw(&at, end, "\n },\n \"start\": {\"kind\": \"state\", \"file\": \"start.state\"},\n"
                       " \"recording\": \"recording.txt\",\n \"progress\": \"progress\",\n \"states_every\": 0,\n"
                       " \"notes\": \"Game > Record replay\"\n}\n");
    *at = '\0';
    *size = (size_t)(at - json);
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

int ReplayFile_Finish(const char *file, char *why, size_t why_size)
{
    char work[1100], path[1200], prefix[300];
    const char *name;
    char *json;
    size_t json_size = 0;
    ModsZipOut *zip;
    int i, ok;
    if (work_folder(file, work, sizeof(work))) {
        snprintf(why, why_size, "The replay's path is too long.");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/recording.txt", work);
    if (!(json = header_json(path, &json_size))) {
        snprintf(why, why_size, "Out of memory.");
        return -1;
    }
    if (!(zip = Mods_ZipCreate(file, why, why_size))) {
        free(json);
        return -1;
    }
    ok = Mods_ZipAddData(zip, "replay.json", json, json_size, why, why_size) &&
         Mods_ZipAddFile(zip, "recording.txt", path, why, why_size);
    free(json);
    snprintf(path, sizeof(path), "%s/start.state", work);
    if (ok) ok = Mods_ZipAddFile(zip, "start.state", path, why, why_size);
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
    if (!Mods_ZipFinish(zip, why, why_size)) return -1;
    Mods_ImportRemoveTree(work);
    return 0;
}

void ReplayFile_Abandon(const char *file)
{
    char work[1100];
    if (!work_folder(file, work, sizeof(work)) && !access(work, F_OK)) Mods_ImportRemoveTree(work);
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
        if (access(path, F_OK)) return 0;
    }
    return -1;
}

int ReplayFile_SetNext(const char *file)
{
    char path[1100];
    FILE *out;
    if (Paths_User(path, sizeof(path), "replays/next.txt") || !(out = fopen(path, "w"))) return -1;
    fprintf(out, "%s\n", file);
    return fclose(out) ? -1 : 0;
}

int ReplayFile_TakeNext(char *file, size_t size)
{
    char path[1100];
    FILE *in;
    /* Asked at every start: no folder is made for it (Paths_User would). */
    if (snprintf(path, sizeof(path), "%s/replays/next.txt", Paths_UserDir()) >= (int)sizeof(path) ||
        !(in = fopen(path, "r")))
        return -1;
    if (!fgets(file, (int)size, in)) file[0] = '\0';
    fclose(in);
    remove(path); /* taken once: a replay that crashes the game does not come back at every start */
    file[strcspn(file, "\r\n")] = '\0';
    return file[0] ? 0 : -1;
}
