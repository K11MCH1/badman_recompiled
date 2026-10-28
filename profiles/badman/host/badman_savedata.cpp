#include "badman_savedata.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/sha256.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace badman {
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint32_t parameter_size=0x600,sfo_size=0x504,max_payload=1024*1024,max_image=2*1024*1024;
constexpr std::size_t max_blob=10*1024*1024;
constexpr const char *owner_name="OWNER.txt";
const std::string owner_text="PSPRecomp Badman private native saves\nschema=1\nNot compatible with PSP or PPSSPP saves. Plaintext guest payload; not PSP secure encryption.\n";
const std::string ref_prefix="PSPRECOMP_BADMAN_NATIVE_REFERENCE_V1\nsha256=";
struct StoreError:std::runtime_error {using std::runtime_error::runtime_error;};
#if !defined(_WIN32)
// Move a file to a new name without ever replacing an existing destination.
// Linux renameat2(RENAME_NOREPLACE) also works where hard links are refused
// (Android app storage); link()+unlink() remains the fallback elsewhere.
enum class Publish {done,not_published,source_retained};
Publish publish_no_replace(const std::filesystem::path &from,const std::filesystem::path &to){
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned rename_noreplace=1u;
    if(::syscall(SYS_renameat2,AT_FDCWD,from.c_str(),AT_FDCWD,to.c_str(),rename_noreplace)==0)return Publish::done;
    if(errno!=ENOSYS&&errno!=EINVAL)return Publish::not_published;
#endif
    if(::link(from.c_str(),to.c_str())!=0)return Publish::not_published;
    return ::unlink(from.c_str())==0?Publish::done:Publish::source_retained;
}
#endif
struct BrokenStoreError:StoreError {using StoreError::StoreError;};
bool bounded_string(const psprecomp::GuestMemory &memory,std::uint32_t address,std::uint32_t limit,std::string &out){
    if(!memory.contains(address,limit))return false;
    out.clear();for(std::uint32_t i=0;i<limit;++i){const auto c=memory.load8(address+i);if(!c)return true;out+=static_cast<char>(c);}return false;
}
bool component(const std::string &text,bool filename=false){
    if(text.empty()||text=="."||text==".."||text.back()=='.'||text.back()==' ')return false;
    for(unsigned char c:text)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||(filename&&c=='.')))return false;
    auto stem=text.substr(0,text.find('.'));for(auto &c:stem)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
    if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL")return false;
    return !(stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9');
}
bool overlaps(std::uint32_t a,std::uint32_t as,std::uint32_t b,std::uint32_t bs){
    a=psprecomp::GuestMemory::canonical(a);b=psprecomp::GuestMemory::canonical(b);
    return std::uint64_t(a)<std::uint64_t(b)+bs&&std::uint64_t(b)<std::uint64_t(a)+as;
}
bool linked(const std::filesystem::path &path){
#if defined(_WIN32)
    const auto attrs=GetFileAttributesW(path.c_str());
    return attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path));
