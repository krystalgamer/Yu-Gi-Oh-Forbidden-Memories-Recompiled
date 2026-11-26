/* What every window backend shares: the interrupt-style clock and the
 * scripted test input. The 1 kHz SIGALRM stands in for the console's
 * interrupts and must fire on the main thread, between instructions of the
 * game (busy-waits poll what the handlers update); backends block signals
 * on every thread they create. Windows interrupts the main thread from a
 * timer thread instead (win32.c). */
#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "platform.h"
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/debug/crash.h"
#include "pc/debug/monitor.h"
#include "pc/debug/crash_test.h"
#include "pc/sdk/display.h"
#include "pc/debug/profile.h"
#include "pc/compat/signal.h"
#include "pc/audio/spu.h"
#include "pc/audio/fifo.h"
#include "pc/render/soft_gpu.h"
#include "controls_runtime.h"
#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include "win32.h"
#else
#include <sys/syscall.h>
#include "signal_context.h"
#endif

static volatile unsigned vblank_count;
static void (*vblank_handler)(void);
static void (*tick_handler)(uint64_t, uint64_t);
static volatile int rate = 100;
static uint64_t real_prev, virtual_now, next_vblank;
static unsigned virtual_remainder; /* hundredths of a microsecond a rate other than 100% left over */
#define VBLANK_PERIOD 16683 /* NTSC; a vsynced present re-phases the real-time clock's to the display */
static volatile unsigned vblank_period = VBLANK_PERIOD;
static volatile int step_pending;
static float present_refresh;
static int present_cap;           /* frames per second; 0 display refresh; -1 every frame */
static uint64_t present_next_us;  /* the present pacer's next slot */
static int vsync_on;              /* the backend's presents wait for the display (Platform_SetVSync) */
static uint64_t last_vsync_real;
/* When the real-time clock's last VBlank was due, and when it came (Platform_NotifyPresent). */
static uint64_t vblank_due_at, vblank_came_at;
static unsigned watchdog_seconds = 5;
static volatile int watchdog_reported;
/* The virtual clock. MEMORIES_DETERMINISTIC=1 asks for it, headless or in a
 * window, for as long as the game runs; headless, uncapped and dumping a
 * frame (MEMORIES_HEADLESS + MEMORIES_DUMP_FRAME + MEMORIES_SPEED=-1, what
 * the smoke tests have always set) gets it too. The run must come out the
 * same on every host. Virtual time then moves only in
 * Platform_WaitVBlank, 1 ms at a time, so the disc, the root counter and the
 * VBlank see the same time between the same two game frames whatever the
 * host's speed or its timer. Driven by the host timer, a frame that took
 * longer to compute got more ticks, so more sectors, and a load finished a
 * frame or ten earlier on one host than another (the random seed, which the
 * name entry screen draws on every frame, then parted company). A VSync
 * that only reads the count steps it too (Platform_PollTime). In a window
 * the frames are then shown at the game speed (pace(), which only sleeps),
 * and pause and frame step work; headless runs flat out. */
static int deterministic_dump, deterministic_asked, deterministic_paced;
/* Asked for at run time (Platform_SetVirtualClock): a replay records or plays. */
static int clock_requested;
static int headless_run;
static void (*clock_rebase)(uint64_t);
static volatile uint64_t deterministic_last_wait; /* real time the game last waited for a VBlank */
static uint64_t pace_next;                         /* real time the next paced VBlank is due */
static int controlled;                             /* a control client decides (Platform_ControlAttach) */

static int virtual_clock(void)
{
    return deterministic_asked || clock_requested || (deterministic_dump && rate == -1);
}
#define DETERMINISTIC_STEP 1000
#define DETERMINISTIC_SPIN 1000000 /* real us outside a wait before the timer steps a spinning game */

static int deterministic_env(void)
{
    const char *asked = getenv("MEMORIES_DETERMINISTIC");
    return asked && *asked && strcmp(asked, "0") != 0;
}

static uint64_t now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* The silent sink under MEMORIES_DETERMINISTIC=1 (Platform_StartSilentAudio):
 * no mixer thread. A thread paced by the host's clock mixed as much as real
 * time allowed, while the game ran flat out on virtual time, so the voice
 * levels the sound driver reads (SpuGetKeyStatus, SpuGetVoiceEnvelope) had
 * moved a different amount at the same game instant in each run: a voice
 * finished its release a tick earlier in one, the driver freed and reused
 * its note records in another order, and two lockstep runs parted in the
 * driver's memory (0x801E0380.., 0x801E15D8..0x801E1B0F). Here the SPU is
 * mixed on the game thread as virtual time passes, 44.1 frames for each
 * 1 ms step, before the step's tick runs the sound driver (a step the
 * interrupt clock's handler takes only owes its frames, which the next step
 * on the game thread mixes before its own tick). The fraction of
 * a frame still owed goes in the save state ("platform-mixer"), since
 * virtual time itself is the process's. */
static void (*stepped_mixer)(int16_t *, size_t);
static FILE *stepped_dump;    /* MEMORIES_DUMP_AUDIO, written as it is mixed */
static uint32_t mix_fraction; /* ten-thousandths of a frame owed, below one frame */
static uint32_t mix_owed;     /* whole frames owed: held (a save state), or stepped from the timer */

/* The stepped mixer to a device (Platform_SetSteppedAudio): while a replay
 * records or plays, the game thread mixes as above and an audio device's
 * thread (or the silent sink's) plays those frames from a FIFO (fifo.h)
 * instead of mixing them itself, so what the sound driver reads of the
 * voices moves with the game's time, not with when the device asks. */
