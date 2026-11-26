/* The stepped mixer's FIFO to an audio device (src/pc/audio/fifo.h), with
 * the software SPU (src/pc/audio/spu.c) as the game thread drives it while a
 * replay records or plays (platform_common.c, mix_owed_frames).
 *
 *  1. The game's side does not depend on the device. A seeded schedule keys
 *     voices and changes their envelopes and pitches between steps; each
 *     1 ms step mixes the 44.1 frames it owes and pushes them, as the
 *     virtual clock's steps do. What the game's sound driver would read
 *     after each step (every voice's envelope and key status) and every frame
 *     mixed are hashed. The same run is made with no consumer, with a
 *     consumer thread taking 256 frames every 5.8 ms of real time (SDL's
 *     callback), and with one taking random amounts as fast as it can: the
 *     three digests must be the same.
 *  2. The consumer's side: frames come out in the order they went in, with
 *     gaps only where the ring was full (dropped at the push) or the latency
 *     was cut (skipped at the pop), and silence only on an underrun.
 *  3. Stopping and starting again (platform_common.c, stepped_audio): once
 *     AudioFifoGate_Switch(off) has returned, no consumer is inside the FIFO
 *     any more, so the reset the next start makes cannot meet a pop. A
 *     consumer thread calls the gate as a device does, and the game side
 *     switches on, fills the ring and switches off, over and over; after
 *     each off the ring must stand still, and a reset leaves it empty.
 *  4. The fades at an underrun's edges (AudioFifo_Fade), through the gate as
 *     a device calls it, in pops shorter than a fade: the first frames after
 *     a silence fade in over AUDIO_FIFO_FADE frames; where the queue runs dry
 *     (a pop that empties it exactly, then pops that find nothing) the last
 *     frame fades out over as many, then silence; a queue that plays again in
 *     the middle of a fade-out fades in from where it stood.
 */
#include "pc/audio/fifo.h"
#include "pc/audio/replace.h"
#include "pc/audio/spu.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- what spu.c links to ----------------------------------------------------- */

int Log_Wanted(LogChannel channel)
{
    (void)channel;
    return 0;
}
void Log_Printf(LogChannel channel, const char *format, ...)
{
    (void)channel;
    (void)format;
}
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    (void)state, (void)tag, (void)fields, (void)count;
    return 0;
}
int AudioReplace_MixBegin(void) { return 0; }
void AudioReplace_MixFrame(int out[6]) { memset(out, 0, 6 * sizeof(*out)); }
void AudioReplace_MixEnd(void) {}

/* --- the game's side ---------------------------------------------------------- */

static uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size--) hash = (hash ^ *p++) * 0x100000001b3ull;
    return hash;
}

static uint32_t seed;
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static AudioFifo fifo;
static volatile int game_done;
enum { NONE, STEADY, GREEDY };

static void nap_us(long us)
{
    struct timespec nap = {0, us * 1000};
    nanosleep(&nap, NULL);
}

static void *consumer(void *how)
{
    static int16_t out[1024 * 2];
    uint32_t local = 99;
    while (!__atomic_load_n(&game_done, __ATOMIC_ACQUIRE)) {
        if ((intptr_t)how == STEADY) {
            AudioFifo_Pop(&fifo, out, 256);
            nap_us(5805);
        } else {
            local = local * 1103515245u + 12345u;
            AudioFifo_Pop(&fifo, out, 1 + (local >> 16) % 1024);
        }
    }
    return NULL;
}

/* 24 short samples at 0x1000 + 0x400 * v, odd ones looping. */
static void make_samples(void)
{
    unsigned v, b, i;
    for (v = 0; v < 24; v++) {
        uint8_t *at = Spu_Ram + 0x1000 + v * 0x400;
        for (b = 0; b < 64; b++) {
            uint8_t *block = at + b * 16;
            block[0] = (uint8_t)((rnd() % 5) << 4 | (rnd() % 16));
            block[1] = b == 0 ? 4 : b == 63 ? (v & 1 ? 3 : 1) : 0;
            for (i = 2; i < 16; i++) block[i] = (uint8_t)rnd();
        }
    }
}

/* A second of game time a run: steps of 1 ms, each mixing what it owes
 * (441 ten-thousandths of a frame a microsecond, as mix_owe counts). */
