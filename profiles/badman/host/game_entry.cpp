// Entry points of the locally generated game library (libbadman_game.so).
// The independent runtime library loads it with dlopen and resolves these by
// name, so the generated translation is the only code built from the ISO.
#include "psprecomp/runtime.hpp"

namespace psprecomp { void register_badman_font_functions(Runtime &); }

extern "C" {
__attribute__((visibility("default"))) int badman_game_abi_version() { return 1; }
__attribute__((visibility("default"))) void badman_register_generated_functions(psprecomp::Runtime &runtime) {
    psprecomp::register_generated_functions(runtime);
}
__attribute__((visibility("default"))) void badman_register_font_functions(psprecomp::Runtime &runtime) {
    psprecomp::register_badman_font_functions(runtime);
}
}