static void (*thread_mix)(int16_t *, size_t); /* what a device or sink thread mixes (Platform_AudioThreadStarted) */
static int stepped_wanted;                    /* a replay asked for it */
static int stepped_by_request;                /* stepped_mixer is thread_mix through the FIFO */
static AudioFifoGate fifo_gate;               /* the threads play the FIFO, or mix (fifo.h) */
static AudioFifo audio_fifo;
static int sink_drains;                       /* the silent sink's thread drains it, not a device */

static void mix_owe(unsigned us)
{
    uint32_t total = mix_fraction + 441u * us; /* 44100 frames a second */
    mix_owed += total / 10000u;
    mix_fraction = total % 10000u;
}

static void mix_owed_frames(void)
{
    static int16_t buffer[256 * 2];
    static unsigned unflushed;
    if (Spu_Held()) return; /* a save state is being taken or applied: the frames stay owed */
    while (mix_owed) {
        size_t count = mix_owed < 256 ? mix_owed : 256;
        stepped_mixer(buffer, count);
        mix_owed -= (uint32_t)count;
        if (stepped_by_request) AudioFifo_Push(&audio_fifo, buffer, count);
        if (stepped_dump) {
            fwrite(buffer, sizeof(int16_t) * 2, count, stepped_dump);
            unflushed += (unsigned)count;
            if (unflushed >= 256) { /* as often as the thread flushed, so a killed run loses as little */
                fflush(stepped_dump);
                unflushed = 0;
            }
        }
    }
}

/* One virtual step's mix. `here` is zero where the step runs in the
 * interrupt clock's handler (MEMORIES_CLOCK=interrupt), which only owes the
 * frames; the next step on the game thread mixes them. */
static void mix_step(int here)
{
    if (!stepped_mixer) return;
    mix_owe(DETERMINISTIC_STEP);
    if (here) mix_owed_frames();
}

static void deliver_vblank(void)
{
    vblank_count++;
    if (vblank_handler) vblank_handler();
}

/* The cooperative clock, the default: no interrupt. The clock's time is taken, and
 * the ticks and VBlanks it owes are run, where the game calls in to wait or
 * to read the time (VSync, Platform_WaitVBlank, Platform_PollTime), so the
 * game's interrupt code never runs in the middle of anything. Every loop the
 * game polls the clock in passes through one of those (notes/pc-build.md,
 * "Cooperative clock"). MEMORIES_CLOCK=interrupt brings back the timer that
 * interrupts the game, as the console's VBlank would; the sampling profiler
 * (MEMORIES_PROFILE) samples from that timer, so it chooses it too. */
static int cooperative;
/* VBlanks the clock may still deliver, -1 for no limit (Platform_LimitVBlanks). */
static volatile int vblank_budget = -1;
/* When the tick handler's next timed work is due, in real us (Platform_SetTickDue). */
static volatile uint64_t tick_due;

static void advance(uint64_t real_now, uintptr_t eip)
{
    uint64_t elapsed = real_prev ? real_now - real_prev : 0;
    real_prev = real_now;
    /* A longer gap is a stall (a breakpoint, a window being dragged), not
     * time the game should catch up. Serviced cooperatively, a heavy frame
     * can legitimately run past 100 ms between two services. */
    if (elapsed > (cooperative ? 500000u : 100000u)) elapsed = 0;
    if (virtual_clock()) {
        /* Time passes in Platform_WaitVBlank. Only a game that has spun for
         * a second without waiting for a VBlank gets steps from the timer,
         * so that it cannot hang; no loop the game runs does that today, and
         * no frame takes that long to compute. Such a step is reported, with
         * where the game was: a loop that polls the clock without a wait. */
        if (deterministic_last_wait && real_now - deterministic_last_wait > DETERMINISTIC_SPIN) {
            static uintptr_t reported[16];
            static unsigned reported_count, steps;
            unsigned i;
            virtual_now += DETERMINISTIC_STEP;
            mix_step(cooperative);
            if (tick_handler) tick_handler(virtual_now, virtual_now);
            steps++;
            for (i = 0; i < reported_count && reported[i] != eip; i++) {}
            if (i == reported_count && reported_count < 16) {
                reported[reported_count++] = eip;
                /* The timer's handler: Log_Signal, never stdio or LOG (log.c). */
                Log_Signal(LOG_FRAMES, "clock: the game spun a second without a wait, at 0x%lx (%ld steps so far)",
                           (long)eip, (long)steps, 0, 0, 0, 0);
            }
        }
        return;
    }
    /* The fraction is carried: a loop that polls the clock sees a
     * microsecond or two per call, and at 50% each such microsecond came
     * out as nothing, so a game waiting on the clock that way (the boot,
     * the movies) stood still for seconds at a time. */
    if (rate > 0) {
        uint64_t scaled = elapsed * (uint64_t)rate + virtual_remainder;
        virtual_now += scaled / 100;
        virtual_remainder = (unsigned)(scaled % 100);
    }
    if (rate == -1) virtual_now += elapsed;
    if (tick_handler) tick_handler(virtual_now, real_now);
    if (!next_vblank) next_vblank = virtual_now;
    while (virtual_now >= next_vblank && vblank_budget != 0) {
        /* MEMORIES_TRACE=frames: how late each VBlank comes against when it
         * was due, in the clock's microseconds (real ones at 100%): the
         * wait's wakeups, and frames that ran past a VBlank. */
        static uint64_t late_total;
        static unsigned late_max, late_over_1ms, late_count;
        unsigned late = (unsigned)(virtual_now - next_vblank);
        late_total += late;
        late_max = late > late_max ? late : late_max;
        late_over_1ms += late > 1000;
        if (++late_count == 600) {
            /* Also run from the timer's handler: Log_Signal (log.c). */
            Log_Signal(LOG_FRAMES, "VBlank lateness over 600 VBlanks: mean %ld us, max %ld us, %ld over 1 ms",
                       (long)(late_total / 600), (long)late_max, (long)late_over_1ms, 0, 0, 0);
            late_total = 0;
            late_max = late_over_1ms = late_count = 0;
        }
        vblank_due_at = next_vblank;
        next_vblank += vblank_period;
        if (virtual_now > next_vblank + 4 * vblank_period) next_vblank = virtual_now;
        vblank_came_at = virtual_now;
        deliver_vblank();
        if (vblank_budget > 0) vblank_budget--;
    }
    if (step_pending && vblank_budget != 0) {
        step_pending = 0;
        vblank_due_at = vblank_came_at = virtual_now;
        deliver_vblank();
        if (vblank_budget > 0) vblank_budget--;
    }
}

