/* AudioLate_Is (src/pc/audio/late_callback.h), on made-up timestamps: a gap
 * is late when it is over twice the time the previous callback's audio
 * lasts, whatever the stream held or the current request asks. What the
 * test does not run is SDL's device thread: sdl.c's feed() calls the rule
 * with the clock and the bytes it put, and nothing here checks that. */
#include <stdio.h>
#include "pc/audio/late_callback.h"

static int failed;

static void check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "audio_late_test: %s: %d, want %d\n", what, got, want);
        failed = 1;
    }
}

int main(void)
{
    const int request = 256 * 4;                  /* 256 frames: 5.805 ms */
    const uint64_t period = 256ull * 1000000000u / AUDIO_LATE_RATE;
    uint64_t t = 1000000000u, previous = 0;
    unsigned late = 0;
    int i;

    check("the first callback", AudioLate_Is(0, t, request), 0);
    check("a zero request", AudioLate_Is(t, t + 10 * period, 0), 0);
    check("on time", AudioLate_Is(t, t + period, request), 0);
    check("a little jitter", AudioLate_Is(t, t + period + period / 2, request), 0);
    check("exactly twice", AudioLate_Is(t, t + 2 * period, request), 0);
    check("just past twice", AudioLate_Is(t, t + 2 * period + 1000, request), 1);
    check("a 100 ms stall", AudioLate_Is(t, t + 100000000u, request), 1);
    check("a longer previous request is longer", AudioLate_Is(t, t + 3 * period, 4 * request), 0);
    check("a longer previous request, late", AudioLate_Is(t, t + 9 * period, 4 * request), 1);
    /* 1024 frames, then 64: the gap is the 1024's, not the 64's. */
    check("a big request then a small one, on time", AudioLate_Is(t, t + 4 * period, 4 * request), 0);
    check("a small request after a big one is not the threshold", AudioLate_Is(t, t + 6 * period, 4 * request), 0);
    check("a small previous request, a long gap", AudioLate_Is(t, t + 3 * period, request / 4), 1);
    check("under one frame tells nothing", AudioLate_Is(t, t + 100000000u, 3), 0);
    check("a negative request tells nothing", AudioLate_Is(t, t + 100000000u, -4), 0);
    check("one frame, a long stall", AudioLate_Is(t, t + 100000000u, 4), 1);

    /* 15178 callbacks, each on time: none late; with three stalls among
     * them: three. (Whether the stream is empty at each is not an input.) */
    for (i = 0; i < 15178; i++) {
        uint64_t now = t + (uint64_t)i * period + 40000000u * ((i >= 3000) + (i >= 9000) + (i >= 15000));
        late += (unsigned)AudioLate_Is(previous, now, request);
        previous = now;
    }
    check("15178 callbacks, three stalls", (int)late, 3);
    if (!failed) printf("audio_late_test: AudioLate_Is: late gaps counted, on-time ones not\n");
    return failed;
}
