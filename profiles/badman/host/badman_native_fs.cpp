#include "badman_native_fs.hpp"
#include "psprecomp/guest_memory.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#endif

namespace badman {
namespace {
constexpr auto missing=static_cast<std::int32_t>(0x80010002u);
constexpr auto io_error=static_cast<std::int32_t>(0x80010005u);
constexpr auto denied=static_cast<std::int32_t>(0x8001000Du);
constexpr auto bad_address=static_cast<std::int32_t>(0x8001000Eu);
constexpr auto exists=static_cast<std::int32_t>(0x80010011u);
constexpr auto no_device=static_cast<std::int32_t>(0x80010013u);
constexpr auto invalid=static_cast<std::int32_t>(0x80010016u);
constexpr auto read_only=static_cast<std::int32_t>(0x8001001Eu);
std::string uppercase(std::string_view text) {
    std::string out(text);for(auto &c:out)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return out;
}
bool component(std::string_view name) {
    if(name.empty()||name.size()>240||name=="."||name==".."||name.back()=='.')return false;
    for(unsigned char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
        (c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
    const auto stem=uppercase(name.substr(0,name.find('.')));
    if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||stem=="CONIN$"||stem=="CONOUT$")return false;
    return !(stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9');
}
std::int32_t split(std::string_view text,std::vector<std::string> &parts) {
    if(text.empty()||text.size()>=1024)return invalid;
    const auto colon=text.find(':');
    const auto device=uppercase(text.substr(0,colon));
    if(device=="DISC0"||device=="FLASH0")return read_only;
    if(colon==std::string_view::npos||device!="MS0")return no_device;
    text.remove_prefix(colon+1);
    if(text.size()<2||text.front()!='/')return invalid;
    text.remove_prefix(1);
    while(!text.empty()) {
        const auto slash=text.find('/');const auto part=text.substr(0,slash);
        if(!component(part))return invalid;
        const auto upper=uppercase(part);
        if(upper=="BADMAN-NATIVE-V1"||upper.starts_with(".BADMAN"))return denied;
        parts.emplace_back(part);
        if(slash==std::string_view::npos)break;
        text.remove_prefix(slash+1);if(text.empty())return invalid;
    }
    if(parts.size()>=2&&uppercase(parts[0])=="PSP"&&uppercase(parts[1])=="SAVEDATA")return denied;
    return parts.empty()?invalid:0;
}
std::int32_t guest_string(const psprecomp::GuestMemory &memory,std::uint32_t at,std::string &out) {
    out.clear();
    for(std::uint32_t i=0;i<1024;++i) {
        if(static_cast<std::uint64_t>(at)+i>0xFFFFFFFFull||!memory.contains(at+i,1))return bad_address;
        const auto value=memory.load8(at+i);if(!value)return 0;out+=static_cast<char>(value);
    }
    return invalid;
}
#if defined(_WIN32)
class Handle {
public:
    explicit Handle(HANDLE value=INVALID_HANDLE_VALUE):value_(value){}
    ~Handle(){if(value_!=INVALID_HANDLE_VALUE)CloseHandle(value_);}
    Handle(const Handle &)=delete;Handle &operator=(const Handle &)=delete;
    Handle(Handle &&other)noexcept:value_(other.value_){other.value_=INVALID_HANDLE_VALUE;}
    Handle &operator=(Handle &&)=delete;
    HANDLE get()const{return value_;}
    explicit operator bool()const{return value_!=INVALID_HANDLE_VALUE;}
private:HANDLE value_;
};
struct FsError {std::int32_t result;};
std::int32_t native_error(DWORD error) {
    switch(error){
    case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return missing;
    case ERROR_ALREADY_EXISTS:case ERROR_FILE_EXISTS:return exists;
    case ERROR_ACCESS_DENIED:case ERROR_SHARING_VIOLATION:case ERROR_LOCK_VIOLATION:return denied;
    case ERROR_INVALID_NAME:case ERROR_BAD_PATHNAME:case ERROR_INVALID_PARAMETER:case ERROR_FILENAME_EXCED_RANGE:return invalid;
    case ERROR_WRITE_PROTECT:return read_only;
    case ERROR_NOT_SAME_DEVICE:return no_device;
    default:return io_error;
    }
}
[[noreturn]] void failed(){throw FsError{native_error(GetLastError())};}
std::wstring extended(const std::filesystem::path &path){return L"\\\\?\\"+path.native();}
struct NativeApi {
    decltype(&NtOpenFile) open{};
    using Rename=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,ULONG);
    Rename rename{};
    decltype(&RtlNtStatusToDosError) error{};
    NativeApi(){
        const auto library=GetModuleHandleW(L"ntdll.dll");if(!library)throw FsError{io_error};
        open=reinterpret_cast<decltype(open)>(GetProcAddress(library,"NtOpenFile"));
        rename=reinterpret_cast<Rename>(GetProcAddress(library,"NtSetInformationFile"));
        error=reinterpret_cast<decltype(error)>(GetProcAddress(library,"RtlNtStatusToDosError"));
        if(!open||!rename||!error)throw FsError{io_error};
    }
};
const NativeApi &native_api(){static const NativeApi api;return api;}
Handle open_relative(HANDLE parent,const std::wstring &name,bool directory) {
    if(name.empty()||name.size()>32766)throw FsError{invalid};
    UNICODE_STRING unicode{};unicode.Buffer=const_cast<wchar_t*>(name.data());
    unicode.Length=static_cast<USHORT>(name.size()*sizeof(wchar_t));unicode.MaximumLength=unicode.Length;
    OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=parent;
    attributes.ObjectName=&unicode;attributes.Attributes=OBJ_CASE_INSENSITIVE|OBJ_DONT_REPARSE;
    IO_STATUS_BLOCK iosb{};HANDLE file=INVALID_HANDLE_VALUE;
    const auto options=FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT|
        (directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE);
    const auto access=FILE_READ_ATTRIBUTES|SYNCHRONIZE|(directory?FILE_TRAVERSE:DELETE);
    const auto status=native_api().open(&file,access,&attributes,&iosb,
        directory?(FILE_SHARE_READ|FILE_SHARE_WRITE):0,options);
    Handle result(file);
    if(status<0||status==0x103)throw FsError{native_error(native_api().error(status))};
    return result;
}
BY_HANDLE_FILE_INFORMATION information(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION out{};if(!GetFileInformationByHandle(handle,&out))failed();return out;
}
Handle pin_directory(const std::filesystem::path &path) {
    // This is used only for the drive root. All remaining opens are relative to
    // pinned directory handles and reject reparses. Deny directory deletion;
    // Windows rename itself needs directory write sharing.
    Handle handle(CreateFileW(extended(path).c_str(),FILE_READ_ATTRIBUTES|FILE_TRAVERSE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!handle)failed();const auto info=information(handle.get());
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||
       GetFileType(handle.get())!=FILE_TYPE_DISK)throw FsError{denied};
    return handle;
}
std::filesystem::path checked_root(const std::filesystem::path &root,std::vector<Handle> &pins) {
    const auto name=root.root_name().native();
    if(root.empty()||!root.is_absolute()||name.size()!=2||name[1]!=L':'||
       !((name[0]>=L'A'&&name[0]<=L'Z')||(name[0]>=L'a'&&name[0]<=L'z'))||root==root.root_path())throw FsError{no_device};
    auto path=root.root_path();pins.push_back(pin_directory(path));
    for(const auto &part:root.relative_path()) {
        const auto value=part.native();
        if(value.empty()||value==L"."||value==L".."||value.back()==L'.'||value.back()==L' '||
           value.find(L':')!=std::wstring::npos)throw FsError{invalid};
        auto next=open_relative(pins.back().get(),value,true);const auto info=information(next.get());
        if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))throw FsError{denied};
        path/=part;pins.push_back(std::move(next));
    }
    return path;
}
std::filesystem::path parent_path(std::filesystem::path path,const std::vector<std::string> &parts,std::vector<Handle> &pins) {
    // Begin from the separately retained mounted-root handle. Relative opens do
    // not re-resolve an absolute path through concurrently changed ancestors.
    for(std::size_t i=0;i+1<parts.size();++i){
        const std::wstring name(parts[i].begin(),parts[i].end());auto next=open_relative(pins.back().get(),name,true);
        const auto info=information(next.get());if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw FsError{denied};
        path/=name;pins.push_back(std::move(next));
    }
    return path;
}
#endif
}
#if defined(_WIN32) && defined(BADMAN_NATIVE_FS_TEST_HOOK)
namespace detail {void native_fs_before_rename(const std::filesystem::path &,const std::filesystem::path &,HANDLE);}
#endif
std::int32_t rename_native_file(const std::filesystem::path &root,std::string_view old_name,std::string_view new_name) noexcept {
    try {
        std::vector<std::string> old_parts,new_parts;
        const auto old_result=split(old_name,old_parts),new_result=split(new_name,new_parts);
        if(old_result)return old_result;if(new_result)return new_result;
        if(uppercase(old_name)==uppercase(new_name))return exists;
#if defined(_WIN32)
        std::vector<Handle> source_pins,target_pins;const auto mount=checked_root(root,source_pins);
        const auto target_mount=checked_root(root,target_pins);
        const auto source_parent=parent_path(mount,old_parts,source_pins);
        const auto target_parent=parent_path(target_mount,new_parts,target_pins);
        const auto target_directory=target_pins.back().get();
        const auto source=source_parent/std::wstring(old_parts.back().begin(),old_parts.back().end());
        Handle file=open_relative(source_pins.back().get(),std::wstring(old_parts.back().begin(),old_parts.back().end()),false);
        if(!file)failed();const auto info=information(file.get());
        if((info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||
           info.nNumberOfLinks!=1||GetFileType(file.get())!=FILE_TYPE_DISK)return denied;
        const auto target=target_parent/std::wstring(new_parts.back().begin(),new_parts.back().end());
        // The Win32 wrapper rejects a relative RootDirectory on this system.
        // The documented native FileRenameInformation form binds the target to
        // the checked directory object, with no absolute-name reparsing.
        const std::wstring name(new_parts.back().begin(),new_parts.back().end());
        const auto byte_count=static_cast<DWORD>(name.size()*sizeof(wchar_t));
        const auto size=sizeof(FILE_RENAME_INFO)+byte_count;
        auto storage=std::make_unique<std::byte[]>(size);std::memset(storage.get(),0,size);
        auto *rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
        rename->ReplaceIfExists=FALSE;rename->RootDirectory=target_directory;rename->FileNameLength=byte_count;
        std::memcpy(rename->FileName,name.data(),byte_count);
#if defined(BADMAN_NATIVE_FS_TEST_HOOK)
        detail::native_fs_before_rename(source,target,file.get());
#endif
        IO_STATUS_BLOCK iosb{};
        auto status=native_api().rename(file.get(),&iosb,rename,static_cast<ULONG>(size),10u);
        if(status==0x103){
            // Synchronous source handles normally complete in the call. Retain
            // every parent and source handle until any pending I/O really ends.
            if(WaitForSingleObject(file.get(),INFINITE)!=WAIT_OBJECT_0)return io_error;
            status=iosb.Status;
        }
        if(status<0)return native_error(native_api().error(status));
        return status==0?0:io_error;
#else
        (void)root;return io_error;
#endif
#if defined(_WIN32)
    }catch(const FsError &error){return error.result;
#endif
    }catch(...){return io_error;}
}
std::int32_t rename_native_file_guest(const psprecomp::GuestMemory &memory,
    const std::filesystem::path &root,std::uint32_t old_name,std::uint32_t new_name) noexcept {
    try{
        std::string old_text,new_text;const auto old_result=guest_string(memory,old_name,old_text);
        const auto new_result=guest_string(memory,new_name,new_text);
        if(old_result)return old_result;if(new_result)return new_result;
        return rename_native_file(root,old_text,new_text);
    }catch(...){return io_error;}
}
}