/* A clock pass is a few hundred bytes deep (about 300 measured over a duel),
 * and the game's VBlank handler may go deeper. */
#define SERVICE_SCRUB_BYTES 8192u

int Platform_DeadStackScrubbable(const void *frame, size_t bytes)
{
    uintptr_t here = (uintptr_t)frame;
    /* Well clear of the stack's guard pages, and inside it. */
    return cooperative && here >= MEMORIES_GAME_STACK_BASE + 0x20000u + bytes &&
           here < MEMORIES_GAME_STACK_BASE + MEMORIES_GAME_STACK_SIZE;
}

void Platform_ScrubGameStackBelow(uintptr_t sp, size_t bytes)
{
    uintptr_t floor_ = MEMORIES_GAME_STACK_BASE + 0x20000u, low;
    if (!cooperative || sp <= floor_ || sp > MEMORIES_GAME_STACK_BASE + MEMORIES_GAME_STACK_SIZE) return;
    low = sp - floor_ > bytes ? sp - bytes : floor_;
    memset((void *)low, 0, sp - low);
}

/* The cooperative clock's service point: the time now, and whatever it owes. */
static int servicing; /* inside advance(): the handlers it runs are the game's interrupt code */
static void service(void)
{
    sigset_t set, previous;
    if (!cooperative) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    servicing++;
    advance(now_us(), (uintptr_t)__builtin_return_address(0));
    servicing--;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

/* The interrupt clock's tick is running (on_tick): the software GPU makes
 * no frozen copy there (soft_gpu.h, SoftGpu_InTick). */
static volatile int ticking;

static void on_tick(uintptr_t eip, void *context)
{
    uint64_t real_now = now_us();
    while (CrashTest_TickHang) {
    }
    Profile_Sample(eip);
    {
        /* MEMORIES_TRACE=frames: where the game is when it has run 30 ms
         * past its last VSync, once per such stretch. */
        static uint64_t reported_stretch;
        if (last_vsync_real && real_now - last_vsync_real > 30000 && reported_stretch != last_vsync_real &&
            Log_Wanted(LOG_FRAMES)) {
            reported_stretch = last_vsync_real;
            Log_Signal(LOG_FRAMES, "long stretch without a VSync: %ld us so far, at 0x%lx",
                       (long)(real_now - last_vsync_real), (long)eip, 0, 0, 0, 0);
        }
    }
#ifndef _WIN32 /* Windows watches from the clock thread (Win32_SetStallReporter) */
    if (watchdog_seconds && rate != 0 && !watchdog_reported &&
        real_now - last_vsync_real >= (uint64_t)watchdog_seconds * 1000000u) {
        watchdog_reported = 1;
        Crash_ReportHang(context);
    }
#else
    (void)context;
#endif
    ticking++;
    advance(real_now, eip);
    ticking--;
}

#ifndef _WIN32
static void on_alarm(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    (void)number;
    (void)info;
    on_tick((uintptr_t)SIGNAL_CONTEXT_PC(user), context);
}
#endif

/* The 1 kHz signal is aimed at the main thread itself (SIGEV_THREAD_ID), not
 * the process: a process-directed signal lands on any thread that does not
 * block it, and graphics drivers start threads of their own after the
 * backend has finished creating its own with the signal masked. With the
 * game's interrupt code running on a driver thread the main thread stalls
 * on that driver's locks. The process-wide timer is only a fallback. */
/* The software GPU's deferred raster holds the tick off while it works on
 * what it keeps back (soft_gpu.h, SoftGpu_HoldClock); it nests the calls
 * itself, so the mask it found is put back as it was. */
static sigset_t gpu_mask;

static unsigned long long gpu_clock(void) { return now_us(); }

static int gpu_in_tick(void) { return ticking != 0; }

static void hold_clock_for_gpu(int hold)
{
    sigset_t set;
    if (hold) {
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, &gpu_mask);
    } else {
        sigprocmask(SIG_SETMASK, &gpu_mask, NULL);
    }
}

