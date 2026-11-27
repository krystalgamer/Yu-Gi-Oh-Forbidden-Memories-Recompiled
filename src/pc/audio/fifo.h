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
 * The virtual clock's pacer keeps the fill near AUDIO_FIFO_TARGET at 100 %
 * (AudioFifo_PacePeriod, platform_common.c's pace()): the device's clock,
 * not the host's, then sets the game's pace, so the latency neither creeps
 * up after a stall nor drifts with a device whose clock runs a little fast
 * or slow.
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
#define AUDIO_FIFO_TARGET 3072u  /* 70 ms: the fill the pacer keeps (AudioFifo_PacePeriod) */
#define AUDIO_FIFO_BAND 512u     /* 12 ms either side of it where the pacer leaves the period alone */

typedef struct {
    int16_t frames[AUDIO_FIFO_FRAMES * 2];
    uint32_t head, tail;     /* written (producer) and read (consumer) counts, mod 2^32 */
    int primed;              /* consumer only */
    int16_t last[2];         /* consumer only: the last frame taken from the queue, unfaded */
    uint32_t level;          /* consumer only: the fade's gain, 0 to AUDIO_FIFO_FADE */
    int16_t heard[2];        /* consumer only: the last frame AudioFifo_Fade gave the device */
    int16_t join[2];         /* consumer only: the frame a skip's crossfade starts from */
    uint32_t join_left;      /* consumer only: frames of that crossfade still to do */
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

/* Frames queued, for either side (the producer's pacer, the HUD). */
static inline uint32_t AudioFifo_Queued(const AudioFifo *fifo)
{
    uint32_t tail = __atomic_load_n(&fifo->tail, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE) - tail;
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

/* Consumer: the first frames of `out` crossfaded from the frame `from` over
 * the `*left` frames still owed (AUDIO_FIFO_FADE at the start), so a sound
 * that jumps (the device's own mixer to the FIFO's and back, or the FIFO
 * across a skip) is heard to move from one to the other, not to cut. */
static inline void AudioFifo_Join(const int16_t from[2], uint32_t *left, int16_t *out, size_t count)
{
    size_t i;
    for (i = 0; i < count && *left; i++, (*left)--) {
        int32_t k = (int32_t)(AUDIO_FIFO_FADE - *left + 1u), rest = (int32_t)AUDIO_FIFO_FADE - k;
        out[i * 2] = (int16_t)((from[0] * rest + out[i * 2] * k) / (int32_t)AUDIO_FIFO_FADE);
        out[i * 2 + 1] = (int16_t)((from[1] * rest + out[i * 2 + 1] * k) / (int32_t)AUDIO_FIFO_FADE);
    }
}

/* Consumer: `count` frames into `out`, silence where there are none. Returns
 * how many came from the queue (the rest is the silence). A pop that skips
 * (queued past AUDIO_FIFO_MOST) leaves the crossfade from what the device
 * last heard to the frames after the skip for AudioFifo_Fade. */
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
        /* After a silence (level 0) the fade in is the join; heard may be
         * digital zero at full level, which a skip to loud frames still cuts. */
        fifo->join[0] = fifo->heard[0];
        fifo->join[1] = fifo->heard[1];
        fifo->join_left = fifo->level ? AUDIO_FIFO_FADE : 0;
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
    /* A skip's jump, crossfaded; frames that did not come leave none to do. */
    if (take) AudioFifo_Join(fifo->join, &fifo->join_left, out, take);
    else fifo->join_left = 0;
    if (count) {
        fifo->heard[0] = out[(count - 1) * 2];
        fifo->heard[1] = out[(count - 1) * 2 + 1];
    }
}

/* The pacer's rule, for a game at 100 % whose
 * FIFO a device drains: the real time to the VBlank after this one, from
 * `period` us at the game speed and `queued` frames, and whether a VBlank
 * already late may still come at once to catch up (*catch_up). Above the
 * band the period grows by 1/128, below it shrinks by as much, so the game
 * makes a little less or more sound than the device plays until the fill is
 * back near AUDIO_FIFO_TARGET; above the target a late VBlank does not
 * catch up, since what a stall left behind is already queued. Past
 * AUDIO_FIFO_MOST no device is draining (its pop would have skipped): the
 * plain grid, as without the FIFO. It only changes when the host sleeps. */
static inline uint64_t AudioFifo_PacePeriod(uint32_t queued, uint64_t period, int *catch_up)
{
    *catch_up = queued <= AUDIO_FIFO_TARGET || queued > AUDIO_FIFO_MOST;
    if (queued > AUDIO_FIFO_MOST) return period;
    if (queued > AUDIO_FIFO_TARGET + AUDIO_FIFO_BAND) return period + period / 128u;
    if (queued + AUDIO_FIFO_BAND < AUDIO_FIFO_TARGET) return period - period / 128u;
    return period;
}

/* The whole decision of platform_common.c's pace(), for the game and for
 * tests/pc/audio_fifo_test.c alike, so the two cannot drift apart: the grid
 * `*next` (the real time of the coming VBlank) at `now`, a VBlank `period`
 * us long. A stall (more than 100 ms late) starts the grid again at `now`;
 * with `by_fifo` the period is AudioFifo_PacePeriod's for the `queued`
 * frames, and a VBlank already late, with the sound queued, does not come
 * back to back. Returns the real time to wait until before the VBlank, and
 * moves `*next` on by the period. Reads and writes no FIFO: when the host
 * sleeps, never what the game sees. */
static inline uint64_t AudioFifo_PaceGrid(uint64_t *next, uint64_t now, uint64_t period, int by_fifo, uint32_t queued)
{
    uint64_t wake;
    if (!*next || now > *next + 100000u) *next = now;
    if (by_fifo) {
        int catch_up;
        period = AudioFifo_PacePeriod(queued, period, &catch_up);
        if (!catch_up && *next < now) *next = now;
    }
    wake = *next;
    *next += period;
    return wake;
}

/* The device thread's switch between this FIFO and its own mixer. A thread
 * calls AudioFifoGate_Call each time its device asks for sound; the game
 * thread flips the gate with AudioFifoGate_Switch, which returns only when
 * no call that began before it is still running: after a switch off, the
 * FIFO is nobody's and may be reset; after a switch on, the thread's own
 * mixer is not running any more and the game thread may take it over. A
 * call that begins after the flip sees it (sequentially consistent). Every
 * switch also counts: two flips between one call and the next (a recording
 * stopped and another started at once) leave `on` as it was, but the FIFO was
 * reset in between, and the call sees from the count that it was handed over.
 *
 * The device hears the hand-over as a crossfade over AUDIO_FIFO_FADE frames,
 * from the last frame it was given by one side to the first ones of the
 * other: the FIFO's silence before its first frames fades the device mixer's
 * last frame out, and the device mixer, back, fades in from the FIFO's last.
 * The fields below are the calling thread's alone, apart from `on` and
 * `in_call`, so the gate's ordering is the one above. */
typedef struct {
    uint32_t state;     /* atomic: bit 0, calls play the FIFO; the rest counts the switches, so that
                         * one word says both and a call cannot take the flip from one and the count
                         * from another */
    int in_call;        /* atomic: a thread is inside AudioFifoGate_Call */
    int fifo_played;    /* calling thread only: the last call played the FIFO */
    uint32_t seen;      /* calling thread only: the switch count of the last call */
    int16_t heard[2];   /* calling thread only: the last frame given to the device */
    int16_t from[2];    /* calling thread only: the frame the hand-over crossfade starts from */
    uint32_t join_left; /* calling thread only: frames of that crossfade still to do */
} AudioFifoGate;

static inline void AudioFifoGate_Call(AudioFifoGate *gate, AudioFifo *fifo, int16_t *out, size_t count,
                                      void (*own_mix)(int16_t *, size_t))
{
    uint32_t state, switches;
    int fifo_on;
    __atomic_store_n(&gate->in_call, 1, __ATOMIC_SEQ_CST);
    state = __atomic_load_n(&gate->state, __ATOMIC_SEQ_CST);
    fifo_on = (int)(state & 1u);
    switches = state >> 1;
    if (fifo_on) AudioFifo_Fade(fifo, out, AudioFifo_Pop(fifo, out, count), count);
    else own_mix(out, count);
    /* A flip since the last call, or a switch that flipped back (the FIFO
     * was reset between), starts the crossfade from the last frame this
     * thread gave (from silence there is nothing to join). It touches the
     * gate's own fields only, and the call is still inside the gate. */
    if (fifo_on != gate->fifo_played || switches != gate->seen) {
        gate->fifo_played = fifo_on;
        gate->seen = switches;
        gate->from[0] = gate->heard[0];
        gate->from[1] = gate->heard[1];
        /* Only a hand-over to the FIFO from silence needs none (its own fade
         * in is that); one to the device's mixer is always faded in. */
        gate->join_left = (!fifo_on || gate->from[0] || gate->from[1]) ? AUDIO_FIFO_FADE : 0;
    }
    AudioFifo_Join(gate->from, &gate->join_left, out, count);
    if (count) {
        gate->heard[0] = out[(count - 1) * 2];
        gate->heard[1] = out[(count - 1) * 2 + 1];
    }
    __atomic_store_n(&gate->in_call, 0, __ATOMIC_SEQ_CST);
}

static inline void AudioFifoGate_Switch(AudioFifoGate *gate, int on)
{
    struct timespec nap = {0, 200000};
    uint32_t old = __atomic_load_n(&gate->state, __ATOMIC_SEQ_CST);
    /* The count up by one and the flip, in one store. */
    while (!__atomic_compare_exchange_n(&gate->state, &old, (((old >> 1) + 1u) << 1) | (on != 0), 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    }
    while (__atomic_load_n(&gate->in_call, __ATOMIC_SEQ_CST)) nanosleep(&nap, NULL);
}
#endif
