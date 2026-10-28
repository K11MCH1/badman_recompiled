# PSP static recompilation: working playbook

These notes record techniques verified while bringing up Badman. They are a
starting checklist for another title, not a claim that the current host supports
arbitrary PSP games. Update this file as new evidence changes the workflow.

## Establish the exact inputs

- Keep the ISO and extracted data ignored. Read `PARAM.SFO` for disc ID/version;
  hash both the ISO and the executable used for translation.
- Check executable bytes rather than trusting filenames. `BOOT.BIN` may be
  entirely zero-filled. `~PSP` indicates an encrypted/compressed module; an ELF
  begins with `7F 45 4C 46`.
- Use an external preparation tool when decryption is necessary. Keep its
  source/license separate from this MIT framework. Never import a decryption
  implementation into the reusable runtime merely to prepare local inputs.
- Record ELF entry, module GP, load segments, file-backed extent, and the full
  memory extent including zero-initialized data. Allocate guest heaps/stacks
  beyond the memory extent, not merely beyond the executable file's size.

Badman identity and addresses belong in `profiles/badman/config/executable.json`.
Do not reuse its addresses or executable hash for another game.

## Pin compatible tools and verify them

The tested Windows setup uses Visual Studio 2022 C++ Build Tools, Windows SDK
10.0.26100, CMake, Ninja, and Python 3.11+. Build/test the framework before
debugging a game. Keep compiler parallelism bounded: MSBuild project parallelism
multiplies compiler `/MP` parallelism and can exhaust memory on generated code.

For the supplied CE-Ghidra-MCP-Pack, the matching combination is Ghidra 12.1.3,
Allegrex extension v21.4, Java 21, and the pack's MCP plugin 7.0.0. That plugin is
version-specific; installing the latest Ghidra does not establish compatibility.
Python 3.13 is used for its local bridge environment. Cheat Engine is not required
for the static pipeline.

Verify MCP initialization, tool enumeration, program metadata, and one real
decompilation. A registration in Codex configuration is not evidence that this
chat has loaded the tools. The local stdio client can verify/use the bridge even
when its tools are absent from the chat catalog.

Headless Ghidra has distinct management tools: `import_file` and `open_program`
require GUI mode in this pack; `load_program_from_project` works headlessly.
`analyzeHeadless` can import/analyze/export first, after which the MCP backend can
open that saved project. Close the project before a separate process edits it.
Some bridge errors are JSON objects inside `structuredContent.result` despite
`isError=false`; inspect both layers.

## Discover code from more than section flags

1. Run `psp_analyze` and retain import/function reports locally.
2. Import the decrypted ELF into Ghidra using its PSP ELF loader and
   `Allegrex:LE:32:default` language.
3. Inspect the executable entry and materialized function pointers: thread
   entries, callbacks, constructor tables, virtual tables and jump tables.
4. Define proven missing entries, rerun analysis, and export defined instruction
   runs. Keep data out of the map unless instruction discovery supports it.
5. Validate indirect targets seen at runtime. A missing PC can be undiscovered
   code; first examine the bytes and caller before changing runtime behavior.

Two Badman examples matter for other games:

- `user_main` was passed to `sceKernelCreateThread` but remained a Ghidra label,
  rather than a function. The generic analyzer did find it as a code pointer.
- All 113 static constructors reside in `.data`. The ELF's executable load
  segment covers them even though that individual section has no executable
  flag. A `.text`-only translation reached a genuine missing code PC. The init
  walker supplied the constructor table bounds, avoiding a blind sweep of data.

`DefineBadmanEntryPoints.java` records those title-specific seeds;
`ExportBadmanCode.java` exports bounded contiguous runs. Preserve delay-slot
boundaries when partitioning. Do not transplant those seeds into another title.

## Prepare auxiliary modules and reject unsupported formats

Inspect bundled PRXs separately from the main executable. Badman's libfont.prx
has a 64-byte `~SCE` envelope around a `~PSP` module. The standalone preparation
needed envelope removal, external decryption, and gzip decompression before the
actual ELF appeared. Check magic bytes after every step, and record all hashes;
a filename ending in `.elf` does not make compressed bytes an ELF.

A compact PRX may have no section headers. Its program headers can carry a
`PT_PSPREL2` compressed relocation stream (`0x700000A1`). The framework previously
looked only at its supported section-based relocation records and could report
zero relocations for that unsupported input. It now rejects that stream before
loading/patching memory. Do not bypass the guard by treating unrelocated pointers
as valid addresses. Implement or independently normalize the format first.

Library PRXs may use ELF entry `0xFFFFFFFF`; preserve that sentinel instead of
adding a load base and wrapping to `base - 1`. Module lifecycle entry points may
instead be in its export table. Relocation, export/import binding, GP changes,
and non-overlapping placement all need validation before executing a new module.

An external loader can also serve as a preparation step. For this PRX, Ghidra's
PSP loader applied all 902 relocations at each of two research bases. Exporting
the relocated PT_LOAD bytes and wrapping them in a fixed-address ET_EXEC made
the existing analyzer usable without copying a relocation decoder. Check all
imports, exports, metadata pointers, zero-initialized data and relocation status
at both bases; an exit code alone does not establish a successful export.
Keep this derived input and its hash manifest ignored. This is preparation,
not evidence that the module executes correctly.

Seed actual module function exports and audit relocated data pointers into code.
Compare a Ghidra instruction map with speculative automatic discovery before
estimating decoder gaps: in this module the automatic report counted 43
unsupported occurrences, while the audited map's six unsupported emissions were
all explicit BREAK traps. Counts from different discovery methods describe
different coverage. Runtime-provided callback targets still need tracing.

Read GP from relocated metadata. A raw GP value may be relative to a different
load segment, so adding the module base directly can be wrong. A valid GP may
point beyond the module's mapped bytes because it anchors a signed-offset window.
Reserve module memory before choosing a runtime base, and preserve each module's
GP across calls and callbacks. Generated import-cache slots must be unique across
all loaded modules: the current runtime caches by slot, not by library/NID.

Test placement against the game's future contiguous allocations too. Reserving
Badman's small font module at a convenient research address split the free arena
and prevented a later 19.5 MiB fixed pool, despite ample total free space. The
module therefore needs a different external relocation base near the top of user
RAM, below the existing thread/bootstrap stacks. Never silently move a fixed
translation without regenerating relocated bytes and address maps.

Forwarding a library import to translated code can preserve its real guest
stack, RA and PSP thread. The generated import wrapper retains a changed PC;
ordinary dispatch can select GP from the destination's owning module and do the
same on callback/return. This currently depends on non-chainable manual units.
Pre/post chained-call hooks do not cover direct compile-time native chaining,
and production fast paths omit those hooks. Revisit the boundary mechanism
before changing generation modes; test a callback with stack arguments and
observable GP-relative memory effects.

## Keep code generation reproducible and buildable

Whole-program automatic generation was expensive on this executable: the local
trial grew beyond 2 GB and was stopped with partial output explicitly marked
incomplete. It is not a validated corpus. Do not treat files existing in a
generation directory as evidence that generation completed.

The working alternative uses the root manual-map generator with Ghidra-defined
instruction runs. The Badman adapter partitions runs into bounded C++ units,
generates import wrappers once, and writes one aggregate registry. Its manifest
records hashes of the ELF, map, adapter, and recompiler. Outputs stay under the
ignored build directory. Generated names include guest addresses to avoid symbol
collisions. Run-map validation rejects misalignment and overlaps.

Bring-up currently uses `/Od /Ob0` on generated units and two compiler workers.
Measure correctness before enabling aggressive optimization or native fast paths.
Manual-map translation is less optimized than the VCS production corpus.

Import stubs must remain dispatcher boundaries even inside one generated unit.
The font probe exposed a manual-map bug: a local JAL jumped to the raw stub's
JR RA instead of its registered HLE wrapper, returning a plausible stale value.
The profile now subtracts every eight-byte import stub before partitioning and
audits local labels/registrations. Generic generation is also being corrected
with synthetic direct/indirect/fallthrough cases. A nonzero library handle alone
does not establish correct execution; verify actual downstream calls/results.

Counts describe different things: Ghidra functions, instruction addresses,
registered runtime entries, and translation units are not interchangeable.

## Compare generated-code optimizations with observed guest work

Test compiler changes in a private copy of a reviewed source snapshot. Record
the exact source differences, regenerate both corpora, and require identical
generated C++ inputs. Keep runtime sources, toolchain, dependencies and unrelated
flags identical. Inspect the generated compiler projects as well as the CMake
text: the effective per-file optimization can override the build configuration.
Retain hashes and modification times for the original package and player cache.

Selected translated kernels can establish correctness by comparing complete CPU
state, source preservation and guarded destination memory between builds. Seed
otherwise untouched floating-point/vector state with observable values. Test a
real callback-driven library separately when its generated code also changes.
These checks do not establish all-game floating-point or gameplay equivalence.

For runtime timing, use the same legitimate front-door route and a private copy
of an earned save. Wait for actual scene readiness; menu globals may already be
initialized during a startup movie. Confirm a guest update counter is advancing
in the intended phase and keep cursor, budget, terrain actions, population and
pause conditions fixed. Measure actual counter deltas against both process CPU
time and monotonic wall time, retaining checkpoint acknowledgement bounds. Run
repeated pairs in alternating order without other project builds or game runs.
A guest update counter is not automatically a presentation FPS counter. Faster
memory kernels and smaller executables can coexist with an unchanged or slower
observed game scene; keep those results separate.

