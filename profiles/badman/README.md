# Badman native recompilation profile

This is a static-recompilation profile under development for **Holy Invasion of Privacy,
Badman!**, US UMD release `ULUS10456`, disc version `1.01`. It runs translated
Allegrex code on the native host and stops at services that are not implemented.
The current build reaches the rendered title with real Windows audio, clears
Training 1–3, and has cleared the first two Story stages through normal controller
input. All three tutorial checkmarks survived normal Save and fresh Load using
the exact v5 executable compiled locally from the player's ISO. Training 1 used
controller-driven dialogue, movement, terrain digging and Slime spawning.
Later tutorials, the full campaign and release validation remain unfinished;
this is not yet a complete playable release.

The newer developer build implements Direct3D 11 and Vulkan sprite rendering,
with a persisted rendering choice in the launcher alongside Software. Exact
full-memory comparisons cover 219 raster cases, 298 transform checkpoints and
GE stall/callback boundaries on all three backends. Clears and strips retain
scalar paths; window composition still uses GDI. Short headless Training 2
pilots measured about 28 guest updates per wall second on Direct3D and 29 on
Vulkan, compared with 12 on Software in the earlier matched scene. These are
engineering measurements, not displayed FPS or sustained fullscreen results.
The v7 source-only package passed fresh own-ISO compilation, read-only cache
verification and separate startup checks on all three backends. Root inspected
each ready prompt, logo movie and title capture; corresponding captures match
across APIs. The player's generated translations and executable remain in the
local data/build cache. Fullscreen performance and full campaign validation
remain separate gates.

The next performance update retains validated live framebuffer decoding and the
FPS counter, but skips a redundant GDI paint when the pixels, dimensions and
formatted FPS label match. CPU triangle strips also use exact incremental edge
equations and a validated VRAM write span, with the original diagnostic write
watch path retained. Full-memory, error, floating-point and write-watch comparisons
passed before adoption. On a Ryzen 9 5950X and Arc B580, an actual hidden 1440p
GDI game run improved from 21.19 to 28.43 idle guest updates per second; faster
digging after three qualified harvests initially still fell to 23.81. Compiling
the main Windows Allegrex translations with `/O2 /Ob2 /fp:precise` then raised that
section to 28.35, close to its 28.45 idle rate. The font module retains its
previous compiler settings. Fresh comparisons covered complete CPU, floating-point
and memory state for selected translated kernels before the ordinary gameplay run.
These are scoped engineering results from the same input protocol, not identical
final actor states or proof of sustained 30 FPS and physical fullscreen timing.
Larger populations and full campaign validation remain open. The source-only
performance package is being validated separately.

## Fullscreen digging and display refresh

The game paces its 30 frames per second on two 59.94 Hz vblanks. A frame whose
game-thread work exceeds about 33 ms waits for a third vblank, so heavy frames
drop straight to 20 FPS. On the user's 2560×1440 165 Hz display (Arc B580, Vulkan
renderer), continuous digging in Training 2 held 30 FPS idle but fell to about
19–20 FPS fullscreen. A wall-clock stack sampler on the game thread showed:

- GDI composition at the full client size: about 4.9 ms per frame with sharp
  pixels and 18.8 ms with smooth filtering, measured at 2560×1440.
- A `PARAM.SFO` existence check on every UMD drive-state poll (about 127 per
  frame): about 7% of the game thread.
- Exact CPU strip rasterization and texture sampling (about 25%), and GPU
  transactions that allocated and zero-filled two 2 MB VRAM buffers per flush.

Changes, all keeping guest-visible results identical:

- `host/badman_presenter.*`: a Direct3D 11 flip-model swap chain on a child
  window, driven by its own thread. The game thread only hands over the decoded
  frame; scaling (integer, fit, stretch), point or linear filtering, the FPS
  label and vsynced presentation at the display's own refresh rate (60–165 Hz
  and above) run on the presenter. GDI composition remains the fallback (no
  hardware device, `BADMAN_PRESENTER=gdi`, a stopped presenter) and still
  draws the native text-input and utility dialogs.
- `Disc::present` re-checks `PARAM.SFO` only after a Windows change
  notification under the data root, so removal is still seen.
- GPU flushes and readbacks reuse their buffers. Wrapped texel addressing uses
  integer remainders and masks, which equal the previous `fmod`/`%` results for
  the exact power-of-two texture sizes. Strips reuse their fragment buffer.

Measured with the same scripted route (Training 2, Square plus direction held
for 24 s, about 170 blocks dug) and no diagnostic frame dumps, fullscreen
2560×1440: the previous build fell to 22.6–29.5 FPS (mostly 23–26), and the new
build held 29.9–30.0 throughout. The game thread went from 80% to 61% busy
while digging. Windowed digging also held 30.0. All host suites (with hardware
GPU comparisons enabled) pass. The game still simulates and renders 30 frames
per second, as on the PSP. A 165 Hz display shows each new frame at its next
refresh, without tearing. Extra in-between frames would need motion
interpolation, which is not implemented.

## Smooth motion up to the display refresh rate

