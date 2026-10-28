#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace badman {
// Independently implemented, bounded single-field PSP utility OSK. Text/result
// output is committed only by Update after a real confirm or cancel response.
class Osk {
public:
    enum class Status : std::uint32_t { none=0, init=1, visible=2, quit=3, finished=4 };
    struct Dialog {
        std::uint64_t generation{};
        std::u16string description, initial;
        std::uint32_t max_units{}; // UTF16 code units, excluding the terminator.
        std::uint32_t line_count{1}, input_type{};
    };
    struct Response {
        enum class Action { confirm, cancel };
        std::uint64_t generation{};
        Action action{Action::cancel};
        std::u16string text;
    };
    using Provider = std::function<std::optional<Response>(const Dialog &)>;
    static constexpr std::uint32_t invalid_status=0x80110001u, invalid_address=0x80110002u,
        invalid_size=0x80110004u, dialog_type_mismatch=0x80110005u;
    std::uint32_t init_start(psprecomp::GuestMemory &, std::uint32_t parameters);
    std::uint32_t get_status();
    std::uint32_t update(psprecomp::GuestMemory &, std::uint32_t animation_speed);
    std::uint32_t shutdown_start();
    void invoke(std::uint32_t nid, psprecomp::Runtime &, psprecomp::AllegrexContext &);
    std::optional<Dialog> dialog() const;
    static bool accepts_text(const Dialog &, const std::u16string &);
    bool respond(const Response &);
    void observe_buttons(std::uint32_t buttons);
    Status status() const { return status_; }
    bool awaiting_response() const { return status_==Status::visible&&!response_; }
    const std::string &last_diagnostic() const { return diagnostic_; }
private:
    Status status_{Status::none};
    std::uint32_t parameters_{}, field_{}, output_{}, capacity_{}, limit_{}, cancel_button_{0x2000u};
    std::uint32_t previous_buttons_{};
    std::uint64_t generation_{};
    bool cancel_armed_{}, finished_reported_{};
    Dialog dialog_;
    std::optional<Response> response_;
    std::string diagnostic_;
};
}
