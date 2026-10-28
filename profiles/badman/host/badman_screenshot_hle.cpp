#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <ostream>

namespace badman {
void Host::install_screenshot() {
    const auto service=[this](std::uint32_t nid,Context &context) {
        if(in_interrupt_context())
            throw psprecomp::Error("Screenshot utility calls are invalid inside an interrupt callback");
        const bool other_active=savedata_.status()!=Savedata::Status::none ||
            osk_.status()!=Osk::Status::none;
        if(other_active) {
            context.gpr[2]=nid==0xd81957b7u ? Osk::dialog_type_mismatch : Screenshot::invalid_status;
            return;
        }
        const auto before=screenshot_.status();
        switch(nid) {
        case 0x0251b134u:context.gpr[2]=screenshot_.init_start(runtime_.memory(),context.gpr[4]);break;
        case 0xd81957b7u:context.gpr[2]=screenshot_.get_status();break;
        case 0xab083ea9u:context.gpr[2]=screenshot_.update(runtime_.memory(),context.gpr[4]);break;
        case 0xf9e0008cu:context.gpr[2]=screenshot_.shutdown_start();break;
        }
        if(before!=screenshot_.status()) {
            trace_<<"[screenshot-status] "<<static_cast<unsigned>(before)<<"->"
                <<static_cast<unsigned>(screenshot_.status())<<" nid="<<psprecomp::hex32(nid)
                <<" return="<<psprecomp::hex32(context.gpr[2])<<'\n';
            if(!screenshot_.last_file().empty())trace_<<"[screenshot-file] "<<screenshot_.last_file().string()<<'\n';
        }
        if(!screenshot_.last_diagnostic().empty())
            trace_<<"[screenshot-error] "<<screenshot_.last_diagnostic()<<'\n';
    };
    bind("sceUtility",0x0251b134u,"sceUtilityScreenshotInitStart",[service](Context &c){service(0x0251b134u,c);});
    bind("sceUtility",0xd81957b7u,"sceUtilityScreenshotGetStatus",[service](Context &c){service(0xd81957b7u,c);});
    bind("sceUtility",0xab083ea9u,"sceUtilityScreenshotUpdate",[service](Context &c){service(0xab083ea9u,c);});
    bind("sceUtility",0xf9e0008cu,"sceUtilityScreenshotShutdownStart",[service](Context &c){service(0xf9e0008cu,c);});
}
}