int Platform_StartTimers(void (*tick)(uint64_t, uint64_t), void (*vblank)(void))
{
#ifndef _WIN32
    struct sigaction action;
 #ifndef __APPLE__
    struct sigevent event;
    struct itimerspec spec;
    timer_t timer;
 #endif
#endif
    tick_handler = tick;
    vblank_handler = vblank;
    SoftGpu_Clock = gpu_clock; /* the deferred raster's settles, timed (SoftGpuDeferredStats) */
    /* The virtual clock (above). The frame-dump combination is kept as it
     * was; MEMORIES_DETERMINISTIC=1 alone needs neither a dump nor speed -1. */
    deterministic_dump = (getenv("MEMORIES_HEADLESS") != NULL || getenv("MEMORIES_DETERMINISTIC") != NULL) &&
                         (getenv("MEMORIES_DUMP_FRAME") != NULL || getenv("MEMORIES_DUMP_FRAMES") != NULL);
    deterministic_asked = deterministic_env();
    headless_run = getenv("MEMORIES_HEADLESS") != NULL;
    deterministic_paced = (deterministic_asked || clock_requested) && !headless_run;
    last_vsync_real = now_us();
    {
        const char *watchdog = getenv("MEMORIES_WATCHDOG");
        if (watchdog && *watchdog) watchdog_seconds = (unsigned)strtoul(watchdog, NULL, 10);
    }
    Profile_Init();
    {
        const char *clock = getenv("MEMORIES_CLOCK"), *profile = getenv("MEMORIES_PROFILE");
        if (clock && *clock) cooperative = strcmp(clock, "interrupt") != 0;
        else cooperative = !(profile && *profile);
#if defined(_WIN32) && defined(__x86_64__)
        if (!cooperative) {
            fprintf(stderr, "memories-pc: the 64-bit build has only the cooperative clock yet\n");
            cooperative = 1;
        }
#endif
        if (!cooperative) {
            fprintf(stderr, "memories-pc: interrupt clock%s\n",
                    clock && *clock ? " (MEMORIES_CLOCK=interrupt)" : ", which MEMORIES_PROFILE samples from");
            SoftGpu_HoldClock = hold_clock_for_gpu;
            SoftGpu_InTick = gpu_in_tick;
        }
    }
#ifdef _WIN32
    Win32_SetStallReporter(Crash_ReportHang, watchdog_seconds);
    if (cooperative) return Win32_StartWatch(); /* the clock thread only watches for a stall */
    return Win32_StartInterrupt(on_tick);
#else
    if (cooperative) return 0; /* the crash monitor process watches for a stall */
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_alarm;
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) {
        return -1;
    }
 #ifndef __APPLE__
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGALRM;
    event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    spec.it_interval.tv_sec = spec.it_value.tv_sec = 0;
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec = 1000000;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) == 0 && timer_settime(timer, 0, &spec, NULL) == 0) {
        return 0;
    }
 #endif
    {
        struct itimerval fallback;
        fallback.it_interval.tv_sec = fallback.it_value.tv_sec = 0;
        fallback.it_interval.tv_usec = fallback.it_value.tv_usec = 1000;
        return setitimer(ITIMER_REAL, &fallback, NULL) ? -1 : 0;
    }
#endif
}

unsigned Platform_VBlankCount(void)
{
    return vblank_count;
}

void Platform_SetClockRate(int percent)
{
    if (percent < -1) percent = -1;
    if (percent > 400) percent = 400;
    if (percent > 0 && percent < 25) percent = 25;
    rate = percent;
    Monitor_Shared()->paused = percent == 0; /* no VSync is expected: not a freeze */
}

int Platform_ClockRate(void) { return rate; }

#ifndef __ANDROID__ /* android.c: GLES only */
int Platform_HasDesktopGL(void) { return 1; }
int Platform_GuestMemoryHelp(char *why, size_t size)
{
    (void)why;
    (void)size;
    return 0;
}
#endif
void Platform_StepFrame(void) { step_pending = 1; }

void Platform_ControlAttach(int attached)
{
    static unsigned watchdog_before;
    if (!attached == !controlled) return;
    controlled = attached;
    if (attached) {
        watchdog_before = watchdog_seconds;
        watchdog_seconds = 0;
        if (!cooperative) {
            fprintf(stderr, "memories-pc: control: the interrupt clock (MEMORIES_CLOCK=interrupt) keeps running "
                            "while the client holds the game; lockstep needs the default clock\n");
        }
    } else {
        watchdog_seconds = watchdog_before;
    }
#ifdef _WIN32
    Win32_SetStallReporter(Crash_ReportHang, watchdog_seconds);
#endif
    Monitor_Modal(attached); /* the crash monitor's freeze check */
}

int Platform_VirtualClock(void)
{
    return virtual_clock();
}

void Platform_OnClockRebase(void (*rebase)(uint64_t))
{
    clock_rebase = rebase;
}

/* From the game thread at a VSync. Going virtual, what a state carries of
 * the clock is brought into the range a load takes back (Platform_State):
 * the VBlank owed or due within four periods, at the console's period (a
 * vsynced present had put the display's), and the tick handler's own
 * schedule, which ran on the host's time, moved to virtual time
 * (Platform_OnClockRebase); going back, the other way. */
