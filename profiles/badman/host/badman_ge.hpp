#pragma once
#include "psprecomp/guest_memory.hpp"
#include "badman_raster.hpp"
#include "badman_clut.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

namespace badman {
struct GeInterrupt {
    bool signal{};
    std::uint32_t function{}, argument{}, token{}, next_pc{};
};

// Synchronous, bounded PSP GE command execution. Geometry requires a renderer;
// an unsupported draw never becomes a successfully completed list.
class Ge {
public:
    using InterruptSink = std::function<void(const GeInterrupt &)>;
    explicit Ge(psprecomp::GuestMemory &memory, InterruptSink sink);
    Ge(psprecomp::GuestMemory &memory, InterruptSink sink, std::uint32_t command_budget);
    std::int32_t set_callback(std::uint32_t address);
    void unset_callback(std::int32_t id);
    std::int32_t enqueue(std::uint32_t address, std::uint32_t stall, std::int32_t callback,
                         std::uint32_t options = 0, bool head = false);
    void update_stall(std::int32_t id, std::uint32_t address);
    int list_status(std::int32_t id, bool wait);
    int draw_status(bool wait);
    // The title's watchdog uses mode 1: cancel queued work, never complete it.
    std::int32_t break_queue(std::uint32_t mode, std::uint32_t parameter);
    std::uint32_t command(std::uint8_t id) const { return commands_[id]; }
    std::uint32_t relative_address(std::uint32_t data) const { return relative(data); }
    std::uint32_t vertex_address() const { return vertex_address_; }
    std::uint64_t color_pixels() const { return color_pixels_; }
    std::uint64_t depth_pixels() const { return depth_pixels_; }
    std::uint32_t matrix_word(std::size_t matrix, std::size_t index) const { return matrices_.at(matrix).at(index); }
    std::uint64_t executed_commands() const { return executed_; }
    std::uint64_t completed_lists() const { return completed_; }
    std::uint64_t delivered_callbacks() const { return interrupts_; }
    std::uint64_t cancelled_lists() const { return cancelled_; }
    std::string renderer_description() const { return raster_.renderer_description(); }
    void set_scene_recording(bool enabled, double beyond = 0) { raster_.set_scene_recording(enabled, beyond); }
    std::shared_ptr<Scene> take_scene(std::uint32_t framebuffer_address, std::int64_t time_ns) {
        return raster_.take_scene(framebuffer_address, time_ns);
    }

private:
    enum class State { queued, running, stalled, done, failed, cancelled };
    struct Callback { std::uint32_t signal{}, signal_arg{}, finish{}, finish_arg{}; };
    struct Return { std::uint32_t pc{}, offset{}; };
    struct List {
        std::int32_t id{}, callback{};
        std::uint32_t pc{}, stall{}, previous{};
        State state{State::queued};
        bool ignore_finish{};
        std::vector<Return> stack;
    };
    void pump();
    void run(List &list, std::uint32_t &remaining_commands);
    void interrupt(const List &list, bool signal, std::uint32_t token, std::uint32_t next_pc);
    std::uint32_t relative(std::uint32_t data) const;
    psprecomp::GuestMemory &memory_;
    Raster raster_;
    Clut clut_;
    InterruptSink sink_;
    std::map<std::int32_t, Callback> callbacks_;
    std::map<std::int32_t, List> lists_;
    std::deque<std::int32_t> queue_;
    std::array<std::uint32_t, 256> commands_{};
    std::array<std::array<std::uint32_t, 128>, 5> matrices_{};
    std::array<std::uint32_t, 5> cursors_{};
    std::uint32_t offset_{};
    std::uint32_t vertex_address_{};
    std::int32_t next_callback_{0}, next_list_{1};
    bool pumping_{};
    std::uint32_t command_budget_{};
    std::uint64_t executed_{}, completed_{}, interrupts_{}, cancelled_{};
    std::uint64_t color_pixels_{}, depth_pixels_{};
};
}
