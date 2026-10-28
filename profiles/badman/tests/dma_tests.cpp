#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

int main(){try{
    using namespace psprecomp;
    Runtime runtime;std::ostringstream trace;
    badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);
    host.install({});host.bootstrap(0x08804000,"synthetic");
    auto &memory=runtime.memory();auto &context=runtime.cpu();
    constexpr std::uint32_t source=0x08810000,destination=0x04088000,size=0x88000;
    std::vector<std::uint8_t> input(size),output(size);
    for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<std::uint8_t>(i^(i>>8));
    memory.copy_in(source,input);memory.store32(destination-4,0x12345678);memory.store32(destination+size,0x87654321);
    const auto copy=[&](std::uint32_t dst,std::uint32_t src,std::uint32_t bytes){
        context.gpr[4]=dst;context.gpr[5]=src;context.gpr[6]=bytes;runtime.invoke_import("sceDmac",0x617F3FE6,context);
    };
    copy(destination,source,size);memory.copy_out(destination,output);
    if(context.gpr[2]!=0||input!=output||host.dma_bytes()!=size||memory.load32(destination-4)!=0x12345678||memory.load32(destination+size)!=0x87654321)
        throw std::runtime_error("Movie-size DMA failed exact copy or overwrote guards");
    input[0]^=0xFF;memory.copy_in(source,input);copy(destination|0x40000000u,source|0x40000000u,size);
    memory.copy_out(destination,output);if(input!=output||host.dma_bytes()!=2u*size)throw std::runtime_error("DMA cached/uncached aliases disagree");
    input[1]^=0x55;memory.copy_in(source,input);copy(destination+GuestMemory::kVramSize,source,size);
    memory.copy_out(destination,output);if(input!=output||host.dma_bytes()!=3u*size)throw std::runtime_error("DMA VRAM mirror selected different backing");
    const auto before=memory.vram_bytes();
    for(const auto invalid:{0u,17u,0x500000u}){
        bool rejected=false;try{copy(destination,source,invalid);}catch(const Error &){rejected=true;}
        if(!rejected||memory.vram_bytes()!=before||host.dma_bytes()!=3u*size)throw std::runtime_error("Invalid DMA mutated its destination/count");
    }
    bool overlap=false;try{copy(destination+16,destination,64);}catch(const Error &){overlap=true;}
    if(!overlap||memory.vram_bytes()!=before)throw std::runtime_error("Overlapping DMA was silently approximated");
    bool mirrored_overlap=false;try{copy(destination+GuestMemory::kVramSize,destination,64);}catch(const Error &){mirrored_overlap=true;}
    if(!mirrored_overlap||memory.vram_bytes()!=before)throw std::runtime_error("Physical overlap through a VRAM mirror was missed");
    bool wrapping=false;try{copy(0x041FFFF0,source,32);}catch(const Error &){wrapping=true;}
    if(!wrapping||memory.vram_bytes()!=before)throw std::runtime_error("Wrapping physical VRAM transfer was silently approximated");
    std::cout<<"Movie DMA bytes, aliases, guard and atomic rejection tests passed\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