void Platform_SetVirtualClock(int on)
{
    sigset_t set, previous;
    int was;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    was = virtual_clock();
    clock_requested = on != 0;
    deterministic_paced = (deterministic_asked || clock_requested) && !headless_run;
    if (tick_handler && was != virtual_clock()) {
        uint64_t now = now_us();
        if (virtual_clock()) {
            vblank_period = VBLANK_PERIOD;
            if (next_vblank && next_vblank <= virtual_now) next_vblank = virtual_now + 1;
            if (next_vblank > virtual_now + vblank_period) next_vblank = virtual_now + vblank_period;
            deterministic_last_wait = now;
            pace_next = 0;
            if (clock_rebase) clock_rebase(virtual_now);
        } else {
            real_prev = now;
            /* Virtual time stood on while this clock ran: when the last VBlank
             * was due and came are stale, and a vsynced present would take
             * them for a VBlank long past (Platform_NotifyPresent). The last
             * one is now. */
            vblank_due_at = vblank_came_at = virtual_now;
            if (clock_rebase) clock_rebase(now);
        }
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void Platform_ControlIdle(void)
{
    /* Waiting on purpose, not frozen: every watch sees a heartbeat. */
    __atomic_add_fetch(&Monitor_Shared()->heartbeat, 1, __ATOMIC_RELEASE);
    last_vsync_real = now_us();
    /* A present meanwhile (the wait's still) services the clock: the virtual
     * one must not take the wait for a spin and step. */
    if (deterministic_last_wait) deterministic_last_wait = last_vsync_real;
#ifdef _WIN32
    Win32_Heartbeat();
#endif
}

void Platform_ControlHold(int held)
{
    sigset_t set, previous;
    uint64_t now;
    if (held) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    now = now_us();
    real_prev = now;                /* the real-time clock does not catch up the wait */
    last_vsync_real = now;
    if (deterministic_last_wait) deterministic_last_wait = now; /* nor does the virtual one take it for a spin */
    pace_next = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

float Platform_GameHz(void)
{
    return rate > 0 ? 1000000.0f / (float)vblank_period * (float)rate / 100.0f : 0.0f;
}

void Platform_SetPresentCap(int fps)
{
    present_cap = fps < -1 ? -1 : fps;
    present_next_us = 0;
}

int Platform_PresentCap(void) { return present_cap; }

unsigned Platform_PresentPeriodUs(void)
{
    if (present_cap == -1) return 0;
    if (present_cap > 0) return (unsigned)(1000000.0f / (float)present_cap + 0.5f);
    return present_refresh > 0.0f ? (unsigned)(1000000.0f / present_refresh + 0.5f) : 0u;
}

/* One present per period, on a fixed grid so a game frame that arrives a
 * little before its slot (frames come from a 1 kHz clock, and the game rate
 * may sit within a few microseconds of the cap) still takes it: a frame is
 * due from half a period before its slot. A stall resets the grid. */
static int vsync_paces_clock(void);

int Platform_PresentDue(void)
{
    unsigned period = Platform_PresentPeriodUs();
    uint64_t now = now_us();
    if (!period) return 1;
    /* The swap paces the game to the display (Platform_NotifyPresent), a
     * frame each refresh: a cap at the refresh or above would only drop the
     * frames that run a 64th of a refresh early while a driver's queue fills,
     * so its swaps never come to wait and the game runs that much fast. */
    if (vsync_paces_clock() && period <= (unsigned)(1010000.0f / present_refresh)) {
        present_next_us = 0;
        return 1;
    }
    if (!present_next_us || now >= present_next_us + period) present_next_us = now;
    if (now + period / 2 < present_next_us) return 0;
    present_next_us += period;
    return 1;
}

int Platform_VSyncPacesGame(void)
{
    if (rate == -1) return 0;
    if (present_refresh <= 0.0f) return rate <= 100; /* every display refreshes at 59.94 Hz or faster */
    return Platform_GameHz() <= present_refresh + 0.5f;
}

void Platform_VSyncHeartbeat(void)
{
    sigset_t set, previous;
    MonitorShared *monitor = Monitor_Shared();
    service();
    monitor->frame = Memories_PresentedFrames();
    monitor->vblank = vblank_count;
    monitor->running = 1;
    __atomic_add_fetch(&monitor->heartbeat, 1, __ATOMIC_RELEASE);
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    last_vsync_real = now_us();
    watchdog_reported = 0;
#ifdef _WIN32
    Win32_Heartbeat();
#endif
    sigprocmask(SIG_SETMASK, &previous, NULL);
#ifdef _WIN32
    /* A VSync(-1) polling loop (the movie waiting for sectors) spends most
     * of its time reading the clock, outside the executable, where the clock
     * thread only leaves the tick pending. Take it here, as the waits do. */
    Win32_ServiceInterrupt();
#endif
    CrashTest_Frame();
}

void Platform_SetVBlankPeriod(unsigned us)
{
    sigset_t set, previous;
    if (!us) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    vblank_period = us;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void Platform_SetPresentRefresh(float hz)
{
    present_refresh = hz;
    present_next_us = 0;
}

float Platform_PresentRefresh(void) { return present_refresh; }

/* A vsynced present at 100% on a display within a hertz of 60 paces the
 * game. A swap that held the game until the display took the frame (or
 * until the driver's queue had room for it) has just returned at the
 * display's refresh, so the game's VBlank is due now, and the next one a
 * refresh on: the game then has the whole refresh to compute and draw its
 * next frame before the next swap holds it again. The VBlank used to be put
 * a period less 1.5 ms after every swap, which fits only a frame computed and
 * drawn in 1.5 ms: since the present comes before the wait (in VSync(0)), the
 * time past that went onto every frame, and 6 ms of game and present
 * (widescreen, the CRT pass and a model mod on an older PC) made 45 fps of a
 * 60 Hz display. A VBlank comes no sooner than a 64th of a refresh before a
 * refresh since the last one was due: a swap that does not block (a driver
 * queueing ahead, a hidden window) runs the game at most that much faster
 * than the display, which is what fills such a queue until its swaps do
 * block. A VBlank that came at this present's own service is the one the
 * swap held for, and the next goes a refresh (less that 64th) from it.
 * Presents that are not the game's (the menu repainted during a frame) come
 * before that, and leave the VBlank where it was. */
/* Never under the virtual clock: the display's phase is not the game's. */
static int vsync_paces_clock(void)
{
    return vsync_on && rate == 100 && present_refresh >= 59.0f && present_refresh <= 61.0f && !virtual_clock();
}

void Platform_SetVSync(int on)
{
    vsync_on = on;
    present_next_us = 0;
}

void Platform_NotifyPresent(uint64_t real_now_us, int vsynced)
{
    (void)real_now_us;
    service(); /* the cooperative clock's time, as of after the present */
    if (vsynced && vsync_paces_clock()) {
        sigset_t set, previous;
        unsigned period = (unsigned)(1000000.0f / present_refresh + 0.5f);
        uint64_t due;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, &previous);
        vblank_period = period;
        due = (vblank_came_at == virtual_now ? virtual_now : vblank_due_at) + period - period / 64;
        next_vblank = due > virtual_now ? due : virtual_now;
        sigprocmask(SIG_SETMASK, &previous, NULL);
        service(); /* a VBlank due now ends the VSync(0) this present came from */
    }
}

void Platform_StopTimers(void)
{
#ifdef _WIN32
    Win32_StopInterrupt();
#else
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, NULL);
#endif
}

#ifdef _WIN32
/* Sleeps from `now` toward `due` (real us), at most `longest` us, and not
 * at all in the last Win32_SleepMargin before it, which the caller polls
 * through as it did: a Windows timer can wake that late, so `due` is kept
 * as well as the loop's polling kept it. */
static void sleep_toward(uint64_t now, uint64_t due, unsigned longest)
{
    unsigned margin = Win32_SleepMargin();
    if (due > now + margin) {
        uint64_t rest = due - now - margin;
        Win32_Sleep(rest < longest ? (unsigned)rest : longest);
    }
}

/* The cooperative clock's wait between two services (Platform_WaitVBlank),
 * on Windows: naps of 0.5 ms, as on Linux, since the disc's plain reads,
 * its completions and the MDEC move a piece at each service, but none in
 * the last Win32_SleepMargin before the VBlank or the sequencer's next tick
 * (Platform_SetTickDue), so that neither comes later than when the wait
 * polled the clock without a break. */
static void nap_before_due(void)
{
    uint64_t now = now_us(), due = tick_due ? tick_due : UINT64_MAX;
    if (rate > 0 && next_vblank > virtual_now) {
        /* As of the service just run (advance): real_prev is its real time. */
        uint64_t vblank = real_prev + (next_vblank - virtual_now) * 100u / (unsigned)rate;
        if (vblank < due) due = vblank;
    } else if (rate > 0) {
        due = now; /* due, and held back by the VBlank limit: poll */
    }
    sleep_toward(now, due, 500);
}
#endif

/* Whether the FIFO's fill paces the game (fifo.h, AudioFifo_PacePeriod):
 * while a device plays the stepped mixer's frames, at 100 %. Not the silent
 * sink's thread (no device: MEMORIES_NO_AUDIO, none found), whose nanosleep
 * drains slower than real time and would only slow a game nobody hears. */
static int fifo_paces(void)
{
    return stepped_by_request && !sink_drains && rate == 100;
}

/* The virtual clock in a window (MEMORIES_DETERMINISTIC=1 without
 * MEMORIES_HEADLESS), before a VBlank: paused, nothing moves until the pause
 * ends or a frame step lets one VBlank through; running, the VBlank waits
 * for its real time at the game speed. It only sleeps and pumps the
 * window's events, so the game sees the same as uncapped.
 *
 * While a replay records or plays, a device plays the frames the game mixed
 * from the FIFO, and at 100 % its fill moves the grid (fifo_paces): each
 * period is nudged by at most 1/128 toward AUDIO_FIFO_TARGET, and a VBlank
 * late after a stall does not catch up while the fill is above it, so the
 * latency stays near 70 ms instead of growing by each stall's silence, and a
 * device clock a little off real time sets the pace instead of drifting the
 * fill into skips or underruns. It reads the FIFO's two counts and writes
 * only pace_next: when the host sleeps, never what the game sees. Under it:
 * - Turbo (400 %) and any other speed keep the plain grid: the game mixes
 *   faster or slower than the device plays, and the device skips past
 *   AUDIO_FIFO_MOST or runs dry, as before.
 * - Paused (0 %), nothing is mixed: the device fades out and plays silence
 *   until AUDIO_FIFO_PRIME frames are queued again.
 * - A frame step lets one VBlank's frames (about 735) through; every third
 *   step or so primes the device for a short burst. Neither reaches the rule
 *   (the rate is 0) and the grid starts again after either.
 * - A control client steps the game itself: pace() does nothing then (and
 *   headless, nothing is paced), so the rule is off with it; the silent
 *   sink's thread, the one draining a headless client's FIFO, never paces. */
static void pace(void)
{
#ifndef _WIN32
    struct timespec nap = {0, 500000};
#endif
    uint64_t now, period;
    if (!deterministic_paced || controlled) return;
    while (rate == 0 && !step_pending && !Platform_ShouldQuit()) {
        deterministic_last_wait = now_us(); /* waiting, not spinning (advance) */
        Platform_PumpEvents();
#ifdef _WIN32
        Win32_Heartbeat(); /* paused, not hung */
        Win32_Sleep(500);
#else
        nanosleep(&nap, NULL);
#endif
    }
    step_pending = 0;
    if (rate <= 0) {
        pace_next = 0;
        return;
    }
    period = (uint64_t)vblank_period * 100u / (unsigned)rate;
    now = now_us();
    if (!pace_next || now > pace_next + 100000u) pace_next = now; /* a stall: start the grid again */
    if (fifo_paces()) {
        int catch_up;
        period = AudioFifo_PacePeriod(AudioFifo_Queued(&audio_fifo), period, &catch_up);
        if (!catch_up && pace_next < now) pace_next = now; /* late, but the sound is queued: no VBlanks back to back */
    }
    while (now < pace_next) {
#ifdef _WIN32
        sleep_toward(now, pace_next, ~0u);
#else
        struct timespec rest = {0, (long)(pace_next - now) * 1000};
        nanosleep(&rest, NULL);
#endif
        now = now_us();
    }
    pace_next += period;
}

void Platform_PollTime(void)
{
    sigset_t set, previous;
    if (!virtual_clock()) {
        service();
        return;
    }
    if (next_vblank && virtual_now + DETERMINISTIC_STEP >= next_vblank) pace();
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    if (!next_vblank) next_vblank = virtual_now + vblank_period;
    virtual_now += DETERMINISTIC_STEP;
    mix_step(1);
    if (tick_handler) tick_handler(virtual_now, virtual_now);
    if (virtual_now >= next_vblank) {
        next_vblank += vblank_period;
        deliver_vblank();
    }
    deterministic_last_wait = now_us();
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

/* DrawSync's service point (libgpu.c). The console takes its VBlank
 * interrupts while the game computes and draws a frame, so when a frame runs
 * past a VBlank, Graphics_SyncFrame's read of the game's VBlank counter
 * (D_8009B0C8) already counts it: that frame gets D_8009B0D8 = 2, then
 * VSync(0) returns at the next VBlank with the counter at 0. The cooperative
 * clock took those VBlanks only at VSync(0)'s entry, after Graphics_SyncFrame
 * had read the counter and set it to -1, so a slow frame left the counter at
 * 1 or more when Input_UpdatePads ran. Input_UpdatePads takes that as a
 * VBlank that came in after VSync and publishes the frame's newly pressed and
 * repeat bits once more on the next frame: one tap of Down moved the title
 * menu's cursor two places (every screen, every input device). Graphics_SyncFrame
 * calls DrawSync(0) just before its read, after the frame's drawing is
 * rasterized, so the VBlanks the frame overran are delivered here, where the
 * console would have delivered them. */
__attribute__((noinline)) static int service_clock(void)
{
    if (servicing) return 0; /* DrawSync from the game's own interrupt code */
    service();
    return 1;
}

void Platform_ServiceClock(void)
{
    if (service_clock()) {
        PLATFORM_SCRUB_DEAD_STACK(SERVICE_SCRUB_BYTES);
    }
}

/* VSync(0) on the console returns at the first VBlank after its entry, with
 * the game's VBlank callback run once; later ones interrupt the next frame,
 * whose Graphics_SyncFrame counts them. A present or a host hiccup that runs
 * past two VBlank periods inside VSync(0) had the clock deliver them all
 * there, in one go, after Graphics_SyncFrame had set the game's counter to
 * -1: Input_UpdatePads then found it at 1 or more, took the frame as late
 * and published its presses twice (see Platform_ServiceClock). VSync(0)
 * limits the clock to one VBlank until it returns; those still owed come at
 * the next service point, normally the next frame's DrawSync. */
void Platform_LimitVBlanks(int count)
{
    vblank_budget = count;
}

void Platform_SetTickDue(uint64_t real_us)
{
    tick_due = real_us;
}

__attribute__((noinline)) static void wait_vblank(unsigned count_at_entry)
{
    struct timespec nap = {0, 500000};
    /* Spent, and still no VBlank since the caller's count: a wait nested in
     * VSync(0) after its VBlank, which the limit must not hold forever. */
    if (vblank_budget == 0 && vblank_count == count_at_entry) vblank_budget = -1;
    while (vblank_count == count_at_entry && !Platform_ShouldQuit()) {
        if (virtual_clock()) {
            sigset_t set, previous;
            pace();
            if (Platform_ShouldQuit()) break;
            sigemptyset(&set);
            sigaddset(&set, SIGALRM);
            sigprocmask(SIG_BLOCK, &set, &previous);
            if (!next_vblank) next_vblank = virtual_now + vblank_period;
            while (virtual_now < next_vblank) {
                virtual_now += DETERMINISTIC_STEP;
                mix_step(1);
                if (tick_handler) tick_handler(virtual_now, virtual_now);
            }
            next_vblank += vblank_period;
            deliver_vblank();
            deterministic_last_wait = now_us();
            sigprocmask(SIG_SETMASK, &previous, NULL);
#ifdef _WIN32
            Win32_ServiceInterrupt();
#endif
            continue;
        }
        if (rate == -1) {
            sigset_t set, previous;
            uint64_t real_now;
            sigemptyset(&set);
            sigaddset(&set, SIGALRM);
            sigprocmask(SIG_BLOCK, &set, &previous);
            real_now = now_us();
            advance(real_now, 0);
            if (vblank_count == count_at_entry) {
                if (!next_vblank) next_vblank = virtual_now;
                virtual_now = next_vblank;
                advance(real_now, 0);
            }
            sigprocmask(SIG_SETMASK, &previous, NULL);
        } else {
            service();
            if (vblank_count != count_at_entry) break;
            if (rate == 0) Platform_PumpEvents();
#ifdef _WIN32
            if (rate == 0) Win32_Heartbeat(); /* paused, not hung */
            Win32_ServiceInterrupt();
            if (vblank_count != count_at_entry) break;
            if (cooperative) {
                nap_before_due();
                continue;
            }
#endif
            nanosleep(&nap, NULL); /* on Windows (interrupt clock), returns at once */
        }
    }
}

/* The game's wait for a VBlank: the clock's passes and the VBlank handlers it
 * runs are all below this frame. */
void Platform_WaitVBlank(unsigned count_at_entry)
{
    wait_vblank(count_at_entry);
    PLATFORM_SCRUB_DEAD_STACK(SERVICE_SCRUB_BYTES);
}

/* The VBlank count is what the game sees through VSync(-1). The clock's
 * phase goes in a chunk of its own ("platform-clock", so that states
 * without it keep loading "platform"): how far off the next VBlank is, in
 * the clock's microseconds. Under the virtual clock the ticks between two VBlanks are the
 * 1 ms steps up to it, 16 or 17 as the phase falls, and each tick reads disc
 * sectors and runs the sound driver; a load that kept the loading process's
 * phase gave a frame a tick more or less than the game that saved it had,
 * a file finished loading a frame apart and the campaign map parted company
 * from it some 350 frames on. */
void Platform_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{(void *)&vblank_count, sizeof(vblank_count)}};
    int64_t phase = next_vblank ? (int64_t)(next_vblank - virtual_now) : 0;
    MemoriesStateField clock = {&phase, sizeof(phase)};
    Memories_StateChunk(state, "platform", fields, 1);
    if (Memories_StateChunk(state, "platform-clock", &clock, 1) && phase > 0 &&
        phase <= 4 * (int64_t)vblank_period) {
        next_vblank = virtual_now + (uint64_t)phase;
    }
    if (stepped_mixer) {
        /* The stepped mixer's phase (above): the steps after a load mix as
         * many frames as they did in the game that saved. A state without
         * it starts from a whole frame, whatever the loading game owed
         * (unlike the loader's "keeps its current state" for a missing
         * chunk: what this game owed has nothing to do with the state's
         * game). Values no game saves (more than a second owed) are a
         * damaged state: clamped, so a step never mixes for seconds. */
        const MemoriesStateField mixer[] = {{&mix_fraction, sizeof(mix_fraction)}, {&mix_owed, sizeof(mix_owed)}};
        if (Memories_StateChunk(state, "platform-mixer", mixer, 2)) {
            if (mix_fraction >= 10000u) mix_fraction = 0;
            if (mix_owed > 44100u) mix_owed = 44100u;
        } else if (Memories_StateLoading(state)) {
            mix_fraction = mix_owed = 0;
        }
    }
}

/* MEMORIES_INPUT="600:0008,610:0000": hex pad bits applied from a frame on;
 * MEMORIES_INPUT2 the same for the second pad, which then counts as
 * connected. Returns the bits in force at `frame`. */
static uint16_t scripted_bits(int port, unsigned frame)
{
    static const char *script[2];
    static int loaded[2];
    static uint16_t bits[2];
    if (!loaded[port]) {
        loaded[port] = 1;
        script[port] = getenv(port ? "MEMORIES_INPUT2" : "MEMORIES_INPUT");
    }
    while (script[port] && *script[port]) {
        char *end;
        unsigned long at = strtoul(script[port], &end, 10);
        if (*end != ':' || at > frame) {
            break;
        }
        bits[port] = (uint16_t)strtoul(end + 1, &end, 16);
        LOG(LOG_INPUT, "script frame %u pad %d %04x", frame, port + 1, bits[port]);
        script[port] = *end == ',' ? end + 1 : end;
    }
    return bits[port];
}

uint16_t Platform_ScriptedBits(unsigned frame)
{
    return scripted_bits(0, frame);
}

uint16_t Platform_ScriptedBits2(unsigned frame)
{
    return scripted_bits(1, frame);
}

int Platform_ScriptedPad2(void)
{
    const char *script = getenv("MEMORIES_INPUT2");
    return script && *script;
}

/* MEMORIES_DUMP_AUDIO=path: no device; mix in real time into raw s16le
 * stereo 44.1 kHz so output can be inspected without speakers. With no path
 * this is the silent sink: the SPU still has to run, because the sound
 * driver reads voice status and envelopes, and the CD/XA input is only
 * drained by the mixer. Under MEMORIES_DETERMINISTIC=1 neither has a thread:
 * the game thread mixes as virtual time passes (mix_step, above), and the
 * dump is written from there, as long as the game's virtual time. */
static void *run_silent(void *path)
{
    static int16_t buffer[256 * 2];
    FILE *file = path ? fopen(path, "wb") : NULL;
    struct timespec nap = {0, 256 * 1000000000ll / 44100};
    for (;;) {
        Platform_ThreadMix(buffer, 256);
        if (file) {
            fwrite(buffer, sizeof(buffer), 1, file);
            fflush(file);
        }
        nanosleep(&nap, NULL);
    }
    return NULL;
}

int Platform_StartSilentAudio(void (*mix)(int16_t *, size_t), const char *dump_path)
{
    pthread_t thread;
    sigset_t all, previous;
    int error;
#ifndef __ANDROID__ /* the phone keeps its mixer thread: Spu_Mix on its game thread is untried */
    if (deterministic_env()) {
        stepped_dump = dump_path ? fopen(dump_path, "wb") : NULL;
        stepped_mixer = mix;
        return 0;
    }
#endif
    sink_drains = 1; /* no device's clock for the pacer to follow (fifo_paces) */
    Platform_AudioThreadStarted(mix);
    /* The timer signal is the game's interrupt and must stay on the main thread. */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    error = pthread_create(&thread, NULL, run_silent, (void *)dump_path);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return error ? -1 : 0;
}

void Platform_ThreadMix(int16_t *out, size_t frames)
{
    AudioFifoGate_Call(&fifo_gate, &audio_fifo, out, frames, thread_mix);
}

static void stepped_audio(int on)
{
    if (on) {
        /* No thread to take over (headless under MEMORIES_DETERMINISTIC,
         * which already steps, or no sound started yet), or done. */
        if (stepped_by_request || !thread_mix || stepped_mixer) return;
        AudioFifo_Reset(&audio_fifo);
        mix_fraction = mix_owed = 0;
        AudioFifoGate_Switch(&fifo_gate, 1);
        stepped_mixer = thread_mix;
        stepped_by_request = 1;
        fprintf(stderr, "memories-pc: audio: the game thread mixes as the virtual clock steps; the device plays it "
                        "from a FIFO\n");
    } else if (stepped_by_request) {
        stepped_mixer = NULL;
        stepped_by_request = 0;
        mix_fraction = mix_owed = 0;
        /* A thread that read the gate as on may still be inside AudioFifo_Pop,
         * and the next start resets the FIFO: the switch waits until it has left. */
        AudioFifoGate_Switch(&fifo_gate, 0);
        fprintf(stderr, "memories-pc: audio: the device mixes again\n");
    }
}

void Platform_AudioThreadStarted(void (*mix)(int16_t *, size_t))
{
    thread_mix = mix;
    if (stepped_wanted) stepped_audio(1);
}

void Platform_SetSteppedAudio(int on)
{
    stepped_wanted = on != 0;
    stepped_audio(stepped_wanted);
}

void Platform_AudioFifoStats(unsigned *dropped, unsigned *underruns, unsigned *skipped)
{
    if (dropped) *dropped = audio_fifo.dropped;
    if (underruns) *underruns = AudioFifo_Underruns(&audio_fifo);
    if (skipped) *skipped = AudioFifo_Skipped(&audio_fifo);
}
