#pragma once

namespace badman {
// Development-only wall-clock sampler for one thread, for devices where perf
// events are unavailable (retail Android phones). A timer signal records the
// sampled thread's program counter and frame-pointer chain; every few seconds
// a writer thread folds the samples into "count module+offset;..." lines
// (leaf first) at `path`, to be symbolized on the build machine. It also
// records each thread's CPU time. Starting it is explicit (BADMAN_PROFILE);
// it never runs in a normal session.
class SamplingProfiler {
public:
    // Samples the calling thread at about `hertz` per second of wall time.
    // Returns false (and changes nothing) when the platform lacks support.
    static bool start_for_current_thread(const char *path, unsigned hertz = 1000);
    // Writes the final profile; safe to call when nothing was started.
    static void finish();
};
}
