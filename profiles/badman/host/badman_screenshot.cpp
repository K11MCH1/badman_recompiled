#include "badman_screenshot.hpp"
#include "badman_window.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
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

namespace badman {
namespace {
using psprecomp::Error;
std::uint32_t word(const std::array<std::uint8_t,928>&r,unsigned offset){
    return std::uint32_t(r[offset])|(std::uint32_t(r[offset+1])<<8)|
        (std::uint32_t(r[offset+2])<<16)|(std::uint32_t(r[offset+3])<<24);
}
std::string safe_name(const std::array<std::uint8_t,928>&r,unsigned offset,unsigned length){
    std::string value;
    for(unsigned i=0;i<length;++i){
        const auto c=r[offset+i];if(!c){
            if(value.empty()||value=="."||value=="..")throw Error("Empty/relative screenshot name");
            return value;
        }
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
            throw Error("Unsupported screenshot name character");
        value+=static_cast<char>(c);
    }
    throw Error("Unterminated screenshot name");
}
bool linked(const std::filesystem::path &p){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(p.c_str());
    return flags!=INVALID_FILE_ATTRIBUTES&&(flags&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(p));
#endif
}
void directory(const std::filesystem::path &p){
    if(linked(p)||!std::filesystem::is_directory(p))throw Error("Screenshot storage is not a real directory");
}
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
void checked(HRESULT result,const char *operation){
    if(FAILED(result))throw Error(std::string("Screenshot ")+operation+" failed (HRESULT "+std::to_string(static_cast<std::uint32_t>(result))+")");
}
std::vector<std::uint8_t> jpeg(std::span<const std::uint32_t> bgra,unsigned width,unsigned height,unsigned quality){
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(initialized)&&initialized!=RPC_E_CHANGED_MODE)checked(initialized,"COM initialization");
    struct ComLifetime {bool active;~ComLifetime(){if(active)CoUninitialize();}} com{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf())),"create encoder factory");
    ComPtr<IStream> stream;checked(CreateStreamOnHGlobal(nullptr,TRUE,stream.GetAddressOf()),"create image stream");
    ComPtr<IWICBitmapEncoder> encoder;
    checked(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,encoder.GetAddressOf()),"create JPEG encoder");
    checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"initialize JPEG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;
    checked(encoder->CreateNewFrame(frame.GetAddressOf(),options.GetAddressOf()),"create JPEG frame");
    PROPBAG2 property{};property.pstrName=const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{};value.vt=VT_R4;value.fltVal=quality?static_cast<float>(quality)/100.0f:0.9f;
    checked(options->Write(1,&property,&value),"set JPEG quality");
    checked(frame->Initialize(options.Get()),"initialize JPEG frame");
    checked(frame->SetSize(width,height),"set JPEG dimensions");
    auto format=GUID_WICPixelFormat24bppBGR;checked(frame->SetPixelFormat(&format),"set JPEG pixel format");
    if(format!=GUID_WICPixelFormat24bppBGR)throw Error("JPEG encoder did not accept BGR pixels");
    std::vector<std::uint8_t> bgr(bgra.size()*3);
    for(std::size_t i=0;i<bgra.size();++i){bgr[i*3]=static_cast<std::uint8_t>(bgra[i]);bgr[i*3+1]=static_cast<std::uint8_t>(bgra[i]>>8);bgr[i*3+2]=static_cast<std::uint8_t>(bgra[i]>>16);}
    checked(frame->WritePixels(height,width*3,static_cast<UINT>(bgr.size()),bgr.data()),"encode JPEG pixels");
    checked(frame->Commit(),"commit JPEG frame");checked(encoder->Commit(),"commit JPEG image");
    STATSTG stat{};checked(stream->Stat(&stat,STATFLAG_NONAME),"measure JPEG stream");
    if(!stat.cbSize.QuadPart||stat.cbSize.QuadPart>16*1024*1024)throw Error("Encoded screenshot size is outside its bound");
    LARGE_INTEGER start{};checked(stream->Seek(start,STREAM_SEEK_SET,nullptr),"rewind JPEG stream");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(stat.cbSize.QuadPart));ULONG read{};
    checked(stream->Read(result.data(),static_cast<ULONG>(result.size()),&read),"read JPEG stream");
    if(read!=result.size())throw Error("Short encoded screenshot read");
    return result;
}
std::filesystem::path publish(const std::filesystem::path &folder,const std::string &name,std::span<const std::uint8_t> bytes){
    GUID id{};checked(CoCreateGuid(&id),"allocate screenshot transaction identity");
    wchar_t suffix[48]{};if(!StringFromGUID2(id,suffix,48))throw Error("Cannot name screenshot transaction");
    const auto stage=folder/(std::wstring(L".capture-")+suffix+L".tmp");
    HANDLE file=CreateFileW(stage.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw Error("Cannot create screenshot transaction");
    bool owned=true;
    try{
        DWORD written{};
        if(!WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file))
            throw Error("Cannot completely write screenshot transaction");
        CloseHandle(file);file=INVALID_HANDLE_VALUE;
        for(unsigned index=1;index<=9999;++index){
            std::ostringstream number;number<<name<<'-'<<std::setw(4)<<std::setfill('0')<<index<<".jpg";
            const auto output=folder/number.str();
            if(MoveFileExW(stage.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH)){owned=false;return output;}
            const auto error=GetLastError();
            if(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS)throw Error("Cannot publish screenshot (Windows error "+std::to_string(error)+")");
        }
        throw Error("Native screenshot sequence is full");
    }catch(...){
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(owned)DeleteFileW(stage.c_str()); // Only the exclusively created transaction.
        throw;
    }
}
#endif
}
void Screenshot::mount(const std::filesystem::path &root){
    if(status_!=Status::none)throw Error("Cannot replace active screenshot storage");
    if(root.empty()){root_.clear();return;}
    const auto absolute=std::filesystem::absolute(root).lexically_normal();
    for(auto p=absolute;!p.empty();p=p.parent_path()){
        if(linked(p))throw Error("Screenshot storage contains a linked directory");
        if(p==p.parent_path())break;
    }
    directory(absolute);root_=absolute;
}
std::uint32_t Screenshot::init_start(psprecomp::GuestMemory &memory,std::uint32_t parameters){
    diagnostic_.clear();
    if(status_!=Status::none)return invalid_status;
    if((parameters&3u)||!memory.contains(parameters,4))return invalid_address;
    if(memory.load32(parameters)!=request_.size())return invalid_size;
    if(!memory.contains(parameters,request_.size()))return invalid_address;
    std::array<std::uint8_t,928> next{};memory.copy_out(parameters,next);
    try{
        // These fields are observed in this title's real R+Select caller.
        if(word(next,48)!=0||word(next,56)!=2||word(next,60)>100||word(next,296)!=1)
            throw Error("Unobserved screenshot startup/format/quality/name rule");
        if(word(next,68)>1024||!word(next,68)||word(next,72)>3||!word(next,84)||!word(next,88)||word(next,84)>1024||word(next,88)>1024||
           std::uint64_t(word(next,76))+word(next,84)>word(next,68)||std::uint64_t(word(next,80))+word(next,88)>1024)
            throw Error("Invalid screenshot framebuffer rectangle");
        (void)safe_name(next,92,12);const auto name=safe_name(next,104,192);
        const auto bytes=word(next,72)==3?4u:2u;
        const auto address=std::uint64_t(word(next,64))+(std::uint64_t(word(next,80))*word(next,68)+word(next,76))*bytes;
        if(address>std::numeric_limits<std::uint32_t>::max())throw Error("Screenshot address overflow");
        // Preflight conversion retains normal scanout format/mirror validation.
        (void)decode_framebuffer_bgra(memory,static_cast<std::uint32_t>(address),word(next,68),word(next,72),word(next,84),word(next,88));
        request_=next;parameters_=parameters;basename_=name;last_file_.clear();
        status_=Status::init;return 0;
    }catch(const Error &error){diagnostic_=error.what();return bad_parameters;}
}
std::uint32_t Screenshot::get_status(){
    const auto result=status_;
    if(status_==Status::init)status_=Status::visible;
    else if(status_==Status::finished)status_=Status::none;
    return static_cast<std::uint32_t>(result);
}
std::uint32_t Screenshot::update(psprecomp::GuestMemory &memory,std::uint32_t animation){
    diagnostic_.clear();
    if(status_!=Status::visible)return invalid_status;
    // This title passes an initially zero-initialized manager field here.
    // Native capture does not need a PSP dialog animation to copy the image.
    if(animation>2)return bad_parameters;
    if(!memory.contains(parameters_,request_.size()))return invalid_address;
    std::array<std::uint8_t,928> current{};memory.copy_out(parameters_,current);
    if(current!=request_){diagnostic_="Screenshot request changed while active";return bad_parameters;}
    std::uint32_t result=0;
    try{
        if(root_.empty())throw Error("Native screenshot storage is not mounted");
        directory(root_);const auto folder=root_/"screenshots";
        if(linked(folder))throw Error("Screenshot output directory is linked");
        std::filesystem::create_directory(folder);directory(folder);
        const auto bytes=word(request_,72)==3?4u:2u;
        const auto address=word(request_,64)+(word(request_,80)*word(request_,68)+word(request_,76))*bytes;
        const auto pixels=decode_framebuffer_bgra(memory,address,word(request_,68),word(request_,72),word(request_,84),word(request_,88));
#ifdef _WIN32
        last_file_=publish(folder,basename_,jpeg(pixels,word(request_,84),word(request_,88),word(request_,60)));
#else
        throw Error("Native JPEG screenshot encoding requires Windows WIC");
#endif
    }catch(const std::exception &error){result=access_error;diagnostic_=error.what();}
    // Done denotes a completed attempt; common.result distinguishes success
    // from an actual encoding/storage failure. Never report success first.
    memory.store32(parameters_+28,result);memory.store32(parameters_+52,1);
    status_=Status::quit;return 0;
}
std::uint32_t Screenshot::shutdown_start(){
    if(status_!=Status::quit)return invalid_status;
    status_=Status::finished;return 0;
}
}
