#include "badman_profiler.hpp"

#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace badman {
namespace {
constexpr unsigned kDepth = 16;
constexpr std::size_t kCapacity = 1u << 19; // About nine minutes at 1 kHz.
struct Sample {
    std::atomic<std::uint32_t> depth{}; // Published last; zero while being written.
    std::uintptr_t frames[kDepth];
};
Sample *samples;
std::atomic<std::size_t> next{};
std::uintptr_t stack_low, stack_high;
timer_t timer;
std::string output;
std::atomic<bool> running{};
std::mutex write_mutex;

void on_signal(int, siginfo_t *, void *context) {
    const auto index = next.fetch_add(1, std::memory_order_relaxed);
    if (index >= kCapacity) return;
    auto &sample = samples[index];
    const auto &machine = static_cast<ucontext_t *>(context)->uc_mcontext;
#if defined(__aarch64__)
    std::uintptr_t pc = machine.pc, frame = machine.regs[29], link = machine.regs[30];
#else
    std::uintptr_t pc = machine.gregs[REG_RIP], frame = machine.gregs[REG_RBP], link = 0;
#endif
    unsigned depth = 0;
    sample.frames[depth++] = pc;
    if (link) sample.frames[depth++] = link;
    // Frame records: [frame] = caller's frame, [frame + 8] = return address.
    while (depth < kDepth && frame >= stack_low && frame + 16 <= stack_high && !(frame & 15)) {
        const auto *record = reinterpret_cast<const std::uintptr_t *>(frame);
        if (record[1] && record[1] != sample.frames[depth - 1]) sample.frames[depth++] = record[1];
        if (record[0] <= frame) break;
        frame = record[0];
    }
    sample.depth.store(depth, std::memory_order_release);
}

std::string where(std::uintptr_t address) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(address), &info) || !info.dli_fname) return "?";
    std::string module = info.dli_fname;
    module = module.substr(module.find_last_of('/') + 1);
    char text[32];
    std::snprintf(text, sizeof(text), "+%#zx", static_cast<std::size_t>(address - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    return module + text;
}

void write_profile() {
    std::lock_guard lock(write_mutex);
    const auto count = std::min(next.load(), kCapacity);
    std::map<std::vector<std::uintptr_t>, std::size_t> stacks;
    for (std::size_t i = 0; i < count; ++i) {
        const auto depth = samples[i].depth.load(std::memory_order_acquire);
        if (depth) ++stacks[std::vector<std::uintptr_t>(samples[i].frames, samples[i].frames + depth)];
    }
    std::map<std::uintptr_t, std::string> names;
    std::ostringstream text;
    text << "# samples " << count << '\n';
    // Per-thread CPU time (utime+stime in clock ticks) shows which threads work.
    if (auto *tasks = opendir("/proc/self/task")) {
        while (auto *entry = readdir(tasks)) {
            if (entry->d_name[0] == '.') continue;
            std::ifstream stat(std::string("/proc/self/task/") + entry->d_name + "/stat");
            std::string line;
            std::getline(stat, line);
            const auto close = line.rfind(')');
            if (close == std::string::npos) continue;
            std::istringstream fields(line.substr(close + 2));
            std::string field;
            unsigned long long utime = 0, stime = 0;
            for (unsigned i = 3; fields >> field; ++i) {
                if (i == 14) utime = std::stoull(field);
                if (i == 15) { stime = std::stoull(field); break; }
            }
            text << "# thread " << entry->d_name << ' ' << line.substr(line.find('(') + 1, close - line.find('(') - 1)
                 << " cpu_ticks=" << utime + stime << '\n';
        }
        closedir(tasks);
    }
    for (const auto &[stack, hits] : stacks) {
        text << hits;
        for (std::size_t i = 0; i < stack.size(); ++i) {
            auto &name = names[stack[i]];
            if (name.empty()) name = where(stack[i]);
            text << (i ? ';' : ' ') << name;
        }
        text << '\n';
    }
    const auto partial = output + ".partial";
    std::ofstream(partial, std::ios::binary) << text.str();
    std::rename(partial.c_str(), output.c_str());
}
}

bool SamplingProfiler::start_for_current_thread(const char *path, unsigned hertz) {
    if (running.exchange(true)) return false;
    pthread_attr_t attributes;
    void *base{};
    std::size_t size{};
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) { running = false; return false; }
    pthread_attr_getstack(&attributes, &base, &size);
    pthread_attr_destroy(&attributes);
    stack_low = reinterpret_cast<std::uintptr_t>(base);
    stack_high = stack_low + size;
    samples = new Sample[kCapacity];
    output = path;
    struct sigaction action{};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    sigevent event{};
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGPROF;
    event.sigev_notify_thread_id = static_cast<pid_t>(syscall(SYS_gettid));
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) { running = false; return false; }
    const long interval = 1000000000L / static_cast<long>(hertz ? hertz : 1000);
    itimerspec spec{{0, interval}, {0, interval}};
    timer_settime(timer, 0, &spec, nullptr);
    std::thread([] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (running) write_profile();
        }
    }).detach();
    return true;
}

void SamplingProfiler::finish() {
    if (!running.exchange(false)) return;
    timer_delete(timer);
    write_profile();
}
}
#else
namespace badman {
bool SamplingProfiler::start_for_current_thread(const char *, unsigned) { return false; }
void SamplingProfiler::finish() {}
}
#endif