Badman's logic advances per vblank tick, not per elapsed time. Running the
emulated vblank at double rate made the Training 2 countdown fall 27 seconds in
14.5 wall seconds. So unlike engines that scale by elapsed time (for example
Metroid Dread's recompilation, which simply raises the frame cap), more game
frames would speed up the game. Instead, the game keeps its 30 frames per
second and the presenter draws in-between frames:

- `host/badman_scene.*`, Raster scene recording: when enabled, every clear,
  sprite and triangle the exact rasterizer draws is also copied into a
  host-owned Scene per target framebuffer. It records exact screen positions,
  world-space positions of camera-transformed draws, view/projection/viewport
  and textures decoded once per content hash (texel memory hashed once per
  frame, palettes once per CLUT load). Guest memory, timing and callbacks are
  untouched. A displayed frame carries its scene, stamped with the steady time
  of the vblank that latched it.
- `scene_motion` pairs the draws of consecutive scenes. A material is the
  kind, texture size and format (not its address: creature animation frames
  sit at different addresses), blend mode, size in world units for
  camera-transformed sprites (screen units for 2D ones) and vertex count.
  Pairing runs in four steps:
  1. Unchanged twins: draws that were standing still, at the same anchor.
  2. Closest pairs first across the whole frame, within 24 pixels. Each
     candidate is ranked by its distance from where its own previous velocity
     predicts it. A steady fast mover may travel up to 48 pixels when it lands
     near that prediction.
  3. A second chance ignoring size (one creature across animation frames)
     within 8 pixels.
  4. A part still alone takes the motion of the moving part drawn next to it.

  Camera-transformed draws also follow the change of the view matrix; unpaired
  ones stay put in the world. A camera jump over 96 pixels is a cut.
- The presenter re-renders each scene on the GPU at the window's resolution
  (game blend modes, clamp/wrap texel addressing, 1/16-texel bilinear weights,
  modulate rounding). It uses a scene only if it re-renders to the exact frame
  at native size within tolerance (at most 0.5% of pixels differing by more
  than 40 per channel, which allows the game's dither). Other frames (movies,
  CPU-drawn screens) are shown exactly. Matching scenes form a timeline drawn
  one frame (plus observed lateness) behind, at the time of each display
  refresh, so every refresh is an even fraction between two game frames.
  When a game frame arrives late (the game dropped a frame), the display
  clock pauses on the newest frame and then catches up gradually instead of
  jumping.
  Display latency grows by about one game frame (about 35 ms).

Measured on the user's 2560×1440 165 Hz display, fullscreen, Vulkan renderer,
scripted Training 2 digging: the game held 30.0 FPS with one 29.7 sample. The
presenter showed 15,750 frames for 2,859 game frames. The label now reads
"FPS: 165 (game 30.0)": frames shown first, the game's own rate after it. 2,838 of 2,840 scenes matched their exact frames, with about 35
differing pixels on average. While the camera scrolled one tile per game frame,
24 screen captures 6 ms apart showed even 16–17 physical-pixel steps on every
refresh. Without in-between frames the image could only change once per game
frame. An early version paired scrolled tiles with their neighbours: matching
keys used projected screen sizes, which jitter by 1/16 pixel. It showed a
sliding black column; world sizes fixed it, and `tests/scene_tests.cpp`
covers that case. Slimes then still occasionally teleported with a ghost.
Their animation frames use different texture addresses, so a slime switching
frames matched a nearby slime that had shown that frame (moves of 16–34 pixels
for creatures crawling 0.7 pixels per frame). Address-free materials and
closest-first pairing removed those cross matches. Then velocity prediction,
the size-free second chance and part carrying handled touching look-alikes,
attack frames and multi-part creatures. Replaying recorded slime scenes
through the real pairing code shows the remaining on-screen discontinuities
are new or vanishing objects and creatures genuinely hopping. Training 3 and 4
showed the same slime family plus the pickaxe's swing, which pairs with
itself; other species were not on screen in those runs. The launcher's Graphics page turns this on or off ("Smooth
motion up to your refresh rate", `BADMAN_INTERPOLATION`). Text-input and
utility dialogs still use the GDI path. Remaining limits: dither is not
re-applied at high resolution; objects that appear, vanish or teleport move
in 30 Hz steps; only the Training 2 route has been measured.

## Ultra-wide view in the dungeon (prototype)

The dungeon can be shown wider than the PSP's 480 pixels, up to 21:9, with
real level picture at the sides rather than stretching or filler.

What the game does (read-only analysis of the translated code and live
captures in Training 2):

- The game does not cull the dungeon itself. Every frame it sends the whole
  level through the GE as camera-transformed sprites: about 9,500, of which
  about 1,000 pass the GE's own clip test. Creatures, surface scenery and the
  sky strips are sent the same way, whether or not they are on screen.
- The camera is a plain orthographic view translation (screen x = world x +
  view x). Guest function `0x0882E3A4` moves it in 18-unit tile steps when
  the cursor leaves a dead zone of ±90 by ±54 pixels around the screen centre.
  The camera struct is at `0x089B5318` (+0: left edge in world units, +12:
  zoom). The left edge stays within [-18, W·18 − 459], where W is the stage
  width. The game reads W through `0x08834AF8` (stage object `0x089BD048`)
  and `0x08836158` (+4 width, +8 height). Training 2 is 60 by 52 tiles, so
  the camera ranges over [-18, 612].

So no guest patch is needed. With `BADMAN_WIDESCREEN=1` (launcher: Display,
"Ultra-wide view in the dungeon"):

- The scene recorder also keeps camera sprites that the GE culled only for
  lying left or right of the screen, within the extra width plus one tile.
  They never reach guest memory. With the option off, recording and the exact
  480×272 rasterization are unchanged.
- The host reads the stage width and accepts it only when the camera struct
  agrees with the frame's own view and the frame is drawn from 18-unit tiles.
  The level extent, border column included, is world x [-18, W·18 + 18].
- The GPU presenter draws such scenes over the window's width up to 21:9:
  centred on the game's view, then kept inside the level, so the border column
  is the last thing shown at either edge. 2D draws move as groups of touching
  draws: the left and right thirds keep their distance to their screen edge,
  and the middle stays centred. Clears and full-width 2D layers span the view.
  The dungeon's vignette (four mirrored 240×136 camera sprites that darken the
  PSP screen's edges) is left out. Scenes without a known level (title, menus)
  and frames without a matching scene (movies) keep the PSP shape with bars.
  Integer scaling keeps whole multiples. Stretch scaling turns the option off.
- It works with or without in-between frames (both checked). With them, the
  view's position is interpolated too.

Verified in Training 2 on the Arc B580 (Vulkan GE, Direct3D 11 presenter,
1904×816 client area, integer ×3, 634-pixel view), using the presenter's own
back buffer (`BADMAN_PRESENT_CAPTURE`, not desktop captures). Results:

- No gaps, seams or vignette edge while scrolling.
- The gray border column is the last thing shown at both camera clamps.
- The HUD sits at the window's edges and the tutorial dialogue stays centred.
- The title keeps 16:9.
- 2,087 of 2,089 scenes matched their exact frames.
- Scene rate and late frames are the same as with the option off, both on the
  scroll route and while digging.

Not covered: other Story stages, Edit-mode test play, and narrow stages
(W < 25, where the game itself shows past the border and the option stays off).
The pause menu also shows a limitation of the grouping: the Monster Born panel
touches the menu and stays with it in the middle. Debris strips are not yet
kept beyond the edges. The Android presenter does not have the option.

Gameplay is unchanged: the game's camera, logic and culling are untouched.
At 21:9 the player sees about four extra tile columns on each side, so
creatures and heroes become visible earlier than on a PSP.

The developer runtime also implements a native JPEG screenshot utility for the
title's observed request ABI, with real image encoding, exclusive numbered file
publication and utility-dialog exclusion. Synthetic component and actual HLE
tests pass. Ordinary in-game R+Select produced two real, independently decoded
480×272 images on each hardware backend, preserving existing images and saves. Images
use the native memory-stick `screenshots` directory; PSP screenshot metadata
and directory interoperability are not claimed.

The current renderer includes a yellow top-left FPS overlay with a black shadow,
updated about once per second. It counts guest framebuffer submissions when
accepted or latched; repainting the same scanout does not add frames. Window
composition now publishes one completed offscreen image to avoid exposing the
intermediate black clear. Actual visible-window flicker confirmation is pending.
Validated contiguous framebuffer/texture reads and matrix quantization changes
improved neutral Training 2 throughput by 13.32% over the window/decoder-only
candidate across four balanced hidden-window runs. This is guest-update
throughput, not display FPS or full-speed gameplay. All 37 integrated suites
passed at that earlier checkpoint.

Training 2 has now reached its normal TRAINING CLEAR screen after 26 ordinary
digs. Three harvests each observed eight live nutrients at the accepted Square
edge and incremented the required Omnom birth counter, ending in phase B with
the clear dialog. This used the original v4 runtime with a private read-only
pointer-watch diagnostic in main; the diagnostic executable is not distributed.
Normal Save succeeded, and a fresh process using the exact unmodified v4
locally compiled executable loaded the save and displayed both Training 1 and
Training 2 checkmarks. Earlier ordinary TIME OVER and missed-harvest results
are preserved. Later tutorials and the full campaign remain unverified.

Training 3 has also reached its normal TRAINING CLEAR after three genuine
Omnom Fly births, including the larva and pupa stages and ordinary feeding.
Normal Save succeeded in a separate test store. A fresh process using the exact
unmodified v5 executable loaded it and displayed earned Training 1, 2 and 3
checkmarks. Training 4 was unlocked; its completion is not yet verified.

The palette renderer retains raw latched colors and caches decoded entries for
the most recently loaded format, with raw fallback for other formats and retained
tails. Three runs per variant in a neutral Training 2 scene measured about 6.4%
more guest updates per second and 6.2% lower CPU cost per update. This is a scoped
engineering measurement, not display FPS or a full-game performance guarantee;
load-heavy synthetic cases incurred additional palette-load cost. Diagnostic
frame publication also retries brief Windows reader contention while preserving
the old frame and reporting persistent failures.

The developer build also runs a first Story battle through Badman's capture,
Stage and Final Results, declining the Save prompt, and returning to the title.
The first Story stage has now also reached a natural STAGE CLEAR: ordinary
digging and placement retained 16 living Slimemoss at battle entry, the first
hero's actual HP fell, and the game set its victory result before showing the
clear screen and normal upgrade/results menus. This used the v5 local runtime
with the private read-only diagnostic main; no HP, clock or progress was patched.
The second Story stage subsequently cleared normally as well, and the game
advanced to the third stage's construction phase. The rest of the campaign
remains unverified.
The Almanac list and food-chain screen, Sound settings, Controls, empty Army
List and Briefing have been exercised. Full campaign completion remains unverified.
An explicit translated-function movie probe exercised the audio-enabled ending
movie through normal EOF and legitimate skipping, including worker/resource
cleanup; this is separate from reaching that movie through normal Story play.

The latest title run submitted and completed 988,928 audio frames, including
1,935,784 nonzero samples. The game's own translated code mixes effects;
ATRAC music uses a verified decoder and real device completion. Native saves use
an explicitly private versioned format, with integrity receipts and confirmed
atomic overwrites. Existing PSP saves are preserved and are not interoperable
with this native format. Deleting through the actual Data menu and loading in a
fresh process correctly reports no data. Further gameplay checks are pending.

Edit mode now reaches its Hero Groups editor, color selection, stage settings
and Test Play construction. Returning from Test Play to the editor and title
has succeeded. Cancelling the actual Name, Title, Comment and Stage Title text
dialogs preserves the original text. An explicit diagnostic text response also
passed through the translated Name handler and displayed the confirmed name.
Hidden native-control tests cover editing, length limits, UTF-16, confirmation
and cancellation; physical keyboard interaction remains unverified. Edit/text-entry
support is included in the v3 through v5 test distributions.
The exact v3 locally compiled output also committed a renamed hero through
Decide and Editor Save, then displayed the saved name after loading in a fresh
process without a text provider. This used a separate disposable native save.

The latest test distribution is `out/badman-training-test-v7`, with its archive
at `out/badman-training-test-v7.zip`. All 188 files match the audited source-only
package, including 124 reviewed source files. The same locally compiled static
CRT executable passed all three headless startup runs with real completed audio
and verified monthly codec libraries. Evidence is in the ignored
`analysis/training-test-v7-root-review.json`. This remains a test release;
visible 2560×1440 performance, later tutorials and campaign completion are pending.
Older packages are retained unchanged.

The previous tested distribution is `out/badman-training-test-v6`, with the
complete archive at `out/badman-training-test-v6.zip`. It adds the yellow FPS
overlay, offscreen composition and measured renderer changes described above.
Fresh own-ISO compilation, read-only verification and headless Play through the
logo movie and title passed; all 157 archive entries match the audited source-only
package. Actual visible-window flicker and performance retesting remain pending.
v5 is retained unchanged. It is an immutable test
candidate, including Edit/text entry and recovery after interrupted dependency
preparation. Fresh own-ISO compilation and headless Play passed for this exact
bundle. Older `badman-recompiler-source-only-v*` folders are historical review
artifacts. v5 adds the active-format palette cache and diagnostic publication
fix described above; all 37 integrated test suites passed.

v4 added validated contiguous-word memory access while preserving mirror/boundary
fallbacks and diagnostic write watches. All 36 integrated test suites passed.
Six matched engineering windows measured 27.84% more neutral Training 2 guest
updates per second and 25.13% less process CPU per update. These are scoped
simulation measurements, not whole-game FPS. Generated game code remains
`/Od /Ob0`; the isolated `/O2` experiment did not improve that scene.

Native startup passes the CRT/static constructors and loads the translated font
PRX. The exact v5 packaged build, compiled locally from the player's own ISO,
decoded all 385 logo movie frames, performed 214,466,560 display-copy bytes and
delivered 775 translated vblank callbacks before reaching the rendered title.
Its 493 submitted audio buffers completed on the real Windows audio device.
Captures show the Japan Studio logo and title; the earlier transformed-sprite
and idle-IRQ stops have been resolved.
Confirming the prompt reaches a savedata LISTLOAD utility; an empty local memory
stick reports the real no-data result after acknowledgement. Native-format saves
support loading, saving, confirmed overwriting and deletion. Existing PSP-format
saves remain unsupported and are preserved. Clears, sprites and bounded
orthographic triangle strips, including T4/T8 palettes, write real guest VRAM.
A capture of the latched display buffer shows the game's Memory Stick prompt
with text and OK/Cancel controls. The Windows framebuffer presenter and keyboard
input are connected. Headless controller routes and hidden native-control tests
provide the current integration evidence; a complete visible-window user test
of the current source-only package remains outstanding.

The game's `libfont.prx` uses compact `PT_PSPREL2` relocations. A verified recipe
reproduces its 902 observed relocations after external decryption; the native host loads a verified,
fixed-address ELF at `0x09FD0000`, reserves its memory, and binds 28 exports to real
translated code. Its map contains 7,727 body instructions in four units plus
11 import wrappers. The 22 raw import-stub instructions are excluded from bodies.

`BadmanFontProbe` independently executes real NewLib/list/open/info/glyph/cleanup
exports against local PGFs. It rasterizes an A (16 by 15, 103 nonzero pixels),
preserves output guards, and frees all nine callback allocations. This probe
establishes font execution, not a rendered game frame.

The distributable target is `BadmanLauncher.exe` plus the independent recompilation
tools and runtime sources. The user selects their own supported ISO; preparation
must extract/decrypt it, generate Allegrex translations as C++, then compile a
Windows executable in that user's data/build cache before Play. No game executable,
game-derived library, generated translation or extracted game asset belongs in
the distributable. The source-only training-test-v5 package passed a fresh own-ISO
preparation and local Windows build in a Korean data path, using bundled Python
from an external working directory with a minimal PATH. All 66 main and 4 font
translation units were generated and compiled locally; preparation/build took
188 seconds. Cached read-only verification passed, and Play launched that local
executable. Earlier source-only review checks also exercised invalid input and
missing compilers. The training-test-v5 distribution's
complete file inventory, hashes and modification times remained unchanged.
This verifies local preparation and compilation, not complete gameplay or a
clean-Windows installation. Installed Visual Studio 2022 C++ Build Tools, a
Windows SDK, CMake and 7-Zip are currently prerequisites.

The exact packaged Play run also verified Unicode codec loading from the user's
dependency cache and the pinned monthly FFmpeg libraries. These results verify
startup and local compilation; they do not establish complete gameplay.

Earlier launcher tests verified Browse, extraction, streamed runtime results and
settings persistence using a local prebuilt development executable. That mode is
an engineering checkpoint and does not satisfy the distribution requirement. The
older `out/badman-player` folder is a personal development snapshot, not a shipping
package. Runtime completeness and the local-build pipeline are separate milestones.

## Launcher window

`BadmanLauncher.exe` hosts an independently written HTML/CSS/JavaScript page
(`launcher-ui/`) in Microsoft Edge WebView2. The SDK loader is linked statically
from NuGet `Microsoft.Web.WebView2` 1.0.2903.40; configure with
`BADMAN_WEBVIEW2_SDK` pointing at its `build/native` folder (default
`out/tools/webview2-1.0.2903.40/pkg/build/native`). The WebView2 Runtime is a
Windows component the player already has on Windows 11 and current Windows 10.
The player's own local game build passes `-DBADMAN_BUILD_LAUNCHER=OFF`, so it
needs no SDK.

The window has a persistent light-blue tab row: Play, Display, Graphics,
Gameplay, Controls, Advanced and Credits. Only the page above it changes. Play
holds the ISO and data folder pickers, the diagnostic trace toggle, Prepare and
build, Open Saves, the big PLAY button and the preparation/runtime log. The
replacement fonts are picked automatically from the bundle. The settings pages
contain only options the runtime actually honours:

| Page | Option | Passed to the game as |
|---|---|---|
| Display | Fullscreen; scaling (integer, fit, stretch); ultra-wide view in the dungeon | `BADMAN_FULLSCREEN`, `BADMAN_SCALING`, `BADMAN_WIDESCREEN` |
| Graphics | Rendering API; filtering (sharp pixels, smooth) | `--renderer`, `BADMAN_FILTER` |
| Gameplay | Skip intro and logos; automatic save profile; FPS counter | `--skip-intro`, `BADMAN_AUTOMATIC_PROFILE`, `BADMAN_FPS_OVERLAY` |
| Controls | Controller; left-stick dead zone | `BADMAN_CONTROLLER`, `BADMAN_STICK_DEADZONE` |
| Advanced | Compile jobs (1–4), verify, open folders | `--jobs`, `--verify-only` |

Rows marked for one platform show only there. The Android app hosts this same
page (see "Android port status") with its own rows: CPU, Vulkan or Software
rendering, smooth motion, on-screen touch controls, verify and *Prepare again
from your ISO*; Display (and any tab with nothing for the platform) is left out.
Mouse, keyboard (arrows, Enter, Esc) and controllers through the browser Gamepad
API drive the page; a pixel-art hand marks the focused item. While playing, F11
or Alt+Enter toggles borderless fullscreen and XInput pads (Xbox controllers,
Steam Deck; others through Steam Input) map onto the PSP buttons and analog stick.

Backgrounds are game art, so none ships. After the first successful Prepare,
setup runs the player's new executable headless for 30 seconds with neutral input
and Skip Intro, captures the title frame and writes
`%LOCALAPPDATA%/BadmanRecomp/art/play.png` through `tools/launcher_art.py`. That
tool rebuilds the subtitle band so the page can draw "What Did I Do To Deserve
This Recompile?" as sharp vector text. A capture failure is reported and leaves
the plain background; it never fails preparation. The other pages read optional
`display.png` … `credits.png` from the same folder and blur them; without them
they use the plain gradient. Pixel art scales with `image-rendering: pixelated`
and all text uses the Pixelify Sans vector font (OFL), so both stay sharp at any
window size.

A player may replace `play.png` with a larger redraw of the same title screen,
either the full 480:272 frame or 480:240 without the copyright bar (for example
1441×720). `python tools/launcher_art.py title <redraw> play.png` removes its
subtitle and menu: it looks for clean rows two game pixels clear of the text
with matching ray edges, then fills each ray from its median colour. The page
reads the image's own shape, fits the Play art to the window width so its
edges are never cut, positions the subtitle in game pixels and filters art
wider than 720 pixels smoothly. The native capture path keeps its fixed anchors
and its output stays byte-identical.

Verified: a fresh own-ISO preparation from the assembled source-only package
(bundled Python, Korean data path) reached READY in 330 seconds and captured a
`play.png` byte-identical to the reviewed one. Window captures at a 1284×721 client area showed integer
2× with the FPS counter, fit with smoothing, and stretch without the counter. In a
browser preview, arrows, Enter and Esc moved between tabs and rows and changed
values. Not yet verified: fullscreen toggling, XInput with a physical pad and
Gamepad navigation of the page.

## Launcher options: Skip Intro and the automatic Saves profile

The launcher has a persisted **Skip intro and logos** toggle on its Gameplay page
and an **Open Saves** button on Play. Setup passes `--skip-intro`/`--no-skip-intro` to the backend,
which sets `BADMAN_SKIP_INTRO` for the native game.

Preparation creates `<game data>/Saves` silently, with a default host profile
(`.badman-player.json`, no game payload). Saves from the older
`.badman-state/memory-stick` location are copied into it once and verified;
the originals are kept. Play mounts that folder through `BADMAN_SAVES_DIRECTORY`,
which also enables the automatic profile (`BADMAN_AUTOMATIC_PROFILE=0` turns it
off for diagnostics):

- The boot "Please insert a Memory Stick" prompt is answered through a genuine
  controller packet: Cross when native save records exist, otherwise Circle.
- Load uses the game's remembered slot, then the first valid saved slot, with no
  chooser. An empty profile returns the real no-data result silently.
- Save writes the game's own slot without a chooser or overwrite prompt; the
  previous save stays as recovery history. Damaged data, non-overwriting
  requests, other errors and Delete remain interactive.

Skip Intro gives the logo player its skip-button mask and presses Cross on that
player's own controller reads, so the game runs its normal movie cancel and
cleanup. Both behaviors live in `host/badman_startup.*` and act at guest
function entries through the runtime's dispatch/chained-call hooks; generated
code is not patched.

Verified with the developer build, headless, neutral controller input and the
D3D11 renderer: with both options off, the game stays at the Memory Stick
prompt after 30 seconds. With them on, a new profile (Circle) and a migrated
earned save (Cross, silent LISTLOAD result 0) both reach the title menu, with
one logo press and no movie resources left open. A read-only watch showed a
payload-backed word set in game state only after the earned load. Synthetic
tests cover the policy, the automatic save modes and the migration. The packaged
`out/badman-training-test-v10` (archive `.zip`, SHA-256 `aaa3afee…7aa6`) then
passed a fresh own-ISO preparation and local build from a Korean/emoji data
path, created Saves silently and reached the title through Play the same way.
Visible-window use and in-game Save through the automatic profile have not been
exercised yet.

## Android port status

The Android app (*Badman Recompiled*, below) prepares and plays the game on the
phone. It keeps the own-ISO rule: the APK contains only independent tools and
runtime sources, and the phone itself extracts the ISO, generates the C++
translation and compiles it. The notes in this section record how that was
established, in order.

Verified on a physical arm64 phone (Android 16, API 36) under a normal app
account: Android blocks executing files from app-writable storage, as
expected; Clang 21 and LLD packaged in the APK's read-only native-library folder
compile and link C++ into a private `.so`, which loads by file descriptor and
runs. Two real generated Badman units compiled at `-O2` in 6–7 seconds each
with about 340 MB peak memory. The generic recompiler, cross-compiled for arm64
Android, produced output identical to the Windows build apart from line endings.
The framework, the host library and all 28 C++ host test suites also
cross-compile with NDK r27d, and every suite passes on that phone. On POSIX,
native saves are published with `renameat2(RENAME_NOREPLACE)` because Android
refuses hard links; the separate-process save round trip is still Windows-only.

A developer-only cross-compiled `BadmanNative` (generated with the Windows
recompiler through `PSPRECOMP_HOST_RECOMPILER`) boots on the phone from an adb
shell: it runs the game's startup, loads the font module, renders through the
software rasterizer and answers the boot prompt, then stops explicitly at the
logo movie because no Android FFmpeg is configured. With the options off, the
captured prompt frame is pixel-identical to the Windows build's. This binary is
an engineering test, not a distributable.

With a minimal LGPL FFmpeg 8.1.3 built for Android from the same commit as the
Windows pin (`tools/build_ffmpeg_android.sh`), the phone now plays the logo
movie and title music: it copied the same 214,466,560 movie bytes and ran the
same 774 vblank callbacks as Windows, the AAudio transport completed buffers on
the real device, and the title frame is pixel-identical to Windows. Skip Intro
behaves identically too. The FFmpeg libraries are loaded at runtime from a
separate directory and are not part of this source tree.

The own-ISO preparation pipeline also runs on the phone without Python or
7-Zip. Starting from the ISO, the phone verified its hash, extracted
`PSP_GAME` with the framework's `psp_iso_extract` (161 files, identical to
7-Zip), decrypted the EBOOT with an Android build of the external pspdecrypt
(`tools/android/build_pspdecrypt_android.sh`), rebuilt the font module with
`badman_prepare font` (pinned SHA-256) and generated 66 main plus 4 font units
with `badman_prepare generate`. All 74 generated files are byte-identical to the
Windows Python pipeline's. The packaged Clang then compiled all 74 units on the
phone under the app account (677 s with the screen off and little free memory;
a shipping app needs a foreground task and wake lock). With `BADMAN_SPLIT_GAME_LIBRARY` (default on Android) the build produces an
independent `libbadman_runtime.so` and a separate `libbadman_game.so` holding
only the generated translation, loaded by name through `game_entry.cpp`. On the
phone, the packaged Clang/LLD compiled the 75 game sources (313 s with the
screen awake) and linked `libbadman_game.so` against the shared runtime; the
game then ran from that phone-built library with the same movie bytes, vblank
count and audio completions as Windows, and a title frame pixel-identical to
Windows. The whole own-ISO pipeline therefore works on the device. An Android
app that drives it, presents the game on screen and maps controllers/touch is
the remaining work; no Android Play exists yet.

