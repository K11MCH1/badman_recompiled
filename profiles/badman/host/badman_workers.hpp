#pragma once
#include <cfenv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace badman {
// Helper threads for raster work. run() hands out job indices to the helpers
// and the calling thread and returns when all are done; each helper first
// adopts the caller's floating-point environment, so results round exactly as
// they would on the calling thread. The first exception is rethrown.
class WorkerPool {
public:
    explicit WorkerPool(unsigned helpers);
    ~WorkerPool();
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    unsigned helpers() const noexcept { return static_cast<unsigned>(threads_.size()); }
    void run(std::size_t count, const std::function<void(std::size_t)> &job);
    // BADMAN_RASTER_THREADS when set (0 = none), else up to six helpers.
    static unsigned default_helpers();
    // Kernel thread ids of every helper started so far in this process (Linux/
    // Android), so a performance hint can cover them along with the game thread.
    static std::vector<int> thread_ids();
private:
    void loop();
    void work(std::uint64_t generation, const std::function<void(std::size_t)> &job);
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, finished_;
    const std::function<void(std::size_t)> *job_{};
    std::size_t count_{}, done_{}, next_{};
    std::uint64_t generation_{};
    fenv_t environment_{};
    std::exception_ptr failure_;
    bool stop_{};
};
}
