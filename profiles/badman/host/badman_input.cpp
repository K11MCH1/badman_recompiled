#include "badman_input.hpp"
#include "badman_clock.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace badman {
using psprecomp::Error;
void Keyboard::set_focused(bool focused) {
    focused_ = focused;
    if (!focused) buttons_ = 0;
}
void Keyboard::set_key(Key key, bool pressed) {
    if (!focused_) return;
    const auto mask = static_cast<std::uint32_t>(key);
    if (pressed) buttons_ |= mask; else buttons_ &= ~mask;
}
Input::Input(Provider provider, Clock clock, Wait wait) : provider_(std::move(provider)) {
    set_timing(std::move(clock), std::move(wait));
}
void Input::set_timing(Clock clock, Wait wait) {
    if (sampled_) throw Error("Cannot change controller timing after observations");
    if (static_cast<bool>(clock) != static_cast<bool>(wait))
        throw Error("Input clock and waiter must be configured together");
    if (!clock) {
        clock = native_clock_now;
        wait = make_native_waiter();
    }
    const auto origin = clock();
    clock_ = std::move(clock); wait_ = std::move(wait); origin_ns_ = origin;
}
void Input::set_provider(Provider provider) {
    provider_ = std::move(provider); sampled_ = false; samples_.clear();
}
std::uint32_t Input::set_sampling_cycle(std::uint32_t microseconds) {
    if (microseconds != 0 && (microseconds < 5555 || microseconds > 20000))
        throw Error("Invalid PSP controller sampling cycle");
    const auto previous = cycle_; cycle_ = microseconds; return previous;
}
std::uint32_t Input::set_sampling_mode(std::uint32_t mode) {
    if (mode > 1) throw Error("Invalid PSP controller sampling mode");
    const auto previous = mode_; mode_ = mode; return previous;
}
std::uint64_t Input::current_time() const {
    const auto now = clock_();
    if (now < origin_ns_ || (sampled_ && now < last_sample_ns_)) throw Error("Controller clock moved backwards");
    return now;
}
std::uint64_t Input::sample_deadline() const {
    const std::uint64_t interval = cycle_ ? static_cast<std::uint64_t>(cycle_) * 1000u : 16683333u;
    if (last_sample_ns_ > std::numeric_limits<std::uint64_t>::max() - interval)
        throw Error("Controller sampling deadline overflow");
    return last_sample_ns_ + interval;
}
bool Input::poll() {
    if (!provider_) return false;
    const auto now = current_time();
    if (sampled_ && now < sample_deadline()) return false;
    const auto state = provider_();
    if (!state.ready) return false;
    Packet packet{};
    const auto put_word = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) packet[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
    };
    put_word(0, static_cast<std::uint32_t>((now - origin_ns_) / 1000u));
    put_word(4, state.buttons);
    packet[8] = mode_ ? state.analog_x : 128; packet[9] = mode_ ? state.analog_y : 128;
    samples_.push_back(packet);
    if (samples_.size() > 64) samples_.pop_front();
    last_sample_ns_ = now; sampled_ = true;
    return true;
}
std::uint64_t Input::next_observation_deadline() const {
    if (!provider_) throw Error("Controller input has no active provider");
    return !samples_.empty() || !sampled_ ? 0 : sample_deadline();
}
std::optional<int> Input::try_read_positive(psprecomp::GuestMemory &memory, std::uint32_t address,
    std::uint32_t count, const PacketFilter &filter) {
    // SCE_KERNEL_ERROR_INVALID_SIZE; reject before observing input or touching RAM.
    if (count < 1 || count > 64) return static_cast<int>(0x80000104u);
    if ((address & 3u) || !memory.contains(address, static_cast<std::size_t>(count) * 16))
        throw Error("Invalid SceCtrlData output buffer");
    if (!provider_) throw Error("Controller input has no active provider");
    poll();
    if (samples_.empty()) {
        if (!provider_().ready) throw Error("Controller input provider is not ready");
        return std::nullopt;
    }
    const auto available = std::min<std::size_t>(count, samples_.size());
    std::array<std::uint8_t, 64 * 16> output{};
    auto cursor = output.begin();
    // Reads expose the newest requested part of the collected snapshot. A
    // smaller request must not make a later read replay older button history.
    for (auto sample = samples_.end() - available; sample != samples_.end(); ++sample)
        cursor = std::copy(sample->begin(), sample->end(), cursor);
    if (filter) filter(std::span<std::uint8_t>(output.data(), available * 16));
    memory.copy_in(address, std::span<const std::uint8_t>(output.data(), available * 16));
    samples_.clear();
    return static_cast<int>(available);
}
int Input::read_positive(psprecomp::GuestMemory &memory, std::uint32_t address,
    std::uint32_t count, bool allow_blocking) {
    if (const auto result=try_read_positive(memory,address,count)) return *result;
    if (!allow_blocking) throw Error("Controller read would block without a sole runnable PSP thread");
    if (!sampled_) throw Error("Controller input provider has no available sample");
    const auto deadline=sample_deadline();wait_(deadline);
    if (current_time()<deadline) throw Error("Controller sampling waiter returned before its deadline");
    const auto result=try_read_positive(memory,address,count);
    if (!result) throw Error("Controller input became unavailable while waiting for a sample");
    return *result;
}
}
