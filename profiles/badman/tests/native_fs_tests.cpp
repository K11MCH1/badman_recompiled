#include "badman_native_fs.hpp"
#include "psprecomp/guest_memory.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std::string_literals;
namespace fs=std::filesystem;
void need(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}
void write(const fs::path &path,const std::string &data){std::ofstream f(path,std::ios::binary);need(bool(f.write(data.data(),data.size())),"fixture write");}
std::string read(const fs::path &path){std::ifstream f(path,std::ios::binary);need(bool(f),"fixture read");return {std::istreambuf_iterator<char>(f),{}};}
using Tree=std::map<std::wstring,std::string>;
Tree inventory(const fs::path &root){Tree result;for(const auto &entry:fs::recursive_directory_iterator(root)){
 if(entry.is_symlink())continue;if(entry.is_regular_file())result[entry.path().lexically_relative(root).native()]=read(entry.path());}return result;}
std::uint64_t identity(const fs::path &path){HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);need(h!=INVALID_HANDLE_VALUE,"identity open");BY_HANDLE_FILE_INFORMATION i{};const bool ok=GetFileInformationByHandle(h,&i);CloseHandle(h);need(ok,"identity info");return (std::uint64_t(i.nFileIndexHigh)<<32)|i.nFileIndexLow;}
unsigned checks=0,successes=0,failures=0,hook_checks=0,reparse_checks=0;
enum class Hook{none,pins,destination_race,reparse_destination};Hook hook=Hook::none;
fs::path hook_outside;
bool junction(const fs::path &link,const fs::path &target,bool create=true);
#if defined(BADMAN_NATIVE_FS_TEST_HOOK)
namespace badman::detail {
void native_fs_before_rename(const fs::path &source,const fs::path &target,HANDLE) {
 if(hook==Hook::pins) {
  for(const auto &parent:{source.parent_path(),target.parent_path()}){
   HANDLE h=CreateFileW(parent.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
   need(h==INVALID_HANDLE_VALUE,"pinned parent accepted delete handle");++hook_checks;
  }
  need(!MoveFileExW(source.parent_path().c_str(),(source.parent_path().wstring()+L"-moved").c_str(),0),"source parent renamed while pinned");++hook_checks;
  HANDLE h=CreateFileW(source.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
  need(h==INVALID_HANDLE_VALUE,"exclusive source accepted writer");++hook_checks;
  need(!MoveFileExW(source.c_str(),(source.wstring()+L"-moved").c_str(),0),"source renamed behind open handle");++hook_checks;
 }else if(hook==Hook::destination_race){write(target,"racing-destination");++hook_checks;}
 else if(hook==Hook::reparse_destination){need(junction(target.parent_path(),hook_outside,false),"concurrent junction setup failed");++hook_checks;}
}
}
#endif
bool junction(const fs::path &link,const fs::path &target,bool create) {
 if(create)fs::create_directory(link);
 HANDLE h=CreateFileW(link.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(h==INVALID_HANDLE_VALUE)return false;
 const auto substitute=L"\\??\\"+fs::absolute(target).native(),print=fs::absolute(target).native();
 struct Buffer{DWORD tag;WORD length,reserved,sub_offset,sub_length,print_offset,print_length;wchar_t text[2048];};
 Buffer b{};b.tag=IO_REPARSE_TAG_MOUNT_POINT;b.sub_length=WORD(substitute.size()*2);b.print_offset=WORD((substitute.size()+1)*2);b.print_length=WORD(print.size()*2);
 std::memcpy(b.text,substitute.c_str(),(substitute.size()+1)*2);std::memcpy(reinterpret_cast<char*>(b.text)+b.print_offset,print.c_str(),(print.size()+1)*2);
 b.length=WORD(8+b.print_offset+b.print_length+2);DWORD received{};const bool ok=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&b,DWORD(8+b.length),nullptr,0,&received,nullptr)!=0;CloseHandle(h);return ok;
}
void rejected(const fs::path &root,const std::string &old_name,const std::string &new_name) {
 const auto before=inventory(root);const auto result=badman::rename_native_file(root,old_name,new_name);
 need(result<0,"rejected operation returned success");need(inventory(root)==before,"failed rename modified fixture files");++checks;++failures;
}
void moved(const fs::path &root,const std::string &old_name,const std::string &new_name,const fs::path &source,const fs::path &target) {
 const auto bytes=read(source);const auto id=identity(source);need(!fs::exists(target),"target unexpectedly exists");
 const auto result=badman::rename_native_file(root,old_name,new_name);
 if(result!=0)std::cerr<<"rename result="<<std::hex<<static_cast<std::uint32_t>(result)<<std::dec<<"\n";
 need(result==0,"actual no-overwrite rename failed");need(!fs::exists(source)&&read(target)==bytes&&identity(target)==id,"successful rename did not preserve actual file identity/content");++checks;++successes;
}
int main(int argc,char **argv){try{
 need(argc<=2,"at most one owned fixture argument required");
 const auto base=argc==2?fs::absolute(argv[1]):fs::canonical(fs::temp_directory_path())/
  ("badman-native-fs-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 need(!fs::exists(base),"fixture exists");fs::create_directories(base);
 const auto root=base/L"메모리 카드";fs::create_directory(root);fs::create_directories(root/"source");fs::create_directories(root/"target");
 fs::create_directories(root/"BADMAN-NATIVE-V1");fs::create_directories(root/"PSP/SAVEDATA");
 write(root/"BADMAN-NATIVE-V1/OWNER.txt","native-store-preserved");write(root/"PSP/SAVEDATA/OLD.BIN","psp-save-preserved");
 std::string bytes(1048576,'\0');for(std::size_t i=0;i<bytes.size();++i)bytes[i]=char((i*37)^(i>>7));
 write(root/"plain.bin",bytes);moved(root,"ms0:/plain.bin","ms0:/renamed.bin",root/"plain.bin",root/"renamed.bin");
 write(root/"source/move.bin",bytes);moved(root,"MS0:/source/move.bin","ms0:/target/moved.bin",root/"source/move.bin",root/"target/moved.bin");
 write(root/"source/fixed.bin","fixed-source");write(root/"target/existing.bin","existing-destination");
 rejected(root,"ms0:/source/fixed.bin","ms0:/target/existing.bin");rejected(root,"ms0:/source/fixed.bin","ms0:/source/fixed.bin");
 rejected(root,"ms0:/source/fixed.bin","ms0:/SOURCE/FIXED.BIN");rejected(root,"ms0:/missing.bin","ms0:/target/missing.bin");
 rejected(root,"ms0:/source","ms0:/target/source-directory");rejected(root,"ms0:/source/fixed.bin","ms0:/target");
 std::vector<std::string> bad={"","relative.bin","/absolute.bin","um0:/a","ms0:","ms0:/","ms0:a","ms0://a","ms0:/a/","ms0:/a//b","ms0:/../escape","ms0:/./file","ms0:/a/../../file","ms0:/a\\b","ms0:/file:stream","ms0:/a:b:c","ms0:/file.","ms0:/file ","ms0:/a b","ms0:/a?b","ms0:/a*b","ms0:/a<b","ms0:/a|b","ms0:/a\nb","ms0:/CON","ms0:/NUL.bin","ms0:/COM1.txt","ms0:/LPT9","ms0:/aux.z","ms0:/PRN","ms0:/한글.bin","ms0:/"+std::string(241,'x'),"ms0:/"+std::string(1024,'x'),"ms0:/BADMAN-NATIVE-V1/OWNER.txt","ms0:/badman-native-v1/new","ms0:/nested/BADMAN-NATIVE-V1/a","ms0:/PSP/SAVEDATA/OLD.BIN","ms0:/psp/savedata/new","ms0:/.badman-state/a","ms0:/.BADMAN-NATIVE-STORE-123/a","disc0:/PSP_GAME/PARAM.SFO","flash0:/font/a.pgf",std::string("ms0:/a\0b",8)};
 for(const auto &name:bad){rejected(root,name,"ms0:/target/never.bin");rejected(root,"ms0:/source/fixed.bin",name);}
 need(static_cast<std::uint32_t>(badman::rename_native_file(root,"disc0:/a","ms0:/a"))==0x8001001Eu,"read-only disc mapping");
 need(static_cast<std::uint32_t>(badman::rename_native_file(root,"ms0:/a","flash0:/a"))==0x8001001Eu,"read-only font mapping");checks+=2;
 const auto before=inventory(root);need(badman::rename_native_file({},"ms0:/source/fixed.bin","ms0:/target/never.bin")<0,"unmounted allowed");
 need(badman::rename_native_file(base/"missing-root","ms0:/source/fixed.bin","ms0:/target/never.bin")<0,"missing mount allowed");
 need(badman::rename_native_file(fs::path("relative-root"),"ms0:/source/fixed.bin","ms0:/target/never.bin")<0,"relative mount allowed");
 need(inventory(root)==before,"bad mount changed fixture");checks+=3;
 // Open handle conflicts preserve both source and target; no optimistic success.
 HANDLE busy=CreateFileW((root/"source/fixed.bin").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);need(busy!=INVALID_HANDLE_VALUE,"busy fixture handle");
 rejected(root,"ms0:/source/fixed.bin","ms0:/target/busy.bin");CloseHandle(busy);
 // Test all invalid guest buffers before any filesystem action and all guest memory unchanged.
 psprecomp::GuestMemory memory;std::fill(memory.raw_pointer(0x08000000,memory.size()),memory.raw_pointer(0x08000000,memory.size())+memory.size(),0xA5);
 auto put=[&](std::uint32_t at,const std::string &text){std::vector<std::uint8_t> data(text.begin(),text.end());data.push_back(0);memory.copy_in(at,data);};
 put(0x08810000,"ms0:/source/fixed.bin");put(0x08811000,"ms0:/target/guest.bin");
 for(const auto [old_at,new_at]:std::vector<std::pair<std::uint32_t,std::uint32_t>>{{0,0x08811000},{0x08810000,0},{0xFFFFFFFF,0x08811000},{0x08810000,0x09FFFFFE},{0x09FFF000,0x08811000},{0x08810000,0x09FFF000}}){
  const auto ram=memory.bytes(),vram=memory.vram_bytes();const auto tree=inventory(root);need(badman::rename_native_file_guest(memory,root,old_at,new_at)<0,"invalid guest buffer succeeded");
  need(memory.bytes()==ram&&memory.vram_bytes()==vram&&inventory(root)==tree,"invalid guest buffer mutated RAM/VRAM/files");++checks;++failures;
 }
 write(root/"source/guest.bin","guest-pointer-source");put(0x08810000,"ms0:/source/guest.bin");
 const auto ram=memory.bytes(),vram=memory.vram_bytes();need(badman::rename_native_file_guest(memory,root,0x88810000,0x08811000)==0,"cached guest pointer rename failed");
 need(memory.bytes()==ram&&memory.vram_bytes()==vram&&!fs::exists(root/"source/guest.bin")&&read(root/"target/guest.bin")=="guest-pointer-source","guest rename content/memory mismatch");++checks;++successes;
 // A real file hard link must be rejected instead of touching linked data.
 write(base/"outside.bin","outside-hardlink");need(CreateHardLinkW((root/"source/hard.bin").c_str(),(base/"outside.bin").c_str(),nullptr)!=0,"hard link fixture unavailable");
 rejected(root,"ms0:/source/hard.bin","ms0:/target/hard.bin");need(read(base/"outside.bin")=="outside-hardlink","outside hard link changed");++checks;
 // Directory junctions work without requiring symbolic-link privilege.
 fs::create_directory(base/"outside-dir");write(base/"outside-dir/secret.bin","outside-directory-file");
 need(junction(root/"source-link",base/"outside-dir"),"source junction fixture failed");
 need(junction(root/"target-link",base/"outside-dir"),"destination junction fixture failed");
 need(badman::rename_native_file(root,"ms0:/source-link/secret.bin","ms0:/target/escaped.bin")<0,"source junction followed");
 need(badman::rename_native_file(root,"ms0:/source/fixed.bin","ms0:/target-link/escaped.bin")<0,"destination junction followed");
 need(junction(base/"host-link",root),"root junction fixture failed");
 need(badman::rename_native_file(base/"host-link","ms0:/source/fixed.bin","ms0:/target/escaped.bin")<0,"host mount junction followed");
 need(read(base/"outside-dir/secret.bin")=="outside-directory-file"&&!fs::exists(base/"outside-dir/escaped.bin")&&read(root/"source/fixed.bin")=="fixed-source","junction failure mutated files");checks+=3;reparse_checks+=3;
 // A final file reparse point is also opened as itself, never its target.
 const bool file_link=CreateSymbolicLinkW((root/"source/symbolic.bin").c_str(),(base/"outside.bin").c_str(),2)!=0;
 if(file_link){need(badman::rename_native_file(root,"ms0:/source/symbolic.bin","ms0:/target/symbolic.bin")<0,"source file symlink followed");++checks;++reparse_checks;}
 else std::cout<<"FILE_SYMLINK_PRIVILEGE_UNAVAILABLE error="<<GetLastError()<<" (directory-junction tests mandatory)\n";
#if defined(BADMAN_NATIVE_FS_TEST_HOOK)
 // Hooks exist only in this private target; shipping compilation has none.
 write(root/"source/pinned.bin","pinned-source");hook=Hook::pins;
 moved(root,"ms0:/source/pinned.bin","ms0:/target/pinned.bin",root/"source/pinned.bin",root/"target/pinned.bin");hook=Hook::none;
 write(root/"source/race.bin","race-source");hook=Hook::destination_race;
 need(badman::rename_native_file(root,"ms0:/source/race.bin","ms0:/target/race.bin")<0,"racing destination overwritten");hook=Hook::none;
 need(read(root/"source/race.bin")=="race-source"&&read(root/"target/race.bin")=="racing-destination","atomic no-overwrite race failed");++checks;++failures;
 // Windows permits changing an empty directory into a junction while its
 // no-delete handle is retained. The relative native rename must reject that
 // operation, not follow the changed directory into the outside fixture.
 fs::create_directory(root/"empty-racing-target");hook_outside=base/"outside-dir";
 write(root/"source/junction-race.bin","junction-race-source");hook=Hook::reparse_destination;
 need(badman::rename_native_file(root,"ms0:/source/junction-race.bin","ms0:/empty-racing-target/escaped.bin")<0,"racing junction accepted rename");hook=Hook::none;
 need(read(root/"source/junction-race.bin")=="junction-race-source"&&!fs::exists(hook_outside/"escaped.bin")&&read(hook_outside/"secret.bin")=="outside-directory-file","racing junction escaped or damaged files");++checks;++failures;++reparse_checks;
#endif
 // Handle lifetime is checked using real repeated successes and failures.
 DWORD handles_before{},handles_after{};need(GetProcessHandleCount(GetCurrentProcess(),&handles_before)!=0,"handle count before");
 write(root/"source/lifetime.bin","lifetime-source");
 for(unsigned i=0;i<200;++i){
  need(badman::rename_native_file(root,"ms0:/source/lifetime.bin","ms0:/target/lifetime.bin")==0,"lifetime forward rename");
  need(badman::rename_native_file(root,"ms0:/target/lifetime.bin","ms0:/source/lifetime.bin")==0,"lifetime backward rename");
  need(badman::rename_native_file(root,"ms0:/source/lifetime.bin","ms0:/source/fixed.bin")<0,"lifetime overwrite accepted");
 }
 need(GetProcessHandleCount(GetCurrentProcess(),&handles_after)!=0&&handles_after==handles_before,"native filesystem handles leaked");
 need(read(root/"source/lifetime.bin")=="lifetime-source"&&read(root/"source/fixed.bin")=="fixed-source","lifetime data mismatch");checks+=600;successes+=400;failures+=200;
 std::cout<<"PASS_NATIVE_FS_RENAME checks="<<checks<<" successful_real_moves="<<successes<<" preservation_failures="<<failures<<" reparse_guards="<<reparse_checks<<" pinned_handle_checks="<<hook_checks<<"\n";
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
