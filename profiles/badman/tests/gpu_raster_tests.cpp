#include "badman_raster.hpp"
#include "badman_ge.hpp"
#include "badman_clut.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cfenv>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using psprecomp::GuestMemory;
constexpr std::uint32_t vertex_address=0x08810000,texture_address=0x08820000,palette_address=0x08840000;
constexpr std::uint32_t target=0x04020000,code_address=0x08850000;
void require(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
void renderer(const std::string &name){
#if defined(_WIN32)
    require(_putenv_s("BADMAN_RENDERER",name.c_str())==0,"Cannot select renderer");
#else
    require(setenv("BADMAN_RENDERER",name.c_str(),1)==0,"Cannot select renderer");
#endif
}
std::uint32_t random_word(std::uint32_t &state){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
struct Outcome {
    std::uint32_t vertices{},next{};std::uint64_t color{},depth{};std::string error;
    bool operator==(const Outcome &)const=default;
};
struct Fixture {
    std::string backend;GuestMemory memory;std::unique_ptr<badman::Raster> raster;
    std::array<std::uint32_t,256> regs{};badman::Clut palette;badman::Raster::Matrices matrices{};
    explicit Fixture(std::string name):backend(std::move(name)) {
        renderer(backend);raster=std::make_unique<badman::Raster>(memory);
        std::vector<std::uint8_t> initial(GuestMemory::kVramSize);std::uint32_t seed=0xABCD7391;
        for(auto &byte:initial)byte=static_cast<std::uint8_t>(random_word(seed));
        memory.copy_in(GuestMemory::kVramPhysicalBase,initial);
        std::cout<<"BACKEND "<<backend<<": "<<raster->renderer_description()<<'\n';
    }
    void setup(unsigned format,bool swizzled,bool small_pitch,bool linear,unsigned seed) {
        regs={};regs[0x12]=0x0080019F;regs[0x16]=63|(63<<10);regs[0x1C]=1;regs[0x1E]=1;
        regs[0x9C]=target-GuestMemory::kVramPhysicalBase;regs[0x9D]=64;regs[0xD2]=3;
        regs[0xD5]=63|(63<<10);regs[0xA0]=texture_address&0xFFFFFF;regs[0xA8]=0x080000|(small_pitch?32:64);
        regs[0xB8]=6|(4<<8);regs[0xC2]=swizzled?1:0;regs[0xC3]=format;
        regs[0xC5]=0xFF03;regs[0xC6]=linear?0x107:0;regs[0xC7]=(seed&1)?0:0x101;regs[0xC9]=0x100;
        regs[0xB0]=palette_address&0xFFFFFF;regs[0xB1]=0x080000;
        std::uint32_t rng=seed+0x91374623;
        for(unsigned i=0;i<8192;++i)memory.store8(texture_address+i,static_cast<std::uint8_t>(random_word(rng)));
        for(unsigned i=0;i<256;++i)memory.store32(palette_address+i*4,random_word(rng));
        palette=badman::Clut{};palette.load(memory,regs,32);
        raster->invalidate_textures();
    }
    void vertex(unsigned index,float x,float y,float u,float v,std::uint32_t color=0xFFFFFFFF) {
        const auto at=vertex_address+index*24;
        for(auto [offset,value]:{std::pair{0u,u},std::pair{4u,v},std::pair{12u,x},std::pair{16u,y}})
            memory.store32(at+offset,std::bit_cast<std::uint32_t>(value));
        memory.store32(at+8,color);memory.store32(at+20,0);
    }
    Outcome draw(unsigned count=2,std::uint32_t primitive=0x04060000) {
        try {auto result=raster->draw(primitive|count,regs,vertex_address,&palette,&matrices);
            return {result.vertices,result.next_vertex_address,result.color_pixels,result.depth_pixels,{}};}
        catch(const psprecomp::Error &e){return {0,0,0,0,e.what()};}
    }
};
void compare(const Fixture &reference,const Fixture &other,const Outcome &expected,const Outcome &actual,const std::string &label) {
    require(expected==actual,label+" "+other.backend+" results/errors differ: expected '"+expected.error+"', actual '"+actual.error+"'");
    if(reference.memory.vram_bytes()!=other.memory.vram_bytes()) {
        auto mismatch=std::mismatch(reference.memory.vram_bytes().begin(),reference.memory.vram_bytes().end(),other.memory.vram_bytes().begin());
        throw std::runtime_error(label+" "+other.backend+" full VRAM differs at byte "+std::to_string(mismatch.first-reference.memory.vram_bytes().begin())+
            " expected="+std::to_string(*mismatch.first)+" actual="+std::to_string(*mismatch.second));
    }
    require(reference.memory.bytes()==other.memory.bytes(),label+" "+other.backend+" full RAM differs");
}
void standalone(std::vector<std::unique_ptr<Fixture>> &fixtures) {
    unsigned cases=0,errors=0;
    for(unsigned format=0;format<6;++format)for(unsigned state=0;state<32;++state) {
        std::vector<Outcome> results;
        for(auto &f:fixtures) {
            f->setup(format,(state&1)!=0,(state&2)!=0,(state&4)!=0,format*47+state);
            const float x0=(state&8)?34.5625f:1.0625f,x1=(state&8)?1.0625f:34.5625f;
            const float y0=(state&16)?31.5625f:2.0625f,y1=(state&16)?2.0625f:31.5625f;
            f->vertex(0,x0,y0,-1.25f,.5625f,0xB0807060);f->vertex(1,x1,y1,71.9375f,19.25f,0xC3CFA791);
            f->regs[0xD4]=3|(4<<10);f->regs[0xD5]=39|(37<<10);
            f->regs[0xE8]=(state%3==0)?0x00F00F55:0;
            if(state&2){f->regs[0x21]=1;f->regs[0xDF]=0x32;}
            if(state&4){f->regs[0x20]=1;f->regs[0xE2]=0x731F;f->regs[0xE3]=0x28A4;f->regs[0xE4]=0x5E90;f->regs[0xE5]=0xC6BD;}
            results.push_back(f->draw());
        }
        const auto label="format="+std::to_string(format)+" state="+std::to_string(state);
        require(results[0].error.empty(),label+" software fixture unexpectedly rejected: "+results[0].error);
        for(unsigned i=1;i<fixtures.size();++i)compare(*fixtures[0],*fixtures[i],results[0],results[i],label);
        ++cases;
    }
    // Every supported blend equation, scalar color masks and overlapping pairs.
    for(unsigned equation=0;equation<6;++equation)for(unsigned mode=0;mode<3;++mode) {
        std::vector<Outcome> results;
        for(auto &f:fixtures) {
            f->setup(3,false,false,false,900+equation*3+mode);f->regs[0x1E]=0;
            f->regs[0x21]=1;f->regs[0xDF]=(equation<<8)|(mode==0?0x32:mode==1?0x54:0xAA);
            f->regs[0xE0]=0x0012ABF0;f->regs[0xE1]=0x00F04731;f->regs[0xE8]=mode==1?0x00A5C37E:0;
            f->vertex(0,3,4,0,0);f->vertex(1,19,23,0,0,0x8DC45031);
            f->vertex(2,11,9,0,0);f->vertex(3,29,25,0,0,0x31B086EE);
            results.push_back(f->draw(4));
        }
        require(results[0].error.empty(),"Blend fixture rejected "+results[0].error);
        for(unsigned i=1;i<fixtures.size();++i)compare(*fixtures[0],*fixtures[i],results[0],results[i],"blend "+std::to_string(equation)+"/"+std::to_string(mode));
        ++cases;
    }
    for(unsigned stride:{128u,512u,1024u}) {
        std::vector<Outcome> results;
        for(auto &f:fixtures) {
            f->setup(3,false,false,true,1200+stride);f->regs[0x9C]=0x2000;f->regs[0x9D]=stride;
            f->regs[0x16]=1023|(511<<10);f->regs[0xD5]=f->regs[0x16];
            const float x=stride==128?91.f:stride==512?461.f:801.f;
            const float y=stride==128?91.f:stride==512?251.f:401.f;
            f->vertex(0,x,y,-.25f,1.5f);f->vertex(1,x+19,y+17,61.75f,14.5f,0xB7DAE931);
            results.push_back(f->draw());
        }
        require(results[0].error.empty(),"Large stride fixture unexpectedly rejected");
        for(unsigned i=1;i<fixtures.size();++i)compare(*fixtures[0],*fixtures[i],results[0],results[i],"large stride "+std::to_string(stride));
        ++cases;
    }
    // Missing palette entries must retain lazy software error semantics; a
    // later invalid sprite cannot publish an earlier pair from that call.
    for(unsigned which=0;which<6;++which) {
        std::vector<Outcome> results;
        for(auto &f:fixtures) {
            f->setup(which<2?5:3,false,false,false,1100+which);
            f->vertex(0,0,0,0,0);f->vertex(1,8,8,1,1);
            f->vertex(2,12,12,0,0);f->vertex(3,20,20,1,1);
            if(which<2){f->palette=badman::Clut{};f->palette.load(f->memory,f->regs,1);
                for(unsigned i=0;i<8192;++i)f->memory.store8(texture_address+i,which?250:0);}
            if(which==2)f->memory.store32(vertex_address+3*24+16,std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
            if(which==3)f->regs[0xC3]=7;
            if(which==4){f->regs[0xA0]=0x20000;f->regs[0xA8]=0x040040;} // Self-texturing remains explicit unsupported.
            if(which==5)f->regs[0x9C]=0x1FFFF0;
            const auto before=f->memory.vram_bytes();auto result=f->draw(which==2?4:2);
            if(which)require(!result.error.empty()&&before==f->memory.vram_bytes(),f->backend+" bad draw published writes or invented success");
            results.push_back(std::move(result));
        }
        for(unsigned i=1;i<fixtures.size();++i)compare(*fixtures[0],*fixtures[i],results[0],results[i],"invalid/lazy "+std::to_string(which));
        errors+=!results[0].error.empty();++cases;
    }
    std::cout<<"PASS_STANDALONE_FULL_MEMORY cases="<<cases<<" exact_error_cases="<<errors<<" ram_bytes=33554432 vram_bytes=2097152\n";
}

void transform_cache_oracle(std::vector<std::unique_ptr<Fixture>> &fixtures) {
    const auto fp_before=std::fegetround();
    struct Restore {int mode;~Restore(){std::fesetround(mode);}} restore{fp_before};
    const auto base=[](Fixture &f) {
        f.setup(3,false,false,false,2000);f.regs[0x12]=0x19F;f.regs[0xD7]=65535;
        f.regs[0x55]=0xFFFFFF;f.regs[0x58]=255;f.matrices={};
        for(unsigned index:{1u,2u})for(unsigned axis=0;axis<3;++axis)f.matrices[index][axis*3+axis]=0x3F800000;
        for(unsigned axis=0;axis<4;++axis)f.matrices[3][axis*4+axis]=0x3F800000;
        for(auto [reg,value]:{std::pair{0x42u,30.f},std::pair{0x43u,-30.f},std::pair{0x44u,1.f},std::pair{0x45u,31.f},std::pair{0x46u,31.f},std::pair{0x47u,0.f},std::pair{0x48u,1.f},std::pair{0x49u,1.f}})
            f.regs[reg]=std::bit_cast<std::uint32_t>(value)>>8;
        f.vertex(0,-.75f,.75f,.125f,.25f);f.vertex(1,.75f,-.75f,.875f,.75f,0xD3A1C7EF);
    };
    const auto word=[](Fixture &f,unsigned i)->std::uint32_t& {
        if(i<12)return f.matrices[1][i];if(i<24)return f.matrices[2][i-12];
        if(i<40)return f.matrices[3][i-24];return f.regs[0x42+i-40];
    };
    unsigned checkpoints=0,errors=0;
    const auto check=[&](const std::string &label,int mode=FE_TONEAREST,std::uint32_t primitive=0x04060000,unsigned count=2) {
        std::vector<Outcome> results;
        for(auto &f:fixtures){require(std::fesetround(mode)==0,"Cannot select requested floating rounding mode");results.push_back(f->draw(count,primitive));}
        for(unsigned i=1;i<fixtures.size();++i)compare(*fixtures[0],*fixtures[i],results[0],results[i],label);
        ++checkpoints;errors+=!results[0].error.empty();return results[0];
    };
    for(unsigned i=0;i<46;++i) {
        for(auto &f:fixtures)base(*f);
        require(check("transform warm "+std::to_string(i)).error.empty(),"Base transform invalid");
        for(auto &f:fixtures){auto &raw=word(*f,i);const auto value=std::bit_cast<float>(i<40?raw:raw<<8);raw=std::bit_cast<std::uint32_t>(value+.0625f);if(i>=40)raw>>=8;}
        check("transform mutate raw word "+std::to_string(i));
        for(auto &f:fixtures)base(*f);
        require(check("transform restore key "+std::to_string(i)).error.empty(),"Good transform failed after mutated key");
    }
    for(unsigned i=0;i<46;++i) {
        for(auto &f:fixtures)base(*f);
        require(check("NaN warm "+std::to_string(i)).error.empty(),"Base transform invalid before NaN");
        for(auto &f:fixtures)word(*f,i)=i<40?0x7FC00001u:0x007FC000u;
        require(!check("NaN key "+std::to_string(i)).error.empty(),"NaN transform key unexpectedly succeeded");
        for(auto &f:fixtures)base(*f);
        require(check("restore after NaN "+std::to_string(i)).error.empty(),"Failed miss poisoned a valid transform cache entry");
    }
    for(auto &f:fixtures)base(*f);check("overflow warm");
    for(auto &f:fixtures){f->matrices[1][0]=0x7F7FFFFF;f->matrices[2][0]=0x7F7FFFFF;}
    require(!check("combined matrix arithmetic overflow").error.empty(),"Overflowed transform succeeded");
    for(auto &f:fixtures)base(*f);check("recover after overflow");
    for(auto &f:fixtures){word(*f,0)=0x7FC00000;f->regs[0xC3]=7;}
    require(!check("invalid texture precedes invalid transform").error.empty(),"Compound malformed transform accepted");
    for(auto &f:fixtures)base(*f);check("live warm");
    const auto before_geometry=fixtures[0]->memory.vram_bytes();
    for(auto &f:fixtures)f->vertex(1,.25f,-.5f,.75f,.5625f,0xC8E137AA);
    require(check("live positions UV colors under same matrix key").error.empty(),"Live vertex cache regression");
    require(fixtures[0]->memory.vram_bytes()!=before_geometry,"Live vertex fixture did not actually change pixels");
    const auto before_texture=fixtures[0]->memory.vram_bytes();
    for(auto &f:fixtures)for(unsigned i=0;i<8192;++i)f->memory.store8(texture_address+i,0xD3);
    require(check("live entire texture under same matrix key").error.empty(),"Live texture cache regression");
    require(fixtures[0]->memory.vram_bytes()!=before_texture,"Live texture fixture did not actually change sampled pixels");
    for(auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO,FE_TONEAREST}) {
        for(auto &f:fixtures)base(*f);
        check("fenv cold "+std::to_string(mode),mode);check("fenv repeat "+std::to_string(mode),mode);
    }
    std::fesetround(FE_TONEAREST);
    // Restore valid arrays while live invalid vertices exercise their original
    // error independently from the transform cache; then verify recovery.
    for(auto &f:fixtures)base(*f);check("bad vertex warm");
    for(auto &f:fixtures)f->memory.store32(vertex_address+24+12,0x7FC00000);
    require(!check("live invalid vertex on cache hit").error.empty(),"Cached transform suppressed bad vertex");
    for(auto &f:fixtures)base(*f);check("recovered vertex");
    // Shared valid transform can be used across ordinary transformed sprites
    // and software-rasterized strips selected under a GPU backend.
    for(auto &f:fixtures) {
        base(*f);f->regs[0x12]=0x1E3;
        const std::array<std::array<float,8>,4> values{{{.125f,.25f,0,0,1,-.5f,.5f,0},
            {.125f,.75f,0,0,1,-.5f,-.5f,0},{.875f,.25f,0,0,1,.5f,.5f,0},{.875f,.75f,0,0,1,.5f,-.5f,0}}};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<8;++j)f->memory.store32(vertex_address+i*32+j*4,std::bit_cast<std::uint32_t>(values[i][j]));
    }
    require(check("strip reuses valid sprite transform",FE_TONEAREST,0x04040000,4).error.empty(),"Cross primitive transform failed");
    for(auto &f:fixtures)base(*f);check("sprite reuses valid strip transform");
    std::cout<<"PASS_TRANSFORM_CACHE_ORACLE checkpoints="<<checkpoints<<" raw_key_mutations=46 nan_keys=46 exact_errors="<<errors<<" fenv_modes=4 live_vertices=1 cross_primitive=1\n";
}

void ge_lists(const std::vector<std::string> &backends) {
    struct GeFixture {
        GuestMemory memory;std::vector<std::uint32_t> seen;std::unique_ptr<badman::Ge> ge;std::string error;
        explicit GeFixture(const std::string &name){renderer(name);ge=std::make_unique<badman::Ge>(memory,[this](const auto &e){
            seen.push_back(e.token);seen.push_back(memory.load32(target));});}
    };
    std::vector<std::unique_ptr<GeFixture>> fixtures;
    for(const auto &name:backends)fixtures.push_back(std::make_unique<GeFixture>(name));
    for(auto &f:fixtures) {
        std::uint32_t at=code_address;
        const auto cmd=[&](unsigned opcode,std::uint32_t value){f->memory.store32(at,(opcode<<24)|(value&0xFFFFFF));at+=4;};
        cmd(0x10,0x080000);cmd(0x01,vertex_address&0xFFFFFF);
        for(auto [reg,value]:{std::pair{0x12u,0x80019Fu},std::pair{0x16u,63u|(63u<<10)},std::pair{0x1Cu,1u},std::pair{0x1Eu,0u},
                std::pair{0x9Cu,0x20000u},std::pair{0x9Du,64u},std::pair{0xD2u,3u},std::pair{0xD5u,63u|(63u<<10)}})cmd(reg,value);
        for(unsigned pair=0;pair<3;++pair) {
            for(unsigned i=0;i<2;++i){const auto address=vertex_address+(pair*2+i)*24;
                f->memory.store32(address,0);f->memory.store32(address+4,0);f->memory.store32(address+8,0xFF102030+pair*0x20406);
                f->memory.store32(address+12,std::bit_cast<std::uint32_t>(i?float(7+pair*3):0.f));
                f->memory.store32(address+16,std::bit_cast<std::uint32_t>(i?float(9+pair*2):0.f));f->memory.store32(address+20,0);}
            cmd(0x04,0x060002);
            if(pair==0){cmd(0x0E,0x010055);cmd(0x0C,0);} // Signal must expose completed prior GPU writes.
        }
        cmd(0x0F,0x66);cmd(0x0C,0);
        constexpr auto callback=0x08860000u;
        f->memory.store32(callback,0x08870000);f->memory.store32(callback+4,0);
        f->memory.store32(callback+8,0x08870004);f->memory.store32(callback+12,0);
        auto id=f->ge->set_callback(callback);auto list=f->ge->enqueue(code_address,0,id);
        require(f->ge->list_status(list,true)==0&&f->ge->completed_lists()==1,"GE fixture did not finish normally");
        require(f->seen.size()==4&&f->seen[0]==0x55&&f->seen[2]==0x66,"GE callback order lost");
        require(f->seen[1]==0x00102030&&f->seen[3]==0x0014283C,"GE callback observed incomplete GPU writes");
        // A real guest stall must expose the completed prefix without inventing
        // list completion. Releasing it continues the same list normally.
        constexpr auto stalled_code=code_address+0x1000;
        f->memory.store32(stalled_code,0x01810000);
        f->memory.store32(stalled_code+4,0x04060002);
        f->memory.store32(stalled_code+8,0x0F000077);f->memory.store32(stalled_code+12,0x0C000000);
        f->memory.store32(vertex_address+24+8,0xFF634281);
        auto stalled=f->ge->enqueue(stalled_code,stalled_code+8,id);
        require(f->ge->list_status(stalled,false)==3&&f->ge->completed_lists()==1,
                "GE stall invented completion or lost running prefix");
        require(f->memory.load32(target)==0x00634281,"GE stalled prefix was not flushed to VRAM");
        f->ge->update_stall(stalled,0);
        require(f->ge->list_status(stalled,true)==0&&f->ge->completed_lists()==2,"GE stall did not resume normally");
        // Invalid later pair: the whole current draw is rejected before any
        // target writes, and exact GE-wrapped error/counters match software.
        constexpr auto bad_code=code_address+0x2000;
        f->memory.store32(bad_code,0x01810000);f->memory.store32(bad_code+4,0x04060004);
        f->memory.store32(bad_code+8,0x0F000088);f->memory.store32(bad_code+12,0x0C000000);
        f->memory.store32(vertex_address+3*24+16,std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
        const auto before=f->memory.vram_bytes();
        try{f->ge->enqueue(bad_code,0,id);}catch(const psprecomp::Error &e){f->error=e.what();}
        require(!f->error.empty()&&f->memory.vram_bytes()==before&&f->ge->completed_lists()==2,
                "Malformed later GE pair published writes or false list completion");
    }
    for(unsigned i=1;i<fixtures.size();++i) {
        require(fixtures[0]->memory.vram_bytes()==fixtures[i]->memory.vram_bytes(),"Deferred GE full VRAM differs for "+backends[i]);
        require(fixtures[0]->memory.bytes()==fixtures[i]->memory.bytes(),"Deferred GE full RAM differs for "+backends[i]);
        require(fixtures[0]->seen==fixtures[i]->seen,"Deferred GE callback values differ");
        require(fixtures[0]->error==fixtures[i]->error,"Deferred GE exact error differs");
        require(fixtures[0]->ge->color_pixels()==fixtures[i]->ge->color_pixels(),"Deferred GE color result differs");
        require(fixtures[0]->ge->executed_commands()==fixtures[i]->ge->executed_commands(),"Deferred GE command count differs");
    }
    std::cout<<"PASS_DEFERRED_GE_FULL_MEMORY callbacks=3 successful_sprites=4 stalled_resume=1 exact_atomic_error=1\n";
}
}
int main(int argc,char **argv) {
    const char *prior=std::getenv("BADMAN_RENDERER");const std::string previous=prior?prior:"";
    try {
        std::vector<std::string> backends{"software"};
        if(argc==1){backends.push_back("d3d11");backends.push_back("vulkan");}
        else for(int i=1;i<argc;++i)if(std::string(argv[i])!="software")backends.emplace_back(argv[i]);
        std::vector<std::unique_ptr<Fixture>> fixtures;for(const auto &name:backends)fixtures.push_back(std::make_unique<Fixture>(name));
        standalone(fixtures);transform_cache_oracle(fixtures);fixtures.clear();ge_lists(backends);renderer(previous);
        std::cout<<"PASS_GPU_RASTER_EXACT backends="<<backends.size()<<" tolerance=0\n";return 0;
    }catch(const std::exception &e){renderer(previous);std::cerr<<e.what()<<'\n';return 1;}
}
