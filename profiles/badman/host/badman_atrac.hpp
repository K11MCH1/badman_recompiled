#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace badman {
// Complete resident RIFF/WAVE ATRAC3plus only. Sample positions are per channel.
struct AtracRiff {
    std::uint32_t file_size{}, data_offset{}, data_size{}, block_align{};
    std::uint32_t samples{}, first_offset{}, skip_samples{};
    std::int32_t loop_start{-1}, loop_end{-1}; // Inclusive end, output coordinates.
    std::vector<std::uint8_t> extra;
    static AtracRiff parse(std::span<const std::uint8_t>);
};
class AtracDecoder {
public:
    virtual ~AtracDecoder() = default;
    // Return every packet's stereo S16 PCM, including priming and trailing data.
    virtual std::vector<std::int16_t> decode(const AtracRiff &,
        std::span<const std::uint8_t> encoded_data) = 0;
};
std::unique_ptr<AtracDecoder> make_ffmpeg_atrac_decoder(const std::filesystem::path &directory);

// Standalone foundation. Does not enable a firmware module, bind HLE or play PCM.
// Bounded eager decode keeps immutable PCM for exact repeated resident regions;
// it deliberately does not model ME timing or partial/streamed guest buffers.
class Atrac {
public:
    using DecoderFactory = std::function<std::unique_ptr<AtracDecoder>()>;
    explicit Atrac(DecoderFactory factory = {}) : factory_(std::move(factory)) {}
    void set_decoder_factory(DecoderFactory factory);
    // Validate and retain a real backend without allocating a guest ID or
    // decoding PCM. Idempotent; factory/backend failures remain exceptions.
    // Inactive factory replacement releases this backend; active IDs forbid it.
    void prepare_decoder();
    std::size_t active_stream_count() const noexcept {
        std::size_t count=0; for(const auto &s:streams_)if(s)++count; return count;
    }
    bool release_decoder_if_idle() noexcept {
        if(active_stream_count())return false; decoder_.reset(); return true;
    }
    static constexpr std::uint32_t frame_samples = 2048, codec_delay = 368;
    static constexpr std::size_t max_file_bytes = 4u * 1024u * 1024u;
    static constexpr std::size_t max_pcm_bytes = 64u * 1024u * 1024u;
    static constexpr std::int32_t no_id = static_cast<std::int32_t>(0x80630003u);
    static constexpr std::int32_t bad_id = static_cast<std::int32_t>(0x80630005u);
    static constexpr std::int32_t unknown_format = static_cast<std::int32_t>(0x80630006u);
    static constexpr std::int32_t bad_data = static_cast<std::int32_t>(0x80630008u);
    static constexpr std::int32_t no_loop = static_cast<std::int32_t>(0x80630021u);
    static constexpr std::int32_t no_second_buffer = static_cast<std::int32_t>(0x80630022u);
    static constexpr std::int32_t all_decoded = static_cast<std::int32_t>(0x80630024u);
    struct DecodeResult { std::uint32_t samples{}, ended{}; std::int32_t remaining{}; };
    struct SoundSample { std::int32_t end{}, loop_start{}, loop_end{}; };
    struct SecondBuffer { std::uint32_t position{}, bytes{}; };
    std::int32_t set_data_and_get_id(std::span<const std::uint8_t> riff);
    std::int32_t release(std::int32_t id);
    // An empty output discards real decoded samples, as a null PSP PCM pointer does.
    // Other short spans reject before cursor/output changes. Errors preserve result.
    std::int32_t decode(std::int32_t id, std::span<std::int16_t> output, DecodeResult &result);
    std::int32_t get_remain_frame(std::int32_t id, std::int32_t &remaining) const;
    // Valid complete-resident streams write zero/zero even though return is error22.
    std::int32_t get_second_buffer_info(std::int32_t id, SecondBuffer &result) const;
    std::int32_t set_loop_num(std::int32_t id, std::int32_t loops);
    std::int32_t get_sound_sample(std::int32_t id, SoundSample &result) const;
    std::int32_t get_max_sample(std::int32_t id, std::uint32_t &result) const;
    // Read-only preflight for transactional guest-memory validation, not another
    // registered PSP import. Returns all_decoded at EOF without touching result.
    std::int32_t next_sample_count(std::int32_t id, std::uint32_t &result) const;
private:
    struct Stream {
        AtracRiff riff;
        std::vector<std::int16_t> pcm;
        std::uint32_t position{};
        std::int32_t loops{};
    };
    const Stream *find(std::int32_t id) const;
    Stream *find(std::int32_t id);
    DecoderFactory factory_;
    std::unique_ptr<AtracDecoder> decoder_;
    std::array<std::unique_ptr<Stream>, 2> streams_{};
};
}
