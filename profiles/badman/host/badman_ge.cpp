#include "badman_ge.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <string>
#include <utility>

namespace badman {
using psprecomp::Error;
using psprecomp::hex32;
Ge::Ge(psprecomp::GuestMemory &memory, InterruptSink sink) : Ge(memory, std::move(sink), 1000000) {}
Ge::Ge(psprecomp::GuestMemory &memory, InterruptSink sink, std::uint32_t command_budget)
    : memory_(memory), raster_(memory), sink_(std::move(sink)), command_budget_(command_budget) {
    if (command_budget == 0) throw Error("GE command budget must be positive");
    raster_.set_deferred(true);
}
std::int32_t Ge::set_callback(std::uint32_t address) {
    Callback callback{memory_.load32(address), memory_.load32(address + 4),
                      memory_.load32(address + 8), memory_.load32(address + 12)};
    const auto id = next_callback_++;
    callbacks_.emplace(id, callback);
    return id;
}
void Ge::unset_callback(std::int32_t id) {
    if (callbacks_.erase(id) != 1) throw Error("Invalid GE callback ID");
}
std::uint32_t Ge::relative(std::uint32_t data) const {
    return ((((commands_[0x10] & 0x000F0000u) << 8u) | (data & 0x00FFFFFFu)) + offset_) & 0x0FFFFFFFu;
}
std::int32_t Ge::enqueue(std::uint32_t address, std::uint32_t stall, std::int32_t callback,
                         std::uint32_t options, bool head) {
    address = memory_.canonical(address); stall = memory_.canonical(stall);
    if ((address & 3u) || (stall & 3u) || !memory_.contains(address, 4)) throw Error("Invalid GE list/stall address");
    if (callback >= 0 && !callbacks_.contains(callback)) throw Error("Unknown GE callback ID");
    const auto id = next_list_++;
    List list; list.id = id; list.pc = address; list.stall = stall; list.callback = callback;
    if (options != 0) {
        const auto size = memory_.load32(options);
        if (size < 8 || size > 16 || (size & 3u) || !memory_.contains(options, size))
            throw Error("Unsupported or truncated GE list-options size");
        if (memory_.load32(options + 4) != 0) throw Error("GE per-list context save/restore is not implemented");
        if (size >= 12) {
            const auto capacity = memory_.load32(options + 8);
            if (capacity > 256) throw Error("GE CALL stack capacity exceeds the diagnostic limit");
        }
        // The firmware's caller-supplied stack is for SIGNAL calls (not yet
        // implemented). Raw GE CALL/RET instead has two hardware return slots.
    }
    lists_.emplace(id, std::move(list));
    if (head && !queue_.empty()) queue_.insert(std::next(queue_.begin()), id);
    else queue_.push_back(id);
    pump();
    return id;
}
void Ge::update_stall(std::int32_t id, std::uint32_t address) {
    if (address & 3u) throw Error("Unaligned GE stall address");
    auto &list = lists_.at(id);
    if (list.state == State::cancelled) throw Error("Cannot resume a cancelled GE list");
    list.stall = memory_.canonical(address);
    if (list.state == State::failed) throw Error("Cannot resume a failed GE list");
    pump();
}
void Ge::interrupt(const List &list, bool signal, std::uint32_t token, std::uint32_t next_pc) {
    raster_.flush(); // Guest callbacks can read VRAM and write future texture sources.
    raster_.invalidate_textures();
    const auto found = callbacks_.find(list.callback);
    if (found == callbacks_.end()) return;
    const auto callback = found->second;
    const auto function = signal ? callback.signal : callback.finish;
    if (function == 0) return;
    sink_({signal, function, signal ? callback.signal_arg : callback.finish_arg, token & 0xFFFFu, next_pc});
    ++interrupts_;
}
void Ge::pump() {
    if (pumping_) return;
    pumping_ = true;
    auto remaining_commands = command_budget_;
    try {
        while (!queue_.empty()) {
            const auto id = queue_.front();
            auto &list = lists_.at(id);
            if (list.state == State::failed) throw Error("GE queue is blocked by an earlier failed list");
            try { run(list, remaining_commands); }
            catch (...) { if (list.state != State::cancelled) list.state = State::failed; throw; }
            // A callback may cancel this queue and enqueue replacement work.
            // The retained map node keeps list valid, but the old head no
            // longer owns queue_.front(). Never pop a replacement list.
            if (queue_.empty() || queue_.front() != id) continue;
            if (list.state != State::done) break;
            queue_.pop_front();
        }
        raster_.flush();
    } catch (...) {
        pumping_ = false;
        raster_.flush(); // Retain completed earlier draws before an explicit later failure.
        throw;
    }
    pumping_ = false;
}
int Ge::list_status(std::int32_t id, bool wait) {
    pump();
    const auto state = lists_.at(id).state;
    if (state == State::cancelled) return static_cast<std::int32_t>(0x80000100u);
    if (state == State::done) return 0;
    if (wait) throw Error("GE wait needs a guest-produced stall update before completion");
    if (state == State::stalled) return 3;
    if (state == State::running) return 2;
    if (state == State::failed) throw Error("GE list failed");
    return 1;
}
int Ge::draw_status(bool wait) {
    pump();
    if (queue_.empty()) return 0;
    if (wait) throw Error("GE draw wait needs a guest-produced stall update before completion");
    return list_status(queue_.front(), false);
}
std::int32_t Ge::break_queue(std::uint32_t mode, std::uint32_t parameter) {
    if (mode > 1) return static_cast<std::int32_t>(0x80000107u);
    // PSPSDK specifies unused/K1-checked, not a memory read or context output.
    // The hardware test accepts 0x7fffffef, rejects 0x7ffffff0 and kernel pointers.
    if (parameter > 0x7FFFFFEFu) return static_cast<std::int32_t>(0x80000023u);
    if (queue_.empty()) return static_cast<std::int32_t>(0x80000020u);
    if (mode != 1) throw Error("Active GE break mode 0/continue is not implemented");
    raster_.flush(); // Commit already accepted draws before cancelling later work.
    for (const auto id : queue_) {
        auto &list = lists_.at(id);
        if (list.state != State::done && list.state != State::cancelled) {
            list.state = State::cancelled;
            ++cancelled_;
        }
    }
    queue_.clear();
    return 0;
}
void Ge::run(List &list, std::uint32_t &remaining_commands) {
    list.state = State::running;
    for (;;) {
        if (list.stall && list.pc == list.stall) { list.state = State::stalled; return; }
        if (remaining_commands == 0) throw Error("GE command budget exceeded without completion or stall");
        --remaining_commands;
        const auto pc = list.pc;
        if(pc>=psprecomp::GuestMemory::kVramPhysicalBase && pc<psprecomp::GuestMemory::kPhysicalBase)raster_.flush();
        if ((pc & 3u) || !memory_.contains(pc, 4)) throw Error("Invalid GE PC " + hex32(pc));
        const auto word = memory_.load32(pc), opcode = word >> 24u, data = word & 0x00FFFFFFu;
        commands_[opcode] = word;
        ++executed_;
        auto next = (pc + 4) & 0x0FFFFFFFu;
        switch (opcode) {
        case 0x01: vertex_address_ = relative(data); break;
        case 0x04:
            if ((data & 0xFFFFu) == 0) break;
            try {
                    const auto result = raster_.draw(word, commands_, vertex_address_, &clut_, &matrices_);
                vertex_address_ = result.next_vertex_address;
                color_pixels_ += result.color_pixels; depth_pixels_ += result.depth_pixels;
            } catch (const Error &error) {
                throw Error("GE geometry/transfer renderer required for command " + hex32(word) +
                    " at " + hex32(pc) + ": " + error.what());
            }
            break;
        case 0x05: case 0x06: case 0x07: case 0x09: case 0xEA:
            throw Error("GE geometry/transfer renderer required for command " + hex32(word) + " at " + hex32(pc));
        case 0x08: next = relative(data & ~3u); break;
        case 0x0A:
            // A third raw CALL is ignored by the GE; it does not jump and does
            // not consume another return slot. SIGNAL calls use a different stack.
            if (list.stack.size() == 2) break;
            list.stack.push_back({next, offset_}); next = relative(data & ~3u); break;
        case 0x0B:
            if (list.stack.empty()) throw Error("GE RET without CALL");
            next = list.stack.back().pc; offset_ = list.stack.back().offset; list.stack.pop_back(); break;
        case 0x13: offset_ = data << 8u; break;
        case 0x14: offset_ = pc; break;
        case 0xC4:
            if(psprecomp::GuestMemory::canonical((commands_[0xB0]&0xFFFFF0u)|((commands_[0xB1]&0xFF0000u)<<8))<psprecomp::GuestMemory::kPhysicalBase)
                raster_.flush(); // VRAM palette sources can alias an earlier draw.
            clut_.load(memory_, commands_, data);raster_.palette_loaded();break;
        case 0x0C:
            if ((list.previous >> 24u) == 0x0E) {
                const auto behavior = (list.previous >> 16u) & 255u;
                if (behavior == 1 || behavior == 2) {
                    interrupt(list, true, list.previous, next);
                    if (list.state == State::cancelled) return;
                }
                else if (behavior == 8) { list.ignore_finish = true; memory_.memory_barrier(); }
                else throw Error("Unsupported GE SIGNAL behavior " + std::to_string(behavior));
            } else if ((list.previous >> 24u) == 0x0F) {
                if (list.ignore_finish) { list.ignore_finish = false; break; }
                list.pc = next; list.state = State::done;
                interrupt(list, false, list.previous, next);
                ++completed_;
                return;
            }
            break;
        default: break;
        }
        // DATA registers stream matrix elements; retaining only the last command
        // word would discard earlier elements during the GU initialization list.
        const std::array<std::uint32_t, 5> numbers{0x2A, 0x3A, 0x3C, 0x3E, 0x40};
        const std::array<std::uint32_t, 5> lengths{96, 12, 12, 16, 12};
        for (std::size_t matrix = 0; matrix < numbers.size(); ++matrix) {
            const auto mask = matrix == 0 ? 127u : 15u;
            if (opcode == numbers[matrix]) cursors_[matrix] = data & mask;
            if (opcode == numbers[matrix] + 1) {
                auto &cursor = cursors_[matrix];
                if (cursor < lengths[matrix]) matrices_[matrix][cursor] = data << 8u;
                cursor = (cursor + 1) & mask;
            }
        }
        list.previous = word;
        list.pc = memory_.canonical(next);
    }
}
}
