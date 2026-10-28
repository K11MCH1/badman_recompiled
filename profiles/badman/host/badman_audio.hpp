#pragma once
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace badman {
struct AudioProgress {
    std::uint64_t ticket{};
    std::uint32_t frames_played{};
    bool complete{};
};
// All calls run on the owning host thread. submit must retain no sample pointer
// on failure. On success samples remain valid until complete or close returns.
// close must stop callbacks and sample reads before returning, without throwing.
class AudioTransport {
public:
    virtual ~AudioTransport() = default;
    virtual void open(std::uint32_t channel) = 0;
    virtual void close(std::uint32_t channel) noexcept = 0;
    virtual void submit(std::uint32_t channel, std::uint64_t ticket,
                        std::span<const std::int16_t> stereo_samples) = 0;
    virtual std::vector<AudioProgress> poll() = 0;
};
// Opens an actual Windows10+ system XAudio2 device or throws. No silent fallback.
std::unique_ptr<AudioTransport> make_xaudio2_transport();
// The platform's real output device: XAudio2 on Windows, AAudio on Android.
std::unique_ptr<AudioTransport> make_native_audio_transport();

// Bounded nonblocking PCM foundation, not an HLE/scheduler or codec substitute.
// Own and destroy this object on one host thread; callbacks never access it.
class Audio {
public:
    explicit Audio(std::unique_ptr<AudioTransport> transport);
    ~Audio();
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    Audio(Audio &&) = delete;
    Audio &operator=(Audio &&) = delete;
    static constexpr std::uint32_t channel_count = 8, queue_capacity = 2;
    static constexpr std::int32_t busy = static_cast<std::int32_t>(0x80260002u);
    static constexpr std::int32_t invalid_channel = static_cast<std::int32_t>(0x80260003u);
    static constexpr std::int32_t no_channel = static_cast<std::int32_t>(0x80260005u);
    static constexpr std::int32_t invalid_size = static_cast<std::int32_t>(0x80260006u);
    static constexpr std::int32_t invalid_format = static_cast<std::int32_t>(0x80260007u);
    static constexpr std::int32_t not_reserved = static_cast<std::int32_t>(0x80260008u);
    static constexpr std::int32_t invalid_volume = static_cast<std::int32_t>(0x8026000Bu);
    std::int32_t reserve(std::int32_t requested, std::uint32_t frames, std::uint32_t format);
    std::int32_t release(std::int32_t channel);
    // Actual close barrier cancels pending playback; cancelled tickets are not
    // reported as played/completed. HLE must reject while a guest waiter exists.
    std::int32_t cancel_release(std::int32_t channel);
    std::int32_t set_length(std::int32_t channel, std::uint32_t frames);
    struct Submission { std::int32_t result{}; std::uint64_t ticket{}; };
    Submission submit(const psprecomp::GuestMemory &memory, std::int32_t channel,
                      std::uint32_t left_volume, std::uint32_t right_volume, std::uint32_t address);
    void poll();
    std::int32_t rest_length(std::int32_t channel);
    bool completed(std::uint64_t ticket) const;
    struct Statistics {
        std::uint64_t submitted_buffers{}, submitted_frames{}, nonzero_samples{};
        std::uint64_t completed_buffers{}, completed_frames{};
    };
    Statistics statistics() const noexcept { return statistics_; }
private:
    struct Buffer {
        std::uint64_t ticket{};
        std::uint32_t frames{}, played{};
        bool done{};
        std::vector<std::int16_t> samples;
    };
    struct Channel { bool reserved{}, mono{}; std::uint32_t frames{}; std::deque<std::unique_ptr<Buffer>> queue; };
    void check_thread() const;
    void check_healthy() const;
    std::int32_t validate_channel(std::int32_t channel) const;
    static bool valid_length(std::uint32_t frames);
    std::unique_ptr<AudioTransport> transport_;
    std::array<Channel, channel_count> channels_{};
    std::thread::id owner_{std::this_thread::get_id()};
    std::uint64_t next_ticket_{1};
    std::vector<std::uint64_t> cancelled_tickets_;
    bool faulted_{};
    Statistics statistics_;
};
}
