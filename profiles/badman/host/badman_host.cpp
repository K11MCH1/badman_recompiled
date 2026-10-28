#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <ostream>
#include <iostream>
#include <atomic>

namespace badman {
using namespace psprecomp;
Host *Host::active_ = nullptr;

Host::Host(Runtime &runtime, std::uint32_t arena_start, std::uint32_t image_end,
           std::uint32_t gp, std::ostream &trace)
    : runtime_(runtime), trace_(trace), image_end_(image_end), gp_(gp),
      ge_(runtime.memory(), [this](const GeInterrupt &interrupt) { invoke_ge_callback(interrupt); }),
      disc_(runtime.game_root()) {
    if (active_ != nullptr) throw Error("Only one Badman host may own runtime hooks");
    if (arena_start > 0x09FEFF00u || arena_start < image_end)
        throw Error("Badman allocation arena overlaps its ELF or bootstrap stack");
    arena_start = (arena_start + 255u) & ~255u;
    free_[arena_start] = 0x09FF0000u - arena_start;
    active_ = this;
}
Host::~Host() {
    if (active_ == this) {
        set_runtime_pre_dispatch_hook(nullptr);
        set_runtime_pre_chained_call_hook(nullptr);
        set_runtime_post_chained_call_hook(nullptr);
        active_ = nullptr;
        set_runtime_thread_identity(-1, "");
    }
}
std::uint32_t Host::free_bytes() const {
    std::uint32_t total = 0;
    for (const auto &[address, size] : free_) { (void)address; total += size; }
    return total;
}
std::uint32_t Host::allocate(std::uint32_t size, bool high, std::uint32_t alignment) {
    if (size == 0 || size > 0x02000000u) throw Error("Invalid Badman allocation size");
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0 || alignment > 0x02000000u)
        throw Error("Invalid Badman allocation alignment");
    alignment = std::max(alignment, 256u);
    size = (size + 255u) & ~255u;
    auto selected = free_.end();
    std::uint32_t address = 0;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size) continue;
        const auto candidate = high ? (it->first + it->second - size) & ~(alignment - 1u)
                                    : (it->first + alignment - 1u) & ~(alignment - 1u);
        if (candidate >= it->first && candidate + size <= it->first + it->second) {
            selected = it; address = candidate; if (!high) break;
        }
    }
    if (selected == free_.end()) throw Error("Badman guest allocator exhausted");
    const auto [base, available] = *selected;
    free_.erase(selected);
    if (address > base) free_[base] = address - base;
    if (address + size < base + available) free_[address + size] = base + available - address - size;
    allocations_[address] = size;
    return address;
}
void Host::release(std::uint32_t address) {
    const auto allocation = allocations_.find(address);
    if (allocation == allocations_.end()) throw Error("Invalid guest allocation release");
    auto [block, inserted] = free_.emplace(address, allocation->second);
    (void)inserted;
    allocations_.erase(allocation);
    if (block != free_.begin()) {
        auto previous = std::prev(block);
        if (previous->first + previous->second == block->first) {
            previous->second += block->second;
            free_.erase(block);
            block = previous;
        }
    }
    auto next = std::next(block);
    if (next != free_.end() && block->first + block->second == next->first) {
        block->second += next->second;
        free_.erase(next);
    }
}
std::string Host::string_at(std::uint32_t address) const {
    std::string value;
    for (std::uint32_t i = 0; i < 1024; ++i) {
        const auto ch = runtime_.memory().load8(address + i);
        if (ch == 0) return value;
        value += static_cast<char>(ch);
    }
    throw Error("Unterminated guest string");
}
void Host::observe(const std::string &name, const Context &context) {
    ++import_calls_;
    last_import_ = name;
    if (import_calls_ <= 2000) {
        trace_ << "[hle] " << name << " uid=" << runtime_thread_uid()
               << " pc=" << hex32(context.pc) << " ra=" << hex32(context.gpr[31]);
        for (std::uint32_t i = 4; i < 10; ++i)
            trace_ << " a" << i - 4 << '=' << hex32(context.gpr[i]);
        trace_ << '\n';
    }
}
void Host::bind(const char *library, std::uint32_t nid, const char *name, Handler handler) {
    runtime_.register_hle(library, nid, [this, name = std::string(name), handler](Runtime &, Context &ctx) {
        observe(name, ctx);
        if (in_interrupt_context() && (name == "sceKernelWaitThreadEnd" || name == "sceKernelExitThread"))
            throw Error("Blocking/exiting from an interrupt callback is not supported");
        const auto token = capture_runtime_execution_context();
        const auto import_pc = ctx.pc;
        handler(ctx);
        // The context may now belong to a different PSP task. Never save that
        // replacement task as though it were returning from this import.
        if (!runtime_.stopped() && runtime_execution_context_matches(token))
            poll_thread_timers(ctx, ctx.pc == import_pc ? ctx.gpr[31] : ctx.pc);
    });
}
Host::Thread &Host::current() { return threads_.at(runtime_thread_uid()); }
void Host::dump_scheduler(std::ostream &output) const {
    const auto state_name = [](State state) {
        switch (state) {
        case State::dormant: return "dormant";
        case State::ready: return "ready";
        case State::running: return "running";
        case State::waiting: return "waiting";
        case State::ended: return "ended";
        }
        return "unknown";
    };
    output << "[scheduler-state] owner=" << runtime_thread_uid()
           << " generation=" << runtime_thread_switch_generation()
           << " interrupts=" << interrupts_enabled_
           << " ge_irq=" << in_ge_callback_ << " display_irq=" << in_display_callback_
           << " mpeg_callbacks=" << mpeg_calls_.size() << '\n';
    for (const auto &[uid, thread] : threads_) {
        const auto &context = uid == runtime_thread_uid() && thread.state == State::running ?
            runtime_.cpu() : thread.context;
        output << "[thread-state] uid=" << uid << " name=" << thread.name
               << " state=" << state_name(thread.state) << " priority=" << thread.priority
               << " pc=" << hex32(context.pc) << " ra=" << hex32(context.gpr[31])
               << " sp=" << hex32(context.gpr[29]) << " gp=" << hex32(context.gpr[28]);
        if (thread.wait_thread) output << " wait_thread=" << thread.wait_thread;
        if (thread.wait_semaphore) output << " wait_sema=" << thread.wait_semaphore << " permits=" << thread.wait_semaphore_count;
        if (thread.wait_event) output << " wait_event=" << thread.wait_event->event
            << " bits=" << hex32(thread.wait_event->wanted) << " mode=" << hex32(thread.wait_event->mode);
        if (thread.wait_vblank) output << " wait_vblank=1";
        if (thread.delay_deadline) output << " delay_ns=" << *thread.delay_deadline;
        if (thread.wait_audio) output << " wait_audio=" << thread.wait_audio->channel << " ticket=" << thread.wait_audio->ticket;
        if (thread.wait_input) output << " wait_input=" << hex32(thread.wait_input->address)
            << " count=" << thread.wait_input->count << " order=" << thread.wait_input->order;
        output << '\n';
    }
    for (const auto &[uid, event] : events_) output << "[event-state] uid=" << uid << " name=" << event.name
        << " bits=" << hex32(event.bits) << " waiters=" << event.waiters.size() << '\n';
    for (const auto &[uid, semaphore] : semaphores_) output << "[semaphore-state] uid=" << uid << " name=" << semaphore.name
        << " count=" << semaphore.count << " maximum=" << semaphore.maximum << " waiters=" << semaphore.waiters.size() << '\n';
    for (std::size_t index = 0; index < display_interrupts_.size(); ++index) {
        const auto &handler = display_interrupts_[index];
        if (handler.function || handler.enabled || handler.pending) output << "[display-irq-state] index=" << index
            << " function=" << hex32(handler.function) << " enabled=" << handler.enabled << " pending=" << handler.pending << '\n';
    }
}
bool Host::schedule(Context &context) {
    service_display_timing(context);
    wake_expired_delays();
    Thread *selected = nullptr;
    for (auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread.state == State::ready &&
            (selected == nullptr || thread.priority < selected->priority)) selected = &thread;
    }
    if (selected == nullptr) {
        if (wait_for_next_delay(context)) return schedule(context);
        if (runtime_.stopped()) return false;
        const bool blocked = std::any_of(threads_.begin(), threads_.end(),
            [](const auto &entry) { return entry.second.state == State::waiting; });
        runtime_.stop(blocked ? "Badman scheduler has no runnable thread" : "All Badman threads exited");
        return false;
    }
    selected->state = State::running;
    context = selected->context;
    set_runtime_thread_identity(selected->uid, selected->name);
    trace_ << "[thread] running uid=" << selected->uid << " name=" << selected->name
           << " pc=" << hex32(context.pc) << '\n';
    return true;
}
void Host::finish_thread(Context &context) {
    auto &thread = current();
    trace_ << "[thread] exited uid=" << thread.uid << " status=" << hex32(context.gpr[2]) << '\n';
    thread.state = State::ended;
    thread.delay_deadline.reset();
    thread.wait_vblank = false;
    thread.wait_event.reset(); thread.wait_audio.reset(); thread.wait_input.reset();
    thread.wait_thread = 0; thread.wait_semaphore = 0; thread.wait_semaphore_count = 0;
    for (auto &[uid, waiter] : threads_) {
        (void)uid;
        if (waiter.state == State::waiting && waiter.wait_thread == thread.uid) {
            waiter.context.gpr[2] = 0;
            waiter.wait_thread = 0;
            waiter.state = State::ready;
        }
    }
    schedule(context);
}
void Host::return_from_thread(Runtime &, Context &context) { active_->finish_thread(context); }
void Host::sync_mutex(std::uint32_t address, const Mutex &mutex) {
    const std::array<std::uint32_t, 8> fields{mutex.count, mutex.owner, mutex.attributes, 0, mutex.uid, 0, 0, 0};
    for (std::uint32_t i = 0; i < fields.size(); ++i) runtime_.memory().store32(address + 4 * i, fields[i]);
}
void Host::bootstrap(std::uint32_t entry, const std::string &argument) {
    Thread root;
    root.uid = 1; root.name = "module_start"; root.entry = entry;
    root.priority = root.initial_priority = 32; root.initial_gp = gp_; root.state = State::running;
    root.tls = allocate(256);
    root.context.gpr[28] = gp_;
    root.context.gpr[26] = root.tls;
    root.context.gpr[29] = 0x09FFF000u;
    root.context.gpr[31] = thread_return_pc;
    const auto args = 0x09FFE000u;
    if (argument.size() >= 1024) throw Error("Bootstrap argument is too long");
    for (std::size_t i = 0; i <= argument.size(); ++i)
        runtime_.memory().store8(args + static_cast<std::uint32_t>(i),
            i == argument.size() ? 0 : static_cast<std::uint8_t>(argument[i]));
    root.context.gpr[4] = static_cast<std::uint32_t>(argument.size() + 1);
    root.context.gpr[5] = args;
    root.context.pc = entry;
    threads_.emplace(1, root);
    runtime_.cpu() = root.context;
    runtime_.register_function(thread_return_pc, &return_from_thread, "badman_thread_return");
    set_runtime_thread_identity(1, root.name);
}
void Host::install(const std::vector<PspImport> &imports) {
    for (const auto &import : imports) {
        const std::string name = import.library + "::" + runtime_.nids().resolve(
            import.library, import.nid).value_or(hex32(import.nid));
        runtime_.register_hle(import.library, import.nid, [this, name, library = import.library, nid = import.nid](Runtime &rt, Context &ctx) {
            observe(name, ctx);
            trace_ << "[unimplemented-import] " << name << " ra=" << hex32(ctx.gpr[31]);
            for (std::uint32_t i = 4; i < 12; ++i) trace_ << " a" << i - 4 << '=' << hex32(ctx.gpr[i]);
            trace_ << '\n';
            if (library == "sceUtility" && nid == 0x50C4CD57u && rt.memory().contains(ctx.gpr[4], 4)) {
                const auto size = rt.memory().load32(ctx.gpr[4]);
                trace_ << "[savedata-params] address=" << hex32(ctx.gpr[4]) << " size=" << size << '\n';
                if (size >= 4 && size <= 4096 && !(size & 3u) && rt.memory().contains(ctx.gpr[4], size))
                    for (std::uint32_t offset = 0; offset < size; offset += 4)
                        trace_ << "[savedata-word] offset=" << hex32(offset) << " value="
                               << hex32(rt.memory().load32(ctx.gpr[4] + offset)) << '\n';
            }
            rt.stop("Unimplemented Badman import " + name + " at " + hex32(ctx.pc));
        });
    }
    bind("SysMemUserForUser", 0x91DE343Cu, "sceKernelSetCompiledSdkVersion500_550",
         [](Context &ctx) { ctx.gpr[2] = 0; });
    bind("SysMemUserForUser", 0xF77D77CBu, "sceKernelSetCompilerVersion",
         [](Context &ctx) { ctx.gpr[2] = 0; });
    bind("SysMemUserForUser", 0x237DBD4Fu, "sceKernelAllocPartitionMemory", [this](Context &ctx) {
        const auto mode = ctx.gpr[6];
        if (ctx.gpr[4] != 2 || mode == 2 || mode > 4)
            throw Error("Unsupported Badman partition allocation mode");
        const auto uid = next_uid_++;
        partitions_[uid] = allocate(ctx.gpr[7], mode == 1 || mode == 4, mode >= 3 ? ctx.gpr[8] : 256);
        ctx.gpr[2] = static_cast<std::uint32_t>(uid);
    });
    bind("SysMemUserForUser", 0x9D9A5BA1u, "sceKernelGetBlockHeadAddr", [this](Context &ctx) {
        ctx.gpr[2] = partitions_.at(static_cast<std::int32_t>(ctx.gpr[4]));
    });
    bind("SysMemUserForUser", 0xB6D61D02u, "sceKernelFreePartitionMemory", [this](Context &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        release(partitions_.at(uid)); partitions_.erase(uid); ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0x446D8DE6u, "sceKernelCreateThread", [this](Context &ctx) {
        Thread thread;
        thread.uid = next_uid_++; thread.name = string_at(ctx.gpr[4]);
        thread.entry = ctx.gpr[5]; thread.priority = thread.initial_priority = ctx.gpr[6]; thread.initial_gp = gp_;
        thread.attributes = ctx.gpr[8];
        thread.stack_size = (std::max(ctx.gpr[7], 512u) + 255u) & ~255u;
        thread.stack = allocate(thread.stack_size, true); thread.tls = allocate(256);
        thread.context.gpr[28] = gp_; thread.context.gpr[26] = thread.tls;
        thread.context.gpr[31] = thread_return_pc;
        trace_ << "[thread] created uid=" << thread.uid << " name=" << thread.name
               << " entry=" << hex32(thread.entry) << " stack=" << thread.stack_size << '\n';
        threads_.emplace(thread.uid, thread);
        ctx.gpr[2] = static_cast<std::uint32_t>(thread.uid);
    });
    bind("ThreadManForUser", 0xF475845Du, "sceKernelStartThread", [this](Context &ctx) {
        if (!ctx.gpr[4]) { ctx.gpr[2] = 0x80020197u; return; }
        const auto found = threads_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == threads_.end()) { ctx.gpr[2] = 0x80020198u; return; }
        auto &thread = found->second;
        if (thread.state != State::dormant && thread.state != State::ended) { ctx.gpr[2] = 0x800201A4u; return; }
        if (!thread.stack || !thread.tls || !allocations_.contains(thread.stack) || !allocations_.contains(thread.tls))
            throw Error("Thread start requires its original owned stack/TLS allocations");
        const auto requested_size = ctx.gpr[5], source = ctx.gpr[6];
        if ((requested_size & 0x80000000u) || (source && requested_size && !runtime_.memory().contains(source,requested_size))) {
            ctx.gpr[2] = 0x800200D3u; return;
        }
        // NULL argp means no arguments even if arglen is nonzero. Stage source
        // bytes first: a restart may copy arguments from its old stack itself.
        const auto argument_size = source ? requested_size : 0u;
        const auto aligned_size = (static_cast<std::uint64_t>(argument_size)+15u)&~15ull;
        if (thread.stack_size < 320u || aligned_size > thread.stack_size-320u) { ctx.gpr[2] = 0x800200D3u; return; }
        std::vector<std::uint8_t> arguments(argument_size);
        if (argument_size) runtime_.memory().copy_out(source,arguments);
        const auto top = thread.stack + thread.stack_size - 256u;
        const auto argument = (top - argument_size) & ~15u;
        Context fresh{};
        fresh.eat_vfpu_prefixes();
        fresh.gpr[4] = argument_size; fresh.gpr[5] = argument_size ? argument : 0u;
        fresh.gpr[26] = thread.tls; fresh.gpr[28] = thread.initial_gp;
        fresh.gpr[29] = argument - 64u; fresh.gpr[31] = thread_return_pc; fresh.pc = thread.entry;
        if (argument_size) runtime_.memory().copy_in(argument,arguments);
        runtime_.memory().zero(thread.tls,256); // Fresh per-invocation TLS, same owned block.
        thread.context = fresh; thread.priority = thread.initial_priority;
        thread.wait_thread = 0; thread.wait_semaphore = 0; thread.wait_semaphore_count = 0;
        thread.delay_deadline.reset(); thread.wait_event.reset(); thread.wait_vblank = false; thread.wait_audio.reset(); thread.wait_input.reset();
        thread.state = State::ready; ++started_threads_; ctx.gpr[2] = 0;
        if (interrupts_enabled_ && !in_interrupt_context() && thread.priority < current().priority) {
            auto &parent = current(); parent.context = ctx;
            parent.context.pc = ctx.gpr[31]; parent.state = State::ready;
            schedule(ctx);
        }
    });
    bind("ThreadManForUser", 0x293B45B8u, "sceKernelGetThreadId", [](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(runtime_thread_uid());
    });
    bind("ThreadManForUser", 0xAA73C935u, "sceKernelExitThread", [this](Context &ctx) {
        ctx.gpr[2] = ctx.gpr[4]; finish_thread(ctx);
    });
    bind("ThreadManForUser", 0x278C0DF5u, "sceKernelWaitThreadEnd", [this](Context &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        if (threads_.at(uid).state == State::ended) { ctx.gpr[2] = 0; return; }
        if (ctx.gpr[5] != 0) throw Error("Timed thread waits are not implemented");
        auto &thread = current(); thread.wait_thread = uid; thread.state = State::waiting;
        thread.context = ctx; thread.context.pc = ctx.gpr[31];
        schedule(ctx);
    });
    bind("Kernel_Library", 0x092968F4u, "sceKernelCpuSuspendIntr", [this](Context &ctx) {
        ctx.gpr[2] = interrupts_enabled_ ? 1u : 0u;
        interrupts_enabled_ = false;
    });
    bind("Kernel_Library", 0x5F10D406u, "sceKernelCpuResumeIntr", [this](Context &ctx) {
        interrupts_enabled_ = ctx.gpr[4] != 0;
        ctx.gpr[2] = 0;
        if (interrupts_enabled_ && !in_interrupt_context() && std::any_of(threads_.begin(), threads_.end(), [this](const auto &item) {
                return item.second.state == State::ready && item.second.priority < current().priority;
            })) {
            auto &thread = current(); thread.context = ctx;
            thread.context.pc = ctx.gpr[31]; thread.state = State::ready;
            schedule(ctx);
        }
    });
    bind("ThreadManForUser", 0xC07BB470u, "sceKernelCreateFpl", [this](Context &ctx) {
        if (ctx.gpr[5] != 2 || ctx.gpr[6] != 0 || ctx.gpr[9] != 0)
            throw Error("Unsupported fixed-pool attributes or options");
        const auto size = ctx.gpr[7], count = ctx.gpr[8];
        if (size == 0 || size > 0x02000000u || count == 0 || count > 65536)
            throw Error("Invalid fixed-pool size");
        const auto stride = (size + 7u) & ~7u;
        const auto total = static_cast<std::uint64_t>(stride) * count;
        if (total > 0x02000000u) throw Error("Fixed pool exceeds guest RAM");
        FixedPool pool{allocate(static_cast<std::uint32_t>(total)), stride, std::vector<std::uint8_t>(count)};
        const auto uid = next_uid_++;
        pools_.emplace(uid, std::move(pool)); ctx.gpr[2] = static_cast<std::uint32_t>(uid);
    });
    bind("ThreadManForUser", 0x623AE665u, "sceKernelTryAllocateFpl", [this](Context &ctx) {
        auto &pool = pools_.at(static_cast<std::int32_t>(ctx.gpr[4]));
        const auto block = std::find(pool.used.begin(), pool.used.end(), 0);
        if (block == pool.used.end()) throw Error("Fixed pool exhausted; failure-code handling not implemented");
        const auto index = static_cast<std::uint32_t>(std::distance(pool.used.begin(), block));
        runtime_.memory().store32(ctx.gpr[5], pool.base + index * pool.stride);
        *block = 1; ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0xED1410E0u, "sceKernelDeleteFpl", [this](Context &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        release(pools_.at(uid).base); pools_.erase(uid); ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0x19CFF145u, "sceKernelCreateLwMutex", [this](Context &ctx) {
        if (ctx.gpr[8] != 0 || (ctx.gpr[6] & ~0x300u) != 0 ||
            ctx.gpr[7] > 0x7FFFFFFFu || (ctx.gpr[7] > 1 && (ctx.gpr[6] & 0x200u) == 0))
            throw Error("Unsupported lightweight-mutex creation options");
        const auto address = ctx.gpr[4];
        Mutex mutex{static_cast<std::uint32_t>(next_uid_++),
            ctx.gpr[7] ? static_cast<std::uint32_t>(runtime_thread_uid()) : 0u, ctx.gpr[7], ctx.gpr[6]};
        if (!mutexes_.emplace(address, mutex).second) throw Error("Duplicate lightweight mutex");
        sync_mutex(address, mutex); ctx.gpr[2] = 0;
    });
    bind("Kernel_Library", 0xBEA46419u, "sceKernelLockLwMutex", [this](Context &ctx) {
        auto &mutex = mutexes_.at(ctx.gpr[4]);
        const auto count = ctx.gpr[5], uid = static_cast<std::uint32_t>(runtime_thread_uid());
        if (count == 0 || count > 0x7FFFFFFFu || mutex.count > 0x7FFFFFFFu - count)
            throw Error("Invalid lightweight-mutex lock count");
        if (mutex.count != 0 && mutex.owner != uid) {
            runtime_.stop("Contended lightweight-mutex scheduling not implemented"); return;
        }
        if ((mutex.attributes & 0x200u) == 0 && (count != 1 || mutex.count != 0))
            throw Error("Recursive lock of a non-recursive lightweight mutex");
        mutex.count += count; mutex.owner = uid;
        sync_mutex(ctx.gpr[4], mutex); ctx.gpr[2] = 0;
    });
    bind("Kernel_Library", 0x15B6446Bu, "sceKernelUnlockLwMutex", [this](Context &ctx) {
        auto &mutex = mutexes_.at(ctx.gpr[4]);
        if (mutex.owner != static_cast<std::uint32_t>(runtime_thread_uid()) || ctx.gpr[5] == 0 ||
            ctx.gpr[5] > mutex.count) throw Error("Invalid lightweight-mutex unlock");
        mutex.count -= ctx.gpr[5];
        if (mutex.count == 0) mutex.owner = 0;
        sync_mutex(ctx.gpr[4], mutex); ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0x60107536u, "sceKernelDeleteLwMutex", [this](Context &ctx) {
        if (mutexes_.at(ctx.gpr[4]).count != 0) throw Error("Deleting a locked lightweight mutex is not implemented");
        mutexes_.erase(ctx.gpr[4]); sync_mutex(ctx.gpr[4], {}); ctx.gpr[2] = 0;
    });
    bind("scePower", 0xEBD177D6u, "scePowerSetClockFrequency", [this](Context &ctx) {
        // The native diagnostic host records the PSP setting; it does not throttle
        // the host CPU. No guest cycle-accurate timing is claimed.
        if (ctx.gpr[4] == 0 || ctx.gpr[4] > 333 || ctx.gpr[5] == 0 || ctx.gpr[5] > ctx.gpr[4] ||
            ctx.gpr[6] == 0 || ctx.gpr[6] > 166) throw Error("Unsupported PSP clock frequencies");
        clock_frequencies_ = {ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]}; ctx.gpr[2] = 0;
    });
    bind("StdioForUser", 0x172D316Eu, "sceKernelStdin", [](Context &ctx) { ctx.gpr[2] = 0; });
    bind("StdioForUser", 0xA6BAB2E9u, "sceKernelStdout", [](Context &ctx) { ctx.gpr[2] = 1; });
    bind("StdioForUser", 0xF78BA90Au, "sceKernelStderr", [](Context &ctx) { ctx.gpr[2] = 2; });
    bind("IoFileMgrForUser", 0x42EC03ACu, "sceIoWrite", [this](Context &ctx) {
        if (ctx.gpr[4] != 1 && ctx.gpr[4] != 2) throw Error("Only stdout/stderr writes are implemented");
        if (ctx.gpr[6] > runtime_.memory().size()) throw Error("Invalid console write length");
        std::vector<std::uint8_t> bytes(ctx.gpr[6]);
        runtime_.memory().copy_out(ctx.gpr[5], bytes);
        auto &output = ctx.gpr[4] == 1 ? std::cout : std::cerr;
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ctx.gpr[2] = ctx.gpr[6];
    });
    const auto elapsed = [this](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - boot_time_).count());
    };
    bind("ThreadManForUser", 0x369ED59Du, "sceKernelGetSystemTimeLow", elapsed);
    bind("UtilsForUser", 0x91E4F6A7u, "sceKernelLibcClock", elapsed);
    bind("UtilsForUser", 0x27CC57F0u, "sceKernelLibcTime", [this](Context &ctx) {
        const auto seconds = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        if (ctx.gpr[4] != 0) runtime_.memory().store32(ctx.gpr[4], seconds);
        ctx.gpr[2] = seconds;
    });
    bind("UtilsForUser", 0x71EC4271u, "sceKernelLibcGettimeofday", [this](Context &ctx) {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (ctx.gpr[4] != 0) {
            runtime_.memory().store32(ctx.gpr[4], static_cast<std::uint32_t>(micros / 1000000));
            runtime_.memory().store32(ctx.gpr[4] + 4, static_cast<std::uint32_t>(micros % 1000000));
        }
        if (ctx.gpr[5] != 0) {
            runtime_.memory().store32(ctx.gpr[5], 0); runtime_.memory().store32(ctx.gpr[5] + 4, 0);
        }
        ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0xE81CAF8Fu, "sceKernelCreateCallback", [this](Context &ctx) {
        const auto uid = next_uid_++;
        callbacks_.emplace(uid, Callback{string_at(ctx.gpr[4]), ctx.gpr[5], ctx.gpr[6], runtime_thread_uid()});
        ctx.gpr[2] = static_cast<std::uint32_t>(uid);
    });
    bind("LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback", [this](Context &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        (void)callbacks_.at(uid); exit_callback_ = uid; ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0x349D6D6Cu, "sceKernelCheckCallback", [](Context &ctx) {
        // No host event source is wired yet, so no callbacks can be pending.
        ctx.gpr[2] = 0;
    });
    bind("sceGe_user", 0x1F6752ADu, "sceGeEdramGetSize", [](Context &ctx) { ctx.gpr[2] = GuestMemory::kVramSize; });
    bind("sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr", [](Context &ctx) { ctx.gpr[2] = GuestMemory::kVramPhysicalBase; });
    const auto power_tick = [this](Context &ctx) {
        const auto mode = ctx.gpr[4];
        if (mode != 0 && mode != 1 && mode != 6) throw Error("Unsupported PSP power tick mode");
        const auto now = std::chrono::steady_clock::now();
        // Maintain guest idle activity. The diagnostic host has no automatic
        // suspend/display-off transition and does not change Windows policy.
        if (mode == 0 || mode == 1) { power_activity_[0] = now; ++power_ticks_[0]; }
        if (mode == 0 || mode == 6) { power_activity_[1] = now; ++power_ticks_[1]; }
        ctx.gpr[2] = 0;
    };
    bind("sceSuspendForUser", 0x090CCB3Fu, "sceKernelPowerTick", power_tick);
    bind("scePower", 0xEFD3C963u, "scePowerTick", power_tick);
    bind("UtilsForUser", 0x79D1C3FAu, "sceKernelDcacheWritebackAll", [](Context &ctx) {
        // CPU, display and GE access one coherent backing store. Publish prior
        // writes; there is no separate emulated data-cache content to flush.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ctx.gpr[2] = 0;
    });
    bind("UtilsForUser", 0x3EE30821u, "sceKernelDcacheWritebackRange", [this](Context &ctx) {
        if (ctx.gpr[5] && !runtime_.memory().contains(ctx.gpr[4], ctx.gpr[5]))
            throw Error("Invalid data-cache writeback range");
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ctx.gpr[2] = 0;
    });
    bind("sceCtrl", 0x6A2774F3u, "sceCtrlSetSamplingCycle", [this](Context &ctx) {
        const auto cycle = ctx.gpr[4];
        if (cycle && (cycle < 5555u || cycle > 20000u)) { ctx.gpr[2] = 0x800001FEu; return; }
        ctx.gpr[2] = input_.set_sampling_cycle(cycle);
    });
    bind("sceCtrl", 0x1F4011E6u, "sceCtrlSetSamplingMode", [this](Context &ctx) {
        if (ctx.gpr[4] > 1) { ctx.gpr[2] = 0x800001FEu; return; }
        ctx.gpr[2] = input_.set_sampling_mode(ctx.gpr[4]);
    });
    bind("sceCtrl", 0x1F803938u, "sceCtrlReadBufferPositive", [this](Context &ctx) {
        if (in_interrupt_context()) throw Error("Controller read is invalid inside an interrupt callback");
        const auto address=ctx.gpr[4],count=ctx.gpr[5];
        if(count<1||count>64){ctx.gpr[2]=0x80000104u;return;}
        if((address&3u)||!runtime_.memory().contains(address,static_cast<std::size_t>(count)*16))
            throw Error("Invalid SceCtrlData output buffer");
        if (window_) { window_->pump(); if (runtime_.stopped()) return; }
        poll_input_waits(); // An earlier blocked reader owns the next observation.
        const auto filter=controller_filter(runtime_thread_uid(),ctx.gpr[31]);
        if(const auto result=input_.try_read_positive(runtime_.memory(),address,count,filter)){
            ctx.gpr[2]=static_cast<std::uint32_t>(*result);return;
        }
        if(!interrupts_enabled_){ctx.gpr[2]=0x800201A7u;return;}
        if(input_wait_order_==std::numeric_limits<std::uint64_t>::max())throw Error("Controller wait order overflow");
        // Save the PSP continuation before handing ownership to a ready worker.
        // Only the owner-thread sampler will later write a genuine packet.
        auto &thread=current();thread.context=ctx;thread.context.pc=ctx.gpr[31];
        thread.wait_thread=0;thread.wait_semaphore=0;thread.wait_semaphore_count=0;
        thread.wait_event.reset();thread.wait_audio.reset();thread.wait_vblank=false;thread.delay_deadline.reset();
        thread.wait_input=InputWait{address,count,++input_wait_order_,ctx.gpr[31]};thread.state=State::waiting;
        schedule(ctx);
    });
    bind("sceImpose", 0x24FD7BCFu, "sceImposeGetLanguageMode", [this](Context &ctx) {
        if ((ctx.gpr[4] & 3u) || (ctx.gpr[5] & 3u) ||
            !runtime_.memory().contains(ctx.gpr[4], 4) || !runtime_.memory().contains(ctx.gpr[5], 4)) {
            ctx.gpr[2] = 0x80000103u; return;
        }
        // Explicit US-profile defaults: English and cross-button confirmation.
        runtime_.memory().store32(ctx.gpr[4], 1);
        runtime_.memory().store32(ctx.gpr[5], 1);
        ctx.gpr[2] = 0;
    });
    bind("sceUtility", 0x34B78343u, "sceUtilityGetSystemParamString", [this](Context &ctx) {
        constexpr std::array<std::uint8_t, 7> nickname{'P', 'l', 'a', 'y', 'e', 'r', 0};
        if (ctx.gpr[4] != 1 || ctx.gpr[6] < nickname.size() ||
            !runtime_.memory().contains(ctx.gpr[5], ctx.gpr[6])) {
            ctx.gpr[2] = 0x80110103u; return;
        }
        runtime_.memory().copy_in(ctx.gpr[5], nickname);
        ctx.gpr[2] = 0;
    });
    bind("sceUtility", 0xA5DA2406u, "sceUtilityGetSystemParamInt", [this](Context &ctx) {
        if ((ctx.gpr[4] != 8 && ctx.gpr[4] != 9) || (ctx.gpr[5] & 3u) ||
            !runtime_.memory().contains(ctx.gpr[5], 4)) { ctx.gpr[2] = 0x80110103u; return; }
        runtime_.memory().store32(ctx.gpr[5], 1); ctx.gpr[2] = 0;
    });
    install_events();
    install_exit_services();
    install_self_exit();
    install_ge();
    install_display();
    install_io();
    install_modules();
    install_semaphores();
    install_savedata();
    install_osk();
    install_screenshot();
    install_mpeg();
    install_dma();
    install_atrac();
    install_audio();
    install_thread_services();
}
}