#endif
}
bool path_exists(const std::filesystem::path &path){
    std::error_code error;const auto s=std::filesystem::symlink_status(path,error);
    if(error==std::errc::no_such_file_or_directory)return false;
    if(error)throw StoreError("Cannot inspect native save path");
    return std::filesystem::exists(s);
}
void regular(const std::filesystem::path &path){
    if(linked(path)||!std::filesystem::is_regular_file(path)||std::filesystem::hard_link_count(path)!=1)
        throw StoreError("Preserving a linked or non-regular native save file");
}
Bytes read(const std::filesystem::path &path,std::size_t maximum){
    regular(path);const auto size=std::filesystem::file_size(path);if(size>maximum)throw StoreError("Native save exceeds its bounded size");
    Bytes result(static_cast<std::size_t>(size));std::ifstream file(path,std::ios::binary);
    if(!file||!file.read(reinterpret_cast<char *>(result.data()),static_cast<std::streamsize>(result.size()))||file.peek()!=EOF)
        throw StoreError("Cannot read a complete native save file");
    regular(path);return result;
}
bool store_ready(const std::filesystem::path &root){
    if(root.empty()||linked(root)||!std::filesystem::is_directory(root))throw StoreError("Memory Stick backing directory is unavailable");
    const auto store=root/Savedata::native_directory_name;
    if(!path_exists(store))return false;
    if(linked(store)||!std::filesystem::is_directory(store)||std::filesystem::canonical(store).parent_path()!=root)
        throw StoreError("Preserving an unsafe native save directory");
    const auto marker=read(store/owner_name,4096);
    if(std::string(marker.begin(),marker.end())!=owner_text)throw StoreError("Preserving a native save directory with an unrecognized owner/schema");
    return true;
}
std::string nonce(){static std::atomic<std::uint64_t> sequence{};return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence);}
void write_exclusive(const std::filesystem::path &path,const Bytes &data){
#if defined(_WIN32)
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw StoreError("Cannot create native save staging file");
    DWORD written=0;const bool ok=data.size()<=std::numeric_limits<DWORD>::max()&&WriteFile(file,data.data(),static_cast<DWORD>(data.size()),&written,nullptr)&&written==data.size()&&FlushFileBuffers(file);
    CloseHandle(file);if(!ok)throw StoreError("Cannot write/flush complete native save; previous save preserved");
#else
    const int file=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(file<0)throw StoreError("Cannot create native save staging file");
    std::size_t done=0;while(done<data.size()){const auto n=::write(file,data.data()+done,data.size()-done);if(n<=0)break;done+=static_cast<std::size_t>(n);}const bool ok=done==data.size()&&::fsync(file)==0;::close(file);
    if(!ok)throw StoreError("Cannot write/flush complete native save; previous save preserved");
#endif
}
void write_exclusive(const std::filesystem::path &path,const std::string &text){write_exclusive(path,Bytes(text.begin(),text.end()));}
void remove_owned_file(const std::filesystem::path &path,const std::filesystem::path &parent) noexcept {
    try{if(path.parent_path()==parent&&!linked(path)&&std::filesystem::is_regular_file(path)&&std::filesystem::hard_link_count(path)==1)std::filesystem::remove(path);}catch(...){}
}
void create_store(const std::filesystem::path &root){
    if(store_ready(root))return;
    const auto stage=root/(".badman-native-store-"+nonce()),destination=root/Savedata::native_directory_name;
    if(!std::filesystem::create_directory(stage))throw StoreError("Native save staging directory already exists");
    try{
        write_exclusive(stage/owner_name,owner_text);
#if defined(_WIN32)
        if(!MoveFileExW(stage.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)&&!store_ready(root))throw StoreError("Cannot publish native save store");
#else
        if(path_exists(destination)){if(!store_ready(root))throw StoreError("Native save destination changed");}
        else std::filesystem::rename(stage,destination);
#endif
    }catch(...){remove_owned_file(stage/owner_name,stage);std::error_code error;std::filesystem::remove(stage,error);throw;}
    remove_owned_file(stage/owner_name,stage);std::error_code error;std::filesystem::remove(stage,error);
}
struct Lock {
    std::filesystem::path path;
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int handle{-1};
#endif
    explicit Lock(const std::filesystem::path &store):path(store/".commit-lock"){
#if defined(_WIN32)
        handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw StoreError("Another save commit is active, or its lock path is occupied; existing data preserved");
#else
        handle=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(handle<0)throw StoreError("Native save commit lock is occupied");
#endif
    }
    ~Lock(){
#if defined(_WIN32)
        if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
#else
        if(handle>=0){::close(handle);::unlink(path.c_str());}
#endif
    }
};
bool digest_text(const std::string &text){return text.size()==64&&std::all_of(text.begin(),text.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string receipt(const std::filesystem::path &reference){
    const auto bytes=read(reference,256);const std::string text(bytes.begin(),bytes.end());
    if(text.size()!=ref_prefix.size()+65||!text.starts_with(ref_prefix)||text.back()!='\n')throw BrokenStoreError("Unrecognized/incomplete native save reference; preserved");
    const auto sha=text.substr(ref_prefix.size(),64);if(!digest_text(sha))throw BrokenStoreError("Invalid native save receipt; preserved");return sha;
}
std::filesystem::path reference(const std::filesystem::path &store,const std::string &game,const std::string &slot,const std::string &file){return store/(game+"-"+slot+"-"+file+".ref");}
struct Record {std::string game,slot,file;Bytes sfo,payload;std::array<Bytes,4> images;};
void word(Bytes &out,std::uint32_t value){for(unsigned n=0;n<4;++n)out.push_back(static_cast<std::uint8_t>(value>>(n*8)));}
Bytes encode(const Record &record){
    Bytes bytes{'B','A','D','N','S','A','V','1'};
    for(const auto value:{1u,static_cast<std::uint32_t>(record.game.size()),static_cast<std::uint32_t>(record.slot.size()),static_cast<std::uint32_t>(record.file.size()),static_cast<std::uint32_t>(record.payload.size()),sfo_size})word(bytes,value);
    for(const auto &image:record.images)word(bytes,static_cast<std::uint32_t>(image.size()));
    for(const auto *text:{&record.game,&record.slot,&record.file})bytes.insert(bytes.end(),text->begin(),text->end());
    bytes.insert(bytes.end(),record.sfo.begin(),record.sfo.end());for(const auto &image:record.images)bytes.insert(bytes.end(),image.begin(),image.end());
    bytes.insert(bytes.end(),record.payload.begin(),record.payload.end());return bytes;
}
Record load_record(const std::filesystem::path &store,const std::string &hash,const std::string &game,const std::string &slot,const std::string &file){
    if(!digest_text(hash))throw StoreError("Invalid native save digest");
    const auto path=store/("sha256-"+hash+".bsav");regular(path);
    if(std::filesystem::file_size(path)>max_blob)throw StoreError("Native save exceeds its bounded size");
    if(psprecomp::sha256_file(path)!=hash)throw BrokenStoreError("Corrupted native save blob; preserved");
    const auto bytes=read(path,max_blob);if(bytes.size()<48||!std::equal(bytes.begin(),bytes.begin()+8,"BADNSAV1"))throw StoreError("Unrecognized native save blob schema; preserved");
    std::size_t cursor=8;auto take_word=[&]{if(cursor+4>bytes.size())throw StoreError("Truncated native save header");std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::uint32_t(bytes[cursor++])<<(n*8);return v;};
    if(take_word()!=1)throw StoreError("Unsupported native save version");
    const auto gn=take_word(),sn=take_word(),fn=take_word(),pn=take_word(),sf=take_word();std::array<std::uint32_t,4> sizes;for(auto &size:sizes)size=take_word();
    if(!gn||gn>12||!sn||sn>19||!fn||fn>12||!pn||pn>max_payload||sf!=sfo_size||std::any_of(sizes.begin(),sizes.end(),[](auto n){return n>max_image;}))throw StoreError("Invalid native save extent");
    auto take=[&](std::size_t n){if(n>bytes.size()-cursor)throw StoreError("Truncated native save content");Bytes value(bytes.begin()+cursor,bytes.begin()+cursor+n);cursor+=n;return value;};
    auto text=[&](auto n){const auto value=take(n);return std::string(value.begin(),value.end());};
    Record result;result.game=text(gn);result.slot=text(sn);result.file=text(fn);
    if(result.game!=game||result.slot!=slot||result.file!=file)throw StoreError("Native save game/slot/file identity mismatch; preserved");
    result.sfo=take(sf);for(unsigned n=0;n<4;++n)result.images[n]=take(sizes[n]);result.payload=take(pn);
    if(cursor!=bytes.size()||psprecomp::sha256_file(path)!=hash)throw StoreError("Native save changed or has trailing data");
    return result;
}
void commit_record(const std::filesystem::path &root,const Record &record,const std::string &previous){
    create_store(root);const auto store=root/Savedata::native_directory_name;Lock lock(store);
    if(!store_ready(root))throw StoreError("Native save store disappeared before commit");
    const auto ref=reference(store,record.game,record.slot,record.file);
    if(path_exists(ref)){
        if(previous.empty()||receipt(ref)!=previous)throw StoreError("Save slot changed since selection; previous data preserved");
        load_record(store,previous,record.game,record.slot,record.file);
    }else if(!previous.empty())throw StoreError("Selected save slot disappeared; no replacement was written");
    const auto temp=store/(".pending-"+nonce()+".tmp"),tempref=store/(".reference-"+nonce()+".tmp");
    try{
        write_exclusive(temp,encode(record));const auto hash=psprecomp::sha256_file(temp);const auto blob=store/("sha256-"+hash+".bsav");
        if(path_exists(blob)){load_record(store,hash,record.game,record.slot,record.file);remove_owned_file(temp,store);}
        else{
#if defined(_WIN32)
            if(!MoveFileExW(temp.c_str(),blob.c_str(),MOVEFILE_WRITE_THROUGH))throw StoreError("Cannot publish verified native save blob");
#else
            std::filesystem::rename(temp,blob);
#endif
        }
        load_record(store,hash,record.game,record.slot,record.file);
        write_exclusive(tempref,ref_prefix+hash+"\n");
        if(previous.empty()){
#if defined(_WIN32)
            if(!MoveFileExW(tempref.c_str(),ref.c_str(),MOVEFILE_WRITE_THROUGH))throw StoreError("Cannot commit new native save reference; destination preserved");
#else
            const auto published=publish_no_replace(tempref,ref);
            if(published==Publish::not_published)throw StoreError("Cannot exclusively publish native save reference");
            if(published==Publish::source_retained)remove_owned_file(tempref,store);
#endif
        }else{
            if(receipt(ref)!=previous)throw StoreError("Save slot changed before commit; previous data preserved");
            const auto backup=store/("previous-"+nonce()+".ref");
#if defined(_WIN32)
            if(!ReplaceFileW(ref.c_str(),tempref.c_str(),backup.c_str(),0,nullptr,nullptr))throw StoreError("Atomic native save replacement failed; previous data retained");
#else
            std::filesystem::copy_file(ref,backup,std::filesystem::copy_options::none);std::filesystem::rename(tempref,ref);
#endif
        }
        if(receipt(ref)!=hash)throw StoreError("Native save reference verification failed after commit");
    }catch(...){remove_owned_file(temp,store);remove_owned_file(tempref,store);throw;}
}
void delete_record(const std::filesystem::path &root,const std::string &game,const std::string &slot,const std::string &file,const std::string &previous){
    if(!store_ready(root))throw StoreError("Native save store disappeared before deletion; no data changed");
    const auto store=root/Savedata::native_directory_name;Lock lock(store);
    if(!store_ready(root))throw StoreError("Native save store changed before deletion");
    const auto ref=reference(store,game,slot,file);
    if(previous.empty()||!path_exists(ref)||receipt(ref)!=previous)throw StoreError("Save slot changed since selection; nothing was deleted");
    load_record(store,previous,game,slot,file);
    const auto history=store/("deleted-"+nonce()+".ref");
#if defined(_WIN32)
    if(!MoveFileExW(ref.c_str(),history.c_str(),MOVEFILE_WRITE_THROUGH))throw StoreError("Cannot atomically deactivate native save; existing data retained");
#else
    const auto moved=publish_no_replace(ref,history);
    if(moved==Publish::not_published)throw StoreError("Cannot retain native save deletion receipt");
    if(moved==Publish::source_retained)throw StoreError("Cannot deactivate native save; recovery receipt retained");
#endif
    if(path_exists(ref)||receipt(history)!=previous)throw StoreError("Native deletion verification failed; recovery history retained");
}
Bytes snapshot(const psprecomp::GuestMemory &memory,std::uint32_t address,std::uint32_t size){Bytes result(size);memory.copy_out(address,result);return result;}
}

struct Savedata::Operation {
    enum class Phase {information,selection,overwrite,delete_confirmation};
    enum class Kind {empty,ready,corrupt};
    struct Slot {std::string name,hash;Kind kind{Kind::empty};};
    std::uint32_t mode{},buffer{},capacity{},size{};
    bool overwrite{};
    std::string game,file;
    std::array<std::uint32_t,4> image_buffers{},image_capacities{};
    Record record;
    std::vector<Slot> slots;
    std::size_t selected{};
    Phase phase{Phase::information};
    bool automatic{};
};
Savedata::Savedata()=default;
Savedata::~Savedata()=default;
void Savedata::mount_memory_stick(const std::filesystem::path &root){
    if(status_!=Status::none)throw psprecomp::Error("Cannot change Memory Stick mount during a savedata operation");
    if(root.empty()){root_.clear();return;}
    if(linked(root))throw psprecomp::Error("Memory Stick mount cannot be a symbolic link or junction");
    const auto resolved=std::filesystem::canonical(root);
    if(!std::filesystem::is_directory(resolved))throw psprecomp::Error("Memory Stick mount must be an existing owned directory");
    root_=resolved;
}
void Savedata::set_automatic_profile(bool enabled){
    if(status_!=Status::none)throw psprecomp::Error("Cannot change the save profile mode during a savedata operation");
    automatic_profile_=enabled;
}
bool Savedata::has_native_records(const std::string &game,const std::string &file) const{
    if(root_.empty()||!store_ready(root_))return false;
    const auto prefix=game+"-",suffix="-"+file+".ref";
    std::error_code error;
    for(const auto &entry:std::filesystem::directory_iterator(root_/native_directory_name,error)){
        const auto name=entry.path().filename().string();
        if(name.size()>prefix.size()+suffix.size()&&name.starts_with(prefix)&&name.ends_with(suffix))return true;
    }
    return false;
}
std::uint32_t Savedata::init_start(psprecomp::GuestMemory &memory,std::uint32_t parameters){
    diagnostic_.clear();auto fail=[&](auto code,const char *message){diagnostic_=message;return code;};
    if(status_!=Status::none)return fail(invalid_status,"Savedata utility is already active");
    if((parameters&3u)||!memory.contains(parameters,4))return fail(invalid_address,"Invalid savedata parameter pointer");
    if(memory.load32(parameters)!=parameter_size)return fail(invalid_size,"Only the observed 0x600-byte savedata ABI is implemented");
    if(!memory.contains(parameters,parameter_size))return fail(invalid_address,"Truncated savedata parameter block");
    const auto mode=memory.load32(parameters+0x30);
    if(mode!=4&&mode!=5&&mode!=7)throw psprecomp::Error("Only LISTLOAD/LISTSAVE/LISTALLDELETE have native-host persistence; other savedata modes are not implemented");
    const auto bad=mode==5?0x80110388u:mode==7?delete_bad_parameters:bad_parameters;
    const auto button_swap=memory.load32(parameters+8);
    if(button_swap>1||memory.load32(parameters+0x34)||memory.load32(parameters+0x38)>1)
        return fail(bad,"Invalid savedata button/bind/overwrite parameters");
    if(memory.load32(parameters+0x5ec)||memory.load32(parameters+0x5f0))throw psprecomp::Error("Unobserved savedata secureVersion/multiStatus is not implemented");
    if(memory.load32(parameters+0x5c8)!=0)throw psprecomp::Error("Only the observed initial savedata focus is implemented");
    auto operation=std::make_unique<Operation>();operation->mode=mode;operation->overwrite=memory.load32(parameters+0x38)!=0;
    std::string initial;
    if(!bounded_string(memory,parameters+0x3c,13,operation->game)||!component(operation->game)||
       !bounded_string(memory,parameters+0x4c,20,initial)||(!initial.empty()&&!component(initial))||
       !bounded_string(memory,parameters+0x64,13,operation->file)||!component(operation->file,true))
        return fail(bad,"Invalid/escaping savedata name component");
    std::vector<std::pair<std::uint32_t,std::uint32_t>> outputs;
    if(mode!=7){
    operation->buffer=memory.load32(parameters+0x74);operation->capacity=memory.load32(parameters+0x78);operation->size=memory.load32(parameters+0x7c);
    if(!operation->capacity||operation->capacity>max_payload||operation->size>operation->capacity||(mode==5&&!operation->size)||
       !memory.contains(operation->buffer,operation->capacity)||overlaps(parameters,parameter_size,operation->buffer,operation->capacity))
        return fail(bad,"Invalid/aliased savedata payload buffer");
    outputs.emplace_back(operation->buffer,operation->capacity);
    for(unsigned index=0;index<4;++index){
        const auto offset=0x584+index*16,address=memory.load32(parameters+offset),capacity=memory.load32(parameters+offset+4),used=memory.load32(parameters+offset+8);
        if(capacity>max_image||used>capacity||(capacity&&(!memory.contains(address,capacity)||overlaps(parameters,parameter_size,address,capacity))))
            return fail(bad,"Invalid savedata image buffer");
        if(mode==4&&capacity){for(const auto &[other,count]:outputs)if(overlaps(address,capacity,other,count))return fail(bad,"Aliased savedata output buffers");outputs.emplace_back(address,capacity);}
        operation->image_buffers[index]=address;operation->image_capacities[index]=capacity;
        if(mode==5&&used)operation->record.images[index]=snapshot(memory,address,used);
    }
    }
    const auto list=memory.load32(parameters+0x60);if(!list)return fail(bad,"LISTLOAD/LISTSAVE requires a bounded save-name list");
    std::vector<std::string> slots;std::set<std::string> unique;bool terminated=false;
    for(std::uint32_t index=0;index<100;++index){
        const auto address=std::uint64_t(list)+index*20u;std::string slot;
        if(address>0xffffffffu||!bounded_string(memory,static_cast<std::uint32_t>(address),20,slot))return fail(bad,"Invalid/unterminated savedata slot list");
        if(slot.empty()){terminated=true;break;}
        auto folded=slot;for(auto &c:folded)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
        if(!component(slot)||!unique.insert(folded).second)return fail(bad,"Invalid/duplicate savedata slot name");
        slots.push_back(slot);
    }
    if(!terminated||slots.empty())return fail(bad,"Save-name list exceeds the verified 99-slot bound");
    if(mode==4)for(const auto &[out,size]:outputs)if(overlaps(out,size,list,static_cast<std::uint32_t>((slots.size()+1)*20)))return fail(bad,"Savedata output aliases the slot-name list");
    operation->record.game=operation->game;operation->record.file=operation->file;
    if(mode==5){operation->record.payload=snapshot(memory,operation->buffer,operation->size);operation->record.sfo=snapshot(memory,parameters+0x80,sfo_size);}
    std::uint32_t result=mode==5?0x80110381u:mode==7?delete_no_media:no_media;
    std::string message="No Memory Stick backing directory is mounted or available.";
    if(!root_.empty()&&!linked(root_)&&std::filesystem::is_directory(root_)){
        bool native_ready=false,legacy=false;
        try{
            native_ready=store_ready(root_);
            if(mode==4){
                auto legacy_root=root_;bool available=true;
                for(const char *part:{"PSP","SAVEDATA"}){
                    legacy_root/=part;if(!path_exists(legacy_root)){available=false;break;}
                    if(linked(legacy_root)||!std::filesystem::is_directory(legacy_root))throw StoreError("PSP savedata path is unsafe/non-directory and was preserved");
                }
                if(available)for(const auto &slot:slots)if(path_exists(legacy_root/(operation->game+slot)))legacy=true;
            }
            const auto store=root_/native_directory_name;
            for(const auto &name:slots){
                Operation::Slot slot;slot.name=name;
                if(native_ready&&path_exists(reference(store,operation->game,name,operation->file))){
                    try{slot.hash=receipt(reference(store,operation->game,name,operation->file));load_record(store,slot.hash,operation->game,name,operation->file);slot.kind=Operation::Kind::ready;}
                    catch(const std::exception &){slot.kind=Operation::Kind::corrupt;}
                }
                if(mode==5||slot.kind!=Operation::Kind::empty)operation->slots.push_back(std::move(slot));
            }
            if(mode==4&&operation->slots.empty()&&legacy)
                throw psprecomp::Error("PSP-format savedata exists and was preserved. This port reads only its explicitly private native-host saves, not PSP secure savedata.");
            if(!operation->slots.empty()){
                result=0;operation->phase=Operation::Phase::selection;
                const auto selected=std::find_if(operation->slots.begin(),operation->slots.end(),[&](const auto &slot){return slot.name==initial;});
                if(selected!=operation->slots.end())operation->selected=static_cast<std::size_t>(selected-operation->slots.begin());
            }else{result=mode==7?delete_no_data:no_data;message=mode==7?"There are no active private native saves for this game. PSP saves are not included.":"There is no saved data for this game on the mounted Memory Stick.";}
        }catch(const psprecomp::Error &){throw;}
        catch(const std::exception &error){result=mode==5?save_access_error:mode==7?delete_access_error:access_error;message=error.what();operation->slots.clear();operation->phase=Operation::Phase::information;}
    }
    if(automatic_profile_&&mode!=7){
        // Delete always keeps its explicit confirmations.
        auto &op=*operation;
        const auto ready=[&](std::size_t index){return op.slots[index].kind==Operation::Kind::ready;};
        if(op.phase==Operation::Phase::selection&&mode==4){
            // The game's remembered slot first, then the profile's first saved slot.
            if(!ready(op.selected))for(std::size_t index=0;index<op.slots.size();++index)if(ready(index)){op.selected=index;break;}
            op.automatic=ready(op.selected);
        }else if(op.phase==Operation::Phase::selection&&mode==5){
            const auto &slot=op.slots[op.selected];
            op.automatic=slot.kind==Operation::Kind::empty||(slot.kind==Operation::Kind::ready&&op.overwrite);
        }else if(mode==4&&result==no_data)op.automatic=true;
    }
    const bool automatic=operation->automatic;
    operation_=std::move(operation);parameters_=parameters;pending_result_=result;
    confirm_button_=button_swap?0x4000u:0x2000u;message_=message;
    ack_pending_=cancel_pending_=ack_armed_=finished_reported_=false;previous_buttons_=0;status_=Status::init;
    ack_pending_=automatic;return 0;
}
std::uint32_t Savedata::get_status(){
    diagnostic_.clear();if(status_==Status::init){status_=Status::visible;return 1;}
    if(status_==Status::finished){if(!finished_reported_){finished_reported_=true;return 4;}status_=Status::none;operation_.reset();parameters_=0;message_.clear();return 0;}
    return static_cast<std::uint32_t>(status_);
}
std::optional<Savedata::Dialog> Savedata::dialog() const{
    if(status_!=Status::visible||!operation_)return {};
    const auto &op=*operation_;const bool save=op.mode==5;
    if(op.automatic)return {};
    const std::string title=save?"Save native game data":op.mode==7?"Delete private native save":"Load saved data";
    const std::string warning="Native saves only (not PSP compatible).\n";
    if(op.phase==Operation::Phase::information)return Dialog{title,message_,pending_result_,!ack_pending_,"Press the game's confirm button to continue."};
    const auto &slot=op.slots.at(op.selected);
    if(op.phase==Operation::Phase::overwrite)return Dialog{title,warning+"Overwrite "+slot.name+"? Previous save retained.",0,!ack_pending_,"Confirm again to overwrite. Cancel leaves data unchanged."};
    if(op.phase==Operation::Phase::delete_confirmation)return Dialog{title,warning+"Delete "+slot.name+" from the active native list? Recovery data retained.",0,!ack_pending_,"Confirm again to delete. Cancel leaves data unchanged."};
    const auto kind=slot.kind==Operation::Kind::ready?"Saved native data":slot.kind==Operation::Kind::empty?"Empty native slot":"Corrupt or unrecognized native data (preserved)";
    return Dialog{title,warning+slot.name+" ("+std::to_string(op.selected+1)+"/"+std::to_string(op.slots.size())+"): "+kind,0,!ack_pending_,"Up/Down: select. Confirm: continue. Cancel: no changes."};
}
bool Savedata::acknowledge(){if(status_!=Status::visible||ack_pending_||cancel_pending_)return false;ack_pending_=true;return true;}
bool Savedata::cancel(){if(status_!=Status::visible||!operation_||operation_->phase==Operation::Phase::information||ack_pending_||cancel_pending_)return false;cancel_pending_=true;return true;}
bool Savedata::select_slot(const std::string &name){
    if(status_!=Status::visible||!operation_||operation_->phase!=Operation::Phase::selection||ack_pending_||cancel_pending_)return false;
    const auto found=std::find_if(operation_->slots.begin(),operation_->slots.end(),[&](const auto &slot){return slot.name==name;});
    if(found==operation_->slots.end())return false;operation_->selected=static_cast<std::size_t>(found-operation_->slots.begin());return true;
}
void Savedata::observe_buttons(std::uint32_t buttons){
    if(status_==Status::visible&&operation_){
        const auto cancel_button=confirm_button_==0x4000u?0x2000u:0x4000u;
        if(!(buttons&(confirm_button_|cancel_button)))ack_armed_=true;
        if(operation_->phase==Operation::Phase::selection&&!ack_pending_&&!cancel_pending_){
            if((buttons&0x10u)&&!(previous_buttons_&0x10u))operation_->selected=(operation_->selected+operation_->slots.size()-1)%operation_->slots.size();
            if((buttons&0x40u)&&!(previous_buttons_&0x40u))operation_->selected=(operation_->selected+1)%operation_->slots.size();
        }
        if(ack_armed_&&(buttons&confirm_button_)&&!(previous_buttons_&confirm_button_))acknowledge();
        else if(ack_armed_&&(buttons&cancel_button)&&!(previous_buttons_&cancel_button))cancel();
    }
    previous_buttons_=buttons;
}
std::uint32_t Savedata::update(psprecomp::GuestMemory &memory,std::uint32_t animation_speed){
    diagnostic_.clear();if(status_!=Status::visible||!operation_){diagnostic_="Savedata Update requires a visible operation";return invalid_status;}
    if(animation_speed!=1&&animation_speed!=2)throw psprecomp::Error("Unsupported savedata Update argument; observed value is 2");
    if(!ack_pending_&&!cancel_pending_)return 0;
    auto &op=*operation_;auto finish=[&](std::uint32_t result){pending_result_=result;memory.store32(parameters_+0x1c,result);status_=Status::quit;ack_pending_=cancel_pending_=false;return 0u;};
    if(!memory.contains(parameters_,parameter_size))return invalid_address;
    if(cancel_pending_)return finish(user_cancelled);
    ack_pending_=false;
    if(op.phase==Operation::Phase::information)return finish(pending_result_);
    auto &slot=op.slots.at(op.selected);
    if(op.mode==7&&slot.kind==Operation::Kind::ready&&op.phase==Operation::Phase::selection){op.phase=Operation::Phase::delete_confirmation;ack_armed_=false;return 0;}
    if(op.mode==5&&slot.kind==Operation::Kind::ready&&op.phase==Operation::Phase::selection&&!op.automatic){
        if(!op.overwrite){pending_result_=save_access_error;message_="This request does not permit overwriting; existing native data was preserved.";op.phase=Operation::Phase::information;ack_armed_=false;return 0;}
        op.phase=Operation::Phase::overwrite;ack_armed_=false;return 0;
    }
    try{
        if(!store_ready(root_)&&(op.mode==4||op.mode==7))throw StoreError("Native save store disappeared before operation");
        if(slot.kind==Operation::Kind::corrupt)throw BrokenStoreError("Corrupt/unrecognized native data was preserved; choose another slot");
        if(op.mode==5){
            auto record=op.record;record.slot=slot.name;commit_record(root_,record,slot.hash);
        }else if(op.mode==7){
            delete_record(root_,op.game,slot.name,op.file,slot.hash);
        }else{
            const auto store=root_/native_directory_name,ref=reference(store,op.game,slot.name,op.file);
            if(receipt(ref)!=slot.hash)throw StoreError("Native save slot changed after selection; no guest output was modified");
            const auto record=load_record(store,slot.hash,op.game,slot.name,op.file);
            if(record.payload.size()>op.capacity)throw StoreError("Native save payload exceeds the caller's output buffer");
            for(unsigned index=0;index<4;++index)if(op.image_capacities[index]&&record.images[index].size()>op.image_capacities[index])throw StoreError("Native save image exceeds the caller's output buffer");
            // All inputs, receipts and output extents have validated. Commit the
            // complete plaintext payload, metadata and lengths only on success.
            memory.copy_in(op.buffer,record.payload);memory.copy_in(parameters_+0x80,record.sfo);
            memory.store32(parameters_+0x7c,static_cast<std::uint32_t>(record.payload.size()));
            for(unsigned index=0;index<4;++index)if(op.image_capacities[index]){
                memory.copy_in(op.image_buffers[index],record.images[index]);memory.store32(parameters_+0x58c+index*16,static_cast<std::uint32_t>(record.images[index].size()));
            }
        }
        for(unsigned index=0;index<20;++index)memory.store8(parameters_+0x4c+index,index<slot.name.size()?static_cast<std::uint8_t>(slot.name[index]):0);
        return finish(0);
    }catch(const std::exception &error){
        diagnostic_=error.what();message_=std::string(error.what())+"\nNo PSP save was imported or modified.";
        op.automatic=false; // A failed automatic operation shows its real error.
        pending_result_=op.mode==5?save_access_error:op.mode==7?(dynamic_cast<const BrokenStoreError *>(&error)?delete_data_broken:delete_access_error):data_broken;op.phase=Operation::Phase::information;ack_armed_=false;return 0;
    }
}
std::uint32_t Savedata::shutdown_start(){diagnostic_.clear();if(status_!=Status::quit){diagnostic_="Savedata Shutdown requires a completed operation";return invalid_status;}status_=Status::finished;finished_reported_=false;return 0;}
void Savedata::invoke(std::uint32_t nid,psprecomp::Runtime &runtime,psprecomp::AllegrexContext &context){
    switch(nid){case 0x50c4cd57u:context.gpr[2]=init_start(runtime.memory(),context.gpr[4]);break;
    case 0x8874dbe0u:context.gpr[2]=get_status();break;case 0xd4b95ffbu:context.gpr[2]=update(runtime.memory(),context.gpr[4]);break;
    case 0x9790b33cu:context.gpr[2]=shutdown_start();break;default:throw psprecomp::Error("Unsupported savedata utility import");}
}
}