Remaining before an Android Play button: platform versions of the Windows-bound
host services (window/presentation, audio, native filesystem and save
publication, screenshots, clock, codec and Vulkan loading); Android ISO
extraction, generation and font-preparation tools; an Android build of the
external decryptor kept under its own license; a license audit of the shipped
compiler; and the in-app own-ISO preparation flow. Presentation, controller
and on-screen touch input, and pause/quit lifecycle now exist in the
engineering app below.
The earlier `out/android-port-v1` APK is a core-only scaffold, not a game port.

### Android app (engineering build)

`android/` holds the app: plain Java built without Gradle by
`tools/android/build_apk.py` (javac with the build-tools lambda stubs, d8,
aapt2, zipalign, apksigner). The APK contains:

- the Java app;
- `libbadman_runtime.so`, the JNI library `libbadman_android.so` and the NDK
  C++ runtime;
- the phone-side tools from the NDK build, as `lib*.so` executables:
  `psp_iso_extract`, `badman_prepare`, `psp_recomp` and the external
  `pspdecrypt` with its licence;
- Termux's Clang 21 and LLD with their libraries, every file checked against
  the signed package index (`COMPILER_PACKAGES.json`);
- `assets/toolchain.zip` (NDK API-29 headers and link stubs, compiler-rt
  builtins, Clang resource headers, the framework headers and
  `game_entry.cpp`) and `assets/profile.json` (ISO, executable and font
  hashes from `config/`);
