#pragma once
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>

namespace badman {
struct InputState {
    bool ready{};
    std::uint32_t buttons{};
    std::uint8_t analog_x{128}, analog_y{128};
};
enum class Key : std::uint32_t {
    select = 0x0001, start = 0x0008, up = 0x0010, right = 0x0020, down = 0x0040, left = 0x0080,
    left_trigger = 0x0100, right_trigger = 0x0200,
    triangle = 0x1000, circle = 0x2000, cross = 0x4000, square = 0x8000
};
// Event-fed keyboard state; focus loss always releases every held button.
class Keyboard {
public:
    void set_focused(bool focused);
    void set_key(Key key, bool pressed);
    InputState state(bool connected) const { return {connected, connected && focused_ ? buttons_ : 0u, 128, 128}; }
private:
    bool focused_{};
    std::uint32_t buttons_{};
};

class Input {
public:
    using State = InputState;
    using Provider = std::function<State()>;
    // Injectable monotonic nanoseconds and an absolute-deadline waiter. Both
    // must be supplied together; default timing uses steady_clock/sleep_until.
    using Clock = std::function<std::uint64_t()>;
    using Wait = std::function<void(std::uint64_t)>;
    explicit Input(Provider provider = {}, Clock clock = {}, Wait wait = {});
    // Configure before the first observation. Host scheduling shares this exact
    // clock so a controller wait can yield to other PSP workers without sleeping.
    void set_timing(Clock, Wait);
    void set_provider(Provider provider);
    std::uint32_t set_sampling_cycle(std::uint32_t microseconds);
    std::uint32_t set_sampling_mode(std::uint32_t mode);
    std::uint32_t sampling_cycle() const { return cycle_; }
    std::uint32_t sampling_mode() const { return mode_; }
    // Collect at most one current observation when its sampling interval has
    // elapsed. Missing/unready providers are harmless here; no missed interval
    // is backfilled with an invented historical observation.
    bool poll();
    std::size_t available_samples() const { return samples_.size(); }
    // Absolute shared-clock deadline, or0 when a buffered/first observation is
    // immediately eligible. An immediate deadline must not move with wall time.
    std::uint64_t next_observation_deadline() const;
    // nullopt means a valid read needs a future real sample. No waiting,
    // invented samples, output writes or consumption occur in that case.
    // Optional profile filter for genuine packets, applied before guest copy.
    using PacketFilter = std::function<void(std::span<std::uint8_t>)>;
    std::optional<int> try_read_positive(psprecomp::GuestMemory &, std::uint32_t address, std::uint32_t count,
        const PacketFilter &filter = {});
    // Request 1..64 newest available records, in chronological order. A read
    // consumes the unread snapshot and can return fewer than requested. An
    // empty queue may wait only if no other PSP task would be stranded.
    int read_positive(psprecomp::GuestMemory &memory, std::uint32_t address,
        std::uint32_t count, bool allow_blocking);
private:
    using Packet = std::array<std::uint8_t, 16>;
    std::uint64_t sample_deadline() const;
    std::uint64_t current_time() const;
    Provider provider_;
    Clock clock_;
    Wait wait_;
    std::uint64_t origin_ns_{}, last_sample_ns_{};
    std::uint32_t cycle_{}, mode_{};
    bool sampled_{};
    std::deque<Packet> samples_;
};
}
