#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <limits>

namespace badman {
using psprecomp::Error;
void Host::install_semaphores() {
    constexpr std::uint32_t unknown = 0x80020199u, unavailable = 0x800201ADu;
    constexpr std::uint32_t overflow = 0x800201AEu, illegal_count = 0x800201BDu;
    const auto preempt = [this](Context &ctx) {
        if (!interrupts_enabled_ || in_interrupt_context()) return;
        const auto running = threads_.find(psprecomp::runtime_thread_uid());
        if (running == threads_.end() || running->second.state != State::running) return;
        if (std::any_of(threads_.begin(), threads_.end(), [&](const auto &entry) {
            return entry.second.state == State::ready && entry.second.priority < running->second.priority;
        })) {
            auto &thread = running->second;
            thread.context = ctx; thread.context.pc = ctx.gpr[31]; thread.state = State::ready;
            schedule(ctx);
        }
    };
    bind("ThreadManForUser", 0xD6DA4BA1u, "sceKernelCreateSema", [this](Context &ctx) {
        if (ctx.gpr[4] == 0) { ctx.gpr[2] = 0x80020001u; return; }
        if (ctx.gpr[5] & ~0x1FFu) { ctx.gpr[2] = 0x80020191u; return; }
        if (ctx.gpr[8] && ((ctx.gpr[8] & 3u) || !runtime_.memory().contains(ctx.gpr[8], 4)))
            throw Error("Invalid semaphore option pointer");
        // The documented option contains only a size; observed hardware accepts
        // every tested size. There is no additional option payload to emulate.
        Semaphore semaphore;
        for (std::uint32_t i = 0; i < 31; ++i) {
            const auto character = runtime_.memory().load8(ctx.gpr[4] + i);
            if (!character) break;
            semaphore.name += static_cast<char>(character);
        }
        semaphore.attributes = ctx.gpr[5];
        // Hardware also accepts negative initial/max values and initial > max.
        semaphore.initial = semaphore.count = static_cast<std::int32_t>(ctx.gpr[6]);
        semaphore.maximum = static_cast<std::int32_t>(ctx.gpr[7]);
        if (next_uid_ <= 0 || next_uid_ == std::numeric_limits<std::int32_t>::max())
            throw Error("Badman UID space exhausted");
        const auto uid = next_uid_;
        semaphores_.emplace(uid, std::move(semaphore)); ++next_uid_;
        ctx.gpr[2] = static_cast<std::uint32_t>(uid);
    });
    bind("ThreadManForUser", 0x28B6489Cu, "sceKernelDeleteSema", [this, preempt](Context &ctx) {
        const auto found = semaphores_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == semaphores_.end()) { ctx.gpr[2] = unknown; return; }
        for (const auto uid : found->second.waiters) {
            auto &thread = threads_.at(uid);
            thread.context.gpr[2] = 0x800201B5u; // SCE_KERNEL_ERROR_WAIT_DELETE
            thread.wait_semaphore = 0; thread.wait_semaphore_count = 0; thread.state = State::ready;
        }
        semaphores_.erase(found); ctx.gpr[2] = 0;
        preempt(ctx);
    });
    bind("ThreadManForUser", 0x3F53E640u, "sceKernelSignalSema", [this, preempt](Context &ctx) {
        const auto found = semaphores_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == semaphores_.end()) { ctx.gpr[2] = unknown; return; }
        auto &semaphore = found->second;
        auto ordered = semaphore.waiters;
        if (semaphore.attributes & 0x100u) std::stable_sort(ordered.begin(), ordered.end(), [&](auto left, auto right) {
            return threads_.at(left).priority < threads_.at(right).priority;
        });
        auto remaining = static_cast<std::int64_t>(semaphore.count) + static_cast<std::int32_t>(ctx.gpr[5]);
        std::size_t awakened = 0;
        // Commit only after checking the residual count. Waiters can consume a
        // signal larger than maximum; overflow must not partially wake anyone.
        for (const auto uid : ordered) {
            const auto requested = threads_.at(uid).wait_semaphore_count;
            if (remaining < requested) break; // Preserve head-of-line ordering.
            remaining -= requested; ++awakened;
        }
        if (remaining > semaphore.maximum) { ctx.gpr[2] = overflow; return; }
        if (remaining < std::numeric_limits<std::int32_t>::min())
            throw Error("Semaphore signal signed-count wrap is not implemented");
        semaphore.count = static_cast<std::int32_t>(remaining);
        for (std::size_t i = 0; i < awakened; ++i) {
            auto &thread = threads_.at(ordered[i]);
            thread.context.gpr[2] = 0; thread.wait_semaphore = 0; thread.wait_semaphore_count = 0;
            thread.state = State::ready;
        }
        ordered.erase(ordered.begin(), ordered.begin() + awakened);
        semaphore.waiters = std::move(ordered); ctx.gpr[2] = 0;
        preempt(ctx);
    });
    bind("ThreadManForUser", 0x58B1F937u, "sceKernelPollSema", [this](Context &ctx) {
        const auto found = semaphores_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == semaphores_.end()) { ctx.gpr[2] = unknown; return; }
        auto &semaphore = found->second;
        const auto requested = static_cast<std::int32_t>(ctx.gpr[5]);
        // Observable PSP precedence: zero request on an empty semaphore reports
        // SEMA_ZERO, whereas zero on a signaled semaphore reports ILLEGAL_COUNT.
        if (semaphore.count <= 0 || !semaphore.waiters.empty()) { ctx.gpr[2] = unavailable; return; }
        if (requested <= 0 || requested > semaphore.maximum) { ctx.gpr[2] = illegal_count; return; }
        if (requested > semaphore.count) { ctx.gpr[2] = unavailable; return; }
        semaphore.count -= requested; ctx.gpr[2] = 0;
    });
    const auto wait = [this](Context &ctx) {
        const auto found = semaphores_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == semaphores_.end()) { ctx.gpr[2] = unknown; return; }
        auto &semaphore = found->second;
        const auto requested = static_cast<std::int32_t>(ctx.gpr[5]);
        if (requested <= 0 || requested > semaphore.maximum) { ctx.gpr[2] = illegal_count; return; }
        if (in_interrupt_context()) { ctx.gpr[2] = 0x80020064u; return; }
        if (!interrupts_enabled_) { ctx.gpr[2] = 0x800201A7u; return; }
        if (ctx.gpr[6]) throw Error("Timed semaphore waits are not implemented");
        auto &thread = current();
        auto insert = semaphore.waiters.end();
        if (semaphore.attributes & 0x100u) insert = std::find_if(semaphore.waiters.begin(), semaphore.waiters.end(), [&](auto uid) {
            return thread.priority < threads_.at(uid).priority;
        });
        const bool is_first = insert == semaphore.waiters.begin();
        if (is_first && semaphore.count >= requested) {
            semaphore.count -= requested; ctx.gpr[2] = 0; return;
        }
        semaphore.waiters.insert(insert, thread.uid);
        thread.wait_thread = 0; thread.wait_semaphore = found->first; thread.wait_semaphore_count = requested;
        thread.context = ctx; thread.context.pc = ctx.gpr[31]; thread.state = State::waiting;
        schedule(ctx);
    };
    bind("ThreadManForUser", 0x4E3A1105u, "sceKernelWaitSema", wait);
    bind("ThreadManForUser", 0x6D212BACu, "sceKernelWaitSemaCB", [this, wait](Context &ctx) {
        if (std::any_of(callbacks_.begin(), callbacks_.end(), [](const auto &entry) {
            return entry.second.owner == psprecomp::runtime_thread_uid() && entry.second.pending != 0;
        })) throw Error("WaitSemaCB pending-callback dispatch is not implemented");
        wait(ctx);
    });
}
}
