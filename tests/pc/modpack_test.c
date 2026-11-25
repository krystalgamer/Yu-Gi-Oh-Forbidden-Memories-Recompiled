/* Modpacks (src/pc/mods/mods.c, notes/mods-window.md "Modpacks") across
 * launches. Mods_Load scans once a process, so each launch is a process of
 * its own: run without arguments, this makes the user directory and runs
 * itself once per step ("<step> <root>"), in order, on that one directory.
 *
 *   pack-first   the pack's first launch: its settings applied, the player's
 *                own saved aside first, all its mods on (Hard too, which the
 *                player's own settings have off); then the player changes an
 *                option and turns Extra off
 *   pack-again   its second launch: the session goes on, the changes kept
 *                (Extra stays off, Hard on)
 *   pack-third   its third launch: still whole (its .verified, .settings-applied
 *                and a desktop.ini do not count), Activate's check too
 *   unlisted     a file its list does not name: not read, at the start and in
 *                Activate's check, the normal mods instead
 *   linked       a link (a junction on Windows) in it to an empty folder
 *                outside: counted as a file its list does not name, never
 *                entered; not read
 *   hand         a folder made by hand (no modpack.json): read, unchecked
 *   nofiles      a modpack.json without "files": not read
 *   normal       no modpack: the player's own settings back, none of the pack's
 *   pack-return  the pack again: its first launch again
 *   fresh        (another user directory) no modpacks/ there: none is made
 *   locked-first a locked pack: its mods on; the player turns one off
 *   locked-again its mods on again, its values again; the player imports
 *                Late (its manifest says on), which comes in off
 *   save-fails   Pack, with settings that cannot be saved: putting the
 *                player's own back fails, and the file they are in is kept
 *   normal-late  no modpack: as normal, and Late is still off
 */
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#endif

