/*
 * Lightning Returns: Final Fantasy XIII - asset streaming & stutter mod
 * Copyright (C) 2026  Dendonflo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// Diagnostic build v3: v2 proved (via true entry-to-return duration, not
// just call spacing) that FUN_00aacf10/FUN_00a2ada0/FUN_00d19a00's own
// per-call cost does NOT increase during the stutter - their call COUNT
// drops (consistent with the stutter itself), but each individual call
// still completes in the same time as baseline. So the bottleneck isn't
// inside these three leaf compute functions; something else is starving
// them of CPU. Extending the same safe duration-measurement technique to
// the layer that actually WAITS - the frame pacer and the job-dispatch
// coordinator - since a leaf compute function's duration can never
// capture time spent blocked/waiting, only a function that itself does
// the waiting can:
//
//   FUN_00ac3040 (frame pacer)         - consistently the single hottest
//                                         function in every CPU-sampling
//                                         capture across this whole
//                                         investigation
//   FUN_00a01a00 (main job dispatcher) - wakes workers, does its own
//                                         share of jobs, then WAITS for
//                                         all workers to finish
//   FUN_00a015b0 (spinlock queue pop)  - shared job-queue contention point
//
// Mechanism unchanged from v2 (return-address hijacking, calling-
// convention-agnostic, entry hijack conditional on a per-thread call-depth
// stack not being full) - see the v2 header comment history in PROGRESS.md
// for the full safety reasoning. Only the number of instrumented targets
// changed.

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <intrin.h>
#include "hook.h"

// ---- Retired-feature gates -------------------------------------------------
// Investigations that reached a conclusion. The code is kept verbatim - each
// one cost real effort and records a negative result that is worth being able
// to re-read in place - but it is compiled out, because "off by default" was
// not the same as "not there": every one of these still installed hooks,
// registered handlers or ran per-call checks regardless of its toggle.
//
// Each gate is independent so a bad build bisects by flipping ONE line rather
// than re-editing. Flip to 1 to bring a block back exactly as it was.
//
// ENABLE_GYSAHL_DIAG  - the Gysahl Green plot bug. SOLVED in the scripts
//   instead (rebuilt scr104.clb; see SCRIPTS_60FPS.md). Everything here is
//   superseded: the FaCmpFix repair, the schedule/window logging, and the
//   hardware watchpoint. Cost while off was NOT zero - seven inline hooks and
//   a front-of-chain vectored exception handler were installed
//   unconditionally, including one on FUN_009d2130, the script VM's string
//   compare, which every sfIsSameStrings in the game passes through.
//
// ENABLE_CASCADE_HUNT - the shadow cascade split distance. Six attempts,
//   ending in a proof that the target is unreachable from D3D9: the engine
//   multiplies world*lightViewProj on the CPU and uploads only the product,
//   so the cascade projection never crosses the API boundary (FEATURES.md,
//   "Attempt 6"). Covers the two logging toggles AND the two rewrite paths -
//   ShadowSplitNear/FarPct (fired correctly, no visible effect - wrong
//   constant) and NearCascadePct (visibly broke the image, reverted).
//   Does NOT cover ShadowMapRes, which is the one shadow feature that works
//   and uses a completely different mechanism (writes the engine's own
//   settings field from the monitor thread, never touches shader constants).
//
// ENABLE_SHADOW_SCALE - scaling the shadow textures behind the engine's back.
//   Superseded by ShadowMapRes, and it failed on its own terms first: the
//   surfaces grew but every calculation consuming them did not, giving
//   shadows confined to a camera-tracking square.
//
// ENABLE_TALK_TIMER   - FieldTalkManager's frame-denominated countdown. The
//   mechanism is real (0.05/frame with no delta-time term, so talk entries
//   live half as long at 60fps) but it was never connected to any bug: it is
//   not the Gysahl cause, and user testing found no effect. Retiring it also
//   removes the mod's only instruction-level patch.
// ENABLE_ALLOCATOR_WARM - allocator prefetch/warmer thread. No measurable
//   effect; user-confirmed.
//
// ENABLE_DEFER_UPLOADS  - deferred / rate-limited GPU uploads (with its
//   DeferPerFrame budget). Not merely null - user reports it made things
//   WORSE when enabled.
//
// ENABLE_MANAGED_POOL   - DEFAULT -> MANAGED texture rewrite. Redundant: once
//   ForceStdD3D9 puts the game on a non-Ex device the runtime already picks
//   the equivalent pool, and the rewrite counter stayed 0 across full sessions.
//
// ENABLE_TIMER_RES      - force 1ms timer resolution. Dead before it was ever
//   tested: the probe showed the system already at 1.0000ms.
//
// ENABLE_NV_BLUR        - NVIDIA's own HBAO+ blur, offered as a per-estimator
//   toggle so it could be judged against our a-trous one. Built faithfully
//   (radius 3, one texel spacing, their gaussian weight) and tested on both
//   estimators; user's verdict 2026-08-18, and it holds up on the mechanism:
//
//     SSAO  - clearly WORSE. SAO's noise is white grain over the whole tap
//             disk, and a fixed 3-tap radius at one texel cannot cover it.
//             Ours reaches 9/17/33/65px through a-trous levels, which is
//             what makes that grain resolve at all.
//     HBAO+ - no visible difference. Its noise is a structured 4x4 tile
//             rather than grain, so it is already resolved by the single
//             narrow pass (spread 5) our blur runs for it - the two kernels
//             are doing the same small job.
//
//   So it is worse where the blur matters and equivalent where it does not,
//   which leaves nothing for it to win. The transcription and the
//   absolute-vs-relative depth finding are the parts worth keeping - see
//   g_aoBlurNvHlsl in 25_ssao.c.
#define ENABLE_NV_BLUR        0
#define ENABLE_GYSAHL_DIAG    0
#define ENABLE_CASCADE_HUNT   0
#define ENABLE_SHADOW_SCALE   0
#define ENABLE_TALK_TIMER     0
#define ENABLE_ALLOCATOR_WARM 0
#define ENABLE_DEFER_UPLOADS  0
#define ENABLE_MANAGED_POOL   0
#define ENABLE_TIMER_RES      0
#define ENABLE_TEXTURE_POOL   0

// ---- Cleanup pass 2 (2026-08-11): diagnostics retired ---------------------
// Everything below is instrumentation that has already answered its question,
// or an experiment that reached a documented dead end. Gated rather than
// deleted, per the project rule - each is one line from coming back, and the
// reasoning for every dead end is in FEATURES.md.
//
// The frametime graph overlay is deliberately NOT here: it is the one
// diagnostic that stays, being the only honest view of the engine tick
// (RTSS graphs presents, which this game decouples from its logic tick).
//
// ENABLE_SHADER_DIAG   - shader dump, the identify walk (map + tint + stable
//                        dropdown), per-shader draw attribution, the
//                        post-shader kill list. This is what FOUND the FXAA
//                        pass and the glyph shader; keep it recoverable, it
//                        is the best tool in here for future shader work.
// ENABLE_SURFACE_DIAG  - BMP surface dumps, per-pass stage capture, the F9
//                        capture, whole-frame edge statistics, StretchRect
//                        and swap-chain probes.
// ENABLE_CUTOUT_AA     - alpha-to-coverage, sample-masked SSAA, and the
//                        blended fringe pass. All three failed (see the AA
//                        section of FEATURES.md); the bytecode transforms are
//                        worth keeping for reference.
// ENABLE_PASS_PROBE    - draw-pass render-target logging, plus the
//                        [ssaa-probe] StretchRect instrument. Re-armed
//                        2026-08-12 for the native-SSAA reconnaissance and
//                        RETIRED AGAIN 2026-08-13, its questions answered: the
//                        UI draws into the backbuffer inside DRAW_MENU (so SSAA
//                        excludes it for free), and the engine already picks
//                        LINEAR whenever a blit scales. This was also the main
//                        producer of log volume - a playthrough reached ~2 GB.
//                        The [pass]/[vp] call sites were never gated (only the
//                        toggle was), so they remain as one branch on the RT
//                        path while LogPassRts is 0.
// ENABLE_FRAMETIME_DUMP- raw frametime capture to the log.
// ENABLE_SHADOWS_OFF   - shadow-render skip (a diagnostic, never a feature).
// ENABLE_SCALING_MODE  - Graphics_Scaling writer; redundant, the game exposes
//                        it in its own options menu.
// ENABLE_SPIN_GUARD    - limiter spin high-water clamp, never confirmed.
#define ENABLE_SHADER_DIAG    0
#define ENABLE_SURFACE_DIAG   0
#define ENABLE_CUTOUT_AA      0
// RETIRED 2026-08-15 (release cleanup). Its question was answered - the
// SsaaOutputRes path is confirmed working - and it was the single largest
// producer of log volume in this project's history. Nothing in a release
// build should be paying for a per-SetRenderTarget pointer scan.
#define ENABLE_PASS_PROBE     0
#define ENABLE_FRAMETIME_DUMP 0
#define ENABLE_SHADOWS_OFF    0
#define ENABLE_SCALING_MODE   0
#define ENABLE_SPIN_GUARD     0
// ENABLE_CLAMP_DEADLINE - the limiter "seesaw aftershock" clamp. It stopped
//   the limiter chasing a stale deadline after a stall, which mattered when
//   the frame delta was the suspected cause of the 60fps logic bugs. SimDeltaFix
//   addresses that at the source (the delta quantisation itself, confirmed in
//   game on the interact prompt and stamina drain), so this is a second
//   mechanism aimed at a problem that is already fixed properly - retired
//   rather than left as a checkbox that invites double-treating it.
#define ENABLE_CLAMP_DEADLINE 0

// ENABLE_LOADER_DIAG - Cause 3 (script/class-loader) measurement, 2026-08-13.
//   Wall-clocks the outermost class load (FUN_009dff70) against its two known
//   hot loops - the native-method binder (FUN_009ddfe0) and the block-decrypt
//   caller (FUN_009fcfd0) - as a per-window [loader] line. The split decides
//   which fix ships: the memoised binder table, the decrypt fast-path, or
//   both. Costs three rare paired hooks; loads happen on first encounter of
//   each class only.
//   RETIRED 2026-08-15 (release cleanup), question answered. The four-zone
//   run measured cause 3 at ONE capture in 23 minutes of gameplay (0.3%)
//   while being 10.5% of loading stutters: it is a loading-screen
//   phenomenon and does not occur in play. Costs three paired hooks on the
//   class-load path for a number we now have. See
//   RUN_2026-08-15_FOUR_ZONES.md.
#define ENABLE_LOADER_DIAG 0

// ENABLE_D3DX_DIAG - d3dx9 call attribution (22_d3dx_diag.c), 2026-08-15.
//   After the compactor mitigation, d3dx9_43 texture work is the largest
//   untraced family in the watchdog census (~15% of captures). IAT-wraps
//   the exe's nine D3DX imports with per-call timing, a monitor line, and
//   capped per-call logs above 1ms carrying the CALLER address - answers
//   which D3DX call it is, which engine site makes it, and on which thread.
//   SUSPENDED 2026-08-15, same day it shipped: two access-violation
//   crashes at the title screen's Load Game option (Windows Event Log
//   10:58:56 / 10:59:17, EIP in unmapped heap memory both times) right
//   after this diagnostic deployed. The wrappers logged 1000+ clean calls
//   at the title screen, but Load Game is where the save-thumbnail
//   decoder would make the FIRST D3DXCreateTextureFromFileInMemoryEx
//   call, and an arity/convention slip there produces exactly the
//   observed jump-into-garbage. OFF pending the bisect run; the crash
//   logger below (ENABLE_CRASH_LOG) ships in its place so a repeat
//   names its faulting site instead of "module: unknown".
#define ENABLE_D3DX_DIAG 0

// ENABLE_CRASH_LOG - RETIRED same day (2026-08-15). Built as a crash
//   logger before discovering the mod already HAS one (ModCrashVeh /
//   ModCrashFilter in 19_boot_install.c, installed at boot). The one run
//   it flew, it found the real bug in the EXISTING logger instead: the
//   watchdog's guarded stack-scan AVs (by design, ~constant during
//   stutters) were consuming ModCrashVeh's one-shot report, which is why
//   the Load Game crash died silently. Fix applied to ModCrashVeh
//   directly (own-module EIP filter + accessing address + stack sweep);
//   the duplicate handler is gated off, kept as the record of why.
#define ENABLE_CRASH_LOG 0

// ENABLE_UPLOAD_GATE - texture-upload fast/slow census (23_upload_gate.c),
//   2026-08-15. FUN_00aa3250 already has a memcpy fast path; it is skipped
//   when the texture is non-power-of-two OR its format differs from the
//   source, and the fallback is D3DXLoadSurfaceFromMemory (~18% of watchdog
//   captures). Entry-only detour, arguments only, nothing written back -
//   deliberately NOT the IAT-wrapper approach that crashed Load Game.
//   RETIRED 2026-08-15 (release cleanup), question answered. The census
//   priced the slow path at 8ms across a whole run (1458 non-power-of-two
//   uploads, 2 format mismatches) - not a stutter source - and the d3dx
//   family it was built to explain turned out to be DDS decode during
//   LOADING, which is 0% of gameplay captures. Both hooks sit on the
//   texture-upload path, which runs ~17,000 times per run; a release build
//   should not pay that to re-derive a settled number.
#define ENABLE_UPLOAD_GATE 0

// ENABLE_AO_RECON - ambient-occlusion injection recon (24_ao_recon.c),
//   2026-08-15. Measures whether the MS_SHADOW screen-shadow buffer is a
//   bindable texture that material shaders sample during MULTI_SAMPLE, and
//   whether it survives untouched until then - the plumbing prerequisites
//   for injecting SSAO into it ("option 2"). One SetTexture hook, the
//   ultra-hot method this project has otherwise deliberately never hooked;
//   diagnostic builds only, OFF for any release.
#define ENABLE_AO_RECON 1
#if ENABLE_AO_RECON
static void InstallAoReconHook(void **vtbl);  // 24_ao_recon.c
static void AoReconTick(void);                // timeout report, monitor thread
static void AoDrawTick(void);                 // draw-level consumption check
static void AoReconReset(void);               // clear latches on device Reset
static volatile LONG g_aoTint;                // tint probe toggle (ini AoTint)
static volatile LONG g_aoDumpRequest;         // one-shot buffer dump (menu button)

// ENABLE_AO_SSAO - the actual SSAO injection (25_ssao.c), built on the recon
//   findings. Requires ENABLE_AO_RECON (uses its RT-set tracking and the
//   s14 hook point).
#define ENABLE_AO_SSAO 1
#if ENABLE_AO_SSAO
// (SsaoApply is forward-declared in 24_ao_recon.c - d3d9 types do not exist
// this early in the TU, but the tunables are plain LONGs and belong here so
// the config table in 08 can see them.)
static volatile LONG g_aoEnable = 0;          // ini AoEnable; Graphics > Ambient Occlusion
                                              //   0=off 1=SSAO (Alchemy) 2=HBAO
static volatile LONG g_aoDebug = 0;           // ini AoDebug: raw AO view
// Strength/Intensity/Radius are PER-ESTIMATOR slots ([0]=SSAO [1]=HBAO,
// user: "HBAO looks better but needs separate saved values") - the two
// estimators' scales don't translate, Intensity especially. Projection and
// BlurSharp stay shared: they describe the camera and the blur, not the
// estimator. The tuning panel re-points its sliders to the live slot.
static volatile LONG g_aoStrengthPctE[2] = { 149, 149 };  // ini AoStrengthPct / AoHbaoStrengthPct
// Every default below is its own reference's shipped value, and the two
// references do not agree - which is fine, because these are two different
// algorithms that merely share a slider.
//   SSAO  = SAO: radius 0.75m, intensity 1.0 (exponent), bias 0.02
//           (G3D AmbientOcclusionSettings constructor)
//   HBAO+ = NVIDIA: radius 2.0, intensity 1.5 (PowExponent), bias 0.1
//           (gl_ssao ssao.cpp m_tweak defaults)
// The HBAO+ radius only looks large next to SAO's: NVIDIA's RadiusToScreen
// carries an extra 0.5, so the disc it actually walks is half what the same
// number would give SAO.
static volatile LONG g_aoIntensityE[2] = { 522, 400 };    // ini AoIntensity100 / AoHbaoIntensity100
static volatile LONG g_aoRadiusE[2] = { 40, 169 };        // ini AoRadius100 / AoHbaoRadius100
// Bias x1000. The knob for false occlusion on smooth, gently curving ground
// - the HBAO talk's "low-tessellation problem", where the tangent plane does
// not match the coarse surface and the estimator invents shading that is not
// there. It is NOT the same quantity in the two estimators:
//   SSAO  (SAO): subtracted from v.n in WORLD UNITS, so it is a distance -
//                "increasing bias increases the maximum concavity that can
//                occur before AO begins". Shipped default 0.02.
//   HBAO+ (NVIDIA): subtracted from the NORMALISED n.v, a dimensionless
//                cosine, so it reads directly as an angle above the tangent
//                plane: 0.1 ~ 5.7 deg, 0.5 = 30 deg - which is the angle
//                bias the 2008 talk illustrates as its own default. Shipped
//                default 0.1, and AOMultiplier = 1/(1 - bias) compensates in
//                the shader so raising it does not merely dim the effect.
// Ceiling 950 rather than NVIDIA's 0.9999: that multiplier is 1/(1 - bias),
// and this codebase has learned twice what one INF in the shadow term does.
static volatile LONG g_aoBiasE[2] = { 101, 300 };      // ini AoBias1000 / AoHbaoBias1000
static volatile LONG g_aoProj100E[2] = { 130, 130 };  // ini AoProj100 / AoHbaoProj100
// Ceiling on the SCREEN-space sample radius, percent of screen width. The
// world-space Radius projects larger the closer geometry is, and without a
// sane ceiling near-camera pixels sample a quarter of the screen - distant
// unrelated geometry, huge variance, banding. 10 = 10% of screen width.
static volatile LONG g_aoRadiusMaxPctE[2] = { 10, 8 };
static volatile LONG g_aoTweakOpen = 0;       // SSAO tuning window (not persisted)
// ini InGameUi: 1 = ALL THREE mod surfaces (AO tuning panel, frametime graph,
// status panel) render INSIDE the frame at EndScene (26_ingame_ui.c) - no
// Win32 windows, so fullscreen cannot cover them, focus cannot be stolen, and
// window switches cost nothing. 0 = the old Win32 windows, kept as the
// fallback until the in-game path has earned trust.
//
// Renamed from AoPanelInGame once it stopped being AO-specific. That key
// existed for one afternoon of beta testing; an ini still carrying it is
// ignored and the default applies, which is the intended outcome.
static volatile LONG g_inGameUi = 1;
// ini AnisoLevel: anisotropic filtering the mod enforces at SetSamplerState,
// replacing the vanilla Texture Filtering menu (whose entire range is 1x and
// 8x - see the header block of 08d_texfilter.c for the disassembly). Values:
// 0 = leave the engine alone, 1 = off (trilinear), 2/4/8/16 = that level,
// clamped to D3DCAPS9.MaxAnisotropy.
//
// SHIPS AT 16 (2026-08-19, confirmed working in game). Anisotropic filtering
// is close to free on any GPU that can run this game at all - it costs
// bandwidth only on the samples it actually takes, and it takes the extra
// ones only where the surface is at a grazing angle. Against that, the
// vanilla ceiling is 8x and vanilla "Standard" is anisotropy OFF, so the
// stock game ships noticeably worse ground and wall detail than the hardware
// has cost-free.
//
// 0 briefly WAS the default, for exactly one build: the census in 08d cannot
// measure the engine's own filtering from a session where we have already
// overwritten it, so that build's default run was the baseline and the menu
// produced the comparison. That run is done, so 0 is now just "leave the
// engine alone" - reachable from the ini, no longer in the menu.
static volatile LONG g_anisoLevel = 16;
// ini ForceTrilinear: rewrite MIPFILTER POINT -> LINEAR. Point mip filtering
// is what produces a visible arc on the ground where one mip level ends and
// the next begins, sliding with the camera. Ships OFF because whether this
// engine ever asks for POINT is exactly what the census is measuring - a fix
// aimed at a defect nobody has confirmed is how you ship a regression.
static volatile LONG g_forceTrilinear = 0;
// ini MipBiasMode: clamp how far NEGATIVE the engine is allowed to push
// MIPMAPLODBIAS. 0 = off (the engine's own value stands), 1 = floor at 0.0
// (neutral), 2 = floor at -0.5, 3 = floor at -1.0.
//
// Why this exists. A negative LOD bias tells the sampler to pick a SHARPER
// mip than the pixel footprint justifies, which is a shimmer generator by
// construction - and a distance-weighted one, because up close the selection
// is already near level 0 and the bias clamps out, while far away it sits
// mid-chain and the bias is fully active. The census found the engine doing
// this constantly: 1,319,073 negative writes against 30,036 positive, with
// the range reaching -5.00. The user then independently reported distant
// terrain in the Wildlands crawling like "a texture too detailed for the
// current res", improving with both anisotropy and SSAA - which is the
// signature of undersampling, not of normal-map or specular aliasing.
//
// A FLOOR, not an override: only the negative side is touched. The positive
// biases are deliberate blur effects (30k of them, on stages 0 and 1 only,
// reaching +5.0) and nothing writes +5.0 by accident - overriding those to 0
// would sharpen something the artist meant to be soft.
//
// SHIPS ON at floor 0.0 (2026-08-19). It went out as an A/B instrument with
// four positions and the user confirmed by eye that clamping fixes the
// Wildlands crawl, so the menu is now the two positions that matter - Off
// (the engine's bias) and On (neutral 0.0) - and On is the default.
//
// Modes 2 and 3 (floors -0.5 and -1.0) stay REACHABLE FROM THE INI. They are
// not dead: a mild negative bias is legitimate practice once anisotropic
// filtering is paying for the extra samples, which it now is at 16x, so they
// are the natural knob for anyone who finds neutral a touch soft. Same
// treatment as ForceTrilinear and ScreenShadowResPct - out of the menu,
// still in the file.
static volatile LONG g_mipBiasMode = 1;
// Custom internal resolution (0/0 = not in force). The game's own Resolution
// menu is eleven hardcoded 16:9 handlers topping out at 3840x2160, with no
// display enumeration anywhere (POST_BETA_PLAN, "Resolution options" - the
// whole mechanism is mapped there). These two let the mod offer entries the
// game does not have, up to 8K: rendering above the desktop is supersampling,
// and for DLDSR users the DESKTOP ITSELF is 5K+, so the vanilla list cannot
// even reach native.
//
// Values write straight into the engine's settings fields (+0x10/+0x14 on
// the 0511558c object) and the engine's own screen-set rebuild detector does
// the apply - the same "write the value the engine reads" approach as
// ShadowMapRes. Persistence is OURS: the game's Configuration.ini serialiser
// walks a static table of its eleven, finds no checked handler for a custom
// value, and simply omits the Graphics_Resolution line (verified by
// disassembly of FUN_00ac4e60) - so the game forgets the setting across a
// restart and this pair is what remembers it.
static volatile LONG g_customResW = 0;
static volatile LONG g_customResH = 0;
// Process-start anchor for the custom-resolution grace window, captured in
// LoadConfig (which runs from DllMain-time setup, long before any menu
// exists). The grace was originally anchored to the FIRST MENU BUILD, which
// is wrong in exactly one common case: the menu does not exist in
// fullscreen, so booting fullscreen and switching to windowed minutes later
// started the "boot" grace at the switch - and a vanilla resolution clicked
// within 15s of it was snapped back to the custom one, leaving both entries
// visibly checked (user-reported 2026-08-20). The parser this grace exists
// to out-wait runs once, at PROCESS start, so that is what it must be
// anchored to.
static DWORD g_bootTickMs = 0;
static volatile LONG g_aoRawView = 0;         // true-raw AO over the frame (not persisted)
// Which stage the raw view shows: 0 = the AO term, 1 = depth, 2 = the
// reconstructed normal, 3 = the raw occlusion sum. Diagnostic, not
// persisted (the armed-toggle lesson).
static volatile LONG g_aoDebugStage = 0;
// One-shot: dump RT A at the RAW-VIEW call site, where it holds the stage
// image (normals/depth/occlusion) rather than the AO term. The normal-stage
// picture is written to an 8-bit target, so smooth normals would band in
// the VIEW regardless - only the numbers can tell a real staircase from a
// quantisation artifact of the visualisation.
static volatile LONG g_aoStageDumpRequest;

// ENABLE_FOV_PROBE - one-run calibration for AoProj100 (24_ao_recon.c).
// The projection scale is NOT recoverable from the depth buffer: for any
// plane 1/z comes out linear in screen position whatever scale you assume,
// so no amount of looking at depth or normals constrains it. It has to
// come from the engine.
//
// Static RE got most of the way: the script natives Camera::getCurrentFov
// (0x009AA700) and Camera::getFieldDefaultFov (0x009AA810) show the camera
// stores HALF-fov in RADIANS - the native multiplies by 180/pi and doubles
// it - with a field default of 80 degrees and a 60 degree fallback. What
// that cannot settle is whether 80 is vertical or horizontal, which is a
// 1.8x difference in the answer (119 vs 212).
//
// So take it from the matrix instead. The engine uploads a constant it
// names "viewProjMatrix", and for M = view * projection with an orthonormal
// view part, the projection scales are exactly the norms of M's first two
// columns - the rotation contributes nothing to a column norm. That yields
// projX and projY directly, no FOV convention to guess at.
// MEASURED 2026-08-16: projX=1.7840 projY=3.1716, ratio 0.5625 = exactly
// 1080/1920, so this is unquestionably the perspective matrix. fovY=35.00
// degrees - which means the engine's 80 degree "field default" is some
// other camera mode or a horizontal figure, and guessing from it would
// have been wrong either way. AoProj100 = 317, against a default of 130
// and a hand-tuned 115: the reconstruction was skewed by 2.75x all along.
//
// So it is measured, always, and there is no setting: a projection scale
// has one correct value and every other value is simply wrong, so a
// control could only be used to break the reconstruction - which is what
// it had been doing at 115 against a true 317. The measurement is
// re-asserted every frame, so cutscene cameras and any lens change are
// followed automatically. AoProj100 / AoHbaoProj100 remain in the ini
// only as the value to start from before the first measurement lands.
#define ENABLE_FOV_PROBE 1
static volatile LONG g_fovProbeDone;
static volatile LONG g_aoProjMeasured = 0;    // last measured cot(fovY/2)*100
static void InstallFovProbe(void **vtbl);   // 24_ao_recon.c
// Black-model bisect (2026-08-16): blackness FOLLOWS THE NEWEST-LOADED
// MODEL (user-observed: switching weapons blackens the newly shown one),
// while the dumped composite is unremarkable over the black object - so
// the mechanism is a per-draw side effect of our passes, not buffer
// content. Each level removes one stage off the END of the pipeline; the
// first level where models stop going black names the culprit:
//   0 full | 1 no composite write | 2 +no StretchRect snapshot
//   3 +no blur draws | 4 +no estimator draw (setup/retargets only)
// DELIBERATELY not persisted (the armed-diagnostic-poisons-later-launches
// lesson from v24): every session starts at 0.
static volatile LONG g_aoBisect = 0;
// Flat-write test: combine writes eng * (value/100) with no estimator
// influence. 0 = off. Session-only, same non-persistence rule as bisect.
static volatile LONG g_aoFlatTest = 0;
static volatile LONG g_aoBlur = 1;            // ini AoBlur: bilateral blur (the noise cure)
static volatile LONG g_aoBlurSharpE[2] = { 681, 915 };  // ini AoBlurSharp / AoHbaoBlurSharp
#if ENABLE_NV_BLUR
// ini AoBlurMode / AoHbaoBlurMode: 0 = our a-trous bilateral, 1 = NVIDIA's own
// HBAO+ blur (fixed 3-tap radius, one texel spacing, separable X then Y).
// RETIRED - see ENABLE_NV_BLUR for the comparison that ended it. Old inis
// carrying either key are ignored, which is the intended outcome.
static volatile LONG g_aoBlurModeE[2] = { 0, 0 };
#endif
// ini AoRespectFloor: stay inside the engine's [0.5..1] shadow envelope.
// Ships as 0 since 1.1. At 1 the composite is clamped at the engine's own 0.5
// floor, which caps AO at half strength in lit areas and cancels it almost
// entirely in engine-shadowed ones (eng ~ 0.5, so any AO lands below the floor
// and is clamped straight back) - the "cannot make it strong enough" reports.
// The tuned defaults 1.0 shipped were calibrated with this at 0, so a 1.0
// install could not reproduce the look those numbers describe. Existing inis
// are moved by the v2 migration in 08_config_persist.c.
//
// The residual risk it guards against is a material that decodes the [0.5..1]
// envelope rendering sub-floor pixels hard black. That was the 2026-08-16 menu
// shield, which was later root-caused to the state block instead - weeks of
// play at 0 since, with no material artifact. Kept as the escape hatch.
static volatile LONG g_aoRespectFloor = 0;
// AO buffer resolution. The DIVISOR is applied to the DISPLAY resolution,
// not to the engine's internal one: at 4K with SSAA x2 the composite is 8K,
// and AO at 8K is pure waste (the effect is low-frequency and gets blurred
// anyway). AoSsaaIndep=0 restores the old behaviour of following the
// composite, for anyone who actually wants supersampled AO.
// Half by default. HBAO+'s jitter is a structured 4x4 interleaved tile
// rather than white noise, so a half-resolution buffer still resolves
// cleanly once blurred - verified in play - and it is the cheaper half of
// the cost. Note this is a deliberate departure from HBAO+'s own spec, which
// mandates full resolution to minimise flicker; the option is still there
// for anyone who wants it.
static volatile LONG g_aoResDiv = 2;          // ini AoResDiv: 1 / 2 / 4 / 8
static volatile LONG g_aoSsaaIndep = 1;       // ini AoSsaaIndep: divide SSAA out
// Upsample filter for reduced-resolution AO. 1 = depth-aware 4-tap
// (default), 0 = plain hardware bilinear. The depth-aware one used to
// stripe at exactly 1/2; that was the same texel-boundary misalignment as
// the estimator's normals, and it is fixed - see the note at UpTap. Keep
// the toggle: if striping ever returns at Half, this is the first thing
// to flip.
static volatile LONG g_aoUpsampleDepth = 1;
// AO quality tier RETIRED as a setting 2026-08-16 (user: "no more option,
// locked to quality"). SsaoApply hardcodes the High tier; the Low/Medium
// tap tables survive in 25_ssao.c so a performance tier can come back as a
// one-line change. AO Resolution remains the performance lever, which is
// the one that actually moves frame time.
// Live AO buffer state, defined in 25_ssao.c - tentative definitions here so
// the status panel (part 10) can read them despite coming earlier in the TU.
static LONG g_aoRtW, g_aoRtH;
static volatile LONG g_ssaoDraws;
static volatile LONG g_aoBlurPassesE[2] = { 4, 1 };   // ini AoBlurPasses / AoHbaoBlurPasses
static volatile LONG g_aoBlurStep100E[2] = { 75, 5 };  // ini AoBlurStep100 / AoHbaoBlurStep100
static void SsaoReleaseRts(void);             // 25_ssao.c - the blur RT pair is
                                              //   D3DPOOL_DEFAULT; AoReconReset calls this
#endif
#endif

// ENABLE_CUTSCENE_DIAG - one-run correlation aid for the cutscene-aware
//   shadow-distance revert (21_cutscene_shadow.c). Logs a small window of
//   CinemaController fields whenever any of them changes, so the field/bit
//   that means "a cutscene is playing" can be identified from a single run
//   instead of guessed from decompilation. Output is capped; turn back to 0
//   once CutsceneFlagMask is set in the ini.
//   ANSWERED 2026-08-14. Pass 1 (8 controller fields) found the inverse of
//   the assumption - everything froze for the whole cutscene - so pass 2
//   widened to the three sub-objects the ctor allocates and muted per-field
//   churn. That found CinemaController+0x5c, mask 1: four transitions in an
//   entire session, no noise, high for exactly the cutscene's frame range.
//   Back to 0 - the shipping [cutscene] ENTER/EXIT lines show the feature
//   working without the 300-line field dump.
#define ENABLE_CUTSCENE_DIAG 0

// ENABLE_DEBUG_MENU - attempt to re-enable the game's retail-dormant debug
//   menu. SEPARATE PROJECT from the stutter/graphics mod (DEBUG_MENU.md).
//
//   OFF, AND THE PREMISE IS DISPROVED (2026-08-14, experiment 1).
//   The poke worked mechanically - the log confirms all three writes landed
//   and STUCK (one apply, no re-assert) - and nothing changed in game.
//   Follow-up analysis found the reason: DAT_024c3d74 is NOT the debug menu
//   manager. Its vtable is white::actor::ActorInterface and its per-frame
//   virtual leads into ScenePathSearch.cpp, so this code was setting debug
//   bits on the SCENE/ACTOR manager. Whatever INTERVAL_DEBUG_MENU_WHITE's
//   handler forwards to, it is not a menu.
//   Worse for the whole idea: the page-registration layer is gone. Nearly
//   every page-id string ("debug_menu_common", "debug_menu_field",
//   "debug_menu_battle", ...) has ZERO references anywhere in the binary
//   (raw pointer search, not just Ghidra), and the DebugMenuPage vtables are
//   referenced only by their own destructors - orphaned vtables the linker
//   kept for RTTI. Flag flipping cannot revive what is never constructed.
//   Left compiled-out rather than deleted, per project convention.
#define ENABLE_DEBUG_MENU 0

// ENABLE_GUI_PANEL - the standalone "LR Stutter Fix - Debug Panel" window.
//   DEPRECATED 2026-08-11: superseded by the game-menu integration
//   (ENABLE_GAME_MENU), which puts the options in the game's own menu bar.
//   New options go THERE, not here. Gated rather than deleted - it was the
//   mod's only UI for most of this investigation and still documents every
//   control that ever existed, including the diagnostic dropdowns that only
//   make sense with ENABLE_SHADER_DIAG on. Turning it back to 1 restores the
//   window as it was; it no longer drives the frametime overlay (OverlayThread
//   owns that), so the two cannot fight.
#define ENABLE_GUI_PANEL      0

// ENABLE_TEXTURE_POOL - reuse released textures instead of destroying them.
//   Rejected on measurement AND on correctness: no throughput win, and it
//   produced visible corruption (a character face) when enabled together with
//   DiscardFix. Retired rather than left as an unticked checkbox because a
//   toggle that corrupts the image is a trap sitting in the GUI. It also puts
//   a branch on IDirect3DTexture9::Release, which every SetTexture hits.
//
// NOT retired despite a null measurement: LoaderThrottle. The A/B here came
// back inside noise, but beta testers report a possible slight effect on DX9.
// Unresolved, so it stays until a proper A/B settles it - a null result from
// one machine is not the same as no effect.

static void LogLine(const char *msg);
// ---- Log file size control -----------------------------------------------
// A playthrough here produced a ~2 GB version_hook.log. Two independent
// bounds now: the file is recreated per run (LogAppend=1 restores appending
// for cross-run debugging), and growth within a run is capped.
// Release builds write a short log: version, settings, failures. 1 restores
// every diagnostic line ever added, which is what a bug report wants. Must be
// declared HERE rather than beside the other config entries: LogLine lives in
// 06_io_alloc.c and this is one translation unit, so the flag has to exist
// before the file that reads it. See LogLineWanted().
static volatile LONG g_logVerbose = 0;
static volatile LONG g_logMaxMB = 64;   // 0 = unlimited
// 0 = flush only during boot (shipping). 1 = flush every line, so a crash
// leaves its last lines on disk instead of losing them in the buffer.
//
// DEFAULT 0 FOR A MEASURED REASON, not caution: per-line fflush is a
// synchronous disk write inside a lock the MAIN THREAD also takes, and with a
// low watchdog threshold it wrote ~75 flushed lines/second - a stack-walk
// capture found the main thread parked in KERNELBASE on 42% of slow frames,
// i.e. the instrument was manufacturing the stutter it existed to measure.
// Turn it on to catch a crash, turn it off to measure anything.
static volatile LONG g_logFlushAlways = 0;
static volatile LONG g_logAppend = 0;   // persisted for visibility; the real
                                        // read happens in LogAppendRequested()
static LONGLONG g_logBytes = 0;
static volatile LONG g_logCapped = 0;
// Set once the startup config rewrite has happened (monitor thread, not
// startup - see InstallPrefetchHook).
static volatile LONG g_configRewritten = 0;
// ---- Shipping defaults (2026-08-11) --------------------------------------
// These initialisers ARE the shipping configuration: a fresh install has no
// ini, so whatever is here is what a new user gets. Confirmed fixes default
// ON; anything unproven or diagnostic defaults OFF.
static volatile LONG g_discardFixEnabled = 1;   // confirmed: cause 1, LockRect
// g_discardMip0Enabled REMOVED. It allowed DISCARD on level 0 of mipmapped
// textures; measured at 166us/call against 170us with it off - i.e. nothing,
// because only ~5.5% of mip locks are level 0. Visually safe but pointless,
// so it is deleted rather than left as a dead toggle. See PROGRESS.md
// ("Level-0-only discard") for the full measurement.
// All three staging redirects default ON: each was measured, not guessed.
// Texture LockRect 170us -> 64us; Surface::LockRect was 262.7us/call before
// it existed; CubeTexture::LockRect was CONFIRMED as the traversal-stutter
// trigger (169.7us/call across 4,782 calls = 38% of a 2.12s stall). They also
// cover the buffer-reuse hazard that GpuSyncSkip removes the fence for.
static volatile LONG g_stagingUploadEnabled = 1;
// Same staging redirect as StagingUpload, but for IDirect3DSurface9::LockRect
// - a separate vtable the texture-level fix never covered. Measured at
// 262.7us/call (vs 64us for the staging-fixed texture path) before this
// existed. Separate toggle so it can be A/B'd and reverted independently.
static volatile LONG g_stagingSurfaceEnabled = 1;
// Cube textures: CONFIRMED the traversal-stutter trigger. Measured at
// 169.7us/call across 4,782 calls (0.811s of a 2.12s total stall = 38%),
// with the exact signature of the original bug: DEFAULT pool, DYNAMIC usage,
// full rect, flags=0 (no DISCARD). Separate toggle so it can be isolated.
static volatile LONG g_stagingCubeEnabled = 1;
// Graph overlay visibility - a UI feature rather than a fix, but it rides
// the same table so it gets a checkbox and config persistence for free.
static volatile LONG g_overlayEnabled = 0;
// Status panel: every mod setting next to what is ACTUALLY applied right now,
// including the live cutscene/gameplay state. Separate window from the
// frametime graph so both can be up at once - they answer different questions
// ("is it stuttering" vs "is it doing what I set"). Off by default; it is a
// diagnostic surface, not a feature to greet new users with.
static volatile LONG g_statusEnabled = 0;
// ONE threshold drives both the stutter watchdog and the graph's colouring,
// so the detector and the visualisation can never disagree about what counts
// as a slow frame. Default 16700us = anything failing 60fps. Raise it for
// 60fps-locked runs (where every frame legitimately sits near 16.7ms and
// would otherwise paint the whole graph amber).
// DEFAULT 1,000,000us = 1 second: effectively "never". The watchdog exists to
// catch stutters during investigation, and a shipping build should not be
// suspending the main thread to walk stacks behind the player's back. Set it
// to something real (16700 = anything failing 60fps) only while hunting.
static volatile LONG g_stutterThresholdUsec = 1000000;
// g_targetFpsX100: 0 = unlocked (near-1ms target, original UnlockFramerate
// behaviour). >0 = lock to that exact rate via the SAME internal mechanism
// the game already uses (fixes the 59.94 imprecision), e.g. 6000 = 60.00fps.
// See ApplyFramerateUnlock for the known CPU-cost tradeoff: this reuses the
// engine's own Sleep(0)/Sleep(1) spin loop, so it reintroduces essentially
// the same ~31% main-thread cost UnlockFramerate=1 removed. Precision and
// CPU cost are separate axes here - the spin burns the same slack regardless
// of target value. Also unproven against the frame-to-frame JITTER symptom
// (distinct from the average-rate bug RTSS already fixes) - deploy as a
// testable experiment, not a presumed fix.
// DEFAULT 6000 = 60.00fps. The stock limiter runs at 59.94, which is what
// every one of the "60fps" logic bugs is actually keyed to; locking to a
// clean 60 with UnlockFramerate on uses the game's own limiter at an exact
// rate. 0 (fully unlocked) is available but pushes the sim delta far outside
// anything the engine was tuned for.
static volatile LONG g_targetFpsX100 = 6000;
// ini ForceDynamicFramerate: put the ENGINE's own framerate mode back to
// Dynamic ("Variable") once per session, if it is sitting on Fixed
// ("Stability"). Fixed halves the mod limiter's effective target, and since
// the mod replaces the vanilla FrameRate popup entirely there is no longer a
// menu entry that could explain or undo it - the symptom reads as "the mod
// stopped working". Users mostly land on Fixed via the game's own reset to
// low settings after an unclean exit (Windows users get it rewritten by Nova
// Launcher; Linux users do not, and are hit hardest).
//
// Default ON. Kept as a key rather than made unconditional because it is the
// first thing the mod WRITES back into the engine's own settings at boot, so
// there has to be a way to switch it off without a new build if it ever
// misbehaves. See GameMenuForceDynamicFps in 09_game_menu.c.
static volatile LONG g_forceDynamicFps = 1;
// Fix for the "seesaw aftershock": after a stall, the limiter's accumulated
// deadline (decompiled in FUN_00ac3040) computes max(one interval, actual
// elapsed rounded to whole intervals) as the increment to the OLD deadline -
// so a big stall produces a NEW deadline close to "now", and the VERY NEXT
// frame's wait can be skipped entirely, producing a fast/short frame right
// after the hitch. That short frame is what desyncs frame-dependent
// gameplay values (interact prompt reset, stamina) for one extra frame.
// Standard limiter fix: don't catch up. Clamp the deadline to (now+interval)
// whenever it has fallen more than one interval behind, so a stall costs
// exactly one bad frame with nothing chasing it. See ClampAc3040Deadline.
static volatile LONG g_clampDeadlineEnabled = 0;
// Deferred GPU uploads. UpdateSurface costs ~0us on the CPU because it only
// QUEUES a GPU copy - but a chunk load dumps hundreds of copies into the
// command stream at once, and when the GPU falls behind the driver blocks
// the CPU (the AMDXN32 / ZwWaitForAlertByThreadId stalls that dominate what
// is left). Measured: 781MB of uploads per run, 100% issued from the main
// thread, while actual memcpy cost is only ~0.1s - so volume is a PROXY for
// chunk-load activity, and the real cost is GPU-side scheduling.
//
// Unlike throttling the main thread (which just moves the stall into the
// frame we are protecting), this spreads the GPU work without blocking
// anyone: the engine's memcpy still completes immediately, the copy to VRAM
// is simply issued over the next few frames.
//
// Tradeoff: a texture can reach the GPU a frame or two late, so it may
// visibly pop in. Rate is tunable to find where "no stutter" meets "no
// visible popping".
static volatile LONG g_deferUploadsEnabled = 0;
static volatile LONG g_deferPerFrame = 12;
static void DrainStagedUploads(void);
static void ApplyCascadeSplitSource(void);   // per-frame; see the split-source block
static void CutsceneDetectTick(void);        // per-frame; see 21_cutscene_shadow.c
static void LogFlushNow(void);               // defined in 19_boot_install.c
#if ENABLE_CRASH_LOG
static void InstallCrashLogVeh(void);        // 22_d3dx_diag.c (crash logger half)
#endif
// Texture pool: when a poolable texture's game-visible refcount reaches
// zero, keep the underlying D3D object alive instead of letting it be
// destroyed, and hand it back out on a later CreateTexture call with
// matching Width/Height/Levels/Format/Pool/Usage instead of asking the
// driver for a fresh allocation. Targets two confirmed findings at once:
// the creation-count correlation with slow frames (1.68-2.62x, see
// PROGRESS.md "Texture-pool question ANSWERED") and the 8.8ms CreateTexture
// cost measured for a 64-byte texture (driver GPU-heap allocation, not data
// size - a repeat-shaped allocation the driver has seen before should be
// cheap). See the "Texture pool" section further down for the full design
// and the DISCARD-fix interaction it has to account for.
static volatile LONG g_texturePoolEnabled = 0;
