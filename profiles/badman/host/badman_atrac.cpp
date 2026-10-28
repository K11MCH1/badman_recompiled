#include "badman_atrac.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace badman {
namespace {
class InvalidRiff : public std::runtime_error {
public:
    explicit InvalidRiff(std::int32_t code) : std::runtime_error("Invalid or unsupported resident ATRAC3plus RIFF"), code(code) {}
    std::int32_t code;
};
std::uint16_t le16(std::span<const std::uint8_t> data, std::size_t at) {
    return data[at] | (static_cast<std::uint16_t>(data[at + 1]) << 8);
}
std::uint32_t le32(std::span<const std::uint8_t> data, std::size_t at) {
    return le16(data, at) | (static_cast<std::uint32_t>(le16(data, at + 2)) << 16);
}
bool tag(std::span<const std::uint8_t> data, std::size_t at, const char *value) {
    return std::memcmp(data.data() + at, value, 4) == 0;
}
}
AtracRiff AtracRiff::parse(std::span<const std::uint8_t> data) {
    if (data.size() < 12 || data.size() > Atrac::max_file_bytes || !tag(data, 0, "RIFF") || !tag(data, 8, "WAVE"))
        throw InvalidRiff(Atrac::unknown_format);
    const std::uint64_t file_size = static_cast<std::uint64_t>(le32(data, 4)) + 8;
    if (file_size < 12 || file_size > data.size()) throw InvalidRiff(Atrac::bad_data);
    AtracRiff result; result.file_size = static_cast<std::uint32_t>(file_size);
    bool format = false, fact = false, payload = false, sample = false;
    std::uint32_t raw_loop_start = 0, raw_loop_end = 0, loop_count = 0;
    for (std::size_t at = 12; at < file_size;) {
        if (file_size - at < 8) throw InvalidRiff(Atrac::bad_data);
        const auto size = le32(data, at + 4);
        const std::uint64_t next = static_cast<std::uint64_t>(at) + 8 + size + (size & 1);
        if (next > file_size) throw InvalidRiff(Atrac::bad_data);
        const auto body = data.subspan(at + 8, size);
        if (tag(data, at, "fmt ")) {
            static constexpr std::array<std::uint8_t, 16> guid{
                0xBF,0xAA,0x23,0xE9,0x58,0xCB,0x71,0x44,0xA1,0x19,0xFF,0xFA,0x01,0xE4,0xCE,0x62};
            if (format || size != 52 || le16(body, 0) != 0xFFFE || le16(body, 2) != 2 ||
                le32(body, 4) != 44100 || le16(body, 14) != 0 || le16(body, 16) != 34 ||
                le16(body, 18) != 2048 || le32(body, 20) != 3 ||
                !std::equal(guid.begin(), guid.end(), body.begin() + 24)) throw InvalidRiff(Atrac::unknown_format);
            result.block_align = le16(body, 12);
            if (!result.block_align || result.block_align > 8192) throw InvalidRiff(Atrac::bad_data);
            result.extra.assign(body.begin() + 40, body.end()); format = true;
        } else if (tag(data, at, "fact")) {
            // The observed Sony variant has an 8-byte fact chunk. Other variants
            // have different skip fields; reject them rather than guessing.
            if (fact || size != 8) throw InvalidRiff(Atrac::bad_data);
            result.samples = le32(body, 0); result.first_offset = le32(body, 4); fact = true;
        } else if (tag(data, at, "data")) {
            if (payload || !size) throw InvalidRiff(Atrac::bad_data);
            result.data_offset = static_cast<std::uint32_t>(at + 8); result.data_size = size; payload = true;
        } else if (tag(data, at, "smpl")) {
            if (sample || size < 36) throw InvalidRiff(Atrac::bad_data);
            sample = true; loop_count = le32(body, 28);
            if (loop_count > 1 || (loop_count && size != 60)) throw InvalidRiff(Atrac::bad_data);
            if (loop_count) {
                if (le32(body, 40) != 0 || le32(body, 52) != 0) throw InvalidRiff(Atrac::bad_data);
                raw_loop_start = le32(body, 44); raw_loop_end = le32(body, 48);
            }
        }
        at = static_cast<std::size_t>(next);
    }
    if (!format || !fact || !payload || result.data_size % result.block_align ||
        !result.samples || result.samples > INT32_MAX) throw InvalidRiff(Atrac::bad_data);
    const std::uint64_t raw_samples = static_cast<std::uint64_t>(result.data_size / result.block_align) * Atrac::frame_samples;
    // PSP hardware headerfields test: zero fact offset means one whole codec
    // frame, rather than zero; the 368-sample decoder delay is additional.
    const std::uint64_t skip = static_cast<std::uint64_t>(result.first_offset ? result.first_offset : Atrac::frame_samples) + Atrac::codec_delay;
    if (raw_samples * 4 > Atrac::max_pcm_bytes || skip + result.samples > raw_samples)
        throw InvalidRiff(Atrac::bad_data);
    result.skip_samples = static_cast<std::uint32_t>(skip);
    if (loop_count) {
        if (raw_loop_start < result.first_offset || raw_loop_end < raw_loop_start ||
            raw_loop_end - result.first_offset >= result.samples) throw InvalidRiff(Atrac::bad_data);
        result.loop_start = static_cast<std::int32_t>(raw_loop_start - result.first_offset);
        result.loop_end = static_cast<std::int32_t>(raw_loop_end - result.first_offset);
    }
    return result;
}
const Atrac::Stream *Atrac::find(std::int32_t id) const {
    return id >= 0 && id < static_cast<std::int32_t>(streams_.size()) ? streams_[id].get() : nullptr;
}
Atrac::Stream *Atrac::find(std::int32_t id) { return const_cast<Stream *>(std::as_const(*this).find(id)); }
void Atrac::set_decoder_factory(DecoderFactory factory) {
    if (std::any_of(streams_.begin(), streams_.end(), [](const auto &stream) { return static_cast<bool>(stream); }))
        throw psprecomp::Error("Cannot replace an active ATRAC3plus decoder factory");
    decoder_.reset();
    factory_ = std::move(factory);
}
void Atrac::prepare_decoder() {
    if (decoder_) return;
    if (!factory_) throw psprecomp::Error("No ATRAC3plus decoder factory configured");
    auto decoder = factory_();
    if (!decoder) throw psprecomp::Error("ATRAC3plus decoder factory returned null");
    decoder_ = std::move(decoder); // Publish readiness only after full construction.
}
std::int32_t Atrac::set_data_and_get_id(std::span<const std::uint8_t> bytes) {
    AtracRiff riff;
    try { riff = AtracRiff::parse(bytes); } catch (const InvalidRiff &error) { return error.code; }
    const auto free = std::find_if(streams_.begin(), streams_.end(), [](const auto &stream) { return !stream; });
    if (free == streams_.end()) return no_id;
    prepare_decoder();
    auto pcm = decoder_->decode(riff, bytes.subspan(riff.data_offset, riff.data_size));
    const std::uint64_t expected = static_cast<std::uint64_t>(riff.data_size / riff.block_align) * frame_samples * 2;
    if (pcm.size() != expected) throw psprecomp::Error("ATRAC3plus decoder returned an incomplete or excessive PCM stream");
    auto stream = std::make_unique<Stream>(); stream->riff = std::move(riff); stream->pcm = std::move(pcm);
    const auto id = static_cast<std::int32_t>(free - streams_.begin()); *free = std::move(stream); return id;
}
std::int32_t Atrac::release(std::int32_t id) {
    if (!find(id)) return bad_id;
    streams_[id].reset(); return 0;
}
std::int32_t Atrac::decode(std::int32_t id, std::span<std::int16_t> output, DecodeResult &result) {
    auto *stream = find(id); if (!stream) return bad_id;
    const auto &riff = stream->riff;
    std::uint32_t count = 0;
    const auto preflight = next_sample_count(id, count); if (preflight) return preflight;
    const bool loop = riff.loop_start >= 0 && stream->loops != 0 && stream->position <= static_cast<std::uint32_t>(riff.loop_end);
    const auto end = loop ? static_cast<std::uint32_t>(riff.loop_end) + 1 : riff.samples;
    const auto raw_position = riff.skip_samples + stream->position;
    if (!output.empty() && output.size() < count * 2) throw psprecomp::Error("ATRAC3plus PCM output span is too short");
    if (!output.empty()) std::copy_n(stream->pcm.begin() + static_cast<std::size_t>(raw_position) * 2, count * 2, output.begin());
    stream->position += count;
    if (loop && stream->position == end) {
        stream->position = static_cast<std::uint32_t>(riff.loop_start);
        if (stream->loops > 0) --stream->loops;
    }
    result = {count, stream->position == riff.samples ? 1u : 0u, -1}; return 0;
}
std::int32_t Atrac::next_sample_count(std::int32_t id, std::uint32_t &result) const {
    const auto *stream = find(id); if (!stream) return bad_id;
    const auto &riff = stream->riff;
    if (stream->position >= riff.samples) return all_decoded;
    const bool loop = riff.loop_start >= 0 && stream->loops != 0 && stream->position <= static_cast<std::uint32_t>(riff.loop_end);
    const auto end = loop ? static_cast<std::uint32_t>(riff.loop_end) + 1 : riff.samples;
    result = std::min(frame_samples - (riff.skip_samples + stream->position) % frame_samples, end - stream->position);
    return 0;
}
std::int32_t Atrac::get_remain_frame(std::int32_t id, std::int32_t &remaining) const {
    if (!find(id)) return bad_id;
    remaining = -1; return 0;
}
std::int32_t Atrac::get_second_buffer_info(std::int32_t id, SecondBuffer &result) const {
    if (!find(id)) return bad_id;
    result = {}; return no_second_buffer;
}
std::int32_t Atrac::set_loop_num(std::int32_t id, std::int32_t loops) {
    auto *stream = find(id); if (!stream) return bad_id;
    if (stream->riff.loop_start < 0) return no_loop;
    if (loops < -1) throw psprecomp::Error("ATRAC3plus loop counts below -1 are not supported");
    stream->loops = loops; return 0;
}
std::int32_t Atrac::get_sound_sample(std::int32_t id, SoundSample &result) const {
    const auto *stream = find(id); if (!stream) return bad_id;
    result = {static_cast<std::int32_t>(stream->riff.samples - 1), stream->riff.loop_start, stream->riff.loop_end}; return 0;
}
std::int32_t Atrac::get_max_sample(std::int32_t id, std::uint32_t &result) const {
    if (!find(id)) return bad_id;
    result = frame_samples; return 0;
}
}
