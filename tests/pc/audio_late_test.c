/* AudioLate_Is (src/pc/audio/late_callback.h): the HUD's audio underruns are
 * callbacks that ran late, not callbacks that found the stream empty. */
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
    check("a larger request is longer", AudioLate_Is(t, t + 3 * period, 4 * request), 0);

    /* 15178 requests, each on time with a stream empty at every one: none
     * late; with three stalls among them: three. */
    for (i = 0; i < 15178; i++) {
        uint64_t now = t + (uint64_t)i * period + 40000000u * ((i >= 3000) + (i >= 9000) + (i >= 15000));
        late += (unsigned)AudioLate_Is(previous, now, request);
        previous = now;
    }
    check("15178 callbacks, three stalls", (int)late, 3);
    if (!failed) printf("audio_late_test: late callbacks counted, empty streams not\n");
    return failed;
}
