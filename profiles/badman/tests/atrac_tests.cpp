#include "badman_atrac.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using badman::Atrac;
void require(bool test, const char *message) { if (!test) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char *message) {
    bool rejected = false; try { operation(); } catch (const std::exception &) { rejected = true; }
    require(rejected, message);
}
void put16(std::vector<std::uint8_t> &data, std::size_t at, std::uint16_t value) {
    data[at] = static_cast<std::uint8_t>(value); data[at + 1] = static_cast<std::uint8_t>(value >> 8);
}
void put32(std::vector<std::uint8_t> &data, std::size_t at, std::uint32_t value) {
    put16(data, at, static_cast<std::uint16_t>(value)); put16(data, at + 2, static_cast<std::uint16_t>(value >> 16));
}
void chunk(std::vector<std::uint8_t> &data, const char *tag, const std::vector<std::uint8_t> &body) {
    const auto at = data.size(); data.resize(at + 8 + body.size() + (body.size() & 1));
    std::memcpy(data.data() + at, tag, 4); put32(data, at + 4, static_cast<std::uint32_t>(body.size()));
    std::copy(body.begin(), body.end(), data.begin() + at + 8);
}
// Four synthetic packets. They are never passed to a real codec or copied from a game.
std::vector<std::uint8_t> fixture(bool loop = false, std::uint32_t samples = 5000,
    std::uint32_t offset = 2048) {
    std::vector<std::uint8_t> data(12); std::memcpy(data.data(), "RIFF", 4); std::memcpy(data.data() + 8, "WAVE", 4);
    std::vector<std::uint8_t> format(52); put16(format, 0, 0xFFFE); put16(format, 2, 2);
    put32(format, 4, 44100); put32(format, 8, 172); put16(format, 12, 8); put16(format, 16, 34);
    put16(format, 18, 2048); put32(format, 20, 3);
    const std::array<std::uint8_t,16> guid{0xBF,0xAA,0x23,0xE9,0x58,0xCB,0x71,0x44,0xA1,0x19,0xFF,0xFA,0x01,0xE4,0xCE,0x62};
    std::copy(guid.begin(), guid.end(), format.begin() + 24); chunk(data, "fmt ", format);
    std::vector<std::uint8_t> fact(8); put32(fact, 0, samples); put32(fact, 4, offset); chunk(data, "fact", fact);
    chunk(data, "JUNK", {1,2,3}); // RIFF odd-byte padding must advance correctly.
    if (loop) {
        std::vector<std::uint8_t> smpl(60); put32(smpl, 28, 1); put32(smpl, 32, 24);
        put32(smpl, 44, offset + 1000); put32(smpl, 48, offset + 2999); chunk(data, "smpl", smpl);
    }
    std::vector<std::uint8_t> coded(32); for (unsigned i = 0; i < 4; ++i) coded[i * 8] = static_cast<std::uint8_t>(i);
    chunk(data, "data", coded); put32(data, 4, static_cast<std::uint32_t>(data.size() - 8)); return data;
}
std::int16_t sample(std::uint32_t frame, unsigned channel) {
    return static_cast<std::int16_t>((frame * 3 + channel * 17001) % 65536 - 32768);
}
struct Observation { unsigned calls{}, created{}, destroyed{}; bool fail{}, short_pcm{}; };
class Decoder final : public badman::AtracDecoder {
    std::shared_ptr<Observation> state_;
public:
    explicit Decoder(std::shared_ptr<Observation> state) : state_(std::move(state)) { ++state_->created; }
    ~Decoder() override { ++state_->destroyed; }
    std::vector<std::int16_t> decode(const badman::AtracRiff &info, std::span<const std::uint8_t> encoded) override {
        ++state_->calls; if (state_->fail) throw psprecomp::Error("Injected codec failure");
        require(info.block_align == 8 && encoded.size() == 32 && encoded[8] == 1, "Wrong packet bytes/extent supplied to decoder");
        std::vector<std::int16_t> result(8192 * 2);
        for (unsigned i = 0; i < 8192; ++i) for (unsigned c = 0; c < 2; ++c) result[i * 2 + c] = sample(i, c);
        if (state_->short_pcm) result.pop_back(); return result;
    }
};
Atrac service(std::shared_ptr<Observation> state) { return Atrac([state] { return std::make_unique<Decoder>(state); }); }
void backend_preparation_lifetime_and_failures() {
    Atrac absent; rejects([&]{absent.prepare_decoder();}, "Missing factory falsely became ready");
    unsigned attempts = 0;
    Atrac pending([&]() -> std::unique_ptr<badman::AtracDecoder> {
        ++attempts;
        if (attempts == 1) throw psprecomp::Error("Injected backend-load failure");
        return {};
    });
    rejects([&]{pending.prepare_decoder();}, "Backend-load failure became ready");
    rejects([&]{pending.prepare_decoder();}, "Null backend became ready");
    require(attempts == 2, "Failed preparation published readiness or skipped retry");
    auto state = std::make_shared<Observation>(); auto atrac = service(state);
    atrac.prepare_decoder(); atrac.prepare_decoder();
    require(state->created == 1 && state->destroyed == 0 && state->calls == 0,
        "Readiness allocated/replaced a decoder or decoded fabricated PCM");
    std::uint32_t maximum = 123;
    require(atrac.get_max_sample(0, maximum) == Atrac::bad_id && maximum == 123,
        "Backend preparation allocated a guest ID");
    const auto id = atrac.set_data_and_get_id(fixture());
    require(id == 0 && state->created == 1 && state->calls == 1,
        "First resident decode failed to reuse readiness backend or consumed an ID");
    rejects([&]{atrac.set_decoder_factory({});}, "Active ID allowed readiness factory replacement");
    require(state->destroyed == 0 && atrac.release(id) == 0, "Rejected factory replacement destroyed active readiness");
    const auto next = atrac.set_data_and_get_id(fixture());
    require(next == 0 && state->created == 1 && state->calls == 2 && atrac.release(next) == 0,
        "Sequential resident RIFFs did not reuse the retained backend");
    atrac.set_decoder_factory({});
    require(state->destroyed == 1, "Inactive factory replacement retained old DLL/backend lifetime");
    rejects([&]{atrac.prepare_decoder();}, "Removed factory retained stale readiness");
}
void resident_and_errors() {
    auto state = std::make_shared<Observation>(); auto atrac = service(state); auto data = fixture();
    const auto info = badman::AtracRiff::parse(data);
    require(info.samples == 5000 && info.skip_samples == 2416 && info.data_size == 32 && info.loop_start == -1,
        "Incorrect fact/skip/data metadata");
    const auto id = atrac.set_data_and_get_id(data); require(id == 0, "First ID must be zero");
    std::uint32_t next = 123;
    require(atrac.next_sample_count(id, next) == 0 && next == 1680, "Read-only decode preflight changed first count");
    rejects([&]{atrac.set_decoder_factory({});}, "Replacing an active decoder factory accepted");
    std::fill(data.begin(), data.end(), 0xFF); // Lifetime is independent of guest/source storage.
    std::uint32_t maximum = 123; require(atrac.get_max_sample(id, maximum) == 0 && maximum == 2048, "Wrong maximum frame size");
    Atrac::SoundSample sound{}; require(atrac.get_sound_sample(id, sound) == 0 && sound.end == 4999 &&
        sound.loop_start == -1 && sound.loop_end == -1, "End/absent-loop coordinates incorrect");
    Atrac::SecondBuffer second{123,456}; require(atrac.get_second_buffer_info(id, second) == Atrac::no_second_buffer &&
        second.position == 0 && second.bytes == 0, "Resident no-second-buffer error must zero outputs");
    second = {123,456}; require(atrac.get_second_buffer_info(2, second) == Atrac::bad_id && second.position == 123 && second.bytes == 456,
        "Invalid ID changed second-buffer outputs");
    require(atrac.set_loop_num(id, 0) == Atrac::no_loop && atrac.set_loop_num(id, -1) == Atrac::no_loop,
        "Loop settings fabricated absent metadata");
    std::array<std::int16_t,4098> pcm{}; Atrac::DecodeResult result{123,456,789};
    rejects([&]{atrac.decode(id, std::span(pcm).first(2), result);}, "Short output accepted");
    require(result.samples == 123 && result.ended == 456 && result.remaining == 789, "Short output changed result");
    std::uint32_t total = 0, calls = 0;
    for (;;) {
        pcm.fill(12345); const auto status = atrac.decode(id, std::span(pcm).first(4096), result);
        if (status == Atrac::all_decoded) break;
        require(status == 0 && result.samples && result.samples <= 2048 && result.remaining == -1, "Bad decode status/count");
        if (!calls) require(result.samples == 1680, "First frame omitted codec priming trim");
        for (unsigned i = 0; i < result.samples; ++i) for (unsigned c = 0; c < 2; ++c)
            require(pcm[i * 2 + c] == sample(2416 + total + i, c), "Decoded sample position/channel changed");
        require(pcm[result.samples * 2] == 12345 && pcm[4097] == 12345, "PCM write exceeded returned sample count");
        total += result.samples; ++calls; require(result.ended == (total == 5000), "End flag asserted at wrong frame");
    }
    require(total == 5000 && calls == 3 && result.samples == 1272 && result.ended == 1, "Wrong fact/trailing trim");
    result = {12,34,56}; require(atrac.decode(id, pcm, result) == Atrac::all_decoded && result.samples == 12 &&
        result.ended == 34 && result.remaining == 56, "End-of-stream error changed output metadata");
    std::int32_t remain = 99; require(atrac.get_remain_frame(id, remain) == 0 && remain == -1, "Drained resident remain must stay -1");
    remain = 99; require(atrac.get_remain_frame(-1, remain) == Atrac::bad_id && remain == 99, "Invalid ID changed remaining-frame output");
    sound = {12,34,56}; require(atrac.get_sound_sample(2, sound) == Atrac::bad_id && sound.end == 12 &&
        sound.loop_start == 34 && sound.loop_end == 56, "Invalid ID changed sound-sample outputs");
    result = {12,34,56}; require(atrac.decode(-1, pcm, result) == Atrac::bad_id && result.samples == 12 && result.ended == 34,
        "Invalid ID changed decode outputs");
    require(atrac.release(id) == 0 && atrac.release(id) == Atrac::bad_id, "Release lifecycle incorrect");
    maximum = 123; require(atrac.get_max_sample(id, maximum) == Atrac::bad_id && maximum == 123, "Released ID changed max output");
    next = 123; require(atrac.next_sample_count(id, next) == Atrac::bad_id && next == 123, "Invalid preflight changed count output");
    atrac.set_decoder_factory({});
}
void loops_and_discard() {
    auto state = std::make_shared<Observation>(); auto atrac = service(state); const auto id = atrac.set_data_and_get_id(fixture(true));
    Atrac::SoundSample sound{}; require(atrac.get_sound_sample(id, sound) == 0 && sound.loop_start == 1000 && sound.loop_end == 2999,
        "smpl points were not converted by fact offset");
    require(atrac.set_loop_num(id, 1) == 0, "Finite loop rejected");
    std::array<std::int16_t,4096> pcm{}; Atrac::DecodeResult result{}; unsigned total = 0;
    for (;;) {
        const auto status = atrac.decode(id, pcm, result); if (status == Atrac::all_decoded) break;
        require(status == 0 && total < 10000, "Loop did not terminate");
        const auto logical = total < 3000 ? total : total - 2000;
        for (unsigned i = 0; i < result.samples; ++i)
            require(pcm[i * 2] == sample(2416 + logical + i, 0), "Loop PCM region/transition incorrect");
        total += result.samples;
    }
    require(total == 7000 && result.ended == 1, "Finite loop count omitted tail or replayed wrong length");
    require(atrac.release(id) == 0, "Release loop failed");
    require(atrac.set_data_and_get_id(fixture(true)) == id && atrac.set_loop_num(id, -1) == 0, "Infinite loop setup failed");
    for (unsigned i = 0; i < 15; ++i) { require(atrac.decode(id, {}, result) == 0 && !result.ended,
        "Discard failed or infinite loop ended"); }
    require(atrac.set_loop_num(id, 0) == 0, "Disabling loop failed");
    unsigned limit = 0; while (atrac.decode(id, {}, result) == 0) require(++limit < 6, "Loop disable did not reach tail");
    require(result.ended == 1, "Disabled loop never ended");
}
void failures_and_bounds() {
    auto state = std::make_shared<Observation>(); auto atrac = service(state); auto data = fixture();
    require(atrac.set_data_and_get_id(data) == 0 && atrac.set_data_and_get_id(data) == 1 &&
        atrac.set_data_and_get_id(data) == Atrac::no_id && state->calls == 2, "ID bound consumed extra codec contexts");
    require(atrac.release(0) == 0 && atrac.set_data_and_get_id(data) == 0, "Released slot not reusable");
    auto healthy = service(state); state->fail = true;
    rejects([&]{healthy.set_data_and_get_id(data);}, "Codec failure hidden as success"); state->fail = false;
    state->short_pcm = true; rejects([&]{healthy.set_data_and_get_id(data);}, "Incomplete decoded PCM accepted"); state->short_pcm = false;
    require(healthy.set_data_and_get_id(data) == 0, "Failed initialization leaked ID");
    Atrac absent; rejects([&]{absent.set_data_and_get_id(data);}, "Missing decoder became a silent substitute");
    Atrac null([]{return std::unique_ptr<badman::AtracDecoder>{};}); rejects([&]{null.set_data_and_get_id(data);}, "Null decoder accepted");
    auto parser = service(state);
    const auto invalid = [&](std::vector<std::uint8_t> bytes, const char *message) {
        require(parser.set_data_and_get_id(bytes) < 0, message);
    };
    invalid({}, "Empty RIFF accepted"); auto bad = data; bad.pop_back(); invalid(bad, "Truncated resident RIFF accepted");
    bad = data; put32(bad, 4, 0xFFFFFFFF); invalid(bad, "RIFF size addition overflow accepted");
    bad = data; put32(bad, 16, 0xFFFFFFFF); invalid(bad, "Chunk size overflow accepted");
    bad = data; bad[44] ^= 1; invalid(bad, "Unknown codec GUID accepted");
    bad = data; put16(bad, 22, 1); invalid(bad, "Mono format silently accepted");
    bad = data; put16(bad, 32, 0); invalid(bad, "Zero block alignment accepted");
    bad = data; put32(bad, 80, 0xFFFFFFFF); invalid(bad, "Fact sample overflow accepted");
    bad = data; put32(bad, 84, 0xFFFFFFFF); invalid(bad, "Priming offset overflow accepted");
    invalid(fixture(false, 6000), "Fact length beyond raw PCM accepted");
    invalid(fixture(false, 0), "Zero fact length accepted");
    bad = fixture(true); // Locate by parsed payload: smpl precedes the final data chunk.
    const auto smpl_body = badman::AtracRiff::parse(bad).data_offset - 68;
    put32(bad, smpl_body + 44, 100); invalid(bad, "Negative logical loop start accepted");
    bad = fixture(true); put32(bad, smpl_body + 48, 999999); invalid(bad, "Loop beyond fact end accepted");
    data.insert(data.end(), 8, 0xEE); require(parser.set_data_and_get_id(data) == 0, "Larger resident allocation rejected valid RIFF extent");
    // All failures above must reject before a decoder sees untrusted extents.
    require(parser.release(0) == 0, "Parser success release failed");
    bad = fixture(); put32(bad, 4, static_cast<std::uint32_t>(bad.size())); bad.resize(bad.size()+8);
    std::memcpy(bad.data()+bad.size()-8, "fact", 4); invalid(bad, "Duplicate fact accepted");
}
void hardware_header_boundaries() {
    // Independent hardware-test observations: zero offset behaves like0x800;
    // offset0x400 changes the first output from1680 to656 samples.
    for (const auto offset : {0u, 2048u, 1024u}) {
        auto state = std::make_shared<Observation>(); auto atrac = service(state);
        const auto id = atrac.set_data_and_get_id(fixture(false, 5000, offset));
        require(id == 0, "Hardware header offset fixture rejected");
        Atrac::DecodeResult result{}; std::array<std::int16_t,4096> pcm{};
        require(atrac.decode(id, pcm, result) == 0 && result.samples == (offset == 1024 ? 656 : 1680),
            "PSP first-frame offset semantics differ from hardware observation");
        const auto skip = offset == 1024 ? 1392u : 2416u;
        require(pcm[0] == sample(skip, 0), "Wrong priming samples skipped");
        const auto limit = 8192 - skip;
        auto other = service(state);
        require(other.set_data_and_get_id(fixture(false, limit, offset)) == 0 &&
            other.set_data_and_get_id(fixture(false, limit + 1, offset)) == Atrac::bad_data,
            "PSP header end-sample limit not enforced");
    }
}
}
int main() {
    try { backend_preparation_lifetime_and_failures(); resident_and_errors(); loops_and_discard(); failures_and_bounds(); hardware_header_boundaries();
        std::cout << "Resident ATRAC3plus parsing/state/PCM tests passed; synthetic injected decoder, no playback device.\n"; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
