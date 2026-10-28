// Native Badman preparation steps for hosts without Python (Android): font
// module reconstruction and partitioned Allegrex code generation. Mirrors
// tools/setup_game.py, reconstruct_font.py, prepare_font.py, generate.py and
// generate_font.py; outputs are checked against the same pinned hashes.
#include "badman_profile_facts.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/deflate.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace {
namespace fs = std::filesystem;
using psprecomp::Error;
using Bytes = std::vector<std::uint8_t>;

Bytes read_file(const fs::path &path, std::size_t maximum) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("Cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(in.tellg());
    if (size > maximum) throw Error("Input exceeds its bounded size: " + path.string());
    in.seekg(0);
    Bytes data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<std::uint64_t>(in.gcount()) != size) throw Error("Short read: " + path.string());
    return data;
}
void write_new(const fs::path &path, std::span<const std::uint8_t> data) {
    if (fs::exists(fs::symlink_status(path))) throw Error("Refusing to overwrite " + path.string());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw Error("Cannot write " + path.string());
}
void write_text_changed(const fs::path &path, const std::string &text) {
    if (fs::is_regular_file(path)) {
        const auto old = read_file(path, 64u << 20u);
        if (std::string(old.begin(), old.end()) == text) return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) throw Error("Cannot write " + path.string());
}
std::string sha(std::span<const std::uint8_t> data) { return psprecomp::sha256_bytes(data); }
std::uint32_t le16(const Bytes &d, std::size_t at) { return static_cast<std::uint32_t>(d.at(at) | d.at(at + 1) << 8u); }
std::uint32_t le32(const Bytes &d, std::size_t at) {
    if (at + 4 > d.size()) throw Error("Truncated little-endian word");
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8u |
           static_cast<std::uint32_t>(d[at + 2]) << 16u | static_cast<std::uint32_t>(d[at + 3]) << 24u;
}
void put16(Bytes &d, std::size_t at, std::uint32_t v) { d.at(at) = static_cast<std::uint8_t>(v); d.at(at + 1) = static_cast<std::uint8_t>(v >> 8u); }
void put32(Bytes &d, std::size_t at, std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) d.at(at + i) = static_cast<std::uint8_t>(v >> (8u * i)); }

