#include "badman_atrac.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <fstream>
#include <iostream>

namespace {
std::vector<std::uint8_t> read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > badman::Atrac::max_file_bytes)
        throw psprecomp::Error("Cannot read bounded ATRAC probe input");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(input.tellg())); input.seekg(0);
    input.read(reinterpret_cast<char *>(data.data()), data.size());
    if (!input) throw psprecomp::Error("ATRAC probe input read failed"); return data;
}
void finish(std::ofstream &output) {
    output.flush(); if (!output) throw psprecomp::Error("ATRAC probe output flush failed");
    output.close(); if (!output) throw psprecomp::Error("ATRAC probe output close failed");
}
int run(const std::filesystem::path &input, const std::filesystem::path &dlls,
    const std::filesystem::path &raw_path, const std::filesystem::path &played_path, unsigned loops) {
    const auto same = [](const auto &left, const auto &right) {
        std::error_code error;
        return std::filesystem::weakly_canonical(left) == std::filesystem::weakly_canonical(right) ||
            std::filesystem::equivalent(left, right, error);
    };
    if (same(input, raw_path) || same(input, played_path) || same(raw_path, played_path))
        throw psprecomp::Error("ATRAC probe input and both output paths must be distinct");
    const auto bytes = read(input); const auto info = badman::AtracRiff::parse(bytes);
    auto decoder = badman::make_ffmpeg_atrac_decoder(dlls);
    const auto raw = decoder->decode(info, std::span(bytes).subspan(info.data_offset, info.data_size));
    std::ofstream raw_file(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw_file) throw psprecomp::Error("Cannot create raw PCM output");
    raw_file.write(reinterpret_cast<const char *>(raw.data()), raw.size() * sizeof(std::int16_t)); finish(raw_file);
    badman::Atrac atrac([&]{return badman::make_ffmpeg_atrac_decoder(dlls);});
    const auto id = atrac.set_data_and_get_id(bytes);
    if (id < 0) throw psprecomp::Error("ATRAC probe resident initialization failed");
    if (loops && atrac.set_loop_num(id, static_cast<std::int32_t>(loops)))
        throw psprecomp::Error("ATRAC probe requested repeats without a valid loop region");
    const auto expected_played = static_cast<std::uint64_t>(info.samples) +
        loops * (static_cast<std::uint64_t>(info.loop_end) - info.loop_start + 1);
    std::ofstream output(played_path, std::ios::binary | std::ios::trunc);
    if (!output) throw psprecomp::Error("Cannot create played PCM output");
    std::array<std::int16_t,4096> pcm{}; badman::Atrac::DecodeResult result{};
    std::uint64_t total = 0; std::uint32_t calls = 0, first = 0, last = 0;
    for (;;) {
        const auto status = atrac.decode(id, pcm, result); if (status == badman::Atrac::all_decoded) break;
        if (status || !result.samples || total + result.samples > expected_played)
            throw psprecomp::Error("ATRAC probe decode count/status invalid");
        output.write(reinterpret_cast<const char *>(pcm.data()), result.samples * 4);
        if (!calls) first = result.samples; last = result.samples; total += result.samples; ++calls;
    }
    finish(output);
    if (total != expected_played || !result.ended || atrac.release(id)) throw psprecomp::Error("Incomplete played PCM stream");
    std::cout << "packets=" << info.data_size/info.block_align << " raw_frames=" << raw.size()/2
        << " fact_frames=" << info.samples << " fact_offset=" << info.first_offset
        << " codec_delay=" << badman::Atrac::codec_delay << " priming_skip=" << info.skip_samples
        << " trailing_skip=" << raw.size()/2-info.samples-info.skip_samples
        << " loop_start=" << info.loop_start << " loop_end=" << info.loop_end
        << " repeats=" << loops << " calls=" << calls << " first=" << first << " last=" << last << " played=" << total
        << "\nNo playback device opened; PCM files are local diagnostic artifacts.\n";
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc != 5 && argc != 6) throw psprecomp::Error("Usage: BadmanAtracProbe <own RIFF> <FFmpeg DLL directory> <raw.s16> <played.s16> [repeats0..2]");
        unsigned loops = 0;
        if (argc == 6) {
            const auto count = std::filesystem::path(argv[5]).string();
            if (count.size() != 1 || count[0] < '0' || count[0] > '2') throw psprecomp::Error("Probe repeats must be0..2");
            loops = count[0] - '0';
        }
        return run(argv[1],argv[2],argv[3],argv[4],loops);
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
