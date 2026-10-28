#include "badman_screenshot.hpp"
#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif
namespace {
using badman::Screenshot;
using psprecomp::GuestMemory;
constexpr std::uint32_t parameters=0x08810000,framebuffer=0x04000000;
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
std::vector<std::uint8_t> contents(const std::filesystem::path &path){
    std::ifstream file(path,std::ios::binary);require(bool(file),"Cannot read screenshot test artifact");
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
void request(GuestMemory &memory){
    memory.zero(parameters-16,960);
    memory.store32(parameters-4,0x1234ABCD);memory.store32(parameters+928,0x5678EF01);
    memory.store32(parameters,928);memory.store32(parameters+56,2);memory.store32(parameters+60,95);
    memory.store32(parameters+64,framebuffer);memory.store32(parameters+68,64);memory.store32(parameters+72,3);
    memory.store32(parameters+84,32);memory.store32(parameters+88,16);memory.store32(parameters+296,1);
    for(const auto &[offset,text]:std::array<std::pair<unsigned,const char*>,2>{{{92,"ULUS10456"},{104,"native_test"}}})
        memory.copy_in(parameters+offset,{reinterpret_cast<const std::uint8_t*>(text),std::strlen(text)+1});
}
void pixels(GuestMemory &memory,bool reverse=false){
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<64;++x){
        // PSP words are AABBGGRR: left red, right blue, with a green lower half.
        const auto value=y>=8?0xFF00FF00u:((x<16)^reverse?0xFF0000FFu:0xFFFF0000u);
        memory.store32(framebuffer+(y*64+x)*4,value);
    }
}
void guards(const GuestMemory &memory){
    require(memory.load32(parameters-4)==0x1234ABCD&&memory.load32(parameters+928)==0x5678EF01,"Screenshot utility overwrote parameter guards");
}
void complete(Screenshot &shot,GuestMemory &memory){
    require(shot.init_start(memory,parameters)==0,"Screenshot initialization failed");
    require(shot.get_status()==1&&shot.get_status()==2,"Screenshot did not become visible");
    require(shot.last_file().empty(),"Screenshot claimed a file before capture");
    require(shot.update(memory,1)==0&&shot.status()==Screenshot::Status::quit,"Screenshot did not finish Update");
    guards(memory);
}
void idle(Screenshot &shot){
    require(shot.shutdown_start()==0,"Screenshot shutdown failed");
    require(shot.get_status()==4&&shot.get_status()==0,"Screenshot lifecycle did not return to idle after finished status");
}
void host_imports(const std::filesystem::path &folder){
    psprecomp::Runtime runtime;auto &memory=runtime.memory();std::ostringstream trace;
    badman::Host host(runtime,0x089f0000,0x089e0000,0x0891f420,trace);
    host.configure_memory_stick(folder);host.install({});request(memory);pixels(memory);
    const auto call=[&](std::uint32_t nid,std::uint32_t arg=0){
        auto &context=runtime.cpu();context.gpr[4]=arg;
        runtime.invoke_import("sceUtility",nid,context);return context.gpr[2];
    };
    require(call(0xd81957b7u)==0&&call(0x0251b134u,parameters)==0,"Screenshot HLE not bound to real utility");
    const auto before=memory.bytes();
    require(call(0xf6269b82u)==badman::Osk::invalid_status&&
        call(0x50c4cd57u)==badman::Savedata::invalid_status&&
        call(0xf3f76017u)==badman::Osk::dialog_type_mismatch&&
        call(0x8874dbe0u)==badman::Osk::dialog_type_mismatch&&memory.bytes()==before,
        "Another utility overlapped screenshot or wrote rejected outputs");
    require(call(0xd81957b7u)==1&&call(0xd81957b7u)==2&&call(0xab083ea9u,0)==0&&
        call(0xd81957b7u)==3&&call(0xf9e0008cu)==0&&call(0xd81957b7u)==4&&call(0xd81957b7u)==0,
        "Real screenshot HLE lifecycle did not drain to idle");
#ifdef _WIN32
    require(memory.load32(parameters+28)==0&&memory.load32(parameters+52)==1&&
        trace.str().find("[screenshot-file]")!=std::string::npos,"Screenshot HLE claimed no actual stored image");
#endif
    // A real pending OSK blocks screenshot in the opposite direction too.
    constexpr std::uint32_t osk=0x08820000,field=0x08821000,initial=0x08822000,output=0x08823000;
    memory.zero(osk,64);memory.zero(field,52);memory.store32(osk,64);memory.store32(osk+4,1);
    memory.store32(osk+8,1);memory.store32(osk+48,1);memory.store32(osk+52,field);
    memory.store32(field,1);memory.store32(field+20,1);memory.store32(field+28,initial);
    memory.store32(field+32,initial);memory.store32(field+36,8);memory.store32(field+40,output);
    memory.store32(field+48,7);memory.store16(initial,'A');memory.store16(initial+2,0);
    require(call(0xf6269b82u,osk)==0,"Synthetic real OSK request failed");
    const auto pending=memory.bytes();
    require(call(0xd81957b7u)==badman::Osk::dialog_type_mismatch&&
        call(0x0251b134u,parameters)==Screenshot::invalid_status&&memory.bytes()==pending,
        "Screenshot ignored active OSK or changed rejected request");
}
#ifdef _WIN32
void inspect_jpeg(const std::filesystem::path &file){
    using Microsoft::WRL::ComPtr;
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    require(SUCCEEDED(initialized),"Test COM initialization failed");
    struct Lifetime {~Lifetime(){CoUninitialize();}} lifetime;
    ComPtr<IWICImagingFactory> factory;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()))),"Independent WIC decoder unavailable");
    ComPtr<IWICBitmapDecoder> decoder;
    require(SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf())),"Native screenshot is not decodable JPEG");
    GUID container{};require(SUCCEEDED(decoder->GetContainerFormat(&container))&&container==GUID_ContainerFormatJpeg,"Screenshot container is not JPEG");
    ComPtr<IWICBitmapFrameDecode> frame;require(SUCCEEDED(decoder->GetFrame(0,frame.GetAddressOf())),"JPEG frame unavailable");
    UINT width{},height{};require(SUCCEEDED(frame->GetSize(&width,&height))&&width==32&&height==16,"Screenshot dimensions changed");
    ComPtr<IWICFormatConverter> converter;require(SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())),"JPEG format converter unavailable");
    require(SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"JPEG decode conversion failed");
    std::vector<std::uint8_t> bgra(width*height*4);
    require(SUCCEEDED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(bgra.size()),bgra.data())),"JPEG pixel readback failed");
    const auto color=[&](unsigned x,unsigned y,unsigned channel){return bgra[(y*width+x)*4+channel];};
    require(color(4,3,2)>230&&color(4,3,0)<25&&color(27,3,0)>230&&color(27,3,2)<25,"Screenshot red/blue channel order changed");
    require(color(4,12,1)>230&&color(4,12,0)<25&&color(4,12,2)<25,"Screenshot rows or orientation changed");
}
#endif
}
int main(int argc,char **argv){
    try{
        const auto parent=argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path()/"badman-screenshot-tests";
        std::filesystem::create_directories(parent);
        const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto folder=parent/unique;require(std::filesystem::create_directory(folder),"Screenshot test fixture already exists");
        GuestMemory memory;request(memory);pixels(memory);
        Screenshot shot;shot.mount(folder);
        require(shot.get_status()==0&&shot.update(memory,1)==Screenshot::invalid_status&&shot.shutdown_start()==Screenshot::invalid_status,"Idle screenshot utility accepted a completion");
        const auto before=memory.bytes();
        require(shot.init_start(memory,parameters+1)==Screenshot::invalid_address,"Unaligned screenshot parameters were accepted");
        require(memory.bytes()==before,"Rejected screenshot wrote guest memory");
        memory.store32(parameters,927);require(shot.init_start(memory,parameters)==Screenshot::invalid_size,"Wrong screenshot ABI size accepted");request(memory);
        memory.store32(parameters+104,'/');require(shot.init_start(memory,parameters)==Screenshot::bad_parameters,"Escaping screenshot filename accepted");request(memory);
        memory.store32(parameters+84,65);require(shot.init_start(memory,parameters)==Screenshot::bad_parameters,"Screenshot row alias accepted");request(memory);
        memory.store32(parameters+64,0x047FFF00);require(shot.init_start(memory,parameters)==Screenshot::bad_parameters,"Cross-boundary screenshot source accepted");request(memory);
        require(shot.init_start(memory,parameters)==0,"Valid screenshot preflight rejected");
        require(shot.init_start(memory,parameters)==Screenshot::invalid_status,"Busy screenshot accepted a second request");
        require(shot.get_status()==1&&shot.get_status()==2,"Screenshot init transition failed");
        memory.store32(parameters+84,31);
        require(shot.update(memory,1)==Screenshot::bad_parameters&&shot.status()==Screenshot::Status::visible,"Mutable screenshot request was committed");
        memory.store32(parameters+84,32);
        const auto original_vram=memory.vram_bytes();
        require(shot.update(memory,0)==0,"Real native screenshot Update with the title's default zero animation failed");
#ifdef _WIN32
        require(memory.load32(parameters+28)==0&&memory.load32(parameters+52)==1,"Screenshot reported success before a real image completed");
        const auto first=shot.last_file();const auto first_bytes=contents(first);
        require(first.filename()=="native_test-0001.jpg"&&first_bytes.size()>32&&first_bytes[0]==0xFF&&first_bytes[1]==0xD8,"Native screenshot file/header missing");
        inspect_jpeg(first);guards(memory);
        require(memory.vram_bytes()==original_vram,"Screenshot modified the framebuffer");
        idle(shot);request(memory);pixels(memory,true);complete(shot,memory);
        require(shot.last_file().filename()=="native_test-0002.jpg"&&contents(first)==first_bytes,"Second screenshot replaced an existing image");
        idle(shot);
        // A real filesystem failure must complete with failure, never an
        // invented success or a partial file path. Existing file is preserved.
        const auto blocked=folder/"blocked";std::filesystem::create_directory(blocked);
        {std::ofstream file(blocked/"screenshots",std::ios::binary);file<<"preserve me";}
        Screenshot failed;failed.mount(blocked);request(memory);complete(failed,memory);
        require(memory.load32(parameters+28)==Screenshot::access_error&&memory.load32(parameters+52)==1&&failed.last_file().empty(),"Failed screenshot claimed a completed image");
        require(contents(blocked/"screenshots")==std::vector<std::uint8_t>({'p','r','e','s','e','r','v','e',' ','m','e'}),"Screenshot failure destroyed an existing file");
#else
        require(memory.load32(parameters+28)==Screenshot::access_error&&shot.last_file().empty(),"Non-Windows screenshot invented an encoder result");
#endif
        const auto host_folder=folder/"host";std::filesystem::create_directory(host_folder);
        host_imports(host_folder);
        std::cout<<"PASS_SCREENSHOT_NATIVE_IMAGE_LIFECYCLE_GUARDS_STORAGE\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
