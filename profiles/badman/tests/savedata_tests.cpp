#include "badman_savedata.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/sha256.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <map>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using badman::Savedata;
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F> void unsupported(F operation,const char *message) {
    bool failed=false;try{operation();}catch(const psprecomp::Error &){failed=true;}require(failed,message);
}
struct Fixture {
    std::filesystem::path parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    std::filesystem::path root=parent/("badman-savedata-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(){require(std::filesystem::create_directory(root),"Temporary savedata root exists");}
    ~Fixture(){
        std::error_code error;const auto resolved=std::filesystem::weakly_canonical(root,error);
        if(!error&&resolved.parent_path()==parent&&resolved.filename().string().starts_with("badman-savedata-test-"))std::filesystem::remove_all(resolved,error);
    }
};
struct Harness {
    psprecomp::Runtime runtime;
    Savedata utility;
    psprecomp::GuestMemory &memory=runtime.memory();
    static constexpr std::uint32_t parameters=0x08800000,list=0x08804001,buffer=0x08802000,sentinel=0x11223344;
    Harness(){request();}
    void string(std::uint32_t address,const std::string &text,std::uint32_t capacity){
        for(std::uint32_t i=0;i<capacity;++i)memory.store8(address+i,i<text.size()?static_cast<std::uint8_t>(text[i]):0);
    }
    void request(){
        for(std::uint32_t i=0;i<0x600;++i)memory.store8(parameters+i,0);
        memory.store32(parameters,0x600);memory.store32(parameters+4,1);memory.store32(parameters+8,1);
        memory.store32(parameters+0x1c,sentinel);memory.store32(parameters+0x30,4);
        string(parameters+0x3c,"ULUS10456",13);string(parameters+0x64,"YUUSHA.BIN",13);
        memory.store32(parameters+0x60,list);memory.store32(parameters+0x74,buffer);
        memory.store32(parameters+0x78,0xd38);memory.store32(parameters+0x7c,0xd38);
        for(std::uint32_t i=0;i<0xd38;++i)memory.store8(buffer+i,0x5a);
        for(unsigned i=0;i<99;++i){std::string name="YSave"+(i<10?std::string("0"):std::string())+std::to_string(i);string(list+i*20,name,20);}
        string(list+99*20,"",20);
        for(std::uint32_t i=0;i<16;++i)memory.store8(parameters+0x5dc+i,0xa5); // Synthetic nonzero key, not game data.
        // These are other-mode pointers, deliberately inactive for LISTLOAD.
        memory.store32(parameters+0x5d0,0xdeadbeef);memory.store32(parameters+0x5f4,0xdeadbeef);
    }
    std::vector<std::uint8_t> snapshot(std::uint32_t address,std::uint32_t size){std::vector<std::uint8_t> out;for(std::uint32_t i=0;i<size;++i)out.push_back(memory.load8(address+i));return out;}
    void visible(){require(utility.init_start(memory,parameters)==0,"Valid LISTLOAD was not accepted");require(utility.get_status()==1,"INIT was not observed");require(utility.get_status()==2,"VISIBLE was not observed");}
    void finish(std::uint32_t expected){
        require(utility.acknowledge(),"Explicit visible acknowledgment was rejected");
        require(utility.update(memory,2)==0,"Acknowledged Update failed");
        if(memory.load32(parameters+0x1c)!=expected)
            throw std::runtime_error("Wrong savedata operation result "+std::to_string(memory.load32(parameters+0x1c))+
                " (expected "+std::to_string(expected)+"): "+utility.last_diagnostic());
        require(utility.get_status()==3,"QUIT was not observed");
        require(utility.shutdown_start()==0,"Shutdown did not start");
        require(utility.get_status()==4&&utility.get_status()==0,"FINISHED/NONE shutdown sequence wrong");
    }
};
void mounted_empty_lifecycle_and_preservation(){
    Fixture fixture;Harness h;h.utility.mount_memory_stick(fixture.root);
    const auto parameters=h.snapshot(h.parameters,0x600),data=h.snapshot(h.buffer,0xd38),names=h.snapshot(h.list,2000);
    require(h.utility.get_status()==0&&!h.utility.acknowledge(),"Idle utility was already active");
    h.visible();require(h.utility.dialog()&&h.utility.dialog()->result==Savedata::no_data&&h.utility.dialog()->can_ack,"Empty mounted card was not exposed as no-data");
    for(unsigned i=0;i<20;++i){require(h.utility.get_status()==2&&h.utility.update(h.memory,2)==0,"Unacknowledged dialog advanced");}
    require(h.memory.load32(h.parameters+0x1c)==h.sentinel,"Pending operation wrote completion prematurely");
    require(h.utility.shutdown_start()==Savedata::invalid_status,"Visible operation allowed early shutdown");
    unsupported([&]{h.utility.mount_memory_stick({});},"Active operation allowed media remount");
    h.finish(Savedata::no_data);
    auto after=h.snapshot(h.parameters,0x600);for(unsigned i=0x1c;i<0x20;++i)after[i]=parameters[i];
    require(after==parameters&&h.snapshot(h.buffer,0xd38)==data&&h.snapshot(h.list,2000)==names,"No-data operation changed payload/name/header outputs beyond result");
    require(!std::filesystem::exists(fixture.root/"PSP"),"LISTLOAD created filesystem data");
}
void absent_card_and_real_confirm_edges(){
    Harness h;h.visible();require(h.utility.dialog()->result==Savedata::no_media,"Unmounted card was confused with empty data");
    h.utility.observe_buttons(0x4000);h.utility.update(h.memory,2);
    h.utility.observe_buttons(0x4000);h.utility.update(h.memory,2);
    require(h.utility.status()==Savedata::Status::visible,"Opening held Cross silently acknowledged firmware dialog");
    h.utility.observe_buttons(0);h.utility.observe_buttons(0x2000);h.utility.update(h.memory,1);
    require(h.utility.status()==Savedata::Status::visible,"Wrong confirm button acknowledged Cross-configured dialog");
    h.utility.observe_buttons(0x4000);require(!h.utility.dialog()->can_ack,"Fresh confirm edge did not request acknowledgment");
    h.utility.update(h.memory,2);require(h.memory.load32(h.parameters+0x1c)==Savedata::no_media,"No-media result incorrect");
    require(h.utility.get_status()==3&&h.utility.shutdown_start()==0&&h.utility.get_status()==4&&h.utility.get_status()==0,"No-media lifecycle did not close");
    h.request();h.memory.store32(h.parameters+8,0);h.visible();
    h.utility.observe_buttons(0);h.utility.observe_buttons(0x4000);h.utility.update(h.memory,2);
    require(h.utility.status()==Savedata::Status::visible,"Button swap ignored Circle preference");
    h.utility.observe_buttons(0x2000);h.utility.update(h.memory,2);
    require(h.utility.get_status()==3,"Circle preference did not accept genuine Circle edge");
}
void request_validation_and_failure_atomicity(){
    Harness h;
    require(h.utility.init_start(h.memory,h.parameters+1)==Savedata::invalid_address,"Unaligned parameter pointer accepted");
    h.memory.store32(h.parameters,0x5fc);require(h.utility.init_start(h.memory,h.parameters)==Savedata::invalid_size,"Unobserved ABI size accepted");
    h.request();h.string(h.parameters+0x64,"../escape",13);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Path traversal accepted");
    h.request();h.string(h.parameters+0x64,"CON",13);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Windows device filename accepted");
    h.request();h.memory.store32(h.parameters+0x78,0xd37);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Data size exceeding capacity accepted");
    h.request();h.memory.store32(h.parameters+0x74,h.parameters+0x1c);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Data/parameter alias accepted");
    h.request();h.memory.store32(h.parameters+0x60,0x09fffff0);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Out-of-range slot list accepted");
    h.request();h.string(h.list+20,"YSave00",20);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Duplicate slot names accepted");
    h.request();h.string(h.list+99*20,"YSave99",20);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Unterminated slot list accepted");
    h.request();h.memory.store32(h.parameters+0x584,0x09fffff0);h.memory.store32(h.parameters+0x588,32);require(h.utility.init_start(h.memory,h.parameters)==Savedata::bad_parameters,"Invalid optional image buffer accepted");
    require(h.utility.status()==Savedata::Status::none&&h.memory.load32(h.parameters+0x1c)==h.sentinel,"Invalid requests changed lifecycle/result");
    h.request();h.visible();const auto before=h.snapshot(h.parameters,0x600);
    require(h.utility.init_start(h.memory,h.parameters)==Savedata::invalid_status,"Concurrent Init was accepted");
    unsupported([&]{h.utility.update(h.memory,3);},"Unsupported Update argument silently succeeded");
    require(h.snapshot(h.parameters,0x600)==before,"Failed lifecycle operation modified guest outputs");
}
void matching_data_is_explicitly_unsupported_and_preserved(){
    Fixture fixture;Harness h;const auto slot=fixture.root/"PSP/SAVEDATA/ULUS10456YSave00";
    std::filesystem::create_directories(slot);
    {std::ofstream out(slot/"YUUSHA.BIN",std::ios::binary);out<<"synthetic save bytes";}
    h.utility.mount_memory_stick(fixture.root);const auto before=h.snapshot(h.parameters,0x600);
    unsupported([&]{h.utility.init_start(h.memory,h.parameters);},"Existing secure savedata was falsely classified as empty");
    require(h.utility.status()==Savedata::Status::none&&h.snapshot(h.parameters,0x600)==before,"Unsupported existing-save request was accepted/modified");
    std::ifstream input(slot/"YUUSHA.BIN",std::ios::binary);std::string contents((std::istreambuf_iterator<char>(input)),{});
    require(contents=="synthetic save bytes","Existing save content changed");
}
void malformed_media_and_import_dispatch(){
    Fixture fixture;Harness h;{std::ofstream file(fixture.root/"PSP");file<<"unrelated";}
    h.utility.mount_memory_stick(fixture.root);h.visible();require(h.utility.dialog()->result==Savedata::access_error,"Invalid storage tree was fabricated as no-data");
    h.finish(Savedata::access_error);
    h.request();h.memory.store32(h.parameters+0x30,6);
    unsupported([&]{h.utility.init_start(h.memory,h.parameters);},"Unsupported LISTDELETE was accepted without implementation");
    Harness dispatch;psprecomp::AllegrexContext context;context.gpr[4]=dispatch.parameters;
    dispatch.utility.invoke(0x50c4cd57,dispatch.runtime,context);require(context.gpr[2]==0,"Init import dispatch failed");
    dispatch.utility.invoke(0x8874dbe0,dispatch.runtime,context);require(context.gpr[2]==1,"Status import dispatch failed");
    require(dispatch.utility.acknowledge(),"Explicit state-access action failed");context.gpr[4]=2;
    dispatch.utility.invoke(0xd4b95ffb,dispatch.runtime,context);require(context.gpr[2]==0,"Update import dispatch failed");
    dispatch.utility.invoke(0x9790b33c,dispatch.runtime,context);require(context.gpr[2]==0,"Shutdown import dispatch failed");
    unsupported([&]{dispatch.utility.invoke(0,dispatch.runtime,context);},"Unknown utility import silently succeeded");
}
using FileTree=std::map<std::filesystem::path,std::vector<std::uint8_t>>;
FileTree files(const std::filesystem::path &root){
    FileTree result;for(const auto &entry:std::filesystem::recursive_directory_iterator(root))if(entry.is_regular_file()){
        std::ifstream input(entry.path(),std::ios::binary);result[entry.path().lexically_relative(root)]={std::istreambuf_iterator<char>(input),{}};
    }return result;
}
void close_operation(Harness &h){require(h.utility.get_status()==3&&h.utility.shutdown_start()==0&&h.utility.get_status()==4&&h.utility.get_status()==0,"Native savedata lifecycle did not close");}
void save(Harness &h,const std::string &slot){
    h.memory.store32(h.parameters+0x30,5);h.memory.store32(h.parameters+0x38,1);h.visible();
    require(h.utility.select_slot(slot),"Requested native save slot was unavailable");
    require(h.utility.acknowledge()&&h.utility.update(h.memory,2)==0,"Native save action failed");
    if(h.utility.status()==Savedata::Status::visible&&h.utility.dialog()->message.find("Overwrite")!=std::string::npos)
        require(h.utility.acknowledge()&&h.utility.update(h.memory,2)==0,"Confirmed overwrite did not run");
    if(h.utility.status()!=Savedata::Status::quit)throw std::runtime_error("Native save failed: "+h.utility.last_diagnostic());
    require(h.memory.load32(h.parameters+0x1c)==0,"Native save returned an error");close_operation(h);
}
void native_save_load_metadata_and_immutable_snapshot(){
    Fixture fixture;Harness writer;writer.utility.mount_memory_stick(fixture.root);
    writer.memory.store32(writer.parameters+0x30,5);writer.memory.store32(writer.parameters+0x38,1);
    writer.string(writer.parameters+0x80,"Synthetic native title",128);
    writer.memory.store32(writer.parameters+0x584,0x08806000);writer.memory.store32(writer.parameters+0x588,64);writer.memory.store32(writer.parameters+0x58c,17);
    for(unsigned i=0;i<17;++i)writer.memory.store8(0x08806000+i,static_cast<std::uint8_t>(i+1));
    writer.visible();require(writer.utility.dialog()->message.find("not PSP compatible")!=std::string::npos,"Private format was not disclosed in utility UI");
    require(!std::filesystem::exists(fixture.root/Savedata::native_directory_name),"Save Init wrote filesystem state before selection");
    require(writer.utility.select_slot("YSave02"),"Explicit slot selection failed");
    writer.memory.store8(writer.buffer,0xee);writer.memory.store8(0x08806000,0xff);
    writer.finish(0);
    require(writer.memory.load8(writer.buffer)==0xee&&writer.memory.load8(writer.parameters+0x4c)=='Y',"Save changed source payload or lost selected save-name output");
    Harness reader;reader.utility.mount_memory_stick(fixture.root);reader.memory.store32(reader.parameters+0x584,0x08807000);reader.memory.store32(reader.parameters+0x588,64);
    for(unsigned i=0;i<64;++i)reader.memory.store8(0x08807000+i,0xa7);
    for(unsigned i=0;i<0xd38;++i)reader.memory.store8(reader.buffer+i,0);
    reader.visible();require(reader.utility.select_slot("YSave02"),"Persisted slot did not appear on native load");reader.finish(0);
    require(reader.memory.load8(reader.buffer)==0x5a&&reader.memory.load32(reader.parameters+0x7c)==0xd38&&reader.memory.load8(reader.parameters+0x80)=='S',"Load did not restore immutable payload/SFO/dataSize");
    require(reader.memory.load32(reader.parameters+0x58c)==17&&reader.memory.load8(0x08807000)==1&&reader.memory.load8(0x08807000+16)==17&&reader.memory.load8(0x08807000+17)==0xa7,
        "Image output size/data/tail preservation failed");
    require(!std::filesystem::exists(fixture.root/"PSP"),"Native persistence wrote a PSP-format savedata tree");
}
void overwrite_requires_fresh_confirmation_and_cancel_preserves(){
    Fixture fixture;Harness initial;initial.utility.mount_memory_stick(fixture.root);save(initial,"YSave00");const auto original=files(fixture.root);
    Harness writer;writer.utility.mount_memory_stick(fixture.root);writer.memory.store32(writer.parameters+0x30,5);writer.memory.store32(writer.parameters+0x38,1);writer.visible();
    writer.utility.observe_buttons(0);writer.utility.observe_buttons(0x4000);writer.utility.update(writer.memory,2);
    require(writer.utility.status()==Savedata::Status::visible&&writer.utility.dialog()->message.find("Overwrite")!=std::string::npos&&files(fixture.root)==original,"First confirm overwrote existing native data");
    writer.utility.observe_buttons(0x4000);writer.utility.update(writer.memory,2);
    require(files(fixture.root)==original&&writer.memory.load32(writer.parameters+0x1c)==writer.sentinel,"Held confirm bypassed overwrite confirmation");
    require(writer.utility.cancel()&&writer.utility.update(writer.memory,2)==0&&writer.memory.load32(writer.parameters+0x1c)==Savedata::user_cancelled,"Overwrite cancel failed");close_operation(writer);
    require(files(fixture.root)==original,"Cancelled overwrite changed existing files");
    Harness replacement;replacement.utility.mount_memory_stick(fixture.root);for(unsigned i=0;i<0xd38;++i)replacement.memory.store8(replacement.buffer+i,0x3c);save(replacement,"YSave00");
    const auto updated=files(fixture.root);require(updated.size()>original.size(),"Overwrite discarded prior immutable save instead of retaining recovery data");
    for(const auto &[name,data]:original)if(name.extension()==".bsav")require(updated.at(name)==data,"Overwrite corrupted the prior immutable blob");
    Harness reader;reader.utility.mount_memory_stick(fixture.root);reader.visible();reader.finish(0);require(reader.memory.load8(reader.buffer)==0x3c,"Overwritten receipt did not load new payload");
}
void corrupt_incomplete_and_foreign_data_are_preserved(){
    {
        Fixture fixture;const auto store=fixture.root/Savedata::native_directory_name;std::filesystem::create_directory(store);{std::ofstream out(store/"OWNER.txt");out<<"unrelated directory owner";}
        Harness h;h.utility.mount_memory_stick(fixture.root);h.memory.store32(h.parameters+0x30,5);const auto before=files(fixture.root);h.visible();h.finish(Savedata::save_access_error);
        require(files(fixture.root)==before,"Foreign native directory was adopted or overwritten");
    }
    {
        Fixture fixture;Harness initial;initial.utility.mount_memory_stick(fixture.root);save(initial,"YSave00");const auto store=fixture.root/Savedata::native_directory_name;
        {std::ofstream out(store/"ULUS10456-YSave01-YUUSHA.BIN.ref");out<<"incomplete reference";}
        {std::ofstream out(store/".pending-user-retained.tmp");out<<"incomplete prior transaction; preserve";}
        const auto before=files(fixture.root);Harness reader;reader.utility.mount_memory_stick(fixture.root);reader.visible();require(reader.utility.select_slot("YSave01"),"Corrupt native entry was silently hidden as empty");
        const auto outputs=reader.snapshot(reader.parameters,0x600),payload=reader.snapshot(reader.buffer,0xd38);
        reader.utility.acknowledge();reader.utility.update(reader.memory,2);
        require(reader.utility.status()==Savedata::Status::visible&&reader.utility.operation_result()==Savedata::data_broken&&reader.snapshot(reader.parameters,0x600)==outputs&&reader.snapshot(reader.buffer,0xd38)==payload,
            "Corrupt native load fabricated success or partially wrote outputs");reader.finish(Savedata::data_broken);
        require(files(fixture.root)==before,"Corrupt/incomplete files were modified or cleaned up");
    }
}
void stale_selection_and_busy_commit_preserve_latest_save(){
    Fixture fixture;Harness initial;initial.utility.mount_memory_stick(fixture.root);save(initial,"YSave00");
    Harness stale;stale.utility.mount_memory_stick(fixture.root);stale.memory.store32(stale.parameters+0x30,5);stale.memory.store32(stale.parameters+0x38,1);stale.visible();stale.utility.acknowledge();stale.utility.update(stale.memory,2);
    Harness newer;newer.utility.mount_memory_stick(fixture.root);newer.memory.store8(newer.buffer,0x66);save(newer,"YSave00");const auto current=files(fixture.root);
    stale.utility.acknowledge();stale.utility.update(stale.memory,2);
    require(stale.utility.status()==Savedata::Status::visible&&stale.utility.operation_result()==Savedata::save_access_error&&files(fixture.root)==current,"Stale overwrite replaced a newer save");stale.finish(Savedata::save_access_error);
    Harness busy;busy.utility.mount_memory_stick(fixture.root);busy.memory.store32(busy.parameters+0x30,5);busy.memory.store32(busy.parameters+0x38,1);busy.visible();busy.utility.select_slot("YSave01");
    const auto lock=fixture.root/Savedata::native_directory_name/".commit-lock";{std::ofstream out(lock);out<<"unrelated lock occupant";}const auto before=files(fixture.root);
    busy.utility.acknowledge();busy.utility.update(busy.memory,2);require(busy.utility.operation_result()==Savedata::save_access_error&&files(fixture.root)==before,"Occupied lock caused false success or data replacement");busy.finish(Savedata::save_access_error);
}
void corrupted_blob_and_foreign_identity_preserve_outputs(){
    for(unsigned scenario=0;scenario<3;++scenario){
        Fixture fixture;Harness initial;initial.utility.mount_memory_stick(fixture.root);save(initial,"YSave00");
        const auto store=fixture.root/Savedata::native_directory_name;
        std::filesystem::path blob;for(const auto &entry:std::filesystem::directory_iterator(store))if(entry.path().extension()==".bsav")blob=entry.path();
        require(!blob.empty(),"Synthetic native blob missing");
        auto content=files(fixture.root).at(blob.lexically_relative(fixture.root));
        if(scenario==0)content.back()^=0xff; // Receipt remains unchanged: reject SHA mismatch.
        else if(scenario==1)content[48]='X'; // Rehashed but belongs to a different game.
        else content.push_back(0); // Rehashed schema with trailing bytes must still fail.
        const auto staging=store/"test-malformed.blob";
        {std::ofstream output(staging,std::ios::binary);output.write(reinterpret_cast<const char *>(content.data()),static_cast<std::streamsize>(content.size()));}
        if(scenario==0)std::filesystem::copy_file(staging,blob,std::filesystem::copy_options::overwrite_existing);
        else{
            const auto hash=psprecomp::sha256_file(staging);std::filesystem::rename(staging,store/("sha256-"+hash+".bsav"));
            std::ofstream output(store/"ULUS10456-YSave00-YUUSHA.BIN.ref",std::ios::binary);output<<"PSPRECOMP_BADMAN_NATIVE_REFERENCE_V1\nsha256="<<hash<<'\n';
        }
        const auto before=files(fixture.root);Harness reader;reader.utility.mount_memory_stick(fixture.root);reader.visible();
        const auto parameters=reader.snapshot(reader.parameters,0x600),payload=reader.snapshot(reader.buffer,0xd38);
        reader.utility.acknowledge();reader.utility.update(reader.memory,2);
        require(reader.utility.operation_result()==Savedata::data_broken&&reader.snapshot(reader.parameters,0x600)==parameters&&reader.snapshot(reader.buffer,0xd38)==payload,
            "Corrupt/foreign blob accepted or partially modified guest outputs");reader.finish(Savedata::data_broken);
        require(files(fixture.root)==before,"Corrupt/foreign native blobs were changed or removed");
    }
}
void roundtrip_child(const std::filesystem::path &root,bool write){
    Harness h;h.utility.mount_memory_stick(root);
    if(write){for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,static_cast<std::uint8_t>((i*17+29)&255));h.string(h.parameters+0x80,"Independent process roundtrip",128);save(h,"YSave07");}
    else{for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,0);h.visible();require(h.utility.select_slot("YSave07"),"New process did not discover saved slot");h.finish(0);
        for(unsigned i=0;i<0xd38;++i)require(h.memory.load8(h.buffer+i)==static_cast<std::uint8_t>((i*17+29)&255),"New process restored different payload bytes");
        require(h.memory.load8(h.parameters+0x80)=='I',"New process did not restore SFO metadata");}
}
void delete_request(Harness &h){
    h.memory.store32(h.parameters+0x30,7);
    // LISTALLDELETE does not consume these inactive fields; accept even unusable pointers/extents.
    h.memory.store32(h.parameters+0x74,0xdeadbeef);h.memory.store32(h.parameters+0x78,0xffffffff);h.memory.store32(h.parameters+0x7c,0xffffffff);
    for(unsigned n=0;n<4;++n){h.memory.store32(h.parameters+0x584+n*16,0xdeadbeef);h.memory.store32(h.parameters+0x588+n*16,0xffffffff);h.memory.store32(h.parameters+0x58c+n*16,0xffffffff);}
}
void erase(Harness &h,const std::string &slot){
    delete_request(h);h.visible();require(h.utility.select_slot(slot),"Native delete slot not listed");
    require(h.utility.acknowledge()&&h.utility.update(h.memory,2)==0,"Delete selection failed");
    require(h.utility.status()==Savedata::Status::visible&&h.utility.dialog()->message.find("Delete "+slot)!=std::string::npos,"Delete skipped explicit confirmation");
    h.finish(0);
}
void delete_confirmation_cancel_and_preservation(){
    Fixture fixture;Harness writer;writer.utility.mount_memory_stick(fixture.root);save(writer,"YSave00");
    writer.request();save(writer,"YSave01");
    Harness other;other.utility.mount_memory_stick(fixture.root);other.string(other.parameters+0x3c,"OTHERGAME",13);save(other,"YSave00");
    const auto psp=fixture.root/"PSP/SAVEDATA/ULUS10456YSave00";std::filesystem::create_directories(psp);{std::ofstream file(psp/"YUUSHA.BIN");file<<"unrelated PSP secure data";}
    const auto original=files(fixture.root);Harness h;h.utility.mount_memory_stick(fixture.root);delete_request(h);const auto payload=h.snapshot(h.buffer,0xd38),parameters=h.snapshot(h.parameters,0x600);
    h.visible();require(h.utility.dialog()->title=="Delete private native save","Native delete scope undisclosed");
    h.utility.observe_buttons(0);h.utility.observe_buttons(0x4000);h.utility.update(h.memory,2);
    require(files(fixture.root)==original&&h.utility.dialog()->message.find("Recovery")!=std::string::npos,"First confirmation deleted data");
    h.utility.observe_buttons(0x4000);h.utility.update(h.memory,2);require(files(fixture.root)==original,"Held confirm skipped delete confirmation");
    require(h.utility.cancel()&&h.utility.update(h.memory,2)==0,"Delete cancel failed");require(h.memory.load32(h.parameters+0x1c)==Savedata::user_cancelled,"Wrong delete cancellation result");close_operation(h);
    require(files(fixture.root)==original,"Delete cancellation changed files");
    h.request();erase(h,"YSave00");const auto updated=files(fixture.root);const auto target=std::filesystem::path(Savedata::native_directory_name)/"ULUS10456-YSave00-YUUSHA.BIN.ref";
    require(!updated.contains(target),"Successful deletion left active native reference");
    bool recovered=false;for(const auto &[name,bytes]:updated)if(name.filename().string().starts_with("deleted-")){require(bytes==original.at(target),"Delete recovery receipt changed");recovered=true;}
    require(recovered,"Delete lost recovery receipt");
    for(const auto &[name,bytes]:original)if(name!=target)require(updated.at(name)==bytes,"Delete changed another slot/game/PSP/blob file");
    require(h.snapshot(h.buffer,0xd38)==payload,"Delete modified unused payload memory");
    // SFO, size, image descriptors and synthetic key remain caller data.
    auto after=h.snapshot(h.parameters,0x600);auto expected=parameters;
    for(unsigned i=0x1c;i<0x20;++i)expected[i]=after[i];for(unsigned i=0x4c;i<0x60;++i)expected[i]=after[i];
    require(after==expected,"Delete modified inactive payload/SFO/image/key fields");
}
void delete_empty_corrupt_stale_and_lock_failures(){
    {Fixture fixture;Harness h;h.utility.mount_memory_stick(fixture.root);delete_request(h);h.visible();h.finish(Savedata::delete_no_data);require(files(fixture.root).empty(),"Empty Delete created store");}
    {Harness h;delete_request(h);h.visible();h.finish(Savedata::delete_no_media);}
    {Harness h;delete_request(h);h.string(h.parameters+0x64,"../escape",13);require(h.utility.init_start(h.memory,h.parameters)==Savedata::delete_bad_parameters,"Delete invalid identity accepted");}
    {
        Fixture fixture;Harness writer;writer.utility.mount_memory_stick(fixture.root);save(writer,"YSave00");
        const auto ref=fixture.root/Savedata::native_directory_name/"ULUS10456-YSave00-YUUSHA.BIN.ref";{std::ofstream file(ref);file<<"incomplete";}
        const auto before=files(fixture.root);Harness h;h.utility.mount_memory_stick(fixture.root);delete_request(h);h.visible();h.utility.acknowledge();h.utility.update(h.memory,2);
        require(h.utility.operation_result()==Savedata::delete_data_broken,"Corrupt delete lacked broken-data error");h.finish(Savedata::delete_data_broken);require(files(fixture.root)==before,"Corrupt Delete changed files");
    }
    {
        Fixture fixture;Harness writer;writer.utility.mount_memory_stick(fixture.root);save(writer,"YSave00");
        Harness h;h.utility.mount_memory_stick(fixture.root);delete_request(h);h.visible();h.utility.acknowledge();h.utility.update(h.memory,2);
        writer.request();writer.memory.store8(writer.buffer,0x33);save(writer,"YSave00");const auto current=files(fixture.root);
        h.utility.acknowledge();h.utility.update(h.memory,2);require(h.utility.operation_result()==Savedata::delete_access_error&&files(fixture.root)==current,"Stale Delete removed newer data");h.finish(Savedata::delete_access_error);
        Harness busy;busy.utility.mount_memory_stick(fixture.root);delete_request(busy);busy.visible();busy.utility.acknowledge();busy.utility.update(busy.memory,2);
        {std::ofstream file(fixture.root/Savedata::native_directory_name/".commit-lock");file<<"unrelated occupant";}const auto before=files(fixture.root);
        busy.utility.acknowledge();busy.utility.update(busy.memory,2);require(busy.utility.operation_result()==Savedata::delete_access_error&&files(fixture.root)==before,"Busy Delete changed data");busy.finish(Savedata::delete_access_error);
    }
}
void delete_roundtrip_child(const std::filesystem::path &root,bool remove){
    Harness h;h.utility.mount_memory_stick(root);
    if(remove)erase(h,"YSave07");
    else{h.visible();require(h.utility.dialog()->result==Savedata::no_data,"New process loaded deleted native slot");h.finish(Savedata::no_data);}
}
void cross_process_roundtrip(){
#if defined(_WIN32)
    Fixture fixture;std::vector<wchar_t> executable(32768);const auto size=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));require(size&&size<executable.size(),"Cannot locate roundtrip test executable");
    const std::wstring program(executable.data(),size);
    for(const auto mode:{L"--write-native",L"--read-native",L"--delete-native",L"--read-empty"}){
        std::wstring command=L"\""+program+L"\" "+mode+L" \""+fixture.root.wstring()+L"\"";
        STARTUPINFOW startup{sizeof(STARTUPINFOW)};PROCESS_INFORMATION process{};
        require(CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"Cannot create independent savedata roundtrip process");
        CloseHandle(process.hThread);const auto waited=WaitForSingleObject(process.hProcess,30000);if(waited!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,1);
        DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);require(waited==WAIT_OBJECT_0&&code==0,"Independent savedata process roundtrip failed");
    }
