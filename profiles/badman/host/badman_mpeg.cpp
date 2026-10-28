#include "badman_mpeg.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace badman {
namespace {
using psprecomp::Error;
constexpr std::uint32_t invalid = 0x80610022u, bad_handle = 0x80618009u, need_data = 0x80618001u;
std::uint32_t big32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw Error("Truncated PSMF field");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
        (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}
void buffer(const psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t bytes) {
    if ((address & 3u) || !memory.contains(address, bytes)) throw Error("Invalid MPEG guest buffer");
}
std::int64_t timestamp(std::span<const std::uint8_t> value) {
    if (value.size() < 5 || !(value[0] & 1) || !(value[2] & 1) || !(value[4] & 1))
        throw Error("Invalid MPEG PES timestamp marker bits");
    return (std::int64_t((value[0] >> 1) & 7) << 30) | (std::int64_t(value[1]) << 22) |
        (std::int64_t(value[2] >> 1) << 15) | (std::int64_t(value[3]) << 7) | (value[4] >> 1);
}
void store_time(psprecomp::GuestMemory &memory, std::uint32_t address, std::int64_t value) {
    const auto bits = value == video_no_timestamp ? UINT64_MAX : static_cast<std::uint64_t>(value);
    memory.store32(address, static_cast<std::uint32_t>(bits >> 32)); memory.store32(address + 4, static_cast<std::uint32_t>(bits));
}
}
PsmfHeader PsmfHeader::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 2048 || std::memcmp(bytes.data(), "PSMF0015", 8) != 0)
        throw Error("Only complete PSMF0015 headers are supported");
    PsmfHeader result{big32(bytes, 8), big32(bytes, 12), 0, 0,
        (std::uint32_t(bytes[0x80]) << 8) | bytes[0x81]};
    if (result.stream_offset != 2048 || !result.stream_size || (result.stream_size & 2047u) ||
        result.stream_size > 64u * 1024u * 1024u || result.streams < 1 || result.streams > 2 || bytes[0x82] != 0xE0)
        throw Error("Unsupported PSMF stream extent/table");
    result.width = std::uint32_t(bytes[0x8E]) * 16; result.height = std::uint32_t(bytes[0x8F]) * 16;
    if (bytes[0x83] != 0 || (result.streams == 2 &&
        (bytes[0x92] != 0xBD || bytes[0x93] != 0 || bytes[0xA0] != 2 || bytes[0xA1] != 2)))
        throw Error("Only PSMF AVC0 and optional stereo44100 ATRAC0 are supported");
    for (unsigned i = 0; i < 6; ++i) {
        result.start_pts = (result.start_pts << 8) | bytes[0x54 + i];
        result.end_pts = (result.end_pts << 8) | bytes[0x5A + i];
    }
    if (!result.width || result.width > 480 || !result.height || result.height > 272)
        throw Error("Unsupported PSMF video dimensions");
    return result;
}
std::vector<VideoPayload> ProgramStream::feed(std::span<const std::uint8_t> bytes, bool end) {
    if (bytes.size() > 4u * 1024u * 1024u || pending_.size() + bytes.size() > 4u * 1024u * 1024u)
        throw Error("MPEG program-stream parser buffer bound exceeded");
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    std::vector<VideoPayload> result;
    std::size_t position = 0;
    while (pending_.size() - position >= 6) {
        const auto packet = std::span<const std::uint8_t>(pending_).subspan(position);
        if (packet[0] || packet[1] || packet[2] != 1) throw Error("Invalid MPEG program-stream start code");
        const auto id = packet[3];
        std::size_t size = 0;
        if (id == 0xBA) {
            if (packet.size() < 14) break;
            if ((packet[4] & 0xC0) != 0x40) throw Error("Only MPEG-2 program-stream packs are supported");
            size = 14 + (packet[13] & 7);
        } else if (id == 0xB9) size = 4;
        else {
            if (id < 0xBB) throw Error("Unexpected elementary start code in MPEG program stream");
            size = 6 + (std::uint32_t(packet[4]) << 8) + packet[5];
            if (size == 6) throw Error("Unbounded MPEG PES packets are not supported");
        }
        if (packet.size() < size) break;
        if (id >= 0xC0 && id <= 0xDF) throw Error("MPEG PCM/MPEG-audio streams are not supported");
        if (id == 0xBD || (id >= 0xE0 && id <= 0xEF)) {
            if ((id != 0xBD && id != 0xE0) || size < 9 || (packet[6] & 0xC0) != 0x80 || std::size_t(9 + packet[8]) > size)
                throw Error("Unsupported MPEG PES header");
            const auto flags = packet[7] >> 6;
            auto pts = video_no_timestamp, dts = video_no_timestamp;
            if (flags == 1 || (flags == 2 && packet[8] < 5) || (flags == 3 && packet[8] < 10))
                throw Error("Truncated MPEG PES timestamp header");
            if ((flags >= 2 && (packet[9] & 0xF0) != (flags == 3 ? 0x30 : 0x20)) ||
                (flags == 3 && (packet[14] & 0xF0) != 0x10))
                throw Error("Invalid MPEG PES timestamp prefix");
            if (flags >= 2) pts = timestamp(packet.subspan(9, 5));
            if (flags == 3) dts = timestamp(packet.subspan(14, 5));
            auto first = packet.begin() + 9 + packet[8];
            std::uint32_t first_frame = 0;
            if (id == 0xBD) {
                if (packet.begin() + size - first < 4 || first[0] || first[1])
                    throw Error("Unsupported PSMF private audio channel/header");
                first_frame = (std::uint32_t(first[2]) << 8) | first[3]; first += 4;
                if (first_frame >= static_cast<std::size_t>(packet.begin() + size - first))
                    throw Error("PSMF audio first-frame offset is outside its PES");
            }
            result.push_back({{first, packet.begin() + size}, pts, dts, consumed_ + position + size, id == 0xBD, first_frame});
            if (result.size() > 4096) throw Error("MPEG parser packet count bound exceeded");
        }
        position += size;
    }
    if (end && position != pending_.size()) {
        if (pending_.size() - position == 4 && pending_[position] == 0 && pending_[position + 1] == 0 &&
            pending_[position + 2] == 1 && pending_[position + 3] == 0xB9) position += 4;
        else throw Error("Truncated MPEG program stream at its declared end");
    }
    pending_.erase(pending_.begin(), pending_.begin() + position); consumed_ += position;
    return result;
}
void ProgramStream::reset() { pending_.clear(); consumed_ = 0; }
void Mpeg::set_decoder_factory(DecoderFactory factory) {
    if (initialized_ || !contexts_.empty()) throw Error("Cannot replace an active MPEG decoder factory");
    factory_ = std::move(factory);
}
void Mpeg::set_audio_decoder_factory(AudioDecoderFactory factory) {
    if (initialized_ || !contexts_.empty()) throw Error("Cannot replace an active MPEG audio decoder factory");
    audio_factory_ = std::move(factory);
}
Mpeg::Context &Mpeg::context(psprecomp::GuestMemory &memory, std::uint32_t pointer) {
    buffer(memory, pointer, 4);
    const auto found = contexts_.find(memory.load32(pointer));
    if (found == contexts_.end()) throw Error("Unknown MPEG context handle");
    return found->second;
}
void Mpeg::write_ring(psprecomp::GuestMemory &memory, const Ring &ring) {
    const std::array<std::uint32_t, 12> values{ring.packets, ring.read, ring.written, ring.in_use, 2048,
        ring.data, ring.callback, ring.argument, ring.data + ring.packets * 2048, 0, ring.context, ring.gp};
    buffer(memory, ring.address, values.size() * 4);
    for (unsigned i = 0; i < values.size(); ++i) memory.store32(ring.address + i * 4, values[i]);
}
Mpeg::RingCallback Mpeg::next_callback(Ring &ring) {
    ring.put->offered = std::min(ring.put->remaining, ring.packets - ring.written);
    return {ring.address, ring.callback, ring.data + ring.written * 2048, ring.put->offered, ring.argument, ring.gp};
}
void Mpeg::feed(Context &ctx, std::span<const std::uint8_t> bytes, bool end) {
    const auto payloads = ctx.stream.feed(bytes, end);
    const auto append = [&](std::vector<EncodedVideo> units) {
        for (auto &unit : units) {
            if (ctx.access.size() >= 2048) throw Error("MPEG pending access-unit bound exceeded");
            std::uint64_t packet_end = ctx.stream_bytes;
            for (const auto &[elementary_end, program_end] : ctx.ranges)
                if (unit.source_end <= elementary_end) {
                    packet_end = program_end - (elementary_end - unit.source_end); break;
                }
            ctx.access.push_back({std::move(unit), packet_end / 2048});
        }
    };
    for (const auto &payload : payloads) {
        if (payload.audio) { feed_audio(ctx, payload); continue; }
        ctx.elementary_bytes += payload.bytes.size();
        ctx.ranges.emplace_back(ctx.elementary_bytes, payload.source_end);
        append(ctx.decoder->parse(payload.bytes, payload.pts, payload.dts, false));
    }
    if (end) {
        append(ctx.decoder->parse({}, video_no_timestamp, video_no_timestamp, true)); ctx.ended = true;
        if (!ctx.audio_pending.empty() || !ctx.audio_anchors.empty())
            throw Error("Truncated/misaligned PSMF ATRAC frame at end of stream");
    }
}
void Mpeg::feed_audio(Context &ctx, const VideoPayload &payload) {
    if (!ctx.header || ctx.header->streams != 2 || !ctx.audio_handle || !ctx.audio_decoder)
        throw Error("PSMF audio requires its registered real ATRAC decoder");
    const auto anchor = ctx.audio_bytes + payload.first_frame_offset;
    if (!ctx.audio_anchors.emplace(anchor, payload.pts).second)
        throw Error("Duplicate PSMF audio timestamp anchor");
    ctx.audio_bytes += payload.bytes.size();
    ctx.audio_ranges.emplace_back(ctx.audio_bytes, payload.source_end);
    ctx.audio_pending.insert(ctx.audio_pending.end(), payload.bytes.begin(), payload.bytes.end());
    if (ctx.audio_pending.size() > 8192) throw Error("PSMF ATRAC frame assembly exceeds its bound");
    std::size_t position = 0;
    while (ctx.audio_pending.size() - position >= 8) {
        const auto frame = std::span<const std::uint8_t>(ctx.audio_pending).subspan(position);
        if (frame[0] != 0x0F || frame[1] != 0xD0 || (frame[2] & 0xFC) != 0x28 ||
            frame[4] || frame[5] || frame[6] || frame[7])
            throw Error("Unsupported PSMF ATRAC3plus stereo44100 frame header");
        const auto size = (((std::uint32_t(frame[2]) & 3) << 8) | frame[3]) * 8 + 8;
        if (size + 8 > 0x840) throw Error("PSMF ATRAC frame exceeds its ES buffer");
        if (frame.size() < size + 8) break;
        const auto start = ctx.audio_consumed + position, end = start + size + 8;
        auto pts = video_no_timestamp;
        if (!ctx.audio_anchors.empty() && ctx.audio_anchors.begin()->first < end) {
            if (ctx.audio_anchors.begin()->first != start)
                throw Error("PSMF audio PES timestamp is not at an ATRAC frame boundary");
            pts = ctx.audio_anchors.begin()->second; ctx.audio_anchors.erase(ctx.audio_anchors.begin());
            if (!ctx.audio_anchors.empty() && ctx.audio_anchors.begin()->first < end)
                throw Error("Multiple PSMF timestamps inside one ATRAC frame");
        }
        std::uint64_t packet_end = payload.source_end;
        for (const auto &[elementary_end, program_end] : ctx.audio_ranges)
            if (end <= elementary_end) { packet_end = program_end - (elementary_end - end); break; }
        if (ctx.audio_access.size() >= 2048) throw Error("MPEG pending audio access-unit bound exceeded");
        ctx.audio_access.push_back({{frame.begin() + 8, frame.begin() + 8 + size}, pts, packet_end / 2048});
        position += size + 8;
    }
    ctx.audio_pending.erase(ctx.audio_pending.begin(), ctx.audio_pending.begin() + position);
    ctx.audio_consumed += position;
}
void Mpeg::release_consumed(psprecomp::GuestMemory &memory, Context &ctx) {
    // Both registered consumers must advance before the shared PS ring can be
    // reused. Encoded pending AUs own their copies after GetAu, as with AVC.
    const bool audio_stream = ctx.audio_handle && ctx.header && ctx.header->streams == 2;
    release_through(memory, ctx, audio_stream ? std::min(ctx.video_released, ctx.audio_released) : ctx.video_released);
}
void Mpeg::release_through(psprecomp::GuestMemory &memory, Context &ctx, std::uint64_t end) {
    auto &ring = rings_.at(ctx.ring);
    const auto released = std::min(end, ring.accepted);
    if (released > ring.released) {
        const auto count = static_cast<std::uint32_t>(released - ring.released);
        ring.in_use -= std::min(count, ring.in_use); ring.read = (ring.read + count) % ring.packets;
        ring.released = released; write_ring(memory, ring);
    }
}
Mpeg::PutStep Mpeg::complete_put(psprecomp::GuestMemory &memory, std::uint32_t address, std::int32_t result) {
    auto &ring = rings_.at(address);
    if (!ring.put) throw Error("MPEG callback has no pending ringbuffer submission");
    if (result < 0) { ring.put.reset(); return {{}, result}; }
    if (static_cast<std::uint32_t>(result) > ring.put->offered) { ring.put.reset(); return {{}, static_cast<std::int32_t>(invalid)}; }
    auto &ctx = contexts_.at(ring.context);
    const auto count = static_cast<std::uint32_t>(result);
    const auto bytes = count * 2048;
    if (!ctx.header || ctx.stream_bytes + bytes > ctx.header->stream_size)
        throw Error("MPEG callback data exceeds its verified PSMF stream extent");
    if (bytes) {
        std::vector<std::uint8_t> input(bytes); memory.copy_out(ring.data + ring.written * 2048, input);
        ctx.stream_bytes += bytes;
        feed(ctx, input, ctx.stream_bytes == ctx.header->stream_size);
        ring.written = (ring.written + count) % ring.packets; ring.accepted += count; ring.in_use += count;
        ring.put->remaining -= count; ring.put->total += count; write_ring(memory, ring);
    } else if (ctx.stream_bytes != ctx.header->stream_size) throw Error("MPEG file callback ended before the declared stream extent");
    if (count == ring.put->offered && ring.put->remaining) return {next_callback(ring), 0};
    const auto total = static_cast<std::int32_t>(ring.put->total); ring.put.reset(); return {{}, total};
}
std::optional<Mpeg::RingCallback> Mpeg::invoke(std::uint32_t nid, psprecomp::Runtime &runtime,
    psprecomp::AllegrexContext &cpu) {
    auto &memory = runtime.memory();
    const auto argument = [&](unsigned index) { return cpu.gpr[4 + index]; };
    const auto output = [&](std::uint32_t value) { cpu.gpr[2] = value; };
    switch (nid) {
    case 0x682A619B: { // Init opens a real decoder before reporting readiness.
        if (!factory_) throw Error("MPEG has no configured video decoder backend");
        auto probe = factory_(); if (!probe) throw Error("MPEG decoder factory returned no decoder");
        initialized_ = true; output(0); break;
    }
    case 0x874624D6:
        if (!contexts_.empty()) throw Error("MPEG Finish requires deleting active contexts first");
        rings_.clear(); initialized_ = false; output(0); break;
    case 0xD7A29F46: output(argument(0) * 0x868u); break;
    case 0xC132E22F:
        if (argument(0) != 0) throw Error("Only MPEG context mode zero is supported");
        output(0x10000); break;
    case 0x37295ED8: {
        const auto address = argument(0), count = argument(1), data = argument(2);
        if (!count || count > 4096 || static_cast<std::int32_t>(argument(3)) < 0 ||
            argument(3) < std::uint64_t(count) * 0x868u) { output(invalid); break; }
        buffer(memory, address, 48); buffer(memory, data, static_cast<std::size_t>(count) * 2048);
        if (!argument(4) || !memory.contains(argument(4), 4)) throw Error("MPEG ringbuffer has no translated callback");
        if (rings_.contains(address) || rings_.size() >= 8) throw Error("MPEG ringbuffer ownership/count bound exceeded");
        Ring ring; ring.address = address; ring.packets = count; ring.data = data;
        ring.callback = argument(4); ring.argument = argument(5); ring.gp = cpu.gpr[28];
        rings_.emplace(address, ring); write_ring(memory, ring); output(0); break;
    }
    case 0x13407F13: {
        const auto found = rings_.find(argument(0));
        if (found == rings_.end()) { output(bad_handle); break; }
        if (found->second.context || found->second.put) throw Error("Destroying an owned/active MPEG ringbuffer");
        rings_.erase(found); output(0); break;
    }
    case 0xD8C5F121: {
        if (!initialized_) throw Error("MPEG Create requires an initialized decoder service");
        const auto found = rings_.find(argument(3));
        if (found == rings_.end() || static_cast<std::int32_t>(argument(2)) < 0 || argument(2) < 0x10000) { output(invalid); break; }
        buffer(memory, argument(0), 4); buffer(memory, argument(1), 0x10000);
        if (argument(4) != 512 || argument(5) || argument(6) || found->second.context || contexts_.contains(argument(1)))
            throw Error("Unsupported MPEG context format/options/ownership");
        Context ctx; ctx.address = argument(1); ctx.ring = argument(3); ctx.frame_width = argument(4); ctx.decoder = factory_();
        if (!ctx.decoder) throw Error("MPEG decoder factory returned no decoder");
        const std::array<std::uint8_t, 12> tag{'L','I','B','M','P','E','G',0,'0','0','1',0};
        memory.copy_in(ctx.address, tag); memory.store32(ctx.address + 16, ctx.ring);
        memory.store32(argument(0), ctx.address); found->second.context = ctx.address;
        write_ring(memory, found->second); contexts_.emplace(ctx.address, std::move(ctx)); output(0); break;
    }
    case 0x606A4649: {
        auto &ctx = context(memory, argument(0)); auto &ring = rings_.at(ctx.ring);
        if (ring.put) throw Error("Deleting MPEG context during a file callback");
        const auto address = ctx.address; ring.context = 0; ring.in_use = 0;
        write_ring(memory, ring); contexts_.erase(address); output(0); break;
    }
    case 0x21FF80E4: {
        auto &ctx = context(memory, argument(0)); buffer(memory, argument(1), 2048); buffer(memory, argument(2), 4);
        std::array<std::uint8_t, 2048> bytes{}; memory.copy_out(argument(1), bytes);
        const auto header = PsmfHeader::parse(bytes);
        ctx.header = header; memory.store32(argument(2), header.stream_offset); output(0); break;
    }
    case 0x611E9E11: {
        buffer(memory, argument(0), 2048); buffer(memory, argument(1), 4);
        std::array<std::uint8_t, 2048> bytes{}; memory.copy_out(argument(0), bytes);
        memory.store32(argument(1), PsmfHeader::parse(bytes).stream_size); output(0); break;
    }
    case 0x42560F23: {
        auto &ctx = context(memory, argument(0));
        if (argument(2) || argument(1) > 1) throw Error("Only MPEG AVC0/ATRAC0 streams are supported");
        if (argument(1) == 0) {
            if (ctx.stream_handle) throw Error("Duplicate MPEG AVC stream registration");
            ctx.stream_handle = ctx.address + 0x12C0; output(ctx.stream_handle);
        } else {
            if (ctx.audio_handle || !audio_factory_) throw Error("MPEG ATRAC stream needs an available independent audio decoder");
            auto decoder = audio_factory_();
            if (!decoder) throw Error("MPEG audio decoder factory returned no decoder");
            ctx.audio_decoder = std::move(decoder); ctx.audio_handle = ctx.address + 0x1700; output(ctx.audio_handle);
        }
        break;
    }
    case 0x591A4AA2: {
        auto &ctx = context(memory, argument(0));
        if (argument(1) == ctx.stream_handle && ctx.stream_handle) ctx.stream_handle = 0;
        else if (argument(1) == ctx.audio_handle && ctx.audio_handle) {
            ctx.audio_handle = 0; ctx.audio_decoder.reset(); ctx.audio_access.clear(); ctx.pending_audio.clear(); ctx.audio_es.clear();
            release_consumed(memory, ctx);
        } else { output(bad_handle); break; }
        output(0); break;
    }
    case 0xA780CF7E: {
        auto &ctx = context(memory, argument(0)); if (ctx.es) throw Error("Multiple MPEG AVC ES buffers are not supported");
        ctx.es = true; output(1); break;
    }
    case 0xCEB870B1: {
        auto &ctx = context(memory, argument(0));
        if (!ctx.es || argument(1) != 1) { output(bad_handle); break; }
        ctx.es = false; output(0); break;
    }
    case 0x167AFD9E: {
        auto &ctx = context(memory, argument(0));
        buffer(memory, argument(2), 24);
        if (argument(1) == 1) { if (!ctx.es) { output(invalid); break; } }
        else {
            if (!ctx.audio_handle) { output(invalid); break; }
            buffer(memory, argument(1), 0x840);
            if (ctx.pending_audio.contains(argument(2)) || ctx.audio_es.size() >= 8)
                throw Error("Active/excessive MPEG audio AU initialization");
            ctx.audio_es[argument(2)] = argument(1);
        }
        store_time(memory, argument(2), 0); store_time(memory, argument(2) + 8, 0);
        memory.store32(argument(2) + 16, argument(1));
        memory.store32(argument(2) + 20, 0); output(0); break;
    }
    case 0xF8DCB679: {
        context(memory, argument(0)); buffer(memory, argument(1), 4); buffer(memory, argument(2), 4);
        memory.store32(argument(1), 0x840); memory.store32(argument(2), 0x2000); output(0); break;
    }
    case 0xB5F6DC87: {
        const auto found = rings_.find(argument(0));
        if (found == rings_.end() || !found->second.context) { output(bad_handle); break; }
        output(found->second.packets - found->second.in_use); break;
    }
    case 0xB240A59E: {
        const auto found = rings_.find(argument(0));
        if (found == rings_.end() || !found->second.context) { output(bad_handle); break; }
        auto &ring = found->second;
        if (ring.put) throw Error("Reentrant MPEG ringbuffer submission");
        if (static_cast<std::int32_t>(argument(1)) < 0 || static_cast<std::int32_t>(argument(2)) < 0) { output(0); break; }
        const auto count = std::min({argument(1), argument(2), ring.packets - ring.in_use});
        if (!count) { output(0); break; }
        ring.put = Ring::Put{count, 0, 0}; return next_callback(ring);
    }
    case 0xFE246728: {
        auto &ctx = context(memory, argument(0));
        if (argument(1) != ctx.stream_handle || !ctx.stream_handle) { output(bad_handle); break; }
        buffer(memory, argument(2), 24); if (argument(3)) buffer(memory, argument(3), 4);
        if (ctx.pending_au.contains(argument(2))) throw Error("MPEG access unit must be decoded before it is replaced");
        if (ctx.access.empty()) { output(need_data); break; }
        auto access = std::move(ctx.access.front()); ctx.access.pop_front();
        store_time(memory, argument(2), access.coded.pts); store_time(memory, argument(2) + 8, access.coded.dts);
        memory.store32(argument(2) + 16, 1); memory.store32(argument(2) + 20, static_cast<std::uint32_t>(access.coded.bytes.size()));
        if (argument(3)) memory.store32(argument(3), 1);
        ctx.video_released = ctx.ended && ctx.access.empty() ? rings_.at(ctx.ring).accepted : access.packet_end;
        release_consumed(memory, ctx);
        ctx.pending_au.emplace(argument(2), std::move(access)); output(0); break;
    }
    case 0xE1CE83A7: {
        auto &ctx = context(memory, argument(0));
        if (!ctx.audio_handle || argument(1) != ctx.audio_handle) { output(bad_handle); break; }
        buffer(memory, argument(2), 24); if (argument(3)) buffer(memory, argument(3), 4);
        const auto es = ctx.audio_es.find(argument(2));
        if (es == ctx.audio_es.end()) { output(invalid); break; }
        buffer(memory, es->second, 0x840);
        if (ctx.pending_audio.contains(argument(2))) throw Error("MPEG audio AU must be decoded before it is replaced");
        if (ctx.audio_access.empty()) { output(need_data); break; }
        auto access = std::move(ctx.audio_access.front()); ctx.audio_access.pop_front();
        memory.copy_in(es->second, access.bytes);
        store_time(memory, argument(2), access.pts); store_time(memory, argument(2) + 8, video_no_timestamp);
        memory.store32(argument(2) + 16, es->second); memory.store32(argument(2) + 20, static_cast<std::uint32_t>(access.bytes.size()));
        if (argument(3)) memory.store32(argument(3), 0);
        ctx.audio_released = ctx.ended && ctx.audio_access.empty() ? rings_.at(ctx.ring).accepted : access.packet_end;
        release_consumed(memory, ctx); ctx.pending_audio.emplace(argument(2), std::move(access)); output(0); break;
    }
    case 0x800C44DF: {
        auto &ctx = context(memory, argument(0));
        if (!ctx.audio_decoder || argument(3) > 1) throw Error("Unsupported MPEG ATRAC decoder/init state");
        buffer(memory, argument(1), 24); buffer(memory, argument(2), 0x2000);
        const auto found = ctx.pending_audio.find(argument(1));
        if (found == ctx.pending_audio.end()) { output(0x807F00FDu); break; }
        if (argument(3)) ctx.audio_decoder->reset();
        const auto pcm = ctx.audio_decoder->decode(found->second.bytes);
        if (pcm.size() != 4096) throw Error("MPEG ATRAC decoder did not produce2048 stereo PCM samples");
        for (std::size_t i = 0; i < pcm.size(); ++i)
            memory.store16(argument(2) + static_cast<std::uint32_t>(i * 2), static_cast<std::uint16_t>(pcm[i]));
        ctx.pending_audio.erase(found); memory.store32(argument(1) + 20, 0); output(0); break;
    }
    case 0x0E3C2E9D:
    case 0x740FCCD1: {
        auto &ctx = context(memory, argument(0)); const bool draining = nid == 0x740FCCD1;
        const auto stride = argument(draining ? 1 : 2), destination_pointer = argument(draining ? 2 : 3), status = argument(draining ? 3 : 4);
        if (stride != ctx.frame_width || !ctx.header) throw Error("Unsupported MPEG decode stride/header state");
        buffer(memory, destination_pointer, 4); buffer(memory, status, 4);
        const auto destination = memory.load32(destination_pointer);
        buffer(memory, destination, static_cast<std::size_t>(stride) * ctx.header->height * 4);
        std::optional<VideoFrame> frame;
        if (draining) frame = ctx.decoder->drain();
        else {
            const auto found = ctx.pending_au.find(argument(1));
            if (found == ctx.pending_au.end()) { output(need_data); break; }
            frame = ctx.decoder->decode(found->second.coded); ctx.pending_au.erase(found);
            memory.store32(argument(1) + 20, 0);
        }
        if (frame) {
            if (frame->width != ctx.header->width || frame->height != ctx.header->height ||
                frame->rgba.size() != static_cast<std::size_t>(frame->width) * frame->height * 4)
                throw Error("Decoded MPEG frame disagrees with the verified PSMF header");
            for (std::uint32_t row = 0; row < frame->height; ++row)
                memory.copy_in(destination + row * stride * 4,
                    std::span<const std::uint8_t>(frame->rgba).subspan(row * frame->width * 4, frame->width * 4));
        }
        memory.store32(status, frame ? 1 : 0); output(0); break;
    }
    case 0x707B7629:
    case 0x4571CC64: {
        auto &ctx = context(memory, argument(0)); auto &ring = rings_.at(ctx.ring);
        if (ring.put) throw Error("Flushing MPEG during a file callback");
        ctx.decoder->reset(); ctx.access.clear(); ctx.pending_au.clear(); ctx.ranges.clear();
        ctx.elementary_bytes = 0;
        if (nid == 0x4571CC64) {
            // AVC-only flush must not reset a registered ATRAC decoder or
            // discard its independently queued AUs/partial interleaved frame.
            ctx.video_released = ring.accepted; release_consumed(memory, ctx); output(0); break;
        }
        ctx.stream.reset();
        ctx.elementary_bytes = 0; ctx.stream_bytes = 0; ctx.ended = false;
        if (ctx.audio_decoder) ctx.audio_decoder->reset();
        ctx.audio_access.clear(); ctx.pending_audio.clear(); ctx.audio_pending.clear();
        ctx.audio_anchors.clear(); ctx.audio_ranges.clear();
        ctx.audio_bytes = ctx.audio_consumed = ctx.video_released = ctx.audio_released = 0;
        ring.read = ring.written = ring.in_use = 0; ring.accepted = ring.released = 0; write_ring(memory, ring); output(0); break;
    }
    default: throw Error("Unsupported MPEG operation or audio codec API");
    }
    return std::nullopt;
}
}
