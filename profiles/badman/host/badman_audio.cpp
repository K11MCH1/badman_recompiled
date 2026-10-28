#include "badman_audio.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <string>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xaudio2.h>
#elif defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif

namespace badman {
using psprecomp::Error;
Audio::Audio(std::unique_ptr<AudioTransport> transport) : transport_(std::move(transport)) {
    if (!transport_) throw Error("Audio requires a real or explicitly injected transport");
}
Audio::~Audio() {
    // COM apartment and voice ownership require destruction on the owner thread.
    if (owner_ != std::this_thread::get_id()) std::terminate();
    for (std::uint32_t i = 0; i < channels_.size(); ++i)
        if (channels_[i].reserved) transport_->close(i);
    // Channel PCM storage outlives close's callback/sample-read barrier.
}
void Audio::check_thread() const {
    if (owner_ != std::this_thread::get_id()) throw Error("Audio called from a non-owner thread");
}
void Audio::check_healthy() const {
    check_thread();
    if (faulted_) throw Error("Audio transport has failed; recreate the audio device");
}
std::int32_t Audio::validate_channel(std::int32_t channel) const {
    if (channel < 0 || channel >= static_cast<std::int32_t>(channel_count)) return invalid_channel;
    return channels_[channel].reserved ? 0 : not_reserved;
}
bool Audio::valid_length(std::uint32_t frames) { return frames >= 64 && frames <= 65472 && !(frames & 63u); }
std::int32_t Audio::reserve(std::int32_t requested, std::uint32_t frames, std::uint32_t format) {
    check_healthy();
    if (!valid_length(frames)) return invalid_size;
    if (format != 0 && format != 0x10) return invalid_format;
    if (requested == -1) {
        requested = 0;
        while (requested < static_cast<std::int32_t>(channel_count) && channels_[requested].reserved) ++requested;
        if (requested == channel_count) return no_channel;
    }
    if (requested < 0 || requested >= static_cast<std::int32_t>(channel_count)) return invalid_channel;
    auto &channel = channels_[requested];
    if (channel.reserved) return busy;
    transport_->open(requested); // Failure leaves the channel unreserved.
    channel.reserved = true; channel.frames = frames; channel.mono = format == 0x10;
    return requested;
}
std::int32_t Audio::release(std::int32_t channel) {
    check_healthy(); poll();
    if (const auto error = validate_channel(channel)) return error;
    auto &state = channels_[channel];
    if (!state.queue.empty()) return busy;
    transport_->close(channel); state.reserved = false; state.frames = 0; state.mono = false;
    return 0;
}
std::int32_t Audio::cancel_release(std::int32_t channel) {
    check_healthy();poll();
    if(const auto error=validate_channel(channel))return error;
    auto &state=channels_[channel];
    // Preserve negative completion evidence without unbounded ticket history.
    // Exhaustion is explicit and leaves the live voice/buffers untouched.
    if(cancelled_tickets_.size()+state.queue.size()>4096)
        throw Error("Audio cancelled-ticket history exceeds its bounded capacity");
    cancelled_tickets_.reserve(cancelled_tickets_.size()+state.queue.size());
    transport_->close(channel); // PCM remains owned until callbacks/reads stop.
    for(const auto &buffer:state.queue)cancelled_tickets_.push_back(buffer->ticket);
    state.queue.clear();state.reserved=false;state.frames=0;state.mono=false;
    return 0;
}
std::int32_t Audio::set_length(std::int32_t channel, std::uint32_t frames) {
    check_healthy();
    if (const auto error = validate_channel(channel)) return error;
    if (!valid_length(frames)) return invalid_size;
    channels_[channel].frames = frames; return 0;
}
Audio::Submission Audio::submit(const psprecomp::GuestMemory &memory, std::int32_t channel,
    std::uint32_t left_volume, std::uint32_t right_volume, std::uint32_t address) {
    check_healthy();
    if (const auto error = validate_channel(channel)) return {error,0};
    if (left_volume > 0x8000 || right_volume > 0x8000) return {invalid_volume,0};
    poll();
    auto &state = channels_[channel];
    if (state.queue.size() == queue_capacity) return {busy,0};
    const auto bytes = state.frames * (state.mono ? 2u : 4u);
    if ((address & 1u) || !memory.contains(address,bytes)) throw Error("Invalid audio PCM source buffer");
    if (next_ticket_ == std::numeric_limits<std::uint64_t>::max()) throw Error("Audio ticket counter exhausted");
    auto buffer = std::make_unique<Buffer>();
    buffer->frames = state.frames; buffer->ticket = next_ticket_;
    buffer->samples.resize(static_cast<std::size_t>(state.frames) * 2);
    const auto scale = [](std::uint16_t packed, std::uint32_t gain) {
        const auto sample = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(packed));
        // Independent packet gains, signed integer truncation toward zero.
        return static_cast<std::int16_t>(sample * static_cast<std::int32_t>(gain) / 32768);
    };
    for (std::uint32_t frame = 0; frame < state.frames; ++frame) {
        const auto at = address + frame * (state.mono ? 2u : 4u);
        const auto left = memory.load16(at), right = state.mono ? left : memory.load16(at + 2);
        buffer->samples[frame * 2] = scale(left,left_volume);
        buffer->samples[frame * 2 + 1] = scale(right,right_volume);
    }
    // Publish owned storage before submitting. Even a very fast completion
    // cannot outlive its PCM; no allocation follows a successful submit.
    state.queue.push_back(std::move(buffer));
    auto &queued = *state.queue.back();
    try { transport_->submit(channel,queued.ticket,queued.samples); }
    catch (...) { state.queue.pop_back(); faulted_ = true; throw; }
    ++statistics_.submitted_buffers;
    statistics_.submitted_frames += queued.frames;
    statistics_.nonzero_samples += static_cast<std::uint64_t>(std::count_if(queued.samples.begin(), queued.samples.end(),
        [](std::int16_t sample) { return sample != 0; }));
    return {0,next_ticket_++};
}
void Audio::poll() {
    check_healthy();
    try {
        const auto updates = transport_->poll();
        if (updates.size() > channel_count * queue_capacity)
            throw Error("Audio transport exceeded the bounded progress snapshot");
        std::vector<Buffer *> targets; targets.reserve(updates.size());
        for (const auto &update : updates) {
            Buffer *found = nullptr;
            for (auto &channel : channels_) for (auto &buffer : channel.queue)
                if (buffer->ticket == update.ticket) found = buffer.get();
            if (!found || std::find(targets.begin(),targets.end(),found) != targets.end() ||
                update.frames_played < found->played || update.frames_played > found->frames ||
                (found->done && !update.complete) || (update.complete && update.frames_played != found->frames))
                throw Error("Audio transport returned invalid ticket/progress/completion");
            targets.push_back(found);
        }
        for (std::size_t i = 0; i < updates.size(); ++i) {
            targets[i]->played = updates[i].frames_played; targets[i]->done = updates[i].complete;
        }
        for (auto &channel : channels_)
            while (!channel.queue.empty() && channel.queue.front()->done) {
                ++statistics_.completed_buffers;
                statistics_.completed_frames += channel.queue.front()->frames;
                channel.queue.pop_front();
            }
    } catch (...) { faulted_ = true; throw; }
}
std::int32_t Audio::rest_length(std::int32_t channel) {
    check_healthy();
    if (const auto error = validate_channel(channel)) return error;
    poll(); std::uint32_t rest = 0;
    for (const auto &buffer : channels_[channel].queue) rest += buffer->frames - buffer->played;
    return static_cast<std::int32_t>(rest);
}
bool Audio::completed(std::uint64_t ticket) const {
    check_healthy();
    if (!ticket || ticket >= next_ticket_) return false;
    if(std::find(cancelled_tickets_.begin(),cancelled_tickets_.end(),ticket)!=cancelled_tickets_.end())return false;
    for (const auto &channel : channels_) for (const auto &buffer : channel.queue)
        if (buffer->ticket == ticket) return false;
    return true;
}

