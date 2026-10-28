#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace badman {
// LISTLOAD/LISTSAVE/LISTALLDELETE over an explicitly private native-host format. PSP secure
// saves are neither read nor overwritten. Storage changes require real UI action.
class Savedata {
public:
    Savedata();
    ~Savedata();
    Savedata(const Savedata &)=delete;
    Savedata &operator=(const Savedata &)=delete;
    enum class Status:std::uint32_t {none=0,init=1,visible=2,quit=3,finished=4};
    struct Dialog {std::string title,message;std::uint32_t result{};bool can_ack{};std::string instructions;};
    static constexpr const char *native_directory_name="BADMAN-NATIVE-V1";
    static constexpr std::uint32_t no_media=0x80110301u,no_data=0x80110307u,access_error=0x80110305u;
    static constexpr std::uint32_t invalid_status=0x80110001u,invalid_address=0x80110002u;
    static constexpr std::uint32_t invalid_size=0x80110004u,bad_parameters=0x80110308u;
    static constexpr std::uint32_t data_broken=0x80110306u,save_access_error=0x80110385u,user_cancelled=1;
    static constexpr std::uint32_t delete_no_media=0x80110341u,delete_access_error=0x80110345u;
    static constexpr std::uint32_t delete_data_broken=0x80110346u,delete_no_data=0x80110347u,delete_bad_parameters=0x80110348u;
    void mount_memory_stick(const std::filesystem::path &root);
    // Automatic profile: Load picks the profile's saved slot and Save writes
    // the game's own slot without a chooser or overwrite prompt. Previous
    // receipts are still retained. Errors and Delete stay interactive.
    void set_automatic_profile(bool enabled);
    bool automatic_profile() const{return automatic_profile_;}
    // True when the mounted native store holds any record reference for this
    // game/file, ready or damaged. Reads only directory names.
    bool has_native_records(const std::string &game,const std::string &file) const;
    void invoke(std::uint32_t nid,psprecomp::Runtime &,psprecomp::AllegrexContext &);
    std::uint32_t init_start(psprecomp::GuestMemory &,std::uint32_t parameters);
    std::uint32_t get_status();
    std::uint32_t update(psprecomp::GuestMemory &,std::uint32_t animation_speed);
    std::uint32_t shutdown_start();
    Status status() const{return status_;}
    std::optional<Dialog> dialog() const;
    bool acknowledge();
    bool cancel();
    bool select_slot(const std::string &name);
    void observe_buttons(std::uint32_t psp_buttons);
    std::uint32_t operation_result() const{return pending_result_;}
    const std::string &last_diagnostic() const{return diagnostic_;}
private:
    struct Operation;
    std::unique_ptr<Operation> operation_;
    std::filesystem::path root_;
    Status status_{Status::none};
    std::uint32_t parameters_{},pending_result_{},confirm_button_{0x4000u},previous_buttons_{};
    bool ack_pending_{},cancel_pending_{},ack_armed_{},finished_reported_{},automatic_profile_{};
    std::string message_,diagnostic_;
};
}
