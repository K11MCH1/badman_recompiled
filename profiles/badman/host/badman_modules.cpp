#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/sha256.hpp"
#include <algorithm>
#include <limits>
#include <ostream>

namespace badman {
using namespace psprecomp;
namespace {
constexpr const char *font_path = "disc0:/PSP_GAME/USRDIR/data_usa/module/libfont.prx";
}
void Host::configure_font_directory(const std::filesystem::path &path) {
    registry_.configure_font_directory(path);
    disc_.mount_font_directory(path);
    trace_ << "[font-config] directory=" << path.string() << " fonts=" << registry_.font_count() << '\n';
}
void Host::reserve(std::uint32_t address, std::uint32_t size) {
    if (!size || (address & 255u) || (size & 255u) ||
        static_cast<std::uint64_t>(address) + size > 0x09FF0000u)
        throw Error("Invalid module reservation");
    auto block = free_.upper_bound(address);
    if (block == free_.begin()) throw Error("Module reservation overlaps occupied guest memory");
    --block;
    const auto base = block->first, available = block->second;
    if (address < base || static_cast<std::uint64_t>(address) + size >
        static_cast<std::uint64_t>(base) + available)
        throw Error("Module reservation overlaps occupied guest memory");
    free_.erase(block);
    if (base < address) free_[base] = address - base;
    if (address + size < base + available) free_[address + size] = base + available - address - size;
}
void Host::configure_font_module(const std::filesystem::path &path, const std::string &hash,
                                 const std::string &source_hash, CodeRegistrar registrar) {
    if (font_image_) throw Error("Font module was already configured");
    if (!registrar || sha256_file(path) != hash) throw Error("Prepared font module hash mismatch");
    auto image = Elf32Image::from_file(path);
    if (image.type() != 2 || image.runtime_entry() != 0xFFFFFFFFu)
        throw Error("Expected a fixed-address font library with no ELF entry");
    std::uint32_t begin = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t end = 0;
    for (const auto &segment : image.segments()) {
        if (segment.type != 1) continue;
        if (!segment.memory_size || segment.file_size > segment.memory_size ||
            !runtime_.memory().contains(segment.vaddr, segment.memory_size))
            throw Error("Invalid prepared font load segment");
        begin = std::min(begin, segment.vaddr);
        end = std::max(end, static_cast<std::uint64_t>(segment.vaddr) + segment.memory_size);
    }
    // Its verified hash ties this image to its statically generated addresses.
    // Placement must leave the main image and bootstrap stack untouched.
    if (begin < image_end_ || end <= begin || end > 0x09FF0000u || (begin & 255u))
        throw Error("Font translation overlaps reserved guest memory");
    font_reservation_size_ = static_cast<std::uint32_t>((end - begin + 255u) & ~255ull);
    reserve(begin, font_reservation_size_); font_reserved_ = true;
    font_base_ = begin; font_end_ = static_cast<std::uint32_t>(end);
    font_source_hash_ = source_hash; font_registrar_ = registrar;
    font_image_.emplace(std::move(image));
    install_entry_hooks();
}
void Host::install_entry_hooks() {
    set_runtime_pre_dispatch_hook(&module_dispatch);
    set_runtime_pre_chained_call_hook(&module_chain);
    set_runtime_post_chained_call_hook(&module_chain);
}
void Host::observe_entry(Context &ctx) {
    if (font_image_) select_module_gp(ctx);
    if (startup_.active())
        startup_.observe(ctx, runtime_thread_uid(), runtime_.memory(), mpeg_.has_live_resources());
}
void Host::select_module_gp(Context &ctx) const {
    const auto pc = GuestMemory::canonical(ctx.pc);
    if (font_image_ && pc >= font_base_ && pc < font_end_) {
        if (!font_uid_ || !font_started_) throw Error("Execution in a stopped or unloaded font module");
        ctx.gpr[28] = font_gp_;
    }
    else if (pc >= 0x08804000u && pc < image_end_) ctx.gpr[28] = gp_;
}
void Host::module_dispatch(Runtime &, Context &ctx, std::uint32_t, std::int32_t) {
    active_->observe_entry(ctx);
}
void Host::module_chain(Runtime &, Context &ctx, std::uint32_t, std::uint32_t) {
    // After a native call, ctx.pc identifies the continuation or newly scheduled
    // thread. Restoring a saved GP could corrupt that thread's module context.
    active_->observe_entry(ctx);
}
void Host::load_font(Context &ctx) {
    const auto path = string_at(ctx.gpr[4]);
    trace_ << "[module-request] " << path << '\n';
    if (path != font_path || !font_image_) {
        runtime_.stop("PSP module loading is not implemented: " + path); return;
    }
    if (ctx.gpr[5] || ctx.gpr[6]) throw Error("Unsupported module load flags/options");
    if (font_uid_) throw Error("Font library is already loaded");
    if ((disc_.status() & 0x20u) == 0) throw Error("Font module requires ready disc media");
    if (sha256_file(disc_.resolve(path)) != font_source_hash_)
        throw Error("Bundled font module hash does not match its static translation");
    if (!font_reserved_) {
        try { reserve(font_base_, font_reservation_size_); }
        catch (const Error &) { ctx.gpr[2] = 0x80020131u; return; } // MEMINUSE: reload leaves live allocations untouched.
        font_reserved_ = true;
    }
    const auto stats = font_image_->load_and_relocate(runtime_.memory());
    if (stats.invalid || stats.unsupported) throw Error("Prepared font relocations unresolved");
    const auto module = font_image_->find_module_info(runtime_.memory());
    if (!module || module->name != "sceFont_Library" || !runtime_.memory().contains(module->gp, 1))
        throw Error("Unexpected prepared font module metadata");
    struct Export { std::string library; std::uint32_t nid, address; };
    std::vector<Export> exports;
    auto &mem = runtime_.memory();
    auto cursor = module->ent_top;
    if (cursor < font_base_ || module->ent_end > font_end_ || cursor >= module->ent_end)
        throw Error("Invalid font export table extent");
    while (cursor < module->ent_end) {
        if (module->ent_end - cursor < 16) throw Error("Truncated font export table");
        const auto words = mem.load8(cursor + 8), variables = mem.load8(cursor + 9);
        const auto functions = mem.load16(cursor + 10);
        const auto table = mem.load32(cursor + 12);
        const auto count = static_cast<std::uint32_t>(functions) + variables;
        if (words < 4 || words * 4u > module->ent_end - cursor || table < font_base_ ||
            static_cast<std::uint64_t>(table) + count * 8u > font_end_)
            throw Error("Invalid font export table");
        const auto name = mem.load32(cursor);
        const auto library = name ? string_at(name) : "syslib";
        if (library == "syslib" && functions) throw Error("Unexpected font lifecycle entry");
        for (std::uint32_t i = 0; i < functions; ++i) {
            const auto address = mem.load32(table + (count + i) * 4u);
            if (address < font_base_ || address >= font_end_ || (address & 3u))
                throw Error("Invalid font function export");
            exports.push_back({library, mem.load32(table + i * 4u), address});
        }
        cursor += words * 4u;
    }
    if (exports.size() != 28) throw Error("Incomplete font export table");
    font_registrar_(runtime_);
    font_gp_ = module->gp;
    font_uid_ = next_uid_++; font_stopped_ = false;
    for (const auto &entry : exports) {
        runtime_.register_hle(entry.library, entry.nid, [this, address = entry.address](Runtime &, Context &call) {
            if (!font_started_) throw Error("Font call before its module was started");
            observe("sceLibFont translated export " + hex32(address), call);
            call.pc = address; call.gpr[28] = font_gp_;
        });
    }
    trace_ << "[module-loaded] sceFont_Library uid=" << font_uid_ << " base=" << hex32(font_base_)
           << " end=" << hex32(font_end_) << " gp=" << hex32(font_gp_) << " exports=" << exports.size() << '\n';
    ctx.gpr[2] = static_cast<std::uint32_t>(font_uid_);
}
void Host::install_modules() {
    bind("sceUtility", 0xE49BFE92u, "sceUtilityUnloadModule", [this](Context &ctx) {
        const auto module=ctx.gpr[4];
        if (module<0x300u || module>0x307u) {ctx.gpr[2]=0x800200D2u;return;}
        auto &loaded=utility_modules_[module-0x300u];
        if (!loaded) {ctx.gpr[2]=0x8002012Eu;return;}
        if (in_interrupt_context()) {ctx.gpr[2]=0x80020143u;return;}
        if (module==0x300u || module==0x302u) {
            if (atrac_.active_stream_count() || mpeg_.has_live_resources() || !mpeg_calls_.empty()) {
                ctx.gpr[2]=0x80020143u;return;
            }
            const auto other=module==0x300u?2u:0u;
            if (!utility_modules_[other] && !atrac_.release_decoder_if_idle()) {
                ctx.gpr[2]=0x80020143u;return;
            }
        } else if (module==0x301u) {
            // PCM tickets and waiters must be closed by their real owners first.
            if (std::any_of(audio_channels_.begin(),audio_channels_.end(),[](const auto &c){return c.reserved;}) ||
                std::any_of(threads_.begin(),threads_.end(),[](const auto &t){return t.second.wait_audio.has_value();})) {
                ctx.gpr[2]=0x80020143u;return;
            }
            sas_cores_.clear();
        }
        loaded=false;ctx.gpr[2]=0;
        trace_ << "[module-unloaded] AV utility module " << hex32(module) << '\n';
    });
    bind("sceUtility", 0x2A2B3DE0u, "sceUtilityLoadModule", [this](Context &ctx) {
        const auto module = ctx.gpr[4];
        if (module < 0x300u || module > 0x307u) {
            runtime_.stop("Unsupported utility module request " + hex32(module)); return;
        }
        auto &loaded = utility_modules_[module - 0x300u];
        if (loaded) { ctx.gpr[2] = 0x80111102u; return; }
        // This title imports PCM/ATRAC and four retained SAS configuration
        // operations; its translated code performs the SFX mixing itself.
        // AVCODEC/ATRAC readiness pins an actual decoder, without allocating a
        // guest ATRAC ID or inventing decoded PCM/device completion.
        if (module == 0x300u || module == 0x302u) {
            try { atrac_.prepare_decoder(); }
            catch (const Error &error) {
                trace_ << "[module-unavailable] AV utility module " << hex32(module) << ": " << error.what() << '\n';
                ctx.gpr[2] = 0x8002013Cu; return;
            }
        } else if (module != 0x301u) {
            trace_ << "[module-unavailable] AV utility module " << hex32(module) << '\n';
            ctx.gpr[2] = 0x8002013Cu; return;
        }
        loaded = true;
        trace_ << "[module-ready] AV utility module " << hex32(module) << '\n';
        ctx.gpr[2] = 0;
    });
    const std::array<std::pair<std::uint32_t, const char *>, 6> registry_imports{{
        {0x92E41280u, "sceRegOpenRegistry"}, {0xFA8A5739u, "sceRegCloseRegistry"},
        {0x1D8A762Eu, "sceRegOpenCategory"}, {0x0CAE832Bu, "sceRegCloseCategory"},
        {0xD4475AA8u, "sceRegGetKeyInfo"}, {0x28A8E98Au, "sceRegGetKeyValue"}}};
    for (const auto &[nid, name] : registry_imports) {
        bind("sceReg", nid, name, [this, nid](Context &ctx) {
            registry_.invoke(nid, runtime_, ctx);
            if (!registry_.last_diagnostic().empty())
                trace_ << "[registry-error] " << registry_.last_diagnostic() << '\n';
            else if (nid == 0x1D8A762Eu || nid == 0xD4475AA8u)
                trace_ << "[registry-query] " << string_at(ctx.gpr[5]) << '\n';
        });
    }
    bind("ModuleMgrForUser", 0x977DE386u, "sceKernelLoadModule", [this](Context &ctx) { load_font(ctx); });
    bind("ModuleMgrForUser", 0x50F0C1ECu, "sceKernelStartModule", [this](Context &ctx) {
        if (!font_uid_ || ctx.gpr[4] != static_cast<std::uint32_t>(font_uid_))
            throw Error("Unknown module start UID");
        if (font_started_ || ctx.gpr[5] || ctx.gpr[6] || ctx.gpr[8])
            throw Error("Unsupported font module start state/arguments/options");
        if (ctx.gpr[7] && !runtime_.memory().contains(ctx.gpr[7], 4))
            throw Error("Invalid module start status pointer");
        // This verified library has neither an ELF entry nor a syslib start
        // export. Loading/binding completes its lifecycle; no guest start code
        // exists to skip. Retain the resident module and report its real UID.
        font_started_ = true; font_stopped_ = false;
        if (ctx.gpr[7]) runtime_.memory().store32(ctx.gpr[7], 0);
        ctx.gpr[2] = static_cast<std::uint32_t>(font_uid_);
        trace_ << "[module-started] uid=" << font_uid_ << " no start entry\n";
    });
    const auto font_busy=[this](const Context &call) {
        const auto inside=[this](std::uint32_t address) {
            address=GuestMemory::canonical(address);return address>=font_base_ && address<font_end_;
        };
        if (in_interrupt_context() || inside(call.pc) || inside(call.gpr[31]))return true;
        for (const auto &[uid,thread]:threads_) {
            if (uid==runtime_thread_uid() || thread.state==State::ended || thread.state==State::dormant)continue;
            if (inside(thread.context.pc) || inside(thread.context.gpr[31]))return true;
        }
        return false;
    };
    bind("ModuleMgrForUser", 0xD1FF982Au, "sceKernelStopModule", [this,font_busy](Context &ctx) {
        const auto uid=static_cast<std::int32_t>(ctx.gpr[4]);
        if (!font_uid_ || uid!=font_uid_) {ctx.gpr[2]=0x8002012Eu;return;}
        if (ctx.gpr[5] || ctx.gpr[6] || ctx.gpr[8]) {ctx.gpr[2]=0x800200D2u;return;}
        const auto status=ctx.gpr[7];
        if (status && ((status&3u)||!runtime_.memory().contains(status,4))) {ctx.gpr[2]=0x800200D3u;return;}
        if (!font_started_) {ctx.gpr[2]=font_stopped_?0x80020135u:0x80020134u;return;}
        if (font_busy(ctx)) {ctx.gpr[2]=0x80020136u;return;}
        // This exact verified font has no module_stop entry. Disable its HLE
        // redirects and both direct/chained-PC execution gates before success.
        font_started_=false;font_stopped_=true;
        if (status)runtime_.memory().store32(status,0);
        ctx.gpr[2]=0;
        trace_ << "[module-stopped] uid=" << uid << " no stop entry\n";
    });
    bind("ModuleMgrForUser", 0x2E0911AAu, "sceKernelUnloadModule", [this,font_busy](Context &ctx) {
        const auto uid=static_cast<std::int32_t>(ctx.gpr[4]);
        if (!font_uid_ || uid!=font_uid_) {ctx.gpr[2]=0x8002012Eu;return;}
        if (font_started_) {ctx.gpr[2]=0x80020137u;return;}
        if (font_busy(ctx)) {ctx.gpr[2]=0x80020136u;return;}
        if (font_reserved_) {
            // reserve() carves a non-allocatable interval. Transfer exactly that
            // owned interval to the allocator's existing coalescing release.
            if (!allocations_.emplace(font_base_,font_reservation_size_).second)
                throw Error("Font reservation unexpectedly overlaps a live allocation");
            try { release(font_base_); }
            catch (...) { allocations_.erase(font_base_); throw; }
            font_reserved_=false;
        }
        font_uid_=0;font_gp_=0;font_stopped_=false;ctx.gpr[2]=static_cast<std::uint32_t>(uid);
        trace_ << "[module-unloaded] font uid=" << uid << " reservation released\n";
    });
    bind("ModuleMgrForUser", 0xD8B73127u, "sceKernelGetModuleIdByAddress", [this](Context &ctx) {
        const auto address = GuestMemory::canonical(ctx.gpr[4]);
        if (font_uid_ && address >= font_base_ && address < font_end_) ctx.gpr[2] = font_uid_;
        else if (address >= 0x08804000u && address < image_end_) ctx.gpr[2] = 1;
        else throw Error("Module lookup outside loaded guest images");
    });
    bind("ModuleMgrForUser", 0xF0A26395u, "sceKernelGetModuleId", [this](Context &ctx) {
        const auto caller = GuestMemory::canonical(ctx.gpr[31]);
        ctx.gpr[2] = font_uid_ && caller >= font_base_ && caller < font_end_ ? font_uid_ : 1;
    });
}
}