#if defined(_WIN32)
namespace {
void check(HRESULT result, const char *operation) {
    if (FAILED(result)) throw Error(std::string("XAudio2 ") + operation + " failed: " + psprecomp::hex32(result));
}
class XAudioTransport final : public AudioTransport {
    struct Ticket { std::uint64_t id{}, begin{}, end{}; std::atomic<bool> done{}; };
    struct VoiceCallback final : IXAudio2VoiceCallback {
        std::atomic<HRESULT> fault{S_OK};
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void *) override {}
        void STDMETHODCALLTYPE OnBufferEnd(void *context) override { static_cast<Ticket *>(context)->done.store(true,std::memory_order_release); }
        void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
        void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT error) override { fault.store(error,std::memory_order_release); }
    };
    struct EngineCallback final : IXAudio2EngineCallback {
        std::atomic<HRESULT> fault{S_OK};
        void STDMETHODCALLTYPE OnProcessingPassStart() override {}
        void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override { fault.store(error,std::memory_order_release); }
    } engine_callback_;
    struct Channel {
        IXAudio2SourceVoice *voice{};
        VoiceCallback callback;
        std::deque<std::unique_ptr<Ticket>> tickets;
        std::uint64_t submitted{}, played{};
    };
    std::array<Channel,Audio::channel_count> channels_{};
    IXAudio2 *engine_{};
    IXAudio2MasteringVoice *master_{};
    bool com_initialized_{}, registered_{};
    DWORD owner_{GetCurrentThreadId()};
    void healthy() const {
        if (owner_ != GetCurrentThreadId()) throw Error("XAudio2 used from a non-owner thread");
        check(engine_callback_.fault.load(std::memory_order_acquire),"engine callback");
        for (const auto &channel : channels_) check(channel.callback.fault.load(std::memory_order_acquire),"voice callback");
    }
    void cleanup() noexcept {
        for (std::uint32_t i=0;i<channels_.size();++i) close(i);
        if (master_) { master_->DestroyVoice(); master_=nullptr; }
        if (engine_) {
            if (registered_) engine_->UnregisterForCallbacks(&engine_callback_);
            engine_->Release(); engine_=nullptr;
        }
        if (com_initialized_) { CoUninitialize(); com_initialized_=false; }
    }
