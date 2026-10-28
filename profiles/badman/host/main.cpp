#include "badman_host.hpp"
#include "badman_capture_file.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/sha256.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(BADMAN_GAME_LIBRARY)
#include <dlfcn.h>
#else
namespace psprecomp { void register_badman_font_functions(Runtime &); }
#endif

namespace {
#if defined(BADMAN_GAME_LIBRARY)
// The locally generated translation lives in its own library; resolve its
// registration entry points by name. Only that library is built from the ISO.
struct GameLibrary { badman::Host::CodeRegistrar main{}, font{}; };
GameLibrary load_game_library() {
    const auto path = std::getenv("BADMAN_GAME_LIBRARY");
    if (path == nullptr || *path == 0) throw psprecomp::Error("BADMAN_GAME_LIBRARY must name the locally built game library");
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw psprecomp::Error(std::string("Cannot load game library: ") + dlerror());
    const auto version = reinterpret_cast<int (*)()>(dlsym(handle, "badman_game_abi_version"));
    GameLibrary game{reinterpret_cast<badman::Host::CodeRegistrar>(dlsym(handle, "badman_register_generated_functions")),
                     reinterpret_cast<badman::Host::CodeRegistrar>(dlsym(handle, "badman_register_font_functions"))};
    if (!version || version() != 1 || !game.main || !game.font) throw psprecomp::Error("Game library entry points are missing or incompatible");
    return game; // Kept loaded for the process lifetime.
}
#endif
std::filesystem::path executable_directory(const char *argument) {
#if defined(_WIN32)
    (void)argument;
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw psprecomp::Error("Cannot locate the native executable directory");
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
#else
    return std::filesystem::weakly_canonical(argument).parent_path();
#endif
}
std::filesystem::path prepare_memory_stick(const std::filesystem::path &data_root) {
    auto directory = std::filesystem::canonical(data_root);
    std::filesystem::path card;
    for (const auto *part : {".badman-state", "memory-stick", "PSP", "SAVEDATA"}) {
        directory /= part;
        std::error_code error;
        auto status = std::filesystem::symlink_status(directory, error);
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (error) throw std::filesystem::filesystem_error("Cannot inspect Memory Stick directory", directory, error);
        if (!std::filesystem::exists(status)) std::filesystem::create_directory(directory);
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(directory) ||
            std::filesystem::canonical(directory) != directory)
            throw psprecomp::Error("Memory Stick backing path contains a link or non-directory");
        if (std::string_view(part) == "memory-stick") card = directory;
    }
    return card;
}
// The launcher's owned Saves folder inside the prepared data root. Only the
// exact folder with its ownership marker is mounted; nothing is adopted.
std::filesystem::path prepare_saves(const std::filesystem::path &data_root, const std::filesystem::path &requested) {
    const auto saves = std::filesystem::canonical(data_root) / "Saves";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(saves, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status) ||
        std::filesystem::canonical(saves) != saves || std::filesystem::weakly_canonical(requested) != saves)
        throw psprecomp::Error("BADMAN_SAVES_DIRECTORY must be the prepared data folder's own Saves directory");
    std::ifstream marker(saves / ".badman-saves-owner", std::ios::binary);
    const std::string owner((std::istreambuf_iterator<char>(marker)), {});
    if (owner != "PSPRecomp Badman player storage\nschema=1\n")
        throw psprecomp::Error("The Saves folder ownership marker is missing or unrecognized; existing data was preserved");
    auto directory = saves;
    for (const auto *part : {"PSP", "SAVEDATA"}) {
        directory /= part;
        const auto child = std::filesystem::symlink_status(directory, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw std::filesystem::filesystem_error("Cannot inspect Saves directory", directory, error);
        error.clear();
        if (!std::filesystem::exists(child)) std::filesystem::create_directory(directory);
        else if (std::filesystem::is_symlink(child) || !std::filesystem::is_directory(child))
            throw psprecomp::Error("Saves folder contains a link or non-directory");
    }
    return saves;
}
bool environment_flag(const char *name, bool fallback) {
    const auto value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    const std::string_view text(value);
    if (text == "1") return true;
    if (text == "0") return false;
    throw psprecomp::Error(std::string(name) + " must be 0 or 1");
}
badman::Input::Provider scripted_input(const char *path, std::ostream &trace) {
    std::ifstream source(path);
    if (!source) throw psprecomp::Error("Cannot open diagnostic input script");
    std::vector<std::pair<std::uint64_t, std::uint32_t>> events;
    std::string line;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string milliseconds, mask, extra;
        if (!(row >> milliseconds >> mask) || (row >> extra))
            throw psprecomp::Error("Input script requires: milliseconds hexadecimal-button-mask");
        std::uint64_t time = 0;
        std::uint32_t buttons = 0;
        const auto a = std::from_chars(milliseconds.data(), milliseconds.data() + milliseconds.size(), time);
        const auto b = std::from_chars(mask.data(), mask.data() + mask.size(), buttons, 16);
        if (a.ec != std::errc{} || a.ptr != milliseconds.data() + milliseconds.size() ||
            b.ec != std::errc{} || b.ptr != mask.data() + mask.size() || time > 600000 ||
            (buttons & ~0xF3F9u) || events.size() >= 128 ||
            (events.empty() ? time != 0 : time <= events.back().first))
            throw psprecomp::Error("Invalid diagnostic input script time, button mask or event count");
        events.emplace_back(time, buttons);
    }
    if (!source.eof() || events.empty()) throw psprecomp::Error("Empty or unreadable diagnostic input script");
    return [events = std::move(events), origin = std::optional<std::chrono::steady_clock::time_point>{},
            index = std::size_t{0}, reported = std::optional<std::size_t>{}, &trace]() mutable {
        const auto now = std::chrono::steady_clock::now();
        if (!origin) origin = now;
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - *origin).count());
        while (index + 1 < events.size() && elapsed >= events[index + 1].first) ++index;
        if (!reported || *reported != index) {
            trace << "[input-script-state] elapsed_ms=" << elapsed << " buttons="
                  << psprecomp::hex32(events[index].second) << '\n';
            reported = index;
        }
        return badman::InputState{true, events[index].second, 128, 128};
    };
}
std::string live_command(const std::filesystem::path &file) {
#if defined(_WIN32)
    // Keep either complete version readable while the diagnostic writer replaces
    // its name. CRT streams do not request FILE_SHARE_DELETE on Windows.
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        handle = CreateFileW(file.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED &&
            error != ERROR_FILE_NOT_FOUND && error != ERROR_DELETE_PENDING) break;
        if (attempt != 3) Sleep(1);
    }
    if (handle == INVALID_HANDLE_VALUE)
        throw psprecomp::Error("Cannot open live diagnostic controller: Windows error " + std::to_string(error));
    std::array<char, 160> bytes{};
    DWORD count{};
    const bool read = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != 0;
    const auto read_error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!read) throw psprecomp::Error("Cannot read live diagnostic controller: Windows error " + std::to_string(read_error));
    if (!count || count == bytes.size())
        throw psprecomp::Error("Live diagnostic controller requires one bounded command line");
    return std::string(bytes.data(), count);
