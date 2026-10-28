// Shared runtime library for platforms that link the locally generated game
// translation separately (Android): the framework core and Badman host are
// linked in whole, and the game library resolves Runtime symbols from here.
extern "C" __attribute__((visibility("default"))) int badman_runtime_abi_version() { return 1; }
