#include "badman_ge.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace psprecomp;
void require(bool okay,const char *message){if(!okay)throw std::runtime_error(message);}
constexpr std::uint32_t code=0x08804000,vertices=0x08810000,target=0x04020000;
struct Result {
    std::vector<std::uint8_t> ram,vram;
    std::vector<std::uint32_t> events;
    std::uint64_t color{},commands{},completed{},cancelled{},callbacks{};
    bool operator==(const Result &)const=default;
};
Result run(const char *backend,bool signal,bool fail_after_cancel){
    _putenv_s("BADMAN_RENDERER",backend);
    GuestMemory memory;badman::Ge *selected=nullptr;
    std::vector<std::uint32_t> events;std::int32_t fresh=-1;
    constexpr auto replacement=code+0x1000,tail=code+0x2000,callback_data=code+0x3000;
    badman::Ge ge(memory,[&](const badman::GeInterrupt &e){
        events.push_back(e.token);events.push_back(memory.load32(target));
        if(e.token==0x55){
            require(memory.load32(target)==0x00102030,"Callback saw uncommitted GPU prefix");
            require(selected->break_queue(1,callback_data)==0,"GPU callback failed cancellation");
            fresh=selected->enqueue(replacement,0,0);
            if(fail_after_cancel)throw Error("Synthetic GPU callback error after reset");
        }
    });selected=&ge;
    const auto store=[&](std::uint32_t address,std::initializer_list<std::uint32_t> words){for(auto word:words){memory.store32(address,word);address+=4;}};
    store(callback_data,{0x08870000,0,0x08870004,0});require(ge.set_callback(callback_data)==0,"Unexpected callback ID");
    for(unsigned pair=0;pair<3;++pair)for(unsigned i=0;i<2;++i){
        const auto at=vertices+(pair*2+i)*24;const auto side=pair==0?7.f:pair==1?2.f:16.f;
        memory.store32(at,0);memory.store32(at+4,0);memory.store32(at+8,pair==0?0xFF102030:pair==1?0xFFABCDEF:0xFF778899);
        memory.store32(at+12,std::bit_cast<std::uint32_t>(i?side:0.f));
        memory.store32(at+16,std::bit_cast<std::uint32_t>(i?(pair==0?9.f:side):0.f));memory.store32(at+20,0);
    }
    auto at=code;
    const auto cmd=[&](unsigned op,std::uint32_t value){memory.store32(at,(op<<24)|(value&0xFFFFFF));at+=4;};
    cmd(0x10,0x080000);cmd(0x01,vertices&0xFFFFFF);
    for(auto [reg,value]:{std::pair{0x12u,0x80019Fu},std::pair{0x16u,63u|(63u<<10)},std::pair{0x1Cu,1u},std::pair{0x1Eu,0u},
            std::pair{0x9Cu,0x20000u},std::pair{0x9Du,64u},std::pair{0xD2u,3u},std::pair{0xD5u,63u|(63u<<10)}})cmd(reg,value);
    cmd(0x04,0x060002);cmd(signal?0x0E:0x0F,signal?0x010055:0x55);cmd(0x0C,0);
    cmd(0x15,0xBAD);cmd(0x04,0x060002);cmd(0x0F,0x66);cmd(0x0C,0);
    store(replacement,{0x01000000u|((vertices+48)&0xFFFFFF),0x04060002,0x0F000077,0x0C000000});
    store(tail,{0x1600BAD0,0x01000000u|((vertices+96)&0xFFFFFF),0x04060002,0x0F000088,0x0C000000});
    const auto original=ge.enqueue(code,code,0),pending=ge.enqueue(tail,0,0);
    bool threw=false;try{ge.update_stall(original,0);}catch(const Error &e){threw=true;require(std::string(e.what())=="Synthetic GPU callback error after reset","Unexpected cancellation error");}
    require(threw==fail_after_cancel,"GPU cancellation exception contract changed");
    require(ge.draw_status(true)==0&&fresh>0&&ge.list_status(fresh,true)==0,"GPU replacement queue did not drain");
    require(static_cast<std::uint32_t>(ge.list_status(original,false))==(signal?0x80000100u:0u),"GPU old active state is wrong");
    require(static_cast<std::uint32_t>(ge.list_status(pending,false))==0x80000100u,"GPU old tail remains active");
    require(ge.command(0x15)==0&&ge.command(0x16)==(0x16000000u|63u|(63u<<10)),"Cancelled GE suffix changed state");
    require(memory.load32(target)==0x00ABCDEF&&memory.load32(target+(3*64+3)*4)==0x00102030&&memory.load32(target+(3*64+8)*4)==0,
        "GPU retained prefix/replacement pixels are wrong");
    require(events==std::vector<std::uint32_t>{0x55,0x00102030,0x77,0x00ABCDEF},"GPU callback observations/order changed");
    require(ge.color_pixels()==67&&ge.depth_pixels()==0&&ge.completed_lists()==(signal?1:2)&&
        ge.cancelled_lists()==(signal?2:1)&&ge.delivered_callbacks()==(fail_after_cancel?1:2),"GPU cancellation counters are wrong");
    return {memory.bytes(),memory.vram_bytes(),events,ge.color_pixels(),ge.executed_commands(),ge.completed_lists(),ge.cancelled_lists(),ge.delivered_callbacks()};
}
}
int main(){try{
    for(auto [signal,fail]:{std::pair{true,false},std::pair{true,true},std::pair{false,false}}){
        const auto reference=run("software",signal,fail);
        for(auto name:{"d3d11","vulkan"})require(reference==run(name,signal,fail),"GPU cancellation full RAM/VRAM/counters differ");
    }
    _putenv_s("BADMAN_RENDERER","");
    std::cout<<"PASS_GE_BREAK_GPU_FULL_MEMORY cases=9 backends=software,d3d11,vulkan tolerance=0 signal_and_finish_break exception_atomicity=1\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