#else
    std::ifstream input(file, std::ios::binary);
    std::array<char, 160> bytes{};
    input.read(bytes.data(), bytes.size());
    const auto count = input.gcount();
    if (!input.eof() || count <= 0 || count == static_cast<std::streamsize>(bytes.size()))
        throw psprecomp::Error("Live diagnostic controller requires one bounded command line");
    return std::string(bytes.data(), static_cast<std::size_t>(count));
#endif
}
badman::Input::Provider live_input(const char *path, std::ostream &trace,
    const psprecomp::GuestMemory &memory, std::vector<std::uint32_t> watch) {
    // Explicit diagnostic controller: sequence, hexadecimal buttons, pulse ms,
    // optionally the analog nub's decimal x and y (0-255) for the same pulse.
    // A pulse always releases itself, even if the external test driver stops.
    return [file = std::filesystem::path(path), sequence = std::optional<std::uint64_t>{},
            buttons = std::uint32_t{}, analog = std::array<std::uint8_t, 2>{128, 128},
            release = std::chrono::steady_clock::time_point{},
            watch = std::move(watch), &memory, &trace]() mutable {
        const auto command = live_command(file);
        if (command.find('\0') != std::string::npos)
            throw psprecomp::Error("Live diagnostic controller cannot contain NUL bytes");
        std::istringstream input(command);
        std::array<char,160> line{};
        if (!input.getline(line.data(), line.size()) || input.peek() != EOF)
            throw psprecomp::Error("Live diagnostic controller requires one bounded command line");
        std::istringstream row(line.data());
        std::string seq, mask, duration, nub_x, nub_y, extra;
        if (!(row >> seq >> mask >> duration) || (row >> nub_x && (!(row >> nub_y) || row >> extra)))
            throw psprecomp::Error("Live diagnostic command requires sequence, buttons, pulse milliseconds[, analog x y]");
        std::array<unsigned, 2> requested_nub{128, 128};
        for (unsigned axis = 0; axis < 2 && !nub_x.empty(); ++axis) {
            const auto &text = axis ? nub_y : nub_x;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), requested_nub[axis]);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || requested_nub[axis] > 255)
                throw psprecomp::Error("Invalid live diagnostic analog position");
        }
        std::uint64_t next{}; std::uint32_t requested{}, milliseconds{};
        const auto a = std::from_chars(seq.data(), seq.data()+seq.size(), next);
        const auto b = std::from_chars(mask.data(), mask.data()+mask.size(), requested, 16);
        const auto c = std::from_chars(duration.data(), duration.data()+duration.size(), milliseconds);
        if (a.ec != std::errc{} || a.ptr != seq.data()+seq.size() ||
            b.ec != std::errc{} || b.ptr != mask.data()+mask.size() ||
            c.ec != std::errc{} || c.ptr != duration.data()+duration.size() ||
            (requested & ~0xF3F9u) || milliseconds > 5000 || ((requested || !nub_x.empty()) && !milliseconds) ||
            (!sequence && (next || requested || milliseconds)) || (sequence && next < *sequence))
            throw psprecomp::Error("Invalid or replayed live diagnostic controller command");
        const auto now = std::chrono::steady_clock::now();
        if (!sequence || next != *sequence) {
            sequence = next; buttons = requested; release = now + std::chrono::milliseconds(milliseconds);
            analog = {static_cast<std::uint8_t>(requested_nub[0]), static_cast<std::uint8_t>(requested_nub[1])};
            trace << "[input-live] sequence=" << next << " buttons=" << psprecomp::hex32(buttons)
                  << " milliseconds=" << milliseconds << " analog=" << int(analog[0]) << "," << int(analog[1]) << std::endl;
            for (const auto address : watch)
                trace << "[input-watch] sequence=" << next << " address=" << psprecomp::hex32(address)
                      << " value=" << psprecomp::hex32(memory.load32(address)) << std::endl;
        }
        const bool held = now < release;
        return badman::InputState{true, held ? buttons : 0u, held ? analog[0] : std::uint8_t{128}, held ? analog[1] : std::uint8_t{128}};
    };
}
void dump_framebuffer(const psprecomp::GuestMemory &memory, const badman::Host::Display &display,
                      const char *path) {
    if (!display.current.address) throw psprecomp::Error("No display framebuffer is latched for capture");
    const auto &frame = display.current;
    const auto pixels = badman::decode_framebuffer_bgra(memory, frame.address, frame.stride,
        frame.format, display.width, display.height);
    std::ofstream output(path, std::ios::binary);
    if (!output) throw psprecomp::Error("Cannot create diagnostic framebuffer capture");
    const auto word = [&](std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) output.put(static_cast<char>(value >> (i * 8)));
    };
    // A top-down, uncompressed 32-bit BMP containing only latched guest VRAM.
    output.write("BM", 2); word(54 + display.width * display.height * 4, 4);
    word(0, 4); word(54, 4); word(40, 4); word(display.width, 4);
    word(0u - display.height, 4); word(1, 2); word(32, 2); word(0, 4);
    word(display.width * display.height * 4, 4); word(0, 4); word(0, 4); word(0, 4); word(0, 4);
    for (const auto pixel : pixels) word(pixel | 0xFF000000u, 4);
    output.close();
    if (!output) throw psprecomp::Error("Cannot write diagnostic framebuffer capture");
}
void publish_live_frame(const psprecomp::GuestMemory &memory, const badman::Host::Display &display,
                        const std::string &path) {
    const std::filesystem::path destination(path);
    const auto temporary = path + ".pending-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    dump_framebuffer(memory, display, temporary.c_str());
#if defined(_WIN32)
    const auto error = badman::replace_capture_file(std::filesystem::path(temporary), destination);
    if (error != ERROR_SUCCESS) {
        DeleteFileW(std::filesystem::path(temporary).c_str()); // Only this call's newly created capture.
        if (error == ERROR_SHARING_VIOLATION) {
            std::cerr << "[live-frame-deferred] error=" << error << " (previous capture retained)\n";
            return;
        }
        throw psprecomp::Error("Cannot atomically publish the live diagnostic frame (MoveFileExW error " + std::to_string(error) + ")");
    }
#else
    std::filesystem::rename(temporary, destination);
#endif
}
}