#else
    Fixture fixture;roundtrip_child(fixture.root,true);roundtrip_child(fixture.root,false);delete_roundtrip_child(fixture.root,true);delete_roundtrip_child(fixture.root,false);
#endif
}
}
void automatic_step(Harness &h,std::uint32_t expected){
    require(h.utility.init_start(h.memory,h.parameters)==0&&h.utility.get_status()==1&&h.utility.get_status()==2,"Automatic operation did not become visible");
    require(!h.utility.dialog(),"Automatic profile operation displayed a chooser or prompt");
    require(!h.utility.acknowledge()&&!h.utility.cancel(),"Automatic operation accepted unexpected user actions");
    require(h.utility.update(h.memory,2)==0,"Automatic Update failed");
    if(h.utility.status()!=Savedata::Status::quit)throw std::runtime_error("Automatic operation did not finish: "+h.utility.last_diagnostic());
    require(h.memory.load32(h.parameters+0x1c)==expected,"Automatic operation returned the wrong result");close_operation(h);
}
void automatic_profile_load_save_and_retained_history(){
    Fixture fixture;
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        require(!h.utility.has_native_records("ULUS10456","YUUSHA.BIN"),"Empty card reported native records");
        automatic_step(h,Savedata::no_data);
        require(!std::filesystem::exists(fixture.root/Savedata::native_directory_name),"Automatic no-data load created storage");}
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        h.memory.store32(h.parameters+0x30,5);h.memory.store32(h.parameters+0x38,1);automatic_step(h,0);
        require(h.memory.load8(h.parameters+0x4c)=='Y'&&h.memory.load8(h.parameters+0x51)=='0'&&h.memory.load8(h.parameters+0x52)=='0',"First automatic save did not use the profile slot");
        require(h.utility.has_native_records("ULUS10456","YUUSHA.BIN")&&!h.utility.has_native_records("ULUS10456","OTHER.BIN"),"Native record detection is wrong");}
    const auto first=files(fixture.root);
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,0x3c);
        h.memory.store32(h.parameters+0x30,5);h.memory.store32(h.parameters+0x38,1);automatic_step(h,0);}
    const auto second=files(fixture.root);require(second.size()>first.size(),"Automatic overwrite did not retain the previous save");
    for(const auto &[name,data]:first)if(name.extension()==".bsav")require(second.at(name)==data,"Automatic overwrite changed a prior immutable save");
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,0);
        automatic_step(h,0);require(h.memory.load8(h.buffer)==0x3c&&h.memory.load32(h.parameters+0x7c)==0xd38,"Automatic load did not restore the latest profile save");}
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        h.memory.store32(h.parameters+0x30,5);h.memory.store32(h.parameters+0x38,0);
        require(h.utility.init_start(h.memory,h.parameters)==0&&h.utility.get_status()==1&&h.utility.get_status()==2&&h.utility.dialog(),"A non-overwriting save request skipped its real error path");
        require(h.utility.acknowledge()&&h.utility.update(h.memory,2)==0&&h.utility.dialog()&&h.utility.dialog()->result==0x80110385u,"Non-overwriting save did not report preserved data");
        require(h.utility.acknowledge()&&h.utility.update(h.memory,2)==0,"Error acknowledgment failed");close_operation(h);
        require(files(fixture.root)==second,"Rejected automatic save changed files");}
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);h.memory.store32(h.parameters+0x30,7);
        require(h.utility.init_start(h.memory,h.parameters)==0&&h.utility.get_status()==1&&h.utility.get_status()==2&&h.utility.dialog(),"Automatic profile removed Delete confirmation");
        require(h.utility.cancel()&&h.utility.update(h.memory,2)==0,"Delete cancel failed");close_operation(h);require(files(fixture.root)==second,"Cancelled Delete changed files");}
    unsupported([]{Harness h;h.utility.init_start(h.memory,h.parameters);h.utility.set_automatic_profile(true);},"Profile mode changed during an active operation");
}
void automatic_profile_prefers_remembered_slot_and_shows_damage(){
    Fixture fixture;
    {Harness h;h.utility.mount_memory_stick(fixture.root);save(h,"YSave00");}
    {Harness h;h.utility.mount_memory_stick(fixture.root);for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,0x77);save(h,"YSave03");}
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);h.string(h.parameters+0x4c,"YSave03",20);
        for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,0);automatic_step(h,0);require(h.memory.load8(h.buffer)==0x77,"Automatic load ignored the game's remembered slot");}
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        for(unsigned i=0;i<0xd38;++i)h.memory.store8(h.buffer+i,0);automatic_step(h,0);require(h.memory.load8(h.buffer)==0x5a,"Automatic load did not choose the first saved slot");}
    const auto store=fixture.root/Savedata::native_directory_name;
    for(const auto *slot:{"YSave00","YSave03"}){std::ofstream damaged(store/(std::string("ULUS10456-")+slot+"-YUUSHA.BIN.ref"),std::ios::binary|std::ios::trunc);damaged<<"damaged";}
    {Harness h;h.utility.mount_memory_stick(fixture.root);h.utility.set_automatic_profile(true);
        require(h.utility.has_native_records("ULUS10456","YUUSHA.BIN"),"Damaged records were not detected");
        require(h.utility.init_start(h.memory,h.parameters)==0&&h.utility.get_status()==1&&h.utility.get_status()==2&&h.utility.dialog(),"Damaged profile data was hidden by automatic load");
        require(h.utility.cancel()&&h.utility.update(h.memory,2)==0,"Damaged-load cancel failed");close_operation(h);}
}
int run_tests(){
    try{mounted_empty_lifecycle_and_preservation();absent_card_and_real_confirm_edges();request_validation_and_failure_atomicity();
        matching_data_is_explicitly_unsupported_and_preserved();malformed_media_and_import_dispatch();
        native_save_load_metadata_and_immutable_snapshot();overwrite_requires_fresh_confirmation_and_cancel_preserves();
        corrupt_incomplete_and_foreign_data_are_preserved();stale_selection_and_busy_commit_preserve_latest_save();
        corrupted_blob_and_foreign_identity_preserve_outputs();delete_confirmation_cancel_and_preservation();delete_empty_corrupt_stale_and_lock_failures();cross_process_roundtrip();
        automatic_profile_load_save_and_retained_history();automatic_profile_prefers_remembered_slot_and_shows_damage();
        std::cout<<"Savedata lifecycle/private native persistence and cross-process tests passed\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
#if defined(_WIN32)
int wmain(int argc,wchar_t **argv){try{if(argc==3){const std::wstring mode=argv[1];if(mode==L"--write-native"||mode==L"--read-native")roundtrip_child(argv[2],mode==L"--write-native");else if(mode==L"--delete-native"||mode==L"--read-empty")delete_roundtrip_child(argv[2],mode==L"--delete-native");else throw std::runtime_error("Unknown synthetic child command");return 0;}return run_tests();}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
#else
int main(){return run_tests();}
#endif