Dependency checks also need the lifecycle context. A movie-only conversion DLL
may unload after video teardown while audio libraries remain active. Verify the
paths and identities required by that phase rather than assuming every library
seen during movie playback must remain loaded forever. Preserve failed harness
assertions and explain fixture corrections instead of relabeling them as game
or compiler failures.

Memory fast paths must preserve initialization timing as well as final bytes.
A write-watch configuration may be captured lazily by the first successful
store. An invalid word store can fail during its old-value read before reaching
the logger; initializing the logger earlier changes later diagnostic behavior.
Use fresh processes to test enabling after construction, changing settings after
a failed operation, and changing them after a successful but unwatched store.
Compare exact errors and watch output alongside complete RAM/VRAM bytes. A
mapped mirrored range may wrap inside the host allocation and therefore have
no contiguous raw pointer; retain the original fallback and boundary errors.

## Bootstrap the guest before replacing its code

- Set GP from module metadata, provide a valid stack, and reserve per-thread TLS
  for K0. Badman's CRT writes through K0 during initialization.
- PSP thread-start arguments are a byte block and byte length; copy the block to
  the new thread's stack. They are not a host `argc/argv` pair.
- Preserve return addresses and all architectural state across scheduling.
  A runtime context token includes thread identity/switch generation. PC equality
  alone cannot prove that an import wrapper is still returning to the same thread.
- A returned module entry does not imply that the game has exited. Ready guest
  threads still need to execute.
- Implement the allocation modes the trace actually requests. Badman's CRT needs
  low aligned allocations with 4 KB alignment; simple bump allocation without
  padding/free-space accounting was insufficient.
- Restore nested interrupt-mask state and defer preemption during masked regions.
- A thread that exits may later be started again under the same UID. Rebuild its
  architectural context and arguments from creation metadata, preserving owned
  stack/TLS allocations rather than treating an exited thread as deleted. Check
  restart priority, GP, argument-pointer and null-argument behavior against
  hardware tests; simply accepting the old state leaves stale return registers.
- Exercise the last runnable worker exiting while other threads await an event
  that only a hardware interrupt can signal. An idle scheduler may still carry
  the exited worker's identity; suppressing all interrupts under that identity
  can strand live waiters. Distinguish a fully ended process from an idle process
  with blocked threads, and preserve real ISR wakeups and thread continuations.

Use executable traces to choose the next HLE operation. Unknown imports should
produce a named diagnostic. For implemented operations, preserve their real
failure behavior; never fabricate a successful handle or completion to move on.

## GE execution and callback details

- Copy callback function/argument values at registration. GU passes a transient
  stack structure that is reused immediately afterwards.
- Execute callbacks as translated guest code on a separate guest stack. Preserve
  the interrupted CPU context and thread identity. For the supported modern SDK,
  argument registers contain token, registered common argument, and continuation PC.
- SIGNAL/FINISH and END form meaningful pairs. A FINISH word by itself is not a
  completed list. Honor callback ordering and retain pending/stalled list state.
- Canonicalize cached/uncached address aliases. Stall-before-fetch behavior is
  essential when GU queues a list at its current write pointer.
- A stalled active list blocks later submissions. Queue-head insertion must not
  bypass that active list. CALL/RET needs bounded, per-list stack state.
- Matrix DATA commands stream many elements; retaining only the last command
  register discards earlier matrix state.
- Raw GE CALL has two hardware slots; a third nested CALL is skipped. Do not
  confuse these with the separate firmware stack for SIGNAL-based calls. Mask
  BASE/address calculations to the hardware's 28-bit address space.
- SIGNAL SYNC suppresses the following FINISH/END pair, including across stalls.
  Count callbacks/lists only after successful delivery, and share a finite pump
  budget across callback-enqueued lists to bound recursive submission.
- State-only lists can execute before a rasterizer exists. Nonempty geometry,
  bounding-box tests, transfers and other unsupported commands must not silently
  turn into successfully rendered/completed work.

The current GE implementation is synchronous and incomplete. Per-list context
save/restore, several SIGNAL behaviors, and drawing/transfer commands still need
work. A FINISH callback executing successfully is not evidence of a visible frame.

Public API reference: <https://pspdev.github.io/pspsdk/pspge_8h.html>.
Measured command behavior: <https://www.ppsspp.org/docs/psp-hardware/gpu/display-lists/>.

## UMD, filesystem and ABI details

- An extracted UMD root can back read-only `disc0:` access. Tie readiness to
  actual local data, rather than returning ready for a missing root.
- Keep host filesystem paths confined to that root; distinguish device paths,
  root-relative paths and program-relative paths. Support EOF/short reads and
  negative results for missing files and rejected writes.
- Async host reads can finish immediately, but the guest still needs a pending
  completion/result contract. Do not overwrite a pending operation or confuse a
  submission return value with its eventual 64-bit result.
- Preserve completion output buffers on errors. PSP async busy/no-request codes
  are `0x80020329`/`0x8002032A`; do not substitute unrelated generic errors.
  Apply media-readiness checks consistently to seek as well as open/read, while
  still allowing completion collection and descriptor cleanup.
- On Windows, reject extra path colons as well as traversal: a confined-looking
  filename can otherwise select an NTFS alternate data stream.
- PSP `sceIoLseek` uses the aligned register pair a2/a3 for its 64-bit offset,
  t0 for `whence`, and v0/v1 for its result. Its fd is in a0. Treating the offset
  as the next unaligned argument pair breaks valid calls.
- Allegrex's guest calling convention uses a0..a3 and t0..t3 for the first eight
  integer arguments. Do not assume the ordinary four-register MIPS O32 convention.
  For example, sceRegGetKeyInfo's fifth output pointer is in t0/gpr8. Test stack
  preservation independently from tests of fifth/sixth register arguments.
- Serialize PSP file metadata explicitly: SceIoStat is 88 bytes with 64-bit
  length at byte8 and three 16-byte PSP datetimes. Native Windows stat packing is
  different. Test a sparse file above 4 GiB and error output preservation.
- Check module paths and return/error branches before deciding a missing module
  is fatal. `sceKernelLoadModule` may request a title-bundled PRX or a firmware
  module; neither successful loading nor the absence of a fallback can be assumed.

References: <https://pspdev.github.io/pspsdk/group__FileIO.html> and
<https://pspdev.github.io/pspsdk/group__UMD.html>.

## Verify and leave an actionable handoff

Retain a boot trace and a small summary of the last verified milestone. Tests
should check observable behavior: allocation/padding reuse, context preservation,
callback ordering, stalls, incomplete-work rejection, EOF, async completion, and
64-bit ABI behavior. Use synthetic fixtures rather than commercial assets for
host unit tests. Run the relevant framework regression suite after integration.

Record the exact executable/build commands, result/exit code, final import or
missing PC, and the next concrete experiment. Distinguish code that was compiled,
code reached by the real game, synthetic-test coverage, and untested assumptions.
Update profile documentation when its current stopping point changes.

For a player-facing release, keep ISO selection, verified preparation, retained
settings and Play in a launcher executable. Run slow extraction/preparation off
the UI thread, pass filenames as process arguments with correct quoting, and
show the child's real outcome. Test Browse, cancellation, paths with spaces,
invalid/mismatched inputs, reuse, and native launch. Never equate a working setup
window or successful glyph probe with a playable game. Bundle/pin required tools
and licenses before describing the result as a portable release.

Enable CTest at the root when either framework or profile tests are enabled.
Calling enable_testing only in a profile subdirectory hides its tests from the
root build when framework tests are disabled; verify this configuration too.

## Repeatable local preparation and packaging

For an own-ISO recompilation launcher, include only the independent compiler,
runtime/profile sources and permitted dependencies in the shipping artifact.
Extraction is not recompilation. The user's first preparation must generate
game C++ from their verified executable and compile it locally; Play uses only
that verified local result. Keep generated code, title-derived libraries and the
compiled game outside the distributable, in a fingerprinted user build cache.
Test from a source-only package with no access to a developer's prebuilt game or
generated translation. Record exactly which compiler prerequisites are required.

Keep analysis tools separate from the player's preparation path. A loader can
establish relocation facts during research; a hash-locked, bounded recipe may
then reproduce the verified module without opening an analysis database. Verify
the original module, each intermediate format and the final prepared ELF. Do not
silently accept a different revision or mark raw extracted data ready when a
required prepared module is missing. Pass its explicit path to the native host.

An extracted-data manifest should cover every artifact required by native boot.
Allow a verified older data folder to gain the new artifact without replacing
unrelated files. A read-only verification command must reject missing artifacts
without reconstructing them. Keep failed preparation in owned staging folders.

Exercise a packaged backend from a directory outside the source checkout, using
its own Python and tools with a minimal PATH. Include spaces, non-ASCII characters
and an emoji in actual ISO, data, font and log paths. On Windows, test both the
native host and external tools: a wide-character launcher does not fix a child
process whose narrow argv uses the system ANSI code page. Preserve upstream
binaries and notices when preparing a separate UTF-8-manifest copy.