#if defined(BADMAN_EMBEDDED_MAIN)
// The Android app runs this on its game thread with the same arguments.
int badman_native_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc < 3 || argc > 7) {
            std::cerr << "Usage: BadmanNative <decrypted ELF> <extracted ISO root> [dispatch limit] [trace file] [PGF directory] [prepared font ELF]\n";
            return 2;
        }
        if (psprecomp::sha256_file(argv[1]) != BADMAN_EXPECTED_ELF_SHA256)
            throw psprecomp::Error("Unsupported executable hash; expected ULUS10456 1.01");
        std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
        if (argc >= 4) {
            const std::string_view text = argv[3];
            const auto result = std::from_chars(text.data(), text.data() + text.size(), limit);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || limit == 0)
                throw psprecomp::Error("Invalid dispatch limit");
        }
        std::ofstream trace_file;
        if (argc >= 5) {
            trace_file.open(argv[4]);
            if (!trace_file) throw psprecomp::Error("Cannot open trace file");
        }
        std::ostream &trace = argc >= 5 ? trace_file : std::cout;
        auto runtime = std::make_unique<psprecomp::Runtime>();
        runtime->set_game_root(argv[2]);
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        const auto relocations = elf.load_and_relocate(runtime->memory());
        if (relocations.invalid || relocations.unsupported)
            throw psprecomp::Error("Unresolved executable relocations");
        const auto module = elf.find_module_info(runtime->memory());
        if (!module || module->name != "yuusha") throw psprecomp::Error("Unexpected PSP module");
        std::uint64_t end = 0;
        for (const auto &segment : elf.segments())
            if (segment.type == 1) end = std::max(end, static_cast<std::uint64_t>(segment.vaddr) + segment.memory_size);
        if (end >= 0x09FF0000u) throw psprecomp::Error("Invalid guest image extent");
