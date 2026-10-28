#include "badman_osk.hpp"
#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using badman::Osk;
void require(bool value,const char *message) {if(!value)throw std::runtime_error(message);}
template<class Function> void unsupported(Function operation,const char *message) {
    bool failed=false;try{operation();}catch(const psprecomp::Error &){failed=true;}require(failed,message);
}
struct Harness {
    psprecomp::Runtime runtime;
    psprecomp::GuestMemory &memory=runtime.memory();
    Osk osk;
    static constexpr std::uint32_t parameters=0x08800000,field=0x08801000,
        description=0x08802000,initial=0x08803000,output=0x08804000;
    Harness() {request();}
    void text(std::uint32_t address,const std::u16string &value) {
        for(std::size_t i=0;i<value.size();++i)memory.store16(address+static_cast<std::uint32_t>(i*2),value[i]);
        memory.store16(address+static_cast<std::uint32_t>(value.size()*2),0);
    }
    void request(std::u16string value=u"Smith",std::uint32_t limit=6,std::uint32_t capacity=18) {
        memory.zero(parameters,64);memory.zero(field,52);
        memory.store32(parameters,64);memory.store32(parameters+4,1);memory.store32(parameters+8,1);
        memory.store32(parameters+0x1c,0xdeadbeefu);memory.store32(parameters+0x30,1);memory.store32(parameters+0x34,field);
        memory.store32(field,1);memory.store32(field+0x14,1);memory.store32(field+0x18,0xaabbaabbu);
        memory.store32(field+0x1c,description);memory.store32(field+0x20,initial);
        memory.store32(field+0x24,capacity);memory.store32(field+0x28,output);
        memory.store32(field+0x2c,0xcafebabeu);memory.store32(field+0x30,limit);
        text(description,u"Name");text(initial,value);
        for(std::uint32_t i=0;i<capacity*2+4;++i)memory.store8(output+i,0xa7);
    }
    std::vector<std::uint8_t> snapshot(std::uint32_t address=parameters,std::uint32_t count=0x5000) {
        std::vector<std::uint8_t> result(count);memory.copy_out(address,result);return result;
    }
    void visible() {
        require(osk.init_start(memory,parameters)==0,"Valid single-field OSK request rejected");
        require(osk.get_status()==1&&osk.get_status()==2,"OSK INIT/VISIBLE sequence lost");
    }
    void finish() {
        require(osk.get_status()==3&&osk.shutdown_start()==0,"Completed OSK did not shut down");
        require(osk.get_status()==4&&osk.get_status()==0&&!osk.dialog(),"OSK FINISHED/NONE cleanup lost");
    }
    Osk::Response response(std::u16string value,Osk::Response::Action action=Osk::Response::Action::confirm) {
        return {osk.dialog()->generation,action,std::move(value)};
    }
    void cancelled_outputs_only(const std::vector<std::uint8_t> &before) {
        auto expected=before;
        for(const auto address:{parameters+0x1c,field+0x2c})
            for(unsigned i=0;i<4;++i)expected[address-parameters+i]=i?0:1;
        require(snapshot()==expected,"Cancellation must write common+field result1 and preserve every text/metadata/unknown byte");
    }
};
void pending_lifecycle_and_unknown_preservation() {
    Harness h;const auto before=h.snapshot();
    require(h.osk.get_status()==0&&h.osk.update(h.memory,1)==Osk::invalid_status,"Idle OSK accepted Update");
    h.visible();const auto dialog=h.osk.dialog();
    require(dialog&&dialog->initial==u"Smith"&&dialog->description==u"Name"&&dialog->max_units==6&&dialog->line_count==1&&dialog->input_type==0,"OSK ABI fields confused unknown/language/type/text/capacity");
    require(h.osk.init_start(h.memory,h.parameters)==Osk::invalid_status&&h.osk.shutdown_start()==Osk::invalid_status,"Active OSK allowed duplicate init or premature shutdown");
    for(unsigned i=0;i<50;++i)require(h.osk.get_status()==2&&h.osk.update(h.memory,1)==0,"Unanswered OSK advanced without user action");
    require(h.snapshot()==before,"Pending OSK wrote guest outputs or unknown fields");
}
void unchanged_and_changed_unicode() {
    Harness h;h.visible();const auto old=h.response(u"old");require(h.osk.respond(h.response(u"Smith")),"Actual unchanged response rejected");
    require(h.memory.load32(h.field+0x2c)==0xcafebabeu&&h.memory.load16(h.output)==0xa7a7u,"Response wrote output before PSP Update");
    require(h.osk.update(h.memory,2)==0&&h.memory.load32(h.field+0x2c)==0&&h.memory.load32(h.parameters+0x1c)==0,"Unchanged confirmation result wrong");
    for(unsigned i=0;i<5;++i)require(h.memory.load16(h.output+i*2)==u"Smith"[i],"Unchanged confirm did not copy initial text");
    require(h.memory.load16(h.output+10)==0&&h.memory.load16(h.output+12)==0xa7a7u&&h.memory.load32(h.field)==1&&h.memory.load32(h.field+0x18)==0xaabbaabbu,"Unchanged confirmation termination/tail/unknown fields changed");
    h.finish();h.request(u"",6,7);h.visible();
    require(!h.osk.respond(old),"Stale prior-dialog response accepted after restart");
    const std::u16string value=u"\uD55C\U0001F3AEabc";
    require(value.size()==6&&h.osk.respond(h.response(value)),"Bounded valid UTF16 supplementary pair rejected");
    require(h.osk.update(h.memory,1)==0&&h.memory.load32(h.field+0x2c)==2&&h.memory.load32(h.parameters+0x1c)==0&&h.memory.load16(h.output+12)==0&&h.memory.load16(h.output+14)==0xa7a7u,"Changed UTF16 common/field result or termination crossed capacity");
    for(unsigned i=0;i<value.size();++i)require(h.memory.load16(h.output+i*2)==value[i],"UTF16 response lost code units");
    h.finish();
}
void rejected_responses_and_atomic_metadata() {
    Harness h;h.visible();const auto before=h.snapshot();
    for(const auto &value:std::vector<std::u16string>{u"toolong",std::u16string(1,0xd800),std::u16string(1,0xdc00),std::u16string{u'a',0,u'b'},u"a\nb"})
        require(!h.osk.respond(h.response(value)),"Malformed/overlong/singleline-invalid response accepted");
    auto stale=h.response(u"abc");++stale.generation;require(!h.osk.respond(stale),"Wrong-dialog response accepted");
    require(h.snapshot()==before&&h.osk.awaiting_response(),"Rejected response changed pending outputs");
    require(h.osk.respond(h.response(u"abc")),"Valid response rejected after malformed input");
    h.memory.store32(h.field+0x24,1);const auto changed=h.snapshot();
    unsupported([&]{h.osk.update(h.memory,1);},"Changed output metadata silently completed");
    require(h.snapshot()==changed&&h.osk.status()==Osk::Status::visible,"Changed output metadata caused partial completion");
    h.memory.store32(h.field+0x24,18);require(h.osk.update(h.memory,1)==0,"Restored actual output metadata did not complete");h.finish();
}
void cancel_edges_and_preserve_text() {
    Harness h;h.visible();const auto before=h.snapshot(h.output,40),all_before=h.snapshot();
    h.osk.observe_buttons(0x2000);h.osk.update(h.memory,1);
    h.osk.observe_buttons(0x2000);h.osk.update(h.memory,1);
    require(h.osk.status()==Osk::Status::visible,"Held opening Circle silently cancelled OSK");
    h.osk.observe_buttons(0);h.osk.observe_buttons(0x4000);h.osk.update(h.memory,1);
    require(h.osk.status()==Osk::Status::visible,"Controller confirm fabricated text acceptance");
    h.osk.observe_buttons(0);h.osk.observe_buttons(0x2000);
    require(h.osk.update(h.memory,1)==0&&h.memory.load32(h.field+0x2c)==1&&h.memory.load32(h.parameters+0x1c)==1&&h.snapshot(h.output,40)==before,"Real Circle cancellation must report common+field result1 without modifying text");
    h.cancelled_outputs_only(all_before);h.finish();
    h.request();h.memory.store32(h.parameters+8,0);h.visible();h.osk.observe_buttons(0);h.osk.observe_buttons(0x4000);
    require(h.osk.update(h.memory,1)==0&&h.memory.load32(h.field+0x2c)==1&&h.memory.load32(h.parameters+0x1c)==1,"buttonSwap0 did not select Cross cancel/common cancellation result");h.finish();
}
void bounded_invalid_abi() {
    Harness h;auto reject=[&](std::uint32_t address,std::uint32_t value,std::uint32_t expected) {
        h.request();h.memory.store32(address,value);const auto before=h.snapshot();
        require(h.osk.init_start(h.memory,h.parameters)==expected&&h.snapshot()==before&&h.osk.status()==Osk::Status::none,"Invalid OSK ABI wrote state/output");
    };
    reject(h.parameters,60,Osk::invalid_size);
    reject(h.field+0x28,h.parameters|0x80000000u,Osk::invalid_address);
    reject(h.field+0x28,h.field,Osk::invalid_address);reject(h.field+0x28,0x09ffffffu,Osk::invalid_address);
    reject(h.parameters+0x34,0x09fffff0u,Osk::invalid_address);
    for(const auto &[address,value]:std::vector<std::pair<std::uint32_t,std::uint32_t>>{{h.field+0x24,0},{h.field+0x30,0xffffffffu},{h.parameters+8,2}}) {
        h.request();h.memory.store32(address,value);const auto before=h.snapshot();
        unsupported([&]{h.osk.init_start(h.memory,h.parameters);},"Invalid bounded OSK field policy did not stop explicitly");
        require(h.snapshot()==before&&h.osk.status()==Osk::Status::none,"Invalid bounded field partially wrote output/state");
    }
    h.request();h.memory.store16(h.initial,0xd800);h.memory.store16(h.initial+2,0);const auto before=h.snapshot();
    unsupported([&]{h.osk.init_start(h.memory,h.parameters);},"Malformed initial UTF16 accepted");
    require(h.snapshot()==before,"Malformed initial UTF16 wrote outputs");
    h.request();h.memory.store32(h.parameters+0x30,2);unsupported([&]{h.osk.init_start(h.memory,h.parameters);},"Multiple fields pretended to be implemented");
    h.request();h.memory.store32(h.field+8,1);unsupported([&]{h.osk.init_start(h.memory,h.parameters);},"Unknown00 was confused with a supported Japanese keyboard");
    h.request();h.memory.store32(h.field+0x10,4);unsupported([&]{h.osk.init_start(h.memory,h.parameters);},"Restricted input policy pretended to filter text");
}
void multiline_and_empty_confirmation() {
    Harness h;h.request(u"",6,7);h.memory.store32(h.field+0x14,2);h.visible();
    require(h.osk.dialog()->line_count==2&&h.osk.respond(h.response(u"a\r\nb")),"Multiline metadata or actual multiline response rejected");
    require(h.osk.update(h.memory,1)==0&&h.memory.load16(h.output+8)==0,"Multiline response not terminated");h.finish();
    h.request(u"",0,1);h.visible();require(h.osk.respond(h.response(u"")),"Empty valid confirmation rejected");
    require(h.osk.update(h.memory,1)==0&&h.memory.load16(h.output)==0&&h.memory.load32(h.field+0x2c)==0,"Empty unchanged result/capacity wrong");h.finish();
    h.request(u"",6,4);h.visible();
    require(h.osk.dialog()->max_units==3&&!h.osk.respond(h.response(u"abcd"))&&h.osk.respond(h.response(u"abc")),"Output capacity did not independently bound the positive caller limit");
    require(h.osk.update(h.memory,1)==0&&h.memory.load16(h.output+6)==0&&h.memory.load16(h.output+8)==0xa7a7u,"Clamped capacity termination overwrote its tail");h.finish();
}
void actual_host_hle_provider() {
    Harness h;std::ostringstream trace;
    badman::Host host(h.runtime,0x089f0000,0x089e0000,0x0891f420,trace);
    std::uint64_t now=1000000000;std::uint32_t buttons=0;
    host.set_scheduler_timing([&]{return now;},[&](std::uint64_t deadline){now=deadline;});
    host.set_input_provider([&]{return badman::InputState{true,buttons,128,128};});host.install({});
    std::optional<Osk::Dialog> shown;std::optional<Osk::Response> response;
    host.set_osk_provider([&](const Osk::Dialog &dialog)->std::optional<Osk::Response>{shown=dialog;auto result=response;response.reset();return result;});
    auto call=[&](std::uint32_t nid,std::uint32_t arg=0) {auto &c=h.runtime.cpu();c.gpr[4]=arg;h.runtime.invoke_import("sceUtility",nid,c);return c.gpr[2];};
    require(call(0xf6269b82,h.parameters)==0&&call(0xf3f76017)==1&&call(0xf3f76017)==2&&shown,"Host OSK imports/provider not actually bound");
    const auto pending=h.snapshot();
    require(call(0x8874dbe0)==Osk::dialog_type_mismatch&&call(0x50c4cd57,0)==Osk::invalid_status&&h.snapshot()==pending,"Other utility status/init ignored the active OSK type or wrote outputs");
    require(call(0x4b85c861,1)==0&&h.memory.load16(h.output)==0xa7a7u,"Headless OSK fabricated confirmation");
    response=Osk::Response{shown->generation,Osk::Response::Action::confirm,u"Kimchi"};
    require(call(0x4b85c861,1)==0&&h.memory.load32(h.field+0x2c)==2&&h.memory.load32(h.parameters+0x1c)==0&&h.memory.load16(h.output)==u'K'&&call(0xf3f76017)==3,"Real injected host response failed common/field HLE output lifecycle");
    require(call(0x3dfaeba9)==0&&call(0xf3f76017)==4&&call(0xf3f76017)==0,"Host OSK shutdown lifecycle wrong");
    require(trace.str().find("[osk-status] 2->3")!=std::string::npos,"OSK completion absent from host diagnostics");
    host.set_osk_provider({});h.request();
    require(call(0xf6269b82,h.parameters)==0&&call(0xf3f76017)==1&&call(0xf3f76017)==2,"Host OSK did not reopen without a text provider");
    const auto before_cancel=h.snapshot(h.output,40),all_before_cancel=h.snapshot();
    const auto sample=[&](std::uint32_t observed) {
        buttons=observed;now+=20000000;auto &c=h.runtime.cpu();c.gpr[4]=0x08808000;c.gpr[5]=1;
        h.runtime.invoke_import("sceCtrl",0x1f803938,c);require(c.gpr[2]==1,"Actual Ctrl HLE did not observe the fixture button");
    };
    sample(0x2000);require(call(0x4b85c861,1)==0&&call(0xf3f76017)==2,"Actual held opening Circle cancelled OSK");
    sample(0);sample(0x2000);
    require(call(0x4b85c861,1)==0&&call(0xf3f76017)==3&&h.memory.load32(h.field+0x2c)==1&&h.memory.load32(h.parameters+0x1c)==1&&h.snapshot(h.output,40)==before_cancel,"Actual Ctrl provider wrapper must report common+field cancellation, avoiding the guest's success-copy branch");
    h.cancelled_outputs_only(all_before_cancel);
    require(call(0x3dfaeba9)==0&&call(0xf3f76017)==4&&call(0xf3f76017)==0,"Host Circle-cancel shutdown lifecycle wrong");
    constexpr auto savedata=0x08810000u,list=0x08819000u;
    h.memory.zero(savedata,0x600);h.memory.zero(list,40);
    h.memory.store32(savedata,0x600);h.memory.store32(savedata+8,1);h.memory.store32(savedata+0x30,4);
    const auto ascii=[&](std::uint32_t address,const char *value){for(;*value;++address,++value)h.memory.store8(address,static_cast<std::uint8_t>(*value));};
    ascii(savedata+0x3c,"ULUS10456");ascii(savedata+0x64,"YUUSHA.BIN");ascii(list,"YSave00");
    h.memory.store32(savedata+0x60,list);h.memory.store32(savedata+0x74,0x08818000u);h.memory.store32(savedata+0x78,32);
    require(call(0x50c4cd57,savedata)==0,"Real synthetic Savedata request did not become active");
    const auto before_osk=h.snapshot();
    require(call(0xf3f76017)==Osk::dialog_type_mismatch&&call(0xf6269b82,h.parameters)==Osk::invalid_status&&h.snapshot()==before_osk,"OSK ignored the active other-utility type or wrote outputs");
}
}
int main() {try {
    pending_lifecycle_and_unknown_preservation();unchanged_and_changed_unicode();
    rejected_responses_and_atomic_metadata();cancel_edges_and_preserve_text();
    bounded_invalid_abi();multiline_and_empty_confirmation();actual_host_hle_provider();
    std::cout<<"OSK tests passed\n";return 0;
} catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}}