- the separately built LGPL FFmpeg, the nine pinned replacement fonts and the
  licence texts, including the compiler packages' notices.

Termux's `libc++_shared.so` and the versioned compiler libraries ship under
private names (`libpsp_dep_*.so`). On the phone, a folder of symbolic links
gives the compiler processes their sonames, so the game keeps the NDK libc++.
The script refuses `libbadman_game.so`; the game library is never packaged.
One local key (`out/android/badman-debug.keystore`) signs every build, so
updates install over each other.

- The app is called *Badman Recompiled*, with a pixel-art icon (an adaptive
  icon: the 47-pixel art at a whole-number scale per density over its own dark
  edge colour, `android/res`).
- `LauncherActivity` shows the Windows launcher's own page (`launcher-ui`,
  packaged as `assets/launcher`) in a WebView, locked to portrait. In portrait
  the title area spans the width, the Play controls stack under it with PLAY
  last, and the tabs wrap onto two rows (one `@media (orientation: portrait)`
  block; landscape is unchanged). The activity plays the part `launcher.cpp`
  plays on Windows: the page is served from the APK under
  `https://launcher.local/` (nothing else loads), messages go through a
  JavaScript interface, and launcher settings map onto the app's preferences.
  *Game ISO* opens Android's file picker and keeps a read grant for the chosen
  document; *Prepare and build* then prepares from it (or only compiles again
  when the prepared files are in place but a new app version needs a fresh
  game library). Progress shows in the page as the current step and a dig bar
  with the percentage (its edge kicks up pixel dust while working; it replaces
  the Windows log on the phone), and in a notification, with Cancel. Back closes an expanded log, then returns to
  Play, then leaves. Play runs only when the decrypted executable, the
  prepared font module and a game library built against the installed runtime
  are all present. On first Play it installs the fonts and creates the owned
  `Saves` folder with its marker (it never adopts an existing folder).
  Backgrounds come from the app's own `files/art` folder (`play.png`,
  `graphics.png`, `gameplay.png`, `controls.png`, `advanced.png`,
  `credits.png`, served as `https://art.local/`), like the Windows art folder;
  none ship in the APK. Portrait art fills the screen; on Android the Play art
  carries its own title (no subtitle overlay), the Play controls sit at the
  bottom, and the other tabs show their art sharp under a light shade. Without
  art the page keeps its plain gradient.
