#pragma once
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace badman {
// Title-bounded screenshot utility. The verified 928-byte request is captured
// to a real native JPEG, independently encoded by Windows WIC. Native output
// naming/storage is explicit; PSP screenshot metadata interoperability is not
// inferred from the game's numeric image-format field.
class Screenshot {
public:
    enum class Status:std::uint32_t {none=0,init=1,visible=2,quit=3,finished=4};
    static constexpr std::uint32_t invalid_status=0x80110001u,invalid_address=0x80110002u,
        invalid_size=0x80110004u,bad_parameters=0x80110003u,access_error=0x80010005u;
    void mount(const std::filesystem::path &root);
    std::uint32_t init_start(psprecomp::GuestMemory &,std::uint32_t parameters);
    std::uint32_t get_status();
    std::uint32_t update(psprecomp::GuestMemory &,std::uint32_t animation);
    std::uint32_t shutdown_start();
    Status status()const{return status_;}
    const std::filesystem::path &last_file()const{return last_file_;}
    const std::string &last_diagnostic()const{return diagnostic_;}
private:
    Status status_{Status::none};
    std::filesystem::path root_,last_file_;
    std::array<std::uint8_t,928> request_{};
    std::uint32_t parameters_{};
    std::string basename_,diagnostic_;
};
}
