#ifndef MEMORIES_PC_AUDIO_LATE_CALLBACK_H
#define MEMORIES_PC_AUDIO_LATE_CALLBACK_H
/* Whether a device callback ran late (sdl.c's feed(); the HUD's audio
 * "late callbacks"). SDL's device thread runs the stream's callback each time
 * the device wants a buffer, and the callback puts exactly what is asked, so
 * the stream is empty at every entry: an empty stream says nothing. A device
 * runs dry when its thread is late, so a gap of more than twice the time the
 * PREVIOUS callback's audio lasts since that callback is late: the gap is
 * what the previous request had to cover, whatever this one asks. Header
 * only, so CTest (tests/pc/audio_late_test.c) builds the same code. */
#include <stdint.h>

#define AUDIO_LATE_RATE 44100u
#define AUDIO_LATE_FRAME_BYTES 4u /* stereo int16_t */

/* `previous_ns` is 0 before the first callback (it is not late);
 * `previous_bytes` is the audio the previous callback put in the stream. Less
 * than one frame tells nothing (a period of 0 would make every gap late). */
static inline int AudioLate_Is(uint64_t previous_ns, uint64_t now_ns, int previous_bytes)
{
    uint64_t period_ns;
    if (!previous_ns || previous_bytes < (int)AUDIO_LATE_FRAME_BYTES) return 0;
    period_ns = (uint64_t)previous_bytes / AUDIO_LATE_FRAME_BYTES * 1000000000u / AUDIO_LATE_RATE;
    if (!period_ns) return 0;
    return now_ns - previous_ns > 2 * period_ns;
}

#endif
