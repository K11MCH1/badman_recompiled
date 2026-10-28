#include "badman_osk.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>

namespace badman {
namespace {
constexpr std::uint32_t parameter_size=64, field_size=52, max_units=4096;
bool valid_text(const std::u16string &text, bool single_line) {
    for (std::size_t i=0;i<text.size();++i) {
        const auto unit=static_cast<std::uint16_t>(text[i]);
        if (!unit || (single_line&&(unit=='\r'||unit=='\n'))) return false;
        if (unit>=0xd800u&&unit<=0xdbffu) {
            if (++i==text.size()||text[i]<0xdc00u||text[i]>0xdfffu) return false;
        } else if (unit>=0xdc00u&&unit<=0xdfffu) return false;
    }
    return true;
}
bool read_text(const psprecomp::GuestMemory &memory, std::uint32_t address,
               std::u16string &out) {
    if (!address||(address&1u)) return false;
    for (std::uint32_t i=0;i<max_units;++i) {
        const auto current=std::uint64_t(address)+i*2u;
        if (current>0xffffffffu||!memory.contains(static_cast<std::uint32_t>(current),2)) return false;
        const auto unit=memory.load16(static_cast<std::uint32_t>(current));
        if (!unit) return valid_text(out,false);
        out.push_back(static_cast<char16_t>(unit));
    }
    return false;
}
bool overlap(std::uint32_t a,std::uint32_t as,std::uint32_t b,std::uint32_t bs) {
    const auto aa=std::uint64_t(psprecomp::GuestMemory::canonical(a));
    const auto bb=std::uint64_t(psprecomp::GuestMemory::canonical(b));
    return aa<bb+bs&&bb<aa+as;
}
}
std::uint32_t Osk::init_start(psprecomp::GuestMemory &memory,std::uint32_t parameters) {
    diagnostic_.clear();
    const auto fail=[&](std::uint32_t code,const char *why){diagnostic_=why;return code;};
    if (status_!=Status::none) return fail(invalid_status,"OSK is already active");
    if ((parameters&3u)||!memory.contains(parameters,4)) return fail(invalid_address,"Invalid OSK parameter pointer");
    if (memory.load32(parameters)!=parameter_size) return fail(invalid_size,"OSK requires the 64-byte PSP parameter ABI");
    if (!memory.contains(parameters,parameter_size)) return fail(invalid_address,"Truncated OSK parameters");
    if (memory.load32(parameters+0x30)!=1) throw psprecomp::Error("OSK supports one actual text field; multiple-field selection is not implemented");
    const auto field=memory.load32(parameters+0x34),button_swap=memory.load32(parameters+8);
    if ((field&3u)||!memory.contains(field,field_size)||overlap(parameters,parameter_size,field,field_size))
        return fail(invalid_address,"Invalid/aliased OSK field pointer");
    if (button_swap>1) throw psprecomp::Error("Invalid OSK buttonSwap");
    // unk_00 is not a language. Badman's actual caller stores 1 there; preserve it.
    const auto language=memory.load32(field+8),type=memory.load32(field+0x10),lines=memory.load32(field+0x14);
    if (language!=0) throw psprecomp::Error("OSK supports the game's DEFAULT input language; explicit language-specific keyboards are not implemented");
    if (type!=0) throw psprecomp::Error("OSK supports INPUTTYPE_ALL; restricted character-class filtering is not implemented");
    if (!lines||lines>8) throw psprecomp::Error("OSK line count exceeds the bounded 1..8-line host UI");
    const auto capacity=memory.load32(field+0x24),output=memory.load32(field+0x28),limit=memory.load32(field+0x30);
    if (!capacity||capacity>max_units||limit>=max_units)
        throw psprecomp::Error("OSK output capacity/limit exceeds the bounded host ABI");
    if ((output&1u)||!memory.contains(output,std::size_t(capacity)*2)||
        overlap(output,capacity*2,parameters,parameter_size)||overlap(output,capacity*2,field,field_size))
        return fail(invalid_address,"Invalid/aliased OSK output buffer");
    Dialog snapshot;
    // Bounded host policy: capacity counts UTF16 units including NUL. The
    // positive caller limit is independently capped so termination always fits.
    snapshot.max_units=std::min(limit,capacity-1);
    if (!read_text(memory,memory.load32(field+0x1c),snapshot.description)||
        !read_text(memory,memory.load32(field+0x20),snapshot.initial)||
        !valid_text(snapshot.initial,lines==1)||snapshot.initial.size()>snapshot.max_units)
        throw psprecomp::Error("OSK description/initial text is unterminated, malformed UTF16, or exceeds the field limit");
    snapshot.generation=++generation_;snapshot.line_count=lines;snapshot.input_type=type;
    parameters_=parameters;field_=field;output_=output;capacity_=capacity;limit_=limit;
    cancel_button_=button_swap==1?0x2000u:0x4000u;
    dialog_=std::move(snapshot);response_.reset();cancel_armed_=false;finished_reported_=false;
    status_=Status::init;
    return 0;
}
std::uint32_t Osk::get_status() {
    diagnostic_.clear();
    if (status_==Status::init) {status_=Status::visible;return 1;}
    if (status_==Status::finished) {
        if (!finished_reported_) {finished_reported_=true;return 4;}
        status_=Status::none;parameters_=field_=output_=capacity_=limit_=0;
        dialog_={};response_.reset();return 0;
    }
    return static_cast<std::uint32_t>(status_);
}
std::optional<Osk::Dialog> Osk::dialog() const {
    if (status_!=Status::visible) return {};
    return dialog_;
}
bool Osk::accepts_text(const Dialog &dialog,const std::u16string &text) {
    return text.size()<=dialog.max_units&&valid_text(text,dialog.line_count==1);
}
bool Osk::respond(const Response &response) {
    if (status_!=Status::visible||response_||response.generation!=dialog_.generation) return false;
    if (response.action!=Response::Action::confirm&&response.action!=Response::Action::cancel) return false;
    if (response.action==Response::Action::confirm&&!accepts_text(dialog_,response.text)) return false;
    response_=response;return true;
}
void Osk::observe_buttons(std::uint32_t buttons) {
    if (status_==Status::visible) {
        if (!(buttons&(0x2000u|0x4000u))) cancel_armed_=true;
        if (cancel_armed_&&(buttons&cancel_button_)&&!(previous_buttons_&cancel_button_))
            respond({dialog_.generation,Response::Action::cancel,{}});
    }
    previous_buttons_=buttons;
}
std::uint32_t Osk::update(psprecomp::GuestMemory &memory,std::uint32_t animation_speed) {
    diagnostic_.clear();
    if (status_!=Status::visible) {diagnostic_="OSK Update requires a visible dialog";return invalid_status;}
    if (animation_speed!=1&&animation_speed!=2) throw psprecomp::Error("Unsupported OSK Update animation argument");
    if (!response_) return 0;
    // Validate every output and immutable extent before committing any output.
    if (!memory.contains(parameters_,parameter_size)||!memory.contains(field_,field_size)||
        !memory.contains(output_,std::size_t(capacity_)*2)) {
        diagnostic_="OSK output disappeared before completion";return invalid_address;
    }
    if (memory.load32(parameters_)!=parameter_size||memory.load32(parameters_+0x30)!=1||
        memory.load32(parameters_+0x34)!=field_||memory.load32(field_+0x24)!=capacity_||
        memory.load32(field_+0x28)!=output_||memory.load32(field_+0x30)!=limit_) {
        throw psprecomp::Error("OSK output metadata changed while dialog was active");
    }
    std::uint32_t result=1; // OSK_RESULT_CANCELLED; output text remains untouched.
    if (response_->action==Response::Action::confirm) {
        const auto &text=response_->text;
        if (text.size()>limit_||text.size()+1>capacity_||!valid_text(text,dialog_.line_count==1)) {
            throw psprecomp::Error("Invalid OSK response text");
        }
        for (std::size_t i=0;i<text.size();++i) memory.store16(output_+static_cast<std::uint32_t>(i*2),static_cast<std::uint16_t>(text[i]));
        memory.store16(output_+static_cast<std::uint32_t>(text.size()*2),0);
        result=text==dialog_.initial?0u:2u;
    }
    memory.store32(field_+0x2c,result);
    // The title's completion consumer branches on the common result:1 skips
    // its text conversion/copy, whereas0 consumes output. Field result alone
    // cannot signal cancellation (verified caller FUN_088d3698).
    memory.store32(parameters_+0x1c,response_->action==Response::Action::cancel?1u:0u);
    response_.reset();status_=Status::quit;return 0;
}
std::uint32_t Osk::shutdown_start() {
    diagnostic_.clear();
    if (status_!=Status::quit) {diagnostic_="OSK Shutdown requires a completed dialog";return invalid_status;}
    status_=Status::finished;finished_reported_=false;return 0;
}
void Osk::invoke(std::uint32_t nid,psprecomp::Runtime &runtime,psprecomp::AllegrexContext &context) {
    switch(nid) {
    case 0xf6269b82u:context.gpr[2]=init_start(runtime.memory(),context.gpr[4]);break;
    case 0xf3f76017u:context.gpr[2]=get_status();break;
    case 0x4b85c861u:context.gpr[2]=update(runtime.memory(),context.gpr[4]);break;
    case 0x3dfaeba9u:context.gpr[2]=shutdown_start();break;
    default:throw psprecomp::Error("Unsupported OSK utility import");
    }
}
}
