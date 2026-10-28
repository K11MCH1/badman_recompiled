#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace badman {
// Synchronous compute transaction. Input and output words are explicit little-
// endian uint32 values. The caller publishes output to guest memory only after
// successful completion; device errors must never become partial guest writes.
class GpuDevice {
public:
    virtual ~GpuDevice() = default;
    virtual std::string description() const = 0;
    virtual void dispatch(std::span<const std::uint32_t> input,
                          std::span<std::uint32_t> state,
                          std::uint32_t groups) = 0;
    // A device that cannot fail once started may instead work directly on the
    // 2 MiB guest VRAM (little-endian words), saving the copies in and out.
    virtual bool dispatch_in_place(std::span<const std::uint32_t>, std::span<std::uint8_t>) { return false; }
};
// Shader contract: StructuredBuffer<uint> input at t0/binding0,
// RWStructuredBuffer<uint> state at u0/binding1; main(), 64x1x1 threads.
std::unique_ptr<GpuDevice> make_d3d11_compute(std::string_view hlsl);
std::unique_ptr<GpuDevice> make_vulkan_compute(std::span<const std::uint32_t> spirv);
// The same pixel program on CPU helper threads (no GPU), for phones and tests.
std::unique_ptr<GpuDevice> make_cpu_compute(unsigned helpers);
}
