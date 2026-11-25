/* src/pc/cards/battle_models.c: what a bigger mod model's polygons carry once
 * its slot's packets are pointed at the slot's texture bank (stamp, reached
 * through textures, its only caller). The bank bits go into the second
 * corner's page word; the third corner's spare half, which both renderers read
 * as a fade on a banked polygon (soft_gpu.h), must come out 0 whatever the
 * model's record left there (its pad, or a Gouraud quad's first normal).
 *
 * The file is included whole, to reach its static functions; what it calls
 * outside is stubbed below. Nothing but stamp's packet walk and textures'
 * choice of bank is run: not the model check, the sort or the mods. A packet
 * run is addressed by a 32-bit guest address, so it is built for 32-bit
 * targets only (CMakeLists.txt). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc/cards/battle_models.c"

static int failures;
#define EXPECT(condition)                                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            fprintf(stderr, "battle_models_stamp_test:%d: %s\n", __LINE__, #condition); \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* The calls battle_models.c makes outside itself. */
ModelSlot D_800F2C40[MODEL_SLOT_COUNT];
s32 D_800FE240;
u8 D_8009AFA2;
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
/* Memories_GpuCommandWords is the real one (src/pc/render/packets.c); the
 * guest-memory calls its packet collector makes are not reached. */
uint8_t *Memories_ModPacketBase;
uint32_t Memories_ModPacketPhysical, Memories_ModPacketSize;
uint32_t Memories_ReadLE32(const uint8_t *bytes) { (void)bytes; return 0; }
void *Memories_Resolve(MemoriesMemory *memory, uint32_t address, size_t length, size_t alignment)
{
    (void)memory; (void)address; (void)length; (void)alignment;
    return NULL;
}
static uint16_t vram[SOFT_GPU_WIDTH * 512], banks[2][SOFT_GPU_WIDTH * 512];
static unsigned char page_data[1];
static int windows[2];
unsigned char *Mods_ModelArena(int arena, unsigned *size) { (void)arena; if (size) *size = 0; return NULL; }
int Mods_ModelPending(int slot) { (void)slot; return 0; }
const unsigned char *Mods_ModelPages(int slot, int *count) { (void)slot; *count = 0; return page_data; }
int *Mods_ModelWindows(unsigned *size) { *size = sizeof(windows); return windows; }
int *Mods_ModelPendings(unsigned *size) { *size = 0; return NULL; }
const uint16_t *SoftGpu_VramRaw(void) { return vram; }
void SoftGpu_Settle(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
uint16_t *SoftGpu_Bank(int bank) { return bank == 17 || bank == 18 ? banks[bank - 17] : NULL; }
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    (void)state; (void)tag; (void)fields; (void)count;
    return 0;
}
int Memories_StateLoading(const MemoriesState *state) { (void)state; return 0; }
void func_8004CB0C(s32 slot, u8 *hmd, s32 size, s32 flag) { (void)slot; (void)hmd; (void)size; (void)flag; }

/* A packet run as the game's sort leaves it: each primitive a tag word (its
 * length in words in the top byte) and its GPU words. */
#define JUNK 0xffff1234u   /* the third corner's word: the half that must go, then the texels */
#define PAGE 0x0034u       /* the second corner's page word's own bits, in its upper half */
static u32 run[160];

/* Writes one primitive at `at`; returns its end. `second` and `third` get
 * where the page word (second corner) and the third corner's word are, for a
 * textured one: those hold their own values, the rest a marker per word. */
static unsigned primitive(unsigned at, unsigned op, unsigned *second, unsigned *third)
{
    unsigned length = (unsigned)Memories_GpuCommandWords((u32)op << 24), i, gouraud = (op & 0x10) ? 1 : 0;
    run[at] = length << 24;
    for (i = 1; i <= length; i++) run[at + i] = 0x11110000u + i;   /* a marker per word */
    run[at + 1] = (u32)op << 24;
    if (!(op & 4)) {
        *second = *third = 0;
        return at + 1 + length;
    }
    *second = at + 1 + 4 + gouraud;
    *third = at + 1 + 6 + 2 * gouraud;
    run[*second] = (u32)PAGE << 16 | 0x00ab;
    run[*third] = JUNK;
    return at + 1 + length;
}

static void stamped(int slot)
{
    static const unsigned ops[] = {0x24, 0x2c, 0x34, 0x3c, 0x26, 0x2e, 0x36, 0x3e};   /* flat, Gouraud; tri, quad; semi too */
    unsigned second[8], third[8], at = 0, i, bank = (unsigned)TEXTURE_BANK(slot), ignored;
    u32 copy[160];
    memset(run, 0, sizeof(run));
    for (i = 0; i < 8; i++) at = primitive(at, ops[i], &second[i], &third[i]);
    /* What is not a textured polygon is left as it is: an untextured one
     * (0x20, 0x38: the words the others' halves are in) and a sprite. */
    at = primitive(at, 0x20, &ignored, &ignored);
    at = primitive(at, 0x38, &ignored, &ignored);
    run[at] = 3u << 24;
    run[at + 1] = 0x64u << 24;
    run[at + 2] = JUNK;
    run[at + 3] = JUNK;
    at += 4;
    memcpy(copy, run, sizeof(copy));
    placed[slot] = -2;
    windows[slot] = 0;
    textures(slot, (s32)(uintptr_t)run, (s32)(uintptr_t)(run + at));
    for (i = 0; i < 8; i++) {
        EXPECT((run[third[i]] >> 16) == 0);                                       /* no fade */
        EXPECT((run[third[i]] & 0xffffu) == (JUNK & 0xffffu));                    /* its texels stay */
        EXPECT(run[second[i]] >> 16 == (PAGE | SOFT_GPU_BANK_TPAGE(bank)));       /* the bank, its page kept */
        EXPECT((run[second[i]] & 0xffffu) == 0xabu);                              /* its palette word's half stays */
        EXPECT(SoftGpu_FadeOf(run[third[i]] >> 16) == 0);
    }
    for (i = 0; i < at; i++) {   /* nothing else changed, the tags included */
        unsigned changed = 0, j;
        for (j = 0; j < 8; j++) changed |= i == second[j] || i == third[j];
        if (!changed) EXPECT(run[i] == copy[i]);
    }
}

int main(void)
{
    stamped(0);   /* bank 17 */
    stamped(1);   /* bank 18 */
    if (failures) return 1;
    puts("battle_models_stamp_test: ok");
    return 0;
}