Test CMake's configure-time generator output separately from generated file
contents. On Windows, `execute_process(OUTPUT_VARIABLE ...)` can decode UTF-8
Python output using the console code page under an older CMake policy. Badman's
real Korean-path build produced correctly named C++ files but mojibake paths in
the Visual Studio project, causing C1083 missing-source errors. Use explicit
`ENCODING UTF-8` when capturing generated source lists and Python `-X utf8` for
the producing commands. Check actual project paths and complete a native build;
an ASCII-path build or a generator exit code cannot verify this boundary.

Treat the package as immutable during both normal preparation and verification.
Python helper imports can create `__pycache__` even when the main script does not
write files. Compare the complete file inventory and hashes before and after a
real packaged run, including files absent from the distribution manifest.

Package manifests should hash an explicit allowlist of payload files. Never use
an unrestricted recursive copy of the workspace, analysis or build directory.
Record remaining runtime prerequisites and distinguish a personal development
bundle from a redistributable, playable release.

For framebuffer QA, bring the selected game window to the foreground before
interpreting a capture. An occluded-window capture can contain another app's
pixels. Preserve the real unsupported-command stop; displaying a black scanout
buffer while a later draw is incomplete does not establish visible game boot.

## Callback and media integration

Classify each callback by its calling contract. A GE interrupt callback needs
isolated interrupt context and cannot block. A movie ringbuffer file callback
runs on the calling PSP thread and may block or schedule. Preserve its caller
per thread, redirect guest PC to the actual callback with the original stack/GP,
and resume a registered host continuation on return. Test with a callback that
really waits while a second translated task runs; an immediate-return callback
does not establish scheduler safety.

Post-import timer preemption must preserve explicit PC redirects. Capture both
the original import PC and execution-context token. After the handler, process
timers only if the same thread context remains; its continuation is the changed
PC when redirected, otherwise the original caller's return address. Saving RA
unconditionally can skip a callback and enter its return trampoline early.

Media probes should establish independent facts: container/PES parsing, decoded
pixel identity, access-unit timestamps, ring occupancy and cleanup. A video PES
packet is not a video frame. FFmpeg can parse/decode correct pixels while leaving
some parser timestamps unknown; validate the title's timing separately. Exact
pixel agreement in a standalone probe still does not establish guest playback.
Keep codec libraries separate with their exact build hashes and license notices.

Preserve missing MPEG AU timestamps as the real firmware sentinel when the PES
does not supply them. Hardware expected outputs show ordinary AVC AUs with
`FFFFFFFF/FFFFFFFF` PTS/DTS. Inspect the guest consumer before filling them:
Badman's translated video and audio queues explicitly handle `-1` using their
own previous timestamp and interval. A full-file frame-count reconstruction
can accidentally hide this guest behavior and prevents true streaming when the
movie is larger than its ring. Test an incomplete stream with missing timestamps,
not only a small resident logo. Primary observed outputs:
<https://raw.githubusercontent.com/hrydgard/pspautotests/master/tests/video/mpeg/basic.expected>.

For interleaved PSMF, track video and audio consumption separately. Reusing a
shared ring slot after only the video consumer advances can invalidate unread
audio. Preserve a partial ATRAC frame across private PES boundaries, apply that
PES's timestamp at its declared first new frame, and test both consumer orders
and actual wrap callbacks. A registered but absent audio stream in a video-only
file must not pin every packet. Verify with a movie larger than the ring, then
compare every RGBA/PCM byte against an independent decoder invocation. Resident
RIFF music and packet-streamed movie audio need different state and trimming;
do not reset the audio decoder for each frame or apply RIFF loop/priming rules
to the movie API. Standalone decode identity is not proof of guest A/V sync.

For asynchronous PCM, retain immutable samples until real completion or a device
close barrier. Queue callbacks may report completion through bounded atomic
state; they must not touch the guest runtime or memory from the device thread.
Apply per-buffer volume before submission when voice-wide volume changes would
otherwise change older queued audio. Device-link tests and injected transport
tests are distinct from opening a real device or verifying audible playback.

Controller buffer counts are maxima, not a requirement to invent historical
samples. Return only genuine observations and preserve the unused output tail.
Keep failed-operation output rules API-specific: valid unmatched event Poll
publishes current bits, while invalid event parameters preserve output.

For long menu/gameplay investigations, an explicit diagnostic controller can
send short, self-releasing button pulses to an owned headless process while
periodically capturing its real latched guest framebuffer. Give commands a
monotonic sequence number so repeated file reads cannot retrigger a pulse;
replace files atomically and bound pulse duration. Inspect each settled screen
before the next menu selection. Record this as injected controller QA, separate
from testing a player's actual keyboard or launcher window. Preserve each run's
binary hash, input commands, trace, stop reason and captures under a unique tag.

Give each diagnostic child one controller-command owner. Serialize route actions
and neutral status requests with a per-child lock: a second writer can otherwise
replace a held direction or advance the sequence while a route still expects
its own acknowledgement. Read-only frame/trace observation is separate from
publishing a neutral controller command. Keep different child processes on
different control files, and retain any ownership/route-guard failure as evidence.

Short timed startup pulses can occur entirely between real input observations
during cold file/font loading. Do not backfill them as invented controller data.
Inspect the observation timestamps and use the actual displayed prompt as a
readiness condition for a fresh bounded pulse. A known, visually inspected BMP
matched across two distinct frame publications can provide a precise headless
guard. Preserve the missed attempt separately, and never treat a diagnostic
deadline at the initial prompt as proof that the game reached its title.

When a controller Read needs a future sample, store a typed PSP wait with its
output range and return continuation, then run other ready PSP workers. Include
the actual input sampling deadline in the idle scheduler; never sleep the host
while another guest worker can run or manufacture repeated samples to satisfy
the request. Share the input/scheduler clock. A ready buffered observation needs
a stable immediate deadline, not a newly sampled current time that keeps moving
ahead of the scheduler's earlier observation. Sequence clock queries explicitly;
C++ comparison operands have no guaranteed evaluation order. Test a clock that
advances on every query and verify already-ready data stays eligible at an
earlier clock observation, as well as testing actual HLE context resumption.

A short timed input pulse can legitimately fall entirely between two guest
controller observations during startup. Record observed input times before
diagnosing a boot regression. The packaged Badman test observed time 0 and then
8217 ms, missing a pulse scheduled at 5000–5200 ms. A separate retry waited for
two real frames showing the initial prompt, sent one bounded live controller
pulse, and then released it. That reached the movie and title without a runtime
change. Preserve the missed-input attempt; do not synthesize historical samples
or report an unchanged prompt as a successful boot.
High-level menu globals can also be initialized before an opening movie finishes.
For scene-driven QA, require the actual rendered ready screen or a separately
verified readiness signal before sending Start; a mode value alone may select
the wrong point in the startup sequence.

An ignored direct-function integration harness can exercise a late guest media
player before completing a campaign. First perform ordinary bootstrap, retain a
genuine PSP thread/TLS/GP, allocate an owned call stack, and reproduce verified
caller prelude/arguments through the ordinary dispatcher and real callbacks.
Freeze matching local objects/libraries/headers and hash the executable. Label
this as a direct probe; it does not establish the natural scene route. Count
actual decoded units, displayed copies, device PCM completions, worker exits
and resource teardown separately. A player function that returns0 after error
paths needs stronger success conditions than its return value alone.

On Windows, an atomic filename replacement needs readers that permit delete
sharing. A CRT input stream can fail to open while the publisher renames the
file. A single `CreateFileW` handle with read/write/delete sharing and a bounded
read observes one complete old or new version when the writer uses atomic
replacement. Retain strict size, line, NUL and sequence validation, and bound any
retry for transient sharing errors. Test rapid real replacements and malformed
input separately; a diagnostic transport failure is not a guest runtime defect.

Verify input effects in the current game phase. An introduction may initialize
a cursor that gameplay later resets, and placement may use a different cursor
from digging. A rejected dig on a diagonal or unopened tile can be normal game
behavior. Observe actual phase, coordinates, resource spending and the resulting
frame before diagnosing the input implementation. Fixed tutorial terrain also
does not establish a route through a randomly generated Story map.

For read-only watches of dynamic terrain, follow both the coordinate getter and
the allocation constructor. A row-pointer table can reference separately
allocated rows; width times height does not prove one contiguous cell slab.
Observe the actual table and row pointers before forming a cell address, then
guard its coordinates and dimensions in the run that uses it. Repeated boots
may produce stable guest allocations, but that is a hypothesis to check rather
than a reason to reuse an unobserved heap address.

Trace on-disc preference fields through their loader, runtime getters and actual
condition dispatcher before naming a goal or timer. Source-table and runtime
offsets can differ; an adjacent music ID can otherwise be mistaken for a victory
condition. Follow counter writers as well as readers to distinguish cumulative
births from a living population. Use visible HUD values as observations, not as
proof of the underlying raw value, unit, objective or final outcome. Preserve
and explicitly correct mistaken mappings instead of silently replacing their
evidence. Title-specific mappings belong in that profile's notes.

For PSP utility text dialogs, distinguish the common GetStatus lifecycle from
the OSK parameter-local state enum; their visible/quit/finished numeric values
differ. Keep unknown field words separate from language/type metadata. Snapshot
bounded UTF16 input at initialization, expose the actual field limit and output
capacity separately, and accept only generation-matched user responses. Commit
terminated text and result only during the guest's Update after actual action;
headless execution must keep waiting without a supplied response. Test held
opening buttons, stale responses, malformed surrogate pairs, output aliases and
metadata changes before writing. Label conservative NUL/cancel/Unicode edge
policies separately when the SDK and hardware tests do not establish them.

