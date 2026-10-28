#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <ostream>

namespace badman {
void Host::refresh_osk_dialog() {
    const auto dialog=osk_.dialog();
    if (window_) {
        window_->set_osk_dialog(dialog);
        if (auto response=window_->take_osk_response()) {
            if (!osk_.respond(*response)) trace_ << "[osk-response] rejected stale or invalid UI response\n";
        }
    }
    if (dialog&&osk_.awaiting_response()&&osk_provider_) {
        if (auto response=osk_provider_(*dialog)) {
            if (!osk_.respond(*response)) trace_ << "[osk-response] rejected stale or invalid provider response\n";
        }
    }
}
void Host::install_osk() {
    const auto service=[this](std::uint32_t nid,Context &context) {
        if (in_interrupt_context()) throw psprecomp::Error("OSK utility calls are invalid inside an interrupt callback");
        const auto before=osk_.status();
        const bool other_active=savedata_.status()!=Savedata::Status::none ||
            screenshot_.status()!=Screenshot::Status::none;
        if (nid==0xf3f76017u&&other_active) {
            context.gpr[2]=Osk::dialog_type_mismatch;
            return;
        }
        if (nid==0xf6269b82u&&other_active) {
            context.gpr[2]=Osk::invalid_status;
            trace_ << "[osk-error] another utility dialog is active\n";
            return;
        }
        // Native controls are pumped on this same host thread. A Response only
        // records the user's action; the PSP Update call performs guest writes.
        if (window_) window_->pump();
        refresh_osk_dialog();
        osk_.invoke(nid,runtime_,context);
        if (before!=osk_.status()) {
            trace_ << "[osk-status] " << static_cast<unsigned>(before) << "->"
                << static_cast<unsigned>(osk_.status()) << " nid=" << psprecomp::hex32(nid)
                << " return=" << psprecomp::hex32(context.gpr[2]) << '\n';
        }
        if (!osk_.last_diagnostic().empty()) trace_ << "[osk-error] " << osk_.last_diagnostic() << '\n';
        refresh_osk_dialog();
    };
    bind("sceUtility",0xf6269b82u,"sceUtilityOskInitStart",[service](Context &c){service(0xf6269b82u,c);});
    bind("sceUtility",0xf3f76017u,"sceUtilityOskGetStatus",[service](Context &c){service(0xf3f76017u,c);});
    bind("sceUtility",0x4b85c861u,"sceUtilityOskUpdate",[service](Context &c){service(0x4b85c861u,c);});
    bind("sceUtility",0x3dfaeba9u,"sceUtilityOskShutdownStart",[service](Context &c){service(0x3dfaeba9u,c);});
}
}
