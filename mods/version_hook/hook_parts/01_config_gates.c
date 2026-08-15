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
static volatile LONG g_aoDebug = 0;           // ini AoDebug: raw AO view
static volatile LONG g_aoStrengthPct = 100;   // ini AoStrengthPct (user: "very aggressive")
static volatile LONG g_aoIntensity100 = 250;  // ini AoIntensity100: estimator gain x100
static volatile LONG g_aoRadius100 = 60;      // ini AoRadius100 (0.6 units - eyeball)
static volatile LONG g_aoProj100 = 130;       // ini AoProj100 (cot(fovY/2)x100 - eyeball)
static volatile LONG g_aoTweakOpen = 0;       // SSAO tuning window (not persisted)
static volatile LONG g_aoRawView = 0;         // true-raw AO over the frame (not persisted)
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