static uint64_t run(int how, unsigned seconds)
{
    static int16_t buffer[256 * 2];
    uint64_t hash = 0xcbf29ce484222325ull;
    uint32_t fraction = 0;
    unsigned step, v;
    pthread_t thread;
    seed = 4242;
    Spu_Reset();
    make_samples();
    AudioFifo_Reset(&fifo);
    game_done = 0;
    if (how != NONE && pthread_create(&thread, NULL, consumer, (void *)(intptr_t)how)) {
        fprintf(stderr, "audio_fifo_test: no thread\n");
        exit(1);
    }
    for (step = 0; step < seconds * 1000; step++) {
        uint32_t total = fraction + 441u * 1000u, owed = total / 10000u;
        fraction = total % 10000u;
        if (rnd() % 8 == 0) {
            unsigned voice = rnd() % 24;
            Spu_SetStart(voice, 0x1000 + (rnd() % 24) * 0x400);
            Spu_SetPitch(voice, (uint16_t)(0x800 + rnd() % 0x1000));
            Spu_SetVolume(voice, 0x2000, 0x2000);
            Spu_SetAdsr(voice, (uint16_t)rnd(), (uint16_t)rnd());
            Spu_KeyOn(1u << voice);
        }
        if (rnd() % 16 == 0) Spu_KeyOff(1u << (rnd() % 24));
        while (owed) {
            size_t count = owed < 256 ? owed : 256;
            Spu_Mix(buffer, count);
            AudioFifo_Push(&fifo, buffer, count);
            hash = fnv(hash, buffer, count * 2 * sizeof(int16_t));
            owed -= (uint32_t)count;
        }
        /* What the sound driver reads at the step's tick. */
        for (v = 0; v < 24; v++) {
            int16_t envelope = Spu_Envelope(v);
            int status = Spu_KeyStatus(v);
            hash = fnv(hash, &envelope, sizeof(envelope));
            hash = fnv(hash, &status, sizeof(status));
        }
        /* The game's own pace against the device's: a burst, then a breath. */
        if (how == STEADY && step % 17 == 16) nap_us(2000);
    }
    __atomic_store_n(&game_done, 1, __ATOMIC_RELEASE);
    if (how != NONE) pthread_join(thread, NULL);
    return hash;
}

/* --- the consumer's side ------------------------------------------------------ */

static volatile int producing;
static void *producer(void *unused)
{
    static int16_t frames[300 * 2];
    uint32_t next = 1, local = 7;
    unsigned i, round;
    (void)unused;
    for (round = 0; round < 20000; round++) {
        size_t count;
        local = local * 1103515245u + 12345u;
        count = 1 + (local >> 16) % 300;
        /* Frame n as two words, never both zero (silence). */
        for (i = 0; i < count; i++, next++) {
            frames[i * 2] = (int16_t)(next & 0x7fff);
            frames[i * 2 + 1] = (int16_t)(0x4000 | (next >> 15 & 0x3fff));
        }
        AudioFifo_Push(&fifo, frames, count);
        if (round % 64 == 0) nap_us(1000);
    }
    __atomic_store_n(&producing, 0, __ATOMIC_RELEASE);
    return NULL;
}

static int order_check(void)
{
    static int16_t out[512 * 2];
    pthread_t thread;
    uint32_t last = 0;
    int failed = 0;
    unsigned long long frames = 0, silent = 0;
    unsigned after = 0;
    AudioFifo_Reset(&fifo);
    producing = 1;
    if (pthread_create(&thread, NULL, producer, NULL)) return 1;
    /* Once the producer is done, a few more pops: what is left under
     * AUDIO_FIFO_PRIME after an underrun stays queued, as a device's would. */
    while (__atomic_load_n(&producing, __ATOMIC_ACQUIRE) || (fifo.head != fifo.tail && after++ < 64)) {
        unsigned i;
        AudioFifo_Pop(&fifo, out, 512);
        for (i = 0; i < 512; i++, frames++) {
            uint16_t left = (uint16_t)out[i * 2], right = (uint16_t)out[i * 2 + 1];
            uint32_t n;
            if (!left && !right) {
                silent++;
                continue;
            }
            if ((right & 0xc000) != 0x4000 || left > 0x7fff) {
                fprintf(stderr, "audio_fifo_test: frame %llu is %04x/%04x, not one pushed\n", frames, left, right);
                return 1;
            }
            /* Forward only: the next frame, or later ones past those
             * dropped at the push or skipped at the pop. */
            n = left | (uint32_t)(right & 0x3fff) << 15;
            if (n <= last) failed = 1;
            last = n;
        }
        nap_us(500);
    }
    pthread_join(thread, NULL);
    if (failed) fprintf(stderr, "audio_fifo_test: a frame came out of order\n");
    if (!silent && !fifo.underruns) {
        fprintf(stderr, "audio_fifo_test: the consumer never ran dry, so its silence was not tried\n");
        failed = 1;
    }
    printf("audio_fifo_test: %llu frames out, %llu silent; %u dropped, %u skipped, %u underruns\n", frames, silent,
           fifo.dropped, fifo.skipped, fifo.underruns);
    return failed;
}

