#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <vector>

namespace badman {
void Host::install_dma() {
    bind("sceDmac", 0x617F3FE6u, "sceDmacMemcpy", [this](Context &context) {
        const auto destination = context.gpr[4], source = context.gpr[5], size = context.gpr[6];
        // Bounded synchronous copy for the aligned, disjoint movie transfers.
        // Overlap/bus contention and other alignment modes are not modeled.
        if (!size || size > 4u * 1024u * 1024u || ((destination | source | size) & 15u))
            throw psprecomp::Error("Unsupported DMA length/alignment");
        auto &memory = runtime_.memory();
        if (!memory.contains(destination, size) || !memory.contains(source, size))
            throw psprecomp::Error("DMA copy range is outside mapped PSP memory");
        const auto backing_address = [size](std::uint32_t address) {
            const auto physical = psprecomp::GuestMemory::canonical(address);
            if (physical >= psprecomp::GuestMemory::kVramPhysicalBase && physical <
                psprecomp::GuestMemory::kVramPhysicalBase + psprecomp::GuestMemory::kVramAddressSpan) {
                const auto offset = (physical - psprecomp::GuestMemory::kVramPhysicalBase) & (psprecomp::GuestMemory::kVramSize - 1u);
                if (static_cast<std::uint64_t>(offset) + size > psprecomp::GuestMemory::kVramSize)
                    throw psprecomp::Error("Wrapping DMA transfers across physical VRAM are not implemented");
                return psprecomp::GuestMemory::kVramPhysicalBase + offset;
            }
            return physical;
        };
        const auto a = backing_address(destination), b = backing_address(source);
        if (static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b) + size &&
            static_cast<std::uint64_t>(b) < static_cast<std::uint64_t>(a) + size)
            throw psprecomp::Error("Overlapping DMA transfer semantics are not implemented");
        std::vector<std::uint8_t> bytes(size);
        memory.copy_out(source, bytes);
        memory.copy_in(destination, bytes);
        dma_bytes_ += size;
        context.gpr[2] = 0;
    });
}
}
