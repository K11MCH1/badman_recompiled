#include "badman_workers.hpp"
#include <algorithm>
#include <cstdlib>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace badman {
namespace {
std::mutex ids_mutex;
std::vector<int> ids;
}
std::vector<int> WorkerPool::thread_ids() {
    std::lock_guard lock(ids_mutex);
    return ids;
}
WorkerPool::WorkerPool(unsigned helpers) {
    for (unsigned i = 0; i < helpers; ++i) threads_.emplace_back([this] { loop(); });
}
WorkerPool::~WorkerPool() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    wake_.notify_all();
    for (auto &thread : threads_) thread.join();
}
unsigned WorkerPool::default_helpers() {
    if (const auto requested = std::getenv("BADMAN_RASTER_THREADS"); requested && *requested)
        return std::min(15u, static_cast<unsigned>(std::strtoul(requested, nullptr, 10)));
    return std::min(6u, std::max(1u, std::thread::hardware_concurrency()) - 1u);
}
void WorkerPool::run(std::size_t count, const std::function<void(std::size_t)> &job) {
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        fegetenv(&environment_);
        job_ = &job; count_ = count; next_ = 0; done_ = 0; failure_ = nullptr;
        generation = ++generation_;
    }
    wake_.notify_all();
    work(generation, job);
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [&] { return done_ == count_; });
    job_ = nullptr;
    if (failure_) std::rethrow_exception(failure_);
}
void WorkerPool::loop() {
#if defined(__linux__)
    { std::lock_guard lock(ids_mutex); ids.push_back(static_cast<int>(gettid())); }
#endif
    std::uint64_t seen = 0;
    for (;;) {
        const std::function<void(std::size_t)> *job{};
        fenv_t environment{};
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || (generation_ != seen && job_); });
            if (stop_) return;
            seen = generation_; job = job_; environment = environment_;
        }
        fesetenv(&environment);
        work(seen, *job);
    }
}
// Indices are claimed under the lock and only for the job of `generation`, so
// a helper finishing late can never take part of the next job.
void WorkerPool::work(std::uint64_t generation, const std::function<void(std::size_t)> &job) {
    for (;;) {
        std::size_t index;
        {
            std::lock_guard lock(mutex_);
            if (generation_ != generation || next_ >= count_) return;
            index = next_++;
        }
        try { job(index); }
        catch (...) { std::lock_guard lock(mutex_); if (!failure_) failure_ = std::current_exception(); }
        std::lock_guard lock(mutex_);
        if (++done_ == count_) finished_.notify_all();
    }
}
}
