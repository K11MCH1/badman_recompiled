#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <ostream>
#include <string>

namespace badman {
using namespace psprecomp;
void Host::install_self_exit() {
    bind("ModuleMgrForUser", 0x8F2DF740u, "sceKernelStopUnloadSelfModuleWithStatus", [this](Context &ctx) {
        // This is the title's verified CRT fallback at 08813404, not a generic
        // unload API. Its only imported call supplies status1 and four zeros.
        // The main module is the native application's lifetime owner; stopping
        // its execution lets main unwind Host and close the actual resources.
        // The separately translated font module must use Stop/UnloadModule.
        if (runtime_.stopped()) return;
        const auto thread=threads_.find(runtime_thread_uid());
        if (in_interrupt_context() || thread==threads_.end() || thread->second.state!=State::running) {
            ctx.gpr[2]=0x80020064u;return;
        }
        if (GuestMemory::canonical(ctx.pc)!=0x088E9AF0u ||
            GuestMemory::canonical(ctx.gpr[31])!=0x0881340Cu || image_end_<0x088E9AF8u ||
            GuestMemory::canonical(thread->second.entry)<0x08804000u ||
            GuestMemory::canonical(thread->second.entry)>=image_end_) {
            ctx.gpr[2]=0x80020136u;return;
        }
        if (ctx.gpr[4]!=1u || ctx.gpr[5] || ctx.gpr[6] || ctx.gpr[7] || ctx.gpr[8]) {
            ctx.gpr[2]=0x800200D2u;return;
        }
        trace_ << "[module-self-exit] main uid=1 exit_status=1; native resource teardown follows\n";
        // Preserve v0 and all guest memory. Success has no guest return into the
        // module being retired; the generated import wrapper sees stopped().
        runtime_.stop("Badman main module requested self-unload with status 1");
    });
}
}