static AudioFifoGate gate;
static volatile int churning;
static unsigned own_mixes;

static void own_mix(int16_t *out, size_t count)
{
    memset(out, 0, count * 2 * sizeof(int16_t));
    own_mixes++;
}

static void *device(void *unused)
{
    static int16_t out[2048 * 2];
    (void)unused;
    while (__atomic_load_n(&churning, __ATOMIC_ACQUIRE)) AudioFifoGate_Call(&gate, &fifo, out, 2048, own_mix);
    return NULL;
}

static int churn_check(void)
{
    static int16_t frames[1024 * 2];
    pthread_t thread;
    unsigned round, bad = 0;
    memset(frames, 1, sizeof(frames));
    churning = 1;
    if (pthread_create(&thread, NULL, device, NULL)) return 1;
    for (round = 0; round < 3000; round++) {
        uint32_t head, tail, underruns, skipped;
        unsigned i;
        /* The start: reset (the device is on its own mixer), then on. */
        AudioFifo_Reset(&fifo);
        AudioFifoGate_Switch(&gate, 1);
        /* The game's frames, enough to be primed and taken, and more than
         * MOST, so a pop is long (it skips, then copies) when the stop comes. */
        for (i = 0; i < 10; i++) AudioFifo_Push(&fifo, frames, 1024);
        if (round % 3) nap_us(rnd() % 200);
        /* The stop. */
        AudioFifoGate_Switch(&gate, 0);
        head = fifo.head;
        tail = fifo.tail;
        underruns = AudioFifo_Underruns(&fifo);
        skipped = AudioFifo_Skipped(&fifo);
        nap_us(100);
        if (head != fifo.head || tail != fifo.tail || underruns != AudioFifo_Underruns(&fifo) ||
            skipped != AudioFifo_Skipped(&fifo)) {
            bad++;
        }
    }
    __atomic_store_n(&churning, 0, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
    AudioFifo_Reset(&fifo);
    if (!own_mixes) {
        fprintf(stderr, "audio_fifo_test: the device never ran its own mixer, so the stops were not tried\n");
        return 1;
    }
    if (fifo.head || fifo.tail || fifo.primed || AudioFifo_Underruns(&fifo) || AudioFifo_Skipped(&fifo)) {
        fprintf(stderr, "audio_fifo_test: a reset left the ring not empty\n");
        return 1;
    }
    if (bad) fprintf(stderr, "audio_fifo_test: a consumer was still in the FIFO after %u stops\n", bad);
    else printf("audio_fifo_test: 3000 stops and starts, the ring still after each stop\n");
    return bad != 0;
}

/* --- the fades at an underrun's edges --------------------------------------------- */

static int fade_failed;

static void fade_expect(const int16_t *out, unsigned at, int left, int right, const char *what)
{
    if (out[at * 2] != left || out[at * 2 + 1] != right) {
        fprintf(stderr, "audio_fifo_test: %s: frame %u is %d/%d, not %d/%d\n", what, at, out[at * 2],
                out[at * 2 + 1], left, right);
        fade_failed = 1;
    }
}

/* `count` frames through the gate, as a device asks, in pops of `chunk`. */
static void fade_pop(int16_t *out, size_t count, size_t chunk)
{
    size_t done;
    for (done = 0; done < count; done += chunk)
        AudioFifoGate_Call(&gate, &fifo, out + done * 2, count - done < chunk ? count - done : chunk, own_mix);
}

static int fade_check(void)
{
    static int16_t frames[AUDIO_FIFO_PRIME * 2], out[4096 * 2];
    const int left = 12000, right = -9000, fade = (int)AUDIO_FIFO_FADE;
    unsigned i, mixes = own_mixes;
    for (i = 0; i < AUDIO_FIFO_PRIME; i++) {
        frames[i * 2] = (int16_t)left;
        frames[i * 2 + 1] = (int16_t)right;
    }
    /* The last frame differs, so the fade out is seen to start from it. */
    frames[(AUDIO_FIFO_PRIME - 1) * 2] = 6400;
    frames[(AUDIO_FIFO_PRIME - 1) * 2 + 1] = -3200;
    AudioFifo_Reset(&fifo);
    AudioFifoGate_Switch(&gate, 1);
    /* From silence: a fade in, in pops of 10. */
    AudioFifo_Push(&fifo, frames, AUDIO_FIFO_PRIME);
    fade_pop(out, 100, 10);
    for (i = 0; i < 100; i++) {
        int level = i < AUDIO_FIFO_FADE ? (int)i + 1 : fade;
        fade_expect(out, i, left * level / fade, right * level / fade, "fade in");
    }
    /* The rest of the queue exactly, then pops of 7 that find nothing: the
     * last frame fades out, then silence. */
    fade_pop(out, AUDIO_FIFO_PRIME - 100, 256);
    fade_expect(out, AUDIO_FIFO_PRIME - 101, 6400, -3200, "the last frame");
    fade_pop(out, 200, 7);
    for (i = 0; i < 200; i++) {
        int level = i < AUDIO_FIFO_FADE ? fade - 1 - (int)i : 0;
        fade_expect(out, i, 6400 * level / fade, -3200 * level / fade, "fade out");
    }
    if (AudioFifo_Underruns(&fifo) != 1) {
        fprintf(stderr, "audio_fifo_test: fade: %u underruns, not 1\n", AudioFifo_Underruns(&fifo));
        fade_failed = 1;
    }
    /* Dry again with 10 frames left, then 20 frames of fade out... */
    AudioFifo_Push(&fifo, frames, AUDIO_FIFO_PRIME);
    fade_pop(out, AUDIO_FIFO_PRIME - 10, 256);
    fade_pop(out, 30, 5);
    for (i = 0; i < 30; i++) {
        int level = i < 10 ? fade : fade - (int)(i - 9);
        int l = i < 9 ? left : 6400, r = i < 9 ? right : -3200;
        fade_expect(out, i, l * level / fade, r * level / fade, "fade out cut short");
    }
    /* ...and the queue plays again: it fades in from 44/64, in pops of 3. */
    AudioFifo_Push(&fifo, frames, AUDIO_FIFO_PRIME);
    fade_pop(out, 40, 3);
    for (i = 0; i < 40; i++) {
        int level = fade - 20 + (int)i + 1;
        if (level > fade) level = fade;
        fade_expect(out, i, left * level / fade, right * level / fade, "fade in from a fade out");
    }
    AudioFifoGate_Switch(&gate, 0);
    AudioFifo_Reset(&fifo);
    if (own_mixes != mixes) {
        fprintf(stderr, "audio_fifo_test: fade: the gate ran its own mixer\n");
        fade_failed = 1;
    }
    if (!fade_failed) {
        printf("audio_fifo_test: fades in and out over %u frames, across pops, and from a fade cut short\n",
               AUDIO_FIFO_FADE);
    }
    return fade_failed;
}

int main(void)
{
    uint64_t alone, steady, greedy;
    setvbuf(stdout, NULL, _IONBF, 0);
    alone = run(NONE, 20);
    steady = run(STEADY, 20);
    greedy = run(GREEDY, 20);
    printf("audio_fifo_test: game side %016llx alone, %016llx with a steady device, %016llx with a greedy one\n",
           (unsigned long long)alone, (unsigned long long)steady, (unsigned long long)greedy);
    if (alone != steady || alone != greedy) {
        fprintf(stderr, "audio_fifo_test: the game's side depends on how the device drains\n");
        return 1;
    }
    if (order_check()) return 1;
    if (churn_check()) return 1;
    return fade_check();
}