Inspect every result channel the guest consumes before choosing a utility's
cancellation behavior. A per-field cancelled result can coexist with a separate
common dialog result; reporting common success can make the guest copy an
untouched output buffer despite the cancelled field. Trace the normal completion
consumer and test both result words, preserved output and the actual post-dialog
game state. A correct status lifecycle and passing buffer tests alone do not
prove that cancellation preserves the player's text.

Repeat new runtime features with the exact executable locally built from the
source-only candidate. A byte-identical engineering copy and a separate private
game-data/save fixture keep the distribution and its prepared cache immutable.
Snapshot both before and after. For persistence, perform the real commit and
Save prompts, then use a fresh process with the diagnostic text provider disabled
to Load and inspect the actual game result. A same-process edited field or a
success return alone does not prove that the value was persisted and restored.

## Recover setup after forced termination

Treat closing a launcher during preparation as a real process-termination case.
A PID written into a lock file does not prove that its owner is still active,
and a Python `finally` block will not run after a forced process stop. Use an
operating-system-held lock whose ownership ends when the handle closes or the
process dies. Badman's bootstrap locks an existing immutable cache marker, so
verification can coordinate with preparation without creating a lock file or
changing any metadata. A legacy PID file is preserved and, when present, also
locked; its mere existence does not prevent recovery.

On Windows, exclusive byte-range locks can also deny reads through another
handle. Locking beyond the end of a file is valid, but the requested read range
still matters. In a real test, a lock at byte 4096 prevented Python's buffered
file hashing even though the small marker ended before that byte. Moving the
coordination byte to 16 MiB kept the bounded marker and ordinary hashing buffers
readable without growing or writing the file. Test the actual concurrent
ownership check and checksum reader, not just the locking wrapper. Windows
documents the access, beyond-EOF and process-death behavior in
[LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex).

Preserve interrupted extraction work instead of deleting everything that looks
temporary. The bootstrap recognizes only its exact temporary-directory name
shapes, scans a bounded tree, rejects links and nonregular entries, and leaves
those directories unchanged. Their contents are ignored, never adopted as
trusted dependencies or reused for publication. A retry creates a fresh stage
and verifies the completed result normally. Unknown directories remain a reason
to preserve the cache and stop rather than guess that their contents are safe
to remove. Read-only verification must not clean stages, rewrite receipts or
change file hashes, sizes, inventories or modification times.

Download into a unique sibling temporary file, flush it, verify its complete
identity, and publish it atomically without replacing an existing destination.
Copying a verified temporary file into the final name can leave a truncated
final archive if the process is killed during that copy. Windows `MoveFileW`
provides a no-replace publication step; a POSIX hard-link publication can serve
the same purpose when followed by removal of the owned temporary name. Preserve
an existing invalid or racing destination instead of silently deleting it.

Exercise recovery with a real owned child process stopped while holding the
lock, while an extraction stage exists, and immediately before publication.
Then retry normally and verify the cache read-only. Retain the old files and
compare their hashes and modification times, including legacy coordination
files and interrupted stages. Synthetic fault injection and a real pinned
dependency extraction are separate, useful checks; neither requires a game
binary or a desktop window.

## Verify renderer caches against uncached behavior

Use the original implementation as an executable oracle before replacing a
per-texel calculation with cached results. Compare complete command transcripts,
returned colors and exact errors; expected values derived from the new cache
cannot detect a shared decoding mistake. Exhaustive packed 16-bit color inputs
are small enough to test, alongside partial loads, format reinterpretation,
failed-load rollback and source-memory mutation after a latched load. Query
unloaded entries too: eagerly decoding a palette must not move a formerly lazy
error to an unrelated valid lookup or load.

An owned cache changes its containing object's layout. Rebuild every transitive
consumer against the matching header, including main and tests, or compile
separate opaque adapters. Reusing a large generated-code object is safe only
after its actual compiler dependency evidence excludes the changed types.
Same-header core-only substitutions and changed-layout Host substitutions need
different link procedures. Keep semantic equivalence, ABI integration and
performance as separate gates; extra work during palette loads can outweigh
lookup savings in load-heavy workloads.

## Correlate controller edges with changing game data

A screenshot can prove that a resource reached a threshold while failing to
prove its value when a later controller action was accepted. Feeding, depletion
and animation updates can change the resource between those observations.
Record a read-only state sample on the same accepted controller sequence as
the action, then correlate the following resource cost, objective counter and
normal outcome. A visible population count alone may differ from the actual
objective. Validate an earned result again through normal Save and a fresh Load.

For heap-owned grids, audit allocation and indexing before deriving watches.
Rows may be separately allocated and may move even within one process after a
stage restarts. An engineering diagnostic can follow a small bounded pointer
chain afresh for each sequence, checking every mapped aligned read and offset
addition. Uninitialized or destroyed objects should produce explicit unresolved
records, never a fabricated zero or a guest write. Test parsing, alias/boundary
behavior, nonmutation and old diagnostic compatibility before actual use. Keep
title addresses in profile configuration/notes and identify an instrumented
executable separately from the exact player-package binary.

## Exercise both sides of Windows diagnostic file replacement

Save the actual Win32 error before cleanup. In owned tests on this machine,
replacing a target held by a share-delete reader returned ACCESS_DENIED5, then
the same complete pending file published after the reader closed. A held source
returned SHARING_VIOLATION32. Read-only files and directory targets also returned5,
so that code alone does not establish a transient condition. Keep real errors
visible and do not delete an old target or clear its attributes to force progress.
The API requires checking the error after a failed
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).

Use a short, nominal retry deadline for transient-capable failures, retaining
the same fully written pending file. Only an actual successful replacement
establishes publication; operating-system I/O itself can exceed a retry deadline.
Test reader release, persistent holds, pending-delete state, read-only/directory
targets and missing parents with real APIs, checking complete old/new bytes,
attributes and failure preservation. A counter-only wrapper around the real API
can observe attempts without duplicating or mocking the replacement algorithm.

Apply the same scrutiny to the parent controller writer. Native capture output
and parent command publication can race different readers. Retry one candidate
sequence and pending command, advance the parent sequence only after actual
publication, and still require the Native's matching input/watch acknowledgement.
Never describe a published command as accepted merely because its rename worked.
Preserve failed runs and restart a comparison with one consistent harness policy;
do not silently mix earlier completed windows into a revised measurement batch.

## Preserve palette semantics when caching decoded colors

A decoded palette cache must preserve the original raw latch, partial-load tails,
load error priority, and delayed lookup-format validation. Cache only entries
whose bytes and format are known; use the original raw path after a format change
outside that prefix. Stage source reads before committing any state. Keep cache
storage owned by each palette so copying an instance cannot share mutable state.

Test the same literal fixtures against the original and candidate implementations,
including actual format-at-load changes, switchbacks, short loads, failed loads,
last physical entries and copying. A change to a nested class layout requires
rebuilding every containing object and executable against matching headers;
relinking a new library with an old main object is insufficient.

Measure load-heavy and lookup-heavy workloads separately, then test a real scene.
An active-format cache can improve repeated lookups while adding load overhead.
Compare combined optimizations directly: their individual gains need not add.
Report guest updates, CPU cost and the tested scene separately from display FPS
or campaign completeness, and preserve unchanged input and executable identities.

## Give diagnostic fields phase-specific meaning

An address may retain a previous mode's result during the next mode's setup.
In a normal Training exit followed by Story construction, the old outcome value
remained present while the new construction phase, map and timer were valid.
A diagnostic assertion that required a fresh battle result during construction
incorrectly rejected the route. Guard fields by their verified lifecycle and
phase; do not reset guest state to satisfy a test. Likewise, a mapped list
sentinel is not a live actor, and a battle timer need not advance during setup.

For timed gameplay QA, keep dialogue completion, automatic stage transitions and
an ordinary Pause input in one bounded controller operation. Returning to model
planning at a stage card can let the next phase's entire timer expire. Observe
the phase-specific counter advancing before Pause, then verify it stays fixed
and inspect the actual menu. Do not append another Start blindly after an entry
helper that already paused. Controller latency is not evidence of a game timing
defect; preserve missed attempts separately from runtime errors.

A guest framebuffer capture may exclude independently rendered host utility
dialogs. Before diagnosing a persistent black capture, check utility status and
dialog traces, plus the global scene state. A per-mode phase can remain unchanged
through a save chooser and the title screen. Use ordinary Cancel/Confirm input
and verify the utility's real shutdown sequence; a black guest buffer alone does
not prove that the renderer or game thread stalled.

## Separate persistent achievements from an in-progress session

A successful Save and fresh Load can preserve tutorial checkmarks, statistics
and unlocks while starting a new campaign at its first stage. Verify the actual
post-load menu and stage before calling this resumable campaign progress. Keep
the earned save in an isolated fixture, and copy it for subsequent experiments.
Authorize expected changes to that fixture in the harness before a normal Save;
a stale assertion that forbids every save change is a harness failure, not a
game failure. Preserve both the original result and the corrected interpretation.

