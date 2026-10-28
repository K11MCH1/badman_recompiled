#include "badman_registry.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    std::filesystem::path root=parent/("badman-registry-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        require(std::filesystem::create_directory(root),"Fixture directory exists");
        font("jpn0.pgf","Synthetic Sans","Regular");
        font("ltn0.pgf","Synthetic Serif","Italic");
    }
    void font(const char *file,const char *name,const char *style) {
        std::array<std::uint8_t,0x188> b{}; b[2]=0x88;b[3]=1;b[4]='P';b[5]='G';b[6]='F';b[7]='0';b[8]=2;b[12]=6;
        auto word=[&](unsigned offset,std::uint32_t value) {for(unsigned i=0;i<4;++i)b[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
        word(0x24,648);word(0x28,648);word(0x2c,8192);word(0x30,8192);
        for(unsigned i=0;name[i];++i)b[0x35+i]=static_cast<std::uint8_t>(name[i]);
        for(unsigned i=0;style[i];++i)b[0x75+i]=static_cast<std::uint8_t>(style[i]);
        std::ofstream out(root/file,std::ios::binary);out.write(reinterpret_cast<const char *>(b.data()),b.size());
    }
    ~Fixture() {
        std::error_code error;const auto resolved=std::filesystem::weakly_canonical(root,error);
        if(!error&&resolved.parent_path()==parent&&resolved.filename().string().starts_with("badman-registry-test-")) std::filesystem::remove_all(resolved,error);
    }
};
struct Harness {
    psprecomp::Runtime runtime;
    badman::Registry registry;
    psprecomp::GuestMemory &memory=runtime.memory();
    static constexpr std::uint32_t param=0x08800000,out=0x08800200,text=0x08800300,key=0x08800500,type=key+4,size=key+8,buffer=0x08800600;
    explicit Harness(const std::filesystem::path &path):registry(path) {
        for(std::uint32_t i=0;i<0x110;++i)memory.store8(param+i,0);
        memory.store32(param,1);
    }
    void string(const std::string &value) {for(std::size_t i=0;i<=value.size();++i)memory.store8(text+static_cast<std::uint32_t>(i),i==value.size()?0:static_cast<std::uint8_t>(value[i]));}
    std::uint32_t call(std::uint32_t nid,std::uint32_t a0=0,std::uint32_t a1=0,std::uint32_t a2=0,std::uint32_t a3=0,std::uint32_t a4=0) {
        psprecomp::AllegrexContext ctx;ctx.gpr[4]=a0;ctx.gpr[5]=a1;ctx.gpr[6]=a2;ctx.gpr[7]=a3;ctx.gpr[8]=a4;
        registry.invoke(nid,runtime,ctx);return ctx.gpr[2];
    }
    std::uint32_t open() {require(call(0x92e41280,param,2,out)==0,"OpenRegistry failed");return memory.load32(out);}
    std::uint32_t category(std::uint32_t reg,const std::string &path) {string(path);require(call(0x1d8a762e,reg,text,2,out)==0,"OpenCategory failed");return memory.load32(out);}
    std::uint32_t info(std::uint32_t cat,const std::string &name) {string(name);require(call(0xd4475aa8,cat,text,key,type,size)==0,"GetKeyInfo failed");return memory.load32(key);}
};
void metadata_and_abi() {
    Fixture fixture;Harness h(fixture.root);require(h.registry.font_count()==2,"Registry fabricated a fixed firmware count");
    const auto reg=h.open(),font=h.category(reg,"/DATA/FONT");
    auto key=h.info(font,"num_fonts");require(h.memory.load32(h.type)==2&&h.memory.load32(h.size)==4,"Integer type/size incorrect");
    require(h.call(0x28a8e98a,font,key,h.buffer,4)==0&&h.memory.load32(h.buffer)==2,"Actual font count not returned");
    key=h.info(font,"path_name");require(h.memory.load32(h.type)==3&&h.memory.load32(h.size)==14,"Path string type/size incorrect");
    h.memory.store8(h.buffer+14,0xa5);require(h.call(0x28a8e98a,font,key,h.buffer,20)==0,"Path read failed");
    require(h.memory.read_c_string(h.buffer)=="flash0:/font/"&&h.memory.load8(h.buffer+14)==0xa5,"Path termination/tail preservation incorrect");
    require(h.call(0x0cae832b,font)==0,"CloseCategory failed");
    const auto resolution=h.category(reg,"/SYSPROFILE/RESOLUTION");key=h.info(resolution,"horizontal");
    require(h.call(0x28a8e98a,resolution,key,h.buffer,4)==0&&h.memory.load32(h.buffer)==8192,"Header resolution not kept as26.6");
    require(h.call(0x0cae832b,resolution)==0,"Close resolution failed");
    const std::array<const char *,15> fields{"h_size","v_size","h_resolution","v_resolution","weight","family_code","style","sub_style","language_code","region_code","country_code","font_name","file_name","extra_attributes","expire_date"};
    for(unsigned i=0;i<2;++i) {
        const auto cat=h.category(reg,"/DATA/FONT/PROPERTY/INFO"+std::to_string(i));
        for(const char *field:fields) {
            key=h.info(cat,field);const auto size=h.memory.load32(h.size);
            require(h.call(0x28a8e98a,cat,key,h.buffer,size)==0,"Native module property sequence failed");
        }
        key=h.info(cat,"file_name");require(h.call(0x28a8e98a,cat,key,h.buffer,h.memory.load32(h.size))==0,"Filename read failed");
        require(h.memory.read_c_string(h.buffer)==(i==0?"jpn0.pgf":"ltn0.pgf"),"File property is fictional");
        require(h.call(0x0cae832b,cat)==0,"Close INFO category failed");
    }
    require(h.call(0xfa8a5739,reg)==0,"CloseRegistry failed");
}
void failure_atomicity_and_handles() {
    Fixture fixture;Harness h(fixture.root);const auto reg=h.open(),cat=h.category(reg,"/DATA/FONT");
    const auto key=h.info(cat,"num_fonts");h.memory.store32(h.buffer,0xa5a5a5a5);
    require(h.call(0x28a8e98a,cat,key,h.buffer,3)!=0&&h.memory.load32(h.buffer)==0xa5a5a5a5,"Undersized value modified output");
    require(h.call(0x28a8e98a,cat,key,h.buffer,0xffffffff)!=0&&h.memory.load32(h.buffer)==0xa5a5a5a5,"Overflowing value buffer accepted");
    const auto other=h.category(reg,"/DATA/FONT");
    require(h.call(0x28a8e98a,other,key,h.buffer,4)!=0&&h.memory.load32(h.buffer)==0xa5a5a5a5,"Key crossed category ownership");
    h.memory.store32(h.key,0x11223344);h.memory.store32(h.type,0x55667788);h.memory.store32(h.size,0x99aabbcc);h.string("unknown");
    require(h.call(0xd4475aa8,cat,h.text,h.key,h.type,h.size)!=0,"Unknown key succeeded");
    require(h.memory.load32(h.key)==0x11223344&&h.memory.load32(h.type)==0x55667788&&h.memory.load32(h.size)==0x99aabbcc,"Failed key lookup changed outputs");
    h.string("num_fonts");require(h.call(0xd4475aa8,cat,h.text,h.key,h.type,0)!=0&&h.memory.load32(h.key)==0x11223344,"Invalid fifth argument partially wrote outputs");
    require(h.call(0xd4475aa8,cat,h.text,h.key,h.type,h.key)!=0&&h.memory.load32(h.key)==0x11223344,"Aliased info outputs accepted");
    h.memory.store32(h.out,0xabcdef01);h.string("/DATA/FONT");
    require(h.call(0x1d8a762e,reg,h.text,1,h.out)!=0&&h.memory.load32(h.out)==0xabcdef01,"Write-mode category open succeeded");
    require(h.call(0x17768e14,cat,h.text,h.buffer,4)!=0&&h.memory.load32(h.buffer)==0xa5a5a5a5,"Unknown/write operation succeeded");
    require(h.call(0x0cae832b,cat)==0&&h.call(0x28a8e98a,cat,key,h.buffer,4)!=0,"Closed category retained keys");
    require(h.call(0xfa8a5739,reg)==0&&h.call(0x0cae832b,other)!=0,"Closed registry retained child category");
    require(h.call(0xfa8a5739,reg)!=0,"Double-close succeeded");
}
void malformed_configuration_and_parameters() {
    Fixture fixture;Harness h(fixture.root);h.memory.store32(h.out,0xaaaa5555);h.memory.store32(h.param,2);
    require(h.call(0x92e41280,h.param,2,h.out)!=0&&h.memory.load32(h.out)==0xaaaa5555,"Invalid registry type changed output");
    h.memory.store32(h.param,1);for(unsigned i=0;i<256;++i)h.memory.store8(h.param+4+i,'x');
    require(h.call(0x92e41280,h.param,2,h.out)!=0&&h.memory.load32(h.out)==0xaaaa5555,"Unterminated name accepted");
    {std::ofstream out(fixture.root/"broken.pgf",std::ios::binary);out<<"not a PGF";}
    bool rejected=false;try{h.registry.configure_font_directory(fixture.root);}catch(const psprecomp::Error &){rejected=true;}
    require(rejected&&h.registry.font_count()==2,"Bad PGF was accepted or destroyed previous configuration");
}
}
int main(int argc,char **argv) {
    try {
        metadata_and_abi();failure_atomicity_and_handles();malformed_configuration_and_parameters();
        if(argc>1) {badman::Registry actual(argv[1]);require(actual.font_count()>0,"Actual font directory enumerated no fonts");std::cout<<"Actual configured fonts: "<<actual.font_count()<<'\n';}
        std::cout<<"Registry tests passed\n";return 0;
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
