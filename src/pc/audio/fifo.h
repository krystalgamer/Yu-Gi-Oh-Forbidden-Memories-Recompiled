#ifndef MEMORIES_PC_AUDIO_FIFO_H
#define MEMORIES_PC_AUDIO_FIFO_H
/* The sound between the game thread's mixer and an audio device, while a
 * replay records or plays (platform_common.c, "The stepped mixer to a
 * device"): one producer, the game thread, which mixes the SPU as the virtual
 * clock steps; one consumer, the device's thread, which takes what it asks
 * for when the device asks for it.
 *
 * The producer never waits and never learns anything back: AudioFifo_Push
 * returns nothing, and frames that find the ring full are dropped. So the
 * game mixes the same frames, and its sound driver reads the same voice
 * levels, whenever and however much the device drains; only what is heard
 * depends on it. The consumer fills an underrun with silence, waits for
 * AUDIO_FIFO_PRIME frames before it plays again (so a paused or slow game
 * gives a clean gap, not a crackle of single blocks), and skips the oldest
 * frames past AUDIO_FIFO_MOST (a game at Turbo makes more than real time
 * plays, and the latency would grow without end).
 *
 * Header only, so the game's backends and CTest (tests/pc/audio_fifo_test.c)
 * build the same code. Stereo frames of two int16_t. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AUDIO_FIFO_FRAMES 16384u /* 372 ms; a power of two */
#define AUDIO_FIFO_PRIME 2048u   /* 46 ms queued before sound starts again */
#define AUDIO_FIFO_MOST 6144u    /* 139 ms queued at most: the rest is skipped */

typedef struct {
    int16_t frames[AUDIO_FIFO_FRAMES * 2];
    uint32_t head, tail;     /* written (producer) and read (consumer) counts, mod 2^32 */
    int primed;              /* consumer only */
    uint32_t dropped;        /* producer only: frames that found the ring full */
    uint32_t underruns;      /* consumer only: calls that ran dry */
    uint32_t skipped;        /* consumer only: frames skipped to bound the latency */
} AudioFifo;

static inline void AudioFifo_Reset(AudioFifo *fifo)
{
    memset(fifo, 0, sizeof(*fifo));
}

/* Producer: the frames just mixed. */
static inline void AudioFifo_Push(AudioFifo *fifo, const int16_t *frames, size_t count)
{
    uint32_t head = fifo->head, tail = __atomic_load_n(&fifo->tail, __ATOMIC_ACQUIRE);
    uint32_t room = AUDIO_FIFO_FRAMES - (head - tail);
    size_t i;
    if (count > room) {
        fifo->dropped += (uint32_t)(count - room);
        count = room;
    }
    for (i = 0; i < count; i++) {
        uint32_t at = (head + (uint32_t)i) & (AUDIO_FIFO_FRAMES - 1u);
        fifo->frames[at * 2] = frames[i * 2];
        fifo->frames[at * 2 + 1] = frames[i * 2 + 1];
    }
    __atomic_store_n(&fifo->head, head + (uint32_t)count, __ATOMIC_RELEASE);
}

/* Consumer: `count` frames into `out`, silence where there are none. */
static inline void AudioFifo_Pop(AudioFifo *fifo, int16_t *out, size_t count)
{
    uint32_t head = __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE), tail = fifo->tail;
    uint32_t queued = head - tail;
    size_t i, take;
    if (queued > AUDIO_FIFO_MOST) {
        uint32_t skip = queued - AUDIO_FIFO_PRIME;
        fifo->skipped += skip;
        tail += skip;
        queued -= skip;
    }
    if (!fifo->primed && queued >= AUDIO_FIFO_PRIME) fifo->primed = 1;
    take = fifo->primed ? (queued < count ? queued : count) : 0;
    for (i = 0; i < take; i++) {
        uint32_t at = (tail + (uint32_t)i) & (AUDIO_FIFO_FRAMES - 1u);
        out[i * 2] = fifo->frames[at * 2];
        out[i * 2 + 1] = fifo->frames[at * 2 + 1];
    }
    if (take < count) {
        memset(out + take * 2, 0, (count - take) * 2 * sizeof(int16_t));
        if (fifo->primed) fifo->underruns++;
        fifo->primed = 0;
    }
    __atomic_store_n(&fifo->tail, tail + (uint32_t)take, __ATOMIC_RELEASE);
}
#endif