For a longer campaign, use an explicit external QA deadline with a separate
opt-in limit instead of changing guest time or silently extending every test.
Keep stage transitions and ordinary Pause input in one bounded operation so
model deliberation does not consume the next construction timer. Hand controller
ownership to the next operation only after the prior receipt is terminal and
its exclusive command lock is released.

Multi-enemy outcomes need the game's complete victory predicate. A first actor's
HP is useful only under a live-actor guard and does not prove that the whole
group is dead. Correlate configured and spawned counts with the game's outcome,
clear screen and ordinary results transition. Prior-stage outcome flags may
survive into construction; ignore them there rather than labeling a new victory.

## Follow asset rejection through the caller

A shipped asset can disagree with the active stage's dimensions while the game
still works as written. Check initialization order and the rejection path before
changing either the asset or runtime. In one tutorial, the guest generated soil
first, rejected a differently sized fixed map before copying it, and retained
the generated terrain. Live dimensions, the entry coordinate and visible terrain
agreed with that fallback. Treat the rejected template's coordinates as unusable
for route planning; do not silently resize it or bypass the guest's check.


### Campaign QA after the first playable stage

A game may retain outcome and victory-latch words across results and into the next construction phase. Interpret those fields together with the current phase and stage. Require a fresh battle reset, the actual terminal flags and a rendered clear/results frame before claiming the next victory. Preserve black fade captures as transitions rather than labelling them as rendered clear screens.

Controller acceptance is separate from consumer readiness. Menu rank-up animations can ignore otherwise accepted Cross edges. Verify an actual resource decrement and the displayed rank/progress before counting a purchase; retain the normal confirmation and commit path. Likewise, later stages can retain a placement cursor instead of repeating the first stage's reset coordinate. A bounded ordinary D-pad response can establish interactive readiness without patching state or assuming a cached coordinate.

Functional campaign tests should use a long, explicit external watchdog and disposable saves while leaving game clocks unchanged. Keep per-stage scripts bounded, batch normal dialogue and the immediate construction Pause together, and distinguish deliberate process cleanup from a game outcome. A successful Save/Load cycle does not by itself establish mid-campaign resume: start the relevant game mode after a fresh Load and observe the actual stage and dungeon.

### Diagnose presentation separately from simulation speed

A software/GDI renderer can expose a black intermediate image if it clears the
visible DC before drawing the framebuffer. Compose bars, frame and overlays on a
retained offscreen surface and publish once. Test destination clipping, native
child controls, resize and GDI resource lifetime with actual offscreen rendering.
That establishes composition correctness, not physical-monitor flicker removal.

Hidden windows may receive no paint calls. Keep that result explicit rather than
forcing paint with an empty destination clip and comparing it to real drawing.
Measure guest-update throughput separately from scanout submissions and window
repaints. An FPS overlay should count accepted immediate submissions or pending
submissions when latched, using actual elapsed time; repainting alone adds none.
Document whether the displayed rate counts submissions or unique images.

Profile broad renderer phases before adding fast paths. Inclusive wall timers
overlap and cannot be summed as CPU percentages. Compare balanced scene repeats
with stable inputs and no concurrent compiler workload. For contiguous memory
reads, retain complete-footprint validation and wrap/mirror fallbacks, and test
live writes after construction rather than caching texel values. Mathematical
bit-level fast paths need boundary and fallback oracles as well as rendering
tests; identical return bits do not automatically imply identical floating-point
exception flags.

### Introduce GPU rendering with an observable memory contract

A synchronous software renderer writes guest VRAM that guest callbacks, DMA and
CPU reads can immediately observe. A GPU backend must preserve that visibility.
Batch only across boundaries where the guest cannot access the affected memory;
complete readback before list yields, callbacks, framebuffer reads or software
fallback. Snapshot texture and palette inputs for queued draws so a later palette
load cannot retroactively change an earlier draw. Test stalled lists and callback
reads alongside ordinary frames.

An integer compute pixel pipeline can retain existing PSP sampling, blend and
dither arithmetic while running the expensive pixel work on a GPU. Keeping exact
CPU-generated fixed UV tables avoids differences in hardware sampler rounding.
It does not accelerate geometry setup by itself. Publish guest writes only after
a successful complete GPU transaction, and report device failures explicitly.
Write-watch diagnostics may require the scalar path to retain individual stores.

Validate each hardware backend against full RAM and VRAM images, exact result
counters and malformed-submission errors, then measure an actual matched scene.
A successful buffer-add compute test establishes device plumbing only. A GPU
framebuffer upload establishes presentation only. Describe which stages actually
run on the GPU and retain a scalar renderer as the reference.

Ship independently written shader source/embedded SPIR-V and permitted public
headers with their upstream licenses and pins. Developer shader compilers can
stay in ignored tool directories; a player's local own-ISO build need not install
a full graphics SDK when the independent shader artifacts are already supplied.

When copying a private candidate into a source tree, check effective compilation
as well as source hashes. Windows Copy-Item can preserve a source modification
time older than the current object, so an incremental build may retain old code.
For known copied inputs, refresh their timestamp or force their target to rebuild,
then verify the expected compiler invocation and a test marker unique to the new
source. Preserve the stale run rather than relabeling it as a new-source pass.


### Inspect owned runtime memory without changing the guest

A separate engineering observer can avoid recompiling diagnostic watches into a
Native executable. Restrict it to a live child created and retained by the test
controller; bind PID, process creation time, exact executable path and hash. Open
only query/read permissions. Reject stale identity and ambiguous memory matches.
Never attach to a player's independently launched process.

For contiguous guest RAM, use bounded private-region queries and two unique code
signatures derived from the exact verified input ELF. Allow only a small, explicit
alignment search around the expected code offset; do not scan arbitrary process
contents or assume allocation base equals a vector's data pointer. Keep addresses,
signature bytes and captures in ignored profile artifacts. A synthetic owned child
can test discovery, ambiguity, identity rejection and complete-memory nonmutation
before the observer is used with a game. This technique has passed synthetic-child
tests; actual game attachment remains a separate functional gate.

Before reading gameplay structures, pause through normal controller input and
verify the correct phase timer and state. Bound total reads, compare exact gates
before and after the snapshot, and reject a changing state. This provides a
consistency check rather than an atomic suspension. Retain heap/list/cell guards
and read pointers afresh; successful signature discovery does not validate a
game-specific structure layout. Keep this observer out of performance windows.

### Preserve lazy diagnostics when replacing per-pixel stores

A raw span optimization must preserve when diagnostic configuration is first
consumed. A constructor-sampled watch flag can differ from a lazy global watcher
initialized by the first successful ordinary store. Validate a nonempty mapped
span before consulting the lazy watcher, and defer that consultation until the
first actual write. Rejected, zero-sized, fully clipped and fully masked work
must not latch the configuration. Fresh processes with environment changes around
those boundaries distinguish these cases; an in-process reset cannot test a
function-static latch faithfully.

Predecode all vertex and target state before raw writes when the original draw
promises all-or-nothing validation. Separate color/depth loops are unsafe if a
hardware depth address permutation can alias color storage. Retain per-pixel
write order for that case, and use bulk rows only when state and disjointness
justify them. Compare complete RAM/VRAM and exact errors, not only visible pixels.

For synthetic timed kernels, changing inputs and work counters supplement a real
checksum. Verify the actual final timed memory against an independent reference;
resetting the memory before taking a checksum only verifies the reset workload.
Keep any common canonical image as an additional check outside the interval and
label kernel speedups separately from actual-game throughput.

### Resolve exit imports by NID, caller and native ownership

Older SDK headers can omit a later firmware variant or give a related name to
a different NID. Find an exact primary declaration, pin its version and retain
the source license separately from the implementation. An independently
maintained binding is evidence for its interface, not a hardware test. Correlate
its argument order with the game's actual instructions before naming raw values.

Distinguish a main application's lifetime from an external library's lifetime.
A native port can implement a verified main exit through an explicit runtime
stop followed by ordinary host stack unwinding. It must prevent guest return
into the retired code and preserve the requested failure status. That does not
establish a generic PRX self-unload implementation or justify silently closing
other live modules. Check caller ownership, thread/interrupt context and the
supported arguments; reject unsupported contracts explicitly.

Test real owned resource destruction separately from returning an HLE value.
Retained decoder objects and reserved audio voices should remain live until the
documented teardown point, then close exactly once. A generated-style import and
native chain with an unreachable continuation can verify the nonreturn contract.
Component tests do not replace the game's ordinary cleanup and exit route.


### Measure native wait overshoot before changing a guest clock

A correct guest deadline can still run slowly when the host sleep primitive
overshoots short waits. Keep the guest monotonic epoch, deadlines and injected
clock tests unchanged, then measure requested duration, actual wake time, CPU
cost and early returns separately. A per-waiter Windows high-resolution
waitable timer avoided the roughly 15ms short-sleep overshoot observed on this
machine. Round relative 100ns deadlines upward, recheck the monotonic clock
after waking and own the handle with RAII. Serialize copies sharing the handle.
This needs Windows 10 1803 or newer; failures must be explicit.

A spin tail can lower latency in an isolated wait test while consuming a core
for repeated short waits. The measured game pilots gave only a small unbalanced
benefit, so the production default retains blocking waits. Never convert a
wait-kernel latency improvement into a claim about sustained gameplay speed.

