#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <ostream>
#include <string_view>

namespace badman {
namespace {
using psprecomp::Error;
using psprecomp::GuestMemory;
constexpr std::size_t string_limit = 1024, output_limit = 4096, argument_limit = 32;
std::string diagnostic_string(const GuestMemory &memory, std::uint32_t address) {
    std::string result;
    for (std::uint32_t i = 0; i < string_limit; ++i) {
        const auto at = static_cast<std::uint64_t>(address) + i;
        if (at > std::numeric_limits<std::uint32_t>::max() ||
            !memory.contains(static_cast<std::uint32_t>(at), 1))
            throw Error("Kernel printf string is outside guest memory");
        const auto value = memory.load8(static_cast<std::uint32_t>(at));
        if (!value) return result;
        result.push_back(static_cast<char>(value));
    }
    throw Error("Kernel printf string exceeds its diagnostic bound");
}
// MIPS EABI has eight integer argument registers and no caller homing area.
// The format consumed a0; subsequent 32-bit arguments use a1..a7 then sp+0.
// Floating point, 64-bit, positional and %n formats are explicitly unsupported.
std::string format_diagnostic(const GuestMemory &memory, const psprecomp::AllegrexContext &ctx) {
    const auto format = diagnostic_string(memory, ctx.gpr[4]);
    std::size_t argument = 0;
    const auto next_argument = [&]() -> std::uint32_t {
        if (argument >= argument_limit) throw Error("Kernel printf has too many arguments");
        const auto index = argument++;
        if (index < 7) return ctx.gpr[5 + index];
        const auto address = static_cast<std::uint64_t>(ctx.gpr[29]) + (index - 7) * 4;
        if ((ctx.gpr[29] & 3u) || address > std::numeric_limits<std::uint32_t>::max() ||
            !memory.contains(static_cast<std::uint32_t>(address), 4))
            throw Error("Kernel printf stack argument is outside guest memory");
        return memory.load32(static_cast<std::uint32_t>(address));
    };
    std::string output;
    const auto append = [&](std::string_view value) {
        if (value.size() > output_limit - output.size()) throw Error("Kernel printf output exceeds its diagnostic bound");
        output.append(value);
    };
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') { append(std::string_view(format).substr(i, 1)); continue; }
        if (++i == format.size()) throw Error("Incomplete kernel printf format");
        if (format[i] == '%') { append("%"); continue; }
        bool left = false, zero = false;
        if (format[i] == '-') { left = true; ++i; }
        if (i < format.size() && format[i] == '0') { zero = true; ++i; }
        std::size_t width = 0;
        while (i < format.size() && format[i] >= '0' && format[i] <= '9') {
            width = width * 10 + static_cast<unsigned>(format[i++] - '0');
            if (width > 256) throw Error("Kernel printf width exceeds its diagnostic bound");
        }
        if (i == format.size()) throw Error("Incomplete kernel printf conversion");
        const char specifier = format[i];
        if (specifier != 's' && specifier != 'c' && specifier != 'd' && specifier != 'i' &&
            specifier != 'u' && specifier != 'x' && specifier != 'X' && specifier != 'p')
            throw Error("Unsupported kernel printf conversion");
        const auto value = next_argument();
        std::string text;
        if (specifier == 's') text = diagnostic_string(memory, value);
        else if (specifier == 'c') text.push_back(static_cast<char>(value));
        else {
            char digits[40];
            if (specifier == 'd' || specifier == 'i') {
                const auto converted = std::to_chars(digits, digits + sizeof(digits), static_cast<std::int32_t>(value));
                text.assign(digits, converted.ptr);
            } else {
                const int radix = specifier == 'u' ? 10 : 16;
                const auto converted = std::to_chars(digits, digits + sizeof(digits), value, radix);
                text.assign(digits, converted.ptr);
                if (specifier == 'X') for (auto &ch : text) if (ch >= 'a' && ch <= 'f') ch -= 'a' - 'A';
                if (specifier == 'p') { text.insert(0, 8 - text.size(), '0'); text.insert(0, "0x"); }
            }
        }
        const auto padding = width > text.size() ? width - text.size() : 0;
        if (left) { append(text); append(std::string(padding, ' ')); }
        else if (zero && specifier != 's' && specifier != 'c' && specifier != 'p') {
            if (!text.empty() && text.front() == '-') {
                append("-"); append(std::string(padding, '0')); append(std::string_view(text).substr(1));
            } else { append(std::string(padding, '0')); append(text); }
        } else { append(std::string(padding, ' ')); append(text); }
    }
    return output;
}
}
void Host::install_exit_services() {
    bind("LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame", [this](Context &) {
        const auto thread = threads_.find(psprecomp::runtime_thread_uid());
        if (in_interrupt_context() || thread == threads_.end() || thread->second.state != State::running)
            throw Error("Kernel exit game requires thread context");
        // Returning from the native dispatch unwinds to main, whose ordinary
        // Host destruction closes audio, decoder and file resources. No guest
        // return value or fabricated next guest instruction is produced.
        runtime_.stop("Game requested exit");
    });
    bind("SysMemUserForUser", 0x13A5ABEFu, "sceKernelPrintf", [this](Context &ctx) {
        const auto text = format_diagnostic(runtime_.memory(), ctx);
        trace_ << "[kernel-printf] ";
        trace_.write(text.data(), static_cast<std::streamsize>(text.size()));
        trace_.flush();
        if (!trace_) throw Error("Kernel printf trace output failed");
        // The SDK declares this function void; preserve the arbitrary v0.
    });
}
}
