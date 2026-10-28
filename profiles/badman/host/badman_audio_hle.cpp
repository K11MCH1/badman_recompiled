#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <limits>

namespace badman {
namespace {
constexpr std::uint64_t poll_period=1000000; // Owner-thread device poll, not playback completion.
std::uint64_t next_poll(std::uint64_t now) {
    if(now>std::numeric_limits<std::uint64_t>::max()-poll_period)throw psprecomp::Error("Audio poll deadline overflow");
    return now+poll_period;
}
bool valid_channel(std::int32_t channel){return channel>=0&&channel<static_cast<std::int32_t>(Audio::channel_count);}
bool valid_frames(std::uint32_t frames){return frames>=64&&frames<=65472&&!(frames&63u);}
}
void Host::configure_audio_transport(std::unique_ptr<AudioTransport> transport) {
    if(audio_)throw psprecomp::Error("Audio transport cannot change after configuration or reservation");
    audio_=std::make_unique<Audio>(std::move(transport));
}
bool Host::channel_audio_waiter(std::int32_t channel) const {
    return std::any_of(threads_.begin(),threads_.end(),[&](const auto &entry) {
        return entry.second.state==State::waiting&&entry.second.wait_audio&&entry.second.wait_audio->channel==channel;
    });
}
std::optional<std::uint64_t> Host::next_audio_poll_deadline() const {
    if(std::none_of(threads_.begin(),threads_.end(),[](const auto &entry){return entry.second.state==State::waiting&&entry.second.wait_audio;}))return {};
    return audio_poll_deadline_;
}
void Host::poll_audio_waits() {
    if(!next_audio_poll_deadline()){audio_poll_deadline_.reset();return;}
    const auto now=scheduler_now();
    if(now<*audio_poll_deadline_)return;
    if(!audio_)throw psprecomp::Error("Audio waiter has no playback transport");
    audio_->poll(); // Only the owning runtime thread observes device completion.
    bool remaining=false;
    for(auto &[uid,thread]:threads_) {
        (void)uid;
        if(thread.state!=State::waiting||!thread.wait_audio)continue;
        if(audio_->completed(thread.wait_audio->ticket)) {
            thread.context.gpr[2]=thread.wait_audio->result;thread.wait_audio.reset();thread.state=State::ready;
        } else remaining=true;
    }
    audio_poll_deadline_=remaining?std::optional<std::uint64_t>{next_poll(now)}:std::nullopt;
}
void Host::output_audio(Context &ctx,bool panned) {
    const auto channel=static_cast<std::int32_t>(ctx.gpr[4]);
    if(!valid_channel(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_channel);return;}
    auto &state=audio_channels_[channel];
    if(!state.reserved||!audio_){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::not_reserved);return;}
    if(in_interrupt_context()){ctx.gpr[2]=0x80020064u;return;}
    if(!interrupts_enabled_){ctx.gpr[2]=0x800201A7u;return;}
    if(channel_audio_waiter(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::busy);return;}
    auto left=ctx.gpr[5],right=panned?ctx.gpr[6]:ctx.gpr[5];
    const auto address=ctx.gpr[panned?7:6];
    // The unpanned API permits -1 to retain the channel's previous gain.
    if(!panned&&left==0xffffffffu){left=state.left;right=state.right;}
    if(left>0xffffu||right>0xffffu){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_volume);return;}
    if(left>0x8000u||right>0x8000u)throw psprecomp::Error("Audio gain above unity requires saturating PCM support");
    audio_->poll();
    const auto previous=state.last_ticket;
    const bool must_wait=previous&&!audio_->completed(previous);
    const auto now=must_wait?scheduler_now():0;
    const auto deadline=must_wait?next_poll(now):0; // Validate before transport mutation.
    if(address) {
        const auto submission=audio_->submit(runtime_.memory(),channel,left,right,address);
        if(submission.result<0){ctx.gpr[2]=static_cast<std::uint32_t>(submission.result);return;}
        state.last_ticket=submission.ticket;
    } // Null is a real drain: never submit zeroed/fabricated PCM.
    state.left=left;state.right=right;
    if(!must_wait){ctx.gpr[2]=state.frames;return;}
    auto &thread=current();
    thread.wait_audio=AudioWait{channel,previous,state.frames};
    thread.wait_thread=0;thread.wait_semaphore=0;thread.wait_semaphore_count=0;
    thread.wait_event.reset();thread.wait_vblank=false;thread.delay_deadline.reset();
    thread.context=ctx;thread.context.pc=ctx.gpr[31];thread.state=State::waiting;
    if(!audio_poll_deadline_||deadline<*audio_poll_deadline_)audio_poll_deadline_=deadline;
    schedule(ctx);
}
std::optional<Host::SasConfiguration> Host::sas_configuration(std::uint32_t core) const {
    const auto found=sas_cores_.find(psprecomp::GuestMemory::canonical(core));
    return found==sas_cores_.end()?std::nullopt:std::optional<SasConfiguration>{found->second};
}
void Host::install_audio() {
    bind("sceAudio",0x5EC81C55u,"sceAudioChReserve",[this](Context &ctx) {
        const auto requested=static_cast<std::int32_t>(ctx.gpr[4]);
        const auto format=ctx.gpr[6];
        if(!valid_frames(ctx.gpr[5])){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_size);return;}
        if(format!=0&&format!=0x10u){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_format);return;}
        if(requested!=-1&&!valid_channel(requested)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_channel);return;}
        if(in_interrupt_context())throw psprecomp::Error("Audio reservation inside an interrupt is not implemented");
        if(!audio_)audio_=std::make_unique<Audio>(make_native_audio_transport());
        const auto result=audio_->reserve(requested,ctx.gpr[5],format);
        if(result>=0)audio_channels_[result]={true,ctx.gpr[5],0x8000,0x8000,0};
        ctx.gpr[2]=static_cast<std::uint32_t>(result);
    });
    bind("sceAudio",0x6FC46853u,"sceAudioChRelease",[this](Context &ctx) {
        const auto channel=static_cast<std::int32_t>(ctx.gpr[4]);
        if(!valid_channel(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_channel);return;}
        if(!audio_||!audio_channels_[channel].reserved){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::not_reserved);return;}
        if(channel_audio_waiter(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::busy);return;}
        const auto result=audio_->cancel_release(channel);
        if(!result)audio_channels_[channel]={};
        ctx.gpr[2]=static_cast<std::uint32_t>(result);
    });
    bind("sceAudio",0xCB2E439Eu,"sceAudioSetChannelDataLen",[this](Context &ctx) {
        const auto channel=static_cast<std::int32_t>(ctx.gpr[4]);
        if(!valid_channel(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_channel);return;}
        if(!valid_frames(ctx.gpr[5])){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_size);return;}
        if(!audio_||!audio_channels_[channel].reserved){ctx.gpr[2]=0x80260001u;return;}
        if(channel_audio_waiter(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::busy);return;}
        const auto result=audio_->set_length(channel,ctx.gpr[5]);
        if(!result)audio_channels_[channel].frames=ctx.gpr[5];
        ctx.gpr[2]=static_cast<std::uint32_t>(result);
    });
    bind("sceAudio",0xB011922Fu,"sceAudioGetChannelRestLength",[this](Context &ctx) {
        const auto channel=static_cast<std::int32_t>(ctx.gpr[4]);
        if(!valid_channel(channel)){ctx.gpr[2]=static_cast<std::uint32_t>(Audio::invalid_channel);return;}
        ctx.gpr[2]=audio_&&audio_channels_[channel].reserved?static_cast<std::uint32_t>(audio_->rest_length(channel)):0;
    });
    bind("sceAudio",0x136CAF51u,"sceAudioOutputBlocking",[this](Context &ctx){output_audio(ctx,false);});
    bind("sceAudio",0x13F592BCu,"sceAudioOutputPannedBlocking",[this](Context &ctx){output_audio(ctx,true);});
    const auto sas=[this](Context &ctx,std::uint32_t nid) {
        const auto core=ctx.gpr[4];
        if(!core||(core&63u)||!runtime_.memory().contains(core,3616)){ctx.gpr[2]=0x80420005u;return;}
        const auto canonical=psprecomp::GuestMemory::canonical(core);
        if(nid==0x42778A9Fu) {
            const auto grain=ctx.gpr[5],voices=ctx.gpr[6],mode=ctx.gpr[7],rate=ctx.gpr[8];
            if(grain<64||grain>2048||(grain&63u)){ctx.gpr[2]=0x80420001u;return;}
            if(voices<1||voices>32){ctx.gpr[2]=0x80420002u;return;}
            if(mode>1){ctx.gpr[2]=0x80420003u;return;}
            if(rate!=44100){ctx.gpr[2]=0x80420004u;return;}
            if(!sas_cores_.contains(canonical)&&sas_cores_.size()>=64)throw psprecomp::Error("SAS configuration exceeds bounded core count");
            for(const auto &[base,configuration]:sas_cores_) {
                (void)configuration;
                if(base!=canonical&&std::uint64_t(base)<std::uint64_t(canonical)+3616&&std::uint64_t(canonical)<std::uint64_t(base)+3616)
                    throw psprecomp::Error("Overlapping SAS core objects are not implemented");
            }
            SasConfiguration configuration{};configuration.grain=grain;configuration.max_voices=voices;
            configuration.output_mode=mode;configuration.sample_rate=rate;sas_cores_[canonical]=configuration;
            ctx.gpr[2]=0;return;
        }
        const auto found=sas_cores_.find(canonical);
        if(found==sas_cores_.end()){ctx.gpr[2]=0x80420100u;return;}
        if(ctx.gpr[5]>=32){ctx.gpr[2]=0x80420010u;return;}
        auto &voice=found->second.voices[ctx.gpr[5]];
        if(nid==0xAD84D37Fu) {
            if(ctx.gpr[6]>0x4000u){ctx.gpr[2]=0x80420012u;return;}
            voice.pitch=ctx.gpr[6];
        } else if(nid==0x440CA7D8u) {
            std::array<std::int32_t,4> volumes;
            for(unsigned index=0;index<4;++index) {
                volumes[index]=static_cast<std::int32_t>(ctx.gpr[6+index]);
                if(volumes[index]<-4096||volumes[index]>4096){ctx.gpr[2]=0x80420018u;return;}
            }
            voice.volumes=volumes;
        } else {
            if(ctx.gpr[7]&0x2000u){ctx.gpr[2]=0x80420013u;return;}
            if((ctx.gpr[6]|ctx.gpr[7])&0xffff0000u)throw psprecomp::Error("SAS packed ADSR above 16 bits is not implemented");
            voice.envelope1=ctx.gpr[6];voice.envelope2=ctx.gpr[7];
        }
        ctx.gpr[2]=0; // Configuration retained; no synthesis API is installed.
    };
    bind("sceSasCore",0x42778A9Fu,"__sceSasInit",[sas](Context &ctx){sas(ctx,0x42778A9Fu);});
    bind("sceSasCore",0xAD84D37Fu,"__sceSasSetPitch",[sas](Context &ctx){sas(ctx,0xAD84D37Fu);});
    bind("sceSasCore",0x440CA7D8u,"__sceSasSetVolume",[sas](Context &ctx){sas(ctx,0x440CA7D8u);});
    bind("sceSasCore",0xCBCD4F79u,"__sceSasSetSimpleADSR",[sas](Context &ctx){sas(ctx,0xCBCD4F79u);});
}
}