### Keep native screenshot compatibility explicit

Recover the actual request size, fields, caller update argument and status
polling before adding a utility. A request's numeric image-format value does
not prove the firmware's container or directory convention. An independently
implemented native image format can support the title while documenting that
PSP metadata interoperability is unverified. A screenshot succeeds only after
real encoding, complete file writing and exclusive publication. Preserve existing
files; expose encoding or storage failures through the guest result.

Decode the output with a separate image decoder and verify dimensions, channels,
orientation, source nonmutation and output bounds. Test utility exclusion in
both directions through the actual import bindings. These checks do not replace
the ordinary in-game input trigger and a real game capture.

### Bind filesystem mutations to retained directory objects

A canonical path checked once can change before a later filesystem mutation.
On Windows, retain ancestors without delete sharing and open each child relative
to the checked parent handle with reparse rejection. A deny-write directory handle
can also block the internal target open needed for rename, so verify real API
behavior rather than assuming share flags compose. In the tested native rename
path, a pinned target-directory handle plus a simple destination name preserved
confinement even when the empty directory was converted into a junction immediately
before rename; the mutation failed and preserved the source/outside files.

Check the atomic no-overwrite operation with a destination created after validation,
not just a preexisting file. Validate both guest strings before host filesystem
access, retain exclusive source ownership through completion, and test real file
identity/content plus handle counts. A scoped native policy can reject unsupported
names, directories, linked files and protected save trees explicitly. Record which
error mappings are independent compatibility choices and which are measured PSP
behavior. System native APIs must fail explicitly if unavailable; do not silently
fall back to a weaker absolute-path implementation.


### Separate tool success from a failed verification controller

A fresh compile can finish successfully before a later engineering assertion
fails. Preserve the compiler exit/log, receipt, binary and original failed
controller. If the defect is in the controller (for example a source-filename
parser that omitted digits and missed a Direct3D11 source), fix that parser in
a separate verification-only continuation. Revalidate the actual compiler
commands, outputs and read-only cache behavior without rebuilding or copying
old game outputs. Keep the failed controller status explicit in the new proof.
This avoids both wasting a fresh own-ISO build and relabeling a failed run.

Follow predicate helpers through to their returned representation before using
them in a gameplay planner. A terrain query may mean "open" rather than "solid";
its function name or a caller's negation is insufficient. Retain a corrected
hypothesis separately from live evidence and require the normal game outcome.

### Follow predicate getters before planning gameplay from raw flags

A caller comparing a returned byte with1 does not establish that an underlying
flag is set. Follow the getter: a predicate can return true when a stored word is
zero. In one eight-neighbor birth branch, this changed the interpretation from
all solid to all open. Pin the exact getter and offset table, correct the earlier
hypothesis explicitly, and test both sides before acting on a live map.

Functional controller scripts should stop by observed guest coordinates and
resource changes when renderer throughput changes. Replaying fixed host-duration
holds from a slower build can overshoot targets or construction deadlines. Keep
historical winning routes as evidence, distinct from an executable plan for a new
actual map. A persistent owning wrapper can retain the process handle for a
read-only observer while exposing a serialized ordinary-input queue; do not
replace that ownership with an attach-by-PID shortcut between command invocations.

### Retry sharing failures on readers as well as publishers

On Windows, an atomically published JSON request can briefly refuse a reader.
Python's CRT open may expose PermissionError with errno13 and no winerror field;
checking only WinError5/32 misses that case. An actual owned fixture reproduced
this by holding a Unicode request file exclusively, then releasing it. A bounded
same-path reader obtained the identical id66 payload after16 attempts. Persistent
locks remained failures and did not change or advance the request.

Bound JSON size and retry only the recognized access/sharing failures; malformed
JSON and a missing request are separate protocol errors. Keep the exact failed
run and its controller sources, then fix a new controller copy. Validate response
readers too. A diagnostic controller terminating its owned child after an IO error
must be recorded as a harness failure, not evidence of a game/runtime crash.

Performance workloads need active evidence. A neutral scene rate cannot explain
costs from digging, debris, camera motion or creatures. Retain actual input and
budget changes, measure idle/activity/post-idle separately, and scope short
per-action intervals as quantized update measurements. Diagnostic frame writing
and per-action captures add IO absent from ordinary Play; use the same policy in
comparisons and do not equate hidden-process rates with window/fullscreen FPS.

Record controller duty cycle before comparing active workloads. A guarded dig
executor may spend most of its phase moving, waiting for ACKs, reading guards,
and capturing frames. Twenty-four180ms Square commands separated by roughly one
second do not establish continuous digging performance. Report commanded holds,
actual ACK spacing, and observation-window duration separately. Short windows
containing8–11 guest updates are quantized; aggregate them and do not label them
instantaneous FPS. Phase-level cumulative renderer counters cannot be attributed
to individual Square holds unless the native process recorded those boundaries.

A faster controller design still needs measured cadence. Removing per-action
captures and using short pulses can leave command ACK, neutral publication and
guard reads as substantial gaps. Retain the exact accepted command timestamps,
check the spacing target against every interval, and preserve a missed workload
target even when all game/resource guards pass. Natural creature birth counters
prove birth events; use actual frames or an independently verified live list
reader before claiming a particular number of surviving creatures.

For an asynchronous Pause, observe its actual flag before taking the first
stable-timer sample. Bracket the timer read with the expected flag, then still
require a later equal sample and ordinary Resume advancement. A slow host can
handle a valid Start after a fixed short delay has elapsed; that delay alone
does not establish Pause. Do not replay Start when a flag wait fails.

Validate diagnostic captures visually and against nonempty pixel data. In the
private Badman composition trial, sending WM_PRINTCLIENT with a DIB DC created
in another process returned successfully but produced all-black pixels and a
native composition callback error. The API return was insufficient proof. Keep
the artifact invalid, preserve the induced failure separately from game behavior,
and prefer capture using the rendering process's own verified DIB. Graceful
shutdown is needed for final counters; forced termination can suppress them.

A neutral headless workload can hide the cost of publishing the same guest frame
repeatedly through a window renderer. Test an actual owned composition sink at
the requested client dimensions, retain its composed pixels, and distinguish
hidden GDI work from physical DWM/fullscreen behavior. Keep final diagnostic
capture outside measured intervals. Whole-process composition totals include
boot and paused time; they cannot supply per-phase costs without phase markers.
Also match the player's natural creature population: a few births and sparse
zero-nutrient digging can miss CPU simulation costs present with many creatures.

Treat generated-code compiler settings as a performance variable. Freeze the
runtime, rendering sources, libraries and input protocol while changing only the
main translation objects. Check complete CPU/FPU state, host floating-point
environment, errno and memory on representative semantic probes before a game
trial, then retain ordinary boot, input, resource and capture evidence. Such
probes support the tested instruction slices; they do not prove whole-game
equivalence. Natural populations may diverge across timing trials even with the
same harvest inputs, so retain both birth events and final population evidence.

### Preserve floating-point status when replacing arithmetic helpers

Equivalent integer results do not prove a floating-point optimization preserves
behavior. A bounded replacement for a texture-coordinate `fmod` produced identical
fixed coordinates, pixels and errno, but lost the existing FE_INEXACT flag in
262,116 of 658,560 comparison records on MSVC. The experiment was rejected.
Keep the failed comparison and compare exception flags under every rounding mode,
including preexisting flags, signaling NaNs, subnormals and finite extremes.

An integer-only change can have a narrower proof. For validated logical dimensions
that are powers of two, converting a signed coordinate to an unsigned word and
masking by dimension-minus-one matches normalized signed remainder even for
negative coordinates and INT_MIN. Prove the dimension invariant from constructors
and every call site; address pitch need not be a power of two. Retain all floating
operations and validate complete sample records before measuring performance.

### Measure redundant presentation without suppressing framebuffer updates

A game may submit thirty frames while the host services sixty display edges.
Repeated GDI composition at the client resolution can consume useful CPU time.
Compare decoded pixels, dimensions and the exact formatted FPS label before
requesting another paint. Continue validating/decoding live guest memory and
updating the FPS counter on every call. Skipping every unsubmitted edge would
freeze paths that overwrite the current framebuffer, such as DMA movie playback.
Preserve resize, utility and external paint paths, and verify their full output.

Hidden windows do not necessarily run WM_PAINT after UpdateWindow. A performance
harness must execute the real compositor into an owned DIB via WM_PRINTCLIENT
when a redraw was requested. Record forced compositions separately; suppressing
work in a hidden window alone is not evidence of a rendering speedup. Such a
test measures GDI composition, not physical DWM presentation or fullscreen FPS.


### Separate original simulation from higher-refresh scene presentation

Audit callback registration, priority ordering and actual state writes before
calling a routine a render function. A callback can build a draw matrix from an
old position and then advance its interpolation, input cursor, lifetime or free
list. Replaying it for extra frames speeds state even when no obvious physics
function is called. Preserve original callbacks and vblank timing; render owned
scene snapshots on a separate presentation schedule. A higher present count from
repeating bitmaps is not evidence of higher-refresh motion.

Recover stable draw identity through the game's own sprite handle/pool slot and
an allocation generation. Tag the exact guest GU command address when emitted,
then consume that side tag at GE execution. Include command-buffer epochs and
CALL-parent ownership because cached command lists can be reused by different
objects. Untagged draws remain discrete. Scope tracking must survive genuine
guest yields; a translated native function returning to its scheduler does not
necessarily mean the guest callback returned.

