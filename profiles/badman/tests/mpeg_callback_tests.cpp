#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace psprecomp;
constexpr std::uint32_t root_pc=0x08810000, caller_return=0x08810004;
constexpr std::uint32_t callback_pc=0x08811000, callback_resume=0x08811004, worker_pc=0x08812000;
constexpr std::uint32_t ring=0x08820000, argument=0x08822000, pointer=0x08825000, samples=0x08840000;
constexpr std::uint32_t workspace=0x08900000, guest_gp=0x0891F420;
std::uint32_t semaphore{}, expected_sp{};
bool callback_ran{}, worker_ran{}, caller_ran{};
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
class UnusedDecoder final:public badman::VideoDecoder {
public:
    std::vector<badman::EncodedVideo> parse(std::span<const std::uint8_t>,std::int64_t,std::int64_t,bool)override{throw Error("Decoder must not run in a negative-read continuation test");}
    std::optional<badman::VideoFrame> decode(const badman::EncodedVideo &)override{throw Error("Unexpected decode");}
    std::optional<badman::VideoFrame> drain()override{return {};}
    void reset()override{}
};
void root(Runtime &runtime,AllegrexContext &context){
    expected_sp=context.gpr[29]; context.gpr[31]=caller_return;
    context.gpr[16]=0x11223344; context.hi=0x1234; context.lo=0x5678;
    context.gpr[4]=ring;context.gpr[5]=1;context.gpr[6]=1;
    runtime.invoke_import("sceMpeg",0xB240A59E,context);
    require(context.pc==callback_pc&&runtime_thread_uid()==1,"MPEG Put failed to enter a same-thread callback");
}
void callback(Runtime &runtime,AllegrexContext &context){
    require(context.gpr[4]==samples&&context.gpr[5]==1&&context.gpr[6]==argument&&
            context.gpr[28]==guest_gp&&context.gpr[29]==expected_sp&&runtime_thread_uid()==1,
            "MPEG callback used the wrong arguments, GP, stack or thread");
    callback_ran=true;runtime.memory().store32(argument,context.gpr[31]);
    context.gpr[31]=callback_resume;context.gpr[4]=semaphore;context.gpr[5]=1;context.gpr[6]=0;
    runtime.invoke_import("ThreadManForUser",0x6D212BAC,context);
    require(runtime_thread_uid()!=1,"Blocked MPEG file callback stranded the ready worker");
}
void worker(Runtime &runtime,AllegrexContext &context){
    worker_ran=true;context.gpr[4]=semaphore;context.gpr[5]=1;
    runtime.invoke_import("ThreadManForUser",0x3F53E640,context);
    context.pc=context.gpr[31];
}
void resume(Runtime &runtime,AllegrexContext &context){
    require(worker_ran&&runtime_thread_uid()==1&&context.gpr[29]==expected_sp,"MPEG callback failed to resume after its file worker");
    context.gpr[16]=0xDEADBEEF;context.hi=0x9999;context.lo=0xAAAA;
    context.gpr[2]=static_cast<std::uint32_t>(-7);context.pc=runtime.memory().load32(argument);
}
void returned(Runtime &,AllegrexContext &context){
    require(callback_ran&&worker_ran&&context.gpr[2]==static_cast<std::uint32_t>(-7)&&
            context.gpr[16]==0x11223344&&context.hi==0x1234&&context.lo==0x5678&&
            context.gpr[28]==guest_gp&&context.gpr[29]==expected_sp&&context.gpr[31]==caller_return,
            "MPEG completion lost its caller, callback error or preserved context");
    caller_ran=true;context.gpr[2]=0;context.pc=badman::Host::thread_return_pc;
}
}
int main(){try{
    Runtime runtime;std::ostringstream trace;
    badman::Host host(runtime,0x089F0000,0x089E0000,guest_gp,trace);
    host.set_video_decoder_factory([]{return std::make_unique<UnusedDecoder>();});
    host.install({});host.bootstrap(root_pc,"synthetic");
    runtime.register_function(root_pc,root,"test_mpeg_caller");runtime.register_function(caller_return,returned,"test_mpeg_return");
    runtime.register_function(callback_pc,callback,"test_mpeg_callback");runtime.register_function(callback_resume,resume,"test_mpeg_resume");
    runtime.register_function(worker_pc,worker,"test_file_worker");
    auto &context=runtime.cpu();
    const auto call=[&](const char *library,std::uint32_t nid,std::initializer_list<std::uint32_t> args){
        unsigned i=4;for(auto value:args)context.gpr[i++]=value;runtime.invoke_import(library,nid,context);
    };
    runtime.memory().store32(argument,0);
    call("ThreadManForUser",0xD6DA4BA1,{argument,0,0,1,0});semaphore=context.gpr[2];
    call("ThreadManForUser",0x446D8DE6,{argument,worker_pc,32,4096,0,0});const auto thread=context.gpr[2];
    call("ThreadManForUser",0xF475845D,{thread,0,0});
    call("sceMpeg",0x682A619B,{});
    call("sceMpeg",0x37295ED8,{ring,2,samples,2*0x868,callback_pc,argument});
    call("sceMpeg",0xD8C5F121,{pointer,workspace,0x10000,ring,512,0,0});
    runtime.run(root_pc,100);
    require(caller_ran&&runtime.stop_reason()=="All Badman threads exited","MPEG continuation did not finish the real scheduler path");
    std::cout<<"MPEG same-thread callback, blocking, resumption and caller restoration passed\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
