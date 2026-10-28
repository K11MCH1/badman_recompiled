#pragma once
#include "badman_video.hpp"
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace badman {
struct PsmfHeader {
    std::uint32_t stream_offset{}, stream_size{}, width{}, height{}, streams{};
    std::uint64_t start_pts{}, end_pts{};
    static PsmfHeader parse(std::span<const std::uint8_t>);
};
struct VideoPayload {
    std::vector<std::uint8_t> bytes;
    std::int64_t pts{video_no_timestamp}, dts{video_no_timestamp};
    std::uint64_t source_end{}; // Program-stream byte offset past this PES.
    bool audio{};
    std::uint32_t first_frame_offset{}; // First ATRAC frame header after the private PES prefix.
};
class ProgramStream {
public:
    std::vector<VideoPayload> feed(std::span<const std::uint8_t>, bool end = false);
    void reset();
private:
    std::vector<std::uint8_t> pending_;
    std::uint64_t consumed_{};
};
class Mpeg {
public:
    using DecoderFactory = std::function<std::unique_ptr<VideoDecoder>()>;
    using AudioDecoderFactory = std::function<std::unique_ptr<MpegAudioDecoder>()>;
    struct RingCallback {
        std::uint32_t ring{}, function{}, data{}, packets{}, argument{}, gp{};
    };
    struct PutStep { std::optional<RingCallback> callback; std::int32_t result{}; };
    explicit Mpeg(DecoderFactory factory = {}, AudioDecoderFactory audio = {})
        : factory_(std::move(factory)), audio_factory_(std::move(audio)) {}
    void set_decoder_factory(DecoderFactory);
    void set_audio_decoder_factory(AudioDecoderFactory);
    std::optional<RingCallback> invoke(std::uint32_t nid, psprecomp::Runtime &,
        psprecomp::AllegrexContext &);
    PutStep complete_put(psprecomp::GuestMemory &, std::uint32_t ring, std::int32_t result);
    bool initialized() const { return initialized_; }
    std::size_t context_count() const { return contexts_.size(); }
    bool has_live_resources() const noexcept { return initialized_ || !rings_.empty() || !contexts_.empty(); }
private:
    struct Ring {
        std::uint32_t address{}, packets{}, data{}, callback{}, argument{}, gp{}, context{};
        std::uint32_t read{}, written{}, in_use{};
        std::uint64_t accepted{}, released{};
        struct Put { std::uint32_t remaining{}, offered{}, total{}; };
        std::optional<Put> put;
    };
    struct Context {
        std::uint32_t address{}, ring{}, frame_width{};
        std::optional<PsmfHeader> header;
        std::unique_ptr<VideoDecoder> decoder;
        std::unique_ptr<MpegAudioDecoder> audio_decoder;
        ProgramStream stream;
        struct Access { EncodedVideo coded; std::uint64_t packet_end{}; };
        std::deque<Access> access;
        std::map<std::uint32_t, Access> pending_au;
        struct AudioAccess {
            std::vector<std::uint8_t> bytes;
            std::int64_t pts{video_no_timestamp};
            std::uint64_t packet_end{};
        };
        std::deque<AudioAccess> audio_access;
        std::map<std::uint32_t, AudioAccess> pending_audio;
        std::map<std::uint32_t, std::uint32_t> audio_es;
        std::vector<std::uint8_t> audio_pending;
        std::map<std::uint64_t, std::int64_t> audio_anchors;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> audio_ranges;
        std::uint64_t audio_bytes{}, audio_consumed{}, video_released{}, audio_released{};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        std::uint64_t elementary_bytes{}, stream_bytes{};
        std::uint32_t stream_handle{}, audio_handle{};
        bool es{}, ended{};
    };
    Context &context(psprecomp::GuestMemory &, std::uint32_t pointer);
    void write_ring(psprecomp::GuestMemory &, const Ring &);
    RingCallback next_callback(Ring &);
    void feed(Context &, std::span<const std::uint8_t>, bool end);
    void feed_audio(Context &, const VideoPayload &);
    void release_through(psprecomp::GuestMemory &, Context &, std::uint64_t end);
    void release_consumed(psprecomp::GuestMemory &, Context &);
    DecoderFactory factory_;
    AudioDecoderFactory audio_factory_;
    bool initialized_{};
    std::map<std::uint32_t, Ring> rings_;
    std::map<std::uint32_t, Context> contexts_;
};
}
