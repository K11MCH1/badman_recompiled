#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace badman {
constexpr std::int64_t video_no_timestamp = INT64_MIN;
struct EncodedVideo {
    std::vector<std::uint8_t> bytes;
    std::int64_t pts{video_no_timestamp}, dts{video_no_timestamp};
    std::uint64_t source_end{}; // Number of elementary-stream bytes parsed.
};
struct VideoFrame {
    std::uint32_t width{}, height{};
    std::int64_t pts{video_no_timestamp};
    std::vector<std::uint8_t> rgba;
};
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual std::vector<EncodedVideo> parse(std::span<const std::uint8_t>, std::int64_t pts,
        std::int64_t dts, bool end) = 0;
    virtual std::optional<VideoFrame> decode(const EncodedVideo &) = 0;
    virtual std::optional<VideoFrame> drain() = 0;
    virtual void reset() = 0;
};
// Opens an actual H.264 decoder from an explicit, separately installed DLL dir.
// Missing/unavailable codecs throw; no successful inert decoder is substituted.
std::unique_ptr<VideoDecoder> make_ffmpeg_video_decoder(const std::filesystem::path &directory);
// Stateful packet decoding for PSMF ATRAC3plus. Container framing/timestamps
// belong to Mpeg; no RIFF trimming/looping is applied to these raw codec frames.
class MpegAudioDecoder {
public:
    virtual ~MpegAudioDecoder() = default;
    virtual std::vector<std::int16_t> decode(std::span<const std::uint8_t>) = 0;
    virtual void reset() = 0;
};
std::unique_ptr<MpegAudioDecoder> make_ffmpeg_mpeg_audio_decoder(const std::filesystem::path &directory);
}