// Runs an external program with exact arguments (no shell) and returns its status.
int run_process(const std::vector<fs::path> &arguments) {
#if defined(_WIN32)
    std::wstring command;
    for (const auto &argument : arguments) {
        if (!command.empty()) command += L' ';
        const auto text = argument.wstring();
        command += L'"'; std::size_t slashes = 0;
        for (const auto c : text) {
            if (c == L'\\') { ++slashes; continue; }
            if (c == L'"') { command.append(slashes * 2 + 1, L'\\'); command += c; }
            else { command.append(slashes, L'\\'); command += c; }
            slashes = 0;
        }
        command.append(slashes * 2, L'\\'); command += L'"';
    }
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(arguments.front().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        throw Error("Cannot start " + arguments.front().string());
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::vector<std::string> storage; for (const auto &argument : arguments) storage.push_back(argument.string());
    std::vector<char *> argv; for (auto &value : storage) argv.push_back(value.data()); argv.push_back(nullptr);
    pid_t child{};
    if (posix_spawn(&child, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
        throw Error("Cannot start " + storage.front());
    int status = 0;
    if (waitpid(child, &status, 0) != child) throw Error("Cannot wait for " + storage.front());
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

// ---------- gzip (framework inflate) ----------
std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) { auto c = i; for (int k = 0; k < 8; ++k) c = c & 1u ? 0xEDB88320u ^ (c >> 1u) : c >> 1u; t[i] = c; }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : data) crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
    return crc ^ 0xFFFFFFFFu;
}
Bytes gunzip(const Bytes &in, std::uint32_t capacity) {
    if (in.size() < 18 || in[0] != 0x1Fu || in[1] != 0x8Bu || in[2] != 8u || (in[3] & 0xE0u))
        throw Error("Decrypted font is not the expected gzip container");
    std::size_t at = 10; const auto flags = in[3];
    if (flags & 4u) at += 2 + le16(in, at);
    for (const unsigned bit : {8u, 16u}) if (flags & bit) { while (at < in.size() && in[at]) ++at; ++at; }
    if (flags & 2u) at += 2;
    if (at >= in.size()) throw Error("Truncated gzip header");
    constexpr std::uint32_t input = 0x08100000u, output = 0x08800000u;
    psprecomp::GuestMemory memory;
    if (in.size() - at > output - input) throw Error("Compressed font exceeds its bound");
    memory.copy_in(input, std::span<const std::uint8_t>(in.data() + at, in.size() - at));
    const auto result = psprecomp::inflate_raw_deflate(memory, output, capacity, input);
    if (result.status != psprecomp::RawDeflateStatus::Ok) throw Error("Invalid or oversized compressed font module");
    Bytes data(result.output_size);
    memory.copy_out(output, data);
    const auto trailer = at + result.input_consumed;
    if (trailer + 8 > in.size() || le32(in, trailer) != crc32(data) || le32(in, trailer + 4) != result.output_size)
        throw Error("Compressed font CRC/size trailer mismatch");
    return data;
}

// ---------- font reconstruction (reconstruct_font.py + prepare_font.py) ----------
struct Load { std::uint32_t file_offset, vaddr, filesz, memsz, flags; };
std::vector<Load> load_segments(const Bytes &source) {
    if (source.size() < 52) throw Error("Truncated PRX ELF header");
    static constexpr std::uint8_t magic[7]{0x7F, 'E', 'L', 'F', 1, 1, 1};
    if (!std::equal(std::begin(magic), std::end(magic), source.begin()) || le16(source, 16) != 0xFFA0u ||
        le16(source, 18) != 8u || le32(source, 20) != 1u)
        throw Error("Expected the externally decrypted/unpacked little-endian Allegrex PRX ELF");
    const auto phoff = le32(source, 28), phsize = le16(source, 42), count = le16(source, 44);
    if (phsize != 32 || count == 0 || count > 32 || static_cast<std::uint64_t>(phoff) + count * phsize > source.size())
        throw Error("Invalid PRX program-header table");
    std::vector<Load> result; std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto at = phoff + i * 32u;
        if (le32(source, at) != 1u) continue;
        const Load row{le32(source, at + 4), le32(source, at + 8), le32(source, at + 16), le32(source, at + 20), le32(source, at + 24)};
        const auto align = le32(source, at + 28);
        if (!row.memsz || row.memsz > 32u << 20u || row.filesz > row.memsz ||
            static_cast<std::uint64_t>(row.file_offset) + row.filesz > source.size() ||
            static_cast<std::uint64_t>(row.vaddr) + row.memsz > 0x100000000ull || !align || (align & (align - 1u)))
            throw Error("Invalid/truncated load-segment extent");
        total += row.memsz; result.push_back(row);
    }
    if (result.empty() || total > 32u << 20u) throw Error("No bounded load segments");
    for (std::size_t i = 0; i < result.size(); ++i) for (std::size_t j = i + 1; j < result.size(); ++j) {
        const auto &a = result[i], &b = result[j];
        if (a.vaddr < b.vaddr + b.memsz && b.vaddr < a.vaddr + a.memsz) throw Error("Overlapping PRX memory segments");
        if (a.filesz && b.filesz && a.file_offset < b.file_offset + b.filesz && b.file_offset < a.file_offset + a.filesz)
            throw Error("Overlapping PRX file segments");
    }
    return result;
}
void apply_relocations(std::vector<Bytes> &parts, const std::vector<Load> &layout) {
    using namespace badman::facts;
    const auto base = font_relocation_base;
    std::vector<std::uint32_t> targets;
    for (std::size_t i = 0; i < parts.size(); ++i) targets.push_back(base + layout[i].vaddr);
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> validated;
    for (const auto &op : font_relocations) {
        if (op.segment >= parts.size() || (op.offset & 3u) || op.offset + 4u > layout[op.segment].filesz)
            throw Error("Unaligned/out-of-file relocation field");
        if (!seen.insert({op.segment, op.offset}).second) throw Error("Overlapping/duplicate relocation fields");
        if ((op.mask != 0xFFFFFFFFu && op.mask != 0x03FFFFFFu && op.mask != 0x0000FFFFu) || op.addend > op.mask)
            throw Error("Unsupported relocation field mask/addend");
        const auto original = le32(parts[op.segment], op.offset);
        const auto opcode = original >> 26u;
        std::set<std::uint32_t> allowed;
        if (op.mask == 0xFFFFFFFFu) allowed.insert(targets.begin(), targets.end());
        else if (op.mask == 0x03FFFFFFu) {
            if (opcode != 2u && opcode != 3u) throw Error("Jump relocation field does not belong to J/JAL");
            for (const auto t : targets) allowed.insert((t >> 2u) & op.mask);
        } else if (opcode == 15u) {
            for (const auto t : targets) for (const std::uint32_t carry : {0u, 1u}) allowed.insert(((t >> 16u) + carry) & op.mask);
        } else {
            if (opcode != 9u && opcode != 35u && opcode != 36u && opcode != 49u)
                throw Error("Low16 relocation is outside the verified instruction families");
            for (const auto t : targets) allowed.insert(t & op.mask);
        }
        if (!allowed.contains(op.addend)) throw Error("Recipe addend is not a permitted segment-base/carry relocation");
        validated.emplace_back(op.segment, op.offset, op.mask, op.addend, original);
    }
    for (const auto &[segment, offset, mask, addend, original] : validated)
        put32(parts[segment], offset, (original & ~mask) | (((original & mask) + addend) & mask));
}
void validate_tables(const std::vector<Bytes> &parts) {
    using namespace badman::facts;
    const auto base = font_load_base;
    const auto at = [&](std::uint32_t va, std::uint32_t size) -> const std::uint8_t * {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto start = base + font_segments[i].vaddr;
            if (start <= va && static_cast<std::uint64_t>(va) + size <= static_cast<std::uint64_t>(start) + parts[i].size())
                return parts[i].data() + (va - start);
        }
        throw Error("Pointer outside module segments");
    };
    const auto word = [&](std::uint32_t va) { const auto *p = at(va, 4); return static_cast<std::uint32_t>(p[0] | p[1] << 8u | p[2] << 16u) | static_cast<std::uint32_t>(p[3]) << 24u; };
    const auto half = [&](std::uint32_t va) { const auto *p = at(va, 2); return static_cast<std::uint32_t>(p[0] | p[1] << 8u); };
    const auto string = [&](std::uint32_t va) {
        std::string result;
        for (std::uint32_t i = 0; i < 256; ++i) {
            const auto c = *at(va + i, 1);
            if (!c) return result;
            if (c >= 0x80u) throw Error("Non-ASCII module string");
            result.push_back(static_cast<char>(c));
        }
        throw Error("Unterminated module string");
    };
    const auto info = base + font_module_info_relative;
    const auto *name = at(info + 4, 28);
    const std::string module(reinterpret_cast<const char *>(name), strnlen(reinterpret_cast<const char *>(name), 28));
    if (module != font_module || word(info + 32) != font_gp) throw Error("Relocated module identity/GP mismatch");
    const auto exp_begin = word(info + 36), exp_end = word(info + 40), imp_begin = word(info + 44), imp_end = word(info + 48);
    if (half(info) != 6u || half(info + 2) != 0x101u || exp_begin != base + 0x7944u || exp_end != base + 0x7968u ||
        imp_begin != base + 0x7970u || imp_end != base + 0x7998u)
        throw Error("Unexpected module-info table bounds/attributes");
    std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t, bool>> exports;
    for (auto cursor = exp_begin; cursor < exp_end;) {
        const auto lib = word(cursor); const auto length = *at(cursor + 8, 1), nvars = *at(cursor + 9, 1);
        const auto nfuncs = half(cursor + 10), table = word(cursor + 12);
        if (length < 4u || cursor + length * 4u > exp_end) throw Error("Invalid export record length");
        const auto library = lib ? string(lib) : std::string("syslib");
        const auto total = nfuncs + nvars;
        for (std::uint32_t i = 0; i < total; ++i)
            exports.emplace_back(library, word(table + i * 4u), word(table + (total + i) * 4u) - base, i < nfuncs);
        cursor += length * 4u;
    }
    if (exports.size() != font_exports.size()) throw Error("Relocated exports do not match the verified NID/address map");
    for (std::size_t i = 0; i < exports.size(); ++i) {
        const auto &e = font_exports[i];
        if (exports[i] != std::make_tuple(std::string(e.library), e.nid, e.address_relative, e.function))
            throw Error("Relocated exports do not match the verified NID/address map");
    }
    std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>> imports;
    for (auto cursor = imp_begin; cursor < imp_end;) {
        const auto lib = word(cursor); const auto length = *at(cursor + 8, 1), nvars = *at(cursor + 9, 1);
        const auto count = half(cursor + 10), nids = word(cursor + 12), stubs = word(cursor + 16);
        if (length < 5u || nvars || cursor + length * 4u > imp_end) throw Error("Invalid import record length/variable imports");
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)at(stubs + i * 8u, 8);
            imports.emplace_back(string(lib), word(nids + i * 4u), stubs + i * 8u - base);
        }
        cursor += length * 4u;
    }
    if (imports.size() != font_imports.size()) throw Error("Relocated imports do not match the verified NID/stub map");
    for (std::size_t i = 0; i < imports.size(); ++i) {
        const auto &e = font_imports[i];
        if (imports[i] != std::make_tuple(std::string(e.library), e.nid, e.stub_relative))
            throw Error("Relocated imports do not match the verified NID/stub map");
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto &spec = font_segments[i];
        if (parts[i].size() != spec.memsz || std::any_of(parts[i].begin() + spec.filesz, parts[i].end(), [](auto v) { return v != 0; }))
            throw Error("Snapshot extent or zero-initialized BSS mismatch");
    }
}
Bytes normalize(const std::vector<Bytes> &parts) {
    using namespace badman::facts;
    const auto base = font_load_base;
    static constexpr char names[] = "\0.load0_head\0.sceModuleInfo\0.load0_tail\0.load1\0.shstrtab\0";
    constexpr std::uint32_t names_size = sizeof(names) - 1;
    const auto offset_of = [&](std::string_view name) {
        const std::string_view all(names, names_size);
        return static_cast<std::uint32_t>(all.find(std::string(name) + '\0'));
    };
    const std::uint32_t off0 = 0x100u;
    const auto off1 = (off0 + static_cast<std::uint32_t>(parts[0].size()) + 15u) & ~15u;
    const auto stroff = off1 + font_segments[1].filesz;
    const auto shoff = (stroff + names_size + 3u) & ~3u;
    Bytes data(shoff + 6u * 40u);
    static constexpr std::uint8_t ident[7]{0x7F, 'E', 'L', 'F', 1, 1, 1};
    std::copy(std::begin(ident), std::end(ident), data.begin());
    put16(data, 16, 2); put16(data, 18, 8); put32(data, 20, 1); put32(data, 24, font_entry); put32(data, 28, 52);
    put32(data, 32, shoff); put32(data, 36, 0x10A23001u); put16(data, 40, 52); put16(data, 42, 32); put16(data, 44, 2);
    put16(data, 46, 40); put16(data, 48, 6); put16(data, 50, 5);
    const std::array<std::uint32_t, 2> offsets{off0, off1};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto &spec = font_segments[i]; const auto va = base + spec.vaddr; const auto at = 52u + static_cast<std::uint32_t>(i) * 32u;
        for (const auto [k, v] : std::array<std::pair<unsigned, std::uint32_t>, 8>{{{0, 1}, {1, offsets[i]}, {2, va}, {3, va},
                {4, spec.filesz}, {5, spec.memsz}, {6, spec.flags}, {7, 16}}}) put32(data, at + k * 4u, v);
        std::copy_n(parts[i].begin(), spec.filesz, data.begin() + offsets[i]);
    }
    std::copy_n(names, names_size, data.begin() + stroff);
    const auto section = [&](std::uint32_t index, std::string_view name, std::uint32_t kind, std::uint32_t flags,
                             std::uint32_t va, std::uint32_t offset, std::uint32_t size, std::uint32_t align) {
        const std::array<std::uint32_t, 10> values{offset_of(name), kind, flags, va, offset, size, 0, 0, align, 0};
        for (std::size_t k = 0; k < values.size(); ++k) put32(data, shoff + index * 40u + static_cast<std::uint32_t>(k) * 4u, values[k]);
    };
    section(1, ".load0_head", 1, 6, base, off0, 0x799Cu, 4);
    section(2, ".sceModuleInfo", 1, 2, base + 0x799Cu, off0 + 0x799Cu, 52, 4);
    section(3, ".load0_tail", 1, 2, base + 0x79D0u, off0 + 0x79D0u, static_cast<std::uint32_t>(parts[0].size()) - 0x79D0u, 4);
    section(4, ".load1", 1, 3, base + 0x8020u, off1, 0x178u, 16);
    section(5, ".shstrtab", 3, 0, 0, stroff, names_size, 1);
    return data;
}
Bytes reconstruct(const Bytes &source) {
    using namespace badman::facts;
    if (sha(source) != font_unpacked_sha256) throw Error("Unpacked font source SHA256 does not match the supported module");
    if (font_relocation_base != font_load_base || font_relocations.size() != 902)
        throw Error("Expected exactly 902 verified relocation fields at the compiled base");
    const auto layout = load_segments(source);
    if (layout.size() != font_segments.size()) throw Error("Source load-segment count differs from verified profile");
    std::vector<Bytes> parts;
    for (std::size_t i = 0; i < layout.size(); ++i) {
        const auto &row = layout[i]; const auto &spec = font_segments[i];
        if (row.vaddr != spec.vaddr || row.filesz != spec.filesz || row.memsz != spec.memsz || row.flags != spec.flags)
            throw Error("Source segment layout differs from verified profile");
        Bytes part(source.begin() + row.file_offset, source.begin() + row.file_offset + row.filesz);
        part.resize(row.memsz);
        parts.push_back(std::move(part));
    }
    apply_relocations(parts, layout);
    for (std::size_t i = 0; i < parts.size(); ++i)
        if (sha(parts[i]) != font_segments[i].sha256) throw Error("Reconstructed segment SHA256 differs from verified relocation result");
    validate_tables(parts);
    auto result = normalize(parts);
    if (sha(result) != font_prepared_sha256) throw Error("Final prepared font ELF SHA256 differs from verified translation input");
    return result;
}
int prepare_font(const fs::path &prx, const fs::path &decryptor, const fs::path &work, const fs::path &output) {
    using namespace badman::facts;
    const auto raw = read_file(prx, 1u << 20u);
    if (sha(raw) != font_input_sha256) throw Error("Font source changed or does not match the supported module");
    if (raw.size() < 68 || std::memcmp(raw.data(), "~SCE", 4) || le32(raw, 4) != 64u || std::memcmp(raw.data() + 64, "~PSP", 4))
        throw Error("Unexpected native font wrapper; original input was preserved");
    if (fs::exists(fs::symlink_status(work))) throw Error("Font work directory must be new: " + work.string());
    fs::create_directories(work);
    const auto inner = work / "libfont-psp.prx", packed = work / "libfont-decrypted.bin";
    write_new(inner, std::span<const std::uint8_t>(raw.data() + 64, raw.size() - 64));
    if (const auto code = run_process({decryptor, "-o", packed, inner}); code != 0)
        throw Error("External font decryption failed (exit " + std::to_string(code) + ")");
    const auto unpacked = gunzip(read_file(packed, 1u << 20u), 35302u + 1u);
    if (unpacked.size() != 35302u || std::memcmp(unpacked.data(), "\x7F" "ELF", 4) || sha(unpacked) != font_unpacked_sha256)
        throw Error("Unpacked font size, ELF magic or SHA256 does not match the supported module");
    write_new(output, reconstruct(unpacked));
    std::cout << "Prepared native font module " << output.string() << " SHA256 " << font_prepared_sha256 << '\n';
    return 0;
}

