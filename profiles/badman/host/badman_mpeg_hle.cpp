#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <ostream>

namespace badman {
void Host::configure_video_decoder(const std::filesystem::path &directory) {
    mpeg_.set_decoder_factory([directory] { return make_ffmpeg_video_decoder(directory); });
    mpeg_.set_audio_decoder_factory([directory] { return make_ffmpeg_mpeg_audio_decoder(directory); });
    trace_ << "[video-config] directory=" << directory.string() << '\n';
}
void Host::enter_mpeg_callback(Context &context, const Mpeg::RingCallback &callback) {
    if (!callback.function || (callback.function & 3u) || !runtime_.memory().contains(callback.function, 4))
        throw psprecomp::Error("Invalid MPEG ring callback function");
    context.gpr[4] = callback.data; context.gpr[5] = callback.packets; context.gpr[6] = callback.argument;
    context.gpr[28] = callback.gp;
    context.gpr[31] = mpeg_return_pc;
    context.pc = callback.function;
    trace_ << "[mpeg-callback] uid=" << psprecomp::runtime_thread_uid()
           << " function=" << psprecomp::hex32(callback.function) << " packets=" << callback.packets << '\n';
}
void Host::return_from_mpeg_callback(psprecomp::Runtime &, Context &context) {
    auto &host = *active_;
    const auto uid = psprecomp::runtime_thread_uid();
    const auto pending = host.mpeg_calls_.find(uid);
    if (pending == host.mpeg_calls_.end()) throw psprecomp::Error("MPEG callback returned without its current-thread continuation");
    const auto step = host.mpeg_.complete_put(host.runtime_.memory(), pending->second.ring,
        static_cast<std::int32_t>(context.gpr[2]));
    if (step.callback) {
        host.enter_mpeg_callback(context, *step.callback);
        return;
    }
    const auto caller = pending->second.caller;
    host.mpeg_calls_.erase(pending);
    context = caller;
    context.gpr[2] = static_cast<std::uint32_t>(step.result);
    context.pc = caller.gpr[31];
}
void Host::install_mpeg() {
    runtime_.register_function(mpeg_return_pc, &Host::return_from_mpeg_callback, "badman_mpeg_callback_return");
    const std::array<std::pair<std::uint32_t, const char *>, 25> imports{{
        {0x0E3C2E9D, "sceMpegAvcDecode"}, {0x13407F13, "sceMpegRingbufferDestruct"},
        {0x167AFD9E, "sceMpegInitAu"}, {0x21FF80E4, "sceMpegQueryStreamOffset"},
        {0x37295ED8, "sceMpegRingbufferConstruct"}, {0x42560F23, "sceMpegRegistStream"},
        {0x4571CC64, "sceMpegAvcDecodeFlush"}, {0x591A4AA2, "sceMpegUnRegistStream"},
        {0x606A4649, "sceMpegDelete"}, {0x611E9E11, "sceMpegQueryStreamSize"},
        {0x682A619B, "sceMpegInit"}, {0x707B7629, "sceMpegFlushAllStream"},
        {0x740FCCD1, "sceMpegAvcDecodeStop"}, {0x800C44DF, "sceMpegAtracDecode"},
        {0x874624D6, "sceMpegFinish"}, {0xA780CF7E, "sceMpegMallocAvcEsBuf"},
        {0xB240A59E, "sceMpegRingbufferPut"}, {0xB5F6DC87, "sceMpegRingbufferAvailableSize"},
        {0xC132E22F, "sceMpegQueryMemSize"}, {0xCEB870B1, "sceMpegFreeAvcEsBuf"},
        {0xD7A29F46, "sceMpegRingbufferQueryMemSize"}, {0xD8C5F121, "sceMpegCreate"},
        {0xE1CE83A7, "sceMpegGetAtracAu"}, {0xF8DCB679, "sceMpegQueryAtracEsSize"},
        {0xFE246728, "sceMpegGetAvcAu"}}};
    for (const auto &[nid, name] : imports) bind("sceMpeg", nid, name, [this, nid](Context &context) {
        if (in_interrupt_context()) throw psprecomp::Error("MPEG operations are invalid inside an interrupt callback");
        const auto caller = context;
        const auto callback = mpeg_.invoke(nid, runtime_, context);
        if (!callback) return;
        const auto uid = psprecomp::runtime_thread_uid();
        if (mpeg_calls_.size() >= 16 || !mpeg_calls_.emplace(uid, MpegCall{caller, callback->ring}).second)
            throw psprecomp::Error("Nested or excessive MPEG callback continuations");
        enter_mpeg_callback(context, *callback);
    });
}
}