#if defined(BADMAN_GAME_LIBRARY)
        const auto game = load_game_library();
        game.main(*runtime);
#else
        psprecomp::register_generated_functions(*runtime);
#endif
        badman::Host host(*runtime, static_cast<std::uint32_t>(end), static_cast<std::uint32_t>(end), module->gp, trace);
        std::cout << "Rendering backend: " << host.ge_state().renderer_description() << '\n';
        host.configure_font_module(argc >= 7 ? argv[6] : BADMAN_FONT_ELF_PATH, BADMAN_FONT_SHA256,
            BADMAN_FONT_SOURCE_SHA256,
#if defined(BADMAN_GAME_LIBRARY)
            game.font);
#else
            &psprecomp::register_badman_font_functions);
#endif
        const auto saves_request = std::getenv("BADMAN_SAVES_DIRECTORY");
        const bool player_saves = saves_request != nullptr && *saves_request != '\0';
        host.configure_memory_stick(player_saves ? prepare_saves(argv[2], saves_request) : prepare_memory_stick(argv[2]));
        // The launcher's Saves folder is one automatic profile: no slot chooser
        // and no boot Memory Stick prompt. Developer runs keep the full UI.
        const bool automatic_profile = environment_flag("BADMAN_AUTOMATIC_PROFILE", player_saves);
        host.set_automatic_profile(automatic_profile);
        host.configure_startup_policy({environment_flag("BADMAN_SKIP_INTRO", false), automatic_profile});
        const auto packaged_video = executable_directory(argv[0]) / "tools/ffmpeg/bin";
        const auto video_override = std::getenv("BADMAN_FFMPEG_DIRECTORY");
        std::error_code video_path_error;
        const bool packaged_video_available = std::filesystem::is_directory(packaged_video, video_path_error);
        const auto codec_directory = video_override ? std::filesystem::path(video_override) :
            packaged_video_available ? packaged_video : std::filesystem::path(BADMAN_FFMPEG_DIRECTORY);
        host.configure_video_decoder(codec_directory);
        host.configure_atrac_decoder(codec_directory);
        const bool explicit_fonts = argc >= 6 && argv[5][0] != '\0';
        const std::filesystem::path font_directory = explicit_fonts ? argv[5] : BADMAN_FONT_DIRECTORY;
        if (explicit_fonts || std::filesystem::is_directory(font_directory)) host.configure_font_directory(font_directory);
        host.install(elf.scan_imports(runtime->memory(), *module));
        host.bootstrap(elf.runtime_entry(), "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
#if defined(_WIN32) || defined(BADMAN_EMBEDDED_MAIN)
        if (std::getenv("BADMAN_HEADLESS") == nullptr) host.enable_window();
#endif
        const auto script = std::getenv("BADMAN_INPUT_SCRIPT");
        const auto control = std::getenv("BADMAN_INPUT_CONTROL_FILE");
        if (script && control) throw psprecomp::Error("Choose either scripted or live diagnostic input");
        if (script) {
            host.set_input_provider(scripted_input(script, trace));
            trace << "[input-script] " << script << '\n';
            std::cout << "Diagnostic scripted input enabled: " << script << '\n';
        }
        if (control) {
            std::vector<std::uint32_t> watch;
            if (const auto requested = std::getenv("BADMAN_INPUT_WATCH_WORDS")) {
                std::istringstream words(requested);
                std::string word;
                while (std::getline(words, word, ',')) {
                    std::uint32_t address{};
                    const auto parsed = std::from_chars(word.data(), word.data()+word.size(), address, 16);
                    if (parsed.ec != std::errc{} || parsed.ptr != word.data()+word.size() ||
                        (address & 3u) || !runtime->memory().contains(address, 4) || watch.size() >= 16)
                        throw psprecomp::Error("Diagnostic input watch requires at most 16 mapped aligned hexadecimal word addresses");
                    watch.push_back(address);
                }
                if (watch.empty()) throw psprecomp::Error("Diagnostic input watch address list is empty");
            }
            host.set_input_provider(live_input(control, trace, runtime->memory(), std::move(watch)));
            std::cout << "Diagnostic live controller enabled: " << control << '\n';
        }
        if (const auto requested_text = std::getenv("BADMAN_OSK_TEST_TEXT")) {
#if defined(_WIN32)
            const std::string encoded(requested_text);
            if(encoded.size()>16380)throw psprecomp::Error("Diagnostic OSK text is too long");
            std::wstring wide;
            if(!encoded.empty()){
                const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,encoded.data(),static_cast<int>(encoded.size()),nullptr,0);
                if(size<=0)throw psprecomp::Error("Diagnostic OSK text must be UTF-8");
                wide.resize(size);
                MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,encoded.data(),static_cast<int>(encoded.size()),wide.data(),size);
            }
            host.set_osk_provider([text=std::u16string(wide.begin(),wide.end()),used=false,&trace](const badman::Osk::Dialog &dialog) mutable ->std::optional<badman::Osk::Response>{
                if(used)return {};
                if(!badman::Osk::accepts_text(dialog,text))throw psprecomp::Error("Diagnostic OSK text does not fit the actual field");
                used=true;
                trace<<"[osk-diagnostic-response] generation="<<dialog.generation<<" action=confirm utf16_units="<<text.size()<<std::endl;
                return badman::Osk::Response{dialog.generation,badman::Osk::Response::Action::confirm,text};
            });
            std::cout<<"Explicit diagnostic text will confirm the first OSK field; this is injected test input.\n";
