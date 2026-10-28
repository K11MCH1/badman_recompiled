#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/sha256.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t base = 0x08C00000u, extent = 0x8254u, reservation = 0x8300u;
constexpr std::uint32_t main_gp = 0x0891F420u, font_gp = 0x08C10190u;
constexpr std::uint32_t font_nid = 0x67F17ED7u, stub = 0x088E9E50u;
constexpr std::uint32_t request = 0x09F00000u, status = request + 0x100u;
constexpr const char *disc_path = "disc0:/PSP_GAME/USRDIR/data_usa/module/libfont.prx";
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template <class Function> void rejects(Function function, const char *message) {
    bool rejected = false;
    try { function(); } catch (const Error &) { rejected = true; }
    require(rejected, message);
}
void put16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8u);
}
void put32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8u));
}
void put_string(std::vector<std::uint8_t> &bytes, std::size_t offset, const std::string &value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
    bytes.at(offset + value.size()) = 0;
}
void guest_string(GuestMemory &memory, std::uint32_t address, const std::string &value) {
    for (std::size_t i = 0; i <= value.size(); ++i)
        memory.store8(address + static_cast<std::uint32_t>(i), i == value.size() ? 0 : value[i]);
}
std::vector<std::uint8_t> font_elf() {
    // Entirely synthetic fixed-address ET_EXEC, with the loader's verified
    // layout contract and 28 synthetic function exports. No commercial bytes.
    std::vector<std::uint8_t> bytes(0x8600, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 1; bytes[5] = 1; bytes[6] = 1;
    put16(bytes, 16, 2); put16(bytes, 18, 8); put32(bytes, 20, 1);
    put32(bytes, 24, 0xFFFFFFFFu); put32(bytes, 28, 52); put32(bytes, 32, 0x8400);
    put16(bytes, 40, 52); put16(bytes, 42, 32); put16(bytes, 44, 1);
    put16(bytes, 46, 40); put16(bytes, 48, 3); put16(bytes, 50, 2);
    put32(bytes, 52, 1); put32(bytes, 56, 0x100); put32(bytes, 60, base);
    put32(bytes, 64, base); put32(bytes, 68, 0x8200); put32(bytes, 72, extent);
    put32(bytes, 76, 7); put32(bytes, 80, 0x100);
    const auto file = [](std::uint32_t relative) { return 0x100u + relative; };
    put32(bytes, file(0), 0xCAFEBABEu);
    put16(bytes, file(0x100), 6); bytes[file(0x102)] = 1; bytes[file(0x103)] = 1;
    put_string(bytes, file(0x104), "sceFont_Library");
    put32(bytes, file(0x120), font_gp); put32(bytes, file(0x124), base + 0x180);
    put32(bytes, file(0x128), base + 0x190);
    put32(bytes, file(0x180), base + 0x300); bytes[file(0x188)] = 4;
    put16(bytes, file(0x18A), 28); put32(bytes, file(0x18C), base + 0x200);
    for (std::uint32_t i = 0; i < 28; ++i) {
        put32(bytes, file(0x200 + i * 4), i == 0 ? font_nid : 0xAB000000u + i);
        put32(bytes, file(0x200 + (28 + i) * 4), base + 0x400 + i * 8);
    }
    put_string(bytes, file(0x300), "sceLibFont");
    const std::string section_names = std::string("\0.rodata.sceModuleInfo\0.shstrtab\0", 33);
    std::copy(section_names.begin(), section_names.end(), bytes.begin() + 0x8500);
    put32(bytes, 0x8428, 1); put32(bytes, 0x842C, 1); put32(bytes, 0x8430, 2);
    put32(bytes, 0x8434, base + 0x100); put32(bytes, 0x8438, file(0x100));
    put32(bytes, 0x843C, 52); put32(bytes, 0x8448, 4);
    put32(bytes, 0x8450, 23); put32(bytes, 0x8454, 3);
    put32(bytes, 0x8460, 0x8500); put32(bytes, 0x8464, static_cast<std::uint32_t>(section_names.size()));
    put32(bytes, 0x8470, 1);
    return bytes;
}
struct Fixture {
    std::filesystem::path parent = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    std::filesystem::path root = parent / ("badman-module-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path prepared = root / "prepared.elf";
    std::filesystem::path source = root / "PSP_GAME/USRDIR/data_usa/module/libfont.prx";
    Fixture() {
        require(std::filesystem::create_directory(root), "Temporary module fixture already exists");
        std::filesystem::create_directories(source.parent_path());
        { std::ofstream out(root / "PSP_GAME/PARAM.SFO", std::ios::binary); out << "synthetic"; }
        { std::ofstream out(source, std::ios::binary); out << "synthetic bundled module"; }
        const auto bytes = font_elf();
        std::ofstream out(prepared, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(out), "Cannot write synthetic module ELF");
    }
    ~Fixture() {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(root, error);
        if (!error && resolved.parent_path() == parent && resolved.filename().string().starts_with("badman-module-test-"))
            std::filesystem::remove_all(resolved, error);
    }
};
std::uint32_t registrations{}, initial_sp{}, initial_tls{};
std::vector<int> flow;
void expect_owner(const AllegrexContext &ctx, std::uint32_t gp) {
    require(ctx.gpr[28] == gp, "Wrong GP at module/main boundary");
    require(runtime_thread_uid() == 1 && ctx.gpr[26] == initial_tls, "Module call changed PSP thread/TLS ownership");
}
void font_resume(Runtime &runtime, AllegrexContext &ctx) {
    expect_owner(ctx, font_gp); flow.push_back(4);
    require(ctx.gpr[2] == 34 && ctx.gpr[29] == initial_sp - 32, "Callback result or module stack was lost");
    runtime.memory().store32(ctx.gpr[28] - 0x8000, ctx.gpr[2] + 1);
    ctx.gpr[2] += 1;
    ctx.gpr[31] = runtime.memory().load32(ctx.gpr[29]); ctx.gpr[29] += 32;
    ctx.pc = ctx.gpr[31];
}
void main_callback(Runtime &runtime, AllegrexContext &ctx) {
    expect_owner(ctx, main_gp); flow.push_back(3);
    require(ctx.gpr[31] == base + 0x404 && ctx.gpr[29] == initial_sp - 32, "Callback return/stack flow is wrong");
    require(runtime.memory().load32(ctx.gpr[29] + 16) == 0xDEADBEEF &&
            runtime.memory().load32(ctx.gpr[29] + 20) == 0xABCDEF01, "Callback lost guest stack canaries");
    ctx.gpr[2] = ctx.gpr[4] + ctx.gpr[5] + ctx.gpr[8] + ctx.gpr[9];
    runtime.memory().store32(ctx.gpr[28] - 0x100, ctx.gpr[2]); ctx.pc = ctx.gpr[31];
}
void font_entry(Runtime &runtime, AllegrexContext &ctx) {
    expect_owner(ctx, font_gp); flow.push_back(2);
    require(ctx.gpr[31] == 0x08804010 && ctx.gpr[29] == initial_sp, "Import forwarding changed caller RA/SP");
    const auto canary_a = runtime.memory().load32(ctx.gpr[29] + 16);
    const auto canary_b = runtime.memory().load32(ctx.gpr[29] + 20);
    ctx.gpr[29] -= 32; runtime.memory().store32(ctx.gpr[29], ctx.gpr[31]);
    runtime.memory().store32(ctx.gpr[29] + 16, canary_a); runtime.memory().store32(ctx.gpr[29] + 20, canary_b);
    ctx.gpr[31] = base + 0x404; ctx.pc = 0x08805000;
    require(!runtime.invoke_chained_call(ctx), "Profile callback unexpectedly became chainable");
}
void unused_export(Runtime &, AllegrexContext &) { throw Error("Unexpected synthetic export execution"); }
void register_font(Runtime &runtime) {
    ++registrations;
    for (std::uint32_t i = 0; i < 28; ++i)
        runtime.register_function(base + 0x400 + i * 8, i == 0 ? font_entry : unused_export, "synthetic_font_code");
    runtime.register_function(base + 0x404, font_resume, "synthetic_font_resume");
}
void main_entry(Runtime &runtime, AllegrexContext &ctx) {
    expect_owner(ctx, main_gp); flow.push_back(1);
    initial_sp = ctx.gpr[29];
    runtime.memory().store32(initial_sp + 16, 0xDEADBEEF); runtime.memory().store32(initial_sp + 20, 0xABCDEF01);
    ctx.gpr[4] = 3; ctx.gpr[5] = 7; ctx.gpr[8] = 11; ctx.gpr[9] = 13; // Allegrex arguments five/six use t0/t1.
    ctx.gpr[31] = 0x08804010; ctx.pc = stub;
}
void import_wrapper(Runtime &runtime, AllegrexContext &ctx) {
    const auto token = capture_runtime_execution_context();
    const auto before = ctx.pc, returned = ctx.gpr[31];
    runtime.invoke_import_cached(132, "sceLibFont", font_nid, ctx);
    if (!runtime.stopped() && runtime_execution_context_matches(token) && ctx.pc == before) ctx.pc = returned;
}
void main_return(Runtime &runtime, AllegrexContext &ctx) {
    expect_owner(ctx, main_gp); flow.push_back(5);
    require(ctx.gpr[2] == 35 && ctx.gpr[29] == initial_sp && ctx.gpr[31] == 0x08804010,
            "Module return did not preserve result, stack, or original RA");
    require(runtime.memory().load32(initial_sp + 16) == 0xDEADBEEF &&
            runtime.memory().load32(initial_sp + 20) == 0xABCDEF01, "Module call corrupted caller stack canaries");
    require(runtime.memory().load32(main_gp - 0x100) == 34 && runtime.memory().load32(font_gp - 0x8000) == 35,
            "Module/main GP-relative writes did not persist");
    runtime.stop("Synthetic module callback returned");
}
void activate(Runtime &runtime) {
    auto &ctx = runtime.cpu(); guest_string(runtime.memory(), request, "disc0:");
    ctx.gpr[4] = 1; ctx.gpr[5] = request;
    runtime.invoke_import("sceUmdUser", 0xC6183D47, ctx);
}
void load(Runtime &runtime) {
    auto &ctx = runtime.cpu(); guest_string(runtime.memory(), request, disc_path);
    ctx.pc = 0x088E9AF8; ctx.gpr[4] = request; ctx.gpr[5] = 0; ctx.gpr[6] = 0;
    runtime.invoke_import("ModuleMgrForUser", 0x977DE386, ctx);
}
void reservation_and_hash_guards() {
    Fixture fixture; Runtime runtime; runtime.set_game_root(fixture.root); std::ostringstream trace;
    badman::Host host(runtime, 0x08B00000, 0x08A00000, main_gp, trace);
    const auto available = host.free_bytes(); registrations = 0;
    runtime.memory().store32(base, 0x12345678);
    rejects([&] { host.configure_font_module(fixture.prepared, std::string(64, '0'), sha256_file(fixture.source), register_font); },
            "Incorrect prepared hash was accepted");
    require(host.free_bytes() == available && registrations == 0 && runtime.memory().load32(base) == 0x12345678,
            "Prepared hash failure changed allocator/RAM or registered code");
    host.configure_font_module(fixture.prepared, sha256_file(fixture.prepared), sha256_file(fixture.source), register_font);
    require(host.free_bytes() == available - reservation && !host.font_loaded() && registrations == 0,
            "Configuration did not reserve exactly the module extent without loading code");
    const auto prefix = host.allocate(base - 0x08B00000);
    const auto suffix = host.allocate(256);
    require(prefix == 0x08B00000 && suffix == base + reservation, "Allocator crossed the reserved module interval");
    const auto after_allocating = host.free_bytes();
    rejects([&] { host.reserve(base, reservation); }, "A module reservation overlapped an existing reservation");
    rejects([&] { host.reserve(suffix, 256); }, "A module reservation overlapped a live allocation");
    require(host.free_bytes() == after_allocating, "Rejected reservation changed free memory");
    host.release(prefix); host.release(suffix);
    require(host.free_bytes() == available - reservation, "Allocation release reclaimed reserved module RAM");
    host.install({}); activate(runtime);
    { std::ofstream out(fixture.source, std::ios::binary | std::ios::trunc); out << "tampered source"; }
    rejects([&] { load(runtime); }, "Incorrect bundled source hash was accepted");
    require(!host.font_loaded() && registrations == 0 && !runtime.has_function(base + 0x400) &&
            runtime.memory().load32(base) == 0x12345678, "Source hash failure wrote RAM or registered code");
}
void lifecycle_and_callback_flow() {
    Fixture fixture; Runtime runtime; runtime.set_game_root(fixture.root); std::ostringstream trace;
    badman::Host host(runtime, 0x08B00000, 0x08A00000, main_gp, trace);
    registrations = 0; flow.clear();
    host.configure_font_module(fixture.prepared, sha256_file(fixture.prepared), sha256_file(fixture.source), register_font);
    host.install({{"sceLibFont", font_nid, stub}});
    auto &ctx = runtime.cpu(); ctx.pc = stub;
    runtime.invoke_import("sceLibFont", font_nid, ctx);
    require(runtime.stopped() && registrations == 0 && !host.font_loaded() && flow.empty(), "Unloaded font export executed");
    activate(runtime);
    runtime.memory().store32(base + 0x8240, 0xDEADBEEF);
    load(runtime); const auto uid = ctx.gpr[2];
    require(uid >= 16 && host.font_loaded() && !host.font_started() && registrations == 1 &&
            runtime.memory().load32(base) == 0xCAFEBABE && runtime.has_function(base + 0x400), "Module load did not load/bind real fixture state");
    require(runtime.memory().load32(base + 0x8240) == 0, "Module BSS was not zero initialized");
    rejects([&] { runtime.invoke_import("sceLibFont", font_nid, ctx); }, "Unstarted font export executed");
    require(flow.empty(), "Unstarted font call reached registered code");
    runtime.memory().store32(status, 0xDEADBEEF);
    ctx.gpr[4] = uid + 1; ctx.gpr[5] = 0; ctx.gpr[6] = 0; ctx.gpr[7] = status; ctx.gpr[8] = 0;
    rejects([&] { runtime.invoke_import("ModuleMgrForUser", 0x50F0C1EC, ctx); }, "Wrong module UID was started");
    require(!host.font_started() && runtime.memory().load32(status) == 0xDEADBEEF, "Rejected start changed status/lifecycle");
    ctx.gpr[4] = uid; ctx.gpr[7] = 0x01000000;
    rejects([&] { runtime.invoke_import("ModuleMgrForUser", 0x50F0C1EC, ctx); }, "Invalid start status pointer was accepted");
    require(!host.font_started(), "Failed status validation started the module");
    ctx.gpr[7] = status; runtime.invoke_import("ModuleMgrForUser", 0x50F0C1EC, ctx);
    require(host.font_started() && ctx.gpr[2] == uid && runtime.memory().load32(status) == 0,
            "No-entry module start did not return its UID and write status zero");
    rejects([&] { runtime.invoke_import("ModuleMgrForUser", 0x50F0C1EC, ctx); }, "Repeated module start was accepted");
    rejects([&] { load(runtime); }, "Repeated module load was accepted");
    require(registrations == 1, "Repeated module operation re-registered code");
    ctx.gpr[4] = base + 0x400; runtime.invoke_import("ModuleMgrForUser", 0xD8B73127, ctx);
    require(ctx.gpr[2] == uid, "Loaded module address did not resolve to its UID");
    runtime.register_function(0x08804000, main_entry, "synthetic_main_entry");
    runtime.register_function(0x08804010, main_return, "synthetic_main_return");
    runtime.register_function(0x08805000, main_callback, "synthetic_main_callback");
    runtime.register_function(stub, import_wrapper, "synthetic_import");
    host.bootstrap(0x08804000, "test"); initial_tls = ctx.gpr[26]; ctx.gpr[28] = 0xBAD;
    runtime.run(0x08804000, 20);
    require(runtime.stop_reason() == "Synthetic module callback returned" && flow == std::vector<int>({1, 2, 3, 4, 5}),
            "Main/font/callback/continuation dispatch flow did not finish in order");
}
struct ReadinessDecoder final : badman::AtracDecoder {
    explicit ReadinessDecoder(unsigned &live) : live_(live) { ++live_; }
    ~ReadinessDecoder() override { --live_; }
    std::vector<std::int16_t> decode(const badman::AtracRiff &, std::span<const std::uint8_t>) override {
        throw Error("Module readiness must not decode a stream");
    }
    unsigned &live_;
};
void utility_module_readiness_and_retry() {
    unsigned attempts = 0, live = 0; // Counters outlive the Host's retained decoder.
    Runtime runtime; std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000u, 0x089E0000u, main_gp, trace);
    host.install({});
    auto &ctx = runtime.cpu();
    const auto load_av = [&](std::uint32_t module) {
        ctx.gpr[4] = module; runtime.invoke_import("sceUtility", 0x2A2B3DE0u, ctx); return ctx.gpr[2];
    };
    require(load_av(0x300) == 0x8002013Cu && load_av(0x302) == 0x8002013Cu,
        "Unavailable codec backend was reported loaded");
    require(load_av(0x301) == 0 && load_av(0x301) == 0x80111102u,
        "Implemented SAS configuration module lost duplicate-load state");
    host.set_atrac_decoder_factory([&]() -> std::unique_ptr<badman::AtracDecoder> {
        if (++attempts == 1) throw Error("Synthetic unavailable codec");
        return std::make_unique<ReadinessDecoder>(live);
    });
    require(load_av(0x300) == 0x8002013Cu && attempts == 1 && live == 0,
        "Failed module preparation published readiness or retained a failed decoder");
    require(load_av(0x300) == 0 && attempts == 2 && live == 1,
        "Module preparation did not retry or retain its real backend");
    require(load_av(0x300) == 0x80111102u && load_av(0x302) == 0 &&
        load_av(0x302) == 0x80111102u && attempts == 2 && live == 1,
        "Successful module loads rebuilt readiness or lost duplicate-load state");
    for (std::uint32_t module = 0x303; module <= 0x307; ++module)
        require(load_av(module) == 0x8002013Cu, "Unsupported AV module claimed readiness");
    runtime.memory().store32(status, 0xDEADBEEF);
    ctx.gpr[4] = 0; ctx.gpr[5] = status;
    runtime.invoke_import("sceAtrac3plus", 0xD6A5F2F7u, ctx);
    require(ctx.gpr[2] == 0x80630005u && runtime.memory().load32(status) == 0xDEADBEEF,
        "Module readiness consumed an ATRAC stream ID or changed an invalid-ID output");
    rejects([&] { host.set_atrac_decoder_factory({}); },
        "A loaded codec module lost its retained backend through reconfiguration");
    require(live == 1, "Rejected decoder reconfiguration released a loaded backend");
}
void stop_unload_reload_and_arena() {
    Fixture fixture;Runtime runtime;runtime.set_game_root(fixture.root);std::ostringstream trace;
    badman::Host host(runtime,0x08B00000,0x08A00000,main_gp,trace);
    const auto available=host.free_bytes();registrations=0;
    host.configure_font_module(fixture.prepared,sha256_file(fixture.prepared),sha256_file(fixture.source),register_font);
    host.install({});activate(runtime);load(runtime);const auto uid=runtime.cpu().gpr[2];
    auto &ctx=runtime.cpu();
    const auto blocked_pc=[&](auto operation) {
        bool blocked=false;
        try {operation();} catch(const Error &error) {
            blocked=std::string(error.what()).find("Execution in a stopped or unloaded font module")!=std::string::npos;
        }
        require(blocked,"Inactive font execution did not reach the module PC guard");
    };
    const auto call=[&](std::uint32_t nid,std::uint32_t module,std::uint32_t output=status){
        ctx.pc=0x08804020;ctx.gpr[31]=0x08804024;ctx.gpr[4]=module;ctx.gpr[5]=ctx.gpr[6]=ctx.gpr[8]=0;ctx.gpr[7]=output;
        runtime.invoke_import("ModuleMgrForUser",nid,ctx);return ctx.gpr[2];
    };
    runtime.memory().store32(status,0xDEADBEEF);
    require(call(0xD1FF982A,uid+1)==0x8002012E,"Invalid stop UID was not rejected");
    require(call(0xD1FF982A,uid)==0x80020134,"Never-started module stopped successfully");
    require(runtime.memory().load32(status)==0xDEADBEEF,"Rejected stop changed output");
    call(0x50F0C1EC,uid);require(host.font_started(),"Fixture start failed");
    runtime.memory().store32(status,0xDEADBEEF);
    require(call(0x2E0911AA,uid)==0x80020137&&host.font_loaded(),"Running module unloaded");
    require(call(0xD1FF982A,uid,0x01000000)==0x800200D3&&host.font_started(),"Invalid status pointer stopped module");
    ctx.pc=0x08804020;ctx.gpr[31]=base+0x400;ctx.gpr[4]=uid;ctx.gpr[5]=ctx.gpr[6]=ctx.gpr[8]=0;ctx.gpr[7]=status;
    runtime.invoke_import("ModuleMgrForUser",0xD1FF982A,ctx);
    require(ctx.gpr[2]==0x80020136&&host.font_started()&&runtime.memory().load32(status)==0xDEADBEEF,"Active module return address was invalidated");
    require(call(0xD1FF982A,uid)==0&&!host.font_started()&&runtime.memory().load32(status)==0,"No-entry stop did not change lifecycle/status");
    require(call(0xD1FF982A,uid)==0x80020135,"Repeated stop invented another stop");
    rejects([&]{runtime.invoke_import("sceLibFont",font_nid,ctx);},"Stopped module HLE export executed");
    blocked_pc([&]{runtime.run(base+0x400,1);});
    // Explicitly exercise the actual chained-code hook, not the non-chainable
    // synthetic export registration used by the import callback-flow fixture.
    runtime.register_function(base+0x400,font_entry,"recomp_unit_synthetic_font");
    ctx.pc=base+0x400;
    blocked_pc([&]{(void)runtime.invoke_chained_call(ctx);});
    require(call(0x2E0911AA,uid)==uid&&!host.font_loaded()&&host.free_bytes()==available,"Unload did not release exact arena ownership");
    require(call(0x2E0911AA,uid)==0x8002012E,"Old unloaded UID remained valid");
    blocked_pc([&]{runtime.run(base+0x400,1);});
    const auto prefix=host.allocate(base-0x08B00000);const auto occupying=host.allocate(reservation);
    require(occupying==base,"Released font arena could not be reused");
    runtime.memory().store32(base,0xBADC0FFE);load(runtime);
    require(ctx.gpr[2]==0x80020131&&!host.font_loaded()&&runtime.memory().load32(base)==0xBADC0FFE,"Reload clobbered another arena allocation");
    host.release(occupying);host.release(prefix);load(runtime);const auto next_uid=ctx.gpr[2];
    require(next_uid!=uid&&host.font_loaded()&&runtime.memory().load32(base)==0xCAFEBABE&&host.free_bytes()==available-reservation,"Reload did not re-reserve/load fresh module");
    call(0x50F0C1EC,next_uid);require(host.font_started(),"Reloaded module did not start");
    require(call(0xD1FF982A,uid)==0x8002012E,"Old UID targeted newly loaded module");
    require(call(0xD1FF982A,next_uid)==0,"Reloaded module did not stop");
    require(call(0x2E0911AA,next_uid)==next_uid&&host.free_bytes()==available,"Repeated lifecycle leaked arena");
}
struct LifetimeDecoder final:badman::AtracDecoder {
    unsigned &live;explicit LifetimeDecoder(unsigned &count):live(count){++live;}
    ~LifetimeDecoder()override{--live;}
    std::vector<std::int16_t> decode(const badman::AtracRiff &info,std::span<const std::uint8_t> coded)override {
        return std::vector<std::int16_t>(coded.size()/info.block_align*badman::Atrac::frame_samples*2);
    }
};
struct EmptyVideo final:badman::VideoDecoder {
    std::vector<badman::EncodedVideo>parse(std::span<const std::uint8_t>,std::int64_t,std::int64_t,bool)override{return {};}
    std::optional<badman::VideoFrame>decode(const badman::EncodedVideo&)override{return {};}
    std::optional<badman::VideoFrame>drain()override{return {};}
    void reset()override{}
};
struct EmptyTransport final:badman::AudioTransport {
    void open(std::uint32_t)override{}
    void close(std::uint32_t)noexcept override{}
    void submit(std::uint32_t,std::uint64_t,std::span<const std::int16_t>)override{}
    std::vector<badman::AudioProgress>poll()override{return {};}
};
std::vector<std::uint8_t> synthetic_atrac() {
    std::vector<std::uint8_t> bytes(12);std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVE",4);
    const auto chunk=[&](const char *name,const std::vector<std::uint8_t>&data){
        const auto at=bytes.size();bytes.resize(at+8+data.size());std::memcpy(bytes.data()+at,name,4);
        put32(bytes,at+4,static_cast<std::uint32_t>(data.size()));std::copy(data.begin(),data.end(),bytes.begin()+at+8);
    };
    std::vector<std::uint8_t> format(52);put16(format,0,0xFFFE);put16(format,2,2);put32(format,4,44100);put32(format,8,172);
    put16(format,12,8);put16(format,16,34);put16(format,18,2048);put32(format,20,3);
    const std::array<std::uint8_t,16> guid{0xBF,0xAA,0x23,0xE9,0x58,0xCB,0x71,0x44,0xA1,0x19,0xFF,0xFA,0x01,0xE4,0xCE,0x62};
    std::copy(guid.begin(),guid.end(),format.begin()+24);chunk("fmt ",format);
    std::vector<std::uint8_t> fact(8);put32(fact,0,5000);put32(fact,4,2048);chunk("fact",fact);chunk("data",std::vector<std::uint8_t>(32));
    put32(bytes,4,static_cast<std::uint32_t>(bytes.size()-8));return bytes;
}
void utility_unload_real_resources_and_retry() {
    unsigned live=0;Runtime runtime;std::ostringstream trace;
    badman::Host host(runtime,0x089F0000,0x089E0000,main_gp,trace);host.install({});
    host.set_atrac_decoder_factory([&]{return std::make_unique<LifetimeDecoder>(live);});
    host.set_video_decoder_factory([]{return std::make_unique<EmptyVideo>();});
    auto &ctx=runtime.cpu();const auto utility=[&](bool load,std::uint32_t module){ctx.gpr[4]=module;runtime.invoke_import("sceUtility",load?0x2A2B3DE0:0xE49BFE92,ctx);return ctx.gpr[2];};
    require(utility(false,0x302)!=0&&live==0,"Unloaded utility claimed a successful release");
    require(utility(true,0x300)==0&&utility(true,0x302)==0&&live==1,"Shared codec readiness was not retained");
    require(utility(false,0x302)==0&&live==1,"First owner dropped the other module's decoder");
    require(utility(false,0x300)==0&&live==0,"Last codec owner retained its cached decoder/DLL");
    require(utility(true,0x300)==0&&live==1,"Unloaded codec could not reopen its retained factory");
    const auto riff=synthetic_atrac();runtime.memory().copy_in(request,riff);ctx.gpr[4]=request;ctx.gpr[5]=static_cast<std::uint32_t>(riff.size());
    runtime.invoke_import("sceAtrac3plus",0x7A20E7AF,ctx);const auto id=ctx.gpr[2];require(id==0,"Synthetic active ATRAC stream failed");
    require(utility(false,0x300)==0x80020143&&live==1&&utility(true,0x300)==0x80111102,"Active ATRAC stream was invalidated or flag cleared");
    ctx.gpr[4]=id;runtime.invoke_import("sceAtrac3plus",0x61EB33F5,ctx);require(ctx.gpr[2]==0,"Synthetic stream release failed");
    runtime.invoke_import("sceMpeg",0x682A619B,ctx);require(ctx.gpr[2]==0,"Synthetic MPEG initialization failed");
    require(utility(false,0x300)==0x80020143&&live==1,"Active MPEG lifecycle allowed codec unload");
    runtime.invoke_import("sceMpeg",0x874624D6,ctx);require(utility(false,0x300)==0&&live==0,"Completed media lifecycle could not unload");
    require(utility(true,0x301)==0,"SAS utility failed to load");
    constexpr std::uint32_t core=0x09E00000;ctx.gpr[4]=core;ctx.gpr[5]=256;ctx.gpr[6]=32;ctx.gpr[7]=0;ctx.gpr[8]=44100;
    runtime.invoke_import("sceSasCore",0x42778A9F,ctx);require(ctx.gpr[2]==0&&host.sas_configuration(core).has_value(),"SAS configuration fixture failed");
    host.configure_audio_transport(std::make_unique<EmptyTransport>());ctx.gpr[4]=0;ctx.gpr[5]=64;ctx.gpr[6]=0;
    runtime.invoke_import("sceAudio",0x5EC81C55,ctx);require(ctx.gpr[2]==0,"Synthetic PCM reserve failed");
    require(utility(false,0x301)==0x80020143&&host.sas_configuration(core).has_value(),"Busy PCM/SAS resources were silently discarded");
    ctx.gpr[4]=0;runtime.invoke_import("sceAudio",0x6FC46853,ctx);require(ctx.gpr[2]==0,"Synthetic PCM release failed");
    require(utility(false,0x301)==0&&!host.sas_configuration(core).has_value(),"SAS unload retained owned configuration");
    require(utility(true,0x301)==0,"SAS module could not reload");
}
}
int main() {
    try {
        reservation_and_hash_guards(); lifecycle_and_callback_flow(); utility_module_readiness_and_retry();
        stop_unload_reload_and_arena(); utility_unload_real_resources_and_retry();
        std::cout << "Badman module reservation, identity, lifecycle and GP/callback tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