- `BuildService` is a foreground service with a wake lock that runs the
  preparation steps:
  1. Copy the ISO into private storage while hashing it; anything but the
     ULUS10456 1.01 image stops here.
  2. Extract `PSP_GAME`, decrypt the executable and rebuild the font module,
     checking the expected hashes.
  3. Generate the main and font C++ units.
  4. Compile every unit with the NDK build's flags, one compile per core as
     free memory allows.
  5. Link `libbadman_game.so` with `--no-undefined` against the packaged
     runtime and NDK libc++, and record the runtime's hash.

  The ISO copy, extraction leftovers, generated sources and objects are
  deleted afterwards. Per-step logs stay in `files/logs`.
- `GameActivity` runs in its own `:game` process. The same native `main`
  (`BADMAN_EMBEDDED_MAIN`) runs on a 16 MiB-stack game thread. Output goes to
  `files/logs/game.log`, which the app shows if the game stops with an error.
- The Android `Window` (`host/badman_android_bridge.*`) draws into a
  SurfaceView kept at 480:272. A scanout thread repeats pixels by the smallest
  whole factor that covers the view, and skips unchanged frames, so the game
  thread never waits for a surface buffer. Leaving the app pauses the game
  inside `Window::pump`. Back asks before quitting.
- Input: controllers by position (A Cross, B Circle, X Square, Y Triangle,
  L1/L2 L, R1/R2 R, Start, Select, d-pad or hat, left stick as the analog nub
  with a 20% dead zone), keyboards as on Windows, and on-screen touch controls
  (`TouchOverlay`): a left analog stick that re-centres where the thumb lands
  in the left third, the four face buttons on the right, L and R in the top
  corners and Select/Start at the top centre. The game reads the analog nub
  for the cursor (the poller at `0x0882F570` keeps values outside 64–192 and
  stores them at `0x089BCF00`), so the stick sends the nub. Menus read only
  the d-pad, so pushed most of the way along one axis (80%, released below
  65%) the stick also presses that d-pad direction. Levels are unaffected:
  the game's input helper at `0x0882F978` adds a d-pad press to the nub as a
  full push clamped to 0–255, and already reports a nub beyond its dead zone
  as d-pad bits.
  A tap stays held at least 70 ms, so it is never missed between the game's
  input reads. The overlay hides while a controller is connected (a device
  with gamepad or joystick input) and as soon as one sends input, and shows
  again when the last one disconnects. The PSP keyboard dialog opens as an
  Android text dialog.
