#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <ostream>
#include <utility>

namespace badman {
void Host::configure_memory_stick(const std::filesystem::path &path) {
    if (savedata_.status()!=Savedata::Status::none || osk_.status()!=Osk::Status::none ||
        screenshot_.status()!=Screenshot::Status::none)
        throw psprecomp::Error("Cannot change Memory Stick mount during a utility operation");
    // Validate each service's mount before replacing the active objects.
    Savedata checked_savedata; checked_savedata.mount_memory_stick(path);
    Screenshot checked_screenshot; checked_screenshot.mount(path);
    savedata_.mount_memory_stick(path);
    screenshot_.mount(path);
    memory_stick_root_ = path.empty() ? std::filesystem::path{} : std::filesystem::canonical(path);
    trace_ << "[memory-stick] root=" << path.string() << '\n';
}
void Host::set_automatic_profile(bool enabled) {
    savedata_.set_automatic_profile(enabled);
    trace_ << "[savedata-profile] automatic=" << (enabled ? 1 : 0) << '\n';
}
void Host::configure_startup_policy(StartupPolicy::Options options) {
    if (!threads_.empty()) throw psprecomp::Error("Configure startup options before bootstrap");
    startup_ = StartupPolicy(options);
    startup_.set_profile_has_records(savedata_.has_native_records("ULUS10456", "YUUSHA.BIN"));
    if (startup_.active()) install_entry_hooks();
    trace_ << "[startup-policy] skip_logo=" << options.skip_logo << " answer_load_prompt=" << options.answer_load_prompt << '\n';
}
Input::PacketFilter Host::controller_filter(std::int32_t uid, std::uint32_t return_pc) {
    if (!startup_.active()) return {};
    return [this, uid, return_pc](std::span<std::uint8_t> packets) {
        startup_.filter_controller(uid, return_pc, packets, runtime_.memory());
    };
}
void Host::set_input_provider(Input::Provider provider) {
    if (!provider) { input_.set_provider({}); return; }
    input_.set_provider([this, provider = std::move(provider)] {
        const auto state = provider();
        if (state.ready) {
            savedata_.observe_buttons(state.buttons);
            osk_.observe_buttons(state.buttons);
        }
        return state;
    });
}
void Host::refresh_savedata_dialog() {
    const auto dialog = savedata_.dialog();
    const auto message = dialog ? dialog->title + "\n\n" + dialog->message +
        "\n\n" + dialog->instructions : std::string{};
    if (message != utility_message_) {
        utility_message_ = message;
        trace_ << "[savedata-dialog] " << (dialog ? dialog->message : "closed");
        if (dialog) trace_ << " result=" << psprecomp::hex32(dialog->result);
        trace_ << '\n';
    }
    if (window_) window_->set_utility_message(utility_message_);
}
void Host::install_savedata() {
    const auto service = [this](std::uint32_t nid, Context &context) {
        if (in_interrupt_context()) throw psprecomp::Error("Savedata utility calls are invalid inside an interrupt callback");
        if (osk_.status()!=Osk::Status::none || screenshot_.status()!=Screenshot::Status::none) {
            if (nid==0x8874dbe0u) { context.gpr[2]=Osk::dialog_type_mismatch;return; }
            if (nid==0x50c4cd57u) { context.gpr[2]=Savedata::invalid_status;return; }
        }
        const auto before = savedata_.status();
        savedata_.invoke(nid, runtime_, context);
        if (before != savedata_.status()) {
            trace_ << "[savedata-status] " << static_cast<unsigned>(before) << "->"
                   << static_cast<unsigned>(savedata_.status()) << " nid=" << psprecomp::hex32(nid)
                   << " return=" << psprecomp::hex32(context.gpr[2]);
            if (savedata_.status() == Savedata::Status::quit)
                trace_ << " result=" << psprecomp::hex32(savedata_.operation_result());
            trace_ << '\n';
        }
        if (!savedata_.last_diagnostic().empty()) trace_ << "[savedata-error] " << savedata_.last_diagnostic() << '\n';
        refresh_savedata_dialog();
    };
    bind("sceUtility", 0x50C4CD57u, "sceUtilitySavedataInitStart", [service](Context &ctx) { service(0x50C4CD57u, ctx); });
    bind("sceUtility", 0x8874DBE0u, "sceUtilitySavedataGetStatus", [service](Context &ctx) { service(0x8874DBE0u, ctx); });
    bind("sceUtility", 0xD4B95FFBu, "sceUtilitySavedataUpdate", [service](Context &ctx) { service(0xD4B95FFBu, ctx); });
    bind("sceUtility", 0x9790B33Cu, "sceUtilitySavedataShutdownStart", [service](Context &ctx) { service(0x9790B33Cu, ctx); });
}
}