// ---------- generation (generate.py / generate_font.py) ----------
struct Run { std::string name; std::uint32_t address, size; };
std::string hex8(std::uint32_t value) { char text[16]; std::snprintf(text, sizeof(text), "%08X", value); return text; }
std::string unit_number(std::size_t value) { char text[16]; std::snprintf(text, sizeof(text), "%04zu", value); return text; }
std::vector<std::vector<Run>> main_partitions() {
    std::vector<std::vector<Run>> result; std::vector<Run> group; std::uint32_t size = 0, end = 0;
    for (const auto &row : badman::facts::code_ranges) {
        if ((row.address & 3u) || !row.size || row.size > 0x4000u || (row.size & 3u) || row.address < end)
            throw Error("Invalid/overlapping code range");
        if (!group.empty() && size + row.size > 0x4000u) { result.push_back(group); group.clear(); size = 0; }
        group.push_back({"badman_code_" + hex8(row.address), row.address, row.size});
        size += row.size; end = row.address + row.size;
    }
    if (!group.empty()) result.push_back(group);
    if (result.empty()) throw Error("Empty instruction map");
    return result;
}
std::vector<std::vector<Run>> font_partitions() {
    using namespace badman::facts;
    constexpr std::uint32_t maximum = 0x2000u;
    const auto base = font_load_base;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> blocked;
    for (const auto &row : font_imports) blocked.emplace_back(base + row.stub_relative, base + row.stub_relative + 8u);
    std::sort(blocked.begin(), blocked.end());
    std::vector<std::vector<Run>> result; std::vector<Run> group; std::uint32_t used = 0, end = 0;
    for (const auto &row : font_code_ranges) {
        if ((row.address & 3u) || !row.size || (row.size & 3u) || row.address < end || row.address < base ||
            row.address + row.size > base + 0x7934u)
            throw Error("Invalid/overlapping font instruction range");
        end = row.address + row.size;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> fragments{{row.address, end}};
        for (const auto &[low, high] : blocked) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> remaining;
            for (const auto &[start, stop] : fragments) {
                if (high <= start || low >= stop) remaining.emplace_back(start, stop);
                else { if (start < low) remaining.emplace_back(start, low); if (high < stop) remaining.emplace_back(high, stop); }
            }
            fragments = remaining;
        }
        for (auto [address, stop] : fragments) {
            for (const auto &[low, high] : blocked) if (address < high && low < stop) throw Error("Import stub survived manual code-range exclusion");
            for (auto length = stop - address; length;) {
                const auto amount = std::min(length, maximum - used);
                group.push_back({"badman_font_code_" + hex8(address), address, amount});
                address += amount; length -= amount; used += amount;
                if (used == maximum) { result.push_back(group); group.clear(); used = 0; }
            }
        }
    }
    if (!group.empty()) result.push_back(group);
    std::uint32_t instructions = 0;
    for (const auto &g : result) for (const auto &r : g) instructions += r.size / 4u;
    if (result.empty() || instructions != 7727u) throw Error("Font instruction map must contain 7727 translated instructions");
    return result;
}
std::string read_universal(const fs::path &path) {
    const auto raw = read_file(path, 64u << 20u);
    std::string text; text.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') { text.push_back('\n'); if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i; }
        else text.push_back(static_cast<char>(raw[i]));
    }
    return text;
}
std::size_t drop_import_registrations(std::string &source) {
    std::string result; result.reserve(source.size()); std::size_t removed = 0;
    for (std::size_t at = 0; at < source.size();) {
        const auto newline = source.find('\n', at);
        if (newline == std::string::npos) { result.append(source, at, std::string::npos); break; }
        const std::string_view line(source.data() + at, newline - at);
        const auto call = line.find("runtime.register_function(");
        if (call != std::string_view::npos && line.find("&import_", call) != std::string_view::npos) ++removed;
        else result.append(source, at, newline - at + 1);
        at = newline + 1;
    }
    source = std::move(result);
    return removed;
}
std::size_t replace_all(std::string &text, std::string_view from, std::string_view to) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size(), ++count) text.replace(at, from.size(), to);
    return count;
}
std::size_t count_of(const std::string &text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
std::size_t uncache_imports(std::string &source) {
    static constexpr std::string_view call = "rt.invoke_import_cached(";
    std::size_t count = 0;
    for (std::size_t at = 0; (at = source.find(call, at)) != std::string::npos;) {
        if (at && word_char(source[at - 1])) { at += call.size(); continue; }
        auto cursor = at + call.size(); const auto digits = cursor;
        while (cursor < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor]))) ++cursor;
        if (cursor == digits || cursor + 1 >= source.size() || source[cursor] != 'u' || source[cursor + 1] != ',') { at += call.size(); continue; }
        cursor += 2;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
        source.replace(at, cursor - at, "rt.invoke_import(");
        ++count;
    }
    return count;
}
bool contains_word(const std::string &lower, const std::string &needle) {
    for (std::size_t at = 0; (at = lower.find(needle, at)) != std::string::npos; ++at) {
        const bool left = at == 0 || !word_char(lower[at - 1]);
        const auto after = at + needle.size();
        if (left && (after == lower.size() || !word_char(lower[after]))) return true;
    }
    return false;
}
int generate(bool font, const fs::path &elf, const fs::path &recompiler, const fs::path &output) {
    using namespace badman::facts;
    const auto expected = font ? font_prepared_sha256 : elf_sha256;
    if (psprecomp::sha256_file(elf) != expected) throw Error(font ? "Expected the verified fixed-base font ELF" :
        "ELF hash does not match the supported ULUS10456 executable");
    const auto partitions = font ? font_partitions() : main_partitions();
    const std::string unit = font ? "badman_font_unit_" : "badman_unit_";
    std::vector<std::string> names;
    for (std::size_t i = 0; i < partitions.size(); ++i) names.push_back(unit + unit_number(i) + ".cpp");
    names.push_back(font ? "badman_font_imports.cpp" : "badman_imports.cpp");
    names.push_back(font ? "badman_font_registry.cpp" : "generated_registry.cpp");
    fs::create_directories(output);
    const auto work = output / ".generate-work";
    fs::create_directories(work);
    const auto csv = work / "functions.csv", cpp = work / "generated.cpp";
    std::set<std::uint32_t> import_pcs;
    for (const auto &row : font_imports) for (const std::uint32_t offset : {0u, 4u}) import_pcs.insert(font_load_base + row.stub_relative + offset);
    for (std::size_t index = 0; index <= partitions.size(); ++index) {
        const bool imports_unit = index == partitions.size();
        {
            std::ofstream out(csv, std::ios::binary | std::ios::trunc);
            out << "name,address,size\r\n";
            if (!imports_unit) for (const auto &run : partitions[index])
                out << run.name << ",0x" << hex8(run.address) << ",0x" << hex8(run.size) << "\r\n";
            if (!out) throw Error("Cannot write partition map");
        }
        fs::remove(cpp);
        if (const auto code = run_process({recompiler, elf, csv, cpp}); code != 0)
            throw Error("psp_recomp failed for unit " + std::to_string(index) + " (exit " + std::to_string(code) + ")");
        auto source = read_universal(cpp);
        std::string symbol;
        if (!imports_unit) {
            const auto first = source.find("static void import_0("), registration = source.find("void register_generated_functions(");
            if (first == std::string::npos || registration == std::string::npos || registration < first)
                throw Error("Generic generator output format changed");
            source.erase(first, registration - first);
            const auto removed = drop_import_registrations(source);
            if (font && removed != font_imports.size()) throw Error("Generic generator import registration format changed");
            symbol = (font ? "register_badman_font_unit_" : "register_badman_unit_") + unit_number(index);
        } else {
            if (font) {
                if (uncache_imports(source) != font_imports.size() || source.find("invoke_import_cached") != std::string::npos)
                    throw Error("Generic generator import wrapper format changed; cannot ensure uncached imports");
            }
            symbol = font ? "register_badman_font_imports" : "register_badman_imports";
        }
        if (font) {
            if (count_of(source, "void register_generated_functions(") != 1)
                throw Error("Generic generator registration format changed");
            std::string lower = source; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto pc : import_pcs) {
                std::string label = "l_" + hex8(pc); std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (contains_word(lower, label)) throw Error("Raw import PC survived as a local label/jump");
                std::string registered = "runtime.register_function(0x" + hex8(pc) + "u";
                std::transform(registered.begin(), registered.end(), registered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!imports_unit) for (std::size_t at = 0; (at = lower.find(registered, at)) != std::string::npos; ++at)
                    if (at + registered.size() == lower.size() || !word_char(lower[at + registered.size()]))
                        throw Error("Raw import PC registered outside import wrappers");
            }
        }
        replace_all(source, "void register_generated_functions(", "void " + symbol + "(");
        write_text_changed(output / names[index], source);
    }
    fs::remove_all(work);
    std::string registry = "#include \"psprecomp/runtime.hpp\"\nnamespace psprecomp {\n";
    std::vector<std::string> symbols;
    for (std::size_t i = 0; i < partitions.size(); ++i) symbols.push_back((font ? "register_badman_font_unit_" : "register_badman_unit_") + unit_number(i));
    symbols.push_back(font ? "register_badman_font_imports" : "register_badman_imports");
    for (const auto &s : symbols) registry += "void " + s + "(Runtime &);\n";
    registry += font ? "void register_badman_font_functions(Runtime &rt) {\n" : "void register_generated_functions(Runtime &rt) {\n";
    for (const auto &s : symbols) registry += "    " + s + "(rt);\n";
    registry += "}\n}\n";
    write_text_changed(output / names.back(), registry);
    std::ostringstream manifest;
    manifest << "{\n  \"producer\": \"badman_prepare\",\n  \"input_sha256\": \"" << expected << "\",\n  \"recompiler_sha256\": \""
             << psprecomp::sha256_file(recompiler) << "\",\n  \"translation_units\": " << partitions.size() << ",\n  \"outputs\": {\n";
    for (std::size_t i = 0; i < names.size(); ++i)
        manifest << "    \"" << names[i] << "\": \"" << psprecomp::sha256_file(output / names[i]) << '"' << (i + 1 < names.size() ? ",\n" : "\n");
    manifest << "  }\n}\n";
    write_text_changed(output / (font ? "font_generation.json" : "generation.json"), manifest.str());
    std::cout << "Generated " << partitions.size() << (font ? " font" : " main") << " units into " << output.string() << '\n';
    return 0;
}

int run(const std::vector<fs::path> &args) {
    const auto usage = [] {
        std::cerr << "Usage:\n  badman_prepare font <libfont.prx> <decryptor> <new work dir> <output fixed ELF>\n"
                     "  badman_prepare generate main|font <elf> <psp_recomp> <output dir>\n";
        return 2;
    };
    if (args.empty()) return usage();
    const auto command = args[0].string();
    if (command == "font" && args.size() == 5) return prepare_font(args[1], args[2], args[3], args[4]);
    if (command == "generate" && args.size() == 5 && (args[1] == "main" || args[1] == "font"))
        return generate(args[1] == "font", args[2], args[3], args[4]);
    return usage();
}
}

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) {
    try { return run(std::vector<fs::path>(argv + 1, argv + argc)); }
    catch (const std::exception &error) { std::cerr << "badman_prepare: " << error.what() << '\n'; return 1; }
}
#else
int main(int argc, char **argv) {
    try { return run(std::vector<fs::path>(argv + 1, argv + argc)); }
    catch (const std::exception &error) { std::cerr << "badman_prepare: " << error.what() << '\n'; return 1; }
}
#endif