static void use_root(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/prog", root);
    assert(!setenv("MEMORIES_PROGRAM_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/user", root);
    assert(!setenv("MEMORIES_USER_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/user/settings.txt", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    unsetenv("MEMORIES_MODS_DIR");
    unsetenv("MEMORIES_MODS_FOLDER");
    unsetenv("MEMORIES_MODS");
    unsetenv("MEMORIES_MOD_HARD");
    unsetenv("MEMORIES_MOD_EXTRA");
}

static int exists(const char *relative)
{
    char path[1024];
    struct stat info;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return !stat(path, &info);
}

static void launch(void)
{
    Settings_Load();
    Mods_Load();
}

static int named(const char *key) { return Settings_GetNamed(key, -1000); }

/* A mod.json in `place`, and its SHA-256 for modpack.json's "files". */
static void manifest(const char *place, const char *text, char hex[65])
{
    char file[1024], path[1300];
    snprintf(file, sizeof(file), "%s/mod.json", place);
    write_text(file, text);
    snprintf(path, sizeof(path), "%s/%s", root, file);
    assert(Mods_Sha256File(path, hex));
}

#define HARD "{\"id\":\"hard\",\"name\":\"Hard\",\"settings\":[{\"key\":\"level\",\"type\":\"int\",\"default\":1,\"max\":10}]}"
#define EXTRA "{\"id\":\"extra\",\"name\":\"Extra\"}"

static void setup(void)
{
    char hard[65], extra[65], json[2048];
    make_dir("prog");
    make_dir("prog/mods");
    make_dir("user");
    make_dir("user/mods");
    make_dir("user/mods/hard");
    write_text("user/mods/hard/mod.json", HARD);
    /* The player's own: Hard off, level 3, 7th in the load order. */
    write_text("user/settings.txt", "mod.hard=0\nmod.hard.level=3\nmod.hard.order=7\n");
    make_dir("user/modpacks");
    make_dir("user/modpacks/Pack");
    make_dir("user/modpacks/Pack/mod-001");
    make_dir("user/modpacks/Pack/mod-002");
    manifest("user/modpacks/Pack/mod-001", HARD, hard);
    manifest("user/modpacks/Pack/mod-002", EXTRA, extra);
    snprintf(json, sizeof(json),
             "{\"name\":\"Pack\",\"locked\":false,\"mods\":["
             "{\"id\":\"hard\",\"folder\":\"mod-001\",\"order\":-2,\"settings\":{\"level\":9}},"
             "{\"id\":\"extra\",\"folder\":\"mod-002\",\"order\":4,\"settings\":{}}],"
             "\"files\":{\"mod-001/mod.json\":\"%s\",\"mod-002/mod.json\":\"%s\"}}",
             hard, extra);
    write_text("user/modpacks/Pack/modpack.json", json);
    make_dir("user/modpacks/Locked");
    make_dir("user/modpacks/Locked/mod-001");
    manifest("user/modpacks/Locked/mod-001", EXTRA, extra);
    snprintf(json, sizeof(json),
             "{\"name\":\"Locked\",\"locked\":true,\"mods\":[{\"id\":\"extra\",\"folder\":\"mod-001\",\"order\":1,"
             "\"settings\":{}}],\"files\":{\"mod-001/mod.json\":\"%s\"}}",
             extra);
    write_text("user/modpacks/Locked/modpack.json", json);
    /* What Explorer leaves in a folder is not a file of the pack's. */
    write_text("user/modpacks/Pack/mod-002/desktop.ini", "[.ShellClassInfo]\n");
    make_dir("user/modpacks/Hand");
    make_dir("user/modpacks/Hand/extra");
    write_text("user/modpacks/Hand/extra/mod.json", EXTRA);
    make_dir("user/modpacks/Nofiles");
    make_dir("user/modpacks/Nofiles/extra");
    write_text("user/modpacks/Nofiles/extra/mod.json", EXTRA);
    write_text("user/modpacks/Nofiles/modpack.json", "{\"name\":\"Nofiles\",\"mods\":[]}");
}

static void choose(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/user/modpacks/selected.txt", root);
    if (name) write_text("user/modpacks/selected.txt", name);
    else assert(!remove(path));
}

/* --- the launches ----------------------------------------------------- */

static void pack_first(void)
{
    launch();
    assert(!strcmp(Mods_ModpackLaunched(), "Pack") && !*Mods_ModpackProblem());
    assert(Mods_Count() == 2 && Mods_Enabled(find("hard")) && Mods_Enabled(find("extra")));
    /* The pack's values, and the player's own saved before they were written. */
    assert(named("mod.hard.level") == 9 && named("mod.hard.order") == -2 && named("mod.extra.order") == 4);
    assert(exists("user/modpacks/.normal-settings") && exists("user/modpacks/Pack/.settings-applied"));
    /* Kept without a save of the player's: the next launch reads them. */
    Settings_Load();
    assert(named("mod.hard.level") == 9 && named("mod.extra.order") == 4);
    /* The player's changes in the pack's session (the Mods window's Apply). */
    Settings_SetNamed("mod.hard.level", 5);
    Settings_SetNamed("mod.extra", 0);
    assert(Settings_Save());
}

static void pack_again(void)
{
    launch();
    assert(!strcmp(Mods_ModpackLaunched(), "Pack") && !*Mods_ModpackProblem());
    assert(named("mod.hard.level") == 5 && named("mod.hard.order") == -2);
    assert(exists("user/modpacks/.normal-settings"));
    /* Turned off, it stays off; Hard, on since the first launch, stays on. */
    assert(!Mods_Enabled(find("extra")) && Mods_Enabled(find("hard")));
}

/* `late`: Late was imported during the pack's session before this launch. */
static void normal_launch(int late)
{
    launch();
    assert(!*Mods_ModpackLaunched());
    assert(Mods_Count() == 1 + late && !Mods_Enabled(find("hard")));
    /* Its manifest says on, but it came in off and nobody turned it on. */
    if (late) assert(find("late") >= 0 && !Mods_Enabled(find("late")));
    /* The player's own values, none of the pack's, and saved so. Extra,
     * turned off in a pack's session, stays off: an off the player's own
     * settings lack is kept, the rest of what the session added goes. */
    for (int pass = 0; pass < 2; pass++) {
        assert(named("mod.hard") == 0 && named("mod.hard.level") == 3 && named("mod.hard.order") == 7);
        assert(named("mod.extra") == 0 && named("mod.extra.order") == -1000);
        if (late) assert(named("mod.late") == 0);
        Settings_Load();
    }
    assert(!exists("user/modpacks/.normal-settings") && !exists("user/modpacks/Pack/.settings-applied"));
}
static void normal(void) { normal_launch(0); }

static void pack_return(void)
{
    launch();
    assert(!strcmp(Mods_ModpackLaunched(), "Pack"));
    /* Its first launch again: the pack's values, not the last session's. */
    assert(named("mod.hard.level") == 9 && named("mod.hard.order") == -2);
    assert(Mods_Enabled(find("hard")) && Mods_Enabled(find("extra")));
}

static void pack_third(void)
{
    char why[300] = "";
    launch();
    assert(!strcmp(Mods_ModpackLaunched(), "Pack") && !*Mods_ModpackProblem());
    assert(exists("user/modpacks/Pack/.verified") && exists("user/modpacks/Pack/.settings-applied"));
    assert(Mods_ModpackPrepare("Pack", NULL, NULL, NULL, why, sizeof(why)) == 1);
}

static void unlisted(void)
{
    char why[300] = "";
    launch();
    assert(!*Mods_ModpackLaunched() && strstr(Mods_ModpackProblem(), "not in its list (mod-001/evil.txt)"));
    assert(Mods_Count() == 1 && find("hard") >= 0 && find("extra") < 0);
    assert(!Mods_ModpackPrepare("Pack", NULL, NULL, NULL, why, sizeof(why)));
    assert(strstr(why, "1 file(s) in it are not in its list (mod-001/evil.txt)"));
}

/* A link in the pack (a junction on Windows) to an empty folder outside it:
 * entered, it would hold nothing unlisted; it is counted itself instead. */
static void linked(void)
{
    char why[300] = "";
    launch();
    assert(!*Mods_ModpackLaunched() && strstr(Mods_ModpackProblem(), "not in its list (mod-001/link)"));
    assert(!Mods_ModpackPrepare("Pack", NULL, NULL, NULL, why, sizeof(why)));
    assert(strstr(why, "1 file(s) in it are not in its list (mod-001/link)"));
}

/* `link` made to point at the folder `target` (both under the root): 1 when
 * it could be. */
static int make_link(const char *link, const char *target)
{
    char from[1024], to[1024];
    snprintf(from, sizeof(from), "%s/%s", root, link);
    snprintf(to, sizeof(to), "%s/%s", root, target);
#ifdef _WIN32
    {
        wchar_t line[5000], *a = Memories_Utf8ToWide(from), *b = Memories_Utf8ToWide(to);
        int made;
        for (wchar_t *at = a; at && *at; at++) if (*at == L'/') *at = L'\\';
        for (wchar_t *at = b; at && *at; at++) if (*at == L'/') *at = L'\\';
        _snwprintf(line, sizeof(line) / sizeof(line[0]), L"mklink /J \"%ls\" \"%ls\" >nul", a, b);
        line[sizeof(line) / sizeof(line[0]) - 1] = 0;
        made = a && b && !_wsystem(line);
        free(a);
        free(b);
        return made;
    }
#else
    return !symlink(to, from);
#endif
}

static void hand(void)
{
    launch();
    assert(!strcmp(Mods_ModpackLaunched(), "Hand") && !*Mods_ModpackProblem());
    assert(Mods_Count() == 1 && Mods_Enabled(find("extra")));
}

static void nofiles(void)
{
    launch();
    assert(!*Mods_ModpackLaunched() && strstr(Mods_ModpackProblem(), "no list of files"));
}

/* A user directory that never had a modpack: a launch reads that there is
 * none and leaves the directory as it was (no modpacks/ made). */
static void fresh(void)
{
    char name[64];
    launch();
    assert(!*Mods_ModpackLaunched() && !*Mods_ModpackProblem() && !Mods_ModpackSelected(name, sizeof(name)));
    assert(Mods_Count() == 0 && !exists("user/modpacks"));
}

static void locked_first(void)
{
    launch();
    assert(!strcmp(Mods_ModpackLaunched(), "Locked") && Mods_ModpackLocked());
    assert(Mods_Count() == 1 && Mods_Enabled(find("extra")) && named("mod.extra.order") == 1);
    Settings_SetNamed("mod.extra", 0);
    Settings_SetNamed("mod.extra.order", 3);
    assert(Settings_Save());
}

static void locked_again(void)
{
    char path[1024];
    int later;
    launch();
    assert(Mods_ModpackLocked() && Mods_Enabled(find("extra")) && named("mod.extra.order") == 1);
    /* Import mod... during the session: a mod whose manifest says on comes
     * in off (Mods_Discover writes mod.late=0), into the normal mods folder. */
    make_dir("user/mods/late");
    write_text("user/mods/late/mod.json", "{\"id\":\"late\",\"name\":\"Late\",\"enabled\":true}");
    snprintf(path, sizeof(path), "%s/user/mods/late", root);
    assert(Mods_Discover(path, &later) >= 0 && named("mod.late") == 0);
    assert(Settings_Save());
}

/* Pack, after Locked's session, with a settings file that cannot be saved
 * (its path is a folder): putting the player's own back fails, so the file
 * they are in must stay as it is, Locked's, and not be replaced by one
 * holding what is in the settings now. */
static void save_fails(void)
{
    char path[1024], line[256];
    FILE *file;
    snprintf(path, sizeof(path), "%s/user/settings-folder", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    launch();
    snprintf(path, sizeof(path), "%s/user/modpacks/.normal-settings", root);
    assert((file = fopen(path, "rb")));
    assert(fgets(line, sizeof(line), file) && !strncmp(line, "modpack=Locked", 14));
    fclose(file);
}

static void normal_late(void) { normal_launch(1); }

static int run_step(const char *self, const char *step)
{
#ifdef _WIN32
    char quoted_self[1100], quoted_root[SCRATCH_MAX + 4];
    const char *args[4];
    snprintf(quoted_self, sizeof(quoted_self), "\"%s\"", self);
    snprintf(quoted_root, sizeof(quoted_root), "\"%s\"", root);
    args[0] = quoted_self;
    args[1] = step;
    args[2] = quoted_root;
    args[3] = NULL;
    fflush(NULL);
    return (int)_spawnv(_P_WAIT, self, args);
#else
    int status;
    pid_t child;
    fflush(NULL);
    child = fork();
    assert(child >= 0);
    if (!child) {
        execl(self, self, step, root, (char *)NULL);
        _exit(127);
    }
    assert(waitpid(child, &status, 0) == child);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static void step(const char *self, const char *name)
{
    int result = run_step(self, name);
    if (result) fprintf(stderr, "modpack_test: step %s failed (%d)\n", name, result);
    assert(result == 0);
}

int main(int argc, char **argv)
{
    if (argc > 2) {   /* one launch */
        snprintf(root, sizeof(root), "%s", argv[2]);
        use_root();
        if (!strcmp(argv[1], "pack-first")) pack_first();
        else if (!strcmp(argv[1], "pack-again")) pack_again();
        else if (!strcmp(argv[1], "normal")) normal();
        else if (!strcmp(argv[1], "pack-return")) pack_return();
        else if (!strcmp(argv[1], "pack-third")) pack_third();
        else if (!strcmp(argv[1], "unlisted")) unlisted();
        else if (!strcmp(argv[1], "linked")) linked();
        else if (!strcmp(argv[1], "hand")) hand();
        else if (!strcmp(argv[1], "nofiles")) nofiles();
        else if (!strcmp(argv[1], "fresh")) fresh();
        else if (!strcmp(argv[1], "locked-first")) locked_first();
        else if (!strcmp(argv[1], "locked-again")) locked_again();
        else if (!strcmp(argv[1], "normal-late")) normal_late();
        else if (!strcmp(argv[1], "save-fails")) save_fails();
        else {
            fprintf(stderr, "modpack_test: unknown step \"%s\"\n", argv[1]);
            return 2;
        }
        return 0;
    }
    assert(scratch_dir(root, sizeof(root), "memories-modpack"));
#ifdef _WIN32
    for (char *c = root; *c; c++)
        if (*c == '\\') *c = '/';
#endif
    setup();
    {   /* another user directory, without modpacks/ */
        char saved[SCRATCH_MAX];
        make_dir("fresh");
        make_dir("fresh/prog");
        make_dir("fresh/prog/mods");
        make_dir("fresh/user");
        make_dir("fresh/user/mods");
        memcpy(saved, root, sizeof(saved));
        snprintf(root, sizeof(root), "%s/fresh", saved);
        step(argv[0], "fresh");
        memcpy(root, saved, sizeof(saved));
    }
    choose("Pack");
    step(argv[0], "pack-first");
    step(argv[0], "pack-again");
    step(argv[0], "pack-third");
    write_text("user/modpacks/Pack/mod-001/evil.txt", "added after the export");
    step(argv[0], "unlisted");
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/user/modpacks/Pack/mod-001/evil.txt", root);
        assert(!remove(path));
    }
    make_dir("outside");
    assert(make_link("user/modpacks/Pack/mod-001/link", "outside"));
    step(argv[0], "linked");
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/user/modpacks/Pack/mod-001/link", root);
#ifdef _WIN32
        assert(!rmdir(path)); /* the junction alone, not what it points at */
#else
        assert(!unlink(path));
#endif
    }
    choose("Hand");
    step(argv[0], "hand");
    choose("Nofiles");
    step(argv[0], "nofiles");
    choose(NULL);
    step(argv[0], "normal");
    choose("Pack");
    step(argv[0], "pack-return");
    choose("Locked");
    step(argv[0], "locked-first");
    step(argv[0], "locked-again");
    make_dir("user/settings-folder");
    choose("Pack");
    step(argv[0], "save-fails");
    choose(NULL);
    step(argv[0], "normal-late");
    puts("modpack: ok");
    return 0;
}