public:
    XAudioTransport() {
        try {
            check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM initialization"); com_initialized_=true;
            check(XAudio2Create(&engine_,0,XAUDIO2_DEFAULT_PROCESSOR),"engine creation");
            check(engine_->RegisterForCallbacks(&engine_callback_),"callback registration"); registered_=true;
            check(engine_->CreateMasteringVoice(&master_),"playback device creation");
        } catch (...) { cleanup(); throw; }
    }
    ~XAudioTransport() override {
        if (owner_ != GetCurrentThreadId()) std::terminate();
        cleanup();
    }
    void open(std::uint32_t index) override {
        healthy();
        if (index >= channels_.size() || channels_[index].voice) throw Error("Invalid XAudio2 channel open");
        auto &channel=channels_[index];
        WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;
        format.nSamplesPerSec=44100;format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=176400;
        const auto created=engine_->CreateSourceVoice(&channel.voice,&format,0,XAUDIO2_DEFAULT_FREQ_RATIO,&channel.callback);
        if (FAILED(created)) { close(index);check(created,"source voice creation"); }
        const auto started=channel.voice->Start();
        if (FAILED(started)) { close(index); check(started,"voice start"); }
    }
    void close(std::uint32_t index) noexcept override {
        if (index >= channels_.size()) return;
        auto &channel=channels_[index];
        if (channel.voice) {
            // DestroyVoice itself stops the voice and is the documented barrier
            // after which no callback/sample read can touch either owner.
            channel.voice->DestroyVoice();channel.voice=nullptr;
        }
        channel.tickets.clear();channel.submitted=channel.played=0;
        channel.callback.fault.store(S_OK,std::memory_order_release);
    }
    void submit(std::uint32_t index,std::uint64_t ticket,std::span<const std::int16_t> samples) override {
        healthy();
        if (index >= channels_.size() || !channels_[index].voice || samples.empty() || (samples.size()&1u))
            throw Error("Invalid XAudio2 PCM submission");
        auto &channel=channels_[index];
        const auto frames=samples.size()/2;
        if (frames > 65472 || channel.tickets.size() >= Audio::queue_capacity ||
            channel.submitted > std::numeric_limits<std::uint64_t>::max()-frames)
            throw Error("XAudio2 submission capacity/range exceeded");
        auto context=std::make_unique<Ticket>();context->id=ticket;
        context->begin=channel.submitted;context->end=channel.submitted+frames;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=static_cast<UINT32>(samples.size_bytes());
        buffer.pAudioData=reinterpret_cast<const BYTE *>(samples.data());buffer.pContext=context.get();
        channel.tickets.push_back(std::move(context));
        const auto submitted=channel.voice->SubmitSourceBuffer(&buffer);
        if (FAILED(submitted)) { channel.tickets.pop_back();check(submitted,"buffer submission"); }
        channel.submitted+=frames;
    }
    std::vector<AudioProgress> poll() override {
        healthy();std::vector<AudioProgress> updates;
        for (auto &channel:channels_) if (channel.voice) {
            XAUDIO2_VOICE_STATE state{};channel.voice->GetState(&state);
            if (state.SamplesPlayed<channel.played || state.SamplesPlayed>channel.submitted)
                throw Error("XAudio2 sample cursor moved outside submitted PCM");
            channel.played=state.SamplesPlayed;
            std::size_t retired=0;bool completed_prefix=true;
            for (const auto &ticket:channel.tickets) {
                const bool done=ticket->done.load(std::memory_order_acquire);
                if (completed_prefix && done) ++retired; else completed_prefix=false;
                const auto frames=ticket->end-ticket->begin;
                const auto played=state.SamplesPlayed>ticket->begin?std::min(frames,state.SamplesPlayed-ticket->begin):0;
                updates.push_back({ticket->id,static_cast<std::uint32_t>(done?frames:played),done});
            }
            // Retire only completions reported in this snapshot. Re-reading
            // callback flags here could drop a completion that arrived after
            // its update was built, leaving the PCM owner waiting forever.
            while(retired--)channel.tickets.pop_front();
        }
        return updates;
    }
};
}
std::unique_ptr<AudioTransport> make_xaudio2_transport() { return std::make_unique<XAudioTransport>(); }
std::unique_ptr<AudioTransport> make_native_audio_transport() { return make_xaudio2_transport(); }
#elif defined(__ANDROID__)
namespace {
// One AAudio output stream per reserved PSP channel. The data callback only
// reads samples the owner published and only writes atomics, mirroring the
// XAudio2 contract: samples stay valid until completion or close returns.
class AAudioTransport final : public AudioTransport {
    static constexpr std::size_t ring = 4; // > Audio::queue_capacity
    struct Slot {
        const std::int16_t *samples{};
        std::uint64_t ticket{};
        std::uint32_t frames{};
        std::atomic<std::uint32_t> played{};
        std::atomic<bool> done{};
    };
    struct Channel {
        AAudioStream *stream{};
        std::array<Slot, ring> slots;
        std::atomic<std::uint64_t> published{}, consumed{}; // Slot sequence numbers.
        std::uint64_t retired{};
        std::atomic<aaudio_result_t> fault{AAUDIO_OK};
    };
    std::array<Channel, Audio::channel_count> channels_{};
    std::thread::id owner_{std::this_thread::get_id()};

