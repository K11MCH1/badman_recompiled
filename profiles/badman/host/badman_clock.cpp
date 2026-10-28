#include "badman_clock.hpp"
#include "psprecomp/common.hpp"
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <thread>
#endif

namespace badman {
namespace {
void validate_deadline(std::uint64_t deadline) {
    if(deadline>static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max()))
        throw psprecomp::Error("Native wait deadline exceeds the host clock range");
}
#if defined(_WIN32)
[[noreturn]] void timer_error(const char *operation,DWORD error) {
    throw psprecomp::Error(std::string("Native high-resolution wait: ")+operation+
        " failed (Win32 "+std::to_string(error)+")");
}
struct NativeWaiter {
    HANDLE timer{};
    std::mutex mutex;
    NativeWaiter() {
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, supported since Windows10 1803.
        timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002u,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer)timer_error("CreateWaitableTimerExW",GetLastError());
    }
    ~NativeWaiter(){if(timer){CancelWaitableTimer(timer);CloseHandle(timer);}}
    void wait(std::uint64_t deadline) {
        validate_deadline(deadline);
        if(native_clock_now()>=deadline)return;
        std::lock_guard lock(mutex);
        for(;;) {
            const auto now=native_clock_now();
            if(now>=deadline)return;
            const auto remaining=deadline-now;
            // Relative kernel due times are signed negative 100ns intervals.
            // Division/remainder avoids overflow in a (remaining+99) ceil.
            const auto ticks=remaining/100u+(remaining%100u!=0);
            LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(ticks);
            if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))
                timer_error("SetWaitableTimer",GetLastError());
            const auto result=WaitForSingleObject(timer,INFINITE);
            if(result==WAIT_FAILED)timer_error("WaitForSingleObject",GetLastError());
            if(result!=WAIT_OBJECT_0)timer_error("unexpected timer wait result",result);
            // Re-observe the monotonic clock even if the kernel wake is early.
            // Another iteration arms a timer and blocks; it never busy-spins.
        }
    }
};
#endif
}
std::uint64_t native_clock_now() {
    static_assert(std::chrono::steady_clock::is_steady);
    const auto count=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if(count<0)throw psprecomp::Error("Native monotonic clock has a negative epoch");
    return static_cast<std::uint64_t>(count);
}
std::function<void(std::uint64_t)> make_native_waiter() {
#if defined(_WIN32)
    auto owner=std::make_shared<NativeWaiter>();
    return [owner=std::move(owner)](std::uint64_t deadline){owner->wait(deadline);};
#else
    return [](std::uint64_t deadline){
        validate_deadline(deadline);
        while(native_clock_now()<deadline)
            std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(deadline)))));
    };
#endif
}
}
