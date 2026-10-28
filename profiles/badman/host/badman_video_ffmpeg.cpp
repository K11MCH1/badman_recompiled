#include "badman_video.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include "badman_ffmpeg_loader.hpp"
#if defined(BADMAN_FFMPEG_RUNTIME)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#endif

namespace badman {
#if defined(BADMAN_FFMPEG_RUNTIME)
namespace {
class FfmpegVideo final : public VideoDecoder {
    FfmpegLibraries libraries_{"video"};
#define VIDEO_API(name) decltype(&name) name##_{};
    VIDEO_API(avcodec_find_decoder) VIDEO_API(avcodec_alloc_context3) VIDEO_API(avcodec_open2)
    VIDEO_API(avcodec_free_context) VIDEO_API(avcodec_flush_buffers) VIDEO_API(avcodec_version)
    VIDEO_API(av_parser_init) VIDEO_API(av_parser_close) VIDEO_API(av_parser_parse2)
    VIDEO_API(av_packet_alloc) VIDEO_API(av_packet_free) VIDEO_API(av_new_packet) VIDEO_API(av_packet_unref)
    VIDEO_API(avcodec_send_packet) VIDEO_API(avcodec_receive_frame)
    VIDEO_API(av_frame_alloc) VIDEO_API(av_frame_free) VIDEO_API(av_frame_unref)
    VIDEO_API(sws_getContext) VIDEO_API(sws_freeContext) VIDEO_API(sws_scale)
#undef VIDEO_API
    AVCodecContext *context_{};
    AVCodecParserContext *parser_{};
    AVPacket *packet_{};
    AVFrame *frame_{};
    std::deque<VideoFrame> ready_;
    std::uint64_t source_{};
    bool draining_{};
    void destroy() noexcept {
        if (parser_ && av_parser_close_) av_parser_close_(parser_);
        if (packet_ && av_packet_free_) av_packet_free_(&packet_);
        if (frame_ && av_frame_free_) av_frame_free_(&frame_);
        if (context_ && avcodec_free_context_) avcodec_free_context_(&context_);
        parser_ = nullptr;
    }
    void receive() {
        for (unsigned count = 0; count < 16; ++count) {
            const auto result = avcodec_receive_frame_(context_, frame_);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            if (result < 0) throw psprecomp::Error("H.264 decoder rejected the submitted video access unit");
            if (frame_->width <= 0 || frame_->height <= 0 || frame_->width > 480 || frame_->height > 272)
                throw psprecomp::Error("Decoded video exceeds the supported PSP display dimensions");
            VideoFrame output{static_cast<std::uint32_t>(frame_->width), static_cast<std::uint32_t>(frame_->height),
                frame_->pts == AV_NOPTS_VALUE ? video_no_timestamp : frame_->pts, {}};
            output.rgba.resize(static_cast<std::size_t>(output.width) * output.height * 4);
            auto *scale = sws_getContext_(frame_->width, frame_->height, static_cast<AVPixelFormat>(frame_->format),
                frame_->width, frame_->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!scale) throw psprecomp::Error("Cannot initialize decoded video color conversion");
            std::uint8_t *destinations[4]{output.rgba.data(), nullptr, nullptr, nullptr};
            const int strides[4]{static_cast<int>(output.width * 4), 0, 0, 0};
            const auto rows = sws_scale_(scale, frame_->data, frame_->linesize, 0, frame_->height, destinations, strides);
            sws_freeContext_(scale);
            if (rows != frame_->height) throw psprecomp::Error("Incomplete decoded video color conversion");
            av_frame_unref_(frame_);
            if (ready_.size() >= 16) throw psprecomp::Error("Video decoder output queue exceeds its bound");
            ready_.push_back(std::move(output));
        }
        throw psprecomp::Error("Video decoder emitted too many frames for one access unit");
    }
    std::optional<VideoFrame> take() {
        if (ready_.empty()) return std::nullopt;
        auto frame = std::move(ready_.front()); ready_.pop_front(); return frame;
    }
public:
    explicit FfmpegVideo(const std::filesystem::path &directory) {
        try {
            const auto util = libraries_.open(directory, "avutil", LIBAVUTIL_VERSION_MAJOR);
            const auto codec = libraries_.open(directory, "avcodec", LIBAVCODEC_VERSION_MAJOR);
            const auto scale = libraries_.open(directory, "swscale", LIBSWSCALE_VERSION_MAJOR);
#define LOAD_CODEC(name) name##_ = libraries_.find<decltype(name##_)>(codec, #name);
            LOAD_CODEC(avcodec_find_decoder) LOAD_CODEC(avcodec_alloc_context3) LOAD_CODEC(avcodec_open2)
            LOAD_CODEC(avcodec_free_context) LOAD_CODEC(avcodec_flush_buffers) LOAD_CODEC(avcodec_version)
            LOAD_CODEC(av_parser_init) LOAD_CODEC(av_parser_close) LOAD_CODEC(av_parser_parse2)
            LOAD_CODEC(av_packet_alloc) LOAD_CODEC(av_packet_free) LOAD_CODEC(av_new_packet) LOAD_CODEC(av_packet_unref)
            LOAD_CODEC(avcodec_send_packet) LOAD_CODEC(avcodec_receive_frame)
#undef LOAD_CODEC
#define LOAD_UTIL(name) name##_ = libraries_.find<decltype(name##_)>(util, #name);
            LOAD_UTIL(av_frame_alloc) LOAD_UTIL(av_frame_free) LOAD_UTIL(av_frame_unref)
#undef LOAD_UTIL
#define LOAD_SCALE(name) name##_ = libraries_.find<decltype(name##_)>(scale, #name);
            LOAD_SCALE(sws_getContext) LOAD_SCALE(sws_freeContext) LOAD_SCALE(sws_scale)
#undef LOAD_SCALE
            if ((avcodec_version_() >> 16) != LIBAVCODEC_VERSION_MAJOR)
                throw psprecomp::Error("Video codec DLL/header major version mismatch");
            const auto *decoder = avcodec_find_decoder_(AV_CODEC_ID_H264);
            if (!decoder) throw psprecomp::Error("Installed codec has no H.264 decoder");
            context_ = avcodec_alloc_context3_(decoder);
            if (!context_) throw psprecomp::Error("Cannot allocate H.264 decoder context");
            context_->thread_count = 1;
            if (avcodec_open2_(context_, decoder, nullptr) < 0) throw psprecomp::Error("Cannot open H.264 decoder");
            parser_ = av_parser_init_(AV_CODEC_ID_H264);
            packet_ = av_packet_alloc_(); frame_ = av_frame_alloc_();
            if (!parser_ || !packet_ || !frame_) throw psprecomp::Error("Cannot allocate H.264 parser/frame state");
        } catch (...) { destroy(); throw; }
    }
    ~FfmpegVideo() override { destroy(); }
    std::vector<EncodedVideo> parse(std::span<const std::uint8_t> bytes, std::int64_t pts,
        std::int64_t dts, bool end) override {
        if (bytes.size() > 4u * 1024u * 1024u) throw psprecomp::Error("Video parser input exceeds its bound");
        std::vector<std::uint8_t> padded(bytes.begin(), bytes.end());
        padded.resize(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::vector<EncodedVideo> result;
        std::size_t position = 0;
        do {
            std::uint8_t *output = nullptr; int output_size = 0;
            const auto used = av_parser_parse2_(parser_, context_, &output, &output_size,
                bytes.empty() ? nullptr : padded.data() + position, static_cast<int>(bytes.size() - position),
                pts == video_no_timestamp ? AV_NOPTS_VALUE : pts, dts == video_no_timestamp ? AV_NOPTS_VALUE : dts,
                static_cast<std::int64_t>(source_));
            if (used < 0 || static_cast<std::size_t>(used) > bytes.size() - position || output_size < 0 || output_size > 4 * 1024 * 1024)
                throw psprecomp::Error("Invalid H.264 access-unit parser result");
            position += used; source_ += used;
            if (output_size) {
                result.push_back({{output, output + output_size},
                    parser_->pts == AV_NOPTS_VALUE ? video_no_timestamp : parser_->pts,
                    parser_->dts == AV_NOPTS_VALUE ? video_no_timestamp : parser_->dts, source_});
                if (result.size() > 1024) throw psprecomp::Error("Video parser access-unit count exceeds its bound");
            }
            if (!used && !output_size) break;
        } while (position < bytes.size());
        if (position != bytes.size()) throw psprecomp::Error("H.264 parser left unconsumed input");
        if (end && !bytes.empty()) {
            auto tail = parse({}, video_no_timestamp, video_no_timestamp, false);
            for (auto &unit : tail) result.push_back(std::move(unit));
        }
        return result;
    }
    std::optional<VideoFrame> decode(const EncodedVideo &unit) override {
        if (draining_ || unit.bytes.empty() || unit.bytes.size() > 4u * 1024u * 1024u)
            throw psprecomp::Error("Invalid video decoder submission state/size");
        if (av_new_packet_(packet_, static_cast<int>(unit.bytes.size())) < 0)
            throw psprecomp::Error("Cannot allocate encoded video packet");
        std::memcpy(packet_->data, unit.bytes.data(), unit.bytes.size());
        packet_->pts = unit.pts == video_no_timestamp ? AV_NOPTS_VALUE : unit.pts;
        packet_->dts = unit.dts == video_no_timestamp ? AV_NOPTS_VALUE : unit.dts;
        const auto sent = avcodec_send_packet_(context_, packet_); av_packet_unref_(packet_);
        if (sent < 0) throw psprecomp::Error("Cannot submit H.264 access unit to the decoder");
        receive(); return take();
    }
    std::optional<VideoFrame> drain() override {
        if (!draining_) {
            const auto result = avcodec_send_packet_(context_, nullptr);
            if (result < 0 && result != AVERROR_EOF) throw psprecomp::Error("Cannot drain H.264 decoder");
            draining_ = true;
        }
        receive(); return take();
    }
    void reset() override {
        avcodec_flush_buffers_(context_); av_parser_close_(parser_);
        parser_ = av_parser_init_(AV_CODEC_ID_H264);
        if (!parser_) throw psprecomp::Error("Cannot reset H.264 parser");
        ready_.clear(); source_ = 0; draining_ = false;
    }
};
// Independent public-API packet adapter. Codec algorithms remain in the
// separately installed FFmpeg shared libraries; no third-party decoder source.
class FfmpegMpegAudio final : public MpegAudioDecoder {
    FfmpegLibraries libraries_{"MPEG audio"};
#define AUDIO_API(name) decltype(&name) name##_{};
    AUDIO_API(avcodec_find_decoder) AUDIO_API(avcodec_alloc_context3) AUDIO_API(avcodec_open2)
    AUDIO_API(avcodec_free_context) AUDIO_API(avcodec_version)
    AUDIO_API(av_packet_alloc) AUDIO_API(av_packet_free) AUDIO_API(av_new_packet) AUDIO_API(av_packet_unref)
    AUDIO_API(avcodec_send_packet) AUDIO_API(avcodec_receive_frame)
    AUDIO_API(av_frame_alloc) AUDIO_API(av_frame_free) AUDIO_API(av_frame_unref) AUDIO_API(avutil_version)
    AUDIO_API(swr_alloc_set_opts2) AUDIO_API(swr_init) AUDIO_API(swr_free) AUDIO_API(swr_convert)
    AUDIO_API(swresample_version)
#undef AUDIO_API
    AVCodecContext *context_{};
    AVPacket *packet_{};
    AVFrame *frame_{};
    SwrContext *convert_{};
    std::size_t block_align_{};
    void clear() noexcept {
        if (convert_) swr_free_(&convert_);
        if (packet_) av_packet_free_(&packet_);
        if (frame_) av_frame_free_(&frame_);
        if (context_) avcodec_free_context_(&context_);
        block_align_ = 0;
    }
public:
    explicit FfmpegMpegAudio(const std::filesystem::path &directory) {
        const auto util = libraries_.open(directory, "avutil", LIBAVUTIL_VERSION_MAJOR);
        const auto codec = libraries_.open(directory, "avcodec", LIBAVCODEC_VERSION_MAJOR);
        const auto resample = libraries_.open(directory, "swresample", LIBSWRESAMPLE_VERSION_MAJOR);
#define LOAD_CODEC(name) name##_ = libraries_.find<decltype(name##_)>(codec, #name);
        LOAD_CODEC(avcodec_find_decoder) LOAD_CODEC(avcodec_alloc_context3) LOAD_CODEC(avcodec_open2)
        LOAD_CODEC(avcodec_free_context) LOAD_CODEC(avcodec_version)
        LOAD_CODEC(av_packet_alloc) LOAD_CODEC(av_packet_free) LOAD_CODEC(av_new_packet) LOAD_CODEC(av_packet_unref)
        LOAD_CODEC(avcodec_send_packet) LOAD_CODEC(avcodec_receive_frame)
#undef LOAD_CODEC
#define LOAD_UTIL(name) name##_ = libraries_.find<decltype(name##_)>(util, #name);
        LOAD_UTIL(av_frame_alloc) LOAD_UTIL(av_frame_free) LOAD_UTIL(av_frame_unref) LOAD_UTIL(avutil_version)
#undef LOAD_UTIL
#define LOAD_SWR(name) name##_ = libraries_.find<decltype(name##_)>(resample, #name);
        LOAD_SWR(swr_alloc_set_opts2) LOAD_SWR(swr_init) LOAD_SWR(swr_free) LOAD_SWR(swr_convert) LOAD_SWR(swresample_version)
#undef LOAD_SWR
        if ((avcodec_version_() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
            (avutil_version_() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
            (swresample_version_() >> 16) != LIBSWRESAMPLE_VERSION_MAJOR)
            throw psprecomp::Error("MPEG audio DLL/header major version mismatch");
        if (!avcodec_find_decoder_(AV_CODEC_ID_ATRAC3P))
            throw psprecomp::Error("Installed codec has no MPEG ATRAC3plus decoder");
    }
    ~FfmpegMpegAudio() override { clear(); }
    void reset() override { clear(); }
    void open(std::size_t block_align) {
        clear();
        try {
            const auto *codec = avcodec_find_decoder_(AV_CODEC_ID_ATRAC3P);
            if (!codec) throw psprecomp::Error("Installed codec has no MPEG ATRAC3plus decoder");
            context_ = avcodec_alloc_context3_(codec); packet_ = av_packet_alloc_(); frame_ = av_frame_alloc_();
            if (!context_ || !packet_ || !frame_) throw psprecomp::Error("Cannot allocate MPEG ATRAC decoder state");
            context_->sample_rate = 44100; context_->ch_layout = AV_CHANNEL_LAYOUT_STEREO; context_->thread_count = 1;
            context_->block_align = static_cast<int>(block_align); block_align_ = block_align;
            if (avcodec_open2_(context_, codec, nullptr) < 0) throw psprecomp::Error("Cannot open MPEG ATRAC3plus decoder");
        } catch (...) { clear(); throw; }
    }
    std::vector<std::int16_t> decode(std::span<const std::uint8_t> bytes) override {
        if (bytes.empty() || bytes.size() > 0x838 || (block_align_ && block_align_ != bytes.size()))
            throw psprecomp::Error("Invalid/changing MPEG ATRAC frame size");
        // The actual container frame establishes block alignment; opening with
        // an invented nominal size would hide a missing codec parameter.
        if (!context_) open(bytes.size());
        if (av_new_packet_(packet_, static_cast<int>(bytes.size())) < 0) throw psprecomp::Error("Cannot allocate MPEG ATRAC packet");
        std::memcpy(packet_->data, bytes.data(), bytes.size());
        const auto sent = avcodec_send_packet_(context_, packet_); av_packet_unref_(packet_);
        if (sent < 0 || avcodec_receive_frame_(context_, frame_) < 0)
            throw psprecomp::Error("MPEG ATRAC3plus packet decode failed");
        if (frame_->nb_samples != 2048 || frame_->sample_rate != 44100 ||
            frame_->ch_layout.nb_channels != 2 || frame_->format != AV_SAMPLE_FMT_FLTP)
            throw psprecomp::Error("Unexpected MPEG ATRAC decoded frame format/count");
        if (!convert_) {
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2_(&convert_, &stereo, AV_SAMPLE_FMT_S16, 44100,
                &frame_->ch_layout, AV_SAMPLE_FMT_FLTP, 44100, 0, nullptr) < 0 || !convert_ || swr_init_(convert_) < 0)
                throw psprecomp::Error("Cannot configure MPEG ATRAC PCM conversion");
        }
        std::vector<std::int16_t> pcm(4096);
        auto *destination = reinterpret_cast<std::uint8_t *>(pcm.data());
        const std::uint8_t *source[2]{frame_->extended_data[0], frame_->extended_data[1]};
        if (swr_convert_(convert_, &destination, 2048, source, 2048) != 2048)
            throw psprecomp::Error("MPEG ATRAC conversion buffered/lost PCM samples");
        av_frame_unref_(frame_);
        const auto extra = avcodec_receive_frame_(context_, frame_);
        if (extra != AVERROR(EAGAIN)) throw psprecomp::Error("MPEG ATRAC decoder emitted an unexpected extra/delayed frame");
        return pcm;
    }
};
}
#endif
std::unique_ptr<VideoDecoder> make_ffmpeg_video_decoder(const std::filesystem::path &directory) {
#if defined(BADMAN_FFMPEG_RUNTIME)
    return std::make_unique<FfmpegVideo>(directory);
#else
    (void)directory;
    throw psprecomp::Error("This build has no configured FFmpeg video adapter");
#endif
}
std::unique_ptr<MpegAudioDecoder> make_ffmpeg_mpeg_audio_decoder(const std::filesystem::path &directory) {
#if defined(BADMAN_FFMPEG_RUNTIME)
    return std::make_unique<FfmpegMpegAudio>(directory);
#else
    (void)directory;
    throw psprecomp::Error("This build has no configured FFmpeg MPEG audio adapter");
#endif
}
}