    static void check(aaudio_result_t result, const char *operation) {
        if (result < 0) throw Error(std::string("AAudio ") + operation + " failed: " + AAudio_convertResultToText(result));
    }
    void healthy() const {
        if (owner_ != std::this_thread::get_id()) throw Error("AAudio used from a non-owner thread");
        for (const auto &channel : channels_) check(channel.fault.load(std::memory_order_acquire), "stream");
    }
    static aaudio_data_callback_result_t render(AAudioStream *, void *user, void *audio, int32_t frames) {
        auto &channel = *static_cast<Channel *>(user);
        auto *out = static_cast<std::int16_t *>(audio);
        std::int32_t written = 0;
        while (written < frames) {
            const auto index = channel.consumed.load(std::memory_order_relaxed);
            if (index == channel.published.load(std::memory_order_acquire)) break; // Starved: silence below.
            auto &slot = channel.slots[index % ring];
            const auto played = slot.played.load(std::memory_order_relaxed);
            const auto count = std::min<std::uint32_t>(slot.frames - played, static_cast<std::uint32_t>(frames - written));
            std::copy_n(slot.samples + static_cast<std::size_t>(played) * 2, static_cast<std::size_t>(count) * 2,
                        out + static_cast<std::size_t>(written) * 2);
            written += static_cast<std::int32_t>(count);
            slot.played.store(played + count, std::memory_order_release);
            if (played + count == slot.frames) {
                slot.done.store(true, std::memory_order_release);
                channel.consumed.store(index + 1, std::memory_order_release);
            }
        }
        std::fill(out + static_cast<std::size_t>(written) * 2, out + static_cast<std::size_t>(frames) * 2, std::int16_t{});
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    static void error(AAudioStream *, void *user, aaudio_result_t result) {
        // Disconnects (for example headphones) surface as an explicit fault.
        static_cast<Channel *>(user)->fault.store(result < 0 ? result : AAUDIO_ERROR_INTERNAL, std::memory_order_release);
    }
public:
    ~AAudioTransport() override {
        if (owner_ != std::this_thread::get_id()) std::terminate();
        for (std::uint32_t i = 0; i < channels_.size(); ++i) close(i);
    }
    void open(std::uint32_t index) override {
        healthy();
        if (index >= channels_.size() || channels_[index].stream) throw Error("Invalid AAudio channel open");
        auto &channel = channels_[index];
        AAudioStreamBuilder *builder{};
        check(AAudio_createStreamBuilder(&builder), "stream builder creation");
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, 44100);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setDataCallback(builder, &render, &channel);
        AAudioStreamBuilder_setErrorCallback(builder, &error, &channel);
        const auto opened = AAudioStreamBuilder_openStream(builder, &channel.stream);
        AAudioStreamBuilder_delete(builder);
        if (opened < 0) { channel.stream = nullptr; check(opened, "playback stream open"); }
        if (AAudioStream_getSampleRate(channel.stream) != 44100 || AAudioStream_getChannelCount(channel.stream) != 2 ||
            AAudioStream_getFormat(channel.stream) != AAUDIO_FORMAT_PCM_I16) {
            close(index); throw Error("AAudio did not provide 44100 Hz stereo 16-bit output");
        }
        const auto started = AAudioStream_requestStart(channel.stream);
        if (started < 0) { close(index); check(started, "stream start"); }
    }
    void close(std::uint32_t index) noexcept override {
        if (index >= channels_.size()) return;
        auto &channel = channels_[index];
        // AAudioStream_close stops the stream and returns only after its
        // callback can no longer run, so no sample read outlives this call.
        if (channel.stream) { AAudioStream_close(channel.stream); channel.stream = nullptr; }
        for (auto &slot : channel.slots) { slot.samples = nullptr; slot.frames = 0; slot.played = 0; slot.done = false; }
        channel.published = 0; channel.consumed = 0; channel.retired = 0; channel.fault = AAUDIO_OK;
    }
    void submit(std::uint32_t index, std::uint64_t ticket, std::span<const std::int16_t> samples) override {
        healthy();
        if (index >= channels_.size() || !channels_[index].stream || samples.empty() || (samples.size() & 1u))
            throw Error("Invalid AAudio PCM submission");
        auto &channel = channels_[index];
        const auto sequence = channel.published.load(std::memory_order_relaxed);
        if (sequence - channel.retired >= Audio::queue_capacity || samples.size() / 2 > 65472)
            throw Error("AAudio submission capacity exceeded");
        auto &slot = channel.slots[sequence % ring];
        slot.samples = samples.data(); slot.ticket = ticket; slot.frames = static_cast<std::uint32_t>(samples.size() / 2);
        slot.played.store(0, std::memory_order_relaxed); slot.done.store(false, std::memory_order_relaxed);
        channel.published.store(sequence + 1, std::memory_order_release);
    }
    std::vector<AudioProgress> poll() override {
        healthy(); std::vector<AudioProgress> updates;
        for (auto &channel : channels_) if (channel.stream) {
            const auto published = channel.published.load(std::memory_order_relaxed);
            std::uint64_t retire = channel.retired; bool prefix = true;
            for (auto sequence = channel.retired; sequence < published; ++sequence) {
                const auto &slot = channel.slots[sequence % ring];
                const bool done = slot.done.load(std::memory_order_acquire);
                if (prefix && done) retire = sequence + 1; else prefix = false;
                updates.push_back({slot.ticket, done ? slot.frames : slot.played.load(std::memory_order_acquire), done});
            }
            // Retire only completions included in this snapshot (see XAudio2).
            channel.retired = retire;
        }
        return updates;
    }
};
}
std::unique_ptr<AudioTransport> make_native_audio_transport() { return std::make_unique<AAudioTransport>(); }
std::unique_ptr<AudioTransport> make_xaudio2_transport() { throw Error("Native XAudio2 requires Windows10 or later"); }
#else
std::unique_ptr<AudioTransport> make_xaudio2_transport() { throw Error("Native XAudio2 requires Windows10 or later"); }
std::unique_ptr<AudioTransport> make_native_audio_transport() { throw Error("This platform has no native audio transport"); }
#endif
}