Capture actual emitted transforms and copy all geometry/material/texture bytes
before handing scene data to a presentation thread. Post-callback object state
may describe the next local visual step while current GE matrices still contain
the old step. That observation alone does not qualify forward prediction: audit
immediate setters, offsets, scale/cel changes, removal/reuse and next-tick camera
writes. Conservative interpolation uses actual endpoint snapshots and an
explicit source-frame delay. Keep original guest framebuffer outputs separate
from interpolated host targets, and count gameplay updates, source scenes, real
interpolated motion frames and duplicate presents separately.

Capture a complete validated draw transaction before publishing its records.
For CPU triangle strips, preserve each triangle's ordered, unblended shaded
sources and a separate coverage mask. Flattening overlapping triangles loses
repeated blend/dither operations; alpha zero does not mean absent coverage.
Keep the original vertex-pair index before culling as the sprite subprimitive
identity. A compacted visible-pair index can silently attach motion to another
quad. Keep CALL namespaces and subprimitive indices in separate identity fields
instead of packing away significant ordinal bits.

An asynchronous frontend must own HWND creation, message pumping, native child
controls and GPU presenter lifetime on one UI thread. Transfer input, OSK results,
errors and close events back to the simulation thread; invoke its Runtime close
callback there. Send immutable owned pixels/scenes in the other direction.
Use a bounded latest-frame slot so a blocked presenter cannot queue unbounded
game frames. Retain identical-frame suppression when changing from GDI to GPU:
an unsubmitted service call must not become another expensive swapchain present.

Test compatibility mode after introducing a scene mailbox. Merely hiding the
higher-refresh FPS label does not prevent the render branch from using that
scene. Verify that original-rate mode dispatches no interpolated scene work and
continues to present the original scanout. Native PSP sprite coverage can have
biased subpixel edges that differ from a continuous high-resolution rectangle;
use an explicit native-coverage oracle for capture completeness and a separate
physical-resolution oracle for the enhanced renderer.

### Apply boot options through entry hooks and genuine input packets

A player option such as "skip the logo movie" or "answer the boot load prompt"
can usually be expressed through behavior the game already implements, instead
of writing guest state or patching generated C++. Badman's logo player takes a
skip-button mask argument (0 = unskippable) and its boot prompt waits for an
ordinary Cross/Circle. The profile observes guest function entries through the
runtime's existing pre-dispatch and pre/post chained-call hooks, which every
manual-map call and return passes through. At the exact original call (callee
PC, return address, argument values and movie path) it sets the mask argument
before the callee runs. A separate filter ORs a button into the genuine packets
returned to one controller call site, identified by the caller's return PC and
thread. Press only on alternate reads so the game sees a release edge, bound the
number of presses, and stop once the game's own state advances. The game then
runs its real cancel, cleanup and load paths.

Verify that every call site is reached through a hook path before relying on
this: same-unit direct gotos and compile-time direct-unit chains skip runtime
hooks, and production fast-path builds compile the chained-call hooks out.
Text-patching generated units works too, but it couples the profile to emitted
label layout and has to be re-validated after every generator change.

Prove the effect in the real game with neutral input only: the unchanged build
stays on its prompt, the option build reaches the title, and a read-only watch
of payload-backed words shows a loaded save actually reached game state. A
successful utility result alone is not that proof.

### One automatic save profile instead of a slot chooser

A list-load/list-save utility can serve one implicit profile without inventing
data. Load selects the game's remembered slot, then the first valid saved slot;
an empty store returns the real no-data result without a dialog. Save writes the
game's selected slot, and an overwrite keeps the previous receipt as recovery
history. Keep damaged records, non-overwriting requests, other errors and Delete
interactive: those are real outcomes the player needs to see. Create the
visible Saves folder at preparation by copying old storage into a staged folder
and publishing it without replacement; originals stay untouched.

### Compile generated C++ on the Android device itself

A local-recompile Android port needs a compiler that runs on the phone, not
NDK cross-compilation on a desktop. On Android 10+ an app cannot execute files
from its writable home directory; files in the app's read-only
`nativeLibraryDir` can be executed. Package Android-host Clang/LLD (and their
shared libraries) as `lib*.so` entries with `extractNativeLibs=true`, unpack the
sysroot/resource headers into private storage as data, compile and link a PIC
`.so` there, and load it with `android_dlopen_ext(ANDROID_DLEXT_USE_LIBRARY_FD)`
on a read-only descriptor. Link with `-z max-page-size=16384`.

Verified on a physical arm64 phone (Android 16, API 36, `untrusted_app`):
writable-file exec was denied (error 13); packaged Clang 21.1.8/LLD compiled
and linked a probe for three targets and the arm64 result loaded and returned
its expected value. Real generated Badman units (about 20,000 lines each)
compiled at `-O2` in 6–7 seconds with about 340 MB peak memory per process.
The same probe crashed inside an Android 25 ARM emulator on this x86 host, so
emulator failures are not evidence against real hardware.

The generic recompiler also cross-compiles unchanged for arm64 Android and
produced byte-identical generated C++ on the phone, except that the Windows
build writes CRLF line endings. Normalize or compare without them before
hashing outputs from different hosts.

Run the portable host test suites on the device before writing platform code.
Cross-compiled test executables run from `/data/local/tmp` under `adb shell`;
set `TMPDIR`, because `std::filesystem::temp_directory_path()` otherwise falls
back to a `/tmp` that Android does not have. This found two real portability
gaps: NDK libc++ has no `std::chrono::clock_cast` (use `file_clock::to_sys` /
`from_sys` and floor to `system_clock::duration`), and Android refuses hard
links, so a `link()`-based no-replace publication fails. Linux
`renameat2(RENAME_NOREPLACE)` provides the same exclusive publication there.

Match the desktop compiler's floating-point contract before comparing frames.
ARM64 Clang contracts `a*b+c` into fused multiply-adds by default, which MSVC's
`/fp:precise` does not do; compile generated units with `-ffp-contract=off`
(and `-fno-strict-aliasing`, as MSVC never assumes strict aliasing). Generate
cross builds with a build-machine recompiler; the target's own tool cannot run
there. A captured guest frame identical across Windows and ARM is a cheap,
strong first check that translation, scheduling and rasterization agree.

### Run the own-ISO preparation without Python or archivers

A phone has no Python, 7-Zip or desktop decryptor. Port each preparation step to
native tools and prove every one against the desktop pipeline's output, not just
against "it ran": a bounded ISO 9660 reader extracted the same 161 files as
7-Zip; the external decryptor rebuilt for the target produced the pinned ELF and
module hashes; the native font recipe produced the pinned prepared ELF; and the
native partition/post-processing generator produced byte-identical C++ to the
Python adapters (normalize CRLF from Windows tool output first, as Python's
universal-newline reads did). Keep the decryptor a separate program under its
own license; a minimal OpenSSL-compatible SHA-256 header and an explicitly
failing DES stub for unused firmware-update paths avoided bundling OpenSSL.
Generate profile facts (hashes, ranges, relocation fields) into a C++ header at
build time so the device tool needs no JSON parser. On Android, a build that runs
while the screen is off is heavily throttled; the app must run compilation as a
foreground task with a wake lock and bounded parallelism for available memory.

### Give the launcher the game's look without shipping game art

A launcher styled after the game wants title art and game screenshots, but those
are commercial assets. Draw them from the player's own copy instead: after the
first local build succeeds, run the new executable headless for a fixed time with
neutral controller input and the intro skip, dump its framebuffer, and convert
that to the launcher background in the player's own data folder. Make it
optional work after "ready": a failed capture keeps a plain background and never
fails preparation. Ship only the page, an original cursor image and an open
font. Scale pixel art by integers or `image-rendering: pixelated` and set text in
a vector font, so neither turns soft at high resolutions. To change text baked
into the art, rebuild that band from the surrounding pixels and draw the new
text over it. Size and place it in container query units tied to the art's
native 480×272 box, so it stays aligned at any window shape. A WebView2 host maps
the page and the art folder to two virtual HTTPS hosts and blocks all other
navigation. Its SDK is a launcher build input, so turn the launcher target off in
the player's local game build; otherwise configure fails on a machine without
the SDK. Keep each option's allowed values the same in the page, the host's
settings table and the backend's validation; a page choice the backend rejects
only fails at Prepare.

### Find the vblank cliff before optimizing a slow scene

A game that paces on vblanks does not slow down gradually. Once a frame's work
passes the two-vblank budget it waits for a third, so 30 FPS becomes 20 at
once, and a few milliseconds decide which side a scene lands on. Profile the
real thread, not a headless stand-in: a small sampler that suspends the game
thread about once a millisecond, walks its stack with DbgHelp and a PDB from a
`/Zi /DEBUG` build, and reports self and inclusive time was enough here.
Host-side costs hid among the emulation: GDI scaling at the window's full size
on the game thread, a filesystem existence check on every drive-state poll,
and whole-buffer allocations with zero-fill on every GPU flush. Move window
composition to a GPU swap chain on its own thread, fed by a latest-frame
handoff. That removes resolution-dependent cost from the game thread and
presents at the display's own refresh. Replace polled filesystem checks with
change notifications when the answer must stay immediate. Measure with the
harness's own overhead removed: a diagnostic frame dump every 500 ms made
periodic over-budget frames that normal play never has.