- `BADMAN_RENDERER=vulkan` now also loads Android's `libvulkan.so`; the
  phone's Adreno 840 runs the compute rasterizer with correct output.

Verified on the NX809J, with test data staged through `run-as` (the decrypted
executable, the prepared font module and a game library cross-built on
Windows): the title screen appears letterboxed and correct. Gamepad A, the
d-pad, touch Cross, touch Circle and the touch d-pad all drive the menus.
Training 2 is playable. The title holds 30 FPS. Gameplay runs at 26–30 FPS with
the software rasterizer (about 20 before the scanout thread) and similarly with
Vulkan, so the game does not hold 30 FPS on the phone yet. Simpleperf cannot
sample on this retail phone (perf events are disabled), so the next
performance step needs in-process sampling. The staged data was removed from
the phone afterwards.

In-app preparation from the player's own ISO was then verified on the same
phone. The player picked the ISO in Android's file picker, and the app checked,
extracted and decrypted it and rebuilt the font module. The decrypted
executable (`72233226…`) and font module (`cd2f652f…`) matched their pins.
Generation took about 25 s and compiling the 75 units, 8 at a time, about
2 minutes, so a whole preparation takes about 3 minutes. Play then ran the game
from the library the phone had just linked.

The first attempt failed at the link with an undefined `_Unwind_Resume`: the
direct LLD link lacked the NDK `libunwind.a` that NDK Clang adds. The link now
uses NDK Clang's exact library list and order. *Compile again for this
version* rebuilt from the already prepared files without the ISO.

After preparation the app keeps about 125 MB: the game files 45 MB, the game
library 33 MB, the unpacked compiler files 40 MB and the fonts 5 MB. The
on-device test ISO copy was removed afterwards. Gameplay frame rate is
unchanged (25–28 FPS), and an "Open with" entry for file managers does not
exist yet.

### Android performance: a steady 30 FPS

With the profiler below, the game thread on the phone showed about 80% of its
time in the GE software rasterizer. The game issues about 9,500 sprite draws
per frame (around 70 pixels each) with about 6× overdraw, and while digging
also 1,200–1,800 triangle strips a second (debris). Game logic was a few
percent. The fixes keep every result bit-exact:

- **CPU tile renderer** (`BADMAN_RENDERER=cpu`, Android default):
  `host/badman_gpu_cpu.cpp` runs the GPU raster's pixel program
  (`badman_gpu_shader.hlsl`) line for line on helper threads. It uses the same
  batched packets, tile bins and fixed-point coordinates as D3D11 and Vulkan,
  visits each packet's covered pixels per 16×16 tile in bin order, and writes
  guest VRAM in place. `badman_cpu_raster_tests` compares it with the software
  rasterizer over full guest RAM and VRAM: 219 random cases, every blend
  mode, lazy palette errors, the transform-cache oracle and deferred GE lists.
- **Banded software primitives:** large sprite batches and triangle strips
  split their rows across `WorkerPool` helpers. Every band samples first, then
  writes its own rows in order. Strip edges are integer equations, so a band
  may start mid-triangle. Raster tests compare banded and single-thread
  results byte for byte.
- **Exact per-draw savings:** texture coordinates are computed once per column
  and row, the per-draw vectors are reused, and camera transforms are cached
  as on the GPU path. The Android scanout thread converts the copied raw
  framebuffer.
- **Android performance hint (API 33+):** the game thread and the raster
  helpers form one `APerformanceHint` session with a 33.3 ms target. Reporting
  the game thread's CPU time made Android lower the clocks (it leaves out waits
  for the helpers), so the session reports the wall time between game frames.

Measured on the NX809J along the dig route (Training 2, continuous digging
with slimes and debris): before, 15–20 FPS while digging. After, 29.4–30.0
FPS, with at most one dropped frame in any five seconds. The phone's Game
Space overlay also shows 30 FPS.

Development switches: the `profile` setting (set through `adb`) samples the
game thread at 1 kHz into `files/logs/profile.txt` (`host/badman_profiler.*`;
fold it with any symbolizer) and enables `BADMAN_RASTER_STATS`, which logs
draws, pixels and time per primitive kind every 5 s. `BADMAN_RASTER_THREADS`
overrides the helper count (0 = none).

