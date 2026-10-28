#pragma once
#include <cstdint>
#include <functional>

namespace badman {
// Same monotonic nanosecond epoch as std::chrono::steady_clock. Guest clocks
// remain injectable; this helper changes only the default host wait mechanism.
std::uint64_t native_clock_now();
// Absolute steady-clock deadline, in 0..INT64_MAX nanoseconds. Copies share one
// owned timer and serialize its use; contention can delay a return, never make
// it early. Windows requires a high-resolution waitable timer (10 version1803+).
// No process/system timer-period change, spinning or guest time advancement.
std::function<void(std::uint64_t)> make_native_waiter();
}