### Test whether game logic is frame-locked before raising the frame rate

Some engines scale by elapsed time, so a higher frame cap gives smoother motion
at the same speed. Many PSP games advance a fixed step per vblank instead.
Check by running the emulated vblank at double rate and comparing an in-game
clock against wall time. Here the countdown ran about twice as fast, so higher
refresh had to come from presentation, not from running the game faster.

### Draw in-between frames from recorded scenes, not from images

Record the draws behind each displayed frame from the exact rasterizer:
positions, world-space positions and matrices for camera-transformed draws,
and textures by content hash. Re-render them on a separate presentation
thread. Never re-run guest render callbacks, which also change game state.
Pair draws between consecutive frames in two passes:

1. Exact twins first: same material at the same world or screen anchor.
2. Then the nearest same-material draw within a small distance.

Build the material key only from values that are exact in both frames. A
projected screen size jitters by a sixteenth of a pixel as the camera moves.
Once a twin's key differs, the nearest-match pass pairs each tile with its
same-looking neighbour, and whole columns slide a tile and leave gaps. Use
world sizes for transformed draws.

Accept a scene only if re-rendering it at native size reproduces the exact
frame within a small tolerance. Show the exact frame otherwise. This handles
movies, CPU-drawn screens and unsupported states without special cases.

Time each scene by the vblank that latched it, less how late that vblank was
serviced. Draw at a display time one frame plus observed lateness behind.
Read the clock right after the vsync wait, before any per-scene work such as
a validation readback. Otherwise the frame after each new scene lands too late
and motion steps alternate between double and small.

Judge smoothness by measuring image shifts between rapid screen captures, and
inspect whole frames for gaps. Counting distinct captures alone missed both
the black-column bug and the uneven steps.

### Pair creatures by shape and velocity, not texture address

Sprite animation frames are often separate images at separate addresses. A
pairing key that includes the address loses the creature every other frame.
Worse, a look-alike that showed that frame a moment earlier becomes its
"nearest" match. The in-between frames then slide one creature over from
another's place: a teleport with an afterimage. Key on texture size and
format, pair the closest candidates first across the whole frame rather than
in draw order, and rank candidates by distance from the position their own
previous velocity predicts. That separates touching look-alikes. A second
pass without the size field keeps attack and turn frames attached, and a
lone part can inherit the motion of the part drawn next to it. To check, replay
recorded scenes through the real pairing code and measure where each moving
draw starts against where any draw of its family ended the frame before.
Count look-alike swaps as harmless, but confirm "jumps" against the
creature's own path; a naive tracker mistakes hops for glitches.

### Widen a 2D game's view from what the GE already culls

Before patching a game to draw more columns, count what it sends. A survey in
the rasterizer of transformed sprites by outcome (drawn, clip-culled,
scissor-empty) and screen x showed that Badman sends its whole level every
frame and leaves all culling to the GE clip test. The extra columns were
already in the command stream. Keep the sprites culled only horizontally in
the host-side scene, never in guest memory, and draw them in the presenter
over a wider native window. Find the camera from the view translation
(a RAM scan for words that follow it, then a write watch with the software
renderer). Clamp the wider view to the level using the game's own stage size,
checked against the camera each frame. Otherwise the sides show the void past
the level border. Move 2D draws as groups of touching draws by their nearest
screen edge. Treat clears (often drawn in strips) and full-width layers as
spanning the view, and keep them out of the grouping, or one full-screen
overlay merges the whole HUD into a single centred group. Effects tied to the
PSP screen's edges, such as a vignette, have to be found and left out: here it
was four camera sprites placed to follow the screen, not a 2D overlay. Verify
from the presenter's own back buffer. Desktop captures pick up whatever window
covers the game.

### Android presentation without blocking the game thread

Present a software-decoded frame through `ANativeWindow` from a dedicated
scanout thread, never from the game thread. `ANativeWindow_lock` can wait for a
free buffer, and a whole-pixel prescale writes millions of pixels. On a phone,
doing both on the game thread for every vblank pushed gameplay frames past the
two-vblank budget (30 to 20 FPS). Hand the decoded 480x272 frame over under a
mutex, skip frames whose pixels did not change, and let the scanout thread do
the scaling. Size the buffer to the smallest whole multiple that covers the
view, so the compositor's filtered scale is only a slight shrink. Keep the
SurfaceView itself at the PSP's shape. To pause in the background, block the
game thread inside the window's pump until the activity resumes.

### Watch the CPU clocks when a phone presents at the display rate

A presenter that draws an in-between frame on every refresh can slow the game
it presents. On the test phone, as soon as the app's surface presented at the
full display rate, the system lowered every core's maximum frequency to about
a third. The game thread, at 100% of one core, then took 2.2 times as long for
the same work, while the presenter itself used 4% of a core. It behaves like a
frame-rate governor: it sees a surface at its target rate and saves power,
not knowing that the game's own 30 FPS thread is behind. Measure it directly:
`scaling_max_freq` and `scaling_cur_freq` for each core while playing, and the
game thread's CPU time against wall time. A/B the presenter's modes (only new
frames, no readback, no refresh request) to find the trigger before
optimizing the emulated code. Also avoid C++ exceptions on hot paths that are
expected to fail, such as an unloaded palette entry: Android's unwinder is far
slower than Windows'.

### Building an APK without Gradle

javac, d8, aapt2, zipalign and apksigner are enough for a framework-only app.
Java 8 lambdas fail to compile against `android.jar` alone (no
`LambdaMetafactory`). Put the build-tools `core-lambda-stubs.jar` on the
classpath and d8 desugars them. `aapt2 link --debug-mode` marks a local
engineering build debuggable, so test data can be staged with
`adb shell "cat file | run-as <package> sh -c 'cat > files/...'"`, or a tar
through the same pipe. A retail phone may refuse perf events entirely
(simpleperf: "cpu-clock is not supported"). Do not change device security
properties to work around this; sample inside the process instead.

### Running a packaged compiler inside an Android app

Android refuses to run programs from an app's writable storage, but it runs
files from the APK's extracted native-library folder. Ship every program as
`lib<name>.so` under `lib/<abi>/` with `android:extractNativeLibs="true"`, and
start it with `ProcessBuilder` by its full path. The same applies to the
compiler (Termux's `clang-21` and `lld`), our own tools and an external
decryptor.

- **Versioned and clashing libraries:** library files that are not plain
  `lib*.so` (for example `libz.so.1`), and a second `libc++_shared.so`, get
  private names. At run time the app creates a folder of symbolic links from
  each soname to that file, and only the compiler processes see it first on
  `LD_LIBRARY_PATH`.
- **Running LLD:** a renamed LLD cannot pick its flavour from `argv[0]`;
  `lld -flavor gnu ...` works without a wrapper.
- **Matching the cross build:** compile with the NDK build's own flags
  (`compile_commands.json`) and the NDK sysroot headers. Link directly with LLD
  against the packaged runtime and NDK libc++ by path, with `--no-undefined`.
  The phone-built library then loads like the cross-built one.
- **Testing without the screen:** before trying the UI, run the same commands
  as the app over USB (`cat script | adb shell run-as <package> sh`) to check
  the commands without the file picker.

### Profiling where perf events are disabled

On a retail phone, simpleperf may refuse every event. An in-process
wall-clock sampler works without any device change. Use
`timer_create(CLOCK_MONOTONIC)` with `SIGEV_THREAD_ID` aimed at the thread of
interest. In the `SIGPROF` handler, record the program counter, link register
and frame-pointer chain into a preallocated buffer (Android arm64 code keeps
frame pointers). Later, on a normal thread, fold the samples into
`module+offset` stacks with `dladdr`. Symbolize on the build machine with
`llvm-symbolizer --inlines` against the unstripped libraries. Take the
innermost name for hot loops and the outermost for the real function. Idle
time shows up as the wait functions, which separates "busy" from "late".

### Many tiny draws: batch them, then parallelize exactly

When a game issues thousands of small draws per frame, per-draw overhead and
the single thread dominate. The cure is the same batching the GPU backends
use: stage packets per draw, bin them by 16x16 tile, and execute the batch
later. A CPU executor of the GPU pixel program then runs the batch on helper
threads. Visiting, per tile, each binned packet's covered pixels in bin order
gives every pixel the same packet sequence as the shader's per-pixel loop.
Checking it against the software rasterizer over full memory (random
textures, every blend equation, lazy palette errors, deferred GE lists) is
what makes it safe.

For primitives that stay on the software path, two-phase banding (sample every
band, then write every band's own rows) is exact when sampling never reads the
target. Integer edge equations let a band start mid-triangle with one
multiplication. Fall back to one thread for rows that alias past the stride
and for diagnostic write watches. Bind `thread_local` scratch lists through a
reference before lambdas run on other threads, or the helpers will each see
their own empty list.

### Android performance hints: report wall time, not CPU time

`APerformanceHint` adjusts clocks to meet a target work duration. Reporting a
thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`) omits its waits on helper
threads, so the system concluded there was headroom and dropped the cores to
about 1 GHz. Report the wall time between finished frames instead (a steady
30 FPS exactly meets a 33.3 ms target, and a late frame exceeds it). Include
the helper threads' ids in the session.