### In-between frames on Android (experimental, off by default)

*Smooth motion up to your screen's refresh rate* on the Android launcher
(`BADMAN_INTERPOLATION=1`) records scenes as on Windows. An OpenGL ES 3
presenter (`host/badman_android_presenter.*`) then draws them on its own
thread. It uses the same geometry, exact-frame check and pacing as the
Direct3D 11 presenter (`host/badman_scene_render.*`), with the scene shader
ported to GLSL ES. The activity asks for the display mode with the panel's
highest refresh rate and gives its surface a frame-rate vote. With the option
off, the CPU scanout is unchanged.

Measured on the NX809J (Adreno 840, Training 2, dev package
`org.psprecomp.badman.dev` installed beside the player's app):

- The scenes re-render to the exact frames: 1,049 of 1,051 matched, with about
  2 pixels differing per scene. In-between frames are drawn on every refresh.
- The panel stays at 60 Hz. The app's request for the 144 Hz mode reaches the
  display manager (vote 144, base mode 2), but in the phone's *Auto*
  refresh-rate setting the vendor controller holds the app at 60
  (`ScreenRefreshRateController_resultFps=60`).
- While the app presents at the full display rate, the system caps every CPU
  core (`scaling_max_freq` 1.23 GHz on cores 0–5 and 1.38 GHz on 6–7, against
  3.63/4.61 GHz). The game thread then needs 80 ms for the 9,500 GE draws of a
  Training 2 frame instead of 36 ms, and the game falls from 22 to 10 FPS.
  Presenting only new game frames (20–25 a second) keeps the clocks uncapped.
  Neither the 144 Hz request nor the exact-frame readback causes the cap.
- The recorder's own work is about 0.4 ms per frame, after palette hashing
  and decoding stopped catching an exception per unloaded CLUT entry.

On top of the multi-core CPU rasterizer and the performance hint session
(above), the same route with in-between frames ran at 20–27 FPS while
scrolling and digging and 30 FPS when idle. The clocks were still capped, the
presenter showed 60 frames a second, and 1,496 of 1,498 scenes matched. Without
in-between frames that build holds 29.4–30 FPS, so the option stays off by
default. Next steps: lighten the presenter thread (fewer draw calls per
refresh, no stall on the native-size readback).

Later, with the player app (`org.psprecomp.badman`) instead of the dev package,
the same phone ran the presenter at 144 frames a second: the log showed
`presents_per_second` 143.7–144.7, SurfaceFlinger's latency data for the game
surface showed a new buffer on every 144 Hz vsync, and the game held 29–30 FPS
while digging, with 3,582 of 3,584 scenes matching. CPU clocks were still below
their maximum (2.4–2.75 GHz). Once, at a level start, 393 of 895 scenes failed
the exact-frame check, and those stretches fell back to 30 FPS; the cause was
not caught. A development dump of rejected scenes (`files/logs/mismatch`, only
when that folder exists) is in place for the next occurrence.

## Local input

Keep the extracted ISO under `profiles/badman/game`, including the complete
`PSP_GAME` directory. Supply a decrypted executable at
`game/PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF` with SHA-256:

```text
722332263291bcd4a342566acb5c239e572c045d6f7e385e49ccdc66ccddbf67
```

The original ISO's `BOOT.BIN` contains only zeroes; `EBOOT.BIN` is encrypted.
Decryption is performed by an external tool, not by this profile or the framework.
No executable, game asset or decryption implementation is included here.

## Build and run on Windows

Requires Visual Studio 2022 C++ Build Tools, a Windows SDK, CMake 3.20+, and
Python 3.11+. From the repository root:

```powershell
python profiles/badman/tools/prepare_font.py
.\profiles\badman\scripts\build.ps1
.\profiles\badman\scripts\run.ps1
```

The build script compiles the native diagnostic executable and runs framework and
host tests. Generated code uses `/Od /Ob0` during bring-up; this is not a performance
build. Compiler parallelism is bounded to control memory usage.

For the player-facing Prepare/Play flow, run `BadmanLauncher.exe` from a complete
assembled source-only bundle, such as `out/badman-training-test-v5`. Keep its
Python, tools, runtime-source, config, font and launcher-ui folders beside the
launcher. A developer build falls back to the source `launcher-ui` folder.
The bare executable produced by the developer build cannot prepare a game by
itself; `build.ps1` does not assemble those companion files.

`BadmanNative.exe` accepts:

```text
BadmanNative <decrypted ELF> <extracted ISO root> [dispatch limit] [trace file] [PGF directory] [prepared font ELF]
```

An unimplemented import, missing translation, or dispatch limit returns exit code
1 and a concrete diagnostic. Exit code 0 means every guest thread exited or the
user closed the game window; it does not establish that gameplay works. The
default run script writes its HLE trace to
`profiles/badman/analysis/boot-trace.log`.

Developer-only capture can set `BADMAN_DUMP_FRAME` to a BMP output path. It copies
the current latched display buffer and logs its address, format, stride and
vblank count. `BADMAN_PRESENT_CAPTURE` names a folder: a `request.txt` there
holding a file name makes the Direct3D 11 presenter save its next presented
picture as `<name>.bmp`, whatever covers the window. `BADMAN_HEADLESS=1` suppresses the window. Headless input requires
an explicit `BADMAN_INPUT_SCRIPT` path; it does not silently supply neutral input.
Scripts contain increasing millisecond times and hexadecimal PSP button masks
without `0x`, beginning with `0 0`. Time starts at the first provider observation;
these are labelled scripted diagnostics, not evidence of keyboard interaction.

For interactive headless diagnostics, `BADMAN_INPUT_CONTROL_FILE` instead reads
one atomic command line: decimal sequence, hexadecimal button mask, decimal
pulse duration in milliseconds. Begin with `0 0 0`; increase the sequence for
each pulse. Durations are bounded to 5,000 ms and release automatically.
`BADMAN_LIVE_FRAME` publishes the real latched framebuffer every 500 ms. Optional
`BADMAN_INPUT_WATCH_WORDS` accepts up to 16 mapped, aligned hexadecimal guest
addresses separated by commas; values are logged read-only on each new command.
These diagnostic controls are separate from normal player keyboard input.

The local PGF directory defaults to the separately prepared replacement-font
directory under `analysis/firmware`. `BADMAN_FONT_DIRECTORY` changes that default;
the `PGF directory` argument overrides it per run. The mounted font directory is
read-only and confined to `flash0:/font`. No fonts are included in this source tree.

The optional, local-input font probe is:

```powershell
.\out\badman\bin\Release\BadmanFontProbe.exe `
  out/badman/font/libfont-fixed.elf profiles/badman/game `
  profiles/badman/analysis/firmware/ppsspp-replacements/flash0/font `
  profiles/badman/analysis/font-probe-trace.log
```

## Setup and local font preparation

The setup backend prepares the required font artifact from the player's own
verified ISO using external 7-Zip/pspdecrypt and the bounded relocation recipe:

```powershell
python profiles/badman/tools/setup_game.py `
  --iso 'Holy Invasion of Privacy, Badman! What Did I Do to Deserve This.iso' `
  --data-root out/setup-testdata `
  --font-directory profiles/badman/analysis/firmware/ppsspp-replacements/flash0/font `
  --trace --play
```

The schema-3 setup manifest includes a verified fixed font ELF under
`prepared/<SHA-256>/libfont-fixed.elf` and a verified locally compiled native
executable under the user's build cache; Play passes the font path explicitly. Preparation
does not require Ghidra or saved loader snapshots. `--verify-only` checks readiness
without writing or repairing artifacts. Existing matching raw game data can be
reused and upgraded; unrelated files are preserved.

For developers reproducing the original loader validation, the independent
snapshot-based path remains available:

`prepare_font.py` requires verified loader-produced snapshots; it does not decrypt
the original PRX or implement REL2. First prepare the original local module by
removing its 64-byte SCE envelope, running the external decryption utility, and
inflating its gzip result. The unpacked ELF must match `config/font_module.json`.
With the tested Ghidra/Allegrex extension and Java 21 installed:

```powershell
$fontWork = 'profiles/badman/analysis/libfont'
$headless = 'CE-Ghidra-MCP-Pack/runtime/ghidra_12.1.3_PUBLIC/support/analyzeHeadless.bat'
$env:JAVA_HOME = (Resolve-Path 'CE-Ghidra-MCP-Pack/runtime/jdk-21.0.12.1+1').Path
& $headless $fontWork LibfontPreparation09fd `
  -import "$fontWork/libfont-unpacked.elf" -loader PspElfLoader `
  -loader-imagebase 0x09FD0000 -noanalysis -scriptPath profiles/badman/tools `
  -postScript ExportRelocatedModule.java $fontWork base09fd
Get-Content "$fontWork/base09fd-snapshot.txt" -Tail 1
python profiles/badman/tools/prepare_font.py
```

Use a fresh dedicated project name. Require the final snapshot report line
`EXPORT_OK image_base=0x09FD0000 relocations=902 applied=902 segments=2`;
Ghidra can return process exit 0 after a script exception. The preparer verifies
the original input/snapshot hashes, relocated tables and BSS before producing the
fixed ELF. All snapshots and derived bytes stay ignored.

Host-only tests can be configured without commercial inputs:

```powershell
cmake -S . -B out/badman-tests -DPSPRECOMP_PROFILE=badman -DBADMAN_BUILD_NATIVE=OFF
cmake --build out/badman-tests --config Release --target badman_host_tests
ctest --test-dir out/badman-tests -C Release -R badman_host_tests --output-on-failure
```

## Translation inputs and reproduction

`config/executable.json` records the supported executable identity and addresses.
`config/code_ranges.csv` contains only addresses and lengths of instructions
defined in Ghidra. The code range map was exported using Ghidra 12.1.3 and
ghidra-allegrex v21.4, after defining these previously missed entry points:

- `0x088E88A4`: the `user_main` thread entry passed to `sceKernelCreateThread`;
- all 113 function pointers in `[0x089169C0, 0x08916B84)`, beginning at
  `0x08912C54`: static-constructor code stored in `.data`. The ELF's load segment
  is executable even though the individual `.data` section lacks its executable flag.

Run `tools/DefineBadmanEntryPoints.java` before auto-analysis, then
`tools/ExportBadmanCode.java` to export instruction runs. The current map contains
238,028 defined instructions, partitioned into 66 generated C++ units. The exporter
excludes the PSP import-stub area, whose wrappers are generated separately. Run
Ghidra auto-analysis before exporting newly defined entry points. Instruction
discovery remains incomplete until runtime coverage and indirect targets have
been reviewed.

The build invokes the generic `psp_recomp` in manual-map mode through the profile's
`tools/generate.py` adapter. The adapter groups bounded instruction runs, moves
import wrappers into one translation unit, and creates a registration unit. It
does not add title addresses to the generic framework or replace guest routines
with native approximations. Translation outputs and input hashes are recorded
under `out/badman/profiles/badman/generated/generation.json`.

To generate independently after building the framework tools:

```powershell
python profiles/badman/tools/generate.py `
  --elf profiles/badman/game/PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF `
  --recompiler out/framework/Release/psp_recomp.exe `
  --output out/badman-generated
```

Generated game translations remain in ignored build directories. Local analysis,
Ghidra databases and traces are not part of the reusable source tree.

## Host scope

The independent host implements startup SDK/compiler registration, module identity
queries, low/high/aligned user partition allocations, thread creation and argument
copying and exited-thread restart, cooperative thread selection, return/exit
handling, thread-end waits, timed delays, nested CPU interrupt masks, fixed-pool
allocation, uncontended lightweight mutexes, event flags and semaphore waits.
It also records clock
settings and exit-callback registrations, supplies host time/console services,
and exposes the runtime's mapped VRAM size/address.

The GE host executes ordered state lists, stall updates, CALL/RET and supported
SIGNAL/FINISH sequences, preserves streaming matrix state, and invokes translated
guest callbacks on an isolated guest stack. Unsupported drawing/transfer work
stops explicitly. Display APIs retain mode/framebuffer metadata, latch pending
buffers with translated vblank interrupts and waiter wakeups, and present guest VRAM in a native
Windows window. Arrow keys provide the D-pad; Z/X/A/S map to cross/circle/square/
triangle, Enter to Start, Backspace to Select, and Q/E to the shoulder buttons.

The disc host mounts the extracted `disc0:` tree read-only and implements file
open/read/seek/close plus pending async completions. Synthetic fixtures cover
those APIs, read-only stat metadata, path confinement and error output preservation.
The real game has loaded its font module and read asset files through this host.

Tests cover allocation alignment/coalescing, invalid releases, fixed-pool
exhaustion, recursive mutex state, event-flag matching/clearing, thread argument
copying, and context switches deferred by nested interrupt masks. The generation
tests check map alignment/overlap, partition sizes, and required indirect targets.

Other imports stop explicitly. Current audio, intro MPEG playback and private
native savedata support are described above; they do not imply support for every
PSP API or media format. Further drawing formats, general callbacks and writable
guest filesystem operations remain incomplete. The host uses
wall/monotonic host time and records PSP clock changes without throttling the
native CPU; cycle-accurate timing is not implemented. Some invalid-operation paths
raise diagnostics rather than returning PSP kernel error codes. The generic
decoder also leaves some trap instructions as explicit unsupported paths.

## Source provenance

Host and generation-adapter implementations were written for this profile.
Public API signatures/semantics were checked against PSPDEV's PSPSDK documentation:
<https://pspdev.github.io/pspsdk/pspthreadman_8h.html>.
PSPLibDoc was used locally to identify imported NIDs; its source and generated
lookup dataset are not bundled into the MIT implementation. Ghidra, its Allegrex
extension, and external decryption utilities remain separate local tools.
