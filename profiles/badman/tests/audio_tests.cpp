#include "badman_audio.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using badman::Audio;
using psprecomp::GuestMemory;
constexpr std::uint32_t source=0x08810000;
void require(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F operation,const char *message) {
    bool failed=false;try{operation();}catch(const psprecomp::Error &){failed=true;}require(failed,message);
}
struct Record {
    std::uint32_t channel{};std::uint64_t ticket{};
    std::span<const std::int16_t> samples;
    std::vector<std::int16_t> expected;
};
struct Observed {
    std::array<bool,8> open{};
    std::vector<Record> retained;
    std::vector<std::vector<std::int16_t>> submitted;
    std::vector<badman::AudioProgress> updates;
    bool fail_open{},fail_submit{},fail_poll{},live_on_close{true};
    unsigned opens{},closes{},polls{};
};
class Transport final:public badman::AudioTransport {
    std::shared_ptr<Observed> observed_;
public:
    explicit Transport(std::shared_ptr<Observed> observed):observed_(std::move(observed)){}
    void open(std::uint32_t channel) override {
        if(observed_->fail_open)throw psprecomp::Error("Injected device open failure");
        require(channel<8&&!observed_->open[channel],"Transport channel opened twice");
        observed_->open[channel]=true;++observed_->opens;
    }
    void close(std::uint32_t channel) noexcept override {
        for(const auto &record:observed_->retained)if(record.channel==channel)
            observed_->live_on_close &= std::equal(record.samples.begin(),record.samples.end(),record.expected.begin(),record.expected.end());
        std::erase_if(observed_->retained,[&](const auto &record){return record.channel==channel;});
        observed_->open[channel]=false;++observed_->closes;
    }
    void submit(std::uint32_t channel,std::uint64_t ticket,std::span<const std::int16_t> samples) override {
        if(observed_->fail_submit)throw psprecomp::Error("Injected queue submission failure");
        require(observed_->open[channel]&&samples.size()%2==0,"Transport received invalid stereo submission");
        observed_->retained.push_back({channel,ticket,samples,{samples.begin(),samples.end()}});
        observed_->submitted.emplace_back(samples.begin(),samples.end());
    }
    std::vector<badman::AudioProgress> poll() override {
        ++observed_->polls;
        if(observed_->fail_poll)throw psprecomp::Error("Injected asynchronous device failure");
        auto updates=std::move(observed_->updates);observed_->updates.clear();
        for(const auto &update:updates)if(update.complete)
            std::erase_if(observed_->retained,[&](const auto &record){return record.ticket==update.ticket;});
        return updates;
    }
};
struct Fixture {
    GuestMemory memory;
    std::shared_ptr<Observed> observed=std::make_shared<Observed>();
    Audio audio{std::make_unique<Transport>(observed)};
    Fixture() { fill(-32768,32767); }
    void fill(std::int16_t left,std::int16_t right) {
        for(unsigned i=0;i<256;++i){memory.store16(source+i*4,std::bit_cast<std::uint16_t>(left));memory.store16(source+i*4+2,std::bit_cast<std::uint16_t>(right));}
    }
};
void reservation_validation_and_release() {
    Fixture f;
    require(f.audio.reserve(-1,63,0)==Audio::invalid_size&&f.audio.reserve(-1,65,0)==Audio::invalid_size&&
            f.audio.reserve(-1,65536,0)==Audio::invalid_size&&f.audio.reserve(-1,64,0x20)==Audio::invalid_format,
            "Invalid PCM lengths/formats reached the transport");
    require(f.audio.reserve(-2,64,0)==Audio::invalid_channel&&f.audio.reserve(8,64,0)==Audio::invalid_channel,
            "Invalid requested audio channel accepted");
    require(f.observed->opens==0,"Invalid reservations opened a device voice");
    for(int i=0;i<8;++i)require(f.audio.reserve(-1,i==7?65472:64,0)==i,"Auto-reservation did not allocate an available channel");
    require(f.audio.reserve(-1,64,0)==Audio::no_channel&&f.audio.reserve(0,64,0)==Audio::busy,"Channel capacity/ownership ignored");
    require(f.audio.set_length(0,64)==0&&f.audio.set_length(0,32)==Audio::invalid_size,"Channel length validation incorrect");
    require(f.audio.release(3)==0&&!f.observed->open[3]&&f.audio.reserve(3,64,0x10)==3,"Released channel was not reusable");
    require(f.audio.release(-1)==Audio::invalid_channel&&f.audio.rest_length(8)==Audio::invalid_channel,"Invalid query/release channel accepted");
}
void snapshots_gain_queue_and_progress() {
    Fixture f;require(f.audio.reserve(0,64,0)==0,"Stereo reservation failed");
    const auto first=f.audio.submit(f.memory,0,0x8000,0x4000,source);
    require(first.result==0&&first.ticket!=0&&f.observed->submitted[0].size()==128&&
            f.observed->submitted[0][0]==-32768&&f.observed->submitted[0][1]==16383,
            "Stereo PCM copy, signed sample interpretation or independent gains incorrect");
    f.fill(-2000,2000);
    const auto second=f.audio.submit(f.memory,0,0,0x8000,source);
    require(second.result==0&&second.ticket>first.ticket&&f.observed->submitted[1][0]==0&&f.observed->submitted[1][1]==2000,
            "Second packet did not preserve its own gains/source snapshot");
    require(f.observed->retained[0].samples[0]==-32768&&f.observed->retained[0].samples[1]==16383,
            "Guest source rewrite or new packet gain changed queued PCM");
    require(f.audio.submit(f.memory,0,0x8000,0x8000,0).result==Audio::busy&&f.observed->submitted.size()==2,
            "Full queue read guest memory or accepted a third outstanding buffer");
    require(f.audio.rest_length(0)==128&&f.audio.release(0)==Audio::busy&&!f.audio.completed(first.ticket),
            "Pending samples were fabricated complete/released");
    f.observed->updates={{first.ticket,32,false}};
    require(f.audio.rest_length(0)==96&&!f.audio.completed(first.ticket),"Partial transport consumption did not update remaining frames");
    f.observed->updates={{first.ticket,64,true}};
    f.audio.poll();
    require(f.audio.completed(first.ticket)&&!f.audio.completed(second.ticket)&&f.audio.rest_length(0)==64,
            "Completion was lost or unrelated packet completed");
    require(!f.audio.completed(0)&&!f.audio.completed(second.ticket+100),"Unknown ticket fabricated completion");
    require(f.audio.set_length(0,128)==0,"New packet length rejected");
    const auto third=f.audio.submit(f.memory,0,0x8000,0x8000,source);
    require(third.result==0&&f.observed->submitted.back().size()==256&&f.audio.rest_length(0)==192,
            "New channel length changed an older packet or had wrong storage size");
    f.observed->updates={{second.ticket,64,true},{third.ticket,128,true}};
    require(f.audio.rest_length(0)==0&&f.audio.completed(third.ticket)&&f.audio.release(0)==0,
            "Completed packets did not release queue slots/channel ownership");
    require(f.audio.completed(first.ticket)&&f.audio.completed(second.ticket),"Channel release lost scheduler-visible ticket completion");
    const auto statistics=f.audio.statistics();
    require(statistics.submitted_buffers==3&&statistics.submitted_frames==256&&statistics.nonzero_samples==448&&
            statistics.completed_buffers==3&&statistics.completed_frames==256,
            "Playback diagnostics did not distinguish scaled PCM from accepted and actually completed buffers");
}
void cancellation_uses_close_barrier_without_played_completion() {
    Fixture f;require(f.audio.reserve(0,64,0)==0,"Cancel fixture reservation failed");
    const auto packet=f.audio.submit(f.memory,0,0x8000,0x8000,source);
    require(f.audio.cancel_release(0)==0&&f.observed->closes==1&&f.observed->retained.empty()&&f.observed->live_on_close,
        "Cancellation freed live PCM before the transport close barrier");
    require(!f.audio.completed(packet.ticket),"Cancelled PCM ticket was fabricated as played completion");
    require(f.audio.reserve(0,64,0)==0,"Cancelled channel remained reserved");
    const auto next=f.audio.submit(f.memory,0,0x8000,0x8000,source);
    f.observed->updates={{next.ticket,64,true}};f.audio.poll();
    require(f.audio.completed(next.ticket)&&!f.audio.completed(packet.ticket),"Later playback erased negative cancellation evidence");
    require(f.audio.statistics().submitted_frames==128&&f.audio.statistics().completed_frames==64,
        "Cancelled audio was counted as genuinely completed playback");
}
void mono_expansion_and_buffer_errors() {
    Fixture f;require(f.audio.reserve(2,64,0x10)==2,"Mono reservation failed");
    f.memory.store16(source,static_cast<std::uint16_t>(-1000));
    f.memory.store16(source+2,3000);
    const auto queued=f.audio.submit(f.memory,2,0x4000,0x8000,source);
    require(queued.result==0&&f.observed->submitted[0][0]==-500&&f.observed->submitted[0][1]==-1000&&
            f.observed->submitted[0][2]==1500&&f.observed->submitted[0][3]==3000,
            "Mono samples were not expanded to independently panned stereo");
    require(f.audio.submit(f.memory,2,0x8001,0,source).result==Audio::invalid_volume&&
            f.audio.submit(f.memory,1,0,0,source).result==Audio::not_reserved,
            "Invalid volume/unreserved channel accepted");
    rejects([&]{f.audio.submit(f.memory,2,0,0,source+1);},"Unaligned PCM source accepted");
    rejects([&]{f.audio.submit(f.memory,2,0,0,0x09FFFFF0);},"Partially valid PCM source footprint accepted");
    require(f.observed->submitted.size()==1,"Invalid PCM source queued a packet");
}
void cleanup_and_real_failure_paths() {
    auto observed=std::make_shared<Observed>();GuestMemory memory;
    memory.store16(source,1234);
    {
        Audio audio(std::make_unique<Transport>(observed));
        require(audio.reserve(0,64,0)==0,"Cleanup fixture reservation failed");
        require(audio.submit(memory,0,0x8000,0x8000,source).result==0,"Cleanup fixture submission failed");
    }
    require(observed->closes==1&&observed->retained.empty()&&observed->live_on_close,
            "Audio destructor freed PCM before closing its asynchronous transport");
    Fixture f;f.observed->fail_open=true;
    rejects([&]{f.audio.reserve(0,64,0);},"Playback device open failure fabricated a channel");
    f.observed->fail_open=false;require(f.audio.reserve(0,64,0)==0,"Failed open left channel reserved");
    const auto first=f.audio.submit(f.memory,0,0x8000,0x8000,source);
    f.observed->fail_submit=true;
    rejects([&]{f.audio.submit(f.memory,0,0x8000,0x8000,source);},"Transport submission failure fabricated accepted PCM");
    require(f.observed->retained.size()==1&&f.observed->retained[0].ticket==first.ticket,"Failed submit corrupted earlier packet ownership");
    require(f.audio.statistics().submitted_buffers==1&&f.audio.statistics().completed_buffers==0,
        "Failed transport submission inflated accepted/completed playback diagnostics");
    rejects([&]{f.audio.rest_length(0);},"Failed audio transport continued returning invented progress");
    Fixture asynchronous;asynchronous.observed->fail_poll=true;
    rejects([&]{asynchronous.audio.poll();},"Asynchronous device error was silently ignored");
}
void malformed_progress_and_thread_ownership() {
    for(unsigned kind=0;kind<4;++kind) {
        Fixture f;f.audio.reserve(0,64,0);
        const auto queued=f.audio.submit(f.memory,0,0x8000,0x8000,source);
        if(kind==0)f.observed->updates={{queued.ticket+99,0,false}};
        if(kind==1)f.observed->updates={{queued.ticket,65,false}};
        if(kind==2)f.observed->updates={{queued.ticket,63,true}};
        if(kind==3)f.observed->updates={{queued.ticket,32,false},{queued.ticket,16,false}};
        rejects([&]{f.audio.poll();},"Malformed callback progress was silently accepted");
    }
    Fixture f;f.audio.reserve(0,64,0);
    const auto queued=f.audio.submit(f.memory,0,0x8000,0x8000,source);
    f.observed->updates={{queued.ticket,32,false}};f.audio.poll();
    f.observed->updates={{queued.ticket,31,false}};
    rejects([&]{f.audio.poll();},"Backward audio sample cursor accepted");
    Fixture owner;bool rejected=false;
    std::thread wrong_thread([&]{try{owner.audio.reserve(0,64,0);}catch(const psprecomp::Error &){rejected=true;}});
    wrong_thread.join();require(rejected&&owner.observed->opens==0,"Audio accepted a non-owner host thread call");
    rejects([]{Audio missing(nullptr);},"Missing playback transport silently substituted silence");
}
}
int main() {
    try {
        reservation_validation_and_release();snapshots_gain_queue_and_progress();cancellation_uses_close_barrier_without_played_completion();mono_expansion_and_buffer_errors();
        cleanup_and_real_failure_paths();malformed_progress_and_thread_ownership();
        std::cout<<"Audio transport/PCM ownership tests passed; native playback device unverified\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