#else
            throw psprecomp::Error("Diagnostic UTF-8 OSK text input currently requires Windows");
#endif
        }
        const auto movie_capture = std::getenv("BADMAN_DUMP_MOVIE_FRAME");
        const auto run_seconds = std::getenv("BADMAN_RUN_SECONDS");
        const auto live_frame = std::getenv("BADMAN_LIVE_FRAME");
        if (movie_capture || run_seconds || live_frame) {
            std::optional<std::chrono::steady_clock::time_point> deadline;
            if (run_seconds) {
                const std::string_view value(run_seconds); unsigned seconds = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !seconds || seconds > 600)
                    throw psprecomp::Error("Diagnostic run duration must be 1..600 whole seconds");
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            }
            host.set_frame_observer([&host, &runtime, path = movie_capture ? std::string(movie_capture) : std::string{},
                live = live_frame ? std::string(live_frame) : std::string{}, next_live = std::chrono::steady_clock::time_point{},
                done = false, deadline](const auto &memory, const auto &display) mutable {
                if (!done && !path.empty() && display.current.address && host.dma_bytes() >= 180ull * 0x88000) {
                    dump_framebuffer(memory, display, path.c_str()); done = true;
                    std::cout << "Captured DMA-threshold display framebuffer: " << path
                              << " address=" << psprecomp::hex32(display.current.address)
                              << " dma_bytes=" << host.dma_bytes() << " vblanks=" << display.vblanks << std::endl;
                }
                const auto now = std::chrono::steady_clock::now();
                if (!live.empty() && display.current.address && now >= next_live) {
                    publish_live_frame(memory, display, live);
                    next_live = now + std::chrono::milliseconds(500);
                }
                if (deadline && now >= *deadline)
                    runtime->stop("Diagnostic wall-clock deadline reached");
            });
        }
        std::cout << "Badman diagnostic AOT host: ULUS10456, entry=" << psprecomp::hex32(elf.runtime_entry())
                  << " registered=" << runtime->function_count() << '\n';
        try { runtime->run(elf.runtime_entry(), limit); }
        catch (const psprecomp::Error &error) { runtime->stop(error.what()); }
        catch (const std::exception &error) {
            runtime->stop("Host failure at " + psprecomp::hex32(runtime->cpu().pc) + ": " + error.what());
        }
        if (runtime->stop_reason().find("GE geometry/transfer") != std::string::npos) {
            trace << "[ge-renderer-checkpoint]\n";
            for (std::uint32_t opcode = 0; opcode < 256; ++opcode) {
                const auto word = host.ge_state().command(static_cast<std::uint8_t>(opcode));
                if (word) trace << "[ge-register] " << psprecomp::hex32(word) << '\n';
            }
            const std::array<std::size_t, 5> matrix_lengths{96, 12, 12, 16, 12};
            for (std::size_t matrix = 0; matrix < matrix_lengths.size(); ++matrix) {
                trace << "[ge-matrix " << matrix << ']';
                for (std::size_t index = 0; index < matrix_lengths[matrix]; ++index)
                    trace << ' ' << psprecomp::hex32(host.ge_state().matrix_word(matrix, index));
                trace << '\n';
            }
            const auto vertices = host.ge_state().vertex_address();
            trace << "[ge-vertices] address=" << psprecomp::hex32(vertices) << '\n';
            if (runtime->memory().contains(vertices, 1024)) {
                for (std::uint32_t offset = 0; offset < 384; offset += 12)
                    trace << "[ge-vertex-data] " << psprecomp::hex32(runtime->memory().load32(vertices + offset))
                          << ' ' << psprecomp::hex32(runtime->memory().load32(vertices + offset + 4))
                          << ' ' << psprecomp::hex32(runtime->memory().load32(vertices + offset + 8)) << '\n';
            }
        }
        host.dump_scheduler(trace);
        if (host.startup_policy().active()) {
            const auto &startup = host.startup_policy().counters();
            std::cout << "Startup policy: logo skips armed/presses/returns " << startup.logo_skips_armed << '/'
                      << startup.logo_presses << '/' << startup.logo_returns
                      << ", returns with live media " << startup.logo_returns_with_live_media
                      << ", load prompt presses " << startup.prompt_presses << " button 0x"
                      << std::hex << startup.prompt_button << std::dec << '\n';
        }
        const auto audio_stats = host.audio_statistics();
        std::cout << "Stop: " << runtime->stop_reason() << "\nHLE calls: " << host.import_calls()
                  << "\nStarted threads: " << host.started_threads()
                  << "\nLast import: " << host.last_import()
                  << "\nGE commands: " << host.ge_state().executed_commands()
                  << "\nGE completed lists: " << host.ge_state().completed_lists()
                  << "\nGE callbacks: " << host.ge_state().delivered_callbacks()
                  << "\nVblank callbacks: " << host.display_state().interrupt_callbacks
                  << "\nDMA bytes: " << host.dma_bytes()
                  << "\nAudio submitted buffers/frames/nonzero samples: " << audio_stats.submitted_buffers << '/'
                  << audio_stats.submitted_frames << '/' << audio_stats.nonzero_samples
                  << "\nAudio completed buffers/frames: " << audio_stats.completed_buffers << '/' << audio_stats.completed_frames
                  << "\nGE color/depth pixels: " << host.ge_state().color_pixels() << '/' << host.ge_state().depth_pixels()
                  << "\nFont module loaded/started: " << host.font_loaded() << '/' << host.font_started()
                  << "\nDisc files opened: " << host.disc_state().files_opened()
                  << "\nDisc bytes read: " << host.disc_state().bytes_read() << '\n';
        std::cout.flush();
        if (const auto capture = std::getenv("BADMAN_DUMP_FRAME"); capture != nullptr) {
            if (!host.display_state().current.address) std::cout << "Capture not written: display framebuffer is disabled; existing file preserved.\n";
            else {
                dump_framebuffer(runtime->memory(), host.display_state(), capture);
                std::cout << "Captured latched guest framebuffer: " << capture
                      << " address=" << psprecomp::hex32(host.display_state().current.address)
                      << " stride=" << host.display_state().current.stride
                      << " format=" << host.display_state().current.format
                      << " vblanks=" << host.display_state().vblanks << '\n';
            }
        }
        if (std::getenv("BADMAN_HOLD_FRAME_ON_STOP") != nullptr) host.wait_for_window_close();
        return runtime->stop_reason() == "All Badman threads exited" ||
            runtime->stop_reason() == "User closed game window" ||
            runtime->stop_reason() == "Game requested exit" ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "Badman error: " << error.what() << '\n';
        return 1;
    }
}
