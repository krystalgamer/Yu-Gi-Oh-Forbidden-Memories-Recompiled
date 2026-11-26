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
 * plays, and the latency would grow without end). Where it runs dry it fades
 * the last frame out, and where it plays again it fades in, each over
 * AUDIO_FIFO_FADE frames (AudioFifo_Fade): no hard cut to zero and back.
 *
 * Header only, so the game's backends and CTest (tests/pc/audio_fifo_test.c)
 * build the same code. Stereo frames of two int16_t. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define AUDIO_FIFO_FRAMES 16384u /* 372 ms; a power of two */
#define AUDIO_FIFO_PRIME 2048u   /* 46 ms queued before sound starts again */
#define AUDIO_FIFO_MOST 6144u    /* 139 ms queued at most: the rest is skipped */
#define AUDIO_FIFO_FADE 64u      /* 1.5 ms of fade out where the queue runs dry, and in where it plays again */

typedef struct {
    int16_t frames[AUDIO_FIFO_FRAMES * 2];
    uint32_t head, tail;     /* written (producer) and read (consumer) counts, mod 2^32 */
    int primed;              /* consumer only */
    int16_t last[2];         /* consumer only: the last frame taken from the queue, unfaded */
    uint32_t level;          /* consumer only: the fade's gain, 0 to AUDIO_FIFO_FADE */
    uint32_t dropped;        /* producer only: frames that found the ring full */
    uint32_t underruns;      /* written by the consumer only; read through AudioFifo_Underruns */
    uint32_t skipped;        /* written by the consumer only; read through AudioFifo_Skipped */
} AudioFifo;

/* Only while no consumer is inside AudioFifo_Pop (AudioFifoGate_Switch waits
 * for that). */
static inline void AudioFifo_Reset(AudioFifo *fifo)
{
    memset(fifo, 0, sizeof(*fifo));
}

/* The consumer's counts, for another thread to read. */
static inline uint32_t AudioFifo_Underruns(const AudioFifo *fifo)
{
    return __atomic_load_n(&fifo->underruns, __ATOMIC_RELAXED);
}

static inline uint32_t AudioFifo_Skipped(const AudioFifo *fifo)
{
    return __atomic_load_n(&fifo->skipped, __ATOMIC_RELAXED);
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

/* Consumer: `count` frames into `out`, silence where there are none. Returns
 * how many came from the queue (the rest is the silence). */
static inline size_t AudioFifo_Pop(AudioFifo *fifo, int16_t *out, size_t count)
{
    uint32_t head = __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE), tail = fifo->tail;
    uint32_t queued = head - tail;
    size_t i, take;
    if (queued > AUDIO_FIFO_MOST) {
        uint32_t skip = queued - AUDIO_FIFO_PRIME;
        __atomic_store_n(&fifo->skipped, fifo->skipped + skip, __ATOMIC_RELAXED);
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
        if (fifo->primed) __atomic_store_n(&fifo->underruns, fifo->underruns + 1, __ATOMIC_RELAXED);
        fifo->primed = 0;
    }
    __atomic_store_n(&fifo->tail, tail + (uint32_t)take, __ATOMIC_RELEASE);
    return take;
}

/* Consumer, after AudioFifo_Pop gave `take` of `count` frames: the edges of
 * an underrun faded, so the sound never jumps to zero or back. The silence
 * after the last frame taken becomes that frame fading out over
 * AUDIO_FIFO_FADE frames, and the first frames after a silence fade in over
 * as many; a fade carries on into the next call, and one cut short turns
 * round where it stands. What is heard only: the game never sees it. */
static inline void AudioFifo_Fade(AudioFifo *fifo, int16_t *out, size_t take, size_t count)
{
    uint32_t level = fifo->level;
    size_t i;
    if (take) {
        fifo->last[0] = out[(take - 1) * 2];
        fifo->last[1] = out[(take - 1) * 2 + 1];
    }
    for (i = 0; i < take && level < AUDIO_FIFO_FADE; i++) {
        level++;
        out[i * 2] = (int16_t)(out[i * 2] * (int32_t)level / (int32_t)AUDIO_FIFO_FADE);
        out[i * 2 + 1] = (int16_t)(out[i * 2 + 1] * (int32_t)level / (int32_t)AUDIO_FIFO_FADE);
    }
    for (i = take; i < count && level; i++) {
        level--;
        out[i * 2] = (int16_t)(fifo->last[0] * (int32_t)level / (int32_t)AUDIO_FIFO_FADE);
        out[i * 2 + 1] = (int16_t)(fifo->last[1] * (int32_t)level / (int32_t)AUDIO_FIFO_FADE);
    }
    fifo->level = level;
}

/* The device thread's switch between this FIFO and its own mixer. A thread
 * calls AudioFifoGate_Call each time its device asks for sound; the game
 * thread flips the gate with AudioFifoGate_Switch, which returns only when
 * no call that began before it is still running: after a switch off, the
 * FIFO is nobody's and may be reset; after a switch on, the thread's own
 * mixer is not running any more and the game thread may take it over. A
 * call that begins after the flip sees it (sequentially consistent). */
typedef struct {
    int on;      /* atomic: calls play the FIFO */
    int in_call; /* atomic: a thread is inside AudioFifoGate_Call */
} AudioFifoGate;

static inline void AudioFifoGate_Call(AudioFifoGate *gate, AudioFifo *fifo, int16_t *out, size_t count,
                                      void (*own_mix)(int16_t *, size_t))
{
    __atomic_store_n(&gate->in_call, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&gate->on, __ATOMIC_SEQ_CST)) AudioFifo_Fade(fifo, out, AudioFifo_Pop(fifo, out, count), count);
    else own_mix(out, count);
    __atomic_store_n(&gate->in_call, 0, __ATOMIC_SEQ_CST);
}

static inline void AudioFifoGate_Switch(AudioFifoGate *gate, int on)
{
    struct timespec nap = {0, 200000};
    __atomic_store_n(&gate->on, on != 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&gate->in_call, __ATOMIC_SEQ_CST)) nanosleep(&nap, NULL);
}
#endif
