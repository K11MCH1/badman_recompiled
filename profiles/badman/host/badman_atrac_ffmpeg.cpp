#include "badman_atrac.hpp"
#include "psprecomp/common.hpp"
#include <cstring>
#include <string>
#include "badman_ffmpeg_loader.hpp"
#if defined(BADMAN_FFMPEG_RUNTIME)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}
#endif

namespace badman {
#if defined(BADMAN_FFMPEG_RUNTIME)
namespace {
// Independently written public-API adapter. Codec implementation remains in the
// separately distributed, pinned LGPL shared libraries; no FFmpeg source copied.
class FfmpegAtrac final : public AtracDecoder {
    FfmpegLibraries libraries_{"ATRAC"};
#define ATRAC_API(name) decltype(&name) name##_{};
    ATRAC_API(avcodec_find_decoder) ATRAC_API(avcodec_alloc_context3) ATRAC_API(avcodec_open2)
    ATRAC_API(avcodec_free_context) ATRAC_API(avcodec_version)
    ATRAC_API(av_packet_alloc) ATRAC_API(av_packet_free) ATRAC_API(av_new_packet) ATRAC_API(av_packet_unref)
    ATRAC_API(avcodec_send_packet) ATRAC_API(avcodec_receive_frame)
    ATRAC_API(av_frame_alloc) ATRAC_API(av_frame_free) ATRAC_API(av_frame_unref)
    ATRAC_API(av_mallocz) ATRAC_API(avutil_version)
    ATRAC_API(swr_alloc_set_opts2) ATRAC_API(swr_init) ATRAC_API(swr_free) ATRAC_API(swr_convert)
    ATRAC_API(swresample_version)
#undef ATRAC_API
    AVCodecContext *context_{};
    AVPacket *packet_{};
    AVFrame *frame_{};
    SwrContext *convert_{};
    void clear() noexcept {
        if (convert_) swr_free_(&convert_);
        if (packet_) av_packet_free_(&packet_);
        if (frame_) av_frame_free_(&frame_);
        if (context_) avcodec_free_context_(&context_);
    }
    void receive(std::vector<std::int16_t> &output, std::size_t expected) {
        for (unsigned count = 0; count < 4; ++count) {
            const auto error = avcodec_receive_frame_(context_, frame_);
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return;
            if (error < 0) throw psprecomp::Error("ATRAC3plus packet decode failed: " + std::to_string(error));
            if (frame_->nb_samples != Atrac::frame_samples || frame_->sample_rate != 44100 ||
                frame_->ch_layout.nb_channels != 2 || frame_->format != AV_SAMPLE_FMT_FLTP ||
                output.size() > expected || expected - output.size() < Atrac::frame_samples * 2)
                throw psprecomp::Error("Unexpected ATRAC3plus decoded frame format/count");
            if (!convert_) {
                const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2_(&convert_, &stereo, AV_SAMPLE_FMT_S16, 44100,
                    &frame_->ch_layout, AV_SAMPLE_FMT_FLTP, 44100, 0, nullptr) < 0 || !convert_ || swr_init_(convert_) < 0)
                    throw psprecomp::Error("Cannot configure ATRAC PCM conversion");
            }
            const auto old_size = output.size(); output.resize(old_size + Atrac::frame_samples * 2);
            std::uint8_t *destination = reinterpret_cast<std::uint8_t *>(output.data() + old_size);
            const std::uint8_t *input[2]{frame_->extended_data[0], frame_->extended_data[1]};
            if (swr_convert_(convert_, &destination, Atrac::frame_samples, input, Atrac::frame_samples) != Atrac::frame_samples)
                throw psprecomp::Error("ATRAC PCM conversion buffered or lost samples");
            av_frame_unref_(frame_);
        }
        throw psprecomp::Error("ATRAC3plus decoder exceeded per-packet output bound");
    }
public:
    explicit FfmpegAtrac(const std::filesystem::path &directory) {
        try {
            const auto util = libraries_.open(directory, "avutil", LIBAVUTIL_VERSION_MAJOR);
            const auto codec = libraries_.open(directory, "avcodec", LIBAVCODEC_VERSION_MAJOR);
            const auto resample = libraries_.open(directory, "swresample", LIBSWRESAMPLE_VERSION_MAJOR);
#define CODEC(name) name##_ = libraries_.find<decltype(name##_)>(codec, #name);
            CODEC(avcodec_find_decoder) CODEC(avcodec_alloc_context3) CODEC(avcodec_open2)
            CODEC(avcodec_free_context) CODEC(avcodec_version)
            CODEC(av_packet_alloc) CODEC(av_packet_free) CODEC(av_new_packet) CODEC(av_packet_unref)
            CODEC(avcodec_send_packet) CODEC(avcodec_receive_frame)
#undef CODEC
#define UTIL(name) name##_ = libraries_.find<decltype(name##_)>(util, #name);
            UTIL(av_frame_alloc) UTIL(av_frame_free) UTIL(av_frame_unref) UTIL(av_mallocz) UTIL(avutil_version)
#undef UTIL
#define SWR(name) name##_ = libraries_.find<decltype(name##_)>(resample, #name);
            SWR(swr_alloc_set_opts2) SWR(swr_init) SWR(swr_free) SWR(swr_convert) SWR(swresample_version)
#undef SWR
            if ((avcodec_version_() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
                (avutil_version_() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
                (swresample_version_() >> 16) != LIBSWRESAMPLE_VERSION_MAJOR)
                throw psprecomp::Error("ATRAC FFmpeg DLL/header major version mismatch");
            if (!avcodec_find_decoder_(AV_CODEC_ID_ATRAC3P))
                throw psprecomp::Error("Installed FFmpeg lacks ATRAC3plus decoding");
        } catch (...) { libraries_.close_all(); throw; }
    }
    ~FfmpegAtrac() override { clear(); libraries_.close_all(); }
    std::vector<std::int16_t> decode(const AtracRiff &riff, std::span<const std::uint8_t> data) override {
        clear();
        if (!riff.block_align || data.size() != riff.data_size || data.size() > Atrac::max_file_bytes ||
            data.size() % riff.block_align || riff.extra.size() != 12)
            throw psprecomp::Error("Invalid bounded ATRAC3plus decoder input");
        const auto expected = data.size() / riff.block_align * Atrac::frame_samples * 2;
        if (!expected || expected > Atrac::max_pcm_bytes / sizeof(std::int16_t))
            throw psprecomp::Error("ATRAC3plus decoded stream exceeds its bound");
        try {
            const auto *decoder = avcodec_find_decoder_(AV_CODEC_ID_ATRAC3P);
            if (!decoder) throw psprecomp::Error("Installed FFmpeg lacks ATRAC3plus decoding");
            context_ = avcodec_alloc_context3_(decoder);
            packet_ = av_packet_alloc_(); frame_ = av_frame_alloc_();
            if (!context_ || !packet_ || !frame_) throw psprecomp::Error("Cannot allocate ATRAC decoder state");
            context_->sample_rate = 44100; context_->block_align = static_cast<int>(riff.block_align);
            context_->ch_layout = AV_CHANNEL_LAYOUT_STEREO; context_->thread_count = 1;
            context_->extradata_size = static_cast<int>(riff.extra.size());
            context_->extradata = static_cast<std::uint8_t *>(av_mallocz_(riff.extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!context_->extradata) throw psprecomp::Error("Cannot allocate ATRAC codec parameters");
            std::memcpy(context_->extradata, riff.extra.data(), riff.extra.size());
            if (avcodec_open2_(context_, decoder, nullptr) < 0) throw psprecomp::Error("Cannot open ATRAC3plus decoder");
            std::vector<std::int16_t> result; result.reserve(expected);
            for (std::size_t at = 0; at < data.size(); at += riff.block_align) {
                if (av_new_packet_(packet_, static_cast<int>(riff.block_align)) < 0)
                    throw psprecomp::Error("Cannot allocate encoded ATRAC packet");
                std::memcpy(packet_->data, data.data() + at, riff.block_align);
                const auto sent = avcodec_send_packet_(context_, packet_); av_packet_unref_(packet_);
                if (sent < 0) throw psprecomp::Error("Cannot submit ATRAC3plus packet: " + std::to_string(sent));
                receive(result, expected);
            }
            if (avcodec_send_packet_(context_, nullptr) < 0) throw psprecomp::Error("Cannot drain ATRAC3plus decoder");
            receive(result, expected);
            std::int16_t tail[2]{}; auto *tail_bytes = reinterpret_cast<std::uint8_t *>(tail);
            if (!convert_ || swr_convert_(convert_, &tail_bytes, 1, nullptr, 0) != 0 || result.size() != expected)
                throw psprecomp::Error("ATRAC3plus decode/conversion has missing or delayed samples");
            clear(); return result;
        } catch (...) { clear(); throw; }
    }
};
}
#endif
std::unique_ptr<AtracDecoder> make_ffmpeg_atrac_decoder(const std::filesystem::path &directory) {
#if defined(BADMAN_FFMPEG_RUNTIME)
    return std::make_unique<FfmpegAtrac>(directory);
#else
    (void)directory;
    throw psprecomp::Error("This build has no configured FFmpeg ATRAC3plus adapter");
#endif
}
}
