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
// ---- D3D9Ex -> plain D3D9 + MANAGED pool (the HelixMod approach) ----------
// Measured facts this is built on, all from our own logs:
//   - the game creates its device via CreateDeviceEx (D3D9Ex), resolving
//     Direct3DCreate9Ex dynamically through GetProcAddress (71 sightings);
//   - it ALSO statically imports plain Direct3DCreate9 (our IAT patch on it
//     succeeds) but has never once called it - the classic "try Ex, fall
//     back to plain" pattern from the XP-compat era;
//   - every texture it creates is pool=0 (DEFAULT) or pool=2 (SYSTEMMEM).
//     Zero MANAGED, which is expected: D3D9Ex FORBIDS D3DPOOL_MANAGED.
//
// That last point is the whole prize. Under D3D9Ex a DEFAULT texture must be
// locked against live GPU memory, which is exactly the sync stall this
// project has spent its entire life working around (the SYSTEMMEM staging
// redirect is a hand-rolled substitute for it). D3DPOOL_MANAGED gets that
// for free and does it properly: the runtime keeps its own system-memory
// copy, every Lock hits that copy with no GPU involvement at all, and the
// runtime schedules the VRAM upload itself. It is the same idea as our
// staging fix, implemented one layer down by code that can actually see the
// GPU's state.
//
// ForceStdD3D9 makes GetProcAddress("Direct3DCreate9Ex") return NULL so the
// game takes its own fallback path to plain D3D9. Whether that path still
// works after 10 years of never being exercised is exactly the unknown, and
// the reason this is one cheap toggle rather than a hand-written
// IDirect3D9Ex->IDirect3D9 shim.
//
// ManagedPool rewrites DEFAULT -> MANAGED at CreateTexture. Gated on
// ForceStdD3D9 because MANAGED on an Ex device fails outright, and it also
// falls back to the original parameters if the MANAGED create fails for any
// other reason - so a bad guess costs a log line, not a broken texture.
//
// Both default OFF. If ForceStdD3D9 breaks startup, the game will not be
// running to un-tick the box, so it must be fixed by editing
// version_hook_config.ini directly (safe while the game is closed).
static volatile LONG g_forceStdD3D9 = 1;
static volatile LONG g_managedPoolEnabled = 0;
// ---- HD GUI mod interop (FF13HDMod's version.dll) --------------------------
// The staging redirect has a side effect on anyone who READS textures: the
// engine's pixels land in our SYSTEMMEM staging copy and reach the real
// texture as a GPU-side UpdateSurface, so the real texture's own lockable
// memory never holds the image. The HD GUI mod identifies textures exactly
// that way (READONLY LockRect + FNV-1a at first bind), so with StagingUpload
// on, its every lookup hashes stale garbage and silently misses — HD
// replacements vanish while everything else keeps working, in BOTH D3D9 and
// D3D9Ex modes.
//
// Fix: hand it the bytes at the only moment they exist CPU-side — right
// after the engine finishes writing the staging texture (HookedTexUnlockRect).
// The HD mod exports HDTex_NotifyTextureData for this; discovery is one
// GetProcAddress pair at startup, so there is no configuration and no
// dependency in either direction (either mod alone: one null check). Cost on
// this side is a SYSTEMMEM re-lock (no GPU sync by construction) plus the
// call; the HD mod copies only textures it has never seen, so its memcpy
// happens once per texture, not per write.
static volatile LONG g_hdTexPushEnabled = 1;
typedef unsigned long (__cdecl *PFN_HDTexInteropVersion)(void);
typedef void (__cdecl *PFN_HDTexNotifyTextureData)(void *tex, unsigned level,
                                                   const void *bits, unsigned pitch,
                                                   unsigned w, unsigned h,
                                                   unsigned d3dFormat);
static PFN_HDTexNotifyTextureData g_hdTexNotify = NULL;
static volatile LONG g_hdTexPushCount = 0;

static void DetectHdTexInterop(void)
{
    // version.dll is a static import of LRFF13.exe (the HD mod ships as a
    // version.dll proxy), so if it is installed it is already loaded — no
    // LoadLibrary, no load-order dependency. The System32 version.dll that
    // proxy chain-loads shares the basename, but GetModuleHandle returns the
    // first module in load order (the exe's own import, i.e. the game-dir
    // proxy) — and the export check below makes even that assumption safe:
    // no HDTex_* exports, no interop.
    PFN_HDTexInteropVersion ver;
    PFN_HDTexNotifyTextureData notify;
    unsigned long abi;
    HMODULE hd = GetModuleHandleW(L"version.dll");
    if (!hd) return;   // HD mod not installed — nothing to do, silently
    ver    = (PFN_HDTexInteropVersion)GetProcAddress(hd, "HDTex_InteropVersion");
    notify = (PFN_HDTexNotifyTextureData)GetProcAddress(hd, "HDTex_NotifyTextureData");
    if (!ver || !notify) {
        LogLine("[hdtex] version.dll loaded but exports no interop - HD GUI mod absent or predates it");
        return;
    }
    abi = ver();
    if (abi != 1) {
        char l[128];
        sprintf(l, "[hdtex] interop ABI mismatch (theirs %lu, ours 1) - push disabled", abi);
        LogLine(l);
        return;
    }
    g_hdTexNotify = notify;
    LogLine("[hdtex] HD GUI mod detected - pushing staged texture uploads to it "
            "(its own LockRect hashing cannot see staged content)");
}
// ---- Third-party d3d9.dll (DXVK / ReShade / wrapper) awareness ------------
// Set by ProbeD3D9ForVtable once the module backing d3d9.dll is known.
// g_d3d9IsThirdParty means the loaded d3d9.dll did NOT come from System32;
// g_d3d9IsDxvk additionally matched DXVK's version resource.
//
// Why this has to be known: HookDeviceVtable installs the generic
// return-address-hijacking timing thunks on ~26 device methods of a THROWAWAY
// probe device, on the assumption - true for Microsoft's d3d9.dll, and
// verified by this project's own logs, where those slots have never produced
// a single sample - that the probe device's vtable is a DIFFERENT class from
// the game's. Native d3d9 splits device classes by vertex-processing mode
// (the probe asks for SOFTWARE, the game uses HARDWARE) and by Ex-ness, so
// the two never share.
//
// DXVK has exactly ONE device class (D3D9DeviceEx) for every combination.
// Under DXVK the probe therefore patches the GAME'S live device vtable, which
// (a) puts 26 never-once-executed hijack thunks on the hot draw path
// (DrawPrimitive/BeginScene/Clear/Present/Reset...) and (b) collides head-on
// with HookRealDevicePresent, which patches ~15 of the SAME slots with real
// replacement functions and captures whatever it finds there as "the
// original". ProbeDeviceThunks exists so that can be A/B'd rather than
// assumed - see the gate in HookDeviceVtable.
static volatile LONG g_d3d9IsThirdParty = 0;
static volatile LONG g_d3d9IsDxvk = 0;
// -1 = auto (on for a System32 d3d9.dll, off for any wrapper). 0/1 force.
static volatile LONG g_probeDeviceThunks = -1;
static volatile LONG g_managedRewriteCount = 0;
static volatile LONG g_managedRewriteFail = 0;
// Direct answer to "does the game ever ask for DEFAULT under ForceStdD3D9,
// anywhere, not just on the one route already tested". Indexed by the
// requested D3DPOOL value BEFORE any ManagedPool rewrite touches it
// (DEFAULT=0, MANAGED=1, SYSTEMMEM=2, SCRATCH=3) - unconditional, every
// CreateTexture call, not just the slow ones. A histogram answers "100% vs
// mostly" where the earlier SLOW-CreateTexture-only logging could not: that
// path only ever fires above the 1000us threshold, so it can prove DEFAULT
// pool is EXPENSIVE but not that DEFAULT pool is ABSENT.
static volatile LONG g_texPoolRequested[4] = { 0, 0, 0, 0 };
// Tentative definition - the real one sits next to ReportAndApplyTimerResolution
// further down, but g_toggles[] above needs its address. Same pattern already
// used for the census/D3D9 counters.
static volatile LONG g_timerResEnabled;
// Render-target texture inventory counter, same pattern: written by
// HookedCreateTexture, which sits earlier in the file than the RT probe block
// that logically owns it.
static volatile LONG g_rtTexLogged;
// Tentative def: g_toggles[] above needs its address, the hook that uses it
// lives with the other real-device hooks further down.
static volatile LONG g_logShaderConsts;
// Tentative def: the monitor thread resets this to restart a capture window,
// and it sits earlier in the file than the constant probe that owns it.
static volatile LONG g_constSeq;
// Tentative def: g_numerics[] needs the address, the hook that uses it lives
// with the real-device hooks further down.
static volatile LONG g_nearCascadePct;
// Tentative def: g_toggles[] needs the address, the hook is further down.
static volatile LONG g_traceSplitCall;
// Default ON. Confirmed in game to fix BOTH the interact prompt failing above
// 59.94fps AND the stamina bar not draining - see the SIM DELTA block. The
// detour still checks this flag every call so it stays hot-toggleable for A/B.
static volatile LONG g_simDeltaFix = 1;
// Tentative def: g_toggles[] sits earlier in this file than the GPU-fence
// block that owns this.
// ON: the biggest measured win in the project (10930 -> 6650us frame time,
// 91 -> 150fps) - the engine flushes the GPU pipeline every frame, forcing
// CPU and GPU to serialise. Trades ~7-13ms input latency; the staging fixes
// cover the buffer-reuse hazard it used to guard.
static volatile LONG g_gpuSyncSkip = 1;
// ---- Draw-pass tagging (declarations) ------------------------------------
// The detours and the install block live much further down with the other
// hooks, but the frame hook and HookedSetRenderTarget both sit EARLIER in this
// file and need these, so the state lives here. See the draw-pass inventory
// block for what it is for.
static volatile LONG g_logPassRts;
// Tentative def: g_toggles[] sits earlier than the shader-dump block.
static volatile LONG g_dumpShaders;
// Tentative defs: g_numerics[] and the Present hooks both sit earlier in this
// file than the MSAA block that owns these.
static volatile LONG g_msaaSamples;
static volatile LONG g_msHasContent;
static volatile LONG g_msR32fHasContent;
static volatile LONG g_alphaToCoverage;
static volatile LONG g_gpuVendor;   // PCI vendor id: 0x1002 AMD, 0x10DE NVIDIA, 0 unknown
// F9 full-frame capture request: set by the GUI thread's timer poll (which
// sits earlier in this file), consumed by the render thread.
static volatile LONG g_captureRequest;
static void *g_prevRt0;   // last surface bound at RT slot 0, updated always
// ---- Alpha-to-coverage (see BuildA2cVariant) -----------------------------
// g_alphaToCoverage above is the RETIRED render-state-only attempt, kept
// inert; this is the shader-rewrite implementation that replaces it.
static volatile LONG g_a2cEnable;      // config: rewrite cutout shaders
static volatile LONG g_a2cSharpness;   // coverage ramp steepness
static volatile LONG g_a2cBinds, g_a2cBuildFails, g_a2cStateOn;
static volatile LONG g_alphaBlendOn;      // live ALPHABLENDENABLE state
static volatile LONG g_alphaTestWasOn;    // engine's own ALPHATESTENABLE
static volatile LONG g_a2cDebugVis;       // build variants that paint coverage as colour
static volatile LONG g_a2cMaskTest;       // probe: restrict foliage to one MSAA sample
static volatile LONG g_ssaaFoliage;       // supersample foliage cutouts via sample masks
static volatile LONG g_ssaaOffsetReg = 32;
static volatile LONG g_ssaaDraws, g_ssaaBuildFails;
static volatile LONG g_fringeFoliage;     // blended fringe second pass
static volatile LONG g_fringeDraws, g_fringeBuildFails;
#define D3DSIO_ADD_ 2
static volatile LONG g_a2cSkipNoRoom;     // matched but no free temp/const
static volatile LONG g_a2cBlockedBlend;   // variant skipped: draw was blended
// void* rather than IDirect3DPixelShader9* - this block sits above the
// d3d9.h include (same reason the other tentative defs up here do).
#define A2C_MAX 512
typedef struct {
    void *orig; void *variant; void *ssaa; void *fringe; DWORD hash;
    volatile LONG draws;
    DWORD ssaaOffReg;   // PER SHADER: each picks its own free constant slot
} A2cPair;
static A2cPair g_a2cVariants[A2C_MAX];
static volatile LONG g_a2cVariantCount;
// EXPLICIT ALLOW-LIST. Applying the rewrite to every cutout shader was a wide
// net that damaged unrelated material (NPC lighting, water) while never
// reaching the grass and barriers it was aimed at. A2C now applies ONLY to
// hashes listed here, so an empty list means the feature is inert and safe.
// Populate from the [a2cvar] report once a shader is confirmed by eye with
// A2cIdentify.
// Confirmed by tinting them magenta in game and seeing tree/bush FOLIAGE
// recolour (not silhouette). All three verified as the exact target shape:
// one texkill, multi-tap colour pass, alpha out from a constant.
//   ps_130C02F5  6 taps  diffuse+spec+normal+2x bakedLight+screenShadowMap
//   ps_2B84B7D3  6 taps  sibling permutation
//   ps_77D363F6  8 taps  sibling permutation
// NOT included, deliberately: ps_8676670C and ps_658CC589 (1-2 taps). Those
// are the DEPTH/prepass variants - tinting them blacked out the foliage, and
// removing their texkill is what produced solid-block vegetation shadows.
static const DWORD g_a2cAllow[] = {
    0x130C02F5u,
    0x2B84B7D3u,
    0x77D363F6u,
    0x513B2A00u,   // GRASS (59 instrs, 5 taps, 2 texkill) - user-identified
};
#define A2C_ALLOW_COUNT (sizeof(g_a2cAllow)/sizeof(g_a2cAllow[0]))
static volatile LONG g_a2cIdentify;   // superseded by g_psIdentify below

// ---- Pixel-shader identity map + identify walk ---------------------------
// Declared this early because the GUI panel and the config table both use it
// and both sit above the shader hooks. No D3D types here, deliberately.
//
// The walk covers EVERY pixel shader, not just cutout candidates: restricting
// it to the A2C set meant stepping the list never lit up the grass, because
// the grass shader was never in that set. Ranking is by draws in the LAST
// WINDOW rather than cumulative - cumulative counts left rank #1 wandering
// between near-equal shaders, which is why it flashed on random objects.
// 16384: one forest area alone filled a 2000-entry list, and Luxerion has to
// fit alongside it in the same session for cross-area hunting. At ~40 bytes
// per entry this is ~640KB of BSS, which is nothing next to the game's own
// footprint, and the list is only ever appended to.
#define PS_MAP_MAX 16384
typedef struct {
    void *obj; DWORD hash; UINT taps;
    volatile LONG draws;      // cumulative
    LONG prevDraws;           // value at the previous report window
    LONG recent;              // draws during the last window - the useful one
    int hasVariant;           // an A2C cutout variant exists for this shader
} PsMapEntry;
static PsMapEntry g_psMap[PS_MAP_MAX];
static volatile LONG g_psMapCount;
static volatile LONG g_curPsIdx = -1;       // index of the bound shader, or -1
// Pointer -> map index, open addressing. SetPixelShader runs thousands of
// times per frame, so this must be O(1); scanning 4096 entries per bind is
// not affordable on the render thread.
// Must stay comfortably larger than PS_MAP_MAX or the open-addressed probe
// degrades badly (and a full table would scan all slots on every miss).
#define PS_LOOKUP_SIZE 65536
static LONG g_psLookup[PS_LOOKUP_SIZE];     // 0 = empty, else index+1
#define PS_RANK_MAX 24
static volatile LONG g_psRankIdx[PS_RANK_MAX];
static volatile LONG g_psRankCount;
static volatile LONG g_psIdentify;          // tint the shader at this rank
// OFF by default: removing the game's FXAA is a look preference, and with no
// MSAA configured it leaves edges bare. Pair it with MsaaSamples.
static volatile LONG g_fxaaOff;
static volatile LONG g_fxaaPick;    // 0 = off, 1..N = kill candidate N (see g_psKillCandidates)
static volatile LONG g_msaaDebugClear;
static volatile LONG g_msNeedDepthClear;   // set at each frame boundary
static volatile LONG g_msFrameSeq;         // increments at each frame boundary (episode dump)
// ---- Scene render-target tracking ----------------------------------------
// Three distinct full-screen A8R8G8B8 surfaces exist, so matching on
// format+size cannot pick the one carrying the scene. Per-surface draw counts
// name it by identity: #1 took 1,024,071 draws (566,220 during MULTI_SAMPLE)
// while #2 and #3 took zero. Declared here because MsaaRelease resets this on
// device Reset and sits earlier in the file than the draw-accounting block.
#define SCENE_RT_MAX 16
#define SCENE_RT_LATCH_DRAWS 2000
static void *g_sceneRts[SCENE_RT_MAX];
static volatile LONG g_sceneRtDraws[SCENE_RT_MAX];
static volatile LONG g_sceneRtDrawsMs[SCENE_RT_MAX];
static volatile LONG g_sceneRtCount = 0;
static volatile LONG g_curSceneRtIdx = -1;   // index of the bound scene RT, or -1
static void *g_sceneRtMain = NULL;           // latched by identity
// The linear-depth (R32F) target, tracked the same way. MS_DEPTH renders the
// entire opaque world into it as a DEPTH PREPASS - and the colour pass then
// draws with z-write off, relying on the Z the prepass left in the depth
// buffer. Substituting only the colour target starves our MS depth of that
// prepass, and with everything passing LESSEQUAL-against-1.0 the sky and sea
// quads (drawn after opaque, normally rejected by prepass Z) overpaint the
// world. Latched by draws taken during MS_DEPTH; substituted with an MS R32F
// paired with the SAME MS depth, so the prepass builds our depth exactly the
// way it builds the engine's.
static void *g_depthRts[SCENE_RT_MAX];
static volatile LONG g_depthRtDrawsMsd[SCENE_RT_MAX];
static volatile LONG g_depthRtCount = 0;
static volatile LONG g_curDepthRtIdx = -1;
static void *g_depthRtMain = NULL;           // latched by identity
// ---- Half-res shadow buffer: who creates it? ------------------------------
// The screen-space shadow pass (DRAW_MULTI_SAMPLE_SHADOW / FUN_00ac6b00)
// renders into 1920x1080 targets at 3840x2160 output - exactly half per axis.
// Making that full-res is the shadow-sharpness win, but scaling a surface
// behind the engine's back is precisely what FAILED as ShadowScale: the
// texture grew while every calculation consuming it kept the old size, giving
// shadows confined to a camera-tracking square.
//
// So find the code that DECIDES the size first. A static search for a "/2" in
// a renderer this size is hopeless; the running game already knows. Logging
// _ReturnAddress() at CreateTexture for any render target exactly half the
// presentation size makes the creator name itself - the same trick that pinned
// the cascade-split upload to FUN_00a84e70.
// (Declared here rather than with the RT probe: HookedCreateTexture is earlier
// in this file than that block.)
static UINT g_backbufW = 0, g_backbufH = 0;
static volatile LONG g_halfResLogged = 0;
// ShadowBufPct: percentage applied to the half-res screen-space shadow
// buffers. 0/100 = untouched (stock half res), 200 = full presentation res,
// 50 = QUARTER res.
//
// A percentage rather than an on/off flag specifically so it can be made
// WORSE. "Slightly sharper" is the hardest change to judge by eye - the user
// could not tell whether it had worked. "Obviously blockier" is unmistakable.
// If 50 visibly degrades shadows then the buffer is confirmed to matter AND
// the interception is confirmed to reach it, which makes 200 trustworthy even
// when its improvement is subtle. Same falsification logic TalkTimerPct used
// with its deliberately-worse 200% option.
static volatile LONG g_shadowBufResPct = 0;
// Counter for descriptor-level scaling (see OnTexImpCtor_C). The two failed
// compensation layers this replaces are documented at the former intervention
// site in HookedCreateTexture.
static volatile LONG g_sbufCtorScaled = 0;
// Tentative defs - the canonical ones (with the reasoning) sit further down,
// but GetInternalRenderSize below needs the SSAA mode and scale, and it is the
// first thing in the file that does. Same pattern as g_mainModBase.
static volatile LONG g_ssaaScale;
static volatile LONG g_ssaaMode;
static volatile LONG g_ssaaDescScaled;
// Also needed early: g_numerics[] sits well before the StretchRect block where
// this one is defined.
static volatile LONG g_ssaaOutputRes;
// Frametime overlay corner: 0 = top-left, 1 = top-right, 2 = bottom-left,
// 3 = bottom-right. Exists because the overlay is a topmost window and at the
// top of the screen it covers the game's own menu bar.
static volatile LONG g_overlayPos;
// The game's presentation window, captured from the present parameters at
// Reset - the overlay is positioned against its client area rather than the
// desktop, so it stays correct in windowed mode too.
static HWND g_gameHwnd = NULL;
// Tentative defs + define, needed by the probes below; the canonical ones sit
// further down the file (same pattern as every other early block here).
static unsigned int g_mainModBase;
static unsigned int g_mainModSize;
#define SHADOW_SETTINGS_PTR_RVA (0x0511558c - 0x00400000)

// The engine's INTERNAL render size, from its own settings object - the same
// fields the screen-buffer allocator derives every target from. This is what
// frees the MSAA identity latch from "internal res must equal desktop res":
// the game runs borderless at desktop res, the menu changes only this, and
// the scene/prepass targets follow it. Falls back to the backbuffer size
// until the settings object exists (pre-Reset boot window).
static void GetInternalRenderSize(unsigned int *w, unsigned int *h)
{
    *w = g_backbufW; *h = g_backbufH;
    __try {
        if (g_mainModBase) {
            DWORD so = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
            if (so) {
                unsigned int sw = *(unsigned int *)(so + 0x10);
                unsigned int sh = *(unsigned int *)(so + 0x14);
                if (sw >= 320 && sw <= 16384 && sh >= 200 && sh <= 16384) {
                    *w = sw; *h = sh;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // SSAA descriptor mode renders into ENLARGED targets while deliberately
    // leaving this settings field alone, so the field no longer describes the
    // surfaces. Apply the same scale here, at the single point every consumer
    // reads - which matters because one of them is the MSAA identity latch:
    // it matches candidate render targets against this size, so without this
    // the latch would stop recognising the scene target and MSAA would
    // silently stop engaging the moment SSAA was switched on.
    if (g_ssaaMode == 1 && g_ssaaScale > 100) {
        unsigned int nw = (unsigned int)(((unsigned __int64)*w * (unsigned)g_ssaaScale) / 100) & ~3u;
        unsigned int nh = (unsigned int)(((unsigned __int64)*h * (unsigned)g_ssaaScale) / 100) & ~3u;
        if (nw >= 16 && nw <= 16384 && nh >= 16 && nh <= 16384) { *w = nw; *h = nh; }
    }
}
// Set while FUN_00b00f10 - the screen-space buffer allocator - is executing.
// This is the provenance gate for ShadowBufPct: "scale what THIS function
// creates" instead of "scale whatever matches a size formula". The formula
// version had partial coverage (some trio creations slipped through at
// 1920x1080, so the pass bound a mix of scaled and unscaled targets) and a
// genuine collision: pre-Reset the screen is 1280x720, whose half is 640x360
// - the same size as a level of the post-process pyramid, so bloom buffers
// could be caught by accident. Provenance has neither problem.
static volatile LONG g_inScreenBufAlloc = 0;
static DWORD g_sbufAllocTid = 0;
static DWORD g_sbufAllocRet = 0;
static void *g_trampoline_sbufAlloc = NULL;

// ---- Screen-set rebuild gate (descriptor-mode SSAA) -----------------------
// FUN_00b014b0 is the engine's change detector:
//
//     if (*(param_1[0x21] + 0x10) != *(DAT_0511558c + 0x10) ||
//         *(param_1[0x21] + 0x14) != *(DAT_0511558c + 0x14))
//         FUN_00b00810();                      // rebuild the screen set
//
// i.e. it compares the recorded dimensions of the buffer in slot 0x21 (byte
// offset 0x84, a full-res colour member) against the settings screen size.
//
// Descriptor mode scales that member, and deliberately does NOT touch the
// settings field - so the comparison can never agree again and the rebuild
// runs every frame. Measured in the first descriptor-mode run: descScaled
// climbing ~390 per report interval, 64,584 in one session. That is the whole
// screen buffer set being destroyed and recreated continuously.
//
// The same comparison also explains why enabling SSAA appeared to do nothing
// until a resolution change: going from unscaled to scaled leaves slot 0x21
// still matching the settings, so the detector never fires and nothing is ever
// rebuilt at the new scale. Changing resolution was doing the triggering.
//
// Both are handled here: gate the rebuild so it happens exactly when it should
// (a genuine resolution change, or a scale change we requested), and poke the
// sentinel's dimensions when a scale change needs one rebuild that the
// detector would not otherwise ask for.
#define SCREENSET_OBJ_PTR_RVA (0x05115724 - 0x00400000)
#define SCREENSET_SENTINEL_OFF 0x84         /* param_1[0x21] */
static void *g_trampoline_screenSetRebuild = NULL;
static volatile LONG g_ssaaRebuildPending = 0;
static volatile LONG g_ssaaRebuildsAllowed = 0;
static volatile LONG g_ssaaRebuildsBlocked = 0;
static LONG g_ssaaSeenSettingsW = 0, g_ssaaSeenSettingsH = 0;

// A screen-set rebuild destroys and recreates every surface in the set, which
// invalidates every pointer the MSAA IDENTITY latch holds. Without this,
// g_sceneRtMain keeps pointing at a freed surface, the new scene target is
// never recognised, and MSAA silently stops for the rest of the session -
// including after SSAA is switched back off, because the latch is already
// "latched" and never re-latches. Observed exactly that way: MSAA works until
// SSAA is enabled once, then never again.
//
// Only the TRACKING is cleared, not the MS surfaces: those are rebuilt on
// their own when the size changes (g_msW != sd.Width), and releasing D3D
// objects is not this function's job.
static void MsaaInvalidateSceneLatch(void)
{
    g_sceneRtMain = NULL;
    g_depthRtMain = NULL;
    g_curSceneRtIdx = -1;
    g_curDepthRtIdx = -1;
    for (int i = 0; i < SCENE_RT_MAX; i++) {
        g_sceneRts[i] = NULL;
        g_sceneRtDraws[i] = 0;
        g_sceneRtDrawsMs[i] = 0;
        g_depthRts[i] = NULL;
        g_depthRtDrawsMsd[i] = 0;
    }
    g_sceneRtCount = 0;
    g_depthRtCount = 0;
}

// Returns nonzero to let the engine's rebuild run.
__declspec(noinline) int __cdecl ScreenSetRebuildAllowed_C(void)
{
    // Not our business unless descriptor mode is actively scaling. At scale
    // 100 the descriptors are untouched, so the detector behaves exactly as it
    // does without this mod and must not be interfered with.
    if (g_ssaaMode != 1 || g_ssaaScale <= 100) {
        InterlockedIncrement(&g_ssaaRebuildsAllowed);
        MsaaInvalidateSceneLatch();   // the surfaces about to be freed are latched
        return 1;
    }
    __try {
        DWORD so = g_mainModBase ? *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA) : 0;
        if (so) {
            LONG sw = *(LONG *)(so + 0x10), sh = *(LONG *)(so + 0x14);
            // A genuine resolution change must still rebuild, or the buffers
            // would be stranded at the old size - exactly the bug this gate
            // could otherwise introduce.
            if (sw != g_ssaaSeenSettingsW || sh != g_ssaaSeenSettingsH) {
                g_ssaaSeenSettingsW = sw;
                g_ssaaSeenSettingsH = sh;
                InterlockedIncrement(&g_ssaaRebuildsAllowed);
                return 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (InterlockedCompareExchange(&g_ssaaRebuildPending, 0, 1) == 1) {
        InterlockedIncrement(&g_ssaaRebuildsAllowed);
        MsaaInvalidateSceneLatch();   // the surfaces about to be freed are latched
        return 1;                    // one rebuild, because the scale changed
    }
    InterlockedIncrement(&g_ssaaRebuildsBlocked);
    return 0;                        // the permanent-mismatch loop
}

// __fastcall(this in ECX), prologue 56 8B F1 8B 4E 6C - 6 bytes, clean.
// ECX must survive the decision call, hence the save/restore; the pops do not
// disturb the flags TEST set.
__declspec(naked) void Detour_screenSetRebuild(void)
{
    __asm {
        push ecx
        push edx
        call ScreenSetRebuildAllowed_C
        test eax, eax
        pop edx
        pop ecx
        jz   skip_rebuild
        jmp dword ptr [g_trampoline_screenSetRebuild]
    skip_rebuild:
        ret                          // __fastcall, no stack args to clean
    }
}

// Force exactly one rebuild by making the detector's own comparison fail.
// Cheaper and far safer than calling the rebuild ourselves: the engine still
// performs it, on its own thread, at its own point in the frame.
static void SsaaForceScreenSetRebuild(void)
{
    if (!g_mainModBase) return;
    __try {
        DWORD obj = *(DWORD *)(g_mainModBase + SCREENSET_OBJ_PTR_RVA);
        if (!obj) return;
        DWORD tex = *(DWORD *)(obj + SCREENSET_SENTINEL_OFF);
        if (!tex) return;
        volatile LONG *w = (volatile LONG *)(tex + 0x10);
        volatile LONG *h = (volatile LONG *)(tex + 0x14);
        // Validate before writing: these must already look like dimensions, or
        // the pointer is not what this expects and the poke would be a blind
        // write into unknown memory.
        if (*w < 16 || *w > 16384 || *h < 16 || *h > 16384) return;
        *w = 0;
        *h = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) int __cdecl OnEnter_sbufAlloc_C(void *retAddr)
{
    g_sbufAllocTid = GetCurrentThreadId();
    g_sbufAllocRet = (DWORD)retAddr;
    g_inScreenBufAlloc = 1;
    return 1;
}

__declspec(noinline) void *__cdecl OnReturn_sbufAlloc_C(void)
{
    g_inScreenBufAlloc = 0;
    return (void *)g_sbufAllocRet;
}

__declspec(naked) void OnReturn_sbufAlloc(void)
{
    __asm {
        push eax
        pushfd
        call OnReturn_sbufAlloc_C
        mov  ecx, eax
        popfd
        pop  eax
        jmp  ecx
    }
}

// ---- Texture-factory entry probe ------------------------------------------
// A second creator keeps producing STOCK half-screen buffers that the shadow
// pass binds, bypassing FUN_00b00f10 (the change detector is exonerated: its
// screen-set sentinel is slot [0x21], a full-res member we never touch). The
// factory's callers cannot be enumerated statically - FUN_00d72400 is reached
// through a vtable - so log the factory's own return address at entry when
// the dims are half-screen. Entry-only, no return hijack: FUN_00a94770 has an
// SEH prologue (6A FF 68 ...), and return-address hijacking on SEH-prologue
// functions is the suspected cause of two historical crashes. Both prologue
// instructions use immediates, so the 7-byte relocation into the trampoline
// is position-independent.
static void *g_trampoline_texFactory = NULL;
static volatile LONG g_texFactoryLogged = 0;

__declspec(noinline) void __cdecl OnTexFactory_C(void *retAddr, unsigned int w, unsigned int h)
{
    __try {
        if (!g_mainModBase) return;
        DWORD so = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
        if (!so) return;
        unsigned int sw = *(unsigned int *)(so + 0x10);
        unsigned int sh = *(unsigned int *)(so + 0x14);
        if (sw < 320 || sw > 16384 || sh < 200 || sh > 16384) return;
        unsigned int hw = (((sw + 1) >> 1) + 1) & ~1u;
        unsigned int hh = (((sh + 1) >> 1) + 1) & ~1u;
        if (!((w == hw && h == hh) || (w == hh && h == hw))) return;
        if (InterlockedIncrement(&g_texFactoryLogged) > 12) return;
        DWORD ra = (DWORD)(UINT_PTR)retAddr;
        char l[176];
        sprintf(l, "[halfres] factory %ux%u from %08X%s inAlloc=%ld",
                w, h,
                (ra > g_mainModBase && ra < g_mainModBase + g_mainModSize)
                    ? ra - g_mainModBase + 0x00400000 : ra,
                (ra > g_mainModBase && ra < g_mainModBase + g_mainModSize)
                    ? " (ghidra)" : " (outside)",
                g_inScreenBufAlloc);
        LogLine(l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void Detour_texFactory(void)
{
    __asm {
        push ecx
        push edx
        // Factory args on the stack: [ret][&out][w][h]... After our two
        // pushes: ret at +8, w at +16, h at +20. Push right-to-left for
        // (retAddr, w, h); each push shifts the frame by 4.
        mov eax, [esp + 20]      // h
        push eax
        mov eax, [esp + 20]      // w  (was +16, now +20 after one push)
        push eax
        mov eax, [esp + 16]      // ret (was +8, now +16 after two pushes)
        push eax
        call OnTexFactory_C
        add esp, 12
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_texFactory]
    }
}

__declspec(naked) void Detour_sbufAlloc(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]          // return address
        push eax
        call OnEnter_sbufAlloc_C
        add esp, 4
        test eax, eax
        jz skip_sbufAlloc
        mov dword ptr [esp + 8], offset OnReturn_sbufAlloc
    skip_sbufAlloc:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_sbufAlloc]
    }
}
// DRAW_MENU and DRAW_BACK_BUFFER added 2026-08-12 for the native-SSAA
// reconnaissance. Everything after DRAW_FILTER used to be attributed to
// DRAW_FILTER, which is exactly the ambiguity that has to be resolved before
// SSAA can be built: if the UI draws into the same off-screen target as the
// scene, supersampling the scene also softens the UI unless the downsample is
// moved to the DRAW_MENU boundary. Tagging those two passes makes the [pass]
// inventory say which target the UI binds, and at what size, instead of
// leaving it to be inferred from pass ORDER (which is all we had).
enum { PASS_NONE = 0, PASS_SHADOW, PASS_MS_SCHEDULE, PASS_MS_PROPAGATION,
       PASS_MS_DEPTH, PASS_MS_SHADOW, PASS_MS, PASS_FILTER,
       PASS_MENU, PASS_BACKBUF, PASS_COUNT };
static const char *g_passNames[PASS_COUNT] = {
    "(none/pre-shadow)", "DRAW_SHADOW", "MS_SCHEDULE", "MS_PROPAGATION",
    "MS_DEPTH", "MS_SHADOW", "MULTI_SAMPLE", "DRAW_FILTER(+post)",
    "DRAW_MENU(UI)", "DRAW_BACK_BUFFER"
};
static volatile LONG g_curPass = PASS_NONE;
static volatile LONG g_passMaxRtIndex[PASS_COUNT];
#define PASS_RT_SEEN_MAX 128
typedef struct { LONG pass; void *surf; } PassRtSeen;
static PassRtSeen g_passRtSeen[PASS_RT_SEEN_MAX];
static volatile LONG g_passRtSeenCount = 0;
// Tentative def + forward decl: g_numerics[] and the monitor thread both sit
// earlier in this file than the limiter block that owns these.
static volatile LONG g_spinGuardUs;
static void ReportAndClampSleepGranularity(void);
static volatile LONG g_pendingShadowCapture;
// Cascade split override percentages - g_numerics[] above needs their
// addresses; the hook that uses them lives with the real-device hooks.
static volatile LONG g_shadowSplitNearPct;
static volatile LONG g_shadowSplitFarPct;
// ---- Shadow map resolution multiplier ------------------------------------
// The RT inventory (see FEATURES.md) identified the shadow set precisely: at
// 4K the game allocates a 2048x4096 R32F atlas (two cascades stacked) plus
// two 2048x2048 R32F cascade targets and two matching 2048x2048 D24S8 depth
// surfaces. All of them arrive through CreateTexture - this engine never
// calls CreateRenderTarget - so scaling them is a creation-time rewrite in
// the hook we already own.
//
// The discriminator matters more than the scaling: R32F render targets also
// exist for LINEAR DEPTH at screen resolution (3840x2160, 1920x1080,
// 1280x720, 640x360), and blowing those up would be both pointless and
// expensive. Every screen-derived target here is 16:9; the shadow set is
// power-of-two with a 1:1 or 1:2 (atlas) aspect. That separates them cleanly
// without needing to guess at call sites.
//
// 1 = untouched. Memory cost is quadratic: R32F at 4096x8192 is ~134MB, so
// this is a tunable rather than a fixed 2x.
static volatile LONG g_shadowScale = 1;
static volatile LONG g_shadowScaled = 0, g_shadowScaleFail = 0;
// First test failed with a textbook symptom: shadows confined to a square
// that tracks the camera. Cause is not the texture size - identification and
// scaling were both correct on all five surfaces - it is that the game sets
// an explicit VIEWPORT sized to the shadow map it expects (2048), so the
// shadow pass only rendered into the top-left quarter of the 4096 texture
// while sampling still spanned the full UV range.
//
// Fix: remember the level-0 surface of every scaled shadow texture, notice
// when one is the active render target, and scale SetViewport by the same
// multiplier while it is. X/Y are scaled too, not just Width/Height - the
// 2-cascade atlas addresses its halves with sub-viewport offsets, and
// leaving those unscaled would stack both cascades into one corner.
// (the surface table and its helpers need D3D9 types, so they live further
// down, immediately before HookedCreateTexture)
#define MAX_SHADOW_SURFACES 16
static volatile LONG g_shadowSurfaceCount = 0;
static volatile LONG g_shadowRtActive = 0;   // a scaled shadow map is bound
static volatile LONG g_viewportScaled = 0;

// ---- Shadow map resolution, the CORRECT way ------------------------------
// Ghidra found the engine's own shadow-resolution value. Its debug menu
// handlers for Graphics_Shadowing are two-liners:
//
//   FUN_00acac90 ("Advanced"): *(DAT_0511558c + 0x24) = 0x800;   // 2048
//   FUN_00acacc0 ("Standard"): *(DAT_0511558c + 0x24) = 0x400;   // 1024
//
// So `[*(0x0511558c) + 0x24]` IS the shadow map edge length, and it is what
// the engine uses for texture creation AND the light projection AND the PCF
// tap offsets (`s_shadowOffset0..7`). That is exactly why scaling the
// TEXTURE behind the engine's back produced shadows confined to a
// camera-tracking square: the surface grew, every calculation that consumed
// it did not.
//
// Writing this value instead makes the engine do all of it consistently -
// same philosophy as ApplyFramerateUnlock writing ticksPerFrame rather than
// patching the limiter. It also explains the 1024->2048 jump seen in the RT
// inventory: that was the game applying "Advanced" after the Reset, not a
// resolution-derived size.
//
// Applied repeatedly from the monitor thread rather than once, so it
// survives the engine reasserting its own value on a settings change or a
// device Reset.
#define SHADOW_SETTINGS_PTR_RVA (0x0511558c - 0x00400000)
static unsigned int g_mainModBase;   // tentative def; real one is further down
static volatile LONG g_shadowMapRes = 0;
static volatile LONG g_shadowResWrites = 0;
static LONG g_shadowResLastSeen = 0;

// ---- Cascade split distances, at the SOURCE --------------------------------
// Ten rounds to get here. Runtime _ReturnAddress() pinned the
// shadowSplitRange upload to FUN_00a84e70+0x8e, which reads:
//
//   local_18 = *(float *)(DAT_05107a00 + 0x42c);                    // near
//   local_14 = *(float *)(DAT_05107a00 + 0x430)
//            * *(float *)(DAT_05107a00 + 0x438);                    // far
//   SetPixelShaderConstantF(dev, handle, &local_18, 1);
//
// So the splits live in the scene/renderer object pointed to by 0x05107a00.
// The far distance is a PRODUCT of two fields, which is also why it varies by
// location the way the user noticed - one factor is presumably per-area.
//
// Patching here rather than at the upload matters: rewriting the uploaded
// constant was tried and did nothing visible, which fits if the shader uses
// it only for cascade fade/blend while actual coverage comes from projections
// built elsewhere FROM THESE SAME FIELDS. Changing the source should reach
// both.
//
// Scaled by percentage, not replaced, to preserve whatever per-area values
// the engine computes.
#define SCENE_PTR_RVA (0x05107a00 - 0x00400000)
static volatile LONG g_splitSrcWrites = 0;
static float g_splitSrcSeenNear = 0.0f, g_splitSrcSeenFar = 0.0f;

static void ApplyCascadeSplitSource(void)
{
    if (g_mainModBase == 0) return;
    if (g_shadowSplitNearPct <= 0 && g_shadowSplitFarPct <= 0) return;
    __try {
        DWORD obj = *(DWORD *)(g_mainModBase + SCENE_PTR_RVA);
        if (!obj) return;
        volatile float *nearF = (volatile float *)(obj + 0x42c);
        volatile float *farA  = (volatile float *)(obj + 0x430);
        volatile float *farB  = (volatile float *)(obj + 0x438);
        float n = *nearF, a = *farA, b = *farB;
        // Sanity: only touch values that look like the observed split setup
        // (near ~10, far product ~79). A wild pointer will not satisfy this.
        if (!(n > 0.1f && n < 2000.0f)) return;
        if (!(a > 0.0f && b > 0.0f && a * b < 20000.0f)) return;
        // COMPOUNDING GUARD. This scales whatever value is currently in the
        // field, and it runs every frame. That is only safe while the engine
        // rewrites the field first - if it ever skips a frame, we would scale
        // our own already-scaled value and the split would run away
        // exponentially (30 -> 90 -> 270...). Remembering exactly what we last
        // wrote makes the difference detectable: if the field still holds our
        // value, the engine did not refresh it and there is nothing to do.
        // Always scale the ENGINE'S value, never our own output.
        //
        // The previous version scaled whatever was currently in the field and
        // only skipped when it matched our last write. That compounded: toggle
        // a setting off (last-write forgotten) then on again while the field
        // still held the scaled value, and 10 became 30 became 90. It also
        // made mid-session percentage changes silently do nothing whenever the
        // engine had not refreshed the field yet. Rebooting looked like a fix
        // only because the field started clean.
        //
        // Fix: keep the engine's own value as a separate baseline. Any value
        // that is not exactly what we last wrote must have come from the
        // engine, so that becomes the new baseline - which also tracks the
        // per-area changes correctly. Everything is then computed from the
        // baseline, so repeated application is idempotent and toggling is safe.
        static float baseNear = 0.0f, lastWroteNear = 0.0f;
        static float baseFarA = 0.0f, lastWroteFarA = 0.0f;
        if (n != lastWroteNear) baseNear = n;
        if (a != lastWroteFarA) baseFarA = a;
        g_splitSrcSeenNear = baseNear;
        g_splitSrcSeenFar = baseFarA * b;
        if (g_shadowSplitNearPct > 0 && baseNear > 0.0f) {
            float want = baseNear * (g_shadowSplitNearPct / 100.0f);
            if (*nearF != want) {
                *nearF = want;
                lastWroteNear = want;
                InterlockedIncrement(&g_splitSrcWrites);
            }
        }
        if (g_shadowSplitFarPct > 0 && baseFarA > 0.0f) {
            // scale one factor only - the other is left alone so whatever
            // per-area meaning it carries is preserved
            float want = baseFarA * (g_shadowSplitFarPct / 100.0f);
            if (*farA != want) {
                *farA = want;
                lastWroteFarA = want;
                InterlockedIncrement(&g_splitSrcWrites);
            }
        }
        // Turning a percentage off needs no restore - we stop writing and the
        // engine's next recompute puts its own value back. lastWrote is left
        // alone on purpose: clearing it was what let the baseline be poisoned
        // by our own output on re-enable.
        if (g_shadowSplitNearPct <= 0) { *nearF = baseNear > 0.0f ? baseNear : n; }
        if (g_shadowSplitFarPct <= 0)  { *farA  = baseFarA > 0.0f ? baseFarA : a; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Force shadows off (diagnostic) --------------------------------------
// The game's own quality menu offers only 1024 and 2048 - there is no "off",
// so the frametime square wave could not be tested against "no shadow pass at
// all". But the DRAW_SHADOW handler gates the whole pass on two BYTES in the
// same settings object ApplyShadowResolution already writes:
//
//   FUN_00ac6040:
//     if (*(char *)(DAT_0511558c + 0x2c) != 0 && *(char *)(DAT_0511558c + 0x2d) != 0) {
//         ... FUN_00a32a00();      // the shadow render
//     }
//     return;
//
// Zeroing one of them makes the engine take its own already-supported
// shadows-disabled branch. Nothing is patched; this is the same "write the
// value the engine reads" approach as ShadowMapRes and ApplyFramerateUnlock.
//
// RETIRED APPROACH - writing the gate byte FROZE ALL RENDERING.
//
// Tested: zeroing [+0x2c] does remove the frametime square wave, but the whole
// image stops updating, which makes the result worthless - a flat frametime
// with nothing being drawn proves nothing about shadows.
//
// The decompile says why, and it contradicts the "engine already supports this
// branch" assumption this was built on. When the gate is false the handler
// skips TWO things beyond the shadow render:
//
//     *(param_1 + 0x298) = *(DAT_05107a00 + 0x940);   // never assigned; stays 0
//     FUN_00a95f40();                                 // never called
//
// FUN_00a95f40 is almost certainly the pass-completion call, so the draw
// manager is never told the pass finished. The gate is presumably only ever
// false in a configuration where the draw graph is built differently, not
// something that can be flipped at runtime.
//
// REPLACEMENT: skip only FUN_00a32a00, the shadow render itself. It sits
// inside the inner `if (iVar3 != 0)` block while the +0x298 write and
// FUN_00a95f40() come after it, so skipping just that call leaves every piece
// of surrounding bookkeeping intact. Expect stale/garbage shadow maps being
// sampled (visual artefacts) rather than a stalled pipeline.
//
// Its prologue is 53 8B DC 83 EC 08 (PUSH EBX / MOV EBX,ESP / SUB ESP,8) -
// exactly 6 bytes, a clean patch boundary. It takes no arguments and returns
// void, so the detour can simply RET: the JMP into it leaves the caller's
// return address at [esp] untouched.
#define SHADOW_RENDER_RVA (0x00a32a00 - 0x00400000)
static volatile LONG g_shadowsOff = 0;
static void *g_trampoline_shadowRender = NULL;

__declspec(naked) void Detour_shadowRender(void)
{
    __asm {
        // Function entry - no flags are live across a call boundary, so the
        // CMP is free to clobber them.
        cmp dword ptr [g_shadowsOff], 0
        jz  run_shadow_render
        ret                                        // skip the render entirely
    run_shadow_render:
        jmp dword ptr [g_trampoline_shadowRender]
    }
}

// ---- Graphics_Scaling: the engine's own image-scaling mode ----------------
// The AA that is present with MSAA off, on geometry AND alpha-tested cutouts,
// and that survives replacing every fullscreen pixel shader with a
// passthrough, is not a post-process draw. The settings table has exactly one
// candidate left, and its three menu handlers (registered in FUN_00acaf60)
// are two-liners of the same shape as Graphics_Shadowing - which is the
// template ShadowMapRes already exploits:
//
//   Graphics_Scaling_Advanced (FUN_00acaea0): [+0x20]=1  [+0x40]=1
//   Graphics_Scaling_Standard (FUN_00acaee0): [+0x20]=1  [+0x40]=0
//   Graphics_Scaling_None     (FUN_00acaf20): [+0x20]=3  [+0x40]=0
//
// The readers of this settings object are FUN_00b00f10 / FUN_00b010c0 /
// FUN_00b014b0 - the screen-buffer allocator and its siblings, i.e. the code
// that decides how the screen-space buffers are BUILT. That is a whole-image
// mechanism, which is exactly the shape of the observed effect.
//
// Config: 0 = leave the engine alone (default), 1 = None, 2 = Standard,
// 3 = Advanced. Values are written every monitor tick like ShadowMapRes, so
// the menu cannot quietly win, and the CURRENT pair is always logged so the
// game's own default is visible rather than assumed.
static volatile LONG g_scalingMode;
static volatile LONG g_scalingWrites;
static volatile LONG g_scalingSeen20 = -1, g_scalingSeen40 = -1;

static void ApplyScalingMode(void)
{
    if (g_mainModBase == 0) return;
    __try {
        DWORD objPtr = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
        if (!objPtr) return;
        volatile DWORD *mode = (volatile DWORD *)(objPtr + 0x20);
        volatile unsigned char *adv = (volatile unsigned char *)(objPtr + 0x40);
        DWORD curMode = *mode;
        unsigned char curAdv = *adv;
        // Only 1 and 3 are ever written by the game's own handlers; anything
        // else means this pointer is not the object this expects.
        if (curMode != 1 && curMode != 3) return;
        g_scalingSeen20 = (LONG)curMode;
        g_scalingSeen40 = (LONG)curAdv;
        if (g_scalingMode <= 0) return;
        DWORD wantMode; unsigned char wantAdv;
        switch (g_scalingMode) {
            case 1:  wantMode = 3; wantAdv = 0; break;   // None
            case 2:  wantMode = 1; wantAdv = 0; break;   // Standard
            default: wantMode = 1; wantAdv = 1; break;   // Advanced
        }
        if (curMode != wantMode || curAdv != wantAdv) {
            *mode = wantMode;
            *adv = wantAdv;
            InterlockedIncrement(&g_scalingWrites);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void ApplyShadowResolution(void)
{
    if (g_shadowMapRes <= 0 || g_mainModBase == 0) return;
    __try {
        DWORD objPtr = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
        if (!objPtr) return;                       // settings object not built yet
        volatile DWORD *field = (volatile DWORD *)(objPtr + 0x24);
        DWORD cur = *field;
        // Only ever seen holding 0x400/0x800; anything wildly outside that
        // range means the pointer is not what this expects and writing would
        // be a blind poke into unknown memory.
        if (cur < 256 || cur > 16384) return;
        g_shadowResLastSeen = (LONG)cur;
        if (cur != (DWORD)g_shadowMapRes) {
            *field = (DWORD)g_shadowMapRes;
            InterlockedIncrement(&g_shadowResWrites);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Native SSAA (supersampling), scene only ------------------------------
// Renders the SCENE above output resolution and lets the engine's own
// scene->backbuffer copy downsample it. Unlike MSAA this supersamples
// SHADING, so it is the only route here that antialiases alpha-tested
// cutouts (vegetation, fences) - the thing the entire alpha-to-coverage
// effort failed to achieve.
//
// WHY THIS WORKS AT ALL - measured, not assumed (2026-08-12 probe runs,
// FEATURES.md "Native SSAA"):
//   1. Internal render size and backbuffer size are ALREADY decoupled. The
//      game's own resolution menu moves the internal size while every device
//      Reset stays at the desktop resolution - which is exactly why the MSAA
//      identity latch had to match GetInternalRenderSize() rather than the
//      backbuffer. SSAA is that same decoupling driven UPWARD.
//   2. The UI is not in the scene target. Inside DRAW_MENU the engine blits
//      the finished scene (A8R8G8B8) into the BACKBUFFER (X8R8G8B8), then
//      binds the backbuffer and draws the UI onto it. So the downsample point
//      already exists, and the UI lands after it at native resolution - the
//      "scene only, not the UI" requirement is satisfied by the engine's own
//      frame structure, with no mid-frame resolve to engineer.
//   3. That blit is POINT. Downscaling with POINT DECIMATES - it would throw
//      away three of every four pixels and produce zero antialiasing at full
//      cost. Forcing LINEAR while scaling is therefore mandatory, not a
//      refinement; see HookedStretchRect.
//
// Written into the engine's own size field rather than by substituting larger
// surfaces underneath it. That is the approach that worked for ShadowMapRes
// and ShadowBufPct, and it avoids the ShadowScale failure mode, where the
// engine kept deriving viewports and texel maths from its own recorded
// dimensions so every consumer stayed at the old size. Here the engine
// recomputes the whole chain - scene target, depth prepass, post pyramid,
// viewports - from the number we write.
//
// Percent, applied to BOTH axes: 100 = off, 200 = 2x per axis = 4x the pixels
// and 4x the shading. Cost is the SQUARE of this number.
static volatile LONG g_ssaaScale = 100;
// 1 = descriptor mode, the only shipping implementation. The numeric entry is
// gated out with the retired mode, so nothing can set this back to 0.
static volatile LONG g_ssaaMode = 1;
static volatile LONG g_ssaaWrites = 0;
static volatile LONG g_ssaaActive = 0;      // read by the StretchRect filter force
static volatile LONG g_ssaaCurW = 0, g_ssaaCurH = 0;
static LONG g_ssaaBaseW = 0, g_ssaaBaseH = 0;    // the engine's own (unscaled) size
static LONG g_ssaaWroteW = 0, g_ssaaWroteH = 0;  // the last value WE wrote
static LONG g_ssaaLoggedW = 0, g_ssaaLoggedH = 0;
// Last values acted on, so a scale/mode change can be detected and turned into
// exactly one screen-set rebuild rather than a per-tick one.
// Initialised to MATCH the defaults, so a fresh launch never looks like a
// change. g_ssaaMode's default is 1; leaving this at 0 made every launch fire
// a spurious forced rebuild.
static LONG g_ssaaLastScale = 100, g_ssaaLastMode = 1;

// STATE 2026-08-12, after four test runs: this route WORKS where the
// presentation is clamped by the display - i.e. fullscreen at the desktop
// resolution, which is the normal way this game is played - and does nothing
// useful elsewhere. It ships in exactly that form.
//
// The root fact: `[settings]+0x10/+0x14` is the game's RESOLUTION, not an
// internal-only render size. It drives the window and swap chain as well as
// the render targets. So raising it renders bigger AND presents bigger,
// EXCEPT when the presentation physically cannot follow - fullscreen at the
// desktop resolution - where the clamp leaves render > presented, which is
// supersampling. That clamp is doing the real work, and it is dependable
// (a display cannot present more pixels than it has).
//
// TWO THINGS TRIED AND REVERTED, both kept below one #define each:
//
//  1. Anchoring the scale to the PRESENTED size instead of the resolution
//     setting (ENABLE_SSAA_PRESENT_ANCHOR). Correct in principle, disastrous
//     in practice: at a 1080p setting the engine already presents into a 4K
//     backbuffer, so 2x asked for 8K. Worse, it closed a feedback loop - our
//     write resizes the window, the window's DECORATED client area becomes
//     the new backbuffer (observed: 3764x2131, a size nobody requested), that
//     re-anchors the scale, which produces another write. 21 Resets in one
//     run, writes=17, and a window blinking about once a second.
//
//  2. Pinning the swap chain at Reset (ENABLE_SSAA_PRESENT_PIN). Never fired
//     once - pins=0 across a whole run - because it tested for our exact
//     target while the engine kept arriving at decoration-adjusted sizes. In
//     the one case it did engage it produced a 1080p swap chain inside a 4K
//     window: unfiltered pixel-doubling, and a 1:1 blit into the top-left
//     quarter with a menu open.
//
// Anchoring to the resolution setting (below) has no such loop: the base only
// changes when someone OTHER than us writes the field, so writes settle at 1.
//
// The route that would lift the fullscreen-only restriction is descriptor
// scaling - never touch this field, enlarge the render TARGETS at creation
// provenance-gated on FUN_00b00f10, as ShadowBufPct already does. See
// FEATURES.md; that is the next piece of work, not a replacement for this one.
// RETIRED 2026-08-12. Descriptor mode supersedes it completely: it works in
// windowed AND fullscreen, at any resolution setting, and honours the chosen
// resolution instead of silently overriding it. This one only ever worked
// where the display clamped the presentation, and its two attempted fixes are
// documented above. Kept compiled-out rather than deleted.
#define ENABLE_SSAA_RESWRITE       0
#define ENABLE_SSAA_PRESENT_ANCHOR 0
#define ENABLE_SSAA_PRESENT_PIN    0

static void ApplySsaaScale(void)
{
    // Descriptor mode does its work in OnTexImpCtor_C and must NOT touch the
    // resolution field - that field moving is the entire reason the retired
    // mode was fullscreen-only. g_ssaaActive tracks "a scale is applied", which
    // the downsample/output-res logic in HookedStretchRect keys off.
    //
    // This path is NOT inside the ENABLE_SSAA_RESWRITE gate: it is the shipping
    // implementation, and gating it with the retired one would silently disable
    // supersampling entirely.
    if (g_ssaaMode == 1) {
        g_ssaaActive = (g_ssaaScale != 100) ? 1 : 0;
        // A scale change only takes effect when the screen set is rebuilt, and
        // going from unscaled to scaled leaves the detector's sentinel still
        // agreeing with the settings - so it would never ask for one. Request
        // it, and make the detector's comparison fail so it actually happens.
        // Only a SCALE change matters, and only when a scale is actually being
        // applied or removed.
        //
        // This previously also compared the mode, with g_ssaaLastMode
        // initialised to 0 while g_ssaaMode defaults to 1 - so on the FIRST
        // tick of every launch it read as "the mode just changed" and forced a
        // rebuild even with SSAA switched off. That poke zeroes slot 0x21's
        // recorded dimensions, which is exactly what the MSAA identity latch
        // compares candidate render targets against: the scene target was
        // never recognised and MSAA silently did nothing
        // (subs=0 with 2.5M draws counted, i.e. hooks live, latch starved).
        // Never poke the engine's bookkeeping when there is nothing to change.
        if (g_ssaaScale != g_ssaaLastScale) {
            int wasScaled = (g_ssaaLastScale > 100);
            int isScaled  = (g_ssaaScale > 100);
            g_ssaaLastScale = g_ssaaScale;
            g_ssaaLastMode = g_ssaaMode;
            if (wasScaled || isScaled) {
                InterlockedExchange(&g_ssaaRebuildPending, 1);
                SsaaForceScreenSetRebuild();
                char cl[128];
                sprintf(cl, "[ssaa] scale changed -> forcing one screen-set rebuild (scale=%ld%%)",
                        g_ssaaScale);
                LogLine(cl);
            }
        }
        return;
    }
#if !ENABLE_SSAA_RESWRITE
    // The retired mode is compiled out; nothing else can apply a scale.
    g_ssaaActive = 0;
    return;
#else
    // Leaving descriptor mode with scaled buffers live needs the same one-shot
    // rebuild, or they stay enlarged after the feature is switched off.
    if (g_ssaaMode != g_ssaaLastMode) {
        g_ssaaLastMode = g_ssaaMode;
        g_ssaaLastScale = g_ssaaScale;
        InterlockedExchange(&g_ssaaRebuildPending, 1);
        SsaaForceScreenSetRebuild();
    }
    if (g_mainModBase == 0) return;
    __try {
        DWORD objPtr = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
        if (!objPtr) return;                    // settings object not built yet
        volatile DWORD *pw = (volatile DWORD *)(objPtr + 0x10);
        volatile DWORD *ph = (volatile DWORD *)(objPtr + 0x14);
        DWORD curW = *pw, curH = *ph;
        // Same sanity window GetInternalRenderSize uses: anything outside it
        // means this pointer is not the object expected, and writing would be
        // a blind poke into unknown memory.
        if (curW < 320 || curW > 16384 || curH < 200 || curH > 16384) return;

        // Whose value is this? Anything we did not write ourselves came from
        // the engine - including the player using the game's own resolution
        // menu - and is remembered so turning SSAA off can put it back.
        if ((LONG)curW != g_ssaaWroteW || (LONG)curH != g_ssaaWroteH) {
            g_ssaaBaseW = (LONG)curW;
            g_ssaaBaseH = (LONG)curH;
        }
        if (g_ssaaBaseW <= 0 || g_ssaaBaseH <= 0) return;

        LONG scale = g_ssaaScale;
        if (scale < 100) scale = 100;
        if (scale > 200) scale = 200;

        // Anchored to the RESOLUTION SETTING, deliberately - see the
        // ENABLE_SSAA_PRESENT_ANCHOR note above for what anchoring to the
        // presented size cost. The base only moves when someone other than us
        // writes the field, so there is no feedback path back into our own
        // input and `writes` settles at 1 instead of climbing.
        LONG wantW = g_ssaaBaseW, wantH = g_ssaaBaseH;
#if ENABLE_SSAA_PRESENT_ANCHOR
        LONG presW = (LONG)g_backbufW, presH = (LONG)g_backbufH;
        if (scale != 100 && (presW <= 0 || presH <= 0)) return;
        if (scale != 100) { wantW = presW; wantH = presH; }
#endif
        if (scale != 100) {
            // Both axes take the SAME percentage and the SAME rounding, so the
            // aspect ratio survives. That matters: the final copy stretches
            // full-surface to full-surface, so a mismatched aspect would
            // DISTORT the image rather than merely soften it.
            wantW = ((wantW * scale / 100) + 2) & ~3L;
            wantH = ((wantH * scale / 100) + 2) & ~3L;
        }

        if ((LONG)curW != wantW || (LONG)curH != wantH) {
            *pw = (DWORD)wantW;
            *ph = (DWORD)wantH;
            g_ssaaWroteW = wantW;
            g_ssaaWroteH = wantH;
            InterlockedIncrement(&g_ssaaWrites);
            // Logged only when the TARGET changes, not per write: if the
            // engine ever fights us for this field the write counter climbs
            // while the log stays quiet, which is the signal worth having.
            if (wantW != g_ssaaLoggedW || wantH != g_ssaaLoggedH) {
                g_ssaaLoggedW = wantW; g_ssaaLoggedH = wantH;
                char l[192];
                sprintf(l, "[ssaa] internal render size %lux%lu -> %ldx%ld"
                           " (scale %ld%%, engine base %ldx%ld)",
                        (unsigned long)curW, (unsigned long)curH,
                        wantW, wantH, scale, g_ssaaBaseW, g_ssaaBaseH);
                LogLine(l);
            }
        }
        g_ssaaCurW = wantW;
        g_ssaaCurH = wantH;
        g_ssaaActive = (scale != 100) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif  // ENABLE_SSAA_RESWRITE
}
// ---- Talk-entry lifetime: a frame-denominated countdown --------------------
// FieldTalkManager::update (FUN_005de2c0) runs once per frame out of the field
// subsystem list (FUN_005b5050) over a fixed pool of 4 talk entries, each
// driven by a 16-state machine. State 0 seeds the entry's timer:
//
//     entry[0x24] = rand(50000) * 0.0001f + 5.0f;        // [5.0, 10.0)
//
// and state 10 counts it down, tearing the entry down when it hits zero:
//
//     entry[0x24] -= 0.05f;                              // <-- per FRAME
//     if (entry[0x24] <= 0.0f) state = 0xc;              // -> FUN_005dbc70,
//                                                        //    frees the slot
//
// There is no delta-time term anywhere in that path. The step is a literal,
// confirmed against the instruction stream rather than trusted from the
// decompiler (the decompiler said "- 0.05" while the float constant sweep
// found no 0.05f reader in this function - the constant is a DOUBLE):
//
//     005e0986  SUBSD XMM0, qword ptr [0x00df8fe0]
//     [0x00df8fe0] = 0.05000000074505806                 // a 0.05f widened
//
// entry[0x24] has exactly three touch points in the whole function - init to
// 0, init to the random seed, and this decrement - so the reading is not
// ambiguous. A talk entry therefore lives 100-200 FRAMES: 3.3-6.7s at 30fps
// but only 1.7-3.3s at 60. The lifetime is halved, and the random seed makes
// failure probabilistic rather than certain - which is the exact shape of the
// reported 60fps bugs (each has a CHANCE to fail; 30fps mode fixes all four).
//
// This rewrites the instruction's disp32 to point at a double we own, so the
// step can be rescaled live from the GUI without ever writing code again. The
// engine's 0.05 constant is SHARED with four unrelated ADDSD sites, which is
// exactly why the constant must not be edited in place.
//
// RETIRED (ENABLE_TALK_TIMER). The mechanism above is real and the analysis
// holds - this genuinely is a frame-denominated countdown with no delta-time
// term - but it was never connected to a bug. It is NOT the Gysahl cause
// (that was a script-side yield bug, fixed in scr104.clb), and user testing
// found no effect on anything else. Retiring it also removes the only place
// this mod rewrites an instruction operand in the game's code section.
#define TALK_STEP_INSN_RVA (0x005e0986 - 0x00400000)
static volatile LONG g_talkTimerPct = 0;   // 0 = untouched, 50 = half speed
static double g_talkStep = 0.05;           // the constant the game will read
static DWORD  g_talkStepOrigDisp = 0;
static int    g_talkStepPatched = 0;       // 0 = not yet, 1 = done, -1 = refused

#if ENABLE_TALK_TIMER
static void ApplyTalkTimerScale(void)
{
    if (g_mainModBase == 0) return;
    LONG pct = g_talkTimerPct;
    if (pct <= 0) return;                  // off: never patch at all

    // Refresh our constant every call, so a GUI change lands on the next frame
    // with no further code writes.
    g_talkStep = 0.05 * (double)pct / 100.0;
    if (g_talkStepPatched) return;

    __try {
        unsigned char *insn = (unsigned char *)(g_mainModBase + TALK_STEP_INSN_RVA);
        if (insn[0] != 0xF2 || insn[1] != 0x0F ||
            insn[2] != 0x5C || insn[3] != 0x05) {
            LogLine("[talk] step patch REFUSED: not SUBSD XMM0,[disp32] at the "
                    "expected RVA - wrong exe build?");
            g_talkStepPatched = -1;
            return;
        }
        DWORD disp = *(DWORD *)(insn + 4);
        double cur = *(double *)disp;
        // Guard against a relocation/version surprise: if the operand is not
        // the 0.05 we identified, this is not the instruction we think it is.
        if (cur < 0.0499 || cur > 0.0501) {
            char l[160];
            sprintf(l, "[talk] step patch REFUSED: operand at 0x%08X is %.6f, "
                       "expected 0.05", disp, cur);
            LogLine(l);
            g_talkStepPatched = -1;
            return;
        }
        DWORD oldProtect;
        if (!VirtualProtect(insn + 4, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LogLine("[talk] step patch: VirtualProtect failed");
            return;                        // retry on the next tick
        }
        g_talkStepOrigDisp = disp;
        *(DWORD *)(insn + 4) = (DWORD)&g_talkStep;
        VirtualProtect(insn + 4, 4, oldProtect, &oldProtect);
        g_talkStepPatched = 1;
        {
            char l[192];
            sprintf(l, "[talk] step patched: 0.05 -> %.4f (%ld%%) | operand "
                       "0x%08X -> 0x%08X | talk entries now live %.1f-%.1f "
                       "frames", g_talkStep, pct, g_talkStepOrigDisp,
                    (DWORD)&g_talkStep, 5.0 / g_talkStep, 10.0 / g_talkStep);
            LogLine(l);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#endif  // ENABLE_TALK_TIMER

#if ENABLE_GYSAHL_DIAG
// ---- FA-object schedule diagnostic ----------------------------------------
// The Gysahl Green plot bug: planting sets the PLOT inert (script line 1139)
// and is then supposed to switch the separate PLANT entity on (line 1150).
// Both are the same call - Field.changeFaObjectSchedule - and the first works
// while the second sometimes does not. That native no-ops SILENTLY when its
// name lookup fails, so from the script side the failure is invisible.
//
//   FUN_009c0490:
//     iVar6 = FUN_0073cfb0(objName);       // resolve name -> string-table entry
//     if (iVar6 != 0) {                    // <-- everything is inside this
//         uVar7  = *(uint *)(iVar6 + 0x14) & 0xffff;      // u16Index
//         piVar9 = DAT_024d252c + 0x82450 + uVar7 * 0x30; // registry entry
//         ...write the new schedule...
//     }                                    // no else, returns void
//
// Script natives all take one arg: the VM context. Both string arguments are
// readable from its stack, per the same decompile:
//     stack = *(int *)(ctx + 0x958);
//     schedName = *(char **)(stack + 4);   objName = *(char **)(stack + 0xc);
//
// Logging the call alone cannot distinguish "the write failed" from "the write
// landed and something reverted it later", so this dumps the target's registry
// entry BEFORE and AFTER, using the return-address hijack already used by the
// pacer hooks. Indices come from r_fao_def.json (u16Index), which is why only
// the eight known farm entities are decoded - anything else still logs, just
// without a registry dump.
#define FA_SCHED_RVA        (0x009c0490 - 0x00400000)
#define FA_REGISTRY_PTR_RVA (0x024d252c - 0x00400000)
#define FA_REGISTRY_BASE_OFF 0x82450
#define FA_ENTRY_STRIDE      0x30

static volatile LONG g_logFaSchedule = 0;
static volatile LONG g_faSchedCalls = 0;
// Tentative definitions - the real ones sit with the window-trace block below,
// which needs to come after the detours it references. C merges them.
static volatile LONG g_plantWatchUntil;
static volatile LONG g_winWaitPolls;
static volatile LONG g_winClosePolls;
static volatile LONG g_faCmpFix;
static volatile LONG g_watchPlantBuf;
static void ArmWatchpointSelf(unsigned int addr);
static void DisarmWatchpointSelf(void);
static char g_faPlantPlot[40];
static const char *g_faPlantPlotPtr;
static void *g_faSchedTarget = NULL;
static void *g_trampoline_faSched = NULL;

// r_fao_def.json u16Index. Plots 0009-0012 -> 421-424, plants 0016-0019 -> 427-430.
static int FaIndexForName(const char *n)
{
    if (!n || strncmp(n, "pm_faoF03_00", 12) != 0) return -1;
    if (strcmp(n, "pm_faoF03_0009") == 0) return 421;
    if (strcmp(n, "pm_faoF03_0010") == 0) return 422;
    if (strcmp(n, "pm_faoF03_0011") == 0) return 423;
    if (strcmp(n, "pm_faoF03_0012") == 0) return 424;
    if (strcmp(n, "pm_faoF03_0016") == 0) return 427;
    if (strcmp(n, "pm_faoF03_0017") == 0) return 428;
    if (strcmp(n, "pm_faoF03_0018") == 0) return 429;
    if (strcmp(n, "pm_faoF03_0019") == 0) return 430;
    return -1;
}

// One pending call at a time: the script VM is single-threaded and this native
// is a leaf, so it cannot nest. Thread id is recorded anyway so a surprise
// concurrent call is dropped rather than corrupting the pairing.
static DWORD g_faPendTid = 0;
static DWORD g_faPendRet = 0;
static char  g_faPendObj[40];
static char  g_faPendSch[40];
static int   g_faPendIdx = -1;
static DWORD g_faPendBefore[6];

static DWORD *FaEntryPtr(int idx)
{
    if (idx < 0 || g_mainModBase == 0) return NULL;
    DWORD regPtr = *(DWORD *)(g_mainModBase + FA_REGISTRY_PTR_RVA);
    if (!regPtr) return NULL;
    return (DWORD *)(regPtr + FA_REGISTRY_BASE_OFF + (DWORD)idx * FA_ENTRY_STRIDE);
}

static void FaSnapshot(int idx, DWORD *out6)
{
    int i;
    for (i = 0; i < 6; i++) out6[i] = 0xDEADBEEF;
    __try {
        DWORD *e = FaEntryPtr(idx);
        if (!e) return;
        for (i = 0; i < 6; i++) out6[i] = e[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// v2. The first build logged NOTHING despite the hook installing correctly.
// Cause was here, not in the game: the whole body sat in ONE __try, so when
// the stack-argument guess faulted, control jumped straight to __except and
// the ECX fallback never executed. Each candidate now gets its own guarded
// probe, and the first 20 calls dump their raw inputs so a wrong convention
// is visible in the log instead of silently producing nothing.
static volatile LONG g_faRawLogged = 0;

// UNSIGNED throughout. v2 read the context correctly and then threw it away:
// the probe returned 0xDD81F228 (this exe is large-address-aware, so its heap
// sits above 2GB) and the validity test `stack > 0x10000` was a SIGNED compare,
// which is false for any pointer with the high bit set. Every call failed the
// check despite the read having worked.
static unsigned int ProbeVmStack(void *ctx)
{
    unsigned int v = 0;
    if (!ctx) return 0;
    __try { v = *(unsigned int *)((unsigned int)ctx + 0x958); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

// Returns 1 to request the return hijack (so the AFTER snapshot can be taken).
__declspec(noinline) int __cdecl OnEnter_faSched_C(void *ctxStack, void *ctxEcx, void *retAddr)
{
    // Runs for the fix as well as for logging: the repair needs the plot name
    // remembered from the hatake3 write even when diagnostics are off.
    if ((!g_logFaSchedule && !g_faCmpFix) || g_mainModBase == 0) return 0;

    unsigned int stackA = ProbeVmStack(ctxStack);
    unsigned int stackB = ProbeVmStack(ctxEcx);
    unsigned int ctx = 0, stack = 0;
    if (stackA > 0x10000)      { ctx = (unsigned int)ctxStack; stack = stackA; }
    else if (stackB > 0x10000) { ctx = (unsigned int)ctxEcx;   stack = stackB; }

    if (g_logFaSchedule && InterlockedIncrement(&g_faRawLogged) <= 20) {
        char l[220];
        sprintf(l, "[fa-raw] #%ld ctxStack=%p ctxEcx=%p stackA=0x%08X stackB=0x%08X -> ctx=0x%08X",
                g_faRawLogged, ctxStack, ctxEcx, (unsigned)stackA, (unsigned)stackB,
                (unsigned)ctx);
        LogLine(l);
    }
    if (!ctx) return 0;

    __try {
        const char *sch = *(const char **)(stack + 4);
        const char *obj = *(const char **)(stack + 0xc);
        if (!obj || !sch) return 0;

        // Log the first few name pairs unfiltered too - if the argument slots
        // are not where the decompile suggested, that shows up here as
        // garbage rather than as an empty log.
        if (g_logFaSchedule && g_faRawLogged <= 20) {
            char l[220];
            sprintf(l, "[fa-raw]   obj='%.32s' sch='%.32s'", obj, sch);
            LogLine(l);
        }

        if (strncmp(obj, "pm_fao", 6) != 0) return 0;   // farm/field objects only

        // Arm on the plot going inert - that IS the planting action, and the
        // name is still intact at this moment.
        if (strcmp(sch, "fsh_fao_hatake3") == 0) {
            // Event-scoped, not time-scoped. The old 30s deadline was
            // arbitrary and silently disabled the repair if the player left
            // the message box open longer than that - a real hole, since the
            // captured data shows wait length has NO bearing on whether the
            // bug fires (bugged at 4 and 15 polls; healthy across 4-18).
            // The long deadline below is only a backstop so a planting that
            // never completes cannot leave this armed for the whole session.
            g_plantWatchUntil = (LONG)(GetTickCount() + 600000);
            g_winWaitPolls = 0;
            g_winClosePolls = 0;
            lstrcpynA(g_faPlantPlot, obj, sizeof(g_faPlantPlot));
            g_faPlantPlotPtr = obj;
            if (g_watchPlantBuf) ArmWatchpointSelf((unsigned int)obj);
            if (g_logFaSchedule)
                LogLine("[fa-win] --- planting started, watch armed ---");
        }
        // Disarm on success: the plant switching on is the last step of the
        // sequence, so nothing after it needs repairing.
        else if (strcmp(sch, "fsh_fao_yasai1") == 0 && g_plantWatchUntil != 0) {
            g_plantWatchUntil = 0;
            g_faPlantPlotPtr = NULL;
            g_faPlantPlot[0] = '\0';
            if (g_logFaSchedule)
                LogLine("[fa-win] --- plant activated, watch disarmed ---");
        }

        if (!g_logFaSchedule) return 0;   // fix-only mode: no before/after dump
        InterlockedIncrement(&g_faSchedCalls);
        g_faPendTid = GetCurrentThreadId();
        g_faPendRet = (DWORD)retAddr;
        lstrcpynA(g_faPendObj, obj, sizeof(g_faPendObj));
        lstrcpynA(g_faPendSch, sch, sizeof(g_faPendSch));
        g_faPendIdx = FaIndexForName(obj);
        FaSnapshot(g_faPendIdx, g_faPendBefore);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

__declspec(noinline) void *__cdecl OnReturn_faSched_C(void)
{
    void *ret = (void *)g_faPendRet;
    __try {
        if (g_faPendTid == GetCurrentThreadId()) {
            DWORD after[6];
            FaSnapshot(g_faPendIdx, after);
            int changed = 0, i;
            for (i = 0; i < 6; i++) if (after[i] != g_faPendBefore[i]) changed = 1;
            char line[320];
            sprintf(line,
                "[fa] %s -> %s | idx=%d | %s | before %08X %08X %08X %08X %08X %08X"
                " | after %08X %08X %08X %08X %08X %08X",
                g_faPendObj, g_faPendSch, g_faPendIdx,
                g_faPendIdx < 0 ? "NO-INDEX"
                                : (changed ? "CHANGED" : "*** UNCHANGED ***"),
                g_faPendBefore[0], g_faPendBefore[1], g_faPendBefore[2],
                g_faPendBefore[3], g_faPendBefore[4], g_faPendBefore[5],
                after[0], after[1], after[2], after[3], after[4], after[5]);
            LogLine(line);
            g_faPendTid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

__declspec(naked) void OnReturn_faSched(void)
{
    __asm {
        pushfd
        call OnReturn_faSched_C
        popfd
        jmp eax
    }
}

__declspec(naked) void Detour_faSched(void)
{
    __asm {
        push ecx
        push edx
        // After the two pushes: [esp+4]=saved ECX, [esp+8]=return address,
        // [esp+12]=the native's stack arg (the VM context). ECX is passed too
        // in case the convention differs from what the decompiler inferred.
        // __cdecl args go right-to-left, so push retAddr, ecx, ctx in that
        // order to land as (ctxStack, ctxEcx, retAddr).
        mov eax, [esp + 8]      // return address        -> 3rd param
        push eax
        mov eax, [esp + 8]      // saved ECX             -> 2nd param
        push eax
        mov eax, [esp + 20]     // VM context stack arg  -> 1st param
        push eax
        call OnEnter_faSched_C
        add esp, 12
        test eax, eax
        jz skip_faSched
        mov dword ptr [esp + 8], offset OnReturn_faSched
    skip_faSched:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_faSched]
    }
}

// ---- Script timer-callback registration, same investigation ---------------
// The first bugged planting captured in the log showed the PLOT write
// (script line 1139) present and the PLANT write (line 1154) ENTIRELY ABSENT -
// not a silent no-op, simply never called. So execution stopped, or the branch
// was never taken, between those two points:
//
//   1151  } else if (sfIsSameStrings(string, "pm_faoF03_0010")) {
//   1152      sfSetTimerCallBack("YasaiTimer2");      <-- this hook
//   1153      sfSetTimerCallBack("YasaiTimerH2");     <-- and this
//   1154      sfSetFaAiSch("pm_faoF03_0018", ...);    <-- known missing
//
// The timers are registered BEFORE the failing call, so their presence splits
// "entered the branch" from "never got there". Entry-only: we only need to
// know whether it ran, not what it returned.
//
// Prologue verified in the shipped exe: 55 8B EC 83 EC 08, and 8B 55 08 /
// 8B 8A 58 09 00 00 confirms the arg really is a stack parameter holding the
// VM context read at +0x958 - the same layout this file already assumes.
#define TIMER_CB_RVA (0x009c22a0 - 0x00400000)
static void *g_trampoline_timerCb = NULL;

__declspec(noinline) void __cdecl OnEnter_timerCb_C(void *ctxStack)
{
    if (!g_logFaSchedule) return;
    unsigned int stack = ProbeVmStack(ctxStack);
    if (stack <= 0x10000) return;
    __try {
        const char *name = *(const char **)(stack + 4);
        if (!name) return;
        // Only the farm timers matter here; the game registers many others.
        if (strncmp(name, "Yasai", 5) != 0) return;
        char l[120];
        sprintf(l, "[fa-timer] setRelativeTimerCallback('%.32s')", name);
        LogLine(l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void Detour_timerCb(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 12]     // VM context stack arg
        push eax
        call OnEnter_timerCb_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_timerCb]
    }
}

// ---- Bisecting sfShowWindowWithKeyWait ------------------------------------
// Runtime capture: plot write (line 1139) present, timers and plant write
// ABSENT. So execution dies between 1139 and 1152, and the only substantial
// thing there is the blocking message box at line 1140:
//
//   1866  n = Window.showMessageWindow(...)
//   1868  while (Window.isWindowOpening(n))              { delay(); }        loop 1
//   1872  while (Window.isWaitingDecideOrCancel(name))   { delay(); delay(); } loop 2
//   1877  Window.hideWindow(n)
//   1878  while (Window.isWindowClosing(name))           { delay(); }        loop 3
//
// All three predicates were checked in the disassembly and all return FALSE
// for a missing window (isWaitingDecideOrCancel zeroes EBX up front and only
// sets it on the success path), so none can spin on a destroyed window - the
// hang theory is dead. Bisect it live instead: hideWindow is called exactly
// once, between loops 2 and 3, so its presence splits "stuck in loop 1/2"
// from "the wait finished and the problem is after line 1140".
//
// Only traced while a planting is in flight - armed by the hatake3 write - so
// the game's constant window traffic does not drown the signal.
#define WIN_SHOW_RVA  (0x009da120 - 0x00400000)
#define WIN_HIDE_RVA  (0x009d7c70 - 0x00400000)
#define WIN_WAIT_RVA  (0x009dac70 - 0x00400000)

static void *g_trampoline_winShow = NULL;
static void *g_trampoline_winHide = NULL;
static void *g_trampoline_winWait = NULL;
// (declared tentatively up in the FA-schedule block, which references them)

static int PlantWatchActive(void)
{
    LONG until = g_plantWatchUntil;
    return until != 0 && (LONG)(GetTickCount() - (DWORD)until) < 0;
}

__declspec(noinline) void __cdecl OnWinShow_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    g_winWaitPolls = 0;
    LogLine("[fa-win] showMessageWindow");
}

__declspec(noinline) void __cdecl OnWinHide_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    char l[96];
    sprintf(l, "[fa-win] hideWindow  (loop2 polls=%ld)", g_winWaitPolls);
    LogLine(l);
}

// Spin detector: loop 2 polls this. A healthy wait is however many frames the
// player takes to press the button; an unbounded count means we never left it.
__declspec(noinline) void __cdecl OnWinWait_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    LONG n = InterlockedIncrement(&g_winWaitPolls);
    if (n == 1 || n == 100 || n == 500 || (n % 2000) == 0) {
        char l[96];
        sprintf(l, "[fa-win] isWaitingDecideOrCancel poll #%ld", n);
        LogLine(l);
    }
}

__declspec(naked) void Detour_winShow(void)
{
    __asm {
        pushad
        pushfd
        call OnWinShow_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winShow]
    }
}
__declspec(naked) void Detour_winHide(void)
{
    __asm {
        pushad
        pushfd
        call OnWinHide_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winHide]
    }
}
__declspec(naked) void Detour_winWait(void)
{
    __asm {
        pushad
        pushfd
        call OnWinWait_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winWait]
    }
}

// ---- Final bisection: loop 3, and the branch dispatch after it ------------
// hideWindow is REACHED in the bugged case, so loops 1 and 2 complete. All
// four wait predicates were disassembled and every one returns false for a
// missing window, so none can spin for that reason. Two possibilities remain:
//
//   * loop 3 spins for some other reason
//         while (Window.isWindowClosing(name)) { White.delay(); }
//   * the wait returns fine and the script simply stops being resumed, i.e.
//     the VM drops the coroutine - in which case nothing after line 1140 runs
//
// isWindowClosing gets a poll counter (an unbounded climb = stuck in loop 3).
// White.stringComp is what sfIsSameStrings compiles to, so it is the branch
// dispatch at script lines 1147/1151 - seeing it prove the chain was reached
// separates "died in the wait" from "died in the dispatch".
#define WIN_CLOSING_RVA (0x009db870 - 0x00400000)
#define STRCMP_RVA      (0x009d2130 - 0x00400000)
static void *g_trampoline_winClosing = NULL;
static void *g_trampoline_strCmp = NULL;
static volatile LONG g_winClosePolls;

__declspec(noinline) void __cdecl OnWinClosing_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    LONG n = InterlockedIncrement(&g_winClosePolls);
    if (n == 1 || n == 100 || n == 500 || (n % 2000) == 0) {
        char l[96];
        sprintf(l, "[fa-win] isWindowClosing poll #%ld", n);
        LogLine(l);
    }
}

// ---- CONFIRMED MECHANISM (capture v30, log line 23156) --------------------
// Bugged planting: the branch chain RAN, but the script's `string` parameter
// was an EMPTY STRING after the blocking message box:
//     stringComp('','pm_faoF03_0009') ... ('','pm_faoF03_0012')  - all fail
// versus healthy: stringComp('pm_faoF03_0009','pm_faoF03_0009').
// The same variable was intact at the hatake3 plot write moments earlier, so
// its backing storage is invalidated DURING the multi-frame wait. With no
// else clause in the script, all four compares miss and the plant is never
// activated. Root cause of the Gysahl Green plot bug.
//
// The repair: the plot name is known - it is the object of the hatake3 write
// we just logged. When, inside the plant-watch window, a compare arrives with
// an EMPTY first arg and a plot literal as the second, rewrite the first-arg
// VM cell to point at the remembered name. The native then routes the script
// into the correct branch itself; lines 1148-1163 use only literals, so the
// dead variable is never needed again. Tightly scoped + loudly logged.
static volatile LONG g_faCmpFix = 1;          // "FaCmpFix" toggle
static volatile LONG g_faCmpFixCount = 0;
static char g_faPlantPlot[40];                // last plot set to hatake3
static const char *g_faPlantPlotPtr = NULL;   // the pointer the VM used then
static char g_faRepairBuf[40];                // stable storage for the rewrite

// Two string args, same VM stack layout as changeFaObjectSchedule:
// arg2 (literal) at +4, arg1 (the script variable) at +0xc.
__declspec(noinline) void __cdecl OnStrCmp_C(void *ctxStack)
{
    if ((!g_logFaSchedule && !g_faCmpFix) || !PlantWatchActive()) return;
    unsigned int stack = ProbeVmStack(ctxStack);
    if (stack <= 0x10000) return;
    __try {
        const char *b = *(const char **)(stack + 4);
        const char *a = *(const char **)(stack + 0xc);
        if (!a || !b) return;
        if (strncmp(a, "pm_faoF03_", 10) != 0 && strncmp(b, "pm_faoF03_", 10) != 0) return;
        if (g_logFaSchedule) {
            // Pointers included: same pointer as the plot write but empty
            // content = backing storage zeroed; different pointer = the VM
            // stack slot itself was overwritten. Distinguishes the two
            // remaining explanations for free on the next capture.
            char l[200];
            sprintf(l, "[fa-cmp] stringComp(a=%p'%.24s', b=%p'%.24s') plotPtr=%p",
                    a, a, b, b, g_faPlantPlotPtr);
            LogLine(l);
        }
        // Repair, gated as tightly as the evidence allows:
        //   1. armed  - a planting is in flight and has not completed
        //   2. a == the EXACT buffer the plot write used (every capture shows
        //      the script re-reads the same address, 006FF39C, on all four
        //      compares, so this holds for the whole chain)
        //   3. that buffer is genuinely empty
        //   4. the comparand is a plot literal
        // Condition 2 is what makes this safe: without it the rule was "any
        // empty string vs any plot name", which could catch an unrelated
        // script. With it we only ever touch the one buffer we watched the
        // game populate seconds earlier.
        // v33: test for MISMATCH, not emptiness. The first capture happened to
        // show an empty buffer, so the guard was written as a[0]=='\0' - too
        // narrow. A later capture had the same buffer holding the raw bytes
        // A8 E0 7F 01, i.e. the pointer 0x017FE0A8 written into it. The
        // clobber is "this stack slot got reused", and the reused content is
        // arbitrary; sometimes it is zeros, sometimes a pointer. Comparing
        // against the remembered name covers every form.
        if (g_faCmpFix && a == g_faPlantPlotPtr && g_faPlantPlot[0] != '\0' &&
            strcmp(a, g_faPlantPlot) != 0 && strncmp(b, "pm_faoF03_00", 12) == 0) {
            lstrcpynA(g_faRepairBuf, g_faPlantPlot, sizeof(g_faRepairBuf));
            // Point the VM's argument cell at our copy. We never write into
            // the game's own buffer: if that stack frame has been handed to
            // something else, writing there would corrupt whatever now owns it.
            *(const char **)(stack + 0xc) = g_faRepairBuf;
            InterlockedIncrement(&g_faCmpFixCount);
            // Dump what the clobber actually left behind. Zeros vs a pointer
            // vs something else is the best lead we have on WHO reuses this
            // stack slot, and it costs nothing to collect on every repair.
            unsigned char *raw = (unsigned char *)a;
            char l[220];
            sprintf(l, "[fa-FIX] repaired to '%s' (vs '%.24s') fix#%ld "
                       "| clobber bytes %02X %02X %02X %02X %02X %02X %02X %02X",
                    g_faRepairBuf, b, g_faCmpFixCount,
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
            LogLine(l);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void Detour_winClosing(void)
{
    __asm {
        pushad
        pushfd
        call OnWinClosing_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winClosing]
    }
}
__declspec(naked) void Detour_strCmp(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 12]
        push eax
        call OnStrCmp_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_strCmp]
    }
}

// ---- Which yield/resume actually corrupts the buffer? ---------------------
// The captured poll counts kill the "every re-entry is a chance" model:
// bugged at 4 and 15 loop iterations, healthy across 4-18. A per-yield dice
// roll would compound with iteration count and cluster the failures at high
// counts. Flat distribution = a SINGLE event at one specific transition.
//
// So find that transition. FUN_009e3670 is the JavaVmCode.cpp interpreter -
// the function whose prologue pushes 0x00D9E0A8, the exact value that ends up
// in the plot-name buffer:
//     55 8B EC        PUSH EBP / MOV EBP,ESP
//     6A FF           PUSH -1
//     68 A8 E0 D9 00  PUSH 0x00D9E0A8      <-- the clobber, in the prologue
//
// Log the tracked buffer on every interpreter entry, but ONLY when its
// contents change - so a whole planting produces two or three lines instead of
// thousands, and the exact entry that destroys it is named. ESP is captured
// too: if the corrupting entry sits at a different stack depth than the
// healthy ones, that is the frame-layout hypothesis confirmed.
// ---- REMOVED: VM interpreter trace (v34) ----------------------------------
// Hooking FUN_009e3670 (the JavaVmCode.cpp bytecode interpreter) BROKE THE
// GAME: with it installed, the planting message box accepted no input and the
// game looped an error sound. That function is re-entered constantly and at
// wildly varying stack depths, and the VM's yield/resume relies on reproducing
// its frame layout exactly - a detour that pushes 36 bytes and makes a call on
// every entry perturbs the very mechanism it was measuring. Do not re-add it.
//
// It did earn its keep before being removed. The trace showed the tracked
// buffer does NOT get destroyed once: it OSCILLATES, dozens of times per
// planting, between the plot name and other frames' data -
//     entry #618  <junk>          -> pm_faoF03_0011   (restored)
//     entry #626  pm_faoF03_0011  -> <junk>           (clobbered)
//     entry #642  <junk>          -> pm_faoF03_0011   (restored)
//     ... and the compares happened to land on a restored moment, so it worked.
// So the VM does save/restore script state across yields, and the bug is a
// RACE: whether the branch compares fall on a restored or a clobbered moment.
// ESP deltas between entries ranged over 1600 bytes, so the same address falls
// inside different frames depending on entry depth.
//
// The right tool for the remaining question is a hardware watchpoint (debug
// registers DR0-DR3): it traps writes without adding a single instruction to
// the hot path, so it cannot perturb what it observes.

// ---- Hardware watchpoint on the plot-name buffer --------------------------
// The software probe (v34, hooking the interpreter) broke the game: it added
// instructions to a hot, re-entrant path whose frame layout is exactly what
// the bug depends on. Debug registers avoid that entirely - the CPU traps the
// write itself, so ZERO instructions are added to the observed code.
//
// DR0 holds the address, DR7 configures it (local enable, write-only, 4 bytes),
// and a vectored exception handler catches the resulting EXCEPTION_SINGLE_STEP
// and records the EIP of whoever wrote. That names the writer directly instead
// of inferring it.
//
// Armed on the hatake3 plot write (we know the buffer address then), capped at
// WATCH_MAX_HITS so the oscillation cannot flood the log or the exception
// machinery, and disarmed when the plant activates.
#define WATCH_MAX_HITS 40
static volatile LONG g_watchPlantBuf = 0;
static volatile LONG g_watchHits = 0;
static PVOID g_vehHandle = NULL;
static unsigned int g_watchAddr = 0;

static void DisarmWatchpointSelf(void)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) return;
    c.Dr0 = 0;
    c.Dr7 &= ~(DWORD)0xF0003;
    SetThreadContext(th, &c);
    g_watchAddr = 0;
}

static void ArmWatchpointSelf(unsigned int addr)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) { LogLine("[fa-wp] GetThreadContext failed"); return; }
    addr &= ~3u;                       // LEN=4 requires 4-byte alignment
    c.Dr0 = addr;
    c.Dr6 = 0;
    // clear DR0's old config, then: L0=1, RW0=01 (write), LEN0=11 (4 bytes)
    c.Dr7 = (c.Dr7 & ~(DWORD)0xF0003) | 0x1 | (0x1u << 16) | (0x3u << 18);
    if (!SetThreadContext(th, &c)) { LogLine("[fa-wp] SetThreadContext failed"); return; }
    g_watchAddr = addr;
    g_watchHits = 0;
    {
        // Read back - setting debug registers on the CURRENT thread is not
        // formally guaranteed, so prove it took rather than assume.
        CONTEXT v;
        memset(&v, 0, sizeof(v));
        v.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        char l[160];
        if (GetThreadContext(th, &v))
            sprintf(l, "[fa-wp] armed at %08X | readback Dr0=%08X Dr7=%08X tid=%lu",
                    addr, (unsigned)v.Dr0, (unsigned)v.Dr7, GetCurrentThreadId());
        else
            sprintf(l, "[fa-wp] armed at %08X (readback failed)", addr);
        LogLine(l);
    }
}

static LONG CALLBACK PlantWatchVeh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if ((ep->ContextRecord->Dr6 & 0x1) == 0)
        return EXCEPTION_CONTINUE_SEARCH;   // not our DR0 breakpoint

    LONG n = InterlockedIncrement(&g_watchHits);
    if (n <= WATCH_MAX_HITS) {
        unsigned int eip = (unsigned int)ep->ContextRecord->Eip;
        unsigned int ghidra = (g_mainModBase && eip > g_mainModBase)
                              ? (eip - g_mainModBase + 0x00400000) : 0;
        const unsigned char *p = (const unsigned char *)g_watchAddr;
        char l[240];
        // A data breakpoint traps AFTER the write, so EIP is the instruction
        // following the store - close enough to identify the writer.
        sprintf(l, "[fa-wp] hit #%ld eip=%08X (ghidra %08X) esp=%08X | now %02X %02X %02X %02X",
                n, eip, ghidra, (unsigned int)ep->ContextRecord->Esp,
                p[0], p[1], p[2], p[3]);
        LogLine(l);
    }
    ep->ContextRecord->Dr6 = 0;
    if (n >= WATCH_MAX_HITS) {
        ep->ContextRecord->Dr7 &= ~(DWORD)0xF0003;   // self-disarm
        LogLine("[fa-wp] hit cap reached, disarmed");
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif  // ENABLE_GYSAHL_DIAG

static volatile LONG g_deadlineClampCount = 0;
static void __cdecl ClampAc3040Deadline(void *pacer);
// Opt-in, default OFF. Rewrites PresentationInterval to IMMEDIATE at the
// game's own IDirect3D9::CreateDevice call. See the injection site
// (HookedIDirect3D9CreateDevice) for the full reasoning and risk - this
// targets the 60Hz-multiple frame-time clustering traced to the swap chain
// requesting D3DPRESENT_INTERVAL_DEFAULT (== ONE, i.e. vsync) while
// Windowed, but can desync any engine timing that assumes a fixed-cadence
// Present.
static volatile LONG g_forceImmediatePresentEnabled = 0;
// Opt-in, default OFF. Overwrites the engine's own frame-pacing target.
// See ApplyFramerateUnlock() for the decompiled limiter and the risks.
static volatile LONG g_unlockFramerateEnabled = 1;
static volatile LONG g_shaderThrottleEnabled = 1;
// Default ON - confirmed engaging (bytes prefetched) but never shown to
// measurably help; runtime-toggleable for benchmark isolation like
// everything else, not because it's suspected of causing a problem.
static volatile LONG g_allocatorWarmEnabled = 1;
// Alternative to the DISCARD fix, not a companion to it - see the injection
// site for the reasoning. Default OFF, untested until a user tries it.
// g_noOverwriteFixEnabled removed - the NOOVERWRITE alternative it gated
// caused graphical corruption AND a game crash under user testing. See the
// retirement note at its former injection site (search PROGRESS.md for
// "NOOVERWRITE") for the full account. Deliberately not left as a disabled
// flag - no toggle for this exists anymore.
static void *PatchIat(HMODULE hostModule, const char *moduleName, const char *funcName, void *newFunc);

// The v6/v7 allocation census + memory warmer is defined further down, next
// to the engine-allocator hook it grew out of, but the OS-level allocation
// hooks sit above it and use it - hence these forward declarations.
#define ALLOC_SRC_NAMEDHEAP 0
#define ALLOC_SRC_HEAP      1
#define ALLOC_SRC_VIRTUAL   2
#define ALLOC_SRC_MAPVIEW   3
#define ALLOC_SRC_READFILE  4
#define NUM_ALLOC_SRC       5
static void CensusRecord(int src, size_t bytes);
static void EnqueueWarm(void *ptr, size_t size);
static DWORD WINAPI WarmerThread(LPVOID param);
// Tentative definitions - the real ones sit with the rest of the census
// state further down; C merges them into a single object.
// Texture churn / upload-volume probe counters follow the same pattern: they
// are written deep in the D3D9 hooks but read by the monitor loop, which
// sits earlier in the file.
static volatile LONG g_createTexCount;
static volatile LONG g_texDestroyCount;
static volatile LONG g_uploadKB;
// Split upload volume by thread. Decides whether upload volume can be
// THROTTLED at all: if the engine's memcpy into our staging surfaces runs on
// the main thread, delaying it just moves the stall into the frame we are
// trying to protect. If it runs on loader/worker threads, a byte-rate limit
// on uploads becomes viable the same way PaceRead limits file reads.
static volatile LONG g_uploadKBMain;

static volatile LONG g_mainThreadId;
static DWORD g_censusTls;
static CRITICAL_SECTION g_allocThreadLock;
static CRITICAL_SECTION g_allocImplLock;
static CRITICAL_SECTION g_warmLock;
static double g_cyclesPerUsec;
static volatile LONG g_readFileCount;
static volatile LONGLONG g_readFileBytes;
static volatile LONG g_namedHeapCount;
#define MAX_THREAD_NAMES 64
typedef struct {
    DWORD threadId;
    char name[64];
} ThreadNameEntry;
static ThreadNameEntry g_threadNames[MAX_THREAD_NAMES];
static volatile LONG g_threadNameCount;
static CRITICAL_SECTION g_threadNameLock;
static volatile LONG g_shaderCompileTotal;
static LONG g_shaderIdCount;
static volatile LONG g_shaderCompileRepeats;
static volatile LONG g_readPaceDelayTotalUsec; // monotonic; the windowed one is reset by the monitor
static volatile LONG g_mainCsWaitStartUsec;
static volatile LONG g_mainCsPtr;
static volatile LONG g_mainCsOwner;
static volatile LONG g_mainWfsoStartUsec;
static volatile LONG g_mainWfsoHandle;
static volatile LONG g_mainCsWaitTotalUsec;
static volatile LONG g_mainCsWaitCount;
static volatile LONG g_mainWfsoWaitTotalUsec;
static unsigned int g_mainModBase;
// Moved up from beside ApplyFramerateUnlock so ClampAc3040Deadline (which
// sits earlier in the file, right at the Detour_ac3040 hook) can see them
// too - both features read/write the same engine-owned ticksPerFrame value.
#define FRAME_TARGET_TICKS_RVA (0x05115570 - 0x00400000)
#define FRAME_TARGET_INIT_RVA  (0x05115578 - 0x00400000)
static unsigned int g_mainModSize;
static LONG NowUsec(void);

static void LogD3DWindow(void);
static int InstallD3D9Hook(void);
static unsigned __int64 g_tscBase;
static DWORD WINAPI StutterWatchdogThread(LPVOID param);

// ---- Loader dispatch throttle (call-site patch, NOT entry/return hijack) --
// FUN_004b5cb0 (the "process one loader request" dispatch switch, found at
// the very start of this investigation) has the same SEH prologue pattern
// that caused the earlier crash when hooked via return-address hijacking
// (FUN_00a01a00/FUN_00a015b0). Its only caller is FUN_004b68e0 (the Loader
// thread's own main loop), which has no SEH scaffolding, and FUN_004b5cb0
// is __fastcall with a single register argument (no stack args at all) -
// so instead of touching either function's entry/return path, this patches
// the single `CALL FUN_004b5cb0` instruction at its one call site
// (Ghidra VA 0x004b6a35, confirmed via Ghidra to be the only reference to
// this function anywhere in the binary) to redirect through a semaphore-
// throttling wrapper. FUN_004b5cb0 itself is never modified and keeps its
// own SEH fully intact - our wrapper brackets a normal, real nested CALL
// to it (standard CALL/RET, no manual return-address manipulation, so an
// exception propagating out of FUN_004b5cb0 unwinds through our wrapper's
// stack frame exactly like any other nested call, no special handling
// needed). Verified the code immediately after the call site reloads
// everything it needs from memory rather than relying on any register
// surviving across the call, so it's safe for our wrapper to leave
// EAX/ECX/EDX in whatever state our own Acquire/Release calls happen to
// leave them (matches how a void function's return already offers no
// register guarantees anyway).

#define LOADER_DISPATCH_CALL_SITE_RVA (0x004b6a35 - 0x00400000)
#define LOADER_DISPATCH_FUNC_RVA (0x004b5cb0 - 0x00400000)
#define LOADER_THROTTLE_MAX 2

// ---- Bulk-load auto-bypass ----------------------------------------------
// User confirmed the bandwidth throttles genuinely help the chunk-load
// stutters (so the trigger really is the game loading too much at once), but
// they also make LOADING SCREENS much slower - which is pure downside, since
// nobody is looking at a frame counter there.
//
// The two cases are distinguishable without finding the loader functions at
// all: a loading screen is SUSTAINED bulk reading, while traversal streaming
// is intermittent bursts between quiet periods. So the throttles bypass
// themselves whenever read volume stays above a bulk threshold for two
// consecutive monitor windows, and re-engage once it drops for two.
//
// Hysteresis (2 windows each way) rather than an instant flip: a single burst
// during traversal must not disable the throttle for the very stutter it
// exists to prevent, and one quiet window mid-load must not re-throttle a
// load that is still running.
#define BULK_LOAD_BYTES_PER_WINDOW (6 * 1024 * 1024)   // ~12MB/s sustained
static volatile LONG g_bulkLoadActive = 0;

static HANDLE g_loaderThrottleSem = NULL;
static void *g_realLoaderDispatch = NULL;
static volatile LONG g_loaderDispatchCount = 0;
// Default ON (attempt 6) - never conclusively shown to help on its own, but
// never shown to cause any problem either. Runtime-toggleable for benchmark
// isolation, same as everything else.
// OFF: still unconcluded. One machine measured a null, but beta testers
// report a possible DX9 effect and dispatch/priority behaviour depends on
// core count and contention. Off is the honest default until a real A/B
// settles it - see the note at the top of this file.
static volatile LONG g_loaderThrottleEnabled = 0;
// Acquire/Release must agree on whether THIS dispatch actually took a
// semaphore slot, even if the flag changes in between - otherwise toggling
// mid-dispatch could Acquire (flag ON) then skip the matching Release (flag
// now OFF), permanently leaking a slot and eventually deadlocking the
// throttle once re-enabled. TLS records the decision made at Acquire time so
// Release always mirrors it, regardless of the flag's value by then.
static DWORD g_loaderThrottleAcquiredTls;

static void __cdecl AcquireLoaderThrottle(void)
{
    int acquiring = (g_loaderThrottleEnabled != 0) && !g_bulkLoadActive;
    TlsSetValue(g_loaderThrottleAcquiredTls, (LPVOID)(UINT_PTR)acquiring);
    if (acquiring) {
        WaitForSingleObject(g_loaderThrottleSem, INFINITE);
    }
}

static void __cdecl ReleaseLoaderThrottle(void)
{
    InterlockedIncrement(&g_loaderDispatchCount);
    if ((UINT_PTR)TlsGetValue(g_loaderThrottleAcquiredTls)) {
        ReleaseSemaphore(g_loaderThrottleSem, 1, NULL);
    }
}

// ---- Dynamic allocation tracking + prefetch --------------------------------
// Rather than reverse-engineering every loader request type's exact buffer
// layout (dozens of cases in FUN_004b5cb0's switch, each different), track
// whatever memory the game's own allocators (HeapAlloc/VirtualAlloc, both
// confirmed imported by name) actually hand out WHILE a dispatch is
// in-flight on this thread, then prefetch exactly those regions the moment
// the dispatch returns - before the job system gets around to touching them
// later. This generalizes automatically to whichever request type turns
// out to matter, without needing to know its structure.

#define MAX_TRACKED_ALLOCS 256
typedef struct {
    void *ptr;
    size_t size;
} TrackedAlloc;
typedef struct {
    int inDispatch;
    int count;
    TrackedAlloc allocs[MAX_TRACKED_ALLOCS];
} DispatchTrackState;

static DWORD g_dispatchTrackTls;
static volatile LONG g_prefetchedAllocCount = 0;
static volatile LONGLONG g_prefetchedByteCount = 0;

static DispatchTrackState *GetDispatchTrackState(void)
{
    DispatchTrackState *st = (DispatchTrackState *)TlsGetValue(g_dispatchTrackTls);
    if (!st) {
        st = (DispatchTrackState *)calloc(1, sizeof(DispatchTrackState));
        if (st) TlsSetValue(g_dispatchTrackTls, st);
    }
    return st;
}

static void __cdecl BeginDispatchTracking(void)
{
    DispatchTrackState *st = GetDispatchTrackState();
    if (st) {
        st->inDispatch = 1;
        st->count = 0;
    }
}

static void __cdecl EndDispatchTrackingAndPrefetch(void)
{
    DispatchTrackState *st = GetDispatchTrackState();
    if (!st) return;
    st->inDispatch = 0;
    for (int i = 0; i < st->count; i++) {
        unsigned char *p = (unsigned char *)st->allocs[i].ptr;
        size_t sz = st->allocs[i].size;
        if (!p || sz == 0 || sz > 16 * 1024 * 1024) continue; // sanity cap only
        // v6: no prefetch here any more. Every allocation this loop would
        // have covered is already enqueued to the warmer thread from
        // OnAllocatorReturn (a strictly wider net), and warming it a second
        // time inline on the loader thread would only duplicate the work.
        // The counters are kept purely as the "how much was allocated inside
        // a loader dispatch" signal, to compare against the per-thread census.
        InterlockedIncrement(&g_prefetchedAllocCount);
        g_prefetchedByteCount += (LONGLONG)sz;
    }
    st->count = 0;
}

static void RecordAllocIfTracking(void *ptr, size_t size)
{
    if (!ptr || size == 0) return;
    DispatchTrackState *st = (DispatchTrackState *)TlsGetValue(g_dispatchTrackTls);
    if (!st || !st->inDispatch) return;
    if (st->count < MAX_TRACKED_ALLOCS) {
        st->allocs[st->count].ptr = ptr;
        st->allocs[st->count].size = size;
        st->count++;
    }
}

typedef LPVOID(WINAPI *PFN_HeapAlloc)(HANDLE, DWORD, SIZE_T);
static PFN_HeapAlloc g_realHeapAlloc = NULL;

static LPVOID WINAPI HookedHeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
{
    LPVOID result = g_realHeapAlloc(hHeap, dwFlags, dwBytes);
    RecordAllocIfTracking(result, dwBytes);
    if (result) CensusRecord(ALLOC_SRC_HEAP, dwBytes);
    return result;
}

typedef LPVOID(WINAPI *PFN_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static PFN_VirtualAlloc g_realVirtualAlloc = NULL;

static LPVOID WINAPI HookedVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    LPVOID result = g_realVirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    // Only track actual commits, not pure reservations - a reserved-but-
    // uncommitted region has no real content yet, prefetching it would be
    // pointless busywork (though harmless - PREFETCH* never faults).
    if ((flAllocationType & MEM_COMMIT) != 0) {
        RecordAllocIfTracking(result, dwSize);
        if (result) {
            CensusRecord(ALLOC_SRC_VIRTUAL, dwSize);
            if ((LONG)GetCurrentThreadId() != g_mainThreadId) EnqueueWarm(result, dwSize);
        }
    }
    return result;
}

// MapViewOfFile / ReadFile: the two remaining bulk paths the exe imports.
// A memory-mapped view is the classic way a 2013-era streaming engine gets
// archive contents into the address space, and its pages fault in on first
// touch - a signature that would look exactly like "cold data sourced from
// far away" to the animation code touching it, while never appearing in any
// allocator at all. Both are plain WINAPI, so both are safe IAT patches.

typedef LPVOID(WINAPI *PFN_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
static PFN_MapViewOfFile g_realMapViewOfFile = NULL;

static LPVOID WINAPI HookedMapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                                         DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                                         SIZE_T dwNumberOfBytesToMap)
{
    LPVOID result = g_realMapViewOfFile(hFileMappingObject, dwDesiredAccess,
                                        dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
    if (result) {
        SIZE_T size = dwNumberOfBytesToMap;
        if (size == 0) {
            // 0 means "to end of mapping" - ask the memory manager how big
            // the view actually turned out to be.
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(result, &mbi, sizeof(mbi)) == sizeof(mbi)) size = mbi.RegionSize;
        }
        CensusRecord(ALLOC_SRC_MAPVIEW, size);
        if ((LONG)GetCurrentThreadId() != g_mainThreadId) EnqueueWarm(result, size);
    }
    return result;
}

typedef BOOL(WINAPI *PFN_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static PFN_ReadFile g_realReadFile = NULL;

// ---- ReadFile rate limiter (fix attempt 8) --------------------------------
// v7's census found the bulk path: ReadFile on the Loader threads, peaking at
// 19.1MB per 500ms (~38MB/s) in ~87KB calls, while HeapAlloc, VirtualAlloc
// and MapViewOfFile were flat zero for the entire session.
//
// This reframes the whole problem. Every fix so far assumed the NEW data is
// cold and tried to warm it. But a ~19MB burst of file data (plus whatever
// ZDecode expands it into) sweeping through the 96MB L3 does something worse:
// it EVICTS the working set of the actors already on screen. The next frames'
// animation and physics jobs then have to re-fetch their own, previously
// warm, data from DRAM. That is precisely the measured signature - same
// instruction count, IPC down 24%, DRAM-sourced fills up 554%, extra time
// attributed to the game's own compute functions rather than the kernel - and
// it explains why five attempts at prefetching the newcomer changed nothing:
// the newcomer being cold was never the problem.
//
// Total bytes cannot be reduced from outside the engine, but the RATE can.
// Spreading the same transfer over more time lowers the eviction pressure per
// unit time and lets L3 re-warm between bursts. Loader threads are already
// BELOW_NORMAL and stream ahead of the player, so trading load latency for
// frame time is the right direction - a slightly later asset is invisible, a
// 67ms frame is not.
//
// Token bucket, no read splitting: an 87KB read is trivial against a 96MB
// cache on its own, so only the aggregate rate needs shaping. Not splitting
// means every ReadFile call is passed through byte-for-byte unchanged, which
// avoids any question about partial reads, EOF handling, or two threads
// sharing a file handle.
#define READ_RATE_BYTES_PER_SEC (12 * 1024 * 1024)
#define READ_BURST_SECONDS 0.25

static double g_readTokens = 0.0;
static unsigned __int64 g_readLastTsc = 0;
static CRITICAL_SECTION g_readPaceLock;
static volatile LONG g_readPaceDelayUsec = 0;

// Confirmed via a direct A/B test: disabling this reproduced a 101ms
// stutter (reads=272/20575KB in a single frame, main thread parked in
// AMDXN32.DLL). Default ON; runtime-toggleable (checkbox/hotkey) for
// benchmarking, not because it's suspected of causing problems.
static volatile LONG g_readPaceEnabled = 1;

static void PaceRead(DWORD bytes)
{
    if (!g_readPaceEnabled) return;
    if (g_bulkLoadActive) return;   // loading screen - let it run at full speed
    if (g_cyclesPerUsec <= 0.0) return; // calibration not done yet - never stall
    const double cap = (double)READ_RATE_BYTES_PER_SEC * READ_BURST_SECONDS;
    unsigned __int64 waitStart = __rdtsc();
    for (;;) {
        int allowed;
        EnterCriticalSection(&g_readPaceLock);
        unsigned __int64 now = __rdtsc();
        if (g_readLastTsc == 0) g_readLastTsc = now;
        double elapsedSec = ((double)(now - g_readLastTsc) / g_cyclesPerUsec) / 1000000.0;
        g_readLastTsc = now;
        g_readTokens += elapsedSec * (double)READ_RATE_BYTES_PER_SEC;
        if (g_readTokens > cap) g_readTokens = cap;
        // A single read larger than the whole burst allowance can never be
        // satisfied by waiting - let it straight through rather than spin.
        allowed = (g_readTokens >= (double)bytes) || ((double)bytes > cap);
        if (allowed) g_readTokens -= (double)bytes;
        LeaveCriticalSection(&g_readPaceLock);
        if (allowed) break;
        Sleep(1);
    }
    LONG waited = (LONG)((double)(__rdtsc() - waitStart) / g_cyclesPerUsec);
    if (waited > 0) {
        InterlockedExchangeAdd(&g_readPaceDelayUsec, waited);
        InterlockedExchangeAdd(&g_readPaceDelayTotalUsec, waited);
    }
}

// ---- ReadFile deep diagnostics (digging into cause 2 properly) -----------
// The rate limiter above only ever trades load latency for frame time - it
// does not explain WHY these bursts cost what they cost, which is the actual
// question now that cause 1 (LockRect) is fixed and confirmed. Three
// distinct explanations would call for three different real fixes, and
// they're distinguishable from here:
//   (a) genuine raw disk throughput - each call's own duration scales with
//       its size at a roughly constant, plausible-for-the-drive rate. Fix
//       would have to reduce total bytes moved (better compression, less
//       redundant streaming) - nothing hookable from outside changes this.
//   (b) many small calls, each fast on its own, but so numerous that their
//       SUM stalls a frame - a batching opportunity (fewer, larger reads,
//       or async I/O overlapping with rendering) would be the real fix.
//   (c) some INDIVIDUAL calls are anomalously slow for their size (latency
//       not explained by volume) - points at contention/queuing (antivirus,
//       disk queue depth, or the file being read scattered/non-sequential)
//       rather than raw transfer cost.
// Per-call duration (not just aggregate bytes/sec) distinguishes all three
// directly. File path is resolved (rate-limited to the slow-call path only,
// so the extra syscall never touches the hot path) to see whether this is
// one archive being read via many small sequential calls, or many distinct
// files/assets.
#define MAX_READFILE_PATH_LOG 40
static volatile LONG g_readFilePathLogCount = 0;
static volatile LONG g_readFileOverlappedCount = 0;
static volatile LONG g_readFileSyncCount = 0;

static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                                  LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    // Never pace the main thread: if it ever reads synchronously, delaying it
    // is delaying the frame - the exact thing being fixed.
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    if (!isMain) {
        PaceRead(nNumberOfBytesToRead);
    }

    if (lpOverlapped) InterlockedIncrement(&g_readFileOverlappedCount);
    else InterlockedIncrement(&g_readFileSyncCount);

    unsigned __int64 t0 = __rdtsc();
    BOOL ok = g_realReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    unsigned __int64 elapsedCycles = __rdtsc() - t0;

    // Census only - the kernel just wrote this buffer, so warming it would be
    // pure waste. What matters is the volume and the owning thread.
    DWORD got = (ok && lpNumberOfBytesRead) ? *lpNumberOfBytesRead : nNumberOfBytesToRead;
    CensusRecord(ALLOC_SRC_READFILE, got);
    InterlockedIncrement(&g_readFileCount);
    g_readFileBytes += (LONGLONG)got;

    if (g_cyclesPerUsec > 0.0 && got > 0) {
        LONG usec = (LONG)((double)elapsedCycles / g_cyclesPerUsec);
        double mbPerSec = (usec > 0) ? ((double)got / 1048576.0) / ((double)usec / 1000000.0) : 0.0;
        // Threshold picked well below what even a slow HDD should take for a
        // typical ~87KB streaming read - anything crossing it either moved
        // an unusually large amount in one call, or was anomalously slow for
        // its size (case (c) above), both worth a closer look either way.
        if (usec > 3000) {
            char path[MAX_PATH] = "?";
            DWORD pathLen = GetFinalPathNameByHandleA(hFile, path, MAX_PATH, FILE_NAME_NORMALIZED);
            const char *shortPath = path;
            if (pathLen > 0 && pathLen < MAX_PATH) {
                // GetFinalPathNameByHandleA prefixes "\\?\" or "\\?\UNC\" -
                // strip it so the log reads as a normal path.
                if (strncmp(path, "\\\\?\\", 4) == 0) shortPath = path + 4;
            }
            DWORD tid = GetCurrentThreadId();
            const char *name = isMain ? "MAIN" : "?";
            if (!isMain) {
                EnterCriticalSection(&g_threadNameLock);
                for (LONG j = 0; j < g_threadNameCount; j++) {
                    if (g_threadNames[j].threadId == tid) { name = g_threadNames[j].name; break; }
                }
                LeaveCriticalSection(&g_threadNameLock);
            }
            LONG n = InterlockedIncrement(&g_readFilePathLogCount);
            if (n <= MAX_READFILE_PATH_LOG) {
                char line[512];
                sprintf(line, "[readfile] SLOW %lu us for %lu bytes (%.1f MB/s) thread=%lu(%s) overlapped=%d path=%s",
                        (unsigned long)usec, (unsigned long)got, mbPerSec, tid, name,
                        lpOverlapped != NULL, shortPath);
                LogLine(line);
            }
        }
    }
    return ok;
}

static int InstallAllocTrackingHooks(void)
{
    g_dispatchTrackTls = TlsAlloc();
    g_censusTls = TlsAlloc();
    InitializeCriticalSection(&g_allocThreadLock);
    InitializeCriticalSection(&g_allocImplLock);
    InitializeCriticalSection(&g_warmLock);
    InitializeCriticalSection(&g_readPaceLock);
#if ENABLE_ALLOCATOR_WARM
    CreateThread(NULL, 0, WarmerThread, NULL, 0, NULL);
#endif

    HMODULE hMain = GetModuleHandleA(NULL);

    void *realHeapAlloc = PatchIat(hMain, "KERNEL32.dll", "HeapAlloc", (void *)HookedHeapAlloc);
    void *realVirtualAlloc = PatchIat(hMain, "KERNEL32.dll", "VirtualAlloc", (void *)HookedVirtualAlloc);
    void *realMapView = PatchIat(hMain, "KERNEL32.dll", "MapViewOfFile", (void *)HookedMapViewOfFile);
    void *realReadFile = PatchIat(hMain, "KERNEL32.dll", "ReadFile", (void *)HookedReadFile);

    if (realHeapAlloc) g_realHeapAlloc = (PFN_HeapAlloc)realHeapAlloc;
    if (realVirtualAlloc) g_realVirtualAlloc = (PFN_VirtualAlloc)realVirtualAlloc;
    if (realMapView) g_realMapViewOfFile = (PFN_MapViewOfFile)realMapView;
    if (realReadFile) g_realReadFile = (PFN_ReadFile)realReadFile;

    char line[192];
    sprintf(line, "Census IAT hooks: HeapAlloc=%d VirtualAlloc=%d MapViewOfFile=%d ReadFile=%d",
            realHeapAlloc != NULL, realVirtualAlloc != NULL, realMapView != NULL, realReadFile != NULL);
    LogLine(line);

    return (realHeapAlloc != NULL) && (realVirtualAlloc != NULL);
}

// ---- Allocation census + background memory warmer (v6) --------------------
// Fix attempt 5 recorded only allocations made *inside* a loader dispatch, on
// the dispatching thread (~105 allocs / ~2.3MB in a burst - far too little to
// be a whole town chunk's worth of actor/skeleton/animation data). Two
// independent problems with that, both addressed here:
//
//  1. COVERAGE. If the memory that goes cold is allocated on a job worker, or
//     on the main thread at actor-instantiation time - anywhere other than
//     inside FUN_004b5cb0 on the loader thread - attempt 5 never saw it at
//     all. The census below records EVERY call through the engine's named-
//     heap allocator, attributed per-thread (thread names already captured
//     via the RaiseException hook), so the log answers directly: which thread
//     allocates the bulk of the bytes, and in which 500ms window. It also
//     records the distinct concrete allocator implementations reached through
//     the heap object's vtable, which is what makes it possible to go find
//     *sibling* wrappers (other allocation paths) statically in Ghidra
//     afterwards - the callers of those implementations are the complete set.
//
//  2. WARMING METHOD. `_mm_prefetch` is a *hint*: the CPU drops prefetches
//     freely once the fill buffers / outstanding-miss slots are saturated,
//     and a tight back-to-back loop over megabytes saturates them almost
//     immediately - so most of attempt 5's "prefetch" very likely never
//     issued a memory request at all, which would explain a mechanism that
//     was confirmed to *run* yet changed nothing. Real (volatile) loads
//     cannot be dropped: each one either hits or stalls until the line
//     actually arrives. Warming now runs on a dedicated BELOW_NORMAL thread
//     draining a queue, so that stall cost lands off the critical path
//     instead of inside the loader dispatch.
//
// Reading memory the engine may have freed in the meantime is guarded by SEH
// on our own thread (the engine's pools are pre-reserved and pre-committed -
// HeapAlloc/VirtualAlloc were confirmed never to fire - so a fault here is
// not an expected path, but the guard costs nothing on a non-critical thread).

// v7: the census is per-thread AND per-source. v6 measured only the engine's
// named-heap wrapper and found it is a main-thread small-object allocator
// (~300 bytes average, ~1.4MB per 500ms, 99.7% of it on the main thread) -
// far too little, and on the wrong thread, to be the multi-megabyte cold
// chunk data. The bulk allocation path is therefore something else, and
// these are the candidates the exe actually imports: HeapAlloc, VirtualAlloc,
// CreateFileMappingA/MapViewOfFile, ReadFile.
static const char *g_allocSrcNames[NUM_ALLOC_SRC] = {
    "namedheap", "HeapAlloc", "VirtualAlloc", "MapViewOfFile", "ReadFile"
};

#define MAX_ALLOC_THREADS 64
typedef struct {
    volatile LONG threadId; // 0 = unclaimed
    volatile LONG count[NUM_ALLOC_SRC];
    volatile LONGLONG bytes[NUM_ALLOC_SRC];
} AllocThreadSlot;
static AllocThreadSlot g_allocThreads[MAX_ALLOC_THREADS];
static CRITICAL_SECTION g_allocThreadLock;
static DWORD g_censusTls;

static AllocThreadSlot *ClaimAllocSlot(DWORD tid)
{
    EnterCriticalSection(&g_allocThreadLock);
    AllocThreadSlot *slot = NULL;
    for (int i = 0; i < MAX_ALLOC_THREADS; i++) {
        if (g_allocThreads[i].threadId == (LONG)tid) { slot = &g_allocThreads[i]; break; }
        if (slot == NULL && g_allocThreads[i].threadId == 0) slot = &g_allocThreads[i];
    }
    if (slot && slot->threadId == 0) slot->threadId = (LONG)tid;
    LeaveCriticalSection(&g_allocThreadLock);
    return slot;
}

static AllocThreadSlot *GetCensusSlot(void)
{
    AllocThreadSlot *s = (AllocThreadSlot *)TlsGetValue(g_censusTls);
    if (!s) {
        s = ClaimAllocSlot(GetCurrentThreadId());
        if (s) TlsSetValue(g_censusTls, s);
    }
    return s;
}

// Per-thread single-writer counters; only the monitor thread reads them.
static void CensusRecord(int src, size_t bytes)
{
    AllocThreadSlot *s = GetCensusSlot();
    if (!s) return;
    s->count[src]++;
    s->bytes[src] += (LONGLONG)bytes;
}

// Distinct concrete allocator implementations behind FUN_00b454a0's virtual
// dispatch. Layout confirmed from its disassembly:
//   heap   = *(void**)this        (MOV ECX,[ESI])
//   vtable = *(void**)heap        (MOV EAX,[ECX])
//   allocFn= ((void**)vtable)[1]  (MOV EAX,[EAX+4])
#define MAX_ALLOC_IMPLS 16
typedef struct {
    void *vtable;
    void *allocFn;
    void *exampleHeap;
    volatile LONG count;
    volatile LONG logged;
} AllocImplEntry;
static AllocImplEntry g_allocImpls[MAX_ALLOC_IMPLS];
static volatile LONG g_allocImplCount = 0;
static CRITICAL_SECTION g_allocImplLock;

static void NoteAllocImpl(void *thisPtr)
{
    if (!thisPtr) return;
    void *heap = *(void **)thisPtr;
    if (!heap) return;
    void *vt = *(void **)heap;
    if (!vt) return;
    void *fn = ((void **)vt)[1];

    LONG n = g_allocImplCount;
    for (LONG i = 0; i < n; i++) {
        if (g_allocImpls[i].vtable == vt && g_allocImpls[i].allocFn == fn) {
            InterlockedIncrement(&g_allocImpls[i].count);
            return;
        }
    }
    EnterCriticalSection(&g_allocImplLock);
    n = g_allocImplCount;
    LONG found = -1;
    for (LONG i = 0; i < n; i++) {
        if (g_allocImpls[i].vtable == vt && g_allocImpls[i].allocFn == fn) { found = i; break; }
    }
    if (found < 0 && n < MAX_ALLOC_IMPLS) {
        g_allocImpls[n].vtable = vt;
        g_allocImpls[n].allocFn = fn;
        g_allocImpls[n].exampleHeap = heap;
        found = n;
        g_allocImplCount = n + 1;
    }
    LeaveCriticalSection(&g_allocImplLock);
    if (found >= 0) InterlockedIncrement(&g_allocImpls[found].count);
}

// Bounded queue, plain critical section. Only allocations >= WARM_MIN_SIZE
// are enqueued, so the rate here is orders of magnitude below the raw
// allocator call rate - a lock is entirely adequate and much easier to reason
// about than a lock-free ring with a wrap-vs-drop desync hazard.
#define WARM_QUEUE_SIZE 8192
#define WARM_MIN_SIZE 4096
#define WARM_MAX_SIZE (16 * 1024 * 1024)

typedef struct {
    void *ptr;
    size_t size;
} WarmEntry;
static WarmEntry g_warmQueue[WARM_QUEUE_SIZE];
static int g_warmHead = 0;
static int g_warmCount = 0;
static CRITICAL_SECTION g_warmLock;

static volatile LONG g_warmEnqueued = 0;
static volatile LONG g_warmDropped = 0;
static volatile LONG g_warmDone = 0;
static volatile LONGLONG g_warmBytes = 0;
static volatile LONG g_warmSink = 0;

static void EnqueueWarm(void *ptr, size_t size)
{
    if (!ptr || size < WARM_MIN_SIZE || size > WARM_MAX_SIZE) return;
    EnterCriticalSection(&g_warmLock);
    if (g_warmCount < WARM_QUEUE_SIZE) {
        int idx = (g_warmHead + g_warmCount) % WARM_QUEUE_SIZE;
        g_warmQueue[idx].ptr = ptr;
        g_warmQueue[idx].size = size;
        g_warmCount++;
        InterlockedIncrement(&g_warmEnqueued);
    } else {
        InterlockedIncrement(&g_warmDropped);
    }
    LeaveCriticalSection(&g_warmLock);
}

static int DequeueWarm(WarmEntry *out)
{
    int got = 0;
    EnterCriticalSection(&g_warmLock);
    if (g_warmCount > 0) {
        *out = g_warmQueue[g_warmHead];
        g_warmHead = (g_warmHead + 1) % WARM_QUEUE_SIZE;
        g_warmCount--;
        got = 1;
    }
    LeaveCriticalSection(&g_warmLock);
    return got;
}

static DWORD WINAPI WarmerThread(LPVOID param)
{
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WarmEntry e;
        if (!DequeueWarm(&e)) {
            Sleep(1);
            continue;
        }
        LONG acc = 0;
        __try {
            volatile const unsigned char *p = (volatile const unsigned char *)e.ptr;
            for (size_t off = 0; off < e.size; off += 64) {
                acc += p[off];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            acc = 0;
        }
        g_warmSink += acc; // keeps the loads from being optimized away
        InterlockedIncrement(&g_warmDone);
        g_warmBytes += (LONGLONG)e.size;
    }
}

// ---- Engine's own named-heap allocator hook (FUN_00b454a0) ----------------
// The HeapAlloc/VirtualAlloc tracking above caught nothing across an entire
// session - confirmed the engine's per-object allocations never reach the
// OS allocator directly. FUN_00b454a0 is the actual layer: a __thiscall
// wrapper (EnterCriticalSection -> virtual "Allocate" call on a named heap
// object -> LeaveCriticalSection) already found and decompiled earlier in
// this investigation. Its own prologue has no SEH scaffolding (verified,
// unlike FUN_004b5cb0/FUN_00a01a00/FUN_00a015b0), so return-address
// hijacking - the same technique used for FUN_00aacf10 etc. - is safe here.
//
// Unlike every other function hijacked this way in this investigation,
// THIS one's return value is real and load-bearing: verified via raw
// disassembly (`MOV EAX,EBX` immediately before `RET 0x8`) that it
// genuinely returns the allocated pointer via EAX, despite Ghidra
// inferring a void signature (the same class of decompiler gap caught
// twice earlier in this investigation - trust disassembly, not the
// decompiled C, when it matters). 923 call sites across the engine depend
// on that value. The hook must preserve EAX across its own bookkeeping
// call and hand back the REAL pointer, not discard it like the
// confirmed-void functions instrumented earlier.
//
// Confirmed via disassembly: __thiscall(this=ECX, param_2=requested size,
// param_3=tag/context), RET 0x8 (cleans the 2 stack args itself - hijacking
// doesn't care, we never touch how it returns, only where).

#define ALLOCATOR_RVA (0x00b454a0 - 0x00400000)
#define ALLOCATOR_PATCH_LEN 9 // PUSH EBP; MOV EBP,ESP; SUB ESP,0x40c

static void *g_allocatorTarget = NULL;
static void *g_allocatorTrampoline = NULL;

// Digging into cause 2, continued: per-call ReadFile duration ruled out raw
// disk speed and per-call I/O contention (only 2 calls all session exceeded
// 3ms, both plain large sequential reads at 4000+ MB/s). The 101ms stutter
// captured earlier can't be ReadFile's own time - 272 calls at that drive's
// real speed total ~5ms, not 100ms - yet its own watchdog capture landed
// mid-way through THIS allocator (EIP inside FUN_00b454a0) with 6070
// allocations in that one frame. This allocator takes a critical section on
// every call (confirmed via disassembly when this hook was first built:
// EnterCriticalSection -> virtual Allocate -> LeaveCriticalSection). 6070
// calls in one frame is a plausible lock-contention cost in its own right -
// timing each call directly tests it, especially whether the MAIN thread's
// own calls specifically slow down while a Loader/decode thread is flooding
// the same lock during a burst.
static volatile LONGLONG g_allocDurSumUsec[2]; // [0]=main thread, [1]=all other threads
static volatile LONG g_allocDurCount[2];
static volatile LONG g_allocDurMaxUsec[2];

#define ALLOC_STACK_DEPTH 16
typedef struct {
    void *trueRetAddr;
    size_t requestedSize;
    unsigned __int64 entryTsc;
} AllocRetFrame;
typedef struct {
    int top;
    DWORD tid;              // cached, so the hot path never re-queries it
    AllocThreadSlot *slot;  // this thread's census slot, claimed once
    AllocRetFrame frames[ALLOC_STACK_DEPTH];
} AllocThreadStack;

static DWORD g_allocStackTls;

static AllocThreadStack *GetAllocThreadStack(void)
{
    AllocThreadStack *ts = (AllocThreadStack *)TlsGetValue(g_allocStackTls);
    if (!ts) {
        ts = (AllocThreadStack *)calloc(1, sizeof(AllocThreadStack));
        if (ts) TlsSetValue(g_allocStackTls, ts);
    }
    return ts;
}

static int __cdecl OnAllocatorEnter(void *trueRetAddr, size_t requestedSize, void *thisPtr)
{
    NoteAllocImpl(thisPtr);
    AllocThreadStack *ts = GetAllocThreadStack();
    if (!ts || ts->top >= ALLOC_STACK_DEPTH) return 0;
    ts->frames[ts->top].trueRetAddr = trueRetAddr;
    ts->frames[ts->top].requestedSize = requestedSize;
    ts->frames[ts->top].entryTsc = __rdtsc();
    ts->top++;
    return 1;
}

// Structurally only ever called for a call OnAllocatorEnter successfully
// recorded (entry hijack is conditional on its return value), so
// ts->top > 0 is guaranteed - same invariant as every other return-hijack
// hook in this investigation.
static void *__cdecl OnAllocatorReturn(void *resultPtr)
{
    AllocThreadStack *ts = GetAllocThreadStack();
    ts->top--;
    AllocRetFrame f = ts->frames[ts->top];
    RecordAllocIfTracking(resultPtr, f.requestedSize);

    if (resultPtr && f.requestedSize) {
        if (!ts->tid) ts->tid = GetCurrentThreadId();
        CensusRecord(ALLOC_SRC_NAMEDHEAP, f.requestedSize);
        InterlockedIncrement(&g_namedHeapCount);

        if (g_cyclesPerUsec > 0.0) {
            LONG usec = (LONG)((double)(__rdtsc() - f.entryTsc) / g_cyclesPerUsec);
            int bucket = ((LONG)ts->tid == g_mainThreadId) ? 0 : 1;
            InterlockedIncrement(&g_allocDurCount[bucket]);
            InterlockedExchangeAdd64(&g_allocDurSumUsec[bucket], usec);
            if (usec > g_allocDurMaxUsec[bucket]) g_allocDurMaxUsec[bucket] = usec;
            // Lock contention shows up as an INDIVIDUAL call taking far
            // longer than the allocation work itself could justify - a plain
            // heap allocation is normally low-single-digit microseconds even
            // under load, so anything crossing 1ms is the lock, not the work.
            if (usec > 1000) {
                char line[128];
                sprintf(line, "[allocator] SLOW %ld us on thread %lu (%s) size=%zu",
                        usec, (unsigned long)ts->tid,
                        bucket == 0 ? "MAIN" : "other", f.requestedSize);
                LogLine(line);
            }
        }
        // Never warm the main thread's own allocations: it is about to touch
        // that memory itself anyway, so warming it would only add bandwidth
        // contention on the exact thread whose frame time we're trying to
        // protect. (Main thread identified as the one calling the frame
        // pacer FUN_00ac3040, which is confirmed once-per-frame/main-only.)
#if ENABLE_ALLOCATOR_WARM
        if (g_allocatorWarmEnabled && (LONG)ts->tid != g_mainThreadId) {
            EnqueueWarm(resultPtr, f.requestedSize);
        }
#endif
    }
    return f.trueRetAddr;
}

// Preserves EAX (the real allocated pointer) across the OnAllocatorReturn
// call - unlike the void-returning hooks elsewhere in this file, this
// value is load-bearing for the 923 real callers of FUN_00b454a0.
__declspec(naked) void OnAllocatorReturnStub(void)
{
    __asm {
        push eax
        push eax
        call OnAllocatorReturn
        mov ecx, eax          ; true return address, stashed in scratch reg
        add esp, 4
        pop eax                ; restore the REAL allocated pointer
        jmp ecx
    }
}

__declspec(naked) void Detour_Allocator(void)
{
    __asm {
        push ecx                       ; save 'this'  -> [esp]=this [esp+4]=ret [esp+8]=size
        push dword ptr [esp]           ; arg3: thisPtr
        push dword ptr [esp + 12]      ; arg2: requestedSize
        push dword ptr [esp + 12]      ; arg1: trueRetAddr
        call OnAllocatorEnter
        add esp, 12
        test eax, eax
        jz skip_allocator_hijack
        mov dword ptr [esp + 4], offset OnAllocatorReturnStub
    skip_allocator_hijack:
        pop ecx                        ; restore 'this'
        jmp dword ptr [g_allocatorTrampoline]
    }
}

static int InstallAllocatorHook(void)
{
    // Census/warmer state is initialised in InstallAllocTrackingHooks, which
    // runs first - it must, since the OS-level hooks it installs can start
    // feeding the census immediately.
    g_allocStackTls = TlsAlloc();

    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    g_allocatorTarget = base + ALLOCATOR_RVA;

    unsigned char saved[ALLOCATOR_PATCH_LEN];
    memcpy(saved, g_allocatorTarget, ALLOCATOR_PATCH_LEN);

    unsigned char *tramp = (unsigned char *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;

    memcpy(tramp, saved, ALLOCATOR_PATCH_LEN);
    tramp[ALLOCATOR_PATCH_LEN] = 0xE9;
    *(int *)(tramp + ALLOCATOR_PATCH_LEN + 1) =
        (int)((unsigned char *)g_allocatorTarget + ALLOCATOR_PATCH_LEN) -
        (int)(tramp + ALLOCATOR_PATCH_LEN + 5);
    g_allocatorTrampoline = tramp;

    DWORD oldProtect;
    if (!VirtualProtect(g_allocatorTarget, ALLOCATOR_PATCH_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;

    unsigned char *t = (unsigned char *)g_allocatorTarget;
    t[0] = 0xE9;
    *(int *)(t + 1) = (int)(void *)Detour_Allocator - (int)(t + 5);
    for (int i = 5; i < ALLOCATOR_PATCH_LEN; i++) t[i] = 0x90;

    VirtualProtect(g_allocatorTarget, ALLOCATOR_PATCH_LEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), g_allocatorTarget, ALLOCATOR_PATCH_LEN);
    return 1;
}

__declspec(naked) void LoaderDispatchWrapper(void)
{
    __asm {
        push ecx
        call AcquireLoaderThrottle
        pop ecx
        push ecx
        call BeginDispatchTracking
        pop ecx
        call dword ptr [g_realLoaderDispatch]
        call EndDispatchTrackingAndPrefetch
        call ReleaseLoaderThrottle
        ret
    }
}

static int InstallLoaderThrottle(void)
{
    g_loaderThrottleAcquiredTls = TlsAlloc();
    g_loaderThrottleSem = CreateSemaphoreA(NULL, LOADER_THROTTLE_MAX, LOADER_THROTTLE_MAX, NULL);
    if (!g_loaderThrottleSem) return 0;

    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    g_realLoaderDispatch = base + LOADER_DISPATCH_FUNC_RVA;

    unsigned char *callSite = base + LOADER_DISPATCH_CALL_SITE_RVA;
    DWORD oldProtect;
    if (!VirtualProtect(callSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;
    callSite[0] = 0xE8; // CALL rel32 - same opcode as the original instruction, only the target changes
    *(int *)(callSite + 1) = (int)(void *)LoaderDispatchWrapper - (int)(callSite + 5);
    VirtualProtect(callSite, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), callSite, 5);
    return 1;
}

// ---- WaitForSingleObject IAT hook -----------------------------------------
// Deliberately a completely different, much safer technique than the
// inline hooks below: patches the game's Import Address Table entry for
// WaitForSingleObject (kernel32.dll) to point at our own normal C wrapper,
// which calls straight through to the real function and returns its
// result normally. No naked asm, no return-address manipulation, no SEH
// risk - this is the standard, decades-proven-safe way to instrument a
// well-defined WINAPI call. Tests the job-system-synchronization
// hypothesis directly (the earlier CSwitch capture found WrDispatchInt/
// WrPreempted wait reasons appearing exclusively during the bad-frame
// cluster) without touching any internal game code at all.

typedef DWORD(WINAPI *PFN_WaitForSingleObject)(HANDLE, DWORD);
static PFN_WaitForSingleObject g_realWaitForSingleObject = NULL;

static volatile LONG g_wfsoCallCount = 0;
static volatile LONGLONG g_wfsoSumCycles = 0;

// Per-thread breakdown: fixed slot table, linear scan to find-or-claim a
// slot for the calling thread ID. Small, bounded number of distinct
// threads actually call this (job system + a handful of others), so a
// simple table is fine - protected by a critical section only for the
// rare "claim a new slot" path; the common "increment existing slot"
// path uses interlocked ops on that slot's own counters, no locking.
#define MAX_WFSO_THREADS 64
typedef struct {
    volatile LONG threadId; // 0 = unclaimed
    volatile LONG callCount;
    volatile LONGLONG sumCycles;
} WfsoThreadSlot;
static WfsoThreadSlot g_wfsoThreads[MAX_WFSO_THREADS];
static CRITICAL_SECTION g_wfsoThreadTableLock;

static WfsoThreadSlot *GetOrClaimWfsoSlot(DWORD tid)
{
    for (int i = 0; i < MAX_WFSO_THREADS; i++) {
        if (g_wfsoThreads[i].threadId == (LONG)tid) return &g_wfsoThreads[i];
    }
    EnterCriticalSection(&g_wfsoThreadTableLock);
    // re-check under lock in case another thread claimed a slot for us
    // (can't happen for our own tid, but a fresh empty slot search must
    // be re-done safely)
    WfsoThreadSlot *slot = NULL;
    for (int i = 0; i < MAX_WFSO_THREADS; i++) {
        if (g_wfsoThreads[i].threadId == (LONG)tid) { slot = &g_wfsoThreads[i]; break; }
        if (slot == NULL && g_wfsoThreads[i].threadId == 0) slot = &g_wfsoThreads[i];
    }
    if (slot && slot->threadId == 0) {
        slot->threadId = (LONG)tid;
    }
    LeaveCriticalSection(&g_wfsoThreadTableLock);
    return slot;
}

static DWORD WINAPI HookedWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    LONG mainStart = 0;
    if (isMain) {
        mainStart = NowUsec();
        g_mainWfsoHandle = (LONG)(ULONG_PTR)hHandle;
        g_mainWfsoStartUsec = mainStart ? mainStart : 1;
    }
    unsigned __int64 t0 = __rdtsc();
    DWORD result = g_realWaitForSingleObject(hHandle, dwMilliseconds);
    unsigned __int64 elapsed = __rdtsc() - t0;
    if (isMain) {
        LONG waited = NowUsec() - mainStart;
        g_mainWfsoStartUsec = 0;
        if (waited > 0) InterlockedExchangeAdd(&g_mainWfsoWaitTotalUsec, waited);
    }
    InterlockedIncrement(&g_wfsoCallCount);
    g_wfsoSumCycles += (LONGLONG)elapsed;

    WfsoThreadSlot *slot = GetOrClaimWfsoSlot(GetCurrentThreadId());
    if (slot) {
        InterlockedIncrement(&slot->callCount);
        slot->sumCycles += (LONGLONG)elapsed;
    }
    return result;
}

// Finds the IAT slot for {moduleName}!{funcName} in the given module's
// import table and overwrites it with newFunc. Returns the original
// function pointer (needed so the wrapper can call through), or NULL on
// failure (leaves everything untouched).
static void *PatchIat(HMODULE hostModule, const char *moduleName, const char *funcName, void *newFunc)
{
    unsigned char *base = (unsigned char *)hostModule;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0) return NULL;

    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + importDir.VirtualAddress);
    for (; imp->Name != 0; imp++) {
        const char *thisModuleName = (const char *)(base + imp->Name);
        if (_stricmp(thisModuleName, moduleName) != 0) continue;

        IMAGE_THUNK_DATA *origThunk = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *iatThunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; origThunk++, iatThunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *byName = (IMAGE_IMPORT_BY_NAME *)(base + origThunk->u1.AddressOfData);
            if (strcmp((const char *)byName->Name, funcName) != 0) continue;

            void *original = (void *)iatThunk->u1.Function;
            DWORD oldProtect;
            if (!VirtualProtect(&iatThunk->u1.Function, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
                return NULL;
            }
            iatThunk->u1.Function = (ULONG_PTR)newFunc;
            VirtualProtect(&iatThunk->u1.Function, sizeof(void *), oldProtect, &oldProtect);
            return original;
        }
    }
    return NULL;
}

// ---- RaiseException IAT hook (thread-name capture) ------------------------
// Same safe technique as WaitForSingleObject: a plain C wrapper, no asm.
// Captures the classic MSVC thread-naming convention (RaiseException with
// code 0x406D1388, already confirmed via static analysis that this engine
// uses named worker threads: "Loader", "GameInitializer", "Trophy",
// "GameUpdate", "CDevCommon Task%d") so the mystery thread IDs showing up
// in the WaitForSingleObject breakdown can be identified. Safe to insert a
// plain function call between the caller's __try and the real
// RaiseException: SEH handler lookup walks the exception-registration
// chain via FS:[0], not literal call-stack depth, so an extra ordinary
// (non-SEH-establishing) frame in between changes nothing.

typedef void(WINAPI *PFN_RaiseException)(DWORD, DWORD, DWORD, const ULONG_PTR *);
static PFN_RaiseException g_realRaiseException = NULL;

// g_threadNames / g_threadNameCount / g_threadNameLock forward-declared near
// the top of the file (needed by the ReadFile diagnostics, which run earlier
// in file order) - C merges these into the same objects.

// Fix attempt: lower the priority of threads specifically named "Loader"
// (confirmed via live thread-name capture to be the ones blocking for
// tens to hundreds of milliseconds - one for 1.66s - in the window right
// before the CPU-side stutter). This doesn't change what work happens,
// only the OS scheduler's preference when a Loader thread and a
// frame-critical job-system thread both want the CPU at the same time -
// standard, fully reversible Windows API, cannot itself cause a crash
// (worst case: loading takes a little longer, invisible to the player,
// vs. a visible frame hitch).
// Shares g_loaderThrottleEnabled with the dispatch semaphore (same original
// fix attempt, bundled). Note this only gates NEWLY-named threads going
// forward - a thread's priority isn't retroactively restored if toggled off
// after it was already lowered, same "takes effect going forward, not
// retroactively" caveat as the other fixes in this session.
static void MaybeLowerLoaderPriority(DWORD tid, const char *name, BOOL isCurrentThread)
{
    if (!g_loaderThrottleEnabled) return;
    if (strcmp(name, "Loader") != 0) return;
    HANDLE hThread = isCurrentThread ? GetCurrentThread() : OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
    if (!hThread) return;
    BOOL ok = SetThreadPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL);
    char line[128];
    sprintf(line, "[priority] Lowered '%s' (tid=%lu) to BELOW_NORMAL: %s", name, tid, ok ? "OK" : "FAILED");
    LogLine(line);
    if (!isCurrentThread) CloseHandle(hThread);
}

static void WINAPI HookedRaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                                         DWORD nNumberOfArguments, const ULONG_PTR *lpArguments)
{
    if (dwExceptionCode == 0x406D1388 && nNumberOfArguments >= 3 && lpArguments != NULL) {
        DWORD type = (DWORD)lpArguments[0];
        const char *name = (const char *)lpArguments[1];
        DWORD tid = (DWORD)lpArguments[2];
        if (type == 0x1000 && name != NULL) {
            BOOL isCurrentThread = (tid == (DWORD)-1);
            DWORD resolvedTid = isCurrentThread ? GetCurrentThreadId() : tid;
            EnterCriticalSection(&g_threadNameLock);
            if (g_threadNameCount < MAX_THREAD_NAMES) {
                ThreadNameEntry *e = &g_threadNames[g_threadNameCount++];
                e->threadId = resolvedTid;
                strncpy(e->name, name, sizeof(e->name) - 1);
                e->name[sizeof(e->name) - 1] = 0;
                char line[128];
                sprintf(line, "[threadname] tid=%lu name=%s", e->threadId, e->name);
                LogLine(line);
            }
            LeaveCriticalSection(&g_threadNameLock);
            MaybeLowerLoaderPriority(resolvedTid, name, isCurrentThread);
        }
    }
    g_realRaiseException(dwExceptionCode, dwExceptionFlags, nNumberOfArguments, lpArguments);
}

static int InstallRaiseExceptionHook(void)
{
    InitializeCriticalSection(&g_threadNameLock);
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "RaiseException", (void *)HookedRaiseException);
    if (!original) return 0;
    g_realRaiseException = (PFN_RaiseException)original;
    return 1;
}

// ---- SetThreadIdealProcessor IAT hook -------------------------------------
// Same safe plain-C IAT-patch technique. Confirmed imported by the exe
// (checked the actual import table directly). Tests whether Loader
// threads and job-worker threads are being assigned overlapping/adjacent
// ideal processors - if so, that's a concrete, directly fixable
// contention mechanism (redirect via the same hook, once confirmed).

typedef DWORD(WINAPI *PFN_SetThreadIdealProcessor)(HANDLE, DWORD);
static PFN_SetThreadIdealProcessor g_realSetThreadIdealProcessor = NULL;
typedef DWORD(WINAPI *PFN_GetThreadId)(HANDLE);
static PFN_GetThreadId g_pGetThreadId = NULL;

static DWORD WINAPI HookedSetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor)
{
    DWORD result = g_realSetThreadIdealProcessor(hThread, dwIdealProcessor);
    DWORD tid = g_pGetThreadId ? g_pGetThreadId(hThread) : 0;
    const char *name = "?";
    EnterCriticalSection(&g_threadNameLock);
    for (LONG i = 0; i < g_threadNameCount; i++) {
        if (g_threadNames[i].threadId == tid) { name = g_threadNames[i].name; break; }
    }
    char line[192];
    sprintf(line, "[idealproc] tid=%lu name=%s requested_ideal_cpu=%lu (prev=%lu)",
            tid, name, dwIdealProcessor, result);
    LeaveCriticalSection(&g_threadNameLock);
    LogLine(line);
    return result;
}

static int InstallIdealProcessorHook(void)
{
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        g_pGetThreadId = (PFN_GetThreadId)GetProcAddress(hKernel32, "GetThreadId");
    }
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "SetThreadIdealProcessor", (void *)HookedSetThreadIdealProcessor);
    if (!original) return 0;
    g_realSetThreadIdealProcessor = (PFN_SetThreadIdealProcessor)original;
    return 1;
}

// ---- EnterCriticalSection contention hook (v10) --------------------------
// 96% of watchdog captures found the main thread blocked inside a system DLL
// rather than executing game code, so the stutter is a WAIT. The exe imports
// EnterCriticalSection, and a lock held by a streaming/decompression thread
// while the main thread wants it produces exactly the observed shape: an
// isolated multi-tens-of-ms frame with normal CPU work either side.
//
// EnterCriticalSection is far too hot to instrument naively. This wrapper
// calls TryEnterCriticalSection FIRST: on the uncontended path (the
// overwhelming majority) that succeeds and returns immediately, costing one
// extra call and no bookkeeping. Only when it FAILS - i.e. an actual
// contended wait, which is the only case of interest - does it record the
// lock, its current owner, and the wait duration before blocking for real.
// Semantics are identical: a successful TryEnterCriticalSection leaves the
// caller owning the lock exactly once, same as EnterCriticalSection.
typedef void(WINAPI *PFN_EnterCriticalSection)(LPCRITICAL_SECTION);
static PFN_EnterCriticalSection g_realEnterCriticalSection = NULL;

static void WINAPI HookedEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (TryEnterCriticalSection(cs)) return;

    if ((LONG)GetCurrentThreadId() != g_mainThreadId) {
        g_realEnterCriticalSection(cs);
        return;
    }

    // Main thread, genuinely contended: this is the event being hunted.
    LONG start = NowUsec();
    g_mainCsPtr = (LONG)(ULONG_PTR)cs;
    g_mainCsOwner = (LONG)(ULONG_PTR)cs->OwningThread;
    g_mainCsWaitStartUsec = start ? start : 1;
    g_realEnterCriticalSection(cs);
    LONG waited = NowUsec() - start;
    g_mainCsWaitStartUsec = 0;
    if (waited > 0) {
        InterlockedExchangeAdd(&g_mainCsWaitTotalUsec, waited);
        InterlockedIncrement(&g_mainCsWaitCount);
    }
}

static int InstallCriticalSectionHook(void)
{
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "EnterCriticalSection", (void *)HookedEnterCriticalSection);
    if (!original) return 0;
    g_realEnterCriticalSection = (PFN_EnterCriticalSection)original;
    return 1;
}

static int InstallWfsoHook(void)
{
    InitializeCriticalSection(&g_wfsoThreadTableLock);
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "WaitForSingleObject", (void *)HookedWaitForSingleObject);
    if (!original) return 0;
    g_realWaitForSingleObject = (PFN_WaitForSingleObject)original;
    return 1;
}

// Turns a runtime address into something directly usable: game-module
// addresses become Ghidra VAs (base-relative + 0x00400000), everything else
// becomes "module.dll+RVA". Resolving system/driver DLLs by NAME is the whole
// point - the watchdog found the main thread inside one 96% of the time, and
// a bare relocated address says nothing about which one.
static void DescribeAddr(unsigned int addr, char *out)
{
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hm) && hm) {
        if ((unsigned int)hm == g_mainModBase) {
            sprintf(out, "%08X", addr - g_mainModBase + 0x00400000);
            return;
        }
        char path[MAX_PATH];
        path[0] = 0;
        GetModuleFileNameA(hm, path, MAX_PATH);
        const char *bn = strrchr(path, 92); // 92 = backslash
        bn = bn ? bn + 1 : path;
        sprintf(out, "%s+%X", bn, addr - (unsigned int)hm);
        return;
    }
    sprintf(out, "?%08X", addr);
}

// The log file is opened ONCE and held, not reopened per line.
//
// This was a genuine observer-effect bug, found by this project's own file-open
// probe. The previous version did fopen/fprintf/fclose on every line, and
// LogLine is called on the MAIN THREAD from the slow-lock path in
// HookedTexLockRect. So every slow lock we reported triggered a synchronous
// CreateFile + CloseHandle on the render thread - which is exactly the kind of
// stall being hunted. It showed up in the stutter attribution as
// `ZwCreateFile` (0.15s), `ZwClose` (0.09s) and `MSVCR100.dll+10A3B` (0.17s,
// the CRT's file I/O), with EBP chains consisting entirely of `VERSION.dll`
// frames - i.e. our own mod. Roughly 0.4s of measured "stutter" in that run
// was the instrument stalling the frame it was measuring.
//
// The same care was taken with the stutter watchdog (rate-limited, never
// suspends more than 25x/sec) and the frametime histogram (one array
// increment, no I/O) - the logger was simply overlooked.
//
// Holding the handle removes the open/close syscalls entirely. fflush is kept
// so a crash still leaves a complete log, which has mattered repeatedly here
// for diagnosing crashes that killed the process outright.
static FILE *g_logFile = NULL;
static CRITICAL_SECTION g_logLock;
static volatile LONG g_logLockState = 0;   // 0=none, 1=initialising, 2=ready
static volatile LONG g_bootFlush = 1;      // flush per line until boot is done

// Reads LogAppend out of the ini directly. LogLine runs before LoadConfig
// (the very first line is written during DllMain), so the normal config path
// is not available yet and the answer is needed before the file is opened.
// Absent key or absent file = 0 = truncate, which is the shipping behaviour.
// Raw Win32, NOT the CRT. The first LogLine happens inside DllMain, under the
// loader lock, while other DLLs (the HD GUI mod's version.dll) are still
// loading. CRT file I/O there is the classic way to turn a clean startup into
// an intermittent one, and doing it with fopen coincided with exactly that -
// one launch where the HD mod did not load, one crash before the window, one
// clean run. CreateFile/ReadFile take no CRT locks and are safe here.
//
// Reads only the first 4 KB: the key is one short line in a small file, and a
// bounded read cannot become a long stall on the startup path.
static int LogAppendRequested(void)
{
    char path[MAX_PATH];
    char buf[4096];
    HANDLE h;
    DWORD got = 0;
    char *p;

    GetModuleFileNameA(NULL, path, MAX_PATH);
    {
        char *slash = strrchr(path, '\\');
        if (!slash) return 0;
        strcpy(slash + 1, "version_hook_config.ini");
    }
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;      // no ini = truncate = default
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!got) return 0;
    buf[got] = '\0';

    // Line-anchored so a key that merely CONTAINS the name cannot match.
    for (p = buf; (p = strstr(p, "LogAppend=")) != NULL; p += 10) {
        if (p != buf && p[-1] != '\n' && p[-1] != '\r') continue;
        return (p[10] != '0');
    }
    return 0;
}

static void LogLine(const char *msg)
{
    if (InterlockedCompareExchange(&g_logLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_logLock);
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "version_hook.log");
        // "w" (truncate) is the SHIPPING default, not "a". Appending across
        // every launch is right for a developer bisecting two sessions and
        // wrong for a player: with any diagnostic armed this file grows
        // without bound, and one playthrough here reached ~2 GB. One run per
        // file keeps it to something a user can send and an editor can open.
        //
        // LogAppend=1 in the ini restores the old cross-run behaviour. It is
        // read straight from the file rather than from g_logAppend, because
        // logging starts long before LoadConfig runs - the boot lines are
        // exactly the ones worth keeping, so the setting has to be known
        // before the first of them is written.
        g_logFile = fopen(path, LogAppendRequested() ? "a" : "w");
        g_logLockState = 2;
    } else {
        while (g_logLockState != 2) Sleep(0);
    }
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
    // Size cap. Truncating per run bounds the file across a playthrough, but
    // NOT within a single long session with a diagnostic armed - LogPassRts
    // and the watchdog can both produce lines continuously. Past the cap the
    // log stops growing rather than being rotated: a partial log from the
    // start of a session is more useful than a tail of whatever happened to
    // be last, because the boot lines and the first occurrence of a problem
    // are what get read.
    if (g_logCapped) { LeaveCriticalSection(&g_logLock); return; }
    fprintf(g_logFile, "%s\n", msg);
    if (g_logMaxMB > 0) {
        g_logBytes += (LONGLONG)strlen(msg) + 1;
        if (g_logBytes > (LONGLONG)g_logMaxMB * 1048576) {
            fprintf(g_logFile, "[log] size cap of %ld MB reached - logging stops here."
                               " Raise LogMaxMB (0 = unlimited) in version_hook_config.ini.\n",
                    g_logMaxMB);
            fflush(g_logFile);
            g_logCapped = 1;
        }
    }
    // BOOT ONLY: flush every line until the first monitor window clears this.
    // Buffered logging is right for the steady state (see the note above),
    // but it made a boot crash undiagnosable - everything up to the fault was
    // lost and the log's last lines came from the PREVIOUS session, which
    // sent me guessing at the diff instead of reading the failure.
    if (g_bootFlush) fflush(g_logFile);
    // fflush per line REMOVED - it was a synchronous disk write inside a lock
    // the MAIN THREAD also takes (via the "[d3d9] SLOW ..." paths). With the
    // stutter watchdog running at a low threshold it wrote ~75 flushed lines a
    // second into a multi-megabyte file, and a stack-walk capture found the
    // main thread parked in KERNELBASE under VERSION.dll on 42% of slow frames
    // - i.e. the instrument was manufacturing the slow frames it was there to
    // diagnose. An instrument that perturbs what it measures is worse than
    // useless, which this file already says about the D3D9 overlay it refused
    // to build; the same rule applies here and was missed.
    //
    // The buffer is flushed once per monitor tick (500ms) instead, so at most
    // half a second of log is at risk on a hard crash. That tradeoff is
    // deliberate and worth stating: several crashes in this project were
    // diagnosed from the log tail. If a crash ever loses its last lines,
    // FlushLog() can be called from wherever the risky work happens rather
    // than restoring the per-line flush.
    LeaveCriticalSection(&g_logLock);
}

static void FlushLog(void)
{
    if (g_logLockState != 2 || !g_logFile) return;
    EnterCriticalSection(&g_logLock);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

// ---- RDTSC -> microseconds calibration -----------------------------------

static double g_cyclesPerUsec = 0.0;

static void CalibrateTsc(void)
{
    LARGE_INTEGER qpcFreq, qpc1, qpc2;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpc1);
    unsigned __int64 tsc1 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&qpc2);
    unsigned __int64 tsc2 = __rdtsc();

    g_tscBase = tsc1;
    double qpcDeltaSec = (double)(qpc2.QuadPart - qpc1.QuadPart) / (double)qpcFreq.QuadPart;
    double tscDelta = (double)(tsc2 - tsc1);
    g_cyclesPerUsec = tscDelta / (qpcDeltaSec * 1000000.0);

    char line[128];
    sprintf(line, "TSC calibration: %.3f cycles/usec", g_cyclesPerUsec);
    LogLine(line);
}

// ---- per-function state -----------------------------------------------------

#define FN_AACF10  0
#define FN_A2ADA0  1
#define FN_D19A00  2
#define FN_AC3040  3
#define FN_A01A00  4
#define FN_A015B0  5
// v17: FUN_00a41570 - the per-entity update loop found in the class of
// gameplay-era stutters remaining after causes 1/2 were fixed/mitigated and
// the overlay-software theory was directly disconfirmed by the user's test.
// Its only reference Ghidra could find is a DATA address (an indirect
// call/vtable slot, not a call instruction), so static analysis can't trace
// its caller any further - hooking it directly gets the true caller
// (already captured by the existing return-hijack machinery as
// trueRetAddr, just needs logging once) plus real call rate and duration,
// empirically, the same way every other lead in this investigation that hit
// a static-analysis wall was eventually resolved.
#define FN_A41570  6
// v17b: FUN_00aa7850 - the shader-compile queue drain, found via the
// FUN_00a95860/FUN_00a57c70/FUN_00a58630 chain (.vpo/.fpo shader container
// parsing, "CDev.Dw.Renderer.Shader") that a watchdog capture caught live in
// an actual stutter. Its own exit check (FUN_00aa7420) is a genuine 80%-of-
// budget time check between queue items - already throttled in principle -
// but that check only runs BETWEEN items, never during one, so a single
// expensive shader compile could still cost a whole frame regardless of the
// budget. Timing this function directly tests that theory: does duration
// spike during stutters (one slow item), or does call count spike (a large
// backlog all becoming eligible at once)?
#define FN_AA7850  7
#define NUM_FNS    8

typedef struct {
    const char *name;
    void *target;
    unsigned int rva;
    int patchLen;
    volatile LONG callCount;
    volatile LONG durationCount;
    volatile LONGLONG durationSumCycles; // not perfectly atomic on 32-bit; fine for a diagnostic average
    volatile LONG maxUsec; // windowed - monitor resets each report, same pattern as D3DSlot
} HookedFunc;

static HookedFunc g_funcs[NUM_FNS] = {
    { "FUN_00aacf10", NULL, 0x00aacf10 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00a2ada0", NULL, 0x00a2ada0 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00d19a00", NULL, 0x00d19a00 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00ac3040", NULL, 0x00ac3040 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00a01a00", NULL, 0x00a01a00 - 0x00400000, 5, 0, 0, 0 },
    { "FUN_00a015b0", NULL, 0x00a015b0 - 0x00400000, 5, 0, 0, 0 },
    { "FUN_00a41570", NULL, 0x00a41570 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00aa7850", NULL, 0x00aa7850 - 0x00400000, 6, 0, 0, 0 },
};

static void *g_trampoline_aacf10 = NULL;
static void *g_trampoline_a2ada0 = NULL;
static void *g_trampoline_d19a00 = NULL;
static void *g_trampoline_ac3040 = NULL;
static void *g_trampoline_a01a00 = NULL;
static void *g_trampoline_a015b0 = NULL;
static void *g_trampoline_a41570 = NULL;
static void *g_trampoline_aa7850 = NULL;

#define STACK_DEPTH 16
typedef struct {
    void *trueRetAddr;
    unsigned __int64 entryTsc;
} RetFrame;
typedef struct {
    int top;
    RetFrame frames[STACK_DEPTH];
} ThreadStack;

static DWORD g_stackTls[NUM_FNS];

static ThreadStack *GetThreadStack(int fnIdx)
{
    ThreadStack *ts = (ThreadStack *)TlsGetValue(g_stackTls[fnIdx]);
    if (!ts) {
        ts = (ThreadStack *)calloc(1, sizeof(ThreadStack));
        if (ts) {
            TlsSetValue(g_stackTls[fnIdx], ts);
        }
    }
    return ts;
}

// Returns 1 if a frame was recorded (caller should hijack the return
// address), 0 if the per-thread stack was full (caller must NOT hijack -
// leave the true return address alone for this call).
static int OnEnterBookkeeping(void *trueRetAddr, int fnIdx)
{
    InterlockedIncrement(&g_funcs[fnIdx].callCount);
    ThreadStack *ts = GetThreadStack(fnIdx);
    if (!ts || ts->top >= STACK_DEPTH) {
        return 0;
    }
    ts->frames[ts->top].trueRetAddr = trueRetAddr;
    ts->frames[ts->top].entryTsc = __rdtsc();
    ts->top++;
    return 1;
}

// Structurally only ever called for a call that OnEnterBookkeeping
// successfully recorded (see asm: hijack is conditional on its return
// value), so ts->top > 0 is guaranteed here.
// Tentative def: the frametime-capture block that owns this sits further down,
// but OnReturnBookkeeping below needs to write it.
static volatile LONG g_lastAc3040Usec;

static void *OnReturnBookkeeping(int fnIdx)
{
    ThreadStack *ts = GetThreadStack(fnIdx);
    ts->top--;
    RetFrame f = ts->frames[ts->top];
    unsigned __int64 elapsed = __rdtsc() - f.entryTsc;

    HookedFunc *hf = &g_funcs[fnIdx];
    InterlockedIncrement(&hf->durationCount);
    hf->durationSumCycles += (LONGLONG)elapsed;
    if (g_cyclesPerUsec > 0.0) {
        LONG usec = (LONG)((double)elapsed / g_cyclesPerUsec);
        if (usec > hf->maxUsec) hf->maxUsec = usec;
        // Keep the LAST duration of the frame function specifically, so the
        // frametime capture can carry it as a column. The monitor's windowed
        // average already showed FUN_00ac3040 eating ~4013us of a ~10930us
        // frame with a max of 8434us - a 2.1x spread that closely matches the
        // frametime's own 1.76x fast/slow ratio. Per-frame values settle
        // whether the 9-frame wave lives INSIDE this call or outside it.
        if (fnIdx == FN_AC3040) g_lastAc3040Usec = usec;
    }

    return f.trueRetAddr;
}

// Written out explicitly rather than macro-generated - MSVC's inline
// assembler doesn't reliably handle multi-line __asm blocks inside macro
// expansion (confirmed: it does not, build caught it immediately with
// C2400 syntax errors). Repetitive, but this is the reliable form.

__declspec(noinline) int __cdecl OnEnter_aacf10_C(void *r) { return OnEnterBookkeeping(r, FN_AACF10); }
__declspec(noinline) void *__cdecl OnReturn_aacf10_C(void) { return OnReturnBookkeeping(FN_AACF10); }
__declspec(naked) void OnReturn_aacf10(void)
{
    __asm {
        pushfd
        call OnReturn_aacf10_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_a2ada0_C(void *r) { return OnEnterBookkeeping(r, FN_A2ADA0); }
__declspec(noinline) void *__cdecl OnReturn_a2ada0_C(void) { return OnReturnBookkeeping(FN_A2ADA0); }
__declspec(naked) void OnReturn_a2ada0(void)
{
    __asm {
        pushfd
        call OnReturn_a2ada0_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_d19a00_C(void *r) { return OnEnterBookkeeping(r, FN_D19A00); }
__declspec(noinline) void *__cdecl OnReturn_d19a00_C(void) { return OnReturnBookkeeping(FN_D19A00); }
__declspec(naked) void OnReturn_d19a00(void)
{
    __asm {
        pushfd
        call OnReturn_d19a00_C
        popfd
        jmp eax
    }
}

// FUN_00ac3040 is the frame pacer: once per frame, main thread only. Its
// caller's thread id therefore identifies the main thread, which the
// allocation warmer needs in order to leave the main thread's own
// allocations alone.
// Confirmed from the v7 log: exactly 30 calls per 500ms window at a locked
// 60fps, so the interval between consecutive entries IS the frame time. That
// makes this the cheapest possible in-process frame-time probe, and - unlike
// PresentMon - it lands in the SAME log, in the SAME 500ms windows, as the
// I/O census. Correlating a stutter with a streaming burst therefore needs no
// clock alignment between two tools.
// ---- Stutter watchdog (v9) ------------------------------------------------
// v8's frame probe overturned the eviction model: in the heaviest streaming
// windows the AVERAGE frame stays at 16.7-17.9ms while a single frame hits
// 30-56ms. One window streamed 8.5MB with a worst frame of 17.0ms; another
// streamed 50MB and still averaged 17.9ms with one 56ms spike. Cache eviction
// would drag every frame down uniformly - it does not. An isolated long frame
// surrounded by perfect ones is a discrete BLOCKING event on the main thread.
//
// So: catch it in the act. The main thread stamps a timestamp and a sequence
// number at each frame boundary; this watchdog polls every 2ms and, once a
// frame has been running longer than STUTTER_THRESHOLD_USEC, suspends the
// main thread, grabs its EIP and walks its EBP chain, and resumes it. Up to
// three samples per bad frame gives a mini-profile of a single stutter.
//
// Addresses are logged already converted to Ghidra VAs, so they can be looked
// up directly without redoing the ASLR arithmetic by hand each session.
//
// Safety: while the main thread is suspended this code calls only
// GetThreadContext (kernel-side, takes no user-mode lock) and reads raw stack
// memory under SEH. It must never take a lock the main thread could be
// holding, and never logs - records go to a ring that the monitor thread
// drains afterwards. Suspension lasts on the order of microseconds and only
// ever happens during a frame that is already ruined.
// Lowered 22000 -> 16700 now that the engine's 59.94fps limiter is bypassed:
// uncapped the game runs 90-130fps (7.7-11.1ms frames), so 16.7ms is a
// genuine 1.5-2x overshoot rather than the normal frame time it used to be.
// Anything over this is, by definition, a frame that failed to sustain 60fps.
//
// This threshold is ONLY safe while the game is genuinely running well above
// 60fps. If a scene is GPU-bound down to ~60, every frame crosses it and the
// watchdog - which SuspendThreads the main thread and walks its stack - would
// fire constantly and become a significant stutter source itself. The rate
// limit below bounds that: detection of every slow frame is the histogram's
// job (free), and this expensive path only needs enough samples to attribute
// causes.
#define STUTTER_THRESHOLD_USEC 16700
// Never suspend the main thread more than this many times per second,
// however many frames cross the threshold.
#define STUTTER_MAX_CAPTURES_PER_SEC 25
static volatile LONG g_stutterRateLimited = 0;   // slow frames whose capture was skipped by the rate limit above
#define STUTTER_MAX_SAMPLES_PER_FRAME 3
#define MAX_STUTTER_RECS 64
#define STUTTER_STACK_DEPTH 16
#define STUTTER_SCAN_DEPTH 14

typedef struct {
    LONG elapsedUsec;
    DWORD eip;
    DWORD esp;
    DWORD stack[STUTTER_STACK_DEPTH];
    LONG stackCount;
    DWORD scan[STUTTER_SCAN_DEPTH];
    LONG scanCount;
    LONG csWaitUsec;
    DWORD csPtr;
    DWORD csOwnerTid;
    LONG wfsoWaitUsec;
    DWORD wfsoHandle;
    LONG readCountInFrame;
    LONG readKbInFrame;
    LONG paceUsecInFrame;
    LONG allocsInFrame;
    volatile LONG ready;
} StutterRec;

static StutterRec g_stutterRecs[MAX_STUTTER_RECS];
static volatile LONG g_stutterWrite = 0;
static volatile LONG g_stutterMissed = 0;

// What the main thread is currently blocked on, if anything. Written only by
// the main thread, read by the watchdog - a torn read costs at worst one
// misleading diagnostic line, never correctness.
// Per-window aggregates of main-thread blocking, so the cost shows up even in
// frames the watchdog did not happen to sample.

static HANDLE g_mainThreadHandle = NULL;
static volatile LONG g_frameStartUsec = 0;
static volatile LONG g_frameSeq = 0;
static unsigned __int64 g_tscBase = 0;

// Frame-scoped deltas, so a captured stutter can be attributed to what
// happened during that specific frame rather than that 500ms window.
static LONG g_frameBaseReadCount = 0;
static LONGLONG g_frameBaseReadBytes = 0;
static LONG g_frameBasePaceUsec = 0;
static LONG g_frameBaseAllocs = 0;

static LONG NowUsec(void)
{
    if (g_cyclesPerUsec <= 0.0) return 0;
    return (LONG)((double)(__rdtsc() - g_tscBase) / g_cyclesPerUsec);
}

static unsigned __int64 g_lastFrameTsc = 0;
static volatile LONG g_maxFrameUsec = 0;   // monitor resets each window
static volatile LONG g_frameCount = 0;
static volatile LONG g_frameSumUsec = 0;
// 0.5ms buckets up to 64ms, last bucket is the overflow catch-all.
#define FRAME_HIST_BUCKETS 129
static LONG g_frameHist[FRAME_HIST_BUCKETS];
// Last window's computed stats, published for the GUI panel's live readout.
// RTSS cannot show these: it measures PRESENTS, and this game's presentation
// runs on its own cadence independent of the logic tick - which is exactly
// why its graph stays flat through a visible hitch. These are the engine's
// real per-tick numbers.
static volatile LONG g_liveP50 = 0, g_liveP99 = 0, g_liveOver16 = 0;
static volatile LONG g_liveFrames = 0, g_liveWorst = 0;

// Ring of recent per-tick frame times for the graph overlay. Written by the
// main thread at the frame boundary (one store, no lock), read by the GUI
// thread while painting. A torn read would at worst draw one wrong bar for
// one repaint, which is not worth a lock in the frame path.
#define FRAME_RING 240
static LONG g_frameRing[FRAME_RING];
static volatile LONG g_frameRingPos = 0;

// ---- Frametime burst capture ---------------------------------------------
// The overlay shows a clearly PERIODIC pattern - clusters of fast frames
// alternating with clusters of slow ones - which is not jitter (jitter gives a
// smooth distribution, this gives two populations). 240 bars eyeballed on a
// screenshot cannot give the period reliably, and the period is the whole
// diagnostic:
//
//   period constant in FRAMES  -> a task that runs every N frames
//   period constant in TIME    -> beating against a fixed-rate process
//                                 (the 59.94Hz sim grid, display refresh, a timer)
//
// Those two want completely different fixes, and telling them apart only needs
// the same capture taken at two different framerates. So: dump raw consecutive
// durations and measure it offline instead of guessing from a picture.
//
// Toggling the flag ON restarts the capture, so it is a "capture from here"
// button - the same pattern the shader-constant dump ended up needing.
#define FT_CAPTURE_MAX 2048
static LONG g_ftBuf[FT_CAPTURE_MAX];
// Second column: CPU time spent in the DRAW_SHADOW pass on the same frame.
// The frametime period turned out to be a fixed 9 FRAMES (invariant across
// 105/101/86 fps), so it is a frame-counted task rather than a beat - and the
// heavy phase's cost scales hard with scene and shadow load (gap 2751us at
// 105fps vs 6425us at 86fps). That points at a periodic render task, and the
// user independently observed the same square pattern appearing/disappearing
// with the shadow resolution setting. Correlating the two directly settles it:
// if the shadow pass carries the same 9-frame square wave, it is the source;
// if it is flat, shadows are innocent and the cycle lives elsewhere.
//
// CAVEAT worth remembering when reading the result: this measures CPU time
// spent issuing the pass, not GPU time executing it. D3D9 queues work, so a
// GPU-bound pass can look cheap here while the cost surfaces later as a block
// in whatever call fills the queue. A flat reading therefore rules the shadow
// pass out as a CPU cost, NOT as a GPU cost.
static LONG g_ftShadow[FT_CAPTURE_MAX];
// Further columns, all deltas over the same frame. Instant EIP sampling turned
// out to be the wrong instrument for "where did the extra 6ms go": the main
// thread makes ~1300 WaitForSingleObject calls per frame averaging 0.63us, so
// a stack sample lands in the wait a third of the time purely on volume, while
// the actual waits total well under 200us. Per-frame COUNTS correlated against
// the wave say what is different about the slow frames; a single instantaneous
// stack cannot.
static LONG g_ftAllocs[FT_CAPTURE_MAX];
static LONG g_ftReads[FT_CAPTURE_MAX];
static LONG g_ftWfso[FT_CAPTURE_MAX];
// Duration of FUN_00ac3040 itself on the same frame. It is measured
// entry-to-return, while the frame time is measured entry-to-ENTRY, so the two
// are independent: if this column carries the 9-frame wave the cost is inside
// the call, if it is flat the cost is in the rest of the frame.
static LONG g_ftPacer[FT_CAPTURE_MAX];
static volatile LONG g_ftCount = 0;
static volatile LONG g_ftDumped = 0;
static volatile LONG g_logFrameTimes;   // tentative def; toggle table is earlier
static volatile LONG g_shadowPassUsec = 0;
// Previous-frame bases for the delta columns above (main thread only).
static LONG g_ftPrevAllocs = 0, g_ftPrevReads = 0, g_ftPrevWfso = 0;

__declspec(noinline) int __cdecl OnEnter_ac3040_C(void *r)
{
    if (!g_mainThreadId) g_mainThreadId = (LONG)GetCurrentThreadId();
    // Frame boundary: clear the draw-pass tag so targets bound before
    // DRAW_SHADOW are labelled (none) rather than carrying last frame's pass.
    g_curPass = PASS_NONE;
    // ...and arm the MSAA depth clear, so it happens exactly once per frame
    // regardless of how many times the scene target is bound.
    g_msNeedDepthClear = 1;
    g_msFrameSeq++;
    unsigned __int64 now = __rdtsc();
    if (g_lastFrameTsc != 0 && g_cyclesPerUsec > 0.0) {
        LONG usec = (LONG)((double)(now - g_lastFrameTsc) / g_cyclesPerUsec);
        // Main thread only, once per frame - no contention, plain compares are
        // fine; the monitor thread is the only other reader/writer and it only
        // ever resets.
        if (usec > g_maxFrameUsec) g_maxFrameUsec = usec;
        g_frameSumUsec += usec;
        g_frameCount++;
        // Full frame-time histogram, 0.5ms buckets. Detection is deliberately
        // separated from attribution here: this costs one array increment on
        // the main thread and catches EVERYTHING including sub-threshold
        // jitter, whereas the stutter watchdog (which suspends the main
        // thread and walks its stack) is far too invasive to fire on every
        // frame. Percentiles are computed from this offline, so no ordering
        // or storage of individual samples is needed.
        {
            LONG b = usec / 500;
            if (b < 0) b = 0;
            if (b >= FRAME_HIST_BUCKETS) b = FRAME_HIST_BUCKETS - 1;
            g_frameHist[b]++;
            LONG rp = g_frameRingPos;
            g_frameRing[rp % FRAME_RING] = usec;
            g_frameRingPos = rp + 1;
            // One predictable branch and a store on the frame path; the
            // formatting all happens on the monitor thread.
            if (g_logFrameTimes) {
                LONG n = g_ftCount;
                LONG aNow = g_namedHeapCount, rNow = g_readFileCount, wNow = g_wfsoCallCount;
                if (n < FT_CAPTURE_MAX) {
                    g_ftBuf[n]    = usec;
                    g_ftShadow[n] = g_shadowPassUsec;
                    g_ftAllocs[n] = aNow - g_ftPrevAllocs;
                    g_ftReads[n]  = rNow - g_ftPrevReads;
                    g_ftWfso[n]   = wNow - g_ftPrevWfso;
                    // Duration of the PREVIOUS frame's FUN_00ac3040 call - we
                    // are at its entry, so its own return has not happened yet.
                    g_ftPacer[n]  = g_lastAc3040Usec;
                    g_ftCount = n + 1;
                }
                g_ftPrevAllocs = aNow; g_ftPrevReads = rNow; g_ftPrevWfso = wNow;
            }
        }
    }
    g_lastFrameTsc = now;

    // Frame boundary: rebase the per-frame deltas and republish the frame
    // start, which is what the watchdog polls against.
    g_frameBaseReadCount = g_readFileCount;
    g_frameBaseReadBytes = g_readFileBytes;
    g_frameBasePaceUsec = g_readPaceDelayTotalUsec;
    g_frameBaseAllocs = g_namedHeapCount;
    // Issue a bounded batch of deferred GPU copies here: once per frame, on
    // the main thread, in the same command stream the engine itself uses.
    DrainStagedUploads();

    // Per-frame, not on the 500ms monitor tick. The engine recomputes the
    // cascade splits every frame, so writing twice a second produced exactly
    // twice-a-second blinking - our value for one frame, the engine's for the
    // rest. Writing on the engine's own frame boundary keeps it applied, and
    // confirms the fields are both correct and visibly effective.
    ApplyCascadeSplitSource();

    g_frameStartUsec = NowUsec();
    InterlockedIncrement(&g_frameSeq);

    return OnEnterBookkeeping(r, FN_AC3040);
}

static DWORD WINAPI StutterWatchdogThread(LPVOID param)
{
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LONG lastSeq = -1;
    int samplesThisFrame = 0;
    for (;;) {
        Sleep(2);
        if (!g_mainThreadHandle || g_cyclesPerUsec <= 0.0) continue;

        LONG seq = g_frameSeq;
        if (seq != lastSeq) { lastSeq = seq; samplesThisFrame = 0; }
        if (samplesThisFrame >= STUTTER_MAX_SAMPLES_PER_FRAME) continue;

        LONG elapsed = NowUsec() - g_frameStartUsec;
        if (elapsed < g_stutterThresholdUsec + samplesThisFrame * 20000) continue;

        // Rate limit: bound how often the main thread gets suspended, so a
        // stretch where every frame crosses the threshold cannot turn this
        // diagnostic into a stutter source of its own.
        {
            static LONG windowStartUsec = 0;
            static LONG capturesThisSec = 0;
            LONG nowUs = NowUsec();
            if (windowStartUsec == 0 || (nowUs - windowStartUsec) >= 1000000) {
                windowStartUsec = nowUs;
                capturesThisSec = 0;
            }
            if (capturesThisSec >= STUTTER_MAX_CAPTURES_PER_SEC) {
                // Distinguishes "no slow frames happened" from "slow frames
                // happened but attribution was rate-limited" - the two look
                // identical in the [stutter] record count alone. Read
                // alongside [frametime]'s over-threshold count: if this stays
                // near zero, the watchdog's records/stall totals are a
                // complete account of what the histogram detected. If it
                // climbs, they are an undercount - the histogram still caught
                // every frame, but some got no stack trace and are not in any
                // records=N/stall=Xs total quoted from watchdog data.
                InterlockedIncrement(&g_stutterRateLimited);
                continue;
            }
            capturesThisSec++;
        }

        StutterRec rec;
        memset(&rec, 0, sizeof(rec));
        rec.elapsedUsec = elapsed;

        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;

        if (SuspendThread(g_mainThreadHandle) == (DWORD)-1) continue;
        // Re-check the sequence: if the frame ended between the elapsed test
        // and the suspend, this sample belongs to a frame that already
        // finished and would be misleading.
        int stale = (g_frameSeq != seq);
        if (!stale && GetThreadContext(g_mainThreadHandle, &ctx)) {
            rec.eip = ctx.Eip;
            rec.esp = ctx.Esp;
            __try {
                DWORD *ebp = (DWORD *)ctx.Ebp;
                for (int i = 0; i < STUTTER_STACK_DEPTH; i++) {
                    if (!ebp) break;
                    DWORD ret = ebp[1];
                    if (!ret) break;
                    rec.stack[rec.stackCount++] = ret;
                    DWORD *next = (DWORD *)ebp[0];
                    // Frames must walk monotonically toward higher addresses;
                    // anything else means FPO or a corrupt chain - stop.
                    if (next <= ebp) break;
                    ebp = next;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // partial stack is still useful
            }

            // Raw stack scan. The EBP chain dies immediately inside FPO
            // system/driver code, which is exactly where the main thread was
            // found 96% of the time - so also sweep the raw stack for values
            // that point into the game module. Those are the return addresses
            // that say which game code entered the blocking call.
            __try {
                DWORD *sp = (DWORD *)ctx.Esp;
                for (int i = 0; i < 1024 && rec.scanCount < STUTTER_SCAN_DEPTH; i++) {
                    DWORD v = sp[i];
                    if (v >= g_mainModBase && v < g_mainModBase + g_mainModSize) {
                        rec.scan[rec.scanCount++] = v;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ResumeThread(g_mainThreadHandle);
        if (stale || rec.eip == 0) continue;

        samplesThisFrame++;

        rec.readCountInFrame = g_readFileCount - g_frameBaseReadCount;
        rec.readKbInFrame = (LONG)((g_readFileBytes - g_frameBaseReadBytes) / 1024);
        rec.paceUsecInFrame = g_readPaceDelayTotalUsec - g_frameBasePaceUsec;
        rec.allocsInFrame = g_namedHeapCount - g_frameBaseAllocs;

        LONG csStart = g_mainCsWaitStartUsec;
        if (csStart) {
            rec.csWaitUsec = NowUsec() - csStart;
            rec.csPtr = (DWORD)g_mainCsPtr;
            rec.csOwnerTid = (DWORD)g_mainCsOwner;
        }
        LONG wStart = g_mainWfsoStartUsec;
        if (wStart) {
            rec.wfsoWaitUsec = NowUsec() - wStart;
            rec.wfsoHandle = (DWORD)g_mainWfsoHandle;
        }

        LONG slot = InterlockedIncrement(&g_stutterWrite) - 1;
        StutterRec *dst = &g_stutterRecs[slot % MAX_STUTTER_RECS];
        if (dst->ready) {
            InterlockedIncrement(&g_stutterMissed);
            continue;
        }
        rec.ready = 0;
        memcpy(dst, &rec, sizeof(rec));
        dst->ready = 1;
    }
}
__declspec(noinline) void *__cdecl OnReturn_ac3040_C(void) { return OnReturnBookkeeping(FN_AC3040); }
__declspec(naked) void OnReturn_ac3040(void)
{
    __asm {
        pushfd
        call OnReturn_ac3040_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_a01a00_C(void *r) { return OnEnterBookkeeping(r, FN_A01A00); }
__declspec(noinline) void *__cdecl OnReturn_a01a00_C(void) { return OnReturnBookkeeping(FN_A01A00); }
__declspec(naked) void OnReturn_a01a00(void)
{
    __asm {
        pushfd
        call OnReturn_a01a00_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_a015b0_C(void *r) { return OnEnterBookkeeping(r, FN_A015B0); }
__declspec(noinline) void *__cdecl OnReturn_a015b0_C(void) { return OnReturnBookkeeping(FN_A015B0); }
__declspec(naked) void OnReturn_a015b0(void)
{
    __asm {
        pushfd
        call OnReturn_a015b0_C
        popfd
        jmp eax
    }
}

// Its only static reference is a data slot (an indirect call target), so the
// caller can't be traced statically - trueRetAddr, already captured by the
// existing hijack machinery for every hooked function, IS the answer;
// logging it (deduplicated by module-relative address, first few distinct
// callers only) turns this into an empirical answer instead of a guess.
#define MAX_A41570_CALLERS 8
static DWORD g_a41570Callers[MAX_A41570_CALLERS];
static volatile LONG g_a41570CallerCount = 0;

__declspec(noinline) int __cdecl OnEnter_a41570_C(void *r)
{
    DWORD rva = (DWORD)r - (DWORD)GetModuleHandleA(NULL) + 0x00400000;
    LONG n = g_a41570CallerCount;
    int known = 0;
    for (LONG i = 0; i < n; i++) {
        if (g_a41570Callers[i] == rva) { known = 1; break; }
    }
    if (!known && n < MAX_A41570_CALLERS) {
        LONG slot = InterlockedIncrement(&g_a41570CallerCount) - 1;
        if (slot < MAX_A41570_CALLERS) {
            g_a41570Callers[slot] = rva;
            char line[96];
            sprintf(line, "[a41570] new caller: %08lX", (unsigned long)rva);
            LogLine(line);
        }
    }
    return OnEnterBookkeeping(r, FN_A41570);
}
__declspec(noinline) void *__cdecl OnReturn_a41570_C(void) { return OnReturnBookkeeping(FN_A41570); }
__declspec(naked) void OnReturn_a41570(void)
{
    __asm {
        pushfd
        call OnReturn_a41570_C
        popfd
        jmp eax
    }
}

// Fix attempt 21: FUN_00aa7850 (shader-compile queue drain) already has a
// time-budget check between items (FUN_00aa7420: accumulated <= 80% of
// *(int*)(this+0x5c), both in microseconds - confirmed via FUN_0049a310,
// which is QueryPerformanceCounter converted to microseconds). Content-
// verified live captures (v20) confirmed genuine shader recompiles bursting
// to hundreds of items within a ~5-10s window after leaving and returning to
// an area, costing up to 22ms in a single call. The check never interrupts
// an item already in progress and always lets at least one item through per
// queue per call (the work happens BEFORE the budget check, not after), so
// temporarily lowering this value cannot cause starvation - only spreads the
// same backlog over more frames instead of bursting it into one, the same
// strategy already proven for the ReadFile limiter, this time aimed at the
// actual mechanism instead of a symptom.
//
// thisPtr (ECX) is read directly from the register, still valid at the
// point of the extra push since nothing between function entry and here
// writes to the ECX register itself (only pushes its value for
// preservation). The original value is restored immediately in
// OnReturn_aa7850_C so nothing else that reads this field between frames
// ever observes it altered - only this one drain call, this one frame.
//
// The budget modification is gated on OnEnterBookkeeping's hijack having
// succeeded (guaranteed return-hijack -> OnReturn will fire to restore it);
// if the hijack failed (per-thread stack full - not expected for a main-
// thread-only, non-reentrant, once-per-frame call, but the path exists),
// the budget is left untouched rather than risk a save with no matching
// restore.
// DISABLED (v23 result): capping this budget causes severe visual
// corruption - distant geometry and even the player character render
// black/transparent, worse at lower resolution, worse the longer the
// throttle stays engaged. The safety analysis behind this fix only checked
// "does the queue eventually finish" (yes - no starvation) but missed the
// actual constraint: an object with a not-yet-compiled shader apparently
// renders as black/nothing rather than waiting or falling back, so what
// matters is how LONG a shader stays pending, not just total CPU spent on
// compiling. The real 20ms/frame default was almost certainly the original
// developers' own deliberate tradeoff - accept a CPU spike to keep that
// window imperceptibly short - not an oversight to correct. Throttling it
// harder optimized the wrong variable (per-frame CPU cost) at the direct
// expense of the one that actually matters visually (time-to-ready). Left
// disabled by default (g_shaderThrottleEnabled starts at 0) pending a
// fundamentally different approach that doesn't extend how long a shader
// stays uncompiled - hot-toggleable via F10 for isolation testing without
// a rebuild, but default OFF until a real fix exists.
#define SHADER_QUEUE_BUDGET_OFFSET 0x5c
#define SHADER_QUEUE_REDUCED_BUDGET_USEC 500
static LONG g_shaderBudgetOriginal = 0;
static DWORD g_shaderBudgetThisPtr = 0;
static volatile LONG g_shaderBudgetLoggedCount = 0;
// v21 result: the first-20-ever log only ever caught quiet, early-session
// calls (all exactly 2000 - coincidentally already at the reduction target,
// so the reduction branch never fired for any of them), giving no real
// visibility into whether the throttle engages during an actual burst -
// despite that, the shader queue's own worst-case per-call cost still
// dropped 2-3x (22ms->11.5ms) session over session, implying the engagement
// IS happening on later, unlogged calls. Replacing the "first N ever" cap
// with running min/max (cheap, every call) plus individual logs specifically
// when a reduction actually engages, capped separately - directly answers
// whether/how often the throttle does real work instead of inferring it.
static volatile LONG g_shaderBudgetMinSeen = 0x7FFFFFFF;
static volatile LONG g_shaderBudgetMaxSeen = 0;
static volatile LONG g_shaderBudgetEngagedCount = 0;
static volatile LONG g_shaderBudgetEngagedLogged = 0;

__declspec(noinline) int __cdecl OnEnter_aa7850_C(void *r, DWORD thisPtr)
{
    int hijacked = OnEnterBookkeeping(r, FN_AA7850);
    if (hijacked) {
        __try {
            int *budgetPtr = (int *)(thisPtr + SHADER_QUEUE_BUDGET_OFFSET);
            int original = *budgetPtr;
            if (original < g_shaderBudgetMinSeen) g_shaderBudgetMinSeen = original;
            if (original > g_shaderBudgetMaxSeen) g_shaderBudgetMaxSeen = original;
            if (g_shaderThrottleEnabled && original > SHADER_QUEUE_REDUCED_BUDGET_USEC) {
                InterlockedIncrement(&g_shaderBudgetEngagedCount);
                if (g_shaderBudgetEngagedLogged < 50) {
                    InterlockedIncrement(&g_shaderBudgetEngagedLogged);
                    char line[128];
                    sprintf(line, "[shaderbudget] ENGAGED original_usec=%d -> capped to %d",
                            original, SHADER_QUEUE_REDUCED_BUDGET_USEC);
                    LogLine(line);
                }
                g_shaderBudgetOriginal = original;
                g_shaderBudgetThisPtr = thisPtr;
                *budgetPtr = SHADER_QUEUE_REDUCED_BUDGET_USEC;
            } else {
                g_shaderBudgetThisPtr = 0; // disabled, or already <= target - nothing to restore
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_shaderBudgetThisPtr = 0;
        }
    }
    return hijacked;
}
__declspec(noinline) void *__cdecl OnReturn_aa7850_C(void)
{
    if (g_shaderBudgetThisPtr) {
        __try {
            *(int *)(g_shaderBudgetThisPtr + SHADER_QUEUE_BUDGET_OFFSET) = g_shaderBudgetOriginal;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_shaderBudgetThisPtr = 0;
    }
    return OnReturnBookkeeping(FN_AA7850);
}
__declspec(naked) void OnReturn_aa7850(void)
{
    __asm {
        pushfd
        call OnReturn_aa7850_C
        popfd
        jmp eax
    }
}

// Entry detours. Preserve ECX/EDX around our own C call regardless of each
// target's exact calling convention - costs nothing, removes any risk of
// corrupting a live fastcall/thiscall argument register. Return-address
// hijack is conditional on OnEnter's result (see file header/PROGRESS.md
// for the full safety reasoning carried over from v2).
__declspec(naked) void Detour_aacf10(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_aacf10_C
        add esp, 4
        test eax, eax
        jz skip_aacf10
        mov dword ptr [esp + 8], offset OnReturn_aacf10
    skip_aacf10:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_aacf10]
    }
}
__declspec(naked) void Detour_a2ada0(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a2ada0_C
        add esp, 4
        test eax, eax
        jz skip_a2ada0
        mov dword ptr [esp + 8], offset OnReturn_a2ada0
    skip_a2ada0:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a2ada0]
    }
}
__declspec(naked) void Detour_d19a00(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_d19a00_C
        add esp, 4
        test eax, eax
        jz skip_d19a00
        mov dword ptr [esp + 8], offset OnReturn_d19a00
    skip_d19a00:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_d19a00]
    }
}

// param_1[0xc]/[0xd] in the decompile are float-typed slots that actually
// hold the raw uint32 low/high halves of a 64-bit QueryPerformanceCounter
// deadline (confirmed: the decompile assigns them via
// `(float)local_1c.s.LowPart` - a bit-pattern reinterpret, not a real float
// conversion). In memory that is bytes +0x30/+0x34 on the pacer object
// FUN_00ac3040 receives in ECX. Read/write as raw DWORDs at those offsets.
//
// Applied BEFORE FUN_00ac3040 runs each frame (called from Detour_ac3040,
// which already has ECX saved on the stack for the existing return-address
// hijack), so by the time the engine's own logic reads the deadline it is
// already clamped and never takes its own catch-up branch.
static void __cdecl ClampAc3040Deadline(void *pacer)
{
    if (!g_clampDeadlineEnabled || !pacer || g_mainModBase == 0) return;

    volatile DWORD *lowPart  = (volatile DWORD *)((char *)pacer + 0x30);
    volatile DWORD *highPart = (volatile DWORD *)((char *)pacer + 0x34);

    LARGE_INTEGER now, deadline, interval;
    if (!QueryPerformanceCounter(&now)) return;
    deadline.LowPart = *lowPart;
    deadline.HighPart = (LONG)*highPart;

    volatile unsigned int *ticks =
        (volatile unsigned int *)(g_mainModBase + FRAME_TARGET_TICKS_RVA);
    interval.LowPart = ticks[0];
    interval.HighPart = (LONG)ticks[1];
    if (interval.QuadPart <= 0) return;   // not initialised yet

    // Only clamp a deadline that has fallen more than one interval BEHIND
    // now. On-schedule or slightly-ahead deadlines are left untouched -
    // this must never make a normal frame wait LONGER than it already would.
    if (deadline.QuadPart < now.QuadPart - interval.QuadPart) {
        LARGE_INTEGER clamped;
        clamped.QuadPart = now.QuadPart + interval.QuadPart;
        *lowPart = clamped.LowPart;
        *highPart = (DWORD)clamped.HighPart;
        InterlockedIncrement(&g_deadlineClampCount);
    }
}

__declspec(naked) void Detour_ac3040(void)
{
    __asm {
        push ecx
        push edx
        // ECX (the frame-pacer "this" pointer FUN_00ac3040 expects via
        // __fastcall) sits at [esp+4] right here - pass it to the clamp
        // fix before doing anything else. Stack is restored to identical
        // layout afterward (verified: cdecl callee, caller cleans up),
        // so the original code below is untouched by this insertion.
        mov eax, [esp + 4]
        push eax
        call ClampAc3040Deadline
        add esp, 4
        mov eax, [esp + 8]
        push eax
        call OnEnter_ac3040_C
        add esp, 4
        test eax, eax
        jz skip_ac3040
        mov dword ptr [esp + 8], offset OnReturn_ac3040
    skip_ac3040:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_ac3040]
    }
}
__declspec(naked) void Detour_a01a00(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a01a00_C
        add esp, 4
        test eax, eax
        jz skip_a01a00
        mov dword ptr [esp + 8], offset OnReturn_a01a00
    skip_a01a00:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a01a00]
    }
}
__declspec(naked) void Detour_a015b0(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a015b0_C
        add esp, 4
        test eax, eax
        jz skip_a015b0
        mov dword ptr [esp + 8], offset OnReturn_a015b0
    skip_a015b0:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a015b0]
    }
}
__declspec(naked) void Detour_a41570(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a41570_C
        add esp, 4
        test eax, eax
        jz skip_a41570
        mov dword ptr [esp + 8], offset OnReturn_a41570
    skip_a41570:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a41570]
    }
}
// Passes ECX (thisPtr) as a second argument, unlike every other Detour_xxx
// in this file - ECX is still valid to read at the "push ecx" (3rd push)
// point since nothing between function entry and here writes the ECX
// REGISTER itself (only pushes/pops its value for preservation).
__declspec(naked) void Detour_aa7850(void)
{
    __asm {
        push ecx
        push edx
        push ecx                ; arg2: thisPtr (register still holds it)
        mov eax, [esp + 12]     ; original [esp+8] = retaddr, now at +12 after the extra push
        push eax                ; arg1: trueRetAddr
        call OnEnter_aa7850_C
        add esp, 8
        test eax, eax
        jz skip_aa7850
        mov dword ptr [esp + 8], offset OnReturn_aa7850
    skip_aa7850:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_aa7850]
    }
}

static int InstallJmpHook(HookedFunc *hf, void *detour, void **trampolineOut)
{
    // 16, not 6: hideWindow needs a 7-byte patch (its prologue's last
    // instruction straddles the 6-byte boundary). Every existing caller uses
    // 5 or 6, so this only removes a latent overflow rather than changing
    // any current behaviour.
    unsigned char saved[16];
    if (hf->patchLen < 5 || hf->patchLen > (int)sizeof(saved)) return 0;
    memcpy(saved, hf->target, hf->patchLen);

    unsigned char *tramp = (unsigned char *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;

    memcpy(tramp, saved, hf->patchLen);
    tramp[hf->patchLen] = 0xE9;
    *(int *)(tramp + hf->patchLen + 1) =
        (int)((unsigned char *)hf->target + hf->patchLen) - (int)(tramp + hf->patchLen + 5);
    *trampolineOut = tramp;

    DWORD oldProtect;
    if (!VirtualProtect(hf->target, hf->patchLen, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;

    unsigned char *t = (unsigned char *)hf->target;
    t[0] = 0xE9;
    *(int *)(t + 1) = (int)detour - (int)(t + 5);
    for (int i = 5; i < hf->patchLen; i++) t[i] = 0x90;

    VirtualProtect(hf->target, hf->patchLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), hf->target, hf->patchLen);
    return 1;
}

// Windowed (per-interval), not cumulative-since-start.
static LONG g_prevDurCount[NUM_FNS];
static LONGLONG g_prevDurSum[NUM_FNS];
static LONG g_prevWfsoCount = 0;
static LONGLONG g_prevWfsoSum = 0;
static LONG g_prevWfsoThreadCount[MAX_WFSO_THREADS];
static LONGLONG g_prevWfsoThreadSum[MAX_WFSO_THREADS];
static LONG g_prevDispatchCount = 0;
static LONG g_prevPrefetchedAllocCount = 0;
static LONGLONG g_prevPrefetchedByteCount = 0;
static LONG g_prevAllocThreadCount[MAX_ALLOC_THREADS][NUM_ALLOC_SRC];
static LONGLONG g_prevAllocThreadBytes[MAX_ALLOC_THREADS][NUM_ALLOC_SRC];
static LONG g_prevWarmEnqueued = 0, g_prevWarmDone = 0, g_prevWarmDropped = 0;
static LONGLONG g_prevWarmBytes = 0;

// ---- Config file persistence ----------------------------------------------
// Plain "Key=Value" text file next to the exe - no library needed, trivial
// to hand-edit if something ever needs fixing outside the game. Loaded once
// at startup (before hooks are installed, so the very first frame already
// reflects the saved state) and saved every time a flag changes, from
// whichever control path changed it (hotkey or GUI checkbox).
static void GetConfigPath(char *outPath, size_t outSize)
{
    GetModuleFileNameA(NULL, outPath, (DWORD)outSize);
    char *slash = strrchr(outPath, '\\');
    if (slash) strcpy(slash + 1, "version_hook_config.ini");
}

// ---- Toggleable fixes (GUI checkbox only - hotkeys removed) ---------------
// Used to also carry a vkey and a GetAsyncKeyState poll for a hotkey per
// entry. Dropped: with nine toggles the F-key list had gotten long, and the
// checkboxes are the only control actually used - one interaction surface
// instead of two that had to be kept in sync.
// One entry per toggleable fix - the GUI panel below iterates this list, so
// adding a future fix's toggle is one line here rather than touching
// multiple places.
typedef struct {
    volatile LONG *flag;
    const char *label;      // shown in the GUI checkbox and log lines
    const char *key;        // config-file key; load and save iterate this too
} ToggleableFix;

static ToggleableFix g_toggles[] = {
    { &g_discardFixEnabled,     "DISCARD fix (cause 1 - LockRect)", "DiscardFix" },
    { &g_shaderThrottleEnabled, "Shader-queue budget throttle", "ShaderThrottle" },
    { &g_readPaceEnabled,       "ReadFile rate limiter", "ReadPace" },
    { &g_loaderThrottleEnabled, "Loader dispatch throttle + priority", "LoaderThrottle" },
#if ENABLE_ALLOCATOR_WARM
    { &g_allocatorWarmEnabled,  "Allocator prefetch/warmer", "AllocatorWarm" },
#endif
    { &g_stagingUploadEnabled,  "SYSTEMMEM staging upload (EXPERIMENTAL)", "StagingUpload" },
    { &g_stagingSurfaceEnabled, "SYSTEMMEM staging for Surface::LockRect (EXPERIMENTAL)", "StagingSurface" },
    { &g_stagingCubeEnabled,    "SYSTEMMEM staging for CubeTexture::LockRect (EXPERIMENTAL)", "StagingCube" },
    { &g_hdTexPushEnabled,      "Push staged texture uploads to the HD GUI mod (interop)", "HDTexPush" },
#if ENABLE_CLAMP_DEADLINE
    { &g_clampDeadlineEnabled,  "Clamp frame-limiter deadline (no catch-up) (EXPERIMENTAL)", "ClampDeadline" },
#endif
#if ENABLE_DEFER_UPLOADS
    { &g_deferUploadsEnabled,   "Defer/rate-limit GPU uploads (EXPERIMENTAL)", "DeferUploads" },
#endif
#if ENABLE_TEXTURE_POOL
    { &g_texturePoolEnabled,   "Texture pool - reuse released textures (EXPERIMENTAL)", "TexturePool" },
#endif
    { &g_forceStdD3D9,         "Force plain D3D9 instead of D3D9Ex (EXPERIMENTAL, may not boot)", "ForceStdD3D9" },
#if ENABLE_MANAGED_POOL
    { &g_managedPoolEnabled,   "D3DPOOL_MANAGED textures (needs ForceStdD3D9)", "ManagedPool" },
#endif
#if ENABLE_TIMER_RES
    { &g_timerResEnabled,      "Force 1ms timer resolution (frame-limiter oversleep)", "TimerRes" },
#endif
#if ENABLE_CASCADE_HUNT
    { &g_logShaderConsts,      "Log shader constants once per register (cascade hunt)", "LogShaderConsts" },
    { &g_traceSplitCall,       "Trace shadowSplitRange upload call site (cascade hunt)", "TraceSplitCall" },
#endif
    { &g_overlayEnabled,        "Frametime graph overlay (engine tick)", "GraphOverlay" },
#if ENABLE_FRAMETIME_DUMP
    { &g_logFrameTimes,         "Dump raw frametimes to log (period diagnostic)", "LogFrameTimes" },
#endif
#if ENABLE_SHADOWS_OFF
    { &g_shadowsOff,            "Skip shadow RENDER (diagnostic - keeps the pass alive)", "ShadowsOff" },
#endif
    { &g_gpuSyncSkip,           "Skip per-frame GPU fence (EXPERIMENTAL - may tear)", "GpuSyncSkip" },
#if ENABLE_PASS_PROBE
    { &g_logPassRts,            "Log render targets per draw pass (resolution probe)", "LogPassRts" },
#endif
#if ENABLE_SHADER_DIAG
    { &g_dumpShaders,           "Dump pixel shaders to shaders\\ (FXAA hunt)", "DumpShaders" },
#endif
#if ENABLE_SURFACE_DIAG
    { &g_msaaDebugClear,        "MSAA diagnostic: paint MS surface magenta", "MsaaDebugClear" },
#endif
    { &g_forceImmediatePresentEnabled, "Force IMMEDIATE present / no vsync (EXPERIMENTAL)", "ForceImmediatePresent" },
    { &g_unlockFramerateEnabled,  "Unlock 59.94fps frame limiter (EXPERIMENTAL)", "UnlockFramerate" },
    { &g_simDeltaFix,          "Unquantise sim delta (fixes 60fps interact + stamina)", "SimDeltaFix" },
    // Cutout AA: three attempts, all dead ends, all retired together.
    //   A2cEnable     - vendor alpha-to-coverage ignored by modern drivers
    //   SsaaFoliage   - works, but 8x cost for a negligible gain
    //   FringeFoliage - cannot ramp where alpha is sub-pixel, i.e. at distance
    // Full reasoning in FEATURES.md; the bytecode transforms behind them are
    // the reusable part if this is ever revisited.
#if ENABLE_CUTOUT_AA
    { &g_a2cEnable,            "Alpha-to-coverage on cutouts (needs MSAA on)", "A2cEnable" },
    { &g_ssaaFoliage,          "Supersample foliage cutouts (needs MSAA on)", "SsaaFoliage" },
    { &g_fringeFoliage,        "Blended foliage fringe (softens cutout edges)", "FringeFoliage" },
#endif
    // Ships: substitutes a centre-tap passthrough for ps_A082B248, the game's
    // FXAA. Independent of the shader-diag tooling that found it.
    { &g_fxaaOff,              "Disable built-in FXAA", "FxaaOff" },
#if ENABLE_GYSAHL_DIAG
    { &g_logFaSchedule,        "Log FA object schedule changes (Gysahl plot bug)", "LogFaSchedule" },
    { &g_faCmpFix,             "Repair empty plot-name string after planting (Gysahl FIX)", "FaCmpFix" },
    { &g_watchPlantBuf,        "Hardware watchpoint on the plot-name buffer (diagnostic)", "WatchPlantBuf" },
#endif
};
#define NUM_TOGGLES (sizeof(g_toggles) / sizeof(g_toggles[0]))

// Numeric settings. Separate from g_toggles because that table coerces every
// value to 0/1; these need their real magnitude preserved.
typedef struct {
    volatile LONG *val;
    const char *key;
    LONG lo, hi;
} NumericSetting;

// See the AdvancedMenu table entry below. Lives here (not in g_toggles) so
// the GUI panel never grows a checkbox for it.
static volatile LONG g_advancedMenu = 0;

static NumericSetting g_numerics[] = {
    // Upper bound raised to 1,000,000 so the default (1s = watchdog off in
    // practice) is actually representable; it used to clamp at 200ms.
    { &g_stutterThresholdUsec, "ThresholdUs", 4000, 1000000 },
    // 0 = unlocked, otherwise fps*100 (6000 = 60.00fps). Only takes effect
    // while UnlockFramerate is also on - see ApplyFramerateUnlock.
    { &g_targetFpsX100, "TargetFpsX100", 0, 24000 },
#if ENABLE_DEFER_UPLOADS
    // NOTE: the GUI's defer edit box reaches this entry as g_numerics[2], so it
    // must stay third while it exists, and its handler is gated in step with it.
    { &g_deferPerFrame, "DeferPerFrame", 1, 512 },
#endif
#if ENABLE_SHADOW_SCALE
    // Shadow map resolution multiplier. 1 = untouched (default). Only takes
    // effect at texture-creation time, i.e. on load - there is deliberately
    // no GUI control because hot-toggling it could not do anything, so the
    // config file IS the interface for this one.
    { &g_shadowScale, "ShadowScale", 1, 4 },
#endif
    // 0 = leave alone. Otherwise the shadow map edge length the ENGINE will
    // use (1024 = its "Standard", 2048 = its "Advanced"). This is the shadow
    // feature that WORKS - it writes the engine's own resolution field so
    // texture size, light projection and PCF taps all move together.
    { &g_shadowMapRes, "ShadowMapRes", 0, 8192 },
    // Percentages applied to the engine's own cascade split distances, at the
    // SOURCE fields (see ApplyCascadeSplitSource). 0 = leave alone,
    // 100 = unchanged, 300 = push the boundary 3x further.
    // Observed stock values: near 10.0, far 79.2.
    { &g_shadowSplitNearPct, "ShadowSplitNearPct", 0, 2000 },
    { &g_shadowSplitFarPct,  "ShadowSplitFarPct",  0, 2000 },
#if ENABLE_CASCADE_HUNT
    // Near-cascade extent multiplier, percent. 0/100 = untouched,
    // 200 = twice the ground covered by the sharp cascade.
    { &g_nearCascadePct, "NearCascadePct", 0, 800 },
#endif
#if ENABLE_TALK_TIMER
    // Percentage applied to FieldTalkManager's per-frame talk-entry countdown
    // step (stock 0.05/frame, unscaled by delta time). 0 = untouched,
    // 100 = unchanged, 50 = the 30fps rate while running at 60. See
    // ApplyTalkTimerScale.
    { &g_talkTimerPct, "TalkTimerPct", 0, 400 },
#endif
    // -1 = auto: install the generic device-vtable timing thunks only when
    // d3d9.dll is Microsoft's, where they are provably inert (see the
    // g_d3d9IsThirdParty comment). 1 forces them on under a wrapper too,
    // which is the A/B that tests the DXVK-crash hypothesis directly.
    // Cap, in microseconds, on the frame limiter's monotonic Sleep-granularity
    // high-water mark. 0 = observe only (default). ~2000 is the value to try:
    // the timer probe measured real resolution at 1.0000ms, so 2ms is a
    // realistic ceiling for what Sleep(1) actually costs. Too low and the
    // limiter sleeps where it should spin and overshoots the deadline.
#if ENABLE_SPIN_GUARD
    { &g_spinGuardUs, "SpinGuardUs", 0, 20000 },
#endif
    // Screen-space shadow buffer scale, percent of the engine's own half-res
    // size. 0/100 = untouched, 200 = full presentation res, 50 = quarter res.
    // Applied at texture creation, so it takes effect on area change or
    // restart - there is no GUI control because hot-toggling could not do
    // anything, exactly like ShadowMapRes. Use 50 first: a change that makes
    // shadows obviously WORSE proves the buffer matters and that this
    // interception reaches it, which is what makes 200 believable.
    // Screen-space shadow buffer resolution, PERCENT OF SCREEN. 0 = off,
    // 50 = the engine's own default, 100 = full screen, 200 = 2x supersampled.
    // Key renamed from ShadowBufPct, which was a percentage of the engine's
    // half-res value - the same numbers meant half as much and read as though
    // 100 were "unchanged" when it was actually a 2x. A stale ShadowBufPct in
    // an old config is simply ignored rather than silently reinterpreted.
    { &g_shadowBufResPct, "ShadowBufResPct", 0, 200 },
    // MSAA sample count on the scene colour pass. 0 = off, 2/4/8 = sample
    // count (this device reports all three supported for A8R8G8B8 + D24S8).
    // EXPERIMENTAL: this substitutes a render target under the engine and
    // resolves it back, the most invasive intervention in this file. It also
    // does nothing for shader/specular shimmer - MSAA is geometry edges only.
    { &g_msaaSamples, "MsaaSamples", 0, 8 },
    // Alpha-to-coverage on top of MSAA: the alpha value becomes a per-SAMPLE
    // coverage mask, so alpha-tested planes (vegetation, fences, barriers -
    // material the game uses everywhere) get real gradient edges out of the
    // resolve instead of the alpha test's binary cutout. Vendor backdoors,
    // auto-detected: AMD 'A2M1'/'A2M0' on POINTSIZE, NVIDIA 'ATOC' on
    // ADAPTIVETESS_Y. 1 = on (mirrors the engine's own ALPHATESTENABLE while
    // the MS pair is bound), 0 = off. Does nothing when MsaaSamples is 0.
    // RETIRED with the cutout-AA work: this was the render-state-only A2C
    // attempt, superseded by the shader rewrite and dead for the same reason
    // (the vendor hack is ignored).
#if ENABLE_CUTOUT_AA
    { &g_alphaToCoverage, "AlphaToCoverage", 0, 1 },
#endif
    // Which post-filter candidate shader to kill: 0 = none, 1..N = index into
    // g_psKillCandidates (top tap-count shaders from the dump scan). The GUI
    // dropdown drives this; substitution happens at BIND time so it is fully
    // hot. With MsaaDebugClear on, the substitute is solid magenta instead of
    // a passthrough - the footprint instrument that finds the real AA pass.
#if ENABLE_SHADER_DIAG
    { &g_fxaaPick, "FxaaPick", 0, 12 },
#endif
    // Graphics_Scaling (see ApplyScalingMode): 0 = don't touch, 1 = None,
    // 2 = Standard, 3 = Advanced. The engine's own image-scaling mode, and
    // the last remaining candidate for the always-on anti-aliasing.
#if ENABLE_SCALING_MODE
    { &g_scalingMode, "ScalingMode", 0, 3 },
#endif
    // Alpha-to-coverage ramp steepness. The coverage value is
    // saturate((alpha - threshold) * sharpness + 0.5), so lower = softer,
    // wider edges; higher = closer to the original hard cutout. 8 is a
    // reasonable starting point; 1 will look obviously blurry, which is
    // useful for confirming the rewrite is actually live.
#if ENABLE_CUTOUT_AA
    { &g_a2cSharpness, "A2cSharpness", 1, 64 },
    { &g_a2cDebugVis, "A2cDebugVis", 0, 1 },
    { &g_a2cMaskTest, "A2cMaskTest", 0, 1 },
#endif
    // Identify walk: paints the selected shader magenta so it can be named on
    // screen. This is the tool that found the FXAA pass and the glyph shader
    // after four offline analyses failed - kept one #define away.
#if ENABLE_SHADER_DIAG
    { &g_psIdentify, "ShaderIdentify", 0, 16000 },
#endif
    // Supersampling, percent per axis. 100 = off. Cost is the SQUARE: 150 is
    // 2.25x the pixels, 200 is 4x. See ApplySsaaScale.
    { &g_ssaaScale, "SsaaScale", 100, 200 },
#if ENABLE_SSAA_RESWRITE
    // Only meaningful while the retired resolution-write mode exists.
    { &g_ssaaMode, "SsaaMode", 0, 1 },
#endif
    // 1 = honour the chosen resolution in borderless fullscreen: downsample to
    // it, then upscale to the display, so picking 1080p really does give a
    // (supersampled) 1080p image. 0 = present the supersampled render directly
    // at display size. See HookedStretchRect.
    { &g_ssaaOutputRes, "SsaaOutputRes", 0, 1 },
    // Frametime overlay corner: 0 TL, 1 TR, 2 BL, 3 BR. Default bottom-right.
    // 0 is still accepted here but is deliberately not offered in the menu -
    // it is where the game's menu bar starts, so it always collides.
    { &g_overlayPos, "OverlayPos", 0, 3 },
    // Log file control. LogAppend=1 keeps one file across runs (developer
    // behaviour); 0 recreates it each launch (shipping). LogMaxMB bounds a
    // single run, 0 = unlimited. Both ini-only - they are not user options.
    { &g_logAppend, "LogAppend", 0, 1 },
    { &g_logMaxMB, "LogMaxMB", 0, 4096 },
    { &g_logFlushAlways, "LogFlush", 0, 1 },
    { &g_probeDeviceThunks, "ProbeDeviceThunks", -1, 1 },
    // Shows the "Optimization" popup (the raw stutter-fix toggles) in the
    // game's menu bar. Deliberately ini-only - no UI control anywhere sets
    // it: normal users get the fixes silently, and there is no visible
    // switch inviting them to turn fixes off. Testers set AdvancedMenu=1
    // by hand.
    { &g_advancedMenu, "AdvancedMenu", 0, 1 },
};
#define NUM_NUMERICS (sizeof(g_numerics) / sizeof(g_numerics[0]))

// Config load and save are driven by the SAME g_toggles table as the hotkeys
// and the GUI, so a new toggle really is one line. This is not cosmetic
// tidying: DiscardMip0 shipped once with its hotkey, its checkbox and its
// load line all wired but its SaveConfig line missing, so every toggle was
// discarded at exit and it read back 0 on the next boot. Hand-maintained
// parallel lists of the same flags in three places will keep producing that
// bug; one list cannot.
static void LoadConfig(void)
{
    char path[MAX_PATH];
    GetConfigPath(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int matched = 0;
        for (size_t i = 0; i < NUM_TOGGLES; i++) {
            char pat[64];
            int val;
            sprintf(pat, "%s=%%d", g_toggles[i].key);
            if (sscanf(line, pat, &val) == 1) { *g_toggles[i].flag = val ? 1 : 0; matched = 1; break; }
        }
        if (!matched) {
            for (size_t i = 0; i < NUM_NUMERICS; i++) {
                char pat[64];
                int val;
                sprintf(pat, "%s=%%d", g_numerics[i].key);
                if (sscanf(line, pat, &val) == 1) {
                    if (val < g_numerics[i].lo) val = g_numerics[i].lo;
                    if (val > g_numerics[i].hi) val = g_numerics[i].hi;
                    *g_numerics[i].val = val;
                    break;
                }
            }
        }
        // NoOverwriteFix is deliberately absent from the table, so a stale
        // config still carrying that key is silently ignored rather than
        // reviving a confirmed-crashing toggle.
    }
    fclose(f);
    // LogFrameTimes is a momentary "capture from here" button, not a setting,
    // but it rides the same table as the real toggles so it was being saved
    // and restored like one. Restored as 1, the monitor thread saw a rising
    // edge on its first tick and armed at STARTUP - filling all 2048 frames
    // during the title and loading screens (~1000us each, shadow pass not
    // even running) before the player reached gameplay. The capture looked
    // valid and was completely worthless. Always start disarmed.
    g_logFrameTimes = 0;
    // 1024 and BOUNDS-CHECKED. This was char[300] and simply ran off the end
    // as options accumulated: the line is now ~700 chars, so every launch
    // smashed the stack here and the process died on return from this
    // function. The config line still reached the log first, which made the
    // crash look like it happened in whatever ran next - it cost several
    // wrong diagnoses before markers showed no code after this point ran at
    // all. The guard means adding options can never reintroduce it.
    char logLine[1024];
    int n = sprintf(logLine, "[config] loaded:");
    for (size_t i = 0; i < NUM_TOGGLES && n < (int)sizeof(logLine) - 64; i++)
        n += sprintf(logLine + n, " %s=%ld", g_toggles[i].key, *g_toggles[i].flag);
    for (size_t i = 0; i < NUM_NUMERICS && n < (int)sizeof(logLine) - 64; i++)
        n += sprintf(logLine + n, " %s=%ld", g_numerics[i].key, *g_numerics[i].val);
    LogLine(logLine);
}

static void SaveConfig(void)
{
    char path[MAX_PATH];
    GetConfigPath(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    // Header. LoadConfig matches keys by name and ignores anything that does
    // not match, so comment lines are safe - and they are worth the three
    // lines: several settings here have no menu entry at all, and an ini is
    // the only place a user can discover them.
    fprintf(f, "# Lightning Returns FFXIII - mod configuration.\n");
    fprintf(f, "# Every setting the mod knows, with its current value.\n");
    fprintf(f, "# Most are set from the in-game menu (Graphics / Other); the rest\n");
    fprintf(f, "# are safe to edit here. Delete this file to restore defaults.\n\n");
    for (size_t i = 0; i < NUM_TOGGLES; i++)
        fprintf(f, "%s=%ld\n", g_toggles[i].key, *g_toggles[i].flag);
    // Numeric settings persist too. This was missing on the first attempt:
    // LoadConfig parsed ThresholdUs correctly and the GUI applied it live, but
    // nothing ever wrote it, so it silently reverted to the default every
    // launch. Exactly the same failure mode as the DiscardMip0 save line -
    // read path present, write path absent, no error anywhere.
    for (size_t i = 0; i < NUM_NUMERICS; i++)
        fprintf(f, "%s=%ld\n", g_numerics[i].key, *g_numerics[i].val);
    fclose(f);
}

// ---- Game-menu integration (the game's own Win32 menu bar) ----------------
// The windowed-mode menu bar (File / Graphics / Control / [Debug]) is built
// by the game through a tiny builder API, fully mapped in
// ghidra_output/menu_system.txt + menu_system2.txt:
//
//   FUN_00abc350(name)          open a popup: InsertMenuItemW at mgr[+0x44]
//                               pos mgr[+0x48], label from a name->wstring
//                               lookup (FUN_00ac9620)
//   FUN_00abc450()              CreateMenu() + attach as hSubMenu, descend
//   FUN_00abc3c0(name,handler)  add leaf: wID = ++mgr[+0x4c], the HANDLER
//                               POINTER goes in dwItemData, and handler(0)
//                               is called for the MFS_CHECKED state
//   FUN_00abc4b0()              pop back to the parent popup
//
// All __thiscall on a manager singleton whose pointer sits at [0x05115554]
// (FUN_00acaf60 prologue: MOV ESI,[0x05115554]). Manager layout: +0x08 root
// HMENU, +0x44 current HMENU, +0x48 current insert position, +0x4c running
// item-ID counter.
//
// Dispatch (FUN_00abcae0): a queued WM_COMMAND id is resolved with
// GetMenuItemInfoA(root, id, BYCOMMAND, {MIIM_DATA}) and the stored pointer
// is called as handler(1); the whole menu is then REBUILT via FUN_00abc310
// (clear + FUN_00acaf60), which re-queries every handler(0) - checkmarks
// maintain themselves. So the handler contract is:
//     char __cdecl handler(char apply)   // apply=0 query, apply!=0 set
// (confirmed against FUN_00aca830 / FUN_00acaea0 / FUN_00acac90, which are
// exactly that shape).
//
// FUN_00acaf60 (the vanilla tree construction) has a SINGLE call site, the
// E8 rel32 at 0x00abc32e inside the rebuild. This hook redirects that one
// call: run the vanilla build, then append the mod's entries with the game's
// own builder functions, so our items ride the native ID allocation,
// dispatch and checkmark machinery with no wndproc work at all.
//
// Labels: FUN_00ac9620 matches names by STRING-POINTER IDENTITY against a
// table built from the exe's own .rdata strings, and returns L"*" (verified:
// bytes 2a 00 00 00 at 02198a60) for anything unknown - so the insert
// succeeds for our names and the real label is applied right afterwards
// with SetMenuItemInfoW on mgr[+0x44]/mgr[+0x48].
#define ENABLE_GAME_MENU 1
#if ENABLE_GAME_MENU

// Ghidra VA - 0x400000 = RVA (exe is ASLR'd; everything below is base+RVA).
#define MENU_RVA_MGR_PTR    0x04D15554  // DAT_05115554: manager singleton ptr
#define MENU_RVA_OPEN       0x006BC350  // FUN_00abc350 open popup(name)
#define MENU_RVA_BEGINSUB   0x006BC450  // FUN_00abc450 descend into popup
#define MENU_RVA_ADDITEM    0x006BC3C0  // FUN_00abc3c0 add item(name,handler)
#define MENU_RVA_CLOSE      0x006BC4B0  // FUN_00abc4b0 pop to parent
#define MENU_RVA_BUILD      0x006CAF60  // FUN_00acaf60 vanilla tree build
#define MENU_RVA_BUILDCALL  0x006BC32E  // its one call site (E8 rel32)
// Vanilla value handlers, used two ways: as dwItemData needles to FIND the
// popups they live in (position- and localisation-independent), and called
// directly from our replacement items so "Standard"/"Advanced"/"Variable"/
// "Stability" keep their exact vanilla behaviour.
#define MENU_RVA_PRES_FS    0x006CA830  // Graphics_Presentation_FullScreen
#define MENU_RVA_SCALE_ADV  0x006CAEA0  // Graphics_Scaling_Advanced
#define MENU_RVA_SHADOW_ADV 0x006CAC90  // Graphics_Shadowing_Advanced (2048)
#define MENU_RVA_SHADOW_STD 0x006CACC0  // Graphics_Shadowing_Standard (1024)
#define MENU_RVA_FRATE_VAR  0x006CADB0  // Graphics_FrameRate_Variable
#define MENU_RVA_FRATE_STAB 0x006CADE0  // Graphics_FrameRate_Stability

#define MENUMGR_ROOT_HMENU  0x08
#define MENUMGR_CUR_HMENU   0x44
#define MENUMGR_CUR_POS     0x48

// Manually-inserted items need IDs that can never collide with the builder's
// sequential counter (a ushort that ends up around ~60 for the whole bar).
#define GAMEMENU_ID_BASE    0xF000

// __thiscall shims: __fastcall with a dummy EDX gives ECX=this + callee-
// cleaned stack args, which is exactly MSVC __thiscall.
typedef void (__fastcall *MenuOpenFn)(void *self, void *edx, const char *name);
typedef void (__fastcall *MenuVoidFn)(void *self, void *edx);
typedef void (__fastcall *MenuAddFn)(void *self, void *edx, const char *name, void *handler);
typedef char (__cdecl *GameMenuHandler)(char apply);

static volatile LONG g_menuBuilds = 0;      // rebuild counter (log first only)
static int g_menuHookInstalled = 0;

// ---- handlers -------------------------------------------------------------
// One tiny function per item because the game's dispatch carries no context:
// the function POINTER (dwItemData) is the identity.

// Boolean toggle: click flips the flag, checkmark mirrors it.
#define GAMEMENU_TOGGLE(fn, flag) \
    static char __cdecl fn(char apply) { \
        if (apply) { InterlockedExchange(&(flag), (flag) ? 0 : 1); SaveConfig(); } \
        return (char)((flag) != 0); }

// Radio preset: click writes one value into a numeric, checkmark = equality.
#define GAMEMENU_VALUE(fn, var, value) \
    static char __cdecl fn(char apply) { \
        if (apply) { InterlockedExchange(&(var), (value)); SaveConfig(); } \
        return (char)((var) == (value)); }

// Optimization popup - the mod's own fixes, flat toggles.
GAMEMENU_TOGGLE(MenuH_Discard,     g_discardFixEnabled)
GAMEMENU_TOGGLE(MenuH_ShaderThr,   g_shaderThrottleEnabled)
GAMEMENU_TOGGLE(MenuH_ReadPace,    g_readPaceEnabled)
GAMEMENU_TOGGLE(MenuH_LoaderThr,   g_loaderThrottleEnabled)
GAMEMENU_TOGGLE(MenuH_StageTex,    g_stagingUploadEnabled)
GAMEMENU_TOGGLE(MenuH_StageSurf,   g_stagingSurfaceEnabled)
GAMEMENU_TOGGLE(MenuH_StageCube,   g_stagingCubeEnabled)
GAMEMENU_TOGGLE(MenuH_GpuSync,     g_gpuSyncSkip)
GAMEMENU_TOGGLE(MenuH_SimDelta,    g_simDeltaFix)
GAMEMENU_TOGGLE(MenuH_StdD3D9,     g_forceStdD3D9)
GAMEMENU_TOGGLE(MenuH_Overlay,     g_overlayEnabled)

// Stutter watchdog threshold. 1s = never fires in practice = "Off", which is
// also the shipping default; the short values re-arm the logging.
GAMEMENU_VALUE(MenuH_WdOff, g_stutterThresholdUsec, 1000000)
GAMEMENU_VALUE(MenuH_Wd4,   g_stutterThresholdUsec, 4000)
GAMEMENU_VALUE(MenuH_Wd8,   g_stutterThresholdUsec, 8000)
GAMEMENU_VALUE(MenuH_Wd16,  g_stutterThresholdUsec, 16000)
GAMEMENU_VALUE(MenuH_Wd33,  g_stutterThresholdUsec, 33000)

// Graphics popup - new groups.
GAMEMENU_VALUE(MenuH_Msaa0, g_msaaSamples, 0)
GAMEMENU_VALUE(MenuH_Msaa2, g_msaaSamples, 2)
GAMEMENU_VALUE(MenuH_Msaa4, g_msaaSamples, 4)
GAMEMENU_VALUE(MenuH_Msaa8, g_msaaSamples, 8)
// Supersampling. Percent per AXIS, so the cost multiplier is the SQUARE of
// the label: 1.25x = 1.6x, 1.5x = 2.25x, 2x = 4x the pixels and the shading.
// The labels used to carry those cost figures; they now show the ratio only,
// because that detail belongs in the mod's description where there is room to
// explain it rather than in a menu entry (user's call 2026-08-13).
GAMEMENU_VALUE(MenuH_Ssaa100, g_ssaaScale, 100)
GAMEMENU_VALUE(MenuH_Ssaa125, g_ssaaScale, 125)
GAMEMENU_VALUE(MenuH_Ssaa150, g_ssaaScale, 150)
GAMEMENU_VALUE(MenuH_Ssaa200, g_ssaaScale, 200)
// Overlay corner. The overlay is a topmost window, so at the top of the screen
// it sits over the game's own menu bar - which is what motivated this.
GAMEMENU_VALUE(MenuH_OvlTL, g_overlayPos, 0)
GAMEMENU_VALUE(MenuH_OvlTR, g_overlayPos, 1)
GAMEMENU_VALUE(MenuH_OvlBL, g_overlayPos, 2)
GAMEMENU_VALUE(MenuH_OvlBR, g_overlayPos, 3)
GAMEMENU_VALUE(MenuH_FxaaOn,  g_fxaaOff, 0)   // vanilla FXAA active
GAMEMENU_VALUE(MenuH_FxaaOff, g_fxaaOff, 1)   // ps_A082B248 -> passthrough
GAMEMENU_VALUE(MenuH_VsyncOn,  g_forceImmediatePresentEnabled, 0)
GAMEMENU_VALUE(MenuH_VsyncOff, g_forceImmediatePresentEnabled, 1)
// Screen-space shadow buffer, percent of screen (engine default is 50).
GAMEMENU_VALUE(MenuH_SsShadStd,  g_shadowBufResPct, 0)
GAMEMENU_VALUE(MenuH_SsShadFull, g_shadowBufResPct, 100)
GAMEMENU_VALUE(MenuH_SsShad2x,   g_shadowBufResPct, 200)

// Shadow draw distance: drives the NEAR cascade split ONLY. The far split
// stays at engine default always (user's call 2026-08-11: far is too
// finnicky). Applying any preset also zeroes the far percentage so a value
// hand-set in an older ini cannot silently stay live under the menu.
static void GameMenuSetSplit(LONG pct)
{
    InterlockedExchange(&g_shadowSplitNearPct, pct);
    InterlockedExchange(&g_shadowSplitFarPct, 0);
    SaveConfig();
}
static char __cdecl MenuH_DistStd(char apply)
{
    if (apply) GameMenuSetSplit(0);
    return (char)(g_shadowSplitNearPct == 0);
}
static char __cdecl MenuH_Dist150(char apply)
{
    if (apply) GameMenuSetSplit(150);
    return (char)(g_shadowSplitNearPct == 150);
}
static char __cdecl MenuH_Dist200(char apply)
{
    if (apply) GameMenuSetSplit(200);
    return (char)(g_shadowSplitNearPct == 200);
}
static char __cdecl MenuH_Dist300(char apply)
{
    if (apply) GameMenuSetSplit(300);
    return (char)(g_shadowSplitNearPct == 300);
}

// Shadowing popup REPLACEMENT. Standard/Advanced wrap the vanilla handlers
// (same engine-field write, same registry persistence) and additionally
// clear the mod's resolution force so the engine value actually sticks;
// High/Ultra use the force. Exactly one of the four can be checked.
static char __cdecl MenuH_Shadow1024(char apply)
{
    GameMenuHandler vanilla = (GameMenuHandler)(g_mainModBase + MENU_RVA_SHADOW_STD);
    if (apply) { vanilla(1); InterlockedExchange(&g_shadowMapRes, 0); SaveConfig(); }
    return (char)(vanilla(0) && g_shadowMapRes == 0);
}
static char __cdecl MenuH_Shadow2048(char apply)
{
    GameMenuHandler vanilla = (GameMenuHandler)(g_mainModBase + MENU_RVA_SHADOW_ADV);
    if (apply) { vanilla(1); InterlockedExchange(&g_shadowMapRes, 0); SaveConfig(); }
    return (char)(vanilla(0) && g_shadowMapRes == 0);
}
GAMEMENU_VALUE(MenuH_Shadow4096, g_shadowMapRes, 4096)
GAMEMENU_VALUE(MenuH_Shadow8192, g_shadowMapRes, 8192)

// FrameRate popup REPLACEMENT. The mod limiter ALWAYS supersedes the
// engine's (user's call 2026-08-11): the vanilla Variable/Stability entries
// are deleted outright and only the mod presets remain, every one of which
// engages the unlock. TargetFpsX100=0 means no cap at all.
#if 0 // Retired with that decision: wrappers that kept vanilla
      // Variable/Stability clickable while disengaging the unlock. Kept in
      // case the vanilla modes ever need to be reachable again.
static char __cdecl MenuH_FrVariable(char apply)
{
    GameMenuHandler vanilla = (GameMenuHandler)(g_mainModBase + MENU_RVA_FRATE_VAR);
    if (apply) { vanilla(1); InterlockedExchange(&g_unlockFramerateEnabled, 0); SaveConfig(); }
    return (char)(vanilla(0) && !g_unlockFramerateEnabled);
}
static char __cdecl MenuH_FrStability(char apply)
{
    GameMenuHandler vanilla = (GameMenuHandler)(g_mainModBase + MENU_RVA_FRATE_STAB);
    if (apply) { vanilla(1); InterlockedExchange(&g_unlockFramerateEnabled, 0); SaveConfig(); }
    return (char)(vanilla(0) && !g_unlockFramerateEnabled);
}
#endif
static void GameMenuSetFps(LONG fpsX100)
{
    InterlockedExchange(&g_unlockFramerateEnabled, 1);
    InterlockedExchange(&g_targetFpsX100, fpsX100);
    SaveConfig();
}
static char __cdecl MenuH_Fr30(char apply)
{
    if (apply) GameMenuSetFps(3000);
    return (char)(g_unlockFramerateEnabled && g_targetFpsX100 == 3000);
}
static char __cdecl MenuH_Fr60(char apply)
{
    if (apply) GameMenuSetFps(6000);
    return (char)(g_unlockFramerateEnabled && g_targetFpsX100 == 6000);
}
static char __cdecl MenuH_FrUnlimited(char apply)
{
    if (apply) GameMenuSetFps(0);
    return (char)(g_unlockFramerateEnabled && g_targetFpsX100 == 0);
}

// ---- tree construction helpers --------------------------------------------

// Overwrite the label of the item the builder just inserted (it inserted the
// lookup fallback L"*" for our unknown names). mgr[+0x44]/mgr[+0x48] still
// address exactly that item until the next builder call.
static void GameMenuFixLabel(void *mgr, const wchar_t *label)
{
    MENUITEMINFOW mii;
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = (LPWSTR)label;
    SetMenuItemInfoW(*(HMENU *)((char *)mgr + MENUMGR_CUR_HMENU),
                     *(UINT *)((char *)mgr + MENUMGR_CUR_POS), TRUE, &mii);
}

// Manual leaf insert for surgery on the vanilla Graphics popup - same item
// shape FUN_00abc3c0 produces (state from handler(0), handler in dwItemData)
// but with an ID from the reserved high range.
static void GameMenuInsertLeaf(HMENU menu, int pos, UINT id,
                               const wchar_t *label, GameMenuHandler h)
{
    MENUITEMINFOW mii;
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STATE | MIIM_ID | MIIM_DATA | MIIM_STRING;
    mii.fState = h(0) ? MFS_CHECKED : MFS_UNCHECKED;
    mii.wID = id;
    mii.dwItemData = (ULONG_PTR)h;
    mii.dwTypeData = (LPWSTR)label;
    InsertMenuItemW(menu, (UINT)pos, TRUE, &mii);
}

// Insert a popup header + fresh submenu at a position. The game's clear pass
// (FUN_00abc2e0 DeleteMenu loop) destroys attached submenus recursively, so
// these CreateMenu handles are reclaimed on every rebuild - no leak.
static HMENU GameMenuInsertGroup(HMENU parent, int pos, const wchar_t *label)
{
    MENUITEMINFOW mii;
    HMENU sub = CreateMenu();
    if (!sub) return NULL;
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_SUBMENU | MIIM_STRING;
    mii.hSubMenu = sub;
    mii.dwTypeData = (LPWSTR)label;
    if (!InsertMenuItemW(parent, (UINT)pos, TRUE, &mii)) {
        DestroyMenu(sub);
        return NULL;
    }
    return sub;
}

// Find the menu containing a leaf whose dwItemData == data (vanilla handler
// pointers make position- and language-independent needles).
static int GameMenuFindByData(HMENU menu, ULONG_PTR data, HMENU *outMenu, int *outPos)
{
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_DATA | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, (UINT)i, TRUE, &mii)) continue;
        if (mii.dwItemData == data && data != 0) {
            *outMenu = menu; *outPos = i; return 1;
        }
        if (mii.hSubMenu &&
            GameMenuFindByData(mii.hSubMenu, data, outMenu, outPos)) return 1;
    }
    return 0;
}

// Find the item whose hSubMenu == target (i.e. locate a popup's header row
// in its parent - used to find where the vanilla groups sit inside the
// Graphics popup so ours can slot in next to their relatives).
static int GameMenuFindPopupItem(HMENU menu, HMENU target, HMENU *outParent, int *outPos)
{
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, (UINT)i, TRUE, &mii)) continue;
        if (mii.hSubMenu == target) { *outParent = menu; *outPos = i; return 1; }
        if (mii.hSubMenu &&
            GameMenuFindPopupItem(mii.hSubMenu, target, outParent, outPos)) return 1;
    }
    return 0;
}

// ---- the appended tree ----------------------------------------------------

static void GameMenuAppend(void)
{
    unsigned char *base = (unsigned char *)g_mainModBase;
    void *mgr;
    HMENU root;
    UINT id = GAMEMENU_ID_BASE;
    int repShadow = 0, repFrate = 0, groups = 0;
    MenuOpenFn mOpen;
    MenuVoidFn mBegin, mClose;
    MenuAddFn mAdd;

    if (!base) return;
    mgr = *(void **)(base + MENU_RVA_MGR_PTR);
    if (!mgr) return;
    root = *(HMENU *)((char *)mgr + MENUMGR_ROOT_HMENU);
    mOpen  = (MenuOpenFn)(base + MENU_RVA_OPEN);
    mBegin = (MenuVoidFn)(base + MENU_RVA_BEGINSUB);
    mAdd   = (MenuAddFn)(base + MENU_RVA_ADDITEM);
    mClose = (MenuVoidFn)(base + MENU_RVA_CLOSE);

    // -- 1. top-level "Optimization" popup: the raw stutter-fix toggles.
    //       ADVANCED ONLY - hidden unless AdvancedMenu=1 was hand-set in the
    //       ini (user's call 2026-08-11: the fixes just run; normal users
    //       should not see a menu inviting them to switch fixes off).
    //       After the vanilla build the manager is back at bar level, so
    //       these append after Control / Debug exactly like a vanilla group.
    if (g_advancedMenu) {
        mOpen(mgr, NULL, "Mod_Optimization"); GameMenuFixLabel(mgr, L"Optimization");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_Discard",   (void *)MenuH_Discard);   GameMenuFixLabel(mgr, L"DISCARD Lock Fix");
        mAdd(mgr, NULL, "Mod_ShaderThr", (void *)MenuH_ShaderThr); GameMenuFixLabel(mgr, L"Shader Queue Throttle");
        mAdd(mgr, NULL, "Mod_ReadPace",  (void *)MenuH_ReadPace);  GameMenuFixLabel(mgr, L"Read Rate Limiter");
        mAdd(mgr, NULL, "Mod_LoaderThr", (void *)MenuH_LoaderThr); GameMenuFixLabel(mgr, L"Loader Dispatch Throttle");
        mOpen(mgr, NULL, "Mod_Staging"); GameMenuFixLabel(mgr, L"Staging Uploads");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_StageTex",  (void *)MenuH_StageTex);  GameMenuFixLabel(mgr, L"Textures");
        mAdd(mgr, NULL, "Mod_StageSurf", (void *)MenuH_StageSurf); GameMenuFixLabel(mgr, L"Surfaces");
        mAdd(mgr, NULL, "Mod_StageCube", (void *)MenuH_StageCube); GameMenuFixLabel(mgr, L"Cube Textures");
        mClose(mgr, NULL);
        mAdd(mgr, NULL, "Mod_GpuSync",   (void *)MenuH_GpuSync);   GameMenuFixLabel(mgr, L"Skip GPU Fence");
        mAdd(mgr, NULL, "Mod_SimDelta",  (void *)MenuH_SimDelta);  GameMenuFixLabel(mgr, L"Sim Delta Fix");
        mAdd(mgr, NULL, "Mod_StdD3D9",   (void *)MenuH_StdD3D9);   GameMenuFixLabel(mgr, L"Force Plain D3D9 (restart)");
        mOpen(mgr, NULL, "Mod_Watchdog"); GameMenuFixLabel(mgr, L"Stutter Watchdog");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_WdOff", (void *)MenuH_WdOff); GameMenuFixLabel(mgr, L"Off");
        mAdd(mgr, NULL, "Mod_Wd4",   (void *)MenuH_Wd4);   GameMenuFixLabel(mgr, L"4 ms");
        mAdd(mgr, NULL, "Mod_Wd8",   (void *)MenuH_Wd8);   GameMenuFixLabel(mgr, L"8 ms");
        mAdd(mgr, NULL, "Mod_Wd16",  (void *)MenuH_Wd16);  GameMenuFixLabel(mgr, L"16 ms");
        mAdd(mgr, NULL, "Mod_Wd33",  (void *)MenuH_Wd33);  GameMenuFixLabel(mgr, L"33 ms");
        mClose(mgr, NULL);
        mClose(mgr, NULL);
    }

    // -- 1b. top-level "Other" popup - always visible. Home of the frametime
    //        overlay, which ships as a user feature (the only honest view of
    //        the engine tick), separate from the advanced-only fix toggles.
    mOpen(mgr, NULL, "Mod_Other"); GameMenuFixLabel(mgr, L"Other");
    mBegin(mgr, NULL);
    mAdd(mgr, NULL, "Mod_Overlay", (void *)MenuH_Overlay); GameMenuFixLabel(mgr, L"Frametime Overlay");
    mOpen(mgr, NULL, "Mod_OverlayPos"); GameMenuFixLabel(mgr, L"Overlay Position");
    mBegin(mgr, NULL);
    // Top Left is not offered: it is where the game's menu bar starts, so it
    // is the one corner guaranteed to collide. MenuH_OvlTL and OverlayPos=0
    // still work from the ini if it is ever wanted back.
    mAdd(mgr, NULL, "Mod_OvlTR", (void *)MenuH_OvlTR); GameMenuFixLabel(mgr, L"Top Right");
    mAdd(mgr, NULL, "Mod_OvlBL", (void *)MenuH_OvlBL); GameMenuFixLabel(mgr, L"Bottom Left");
    mAdd(mgr, NULL, "Mod_OvlBR", (void *)MenuH_OvlBR); GameMenuFixLabel(mgr, L"Bottom Right");
    mClose(mgr, NULL);
    mClose(mgr, NULL);

    // -- 2. locate the vanilla Graphics anchors BEFORE any surgery (the
    //       replacements below delete the very items the needles point at).
    {
        HMENU mPresPop = NULL, mScalePop = NULL, mShadPop = NULL, mFratePop = NULL;
        HMENU mTmp = NULL;
        int pTmp = -1, pStd = -1, pAdv = -1, pVar = -1, pStab = -1;

        GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_PRES_FS), &mPresPop, &pTmp);
        GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_SCALE_ADV), &mScalePop, &pTmp);
        if (!GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_SHADOW_ADV), &mShadPop, &pAdv) ||
            !GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_SHADOW_STD), &mTmp, &pStd) ||
            mTmp != mShadPop) { mShadPop = NULL; }
        if (!GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_FRATE_VAR), &mFratePop, &pVar) ||
            !GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_FRATE_STAB), &mTmp, &pStab) ||
            mTmp != mFratePop) { mFratePop = NULL; }

        // -- 3. Shadowing popup: Standard/Advanced replaced by four
        //       resolutions on the same engine field.
        if (mShadPop) {
            int lo = pStd < pAdv ? pStd : pAdv;
            DeleteMenu(mShadPop, (UINT)(pStd > pAdv ? pStd : pAdv), MF_BYPOSITION);
            DeleteMenu(mShadPop, (UINT)lo, MF_BYPOSITION);
            GameMenuInsertLeaf(mShadPop, lo + 0, id++, L"Standard (1024)", MenuH_Shadow1024);
            GameMenuInsertLeaf(mShadPop, lo + 1, id++, L"Advanced (2048)", MenuH_Shadow2048);
            GameMenuInsertLeaf(mShadPop, lo + 2, id++, L"High (4096)",     MenuH_Shadow4096);
            // The oscillating stutter once blamed on 8192 was the per-frame
            // GPU fence serialising CPU and GPU (user, 2026-08-11) - fixed
            // by GpuSyncSkip, so 8192 carries no warning label.
            GameMenuInsertLeaf(mShadPop, lo + 3, id++, L"Ultra (8192)", MenuH_Shadow8192);
            repShadow = 1;
        }

        // -- 4. FrameRate popup: Variable/Stability wrapped (they now also
        //       disengage the limiter unlock), FPS presets appended.
        if (mFratePop) {
            int lo = pVar < pStab ? pVar : pStab;
            DeleteMenu(mFratePop, (UINT)(pVar > pStab ? pVar : pStab), MF_BYPOSITION);
            DeleteMenu(mFratePop, (UINT)lo, MF_BYPOSITION);
            GameMenuInsertLeaf(mFratePop, lo + 0, id++, L"30FPS", MenuH_Fr30);
            GameMenuInsertLeaf(mFratePop, lo + 1, id++, L"60FPS", MenuH_Fr60);
            // Uncapped means the sim-delta quantisation collides timestamps
            // constantly - SimDeltaFix covers the two confirmed bugs but the
            // residual risk is real, so the label says so plainly.
            GameMenuInsertLeaf(mFratePop, lo + 2, id++,
                               L"Unlimited (unstable, WILL cause bugs)", MenuH_FrUnlimited);
            repFrate = 1;
        }

        // -- 5. new groups inside the Graphics popup, slotted next to their
        //       vanilla relatives: VSync after Presentation, MSAA+FXAA after
        //       Scaling, the two shadow extras after Shadowing. Inserting at
        //       the HIGHEST anchor first keeps the earlier positions valid.
        {
            HMENU gfx = NULL, gfxCheck = NULL, sub;
            int presPos = -1, scalePos = -1, shadPos = -1;

            if (mPresPop) GameMenuFindPopupItem(root, mPresPop, &gfx, &presPos);
            if (mScalePop && GameMenuFindPopupItem(root, mScalePop, &gfxCheck, &scalePos) &&
                gfx && gfxCheck != gfx) scalePos = -1;   // different parent: skip anchor
            if (!gfx) gfx = gfxCheck;
            gfxCheck = NULL;
            if (mShadPop && GameMenuFindPopupItem(root, mShadPop, &gfxCheck, &shadPos) &&
                gfx && gfxCheck != gfx) shadPos = -1;
            if (!gfx) gfx = gfxCheck;

            // The replaced popup reads "Shadows", not the vanilla
            // "Shadowing" (user-requested rename). Only when its contents
            // really were replaced - a vanilla-content popup keeps its
            // vanilla (localised) name.
            if (repShadow && gfx && shadPos >= 0) {
                MENUITEMINFOW mii;
                memset(&mii, 0, sizeof(mii));
                mii.cbSize = sizeof(mii);
                mii.fMask = MIIM_STRING;
                mii.dwTypeData = (LPWSTR)L"Shadows";
                SetMenuItemInfoW(gfx, (UINT)shadPos, TRUE, &mii);
            }

            if (gfx) {
                int endPos = GetMenuItemCount(gfx);
                int at;

                at = (shadPos >= 0) ? shadPos + 1 : endPos;
                sub = GameMenuInsertGroup(gfx, at, L"Shadow Distance");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, L"Standard", MenuH_DistStd);
                    GameMenuInsertLeaf(sub, 1, id++, L"150%",     MenuH_Dist150);
                    GameMenuInsertLeaf(sub, 2, id++, L"200%",     MenuH_Dist200);
                    GameMenuInsertLeaf(sub, 3, id++, L"300%",     MenuH_Dist300);
                    groups++;
                }
                // Screen-Space Shadows REMOVED from the menu 2026-08-12: the
                // effect is too subtle to perceive (verified during the
                // ShadowBufPct work - 200% was indistinguishable from stock
                // because the content is band-limited by PCF plus the
                // MULTI_SAMPLE interleave), and it only applies on area change
                // or restart. A setting that does nothing visible and needs a
                // reload is a trap in a user-facing menu. ShadowBufResPct
                // survives as an ini key for anyone who wants it.

                at = (scalePos >= 0) ? scalePos + 1 : endPos;
                // Named "SSAA" rather than "Supersampling": the user maintains
                // a guide explaining each option, so the menu carries the term
                // people will search for rather than a description.
                sub = GameMenuInsertGroup(gfx, at, L"SSAA");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, L"Off",   MenuH_Ssaa100);
                    GameMenuInsertLeaf(sub, 1, id++, L"1.25x", MenuH_Ssaa125);
                    GameMenuInsertLeaf(sub, 2, id++, L"1.5x",  MenuH_Ssaa150);
                    GameMenuInsertLeaf(sub, 3, id++, L"2x",    MenuH_Ssaa200);
                    groups++;
                }
                sub = GameMenuInsertGroup(gfx, at + 1, L"MSAA");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, L"Off", MenuH_Msaa0);
                    GameMenuInsertLeaf(sub, 1, id++, L"2x",  MenuH_Msaa2);
                    GameMenuInsertLeaf(sub, 2, id++, L"4x",  MenuH_Msaa4);
                    GameMenuInsertLeaf(sub, 3, id++, L"8x",  MenuH_Msaa8);
                    groups++;
                }
                sub = GameMenuInsertGroup(gfx, at + 2, L"FXAA");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, L"On",  MenuH_FxaaOn);
                    GameMenuInsertLeaf(sub, 1, id++, L"Off", MenuH_FxaaOff);
                    groups++;
                }

                at = (presPos >= 0) ? presPos + 1 : endPos;
                sub = GameMenuInsertGroup(gfx, at, L"VSync");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, L"On",  MenuH_VsyncOn);
                    GameMenuInsertLeaf(sub, 1, id++, L"Off", MenuH_VsyncOff);
                    groups++;
                }
            }
        }
    }

    if (InterlockedIncrement(&g_menuBuilds) == 1) {
        char line[256];
        sprintf(line, "[menu] first build: %s, %d Graphics groups added, "
                "shadows %s, framerate %s",
                g_advancedMenu ? "Optimization+Other popups (AdvancedMenu=1)"
                               : "Other popup (Optimization hidden, AdvancedMenu=0)",
                groups, repShadow ? "replaced" : "NOT FOUND",
                repFrate ? "replaced" : "NOT FOUND");
        LogLine(line);
    }
}

// Redirected target of the E8 at MENU_RVA_BUILDCALL: vanilla tree first,
// then ours. cdecl void(void) is safe here - the site clobbers no state the
// caller reads back (confirmed: FUN_00abc310 only writes the dirty flag
// afterwards), and callee-saved registers are preserved by the compiler.
static void __cdecl GameMenuBuildDetour(void)
{
    ((void (*)(void))((unsigned char *)g_mainModBase + MENU_RVA_BUILD))();
    GameMenuAppend();
}

// Patch the single call site of the vanilla tree builder. Verifies the site
// still is E8 -> FUN_00acaf60 and that the builder's prologue still loads
// the manager singleton from the relocated DAT_05115554 before touching
// anything - both must hold or the RVAs are stale and nothing is patched.
static void InstallGameMenuHook(void)
{
    unsigned char *base = (unsigned char *)g_mainModBase;
    unsigned char *site, *build;
    DWORD oldProtect;
    char line[160];

    if (!base) return;
    site = base + MENU_RVA_BUILDCALL;
    build = base + MENU_RVA_BUILD;

    if (site[0] != 0xE8 ||
        (unsigned char *)(site + 5 + *(int *)(site + 1)) != build) {
        sprintf(line, "[menu] call site mismatch at %08X (byte %02X) - hook NOT installed",
                (unsigned int)site, site[0]);
        LogLine(line);
        return;
    }
    // 56 8B 35 <abs> = PUSH ESI; MOV ESI,[DAT_05115554], relocated by ASLR.
    if (build[0] != 0x56 || build[1] != 0x8B || build[2] != 0x35 ||
        *(unsigned int *)(build + 3) != (unsigned int)(base + MENU_RVA_MGR_PTR)) {
        LogLine("[menu] builder prologue mismatch - hook NOT installed");
        return;
    }

    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogLine("[menu] VirtualProtect failed - hook NOT installed");
        return;
    }
    *(int *)(site + 1) = (int)GameMenuBuildDetour - (int)(site + 5);
    VirtualProtect(site, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_menuHookInstalled = 1;
    LogLine("[menu] game-menu build hook installed (call site 0x6BC32E)");
}
#endif // ENABLE_GAME_MENU

// ---- Control panel (separate Win32 window, not a D3D9 overlay) ------------
// A D3D9-rendered overlay (ImGui or similar, drawn via the already-hooked
// Present) was considered and rejected for this: it would need its own
// input capture wired through the game's own message loop, a font/vertex
// buffer setup, and stays coupled to the render pipeline this whole
// investigation has been chasing bugs in - exactly the wrong place to add
// more surface area right now. A plain top-level Win32 window is simpler on
// every axis: it gets its own independent message pump and input handling
// for free from the OS, is completely decoupled from the game's own
// rendering (so it can't be broken by anything found in this file), and
// native checkboxes are both the visual feedback AND the control - no
// separate "did my toggle take effect" step needed. Extensible for future
// mod features the same way: add another CreateWindowA checkbox/control and
// a WM_COMMAND case.
#define IDC_CHECK_BASE 1001 // one ID per entry in g_toggles: 1001, 1002, 1003...
#define IDT_SYNC_TIMER 2001
#define IDC_THRESH_EDIT 3001
#define IDC_THRESH_APPLY 3002
#define IDC_FPS_EDIT 3003
#define IDC_FPS_APPLY 3004
#define IDC_DEFER_EDIT 3005
#define IDC_DEFER_APPLY 3006
#define IDC_SHADOW_COMBO 3007
#define IDC_SPLITNEAR_COMBO 3008
#define IDC_SPLITFAR_COMBO 3009
#define IDC_TALKTIMER_COMBO 3010
// The engine's talk-entry countdown steps 0.05 per FRAME with no delta-time
// term, so at 60fps a dialogue's slot is torn down in half the wall-clock time
// the designers tuned for. 50% restores the 30fps rate. Higher values are
// offered for A/B: if 50 fixes a bug and 200 makes it much worse, that is the
// confirmation that this timer is the one involved.
static const LONG g_talkPctChoices[] = { 0, 50, 100, 200 };
static const char *g_talkPctLabels[] = {
    "Off (leave engine alone)",
    "50%  (30fps rate - the fix)",
    "100%  (unchanged)",
    "200%  (2x faster - should worsen)"
};
#define NUM_TALK_CHOICES (sizeof(g_talkPctChoices)/sizeof(g_talkPctChoices[0]))
static const LONG g_splitPctChoices[] = { 0, 100, 150, 200, 300, 400, 600 };
static const char *g_splitPctLabels[] = {
    "Off (engine default)", "100%  (unchanged)", "150%", "200%  (2x)",
    "300%  (3x)", "400%  (4x)", "600%  (6x)"
};
#define NUM_SPLIT_CHOICES (sizeof(g_splitPctChoices)/sizeof(g_splitPctChoices[0]))
// Engine's own two settings are 1024 ("Standard") and 2048 ("Advanced"), so
// the multipliers are expressed against 2048. 8192 means an 8192x16384
// cascade atlas, which is past what a lot of D3D9 drivers will allocate -
// offered, but expected to be the one that fails.
static const LONG g_shadowResChoices[] = { 0, 1024, 2048, 4096, 8192 };
static const char *g_shadowResLabels[] = {
    "Off (leave engine alone)",
    "1024  (engine Standard)",
    "2048  (engine Advanced)",
    "4096  (x2)",
    // x4 was tested: it allocates and renders, but some camera angles push
    // the GPU into heavy oscillating stutter (8192 means an 8192x16384
    // atlas, ~512MB in R32F before the depth pair). Left selectable, marked
    // so it is not mistaken for a free upgrade. x2 is the practical ceiling.
    "8192  (x4 - allocates, but stutters)"
};
#define NUM_SHADOW_CHOICES (sizeof(g_shadowResChoices)/sizeof(g_shadowResChoices[0]))

// Screen-space shadow buffer resolution, as a percentage of the SCREEN.
// Expressed against the screen rather than against the engine's own half-res
// value, so the numbers mean what they look like: 50 IS the engine default,
// 100 is full screen, 200 is 2x supersampled. (The internal descriptor
// multiplier is twice this, since the engine's own buffers start at half.)
//
// Measured response, 4K, user-tested: 25 unusable (blocky, artifacts around
// objects), 50 shimmery, 100 modestly better, 200 clearly better. Gains do
// NOT plateau at full screen - they continue into supersampling, which means
// the limit is ALIASING in the shadow term, not softness in the filter.
// VRAM is the reason to stop: the set is three buffers at the chosen size
// plus two at half of it, all 4 bytes/px.
static const LONG g_shadowBufChoices[] = { 0, 25, 50, 100, 200 };
static const char *g_shadowBufLabels[] = {
    "Off (leave engine alone)",
    "25%  (quarter screen - unusable, artifacts)",
    "50%  (half screen - ENGINE DEFAULT)",
    "100%  (full screen, ~115MB at 4K)",
    "200%  (2x supersampled, ~460MB at 4K)"
};
#define NUM_SHADOWBUF_CHOICES (sizeof(g_shadowBufChoices)/sizeof(g_shadowBufChoices[0]))
#define IDC_SHADOWBUF_COMBO 3011

// MSAA sample count. Hot-togglable: the substitution checks this per bind, so
// a change applies within a frame; a sample-count change releases and
// recreates the MS surfaces on the render thread (never from here - GUI
// thread must not touch device objects). Only engages when the in-game
// resolution equals the desktop resolution (identity latch requirement).
static const LONG g_msaaChoices[] = { 0, 2, 4, 8 };
static const char *g_msaaLabels[] = {
    "Off",
    "2x  (~130MB at 4K)",
    "4x  (~400MB at 4K)",
    "8x  (~790MB at 4K - may exceed VRAM)"
};
#define NUM_MSAA_CHOICES (sizeof(g_msaaChoices)/sizeof(g_msaaChoices[0]))
#define IDC_MSAA_COMBO 3012
#define IDC_A2C_CHECK  3013
#define IDC_FXAA_COMBO 3014

// Post-shader kill candidates - the top tap-count fullscreen-filter shaders
// from the offline dump scan. Declared here because the GUI dropdown needs
// the labels and sits earlier in this file than the shader hooks; the hash
// table is the authority, labels must stay in step. Fingerprint-by-constants
// already misfired once (ps_072C19AF matched FXAA's 1/8+1/16 thresholds and
// proved visually inert across 11,533 confirmed substitutions - a PCF/blur
// step ladder), so the dropdown + magenta footprint mode exists to identify
// the real AA pass EMPIRICALLY.
// v2 list. The v1 list ranked by TAP COUNT and every entry proved irrelevant
// (tone/grade pass, glyph filter, material ubershaders). That ranking was the
// mistake: AA is confirmed present on alpha-tested cutouts as well as
// geometry with MSAA off, which no MSAA-like mechanism can do - it needs a
// whole-image operation, and a temporal/accumulation resolve is only
// 2 taps (current + history) so tap ranking actively hid it.
//
// These are the shaders RUNTIME attribution proved are bound for fullscreen
// quads, ordered by suspicion:
//   #1 ps_5CFEC5A6 - 2xtexld, mul, LRP. An lrp between two textures is
//      literally lerp(current, history, k): the shape of a temporal resolve.
//   #2 ps_1983C6B6 / #3 ps_EBAA9CE9 - 2-tap, appear in both MULTI_SAMPLE and
//      DRAW_FILTER, the other blend-shaped candidates.
//   #4 ps_68A227BC - 2-tap with the largest draw count of any post shader.
//   #5-8 - the cmp-heavy 1-tap filters from DRAW_FILTER.
static const DWORD g_psKillCandidates[] = {
    0x5CFEC5A6u,   // 2 taps: texld x2, mul, LRP  <- temporal-resolve shape
    0x1983C6B6u,   // 2 taps, MULTI_SAMPLE + DRAW_FILTER
    0xEBAA9CE9u,   // 2 taps, mad-heavy
    0x68A227BCu,   // 2 taps, ~600k draws
    0x0619441Cu,   // 1 tap, 25 instrs, 11x cmp
    0x97308FE6u,   // 1 tap, 54 instrs, 8x cmp
    0xD1BDD522u,   // 1 tap, 12 instrs
    0x77C4B5D4u,   // 2 taps, MULTI_SAMPLE
    0x20656E5Fu,   // 2 taps, dp3-heavy
    0xE5537D53u,   // 1 tap, MULTI_SAMPLE
    0x23A804BFu,   // 1 tap, simplest blit
    0xDCD57A17u,   // 25 taps - the GLYPH OUTLINE filter (HD GUI mod lever)
};
// (Graphics_Scaling has no GUI control here - it is an in-game menu option.)
// A2C candidate identify: hot dropdown, because walking candidates by editing
// the ini and relaunching is not a workflow anyone should be asked to use.
#define IDC_A2CID_COMBO 3016
// A long STATIC list beats a short reordering one: entries are keyed by
// shader creation index and never move, so it can be stepped through fast.
#define PS_IDENTIFY_LIST_MAX 16000

#define NUM_PS_KILL (sizeof(g_psKillCandidates)/sizeof(g_psKillCandidates[0]))
static const char *g_psKillLabels[] = {
    "Off (kill nothing)",
    "1: ps_5CFEC5A6  LRP blend <- prime AA suspect",
    "2: ps_1983C6B6  2-tap blend",
    "3: ps_EBAA9CE9  2-tap",
    "4: ps_68A227BC  2-tap, most draws",
    "5: ps_0619441C  1-tap, cmp-heavy",
    "6: ps_97308FE6  1-tap, cmp-heavy",
    "7: ps_D1BDD522  1-tap",
    "8: ps_77C4B5D4  2-tap",
    "9: ps_20656E5F  2-tap dp3",
    "10: ps_E5537D53  1-tap",
    "11: ps_23A804BF  1-tap blit",
    "12: ps_DCD57A17  glyph outlines (HD GUI)",
};

static HWND g_hCheckBoxes[NUM_TOGGLES];
static HWND g_hStatsLabel = NULL;
static HWND g_hThreshEdit = NULL;
static HWND g_hFpsEdit = NULL;
static HWND g_hDeferEdit = NULL;
static HWND g_hShadowCombo = NULL;
static HWND g_hShadowBufCombo = NULL;
static HWND g_hSplitNearCombo = NULL;
static HWND g_hSplitFarCombo = NULL;
static HWND g_hTalkCombo = NULL;
static HWND g_hMsaaCombo = NULL;
static HWND g_hA2cCheck = NULL;   // retired from the panel; see the texkill note
static HWND g_hFxaaCombo = NULL;
static HWND g_hA2cIdCombo = NULL;
static HWND g_hA2cIdLabel = NULL;

// ---- Frametime graph overlay ---------------------------------------------
// A layered, click-through, always-on-top window drawn over the game with
// plain GDI - deliberately NOT a D3D9 overlay rendered through the game's own
// pipeline. Same reasoning as the control panel: this whole investigation has
// been chasing bugs in that pipeline, an in-engine overlay would need the
// Present hook (which has never once fired on this game's real device), and
// an instrument that perturbs the thing it measures is worse than useless
// here. A separate window costs the render path nothing.
//
// It graphs the ENGINE TICK, which is the entire point: RTSS graphs presents,
// and this game's presentation is decoupled from its logic tick, so RTSS
// shows a flat line through hitches that are plainly visible. This shows what
// the game logic actually did.
//
// Works because the game runs Windowed (confirmed: `Windowed=1` on the real
// swap chain). A true exclusive-fullscreen device would not be overlayable
// this way.
// Sized for a 4K capture that gets viewed at 1080p: everything is halved on
// the way down, so the text is drawn at roughly 2x what would be comfortable
// natively. GDI's default font is ~16px and becomes illegible after that
// downscale, hence an explicit font rather than the stock one.
#define OVL_W 1000
#define OVL_H 320
#define OVL_FONT_H 34            // header/footer text height in pixels
#define OVL_LABEL_H 24           // reference-line labels
// Plot ceiling and colour bands are all derived from g_stutterThresholdUsec,
// so changing the threshold rescales the graph instead of leaving bars
// clipped or the whole thing amber. 2.2x keeps the "2x threshold" line
// comfortably inside the plot.
static HWND g_hOverlay = NULL;
static HFONT g_ovlFont = NULL, g_ovlFontSmall = NULL;
// Default bottom-RIGHT. The game's menu bar runs along the top and this window
// is topmost, so the top corners cover it; bottom-right is furthest from both
// the menu and the game's own bottom-left HUD elements.
static volatile LONG g_overlayPos = 3;

static void EnsureOverlayFonts(void)
{
    if (!g_ovlFont) {
        // Negative height = character height rather than cell height. Consolas
        // is fixed-width, so the numbers stop jittering horizontally as they
        // change - which matters a lot when reading a live readout.
        g_ovlFont = CreateFontA(-OVL_FONT_H, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    }
    if (!g_ovlFontSmall) {
        g_ovlFontSmall = CreateFontA(-OVL_LABEL_H, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    }
}

static void DrawOverlayGraph(HDC dc)
{
    EnsureOverlayFonts();
    HGDIOBJ oldFont = SelectObject(dc, g_ovlFont);
    RECT full = { 0, 0, OVL_W, OVL_H };
    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 16));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    const int padL = 10, padT = OVL_FONT_H + 14, padB = OVL_FONT_H + 12;
    int plotH = OVL_H - padT - padB;
    int plotW = OVL_W - padL * 2;

    // Reference lines: 60fps (16.7ms) and 30fps (33.3ms). Anything touching
    // the 16.7 line is a frame that failed to sustain 60fps.
    // The graph's scale is derived from the threshold, but the threshold now
    // defaults to 1s (watchdog effectively off) - which would scale the plot
    // to 2.2 SECONDS and flatten every real frame into the bottom pixel.
    // Clamp the scaling input so the two settings stop being coupled: the
    // watchdog can be as insensitive as it likes while the graph stays
    // readable around the 16.7/33.3ms reference lines.
    LONG thr = g_stutterThresholdUsec;
    if (thr < 1000) thr = 1000;
    if (thr > 33333) thr = 33333;
    LONG plotMax = (LONG)(thr * 2.2);
    char lbl1[16], lbl2[16];
    sprintf(lbl1, "%.1f", thr / 1000.0);
    sprintf(lbl2, "%.1f", thr * 2 / 1000.0);
    struct { LONG us; COLORREF c; const char *label; } refs[2] = {
        { thr,     RGB(70, 120, 70), lbl1 },
        { thr * 2, RGB(120, 70, 70), lbl2 },
    };
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 2; i++) {
        int y = padT + plotH - (int)((double)refs[i].us / plotMax * plotH);
        if (y < padT || y > padT + plotH) continue;
        HPEN pen = CreatePen(PS_SOLID, 2, refs[i].c);
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, padL, y, NULL);
        LineTo(dc, padL + plotW, y);
        SelectObject(dc, old);
        DeleteObject(pen);
        SetTextColor(dc, refs[i].c);
        SelectObject(dc, g_ovlFontSmall);
        TextOutA(dc, padL + plotW - 64, y - OVL_LABEL_H - 2, refs[i].label, (int)strlen(refs[i].label));
    }

    // Bars, oldest to newest left-to-right. Colour encodes severity so a
    // glance is enough: green under 60fps-equivalent, amber past it, red past
    // 30fps-equivalent.
    LONG pos = g_frameRingPos;
    LONG worst = 0;
    for (int i = 0; i < FRAME_RING && i < plotW; i++) {
        LONG idx = (pos - FRAME_RING + i) % FRAME_RING;
        if (idx < 0) idx += FRAME_RING;
        LONG us = g_frameRing[idx];
        if (us <= 0) continue;
        if (us > worst) worst = us;
        int h = (int)((double)us / plotMax * plotH);
        if (h > plotH) h = plotH;
        if (h < 1) h = 1;
        COLORREF c = (us > thr * 2) ? RGB(230, 70, 70)
                   : (us > thr)     ? RGB(230, 180, 60)
                                    : RGB(90, 200, 110);
        int x = padL + (plotW * i) / FRAME_RING;
        RECT bar = { x, padT + plotH - h, x + 3, padT + plotH };
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &bar, b);
        DeleteObject(b);
    }

    // Kept short on purpose: at ~34px Consolas roughly 50 characters fit
    // across OVL_W, and an overflowing header is worse than a terse one.
    // The "which clock is this" caption lives in the footer, which has room.
    char hdr[160];
    LONG p50 = g_liveP50;
    sprintf(hdr, "%.0ffps  p50 %.1f  p99 %.1f  max %.1f  >%.1f %ld/%ld",
            p50 > 0 ? 1000000.0 / (double)p50 : 0.0,
            p50 / 1000.0, g_liveP99 / 1000.0, g_liveWorst / 1000.0,
            thr / 1000.0, g_liveOver16, g_liveFrames);
    SetTextColor(dc, RGB(235, 235, 245));
    SelectObject(dc, g_ovlFont);
    TextOutA(dc, padL, 6, hdr, (int)strlen(hdr));

    char foot[96];
    sprintf(foot, "ENGINE TICK - game logic, not presents (%d)", FRAME_RING);
    SetTextColor(dc, RGB(150, 150, 165));
    SelectObject(dc, g_ovlFont);
    TextOutA(dc, padL, OVL_H - OVL_FONT_H - 8, foot, (int)strlen(foot));
    SelectObject(dc, oldFont);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        // Double-buffered: painting bar-by-bar straight to the window
        // flickers badly at this repaint rate.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, OVL_W, OVL_H);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
        DrawOverlayGraph(mem);
        BitBlt(dc, 0, 0, OVL_W, OVL_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;   // fully repainted above
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void EnsureOverlayWindow(void)
{
    if (g_hOverlay) return;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LRStutterOverlay";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    // WS_EX_TRANSPARENT makes it click-through so it never steals input from
    // the game; WS_EX_NOACTIVATE keeps it from taking focus.
    g_hOverlay = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LRStutterOverlay", "", WS_POPUP,
        20, 20, OVL_W, OVL_H, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (g_hOverlay) SetLayeredWindowAttributes(g_hOverlay, 0, 205, LWA_ALPHA);
}

// Overlay driver thread. The overlay used to be driven from the control
// panel's 250ms sync timer, which made a shipping feature depend on a
// debug window; with the panel deprecated (ENABLE_GUI_PANEL 0) it owns its
// own thread. Sole owner either way - the panel's timer no longer touches
// the overlay, so there is no double-drive when the panel is compiled back in.
//
// PeekMessage pump rather than SetTimer: the overlay window is created on
// THIS thread, so this loop is what dispatches its WM_PAINT. Same 250ms
// cadence as the old timer.
static DWORD WINAPI OverlayThread(LPVOID param)
{
    (void)param;
    LogLine("[boot] overlay: driver thread entered");
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (g_overlayEnabled) {
            EnsureOverlayWindow();
            if (g_hOverlay) {
                if (!IsWindowVisible(g_hOverlay)) ShowWindow(g_hOverlay, SW_SHOWNOACTIVATE);
                // Positioned against the GAME's client area rather than the
                // desktop, so it stays put in windowed mode as well. Re-asserts
                // topmost each tick: the game reasserts its own z-order on
                // focus changes and would otherwise cover this.
                {
                    int x = 20, y = 20;
                    const int m = 20;      // margin from the chosen corner
                    RECT rc;
                    HWND gw = g_gameHwnd;
                    POINT tl, br;
                    int have = 0;
                    if (gw && IsWindow(gw) && GetClientRect(gw, &rc) &&
                        rc.right > rc.left && rc.bottom > rc.top) {
                        tl.x = rc.left;  tl.y = rc.top;
                        br.x = rc.right; br.y = rc.bottom;
                        if (ClientToScreen(gw, &tl) && ClientToScreen(gw, &br)) have = 1;
                    }
                    if (!have) {           // pre-Reset, or the window is gone
                        RECT wa;
                        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
                            tl.x = wa.left;  tl.y = wa.top;
                            br.x = wa.right; br.y = wa.bottom;
                            have = 1;
                        }
                    }
                    if (have) {
                        switch (g_overlayPos) {
                        case 1: x = br.x - OVL_W - m; y = tl.y + m;          break; // top-right
                        case 2: x = tl.x + m;         y = br.y - OVL_H - m;  break; // bottom-left
                        case 3: x = br.x - OVL_W - m; y = br.y - OVL_H - m;  break; // bottom-right
                        default: x = tl.x + m;        y = tl.y + m;          break; // top-left
                        }
                    }
                    SetWindowPos(g_hOverlay, HWND_TOPMOST, x, y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                }
                InvalidateRect(g_hOverlay, NULL, FALSE);
            }
        } else if (g_hOverlay && IsWindowVisible(g_hOverlay)) {
            ShowWindow(g_hOverlay, SW_HIDE);
        }
        Sleep(250);
    }
}

// ---- DEPRECATED: the standalone control-panel window ----------------------
// Superseded 2026-08-11 by the game-menu integration above (ENABLE_GAME_MENU).
// From now on new options go in the game's own menu bar; this window is not
// the place to add them. Gated rather than deleted, per the project rule -
// it was the mod's only UI for most of this investigation and it still
// documents every control that ever existed, including the diagnostic
// dropdowns that live behind ENABLE_SHADER_DIAG.
//
// Note the one real coupling that had to be broken to retire it: the
// frametime overlay was driven by this window's sync timer. That drive loop
// now lives in OverlayThread above, so the overlay is unaffected.
#if ENABLE_GUI_PANEL

static LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        for (size_t i = 0; i < NUM_TOGGLES; i++) {
            g_hCheckBoxes[i] = CreateWindowA("BUTTON", g_toggles[i].label,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 10 + (int)i * 26, 300, 24, hwnd,
                (HMENU)(UINT_PTR)(IDC_CHECK_BASE + i), NULL, NULL);
        }
        // Live frametime readout. This exists because RTSS measures
        // PRESENTS, and this game's presentation runs on a cadence
        // independent of the logic tick - its graph stays flat through a
        // visible hitch, which makes it untrustworthy here. These numbers
        // come from the engine's own per-tick timing, so they show what the
        // game is actually doing.
        {
            int y = 10 + (int)NUM_TOGGLES * 26 + 6;
            CreateWindowA("STATIC", "Stutter/graph threshold (ms):", WS_CHILD | WS_VISIBLE,
                10, y + 4, 180, 20, hwnd, NULL, NULL, NULL);
            char cur[32];
            sprintf(cur, "%.1f", g_stutterThresholdUsec / 1000.0);
            g_hThreshEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cur,
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                195, y, 55, 24, hwnd, (HMENU)(UINT_PTR)IDC_THRESH_EDIT, NULL, NULL);
            CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                258, y, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_THRESH_APPLY, NULL, NULL);

            // Target FPS lock. Only meaningful while UnlockFramerate is also
            // checked - see ApplyFramerateUnlock for the known CPU-cost
            // tradeoff (this reuses the game's own Sleep-spin limiter, so it
            // is NOT free like the plain unlock) and the open question of
            // whether it helps the frame-to-frame jitter symptom at all.
            int y2 = y + 30;
            CreateWindowA("STATIC", "Target FPS (0=unlocked, needs Unlock on):", WS_CHILD | WS_VISIBLE,
                10, y2 + 4, 260, 20, hwnd, NULL, NULL, NULL);
            char curFps[32];
            sprintf(curFps, "%.2f", g_targetFpsX100 / 100.0);
            g_hFpsEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", curFps,
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                275, y2, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_FPS_EDIT, NULL, NULL);
            CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                340, y2, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_FPS_APPLY, NULL, NULL);

#if ENABLE_DEFER_UPLOADS
            // Deferred-upload rate. Only meaningful while DeferUploads is
            // also checked - see SubmitStagedUpload/DrainStagedUploads.
            int y3 = y2 + 30;
            CreateWindowA("STATIC", "Deferred uploads per frame (needs Defer on):", WS_CHILD | WS_VISIBLE,
                10, y3 + 4, 260, 20, hwnd, NULL, NULL, NULL);
            char curDefer[32];
            sprintf(curDefer, "%ld", g_deferPerFrame);
            g_hDeferEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", curDefer,
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                275, y3, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_DEFER_EDIT, NULL, NULL);
            CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                340, y3, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_DEFER_APPLY, NULL, NULL);
#else
            int y3 = y2;   // defer control retired; rows below move up
#endif

            // Shadow map resolution. A dropdown rather than an edit box
            // because the engine only ever uses discrete power-of-two sizes
            // and a typo here writes into its live settings object.
            int y4 = y3 + 30;
            CreateWindowA("STATIC", "Shadow map resolution:", WS_CHILD | WS_VISIBLE,
                10, y4 + 4, 150, 20, hwnd, NULL, NULL, NULL);
            // Height here budgets for the dropped-down list, not the closed box.
            g_hShadowCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4, 235, 200, hwnd, (HMENU)(UINT_PTR)IDC_SHADOW_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_SHADOW_CHOICES; i++) {
                    SendMessageA(g_hShadowCombo, CB_ADDSTRING, 0, (LPARAM)g_shadowResLabels[i]);
                    if (g_shadowResChoices[i] == g_shadowMapRes) sel = (int)i;
                }
                SendMessageA(g_hShadowCombo, CB_SETCURSEL, sel, 0);
            }

            // Screen-space shadow buffer resolution. Same creation-time
            // caveat as the shadow map above: it applies when the buffers are
            // next built, i.e. on an area change or restart.
            int y4b = y4 + 30;
            CreateWindowA("STATIC", "Shadow buffer res:", WS_CHILD | WS_VISIBLE,
                10, y4b + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hShadowBufCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4b, 235, 200, hwnd, (HMENU)(UINT_PTR)IDC_SHADOWBUF_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_SHADOWBUF_CHOICES; i++) {
                    SendMessageA(g_hShadowBufCombo, CB_ADDSTRING, 0, (LPARAM)g_shadowBufLabels[i]);
                    if (g_shadowBufChoices[i] == g_shadowBufResPct) sel = (int)i;
                }
                SendMessageA(g_hShadowBufCombo, CB_SETCURSEL, sel, 0);
            }

            // MSAA + alpha-to-coverage. Both apply within a frame: the
            // substitution reads the sample count per bind, and A2C mirrors
            // the next ALPHATESTENABLE write. Surface rebuild happens on the
            // render thread, never here.
            int y4c = y4b + 30;
            CreateWindowA("STATIC", "MSAA (needs native res):", WS_CHILD | WS_VISIBLE,
                10, y4c + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hMsaaCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4c, 235, 200, hwnd, (HMENU)(UINT_PTR)IDC_MSAA_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_MSAA_CHOICES; i++) {
                    SendMessageA(g_hMsaaCombo, CB_ADDSTRING, 0, (LPARAM)g_msaaLabels[i]);
                    if (g_msaaChoices[i] == g_msaaSamples) sel = (int)i;
                }
                SendMessageA(g_hMsaaCombo, CB_SETCURSEL, sel, 0);
            }
            // A2C checkbox RETIRED from the panel: 85/256 of this engine's
            // pixel shaders do their cutouts with in-shader TEXKILL and the
            // engine never writes ALPHATESTENABLE (a2c stayed 0 across whole
            // sessions), so the vendor A2C backdoor has nothing to act on.
            // Working A2C here means patching the cutout shaders - parked
            // with the planned shader-injection work. Config key and mirror
            // code remain, inert.
            //
            // In its place: the post-shader kill list. Substitutes the chosen
            // candidate's pixel shader at BIND time - passthrough kills the
            // filter; with the magenta debug toggle on it paints the pass's
            // footprint instead, which is the instrument for FINDING the
            // real AA pass among the candidates.
            // NO GUI for Graphics_Scaling: it is already a graphics option in
            // the game's own menu, so a duplicate control here is pure
            // clutter in a panel that was deliberately trimmed. The config
            // key and ApplyScalingMode remain (harmless, default 0 = don't
            // touch) only because the [scaling] log line reporting the
            // engine's current mode is worth keeping.
#if ENABLE_SHADER_DIAG
            // Identify walk - hot. Selecting N paints that shader magenta and
            // the label names it. Retired from the shipping panel with the
            // rest of the diagnostics; one #define brings it back.
            int y4f = y4c + 30;
            CreateWindowA("STATIC", "Identify shader:", WS_CHILD | WS_VISIBLE,
                10, y4f + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hA2cIdCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4f, 235, 260, hwnd, (HMENU)(UINT_PTR)IDC_A2CID_COMBO, NULL, NULL);
            {
                // Only "Off" up front - real entries are appended by the
                // timer as the game creates shaders, so the list grows but
                // existing indices never move.
                SendMessageA(g_hA2cIdCombo, CB_ADDSTRING, 0, (LPARAM)"Off");
                SendMessageA(g_hA2cIdCombo, CB_SETCURSEL, 0, 0);
            }
            int y4g = y4f + 28;
            g_hA2cIdLabel = CreateWindowA("STATIC", "(no candidates yet)",
                WS_CHILD | WS_VISIBLE, 10, y4g, 390, 18, hwnd, NULL, NULL, NULL);

            int y4d = y4g + 24;
            CreateWindowA("STATIC", "Kill post shader:", WS_CHILD | WS_VISIBLE,
                10, y4d + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hFxaaCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4d, 235, 220, hwnd, (HMENU)(UINT_PTR)IDC_FXAA_COMBO, NULL, NULL);
            {
                for (size_t i = 0; i <= NUM_PS_KILL; i++)
                    SendMessageA(g_hFxaaCombo, CB_ADDSTRING, 0, (LPARAM)g_psKillLabels[i]);
                LONG p = g_fxaaPick;
                if (p < 0 || (size_t)p > NUM_PS_KILL) p = 0;
                SendMessageA(g_hFxaaCombo, CB_SETCURSEL, (WPARAM)p, 0);
            }
#else
            int y4d = y4c;   // diagnostic rows retired; the panel closes up
#endif

            // Cascade splits. These apply per frame, so changing them here is
            // visible immediately - no relaunch and no area change needed,
            // unlike the shadow resolution above.
            int y5 = y4d + 30;
            CreateWindowA("STATIC", "Shadow cascade near split:", WS_CHILD | WS_VISIBLE,
                10, y5 + 4, 160, 20, hwnd, NULL, NULL, NULL);
            g_hSplitNearCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                175, y5, 225, 200, hwnd, (HMENU)(UINT_PTR)IDC_SPLITNEAR_COMBO, NULL, NULL);
            int y6 = y5 + 30;
            CreateWindowA("STATIC", "Shadow cascade far split:", WS_CHILD | WS_VISIBLE,
                10, y6 + 4, 160, 20, hwnd, NULL, NULL, NULL);
            g_hSplitFarCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                175, y6, 225, 200, hwnd, (HMENU)(UINT_PTR)IDC_SPLITFAR_COMBO, NULL, NULL);
            {
                int selN = 0, selF = 0;
                for (size_t i = 0; i < NUM_SPLIT_CHOICES; i++) {
                    SendMessageA(g_hSplitNearCombo, CB_ADDSTRING, 0, (LPARAM)g_splitPctLabels[i]);
                    SendMessageA(g_hSplitFarCombo,  CB_ADDSTRING, 0, (LPARAM)g_splitPctLabels[i]);
                    if (g_splitPctChoices[i] == g_shadowSplitNearPct) selN = (int)i;
                    if (g_splitPctChoices[i] == g_shadowSplitFarPct)  selF = (int)i;
                }
                SendMessageA(g_hSplitNearCombo, CB_SETCURSEL, selN, 0);
                SendMessageA(g_hSplitFarCombo,  CB_SETCURSEL, selF, 0);
            }

#if ENABLE_TALK_TIMER
            // Talk-entry countdown rate. Takes effect on the next frame once
            // patched, so a bugged interaction can be retried without a
            // relaunch.
            int y7 = y6 + 30;
            CreateWindowA("STATIC", "Talk timer rate (60fps fix):", WS_CHILD | WS_VISIBLE,
                10, y7 + 4, 165, 20, hwnd, NULL, NULL, NULL);
            g_hTalkCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                180, y7, 220, 200, hwnd, (HMENU)(UINT_PTR)IDC_TALKTIMER_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_TALK_CHOICES; i++) {
                    SendMessageA(g_hTalkCombo, CB_ADDSTRING, 0, (LPARAM)g_talkPctLabels[i]);
                    if (g_talkPctChoices[i] == g_talkTimerPct) sel = (int)i;
                }
                SendMessageA(g_hTalkCombo, CB_SETCURSEL, sel, 0);
            }
#else
            // Talk-timer control retired: nothing sits between the split
            // combos and the stats label, so the label moves up into the freed
            // row instead of leaving a gap.
            int y7 = y6;
#endif

            g_hStatsLabel = CreateWindowA("STATIC", "frametime: (waiting)",
                WS_CHILD | WS_VISIBLE,
                10, y7 + 30, 400, 56, hwnd, NULL, NULL, NULL);
        }
        SetTimer(hwnd, IDT_SYNC_TIMER, 250, NULL);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_THRESH_APPLY) {
            char buf[32];
            GetWindowTextA(g_hThreshEdit, buf, sizeof(buf));
            double ms = atof(buf);
            LONG us = (LONG)(ms * 1000.0 + 0.5);
            // Clamp to the same bounds the config loader enforces, then write
            // the clamped value back into the box so the UI never shows a
            // number that is not actually in effect.
            if (us < g_numerics[0].lo) us = g_numerics[0].lo;
            if (us > g_numerics[0].hi) us = g_numerics[0].hi;
            g_stutterThresholdUsec = us;
            sprintf(buf, "%.1f", us / 1000.0);
            SetWindowTextA(g_hThreshEdit, buf);
            SaveConfig();
            char line[128];
            sprintf(line, "[gui] stutter/graph threshold set to %ldus (%.1fms)", us, us / 1000.0);
            LogLine(line);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_FPS_APPLY) {
            char buf[32];
            GetWindowTextA(g_hFpsEdit, buf, sizeof(buf));
            double fps = atof(buf);
            LONG x100 = (fps <= 0.0) ? 0 : (LONG)(fps * 100.0 + 0.5);
            if (x100 < g_numerics[1].lo) x100 = g_numerics[1].lo;
            if (x100 > g_numerics[1].hi) x100 = g_numerics[1].hi;
            g_targetFpsX100 = x100;
            sprintf(buf, "%.2f", x100 / 100.0);
            SetWindowTextA(g_hFpsEdit, buf);
            SaveConfig();
            char line[160];
            if (x100 > 0) {
                sprintf(line, "[gui] target FPS set to %.2f - reuses the game's own limiter, "
                              "CPU cost from UnlockFramerate WILL return", x100 / 100.0);
            } else {
                sprintf(line, "[gui] target FPS set to unlocked (0)");
            }
            LogLine(line);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE &&
            (LOWORD(wParam) == IDC_SPLITNEAR_COMBO || LOWORD(wParam) == IDC_SPLITFAR_COMBO)) {
            int isNear = (LOWORD(wParam) == IDC_SPLITNEAR_COMBO);
            HWND h = isNear ? g_hSplitNearCombo : g_hSplitFarCombo;
            int sel = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_SPLIT_CHOICES) {
                LONG v = g_splitPctChoices[sel];
                if (isNear) g_shadowSplitNearPct = v; else g_shadowSplitFarPct = v;
                SaveConfig();
                char line[160];
                sprintf(line, "[gui] cascade %s split set to %ld%% (applies immediately)",
                        isNear ? "near" : "far", v);
                LogLine(line);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_MSAA_COMBO) {
            int sel = (int)SendMessageA(g_hMsaaCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_MSAA_CHOICES) {
                g_msaaSamples = g_msaaChoices[sel];
                SaveConfig();
                char line[224];
                sprintf(line, "[gui] MSAA set to %ld - applies within a frame; surfaces are "
                              "rebuilt on the render thread. Engages only when in-game res "
                              "matches the desktop res.", g_msaaSamples);
                LogLine(line);
            }
            return 0;
        }
#if ENABLE_SHADER_DIAG
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_A2CID_COMBO) {
            int sel = (int)SendMessageA(g_hA2cIdCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel <= PS_IDENTIFY_LIST_MAX) {
                g_psIdentify = sel;
                SaveConfig();
                char line[160];
                sprintf(line, "[gui] shader identify -> rank %d%s", sel,
                        sel ? " (magenta)" : " (off)");
                LogLine(line);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_FXAA_COMBO) {
            int sel = (int)SendMessageA(g_hFxaaCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel <= NUM_PS_KILL) {
                g_fxaaPick = sel;
                SaveConfig();
                char line[224];
                sprintf(line, "[gui] post-shader kill set to %s%s", g_psKillLabels[sel],
                        (sel > 0 && g_msaaDebugClear)
                            ? "  (magenta debug ON: footprint mode)" : "");
                LogLine(line);
            }
            return 0;
        }
#endif  // ENABLE_SHADER_DIAG
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_SHADOWBUF_COMBO) {
            int sel = (int)SendMessageA(g_hShadowBufCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_SHADOWBUF_CHOICES) {
                g_shadowBufResPct = g_shadowBufChoices[sel];
                SaveConfig();
                char line[256];
                sprintf(line, "[gui] shadow buffer res set to %ld%% of screen - applies when the "
                              "buffers are next created (area change or restart). Engine default "
                              "is 50%%; gains continue past 100%% because the limit is aliasing, "
                              "not filter softness.", g_shadowBufResPct);
                LogLine(line);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_SHADOW_COMBO) {
            int sel = (int)SendMessageA(g_hShadowCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_SHADOW_CHOICES) {
                g_shadowMapRes = g_shadowResChoices[sel];
                SaveConfig();
                char line[224];
                // The engine reads this value both when it CREATES the shadow
                // textures and every frame for the projection and PCF offsets.
                // Changing it mid-session updates the maths immediately while
                // the existing textures keep their old size, so shadows can
                // look wrong until the set is rebuilt (area change / device
                // reset). Say so rather than let it look like a broken fix.
                sprintf(line, "[gui] shadow map resolution set to %ld - takes full effect when the "
                              "shadow maps are next recreated (area change or restart); may look "
                              "wrong until then", g_shadowMapRes);
                LogLine(line);
            }
            return 0;
        }
#if ENABLE_TALK_TIMER
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_TALKTIMER_COMBO) {
            int sel = (int)SendMessageA(g_hTalkCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_TALK_CHOICES) {
                g_talkTimerPct = g_talkPctChoices[sel];
                SaveConfig();
                char line[224];
                // Once the instruction is patched the rate follows this value
                // live, but the patch itself is one-way for the session: going
                // back to Off leaves the operand pointing at our double, which
                // then has to hold the stock 0.05 rather than be ignored.
                if (g_talkTimerPct <= 0 && g_talkStepPatched == 1) g_talkStep = 0.05;
                sprintf(line, "[gui] talk timer rate set to %ld%% (step %.4f/frame, "
                              "entries live %.0f-%.0f frames) - applies to the next "
                              "conversation", g_talkTimerPct,
                        g_talkTimerPct > 0 ? 0.05 * g_talkTimerPct / 100.0 : 0.05,
                        5.0 / (g_talkTimerPct > 0 ? 0.05 * g_talkTimerPct / 100.0 : 0.05),
                        10.0 / (g_talkTimerPct > 0 ? 0.05 * g_talkTimerPct / 100.0 : 0.05));
                LogLine(line);
            }
            return 0;
        }
#endif  // ENABLE_TALK_TIMER
#if ENABLE_DEFER_UPLOADS
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_DEFER_APPLY) {
            char buf[32];
            GetWindowTextA(g_hDeferEdit, buf, sizeof(buf));
            LONG n = atol(buf);
            if (n < g_numerics[2].lo) n = g_numerics[2].lo;
            if (n > g_numerics[2].hi) n = g_numerics[2].hi;
            g_deferPerFrame = n;
            sprintf(buf, "%ld", n);
            SetWindowTextA(g_hDeferEdit, buf);
            SaveConfig();
            char line[128];
            sprintf(line, "[gui] deferred uploads per frame set to %ld", n);
            LogLine(line);
            return 0;
        }
#endif
        if (HIWORD(wParam) == BN_CLICKED) {
            size_t idx = (size_t)(LOWORD(wParam) - IDC_CHECK_BASE);
            if (idx < NUM_TOGGLES) {
                LONG newVal = (SendMessageA(g_hCheckBoxes[idx], BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                *g_toggles[idx].flag = newVal;
                SaveConfig();
                char line[200];
                sprintf(line, "[gui] %s now %s", g_toggles[idx].label, newVal ? "ENABLED" : "DISABLED");
                LogLine(line);
            }
        }
        return 0;
    case WM_TIMER:
        // Keeps the checkboxes truthful regardless of which control path
        // (hotkey or this window) last changed the flags - a hotkey press
        // updates the underlying value immediately; this just re-syncs the
        // checkbox visuals to match within 250ms.
        if (wParam == IDT_SYNC_TIMER) {
            for (size_t i = 0; i < NUM_TOGGLES; i++) {
                SendMessageA(g_hCheckBoxes[i], BM_SETCHECK,
                             *g_toggles[i].flag ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            // F9 = capture the FULL scene target at full resolution, right
            // now. A timer-armed capture cannot be aimed: the frame that
            // matters is whatever the user is looking at when they see the
            // effect, and so far every automatic capture has landed on the
            // wrong subject. Polled here (250ms) rather than from the render
            // thread; the render thread performs the actual capture.
#if ENABLE_SHADER_DIAG
            // Grow the identify list as the game creates shaders. APPEND-ONLY,
            // so an index selected a minute ago still means the same shader -
            // the whole point of moving off activity ranking.
            if (g_hA2cIdCombo) {
                LONG have = (LONG)SendMessageA(g_hA2cIdCombo, CB_GETCOUNT, 0, 0) - 1;
                LONG want = g_psMapCount;
                if (want > PS_IDENTIFY_LIST_MAX) want = PS_IDENTIFY_LIST_MAX;
                if (have < 0) have = 0;
                for (LONG i = have; i < want; i++) {
                    char it[64];
                    sprintf(it, "#%ld  ps_%08X%s", i + 1, g_psMap[i].hash,
                            g_psMap[i].hasVariant ? "  CUTOUT" : "");
                    SendMessageA(g_hA2cIdCombo, CB_ADDSTRING, 0, (LPARAM)it);
                }
            }
            // Readout for the SELECTED entry. "drawing now" is what tells you
            // whether the thing you are looking at is even on screen, which
            // matters when stepping a long static list.
            if (g_hA2cIdLabel) {
                char st[192];
                LONG want = g_psIdentify;
                if (want == 0) {
                    sprintf(st, "off - %ld shaders known, %ld drawing now",
                            g_psMapCount, g_psRankCount);
                } else if (want <= g_psMapCount) {
                    LONG idx = want - 1;
                    sprintf(st, "#%ld ps_%08X  draws/s=%ld  taps=%u  %s",
                            want, g_psMap[idx].hash, g_psMap[idx].recent * 2,
                            g_psMap[idx].taps,
                            g_psMap[idx].hasVariant ? "CUTOUT (A2C-able)" : "not a cutout");
                } else {
                    sprintf(st, "#%ld: not created yet (%ld known)",
                            want, g_psMapCount);
                }
                SetWindowTextA(g_hA2cIdLabel, st);
            }
#endif  // ENABLE_SHADER_DIAG
#if ENABLE_SURFACE_DIAG
            {
                static int f9WasDown = 0;
                int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                if (f9 && !f9WasDown) {
                    g_captureRequest = 1;
                    LogLine("[capture] F9 pressed - full-frame capture armed");
                }
                f9WasDown = f9;
            }
#endif  // ENABLE_SURFACE_DIAG
            if (g_hStatsLabel) {
                LONG p50 = g_liveP50, p99 = g_liveP99;
                LONG n = g_liveFrames, over = g_liveOver16, worst = g_liveWorst;
                char st[256];
                sprintf(st, "ENGINE TICK (not RTSS/present):\r\n"
                            "  %.1f fps   p50 %.1fms   p99 %.1fms\r\n"
                            "  worst %.1fms   over 16.7ms: %ld/%ld",
                        p50 > 0 ? 1000000.0 / (double)p50 : 0.0,
                        p50 / 1000.0, p99 / 1000.0, worst / 1000.0, over, n);
                SetWindowTextA(g_hStatsLabel, st);
            }
            // (The overlay show/hide/repaint drive used to live here; it is
            // OverlayThread's job now so the overlay survives this window
            // being retired.)
        }
        return 0;
    case WM_CLOSE:
        // Hide rather than destroy - an accidental close shouldn't lose the
        // panel or affect the hooks, which keep running either way since
        // the flags they read live independently of this window.
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI GuiThread(LPVOID param)
{
    (void)param;
    LogLine("[boot] gui: thread entered");
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LRStutterFixPanel";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);

    int panelHeight = 10 + (int)NUM_TOGGLES * 26 + 6 + 30 * 10 + 28 + 24 + 56 + 46;  // +threshold +fps +shadow rows +msaa +a2c identify(+label) +kill +stats
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "LRStutterFixPanel", "LR Stutter Fix - Debug Panel",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        10, 10, 420, panelHeight, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { LogLine("[boot] gui: CreateWindow FAILED"); return 0; }
    LogLine("[boot] gui: panel created");
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

#endif  // ENABLE_GUI_PANEL

// ---- Timer resolution probe + override -----------------------------------
// Testing a specific hypothesis about the COLD-pass stutters. Categorising
// the cold pass's ntdll waits by what they are actually blocked on gave
// 27 "main thread asleep INSIDE the game's own frame limiter" vs only 20
// genuine AMD driver waits - i.e. nearly half were never a driver problem at
// all. A normal frame here is ~17ms and consists almost entirely of that
// limiter sleep, so a frame only crosses the 20ms threshold this way if a
// single Sleep(1) inside FUN_00ac3040's spin overshoots badly.
//
// Windows' DEFAULT timer resolution is ~15.6ms, which would make Sleep(1)
// block for up to that long - exactly the overshoot signature. Nothing in
// this mod has ever called timeBeginPeriod (verified: zero references), and
// while RTSS is loaded and probably raises it process-wide, that has never
// been MEASURED here. So: query the real value rather than assume it either
// way. NtQueryTimerResolution reports in 100ns units, so 1ms == 10000 and
// the 15.6ms default == 156250.
//
// This probe is deliberately decisive in BOTH directions: if the current
// resolution already reads ~1ms, the whole hypothesis is dead on the spot
// and TimerRes cannot help, no test run needed. Only if it reads coarse is
// the override worth A/B testing.
//
// timeBeginPeriod/timeEndPeriod are a matched pair and safe to call at
// runtime, so this can hot-toggle. winmm is loaded dynamically to keep the
// existing link line unchanged and to stay out of DllMain's loader lock.
// RETIRED (ENABLE_TIMER_RES). The probe above was decisive in the direction
// that kills it: the system already reads 1.0000ms, so the override has
// nothing to raise and the Sleep-overshoot hypothesis is dead. Never produced
// a test run.
#if ENABLE_TIMER_RES
typedef LONG (WINAPI *PFN_NtQueryTimerResolution)(PULONG, PULONG, PULONG);
typedef UINT (WINAPI *PFN_timePeriod)(UINT);
static PFN_NtQueryTimerResolution g_ntQueryTimerRes = NULL;
static PFN_timePeriod g_timeBeginPeriod = NULL;
static PFN_timePeriod g_timeEndPeriod = NULL;
static volatile LONG g_timerResEnabled;   // tentative def near the top too
static LONG g_timerResApplied = 0;   // monitor-thread only, no atomics needed

static void ReportAndApplyTimerResolution(void)
{
    static int resolved = 0;
    if (!resolved) {
        resolved = 1;
        HMODULE hNt = GetModuleHandleA("ntdll.dll");
        if (hNt) g_ntQueryTimerRes =
            (PFN_NtQueryTimerResolution)GetProcAddress(hNt, "NtQueryTimerResolution");
        HMODULE hWinmm = LoadLibraryA("winmm.dll");
        if (hWinmm) {
            g_timeBeginPeriod = (PFN_timePeriod)GetProcAddress(hWinmm, "timeBeginPeriod");
            g_timeEndPeriod   = (PFN_timePeriod)GetProcAddress(hWinmm, "timeEndPeriod");
        }
    }

    if (g_timerResEnabled && !g_timerResApplied && g_timeBeginPeriod) {
        g_timeBeginPeriod(1);
        g_timerResApplied = 1;
        LogLine("[timer] TimerRes ON - timeBeginPeriod(1) applied");
    } else if (!g_timerResEnabled && g_timerResApplied && g_timeEndPeriod) {
        g_timeEndPeriod(1);
        g_timerResApplied = 0;
        LogLine("[timer] TimerRes OFF - timeEndPeriod(1), resolution released");
    }

    // Report the ACTUAL achieved resolution, not what we asked for - another
    // process can be holding a finer period, and only the real value explains
    // the limiter's behaviour.
    if (g_ntQueryTimerRes) {
        ULONG mn = 0, mx = 0, cur = 0;
        if (g_ntQueryTimerRes(&mn, &mx, &cur) == 0) {
            static ULONG lastCur = 0;
            if (cur != lastCur) {
                lastCur = cur;
                char line[192];
                sprintf(line, "[timer] actual resolution now %.4fms (coarsest=%.4fms finest=%.4fms) - "
                              "Sleep(1) can overshoot up to this much",
                        cur / 10000.0, mn / 10000.0, mx / 10000.0);
                LogLine(line);
            }
        }
    }
}
#endif  // ENABLE_TIMER_RES

static DWORD WINAPI MonitorThread(LPVOID param)
{
    (void)param;
    for (;;) {
        Sleep(500);
#if ENABLE_SURFACE_DIAG
        // F9 capture poll lives here as well as in the panel's timer: the
        // panel's timer only runs while that window is OPEN, and the first
        // F9 attempt logged nothing at all, which a closed panel would
        // explain exactly. This thread always runs.
        {
            static int f9Down = 0;
            int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (f9 && !f9Down) {
                g_captureRequest = 1;
                LogLine("[capture] F9 (monitor thread) - full-frame capture armed");
            }
            f9Down = f9;
        }
#endif  // ENABLE_SURFACE_DIAG
#if ENABLE_TIMER_RES
        ReportAndApplyTimerResolution();
#endif
        // REMOVED 2026-08-13: the one-shot "rewrite the ini so every key is
        // visible" used to run here, on the monitor thread's FIRST tick.
        //
        // Moving it here from startup was supposed to make it safe. It did
        // not: every failing run dies at exactly this point in the log - the
        // first monitor window - and the crashes are access violations on the
        // MAIN thread inside the HD GUI mod, i.e. concurrent with this write
        // rather than caused by it directly. Whether the mechanism is
        // contention or timing, a file write is not worth putting next to
        // another mod's initialisation.
        //
        // The ini is already complete on disk, and SaveConfig still runs
        // whenever a setting actually changes, so nothing is lost except the
        // first-launch-after-update case - which is worth far less than a
        // reliable startup. If it is ever wanted back, it belongs somewhere
        // provably late (first rendered frame, not first monitor tick).
        ApplyShadowResolution();
        // Same cadence and same risk profile as ApplyShadowResolution: writes
        // an engine settings field from this thread and lets the engine's own
        // change detector reallocate. That pattern is already shipping.
        ApplySsaaScale();
#if ENABLE_SCALING_MODE
        ApplyScalingMode();
#endif
#if ENABLE_SHADER_DIAG
        // Rank EVERY pixel shader by draws in the last window. Cumulative
        // counts made rank #1 wander between near-equal shaders (it flashed
        // on random objects); a per-window delta ranks what is actually being
        // drawn in the current view, which is the whole point of the walk.
        {
            LONG n = g_psMapCount, i, r;
            if (n > PS_MAP_MAX) n = PS_MAP_MAX;
            for (i = 0; i < n; i++) {
                LONG d = g_psMap[i].draws;
                g_psMap[i].recent = d - g_psMap[i].prevDraws;
                g_psMap[i].prevDraws = d;
            }
            LONG prevBest = 0x7FFFFFFF, count = 0;
            for (r = 0; r < PS_RANK_MAX; r++) {
                LONG cur = -1, curDraws = 0;
                for (i = 0; i < n; i++) {
                    LONG d = g_psMap[i].recent;
                    if (d <= 0 || d >= prevBest) continue;
                    if (d > curDraws) { curDraws = d; cur = i; }
                }
                if (cur < 0) break;
                g_psRankIdx[r] = cur;
                prevBest = curDraws;
                count = r + 1;
            }
            g_psRankCount = count;
            // Mark which ranked shaders have a cutout variant, so the label
            // can say whether A2C is even applicable to what is highlighted.
            for (r = 0; r < count; r++) {
                LONG idx = g_psRankIdx[r];
                g_psMap[idx].hasVariant = 0;
                LONG vn = g_a2cVariantCount;
                if (vn > A2C_MAX) vn = A2C_MAX;
                for (i = 0; i < vn; i++)
                    if (g_a2cVariants[i].orig == g_psMap[idx].obj) {
                        g_psMap[idx].hasVariant = 1;
                        break;
                    }
            }
        }
#endif  // ENABLE_SHADER_DIAG
        ReportAndClampSleepGranularity();
        FlushLog();   // see LogLine: buffered writes, flushed here instead
#if ENABLE_FRAMETIME_DUMP
        // Frametime burst: arm on the rising edge, dump once full.
        {
            static LONG lastFtState = 0;
            LONG now = g_logFrameTimes;
            if (now && !lastFtState) {
                g_ftCount = 0;
                g_ftDumped = 0;
                LogLine("[ft] ---- frametime capture armed (toggle switched on) ----");
            }
            // Dump when the buffer fills OR when the toggle is switched off.
            // The first version only dumped on "full", so a capture ended
            // early - by unticking it or closing the game before 2048 frames
            // (~18s at 110fps) - produced NOTHING. That silently cost a
            // second capture. A partial run is still perfectly usable for a
            // period measurement, so it should never be thrown away.
            LONG fell = (!now && lastFtState);
            lastFtState = now;
            if (!g_ftDumped && g_ftCount > 0 &&
                (fell || (now && g_ftCount >= FT_CAPTURE_MAX))) {
                LONG total = g_ftCount;
                g_ftDumped = 1;
                char l[420];
                sprintf(l, "[ft] ---- dumping %ld samples (%s) ----", total,
                        fell ? "stopped early - toggle switched off" : "buffer full");
                LogLine(l);
                for (LONG i = 0; i < total; i += 20) {
                    int o = sprintf(l, "[ft] %4ld:", i);
                    for (LONG j = i; j < i + 20 && j < total; j++)
                        o += sprintf(l + o, " %ld", g_ftBuf[j]);
                    LogLine(l);
                }
                // Every series is the same frames in the same order, so they
                // line up index-for-index and can be correlated directly.
                struct { const char *tag; LONG *buf; } cols[] = {
                    { "fs", g_ftShadow }, { "fa", g_ftAllocs },
                    { "fr", g_ftReads },  { "fw", g_ftWfso },
                    { "fp", g_ftPacer },
                };
                for (int c = 0; c < 5; c++) {
                    for (LONG i = 0; i < total; i += 20) {
                        int o = sprintf(l, "[%s] %4ld:", cols[c].tag, i);
                        for (LONG j = i; j < i + 20 && j < total; j++)
                            o += sprintf(l + o, " %ld", cols[c].buf[j]);
                        LogLine(l);
                    }
                }
                LogLine("[ft] ---- end: ft=engine tick us, fs=DRAW_SHADOW cpu us, "
                        "fa=allocs, fr=file reads, fw=WFSO calls, fp=FUN_00ac3040 us ----");
            }
        }
#endif  // ENABLE_FRAMETIME_DUMP
#if ENABLE_TALK_TIMER
        ApplyTalkTimerScale();
#endif
#if ENABLE_CASCADE_HUNT
        // The 4000-entry constant dump filled up during startup and menus, so
        // it never reached actual gameplay and captured no shadow pass at all.
        // Restarting the counter every time the toggle is switched ON turns it
        // into a "capture from here" button: tick it while outdoors and the
        // window covers that moment instead of the title screen.
        {
            static LONG lastLogState = 0;
            LONG now = g_logShaderConsts;
            if (now && !lastLogState) {
                g_constSeq = 0;
                LogLine("[seq] ---- capture restarted (toggle switched on) ----");
            }
            lastLogState = now;
        }
#endif
        char line[256];

        // Frame time first, so every window in the log opens with the number
        // that actually matters.
        if (!g_mainThreadHandle && g_mainThreadId) {
            g_mainThreadHandle = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, (DWORD)g_mainThreadId);
            sprintf(line, "[watchdog] main thread tid=%ld handle=%s",
                    g_mainThreadId, g_mainThreadHandle ? "OK" : "FAILED");
            LogLine(line);
        }

        // Bulk-load detection: sustained high read volume = loading screen,
        // intermittent = traversal streaming. See BULK_LOAD_BYTES_PER_WINDOW.
        {
            static LONGLONG prevReadBytes = 0;
            static int bulkStreak = 0, quietStreak = 0;
            LONGLONG nowBytes = g_readFileBytes;
            LONGLONG delta = nowBytes - prevReadBytes;
            prevReadBytes = nowBytes;
            if (delta >= BULK_LOAD_BYTES_PER_WINDOW) { bulkStreak++; quietStreak = 0; }
            else { quietStreak++; bulkStreak = 0; }
            if (!g_bulkLoadActive && bulkStreak >= 2) {
                g_bulkLoadActive = 1;
                sprintf(line, "[probe] bulk load detected (%lldKB this window) - throttles BYPASSED",
                        delta / 1024);
                LogLine(line);
            } else if (g_bulkLoadActive && quietStreak >= 2) {
                g_bulkLoadActive = 0;
                LogLine("[probe] bulk load ended - throttles re-engaged");
            }
        }

        {
            static LONG prevCreate = 0, prevDestroy = 0;
            LONG cNow = g_createTexCount, dNow = g_texDestroyCount;
            LONG dC = cNow - prevCreate, dD = dNow - prevDestroy;
            prevCreate = cNow; prevDestroy = dNow;
            LONG kb = InterlockedExchange(&g_uploadKB, 0);
            if (dC > 0 || dD > 0 || kb > 0) {
                LONG kbMain = InterlockedExchange(&g_uploadKBMain, 0);
                sprintf(line, "[probe] tex churn: created=%ld destroyed=%ld (cum %ld/%ld) | uploaded=%ldKB (mainthread=%ldKB) this window",
                        dC, dD, cNow, dNow, kb, kbMain);
                LogLine(line);
            }
        }

        LONG clamped = InterlockedExchange(&g_deadlineClampCount, 0);
        if (clamped > 0) {
            sprintf(line, "[probe] frame-limiter deadline clamped %ld time(s) this window (aftershock prevented)", clamped);
            LogLine(line);
        }

        LONG maxFrame = InterlockedExchange(&g_maxFrameUsec, 0);
        LONG frames = InterlockedExchange(&g_frameCount, 0);
        LONG frameSum = InterlockedExchange(&g_frameSumUsec, 0);
        LONG paceDelay = InterlockedExchange(&g_readPaceDelayUsec, 0);
        LONG csWait = InterlockedExchange(&g_mainCsWaitTotalUsec, 0);
        LONG csWaitN = InterlockedExchange(&g_mainCsWaitCount, 0);
        LONG wfsoWait = InterlockedExchange(&g_mainWfsoWaitTotalUsec, 0);
        sprintf(line, "[monitor] FRAME: frames=%ld avg_usec=%.1f WORST_usec=%ld | readpace=%ld mainCS=%ldus/%ld mainWFSO=%ldus",
                frames, frames > 0 ? (double)frameSum / (double)frames : 0.0, maxFrame,
                paceDelay, csWait, csWaitN, wfsoWait);
        LogLine(line);

        LONG rateLimited = InterlockedExchange(&g_stutterRateLimited, 0);
        if (rateLimited > 0) {
            sprintf(line, "[watchdog] %ld slow frame(s) this window had NO capture attempt - "
                          "rate limit (%d/sec) saturated, records/stall totals are an UNDERCOUNT here",
                    rateLimited, STUTTER_MAX_CAPTURES_PER_SEC);
            LogLine(line);
        }

        // Frame-time distribution for this window. Percentiles come straight
        // out of the histogram, and the "below 60fps" counts are the
        // comprehensive sub-60 list - every frame over 16.67ms, regardless of
        // whether it was severe enough for the watchdog to bother capturing.
        {
            LONG hist[FRAME_HIST_BUCKETS];
            LONG total = 0;
            for (int b = 0; b < FRAME_HIST_BUCKETS; b++) {
                hist[b] = InterlockedExchange(&g_frameHist[b], 0);
                total += hist[b];
            }
            if (total > 0) {
                LONG p50=0, p90=0, p99=0, acc=0;
                LONG thr = g_stutterThresholdUsec;
                LONG n60=0, n50=0, n30=0;   // >threshold, >1.2x, >2x
                for (int b = 0; b < FRAME_HIST_BUCKETS; b++) {
                    acc += hist[b];
                    LONG usec_hi = (b + 1) * 500;
                    if (!p50 && acc * 100 >= total * 50) p50 = usec_hi;
                    if (!p90 && acc * 100 >= total * 90) p90 = usec_hi;
                    if (!p99 && acc * 100 >= total * 99) p99 = usec_hi;
                    if (usec_hi > thr) n60 += hist[b];
                    if (usec_hi > thr * 6 / 5) n50 += hist[b];
                    if (usec_hi > thr * 2) n30 += hist[b];
                }
                sprintf(line, "[frametime] n=%ld p50=%ld p90=%ld p99=%ld | thr=%ldus over=%ld (%.1f%%) over1.2x=%ld over2x=%ld",
                        total, p50, p90, p99, thr, n60, total ? n60*100.0/total : 0.0, n50, n30);
                LogLine(line);
                g_liveP50 = p50; g_liveP99 = p99; g_liveOver16 = n60;
                g_liveFrames = total; g_liveWorst = maxFrame;
            }
        }

        // Drain whatever the stutter watchdog captured. Logged as Ghidra VAs
        // (runtime address - module base + 0x00400000) so they can be pasted
        // straight into Ghidra with no per-session ASLR arithmetic.
        for (int r = 0; r < MAX_STUTTER_RECS; r++) {
            StutterRec *rec = &g_stutterRecs[r];
            if (!rec->ready) continue;

            char eipStr[160];
            DescribeAddr(rec->eip, eipStr);
            char head[512];
            sprintf(head, "[stutter] elapsed_usec=%ld EIP=%s reads=%ld/%ldKB pace_usec=%ld allocs=%ld",
                    rec->elapsedUsec, eipStr, rec->readCountInFrame, rec->readKbInFrame,
                    rec->paceUsecInFrame, rec->allocsInFrame);
            LogLine(head);

            if (rec->csWaitUsec || rec->wfsoWaitUsec) {
                const char *ownerName = "?";
                EnterCriticalSection(&g_threadNameLock);
                for (LONG j = 0; j < g_threadNameCount; j++) {
                    if (g_threadNames[j].threadId == rec->csOwnerTid) { ownerName = g_threadNames[j].name; break; }
                }
                LeaveCriticalSection(&g_threadNameLock);
                sprintf(head, "[stutter]   BLOCKED cs=%08X held_by=tid %lu (%s) for %ld us | wfso handle=%08X for %ld us",
                        rec->csPtr, rec->csOwnerTid, ownerName, rec->csWaitUsec,
                        rec->wfsoHandle, rec->wfsoWaitUsec);
                LogLine(head);
            }

            char buf[1024];
            int so = 0;
            buf[0] = 0;
            for (LONG k = 0; k < rec->stackCount && so < 800; k++) {
                char one[160];
                DescribeAddr(rec->stack[k], one);
                so += sprintf(buf + so, " %s", one);
            }
            sprintf(head, "[stutter]   ebp:%s", buf);
            LogLine(head);

            so = 0;
            buf[0] = 0;
            for (LONG k = 0; k < rec->scanCount && so < 800; k++) {
                so += sprintf(buf + so, " %08X", rec->scan[k] - g_mainModBase + 0x00400000);
            }
            sprintf(head, "[stutter]   scan:%s", buf);
            LogLine(head);

            rec->ready = 0;
        }
        for (int i = 0; i < NUM_FNS; i++) {
            HookedFunc *hf = &g_funcs[i];
            LONG count = hf->durationCount;
            LONGLONG sum = hf->durationSumCycles;

            LONG windowCount = count - g_prevDurCount[i];
            LONGLONG windowSum = sum - g_prevDurSum[i];
            g_prevDurCount[i] = count;
            g_prevDurSum[i] = sum;

            double avgUsec = (windowCount > 0 && g_cyclesPerUsec > 0.0)
                ? ((double)windowSum / (double)windowCount) / g_cyclesPerUsec
                : 0.0;
            LONG maxUsec = InterlockedExchange(&hf->maxUsec, 0);
            sprintf(line, "[monitor] %s: calls_in_window=%ld avg_duration_usec=%.2f max_duration_usec=%ld total_calls=%ld",
                    hf->name, windowCount, avgUsec, maxUsec, hf->callCount);
            LogLine(line);
        }

        // WaitForSingleObject: report TOTAL time spent blocked across all
        // threads in this window (sum, not average) - this is the
        // aggregate "how much wall-clock-equivalent time did the process
        // spend waiting on kernel sync objects" signal, most relevant to
        // the job-system-contention hypothesis.
        LONG wCount = g_wfsoCallCount;
        LONGLONG wSum = g_wfsoSumCycles;
        LONG wWindowCount = wCount - g_prevWfsoCount;
        LONGLONG wWindowSum = wSum - g_prevWfsoSum;
        g_prevWfsoCount = wCount;
        g_prevWfsoSum = wSum;
        double totalUsec = (g_cyclesPerUsec > 0.0) ? (double)wWindowSum / g_cyclesPerUsec : 0.0;
        sprintf(line, "[monitor] WaitForSingleObject: calls_in_window=%ld total_blocked_usec=%.1f total_calls=%ld",
                wWindowCount, totalUsec, wCount);
        LogLine(line);

        // Per-thread windowed breakdown - which thread(s) the blocking
        // time above actually belongs to.
        for (int i = 0; i < MAX_WFSO_THREADS; i++) {
            WfsoThreadSlot *slot = &g_wfsoThreads[i];
            if (slot->threadId == 0) continue;
            LONG tCount = slot->callCount;
            LONGLONG tSum = slot->sumCycles;
            LONG tWindowCount = tCount - g_prevWfsoThreadCount[i];
            LONGLONG tWindowSum = tSum - g_prevWfsoThreadSum[i];
            g_prevWfsoThreadCount[i] = tCount;
            g_prevWfsoThreadSum[i] = tSum;
            if (tWindowCount <= 0) continue;
            double avgUsec = (g_cyclesPerUsec > 0.0)
                ? ((double)tWindowSum / (double)tWindowCount) / g_cyclesPerUsec
                : 0.0;
            sprintf(line, "[monitor]   thread %ld: calls_in_window=%ld avg_blocked_usec=%.2f",
                    slot->threadId, tWindowCount, avgUsec);
            LogLine(line);
        }

        LogD3DWindow();

        LONG dispatchTotal = g_loaderDispatchCount;
        LONG dispatchWindow = dispatchTotal - g_prevDispatchCount;
        g_prevDispatchCount = dispatchTotal;
        sprintf(line, "[monitor] LoaderDispatch: processed_in_window=%ld total=%ld",
                dispatchWindow, dispatchTotal);
        LogLine(line);

        LONG pfAllocTotal = g_prefetchedAllocCount;
        LONGLONG pfByteTotal = g_prefetchedByteCount;
        LONG pfAllocWindow = pfAllocTotal - g_prevPrefetchedAllocCount;
        LONGLONG pfByteWindow = pfByteTotal - g_prevPrefetchedByteCount;
        g_prevPrefetchedAllocCount = pfAllocTotal;
        g_prevPrefetchedByteCount = pfByteTotal;
        sprintf(line, "[monitor] InDispatchAllocs: allocs_in_window=%ld bytes_in_window=%lld total_allocs=%ld total_bytes=%lld",
                pfAllocWindow, pfByteWindow, pfAllocTotal, pfByteTotal);
        LogLine(line);

        // Sync vs overlapped ReadFile split - if this ever shows overlapped
        // calls in real numbers, per-call duration stops meaning "blocked
        // this long" and the real wait would be hiding in a later
        // GetOverlappedResult/WaitForSingleObject instead.
        LONG syncTotal = g_readFileSyncCount, ovlTotal = g_readFileOverlappedCount;
        sprintf(line, "[monitor] ReadFile mode: sync_total=%ld overlapped_total=%ld", syncTotal, ovlTotal);
        LogLine(line);

        // Distinct compiled shaders vs total compiles vs repeats - answers
        // directly whether reproducible stutters are the SAME shader
        // recompiling (cache eviction - repeats climbing) or genuinely new
        // ones each time (permutation explosion - distinct climbs, repeats
        // stay near zero). [shader] REPEAT lines above give the per-event
        // detail; this is the running aggregate.
        sprintf(line, "[monitor] ShaderCompiles: total=%ld distinct=%ld repeats=%ld",
                g_shaderCompileTotal, g_shaderIdCount, g_shaderCompileRepeats);
        LogLine(line);

        sprintf(line, "[monitor] ShaderBudget: min_seen=%ld max_seen=%ld engaged_total=%ld",
                g_shaderBudgetMinSeen == 0x7FFFFFFF ? -1 : g_shaderBudgetMinSeen,
                g_shaderBudgetMaxSeen, g_shaderBudgetEngagedCount);
        LogLine(line);

        for (int b = 0; b < 2; b++) {
            LONG c = InterlockedExchange(&g_allocDurCount[b], 0);
            LONGLONG s = InterlockedExchange64((LONGLONG *)&g_allocDurSumUsec[b], 0);
            LONG mx = InterlockedExchange(&g_allocDurMaxUsec[b], 0);
            if (c <= 0) continue;
            sprintf(line, "[monitor] AllocatorDuration[%s]: calls=%ld avg_usec=%.2f max_usec=%ld",
                    b == 0 ? "MAIN" : "other", c, (double)s / (double)c, mx);
            LogLine(line);
        }

        // ---- v6: full allocation census, per thread ----------------------
        // The decisive number: which thread actually allocates the bulk of
        // the bytes during a chunk-load stutter. Attempt 5 only ever saw the
        // loader-dispatch slice of this.
        LONG srcCount[NUM_ALLOC_SRC] = { 0 };
        LONGLONG srcBytes[NUM_ALLOC_SRC] = { 0 };
        for (int i = 0; i < MAX_ALLOC_THREADS; i++) {
            AllocThreadSlot *slot = &g_allocThreads[i];
            if (slot->threadId == 0) continue;

            LONG wc[NUM_ALLOC_SRC];
            LONGLONG wb[NUM_ALLOC_SRC];
            LONG anyCount = 0;
            for (int s = 0; s < NUM_ALLOC_SRC; s++) {
                LONG c = slot->count[s];
                LONGLONG b = slot->bytes[s];
                wc[s] = c - g_prevAllocThreadCount[i][s];
                wb[s] = b - g_prevAllocThreadBytes[i][s];
                g_prevAllocThreadCount[i][s] = c;
                g_prevAllocThreadBytes[i][s] = b;
                anyCount += wc[s];
                srcCount[s] += wc[s];
                srcBytes[s] += wb[s];
            }
            if (anyCount <= 0) continue;

            const char *name = "?";
            EnterCriticalSection(&g_threadNameLock);
            for (LONG j = 0; j < g_threadNameCount; j++) {
                if (g_threadNames[j].threadId == (DWORD)slot->threadId) { name = g_threadNames[j].name; break; }
            }
            LeaveCriticalSection(&g_threadNameLock);

            char detail[256];
            int off = 0;
            for (int s = 0; s < NUM_ALLOC_SRC; s++) {
                if (wc[s] <= 0) continue;
                off += sprintf(detail + off, " %s=%ld/%lldB", g_allocSrcNames[s], wc[s], wb[s]);
            }
            sprintf(line, "[monitor]   alloc thread %ld (%s%s):%s",
                    slot->threadId, name,
                    (slot->threadId == g_mainThreadId) ? ",MAIN" : "",
                    detail);
            LogLine(line);
        }
        int coff = 0;
        char ctotals[256];
        for (int s = 0; s < NUM_ALLOC_SRC; s++) {
            coff += sprintf(ctotals + coff, " %s=%ld/%lldB", g_allocSrcNames[s], srcCount[s], srcBytes[s]);
        }
        sprintf(line, "[monitor] AllocCensus:%s", ctotals);
        LogLine(line);

#if ENABLE_ALLOCATOR_WARM
        // Unconditional, unlike most of these - so it has to be gated rather
        // than left to a zero counter, or it would log an all-zeros line every
        // window forever.
        LONG weTotal = g_warmEnqueued, wdTotal = g_warmDone;
        LONG wdropTotal = g_warmDropped;
        LONGLONG wbTotal = g_warmBytes;
        sprintf(line, "[monitor] Warmer: enqueued_in_window=%ld warmed_in_window=%ld bytes_in_window=%lld dropped_in_window=%ld backlog=%d",
                weTotal - g_prevWarmEnqueued, wdTotal - g_prevWarmDone,
                wbTotal - g_prevWarmBytes, wdropTotal - g_prevWarmDropped,
                g_warmCount);
        g_prevWarmEnqueued = weTotal;
        g_prevWarmDone = wdTotal;
        g_prevWarmBytes = wbTotal;
        g_prevWarmDropped = wdropTotal;
        LogLine(line);
#endif

        // Distinct concrete allocator implementations behind the named-heap
        // wrapper. Newly-seen ones are logged once each; their addresses are
        // what to feed back into Ghidra to enumerate every OTHER wrapper that
        // funnels into the same implementation (i.e. the other allocation
        // paths this hook does not currently see).
        LONG implCount = g_allocImplCount;
        for (LONG i = 0; i < implCount; i++) {
            if (g_allocImpls[i].logged) continue;
            g_allocImpls[i].logged = 1;
            sprintf(line, "[allocimpl] #%ld vtable=0x%08X allocFn=0x%08X heap=0x%08X (module_rva_allocFn=0x%08X)",
                    i,
                    (unsigned int)g_allocImpls[i].vtable,
                    (unsigned int)g_allocImpls[i].allocFn,
                    (unsigned int)g_allocImpls[i].exampleHeap,
                    (unsigned int)((unsigned char *)g_allocImpls[i].allocFn - (unsigned char *)GetModuleHandleA(NULL)));
            LogLine(line);
        }
    }
}

// ---- D3D9 method timing (v11) ---------------------------------------------
// v10 located the stall: 74% of watchdog captures had the main thread parked
// in the graphics stack, overwhelmingly inside AMDXN32.DLL (the AMD D3D9
// driver) calling a KERNELBASE wait, ~4.6 seconds of stall across the
// session. That finally explains DXVK: it replaces d3d9.dll wholesale and
// never enters AMDXN32's D3D9 path at all. The critical-section hook fired
// only twice, so lock contention - and with it the priority-inversion theory
// - is ruled out.
//
// What is NOT yet known is WHICH D3D9 call blocks, and the fix depends
// entirely on that:
//   - Present blocking  -> the GPU or the driver's queue is behind, and the
//                          long frame is a symptom rather than a cause.
//   - a Create*/Lock*   -> a CPU-side driver stall on resource creation or
//     blocking              on locking a resource the GPU still owns, which
//                          is directly pace-able the way ReadFile was.
//
// Mechanism: COM methods on x86 are __stdcall with `this` as the first stack
// argument and a callee-cleaned stack of unknown size, so the arg-count-
// agnostic return-address hijack used throughout this investigation is again
// the right tool. Each hooked slot gets a 10-byte thunk (`mov eax, <id>` /
// `jmp GenericD3DEntry`) so one shared entry/return pair covers every method.
//
// Deliberately NOT hooking the ultra-hot state setters (SetTexture,
// SetRenderState, SetVertexShaderConstantF, ...): they cannot block, and at
// hundreds of thousands of calls per second the hook overhead would itself
// perturb frame timing. Only methods that can plausibly block are hooked.
#include <d3d9.h>

#define MAX_D3D_SLOTS 64

typedef struct {
    void **entry;      // address of the vtable slot itself
    void *orig;        // original function pointer
    const char *name;
    volatile LONG count;
    volatile LONG sumUsec;
    volatile LONG maxUsec;
} D3DSlot;

static D3DSlot g_d3dSlots[MAX_D3D_SLOTS];
// Flat array of original function pointers, kept separate from D3DSlot purely
// so the dispatch thunk can index it with an encodable x86 scale (*4);
// sizeof(D3DSlot) is 24, which no scaled-index addressing mode can express.
static void *g_d3dOrig[MAX_D3D_SLOTS];
static volatile LONG g_d3dSlotCount = 0;
static unsigned char *g_d3dThunks = NULL;
static LONG g_prevD3dCount[MAX_D3D_SLOTS];
static LONG g_prevD3dSum[MAX_D3D_SLOTS];

// Two independent hook layers can target the same vtable slot: the generic
// timing thunks (installed from the throwaway probe device) and the real
// replacement functions (installed on the game's device). On Microsoft's
// d3d9.dll those are always different vtables, so they never meet. On a
// single-class wrapper they are the SAME vtable, and whichever layer runs
// second would capture the other's hook as "the original" - producing a hook
// that calls a hook, with a return-address hijack in the middle.
//
// This resolves a slot value that is really one of our thunks back to the
// function the thunk was built to call. Layout is fixed by HookVtableSlotEx:
// 16-byte stride, `B8 <id:32>` then `E9 <rel32>`, so the id is recoverable
// from the thunk body itself rather than by searching the slot table.
static void *ResolveOrigSlot(void *cur)
{
    if (!cur || !g_d3dThunks) return cur;
    unsigned char *p = (unsigned char *)cur;
    if (p < g_d3dThunks || p >= g_d3dThunks + MAX_D3D_SLOTS * 16) return cur;
    if ((size_t)(p - g_d3dThunks) % 16 != 0 || p[0] != 0xB8 || p[5] != 0xE9) return cur;
    LONG id = *(LONG *)(p + 1);
    if (id < 0 || id >= MAX_D3D_SLOTS || !g_d3dOrig[id]) return cur;
    return g_d3dOrig[id];
}

#define D3D_STACK_DEPTH 16
typedef struct {
    void *trueRetAddr;
    unsigned __int64 entryTsc;
    LONG slotId;
    // Copied out of the caller's stack AT ENTRY, not referenced by pointer:
    // by the time OnD3DReturn runs, the real callee has already executed its
    // own `ret N` (STDMETHODCALLTYPE cleans its own args), so the original
    // stack slots are stale/reused - only values copied into our own memory
    // early are safe to read later. Generic slot-agnostic capture (this +
    // first 4 explicit args); each hooked method interprets what it needs.
    DWORD argThis, arg1, arg2, arg3, arg4;
} D3DRetFrame;
static LONG g_lockRectSlotId = -1;
static LONG g_presentSlotId = -1;
static volatile LONG g_presentParamsLogged = 0;
static volatile LONG g_lockRectDiscardInjected = 0;
// Diagnostics so the next run can confirm the seen-table capacity fix
// empirically rather than leaving it to inference again. Declared here rather
// than beside the table itself because the injection site above uses them.
static volatile LONG g_seenTexClears = 0;
static volatile LONG g_lockRectEligible = 0;   // dynamic + fullrect + unflagged
static volatile LONG g_lockRectFirstSeen = 0;  // eligible but first sighting -> skipped
static volatile LONG g_lockRectMipSkipped = 0; // eligible but mipmapped -> never safe to discard
static LONG g_prevLockRectDiscardInjected = 0;
typedef struct {
    int top;
    D3DRetFrame frames[D3D_STACK_DEPTH];
} D3DThreadStack;
static DWORD g_d3dStackTls;

static D3DThreadStack *GetD3DStack(void)
{
    D3DThreadStack *ts = (D3DThreadStack *)TlsGetValue(g_d3dStackTls);
    if (!ts) {
        ts = (D3DThreadStack *)calloc(1, sizeof(D3DThreadStack));
        if (ts) TlsSetValue(g_d3dStackTls, ts);
    }
    return ts;
}

// rawStack points at the untouched original stack frame at the moment of
// entry: rawStack[0]=return address, [1]=this, [2..5]=first four explicit
// args. Valid only during this call (see D3DRetFrame comment), so everything
// needed later is copied out immediately.
static int __cdecl OnD3DEnter(void *trueRetAddr, LONG slotId, DWORD *rawStack)
{
    D3DThreadStack *ts = GetD3DStack();
    if (!ts || ts->top >= D3D_STACK_DEPTH) return 0;
    D3DRetFrame *f = &ts->frames[ts->top];
    f->trueRetAddr = trueRetAddr;
    f->entryTsc = __rdtsc();
    f->slotId = slotId;
    f->argThis = rawStack[1];
    f->arg1 = rawStack[2];
    f->arg2 = rawStack[3];
    f->arg3 = rawStack[4];
    f->arg4 = rawStack[5];
    ts->top++;

    // NOTE: a Present-based diagnostic used to live here. Confirmed dead:
    // slotId never equals g_presentSlotId in practice, because the device
    // vtable this thunk patches comes from a throwaway PROBE device and is
    // not shared with the game's real one (see the comment beside
    // g_stagingDevice's first assignment for why, and the replacement
    // diagnostic that actually works).

    // Fix attempt 13: v12 found the actual blocking call. All six slow
    // Texture::LockRect captures this session shared one exact signature -
    // D3DUSAGE_DYNAMIC, full-resource lock (pRect==NULL), Flags==0 - the
    // textbook D3D9 stall: without D3DLOCK_DISCARD or D3DLOCK_NOOVERWRITE,
    // the driver has no choice but to fully sync with the GPU before handing
    // back a write pointer, blocking the CPU until the GPU catches up.
    // DYNAMIC resources exist specifically to be used WITH one of those
    // flags; locking one without either is the anti-pattern this depends on.
    //
    // rawStack is a live pointer into the real call's stack, not a copy -
    // writing rawStack[5] here changes the Flags argument the real LockRect
    // is about to receive when GenericD3DEntry jumps into it, no additional
    // asm needed.
    //
    // Gated deliberately narrow, matching exactly what was observed and
    // nothing more:
    //   - only this slot (Texture::LockRect)
    //   - only D3DUSAGE_DYNAMIC resources (D3DLOCK_DISCARD is invalid/
    //     undefined on anything else)
    //   - only when pRect == NULL (a full-resource lock) - every slow
    //     capture was fullRect=1; a partial lock could be relying on
    //     previously-written regions surviving, which DISCARD would
    //     invalidate wholesale and cause visual corruption. Leaving partial
    //     locks completely untouched means this can only ever affect the
    //     exact pattern already confirmed to stall, never a code path that
    //     hasn't been observed.
    //   - only when neither flag is already set (nothing to fix otherwise)
    //
    // Default DISABLED (g_discardFixEnabled starts at 0): user confirmed via
    // reboot-with-config-saved that this fix IS the source of black/
    // transparent rendering (player face, distant geometry), and that
    // toggling it on mid-session (after boot) does NOT reproduce it - which
    // pins the corruption specifically to a texture's FIRST-time population,
    // concentrated at boot when nearly everything streams in at once.
    // DISCARD only affects future lock calls, so enabling it mid-session
    // with nothing new streaming in never exercises the vulnerable path.
    //
    // Refinement: track each distinct texture pointer seen through this
    // check and only inject DISCARD from its SECOND qualifying lock onward,
    // never its first. The first lock is presumably part of the vulnerable
    // initial-population sequence (however many steps that turns out to
    // be); by the second lock, that sequence has already completed at least
    // once without our interference. This also lines up with the original
    // evidence: the measured stalls were on a texture locked repeatedly
    // every frame (a lookup/curve table), not a one-time load - by the time
    // it reaches "steady state," it has been locked many times already, so
    // requiring just one prior sighting is a conservative floor, not a
    // tight fit to a specific count.
    // RETIRED: a D3DLOCK_NOOVERWRITE alternative to DISCARD was tried and
    // tested here (same injection site, same gating), on the reasoning that
    // NOOVERWRITE avoids the same GPU-sync wait without discarding the
    // buffer, so - unlike DISCARD - it should need no first-lock skip.
    // User testing found real graphical glitches in the main menu AND a
    // crash once level load completed. Confirms the flagged risk was real:
    // NOOVERWRITE is a promise the driver trusts rather than one it
    // enforces, and this resource's actual GPU usage evidently does
    // overlap with the CPU write closely enough to cause a genuine
    // race - a worse failure class than DISCARD's corruption (recoverable
    // by toggling off; this was not, since it could crash before a toggle
    // was even possible). Removed entirely rather than left behind a flag -
    // no hotkey, no checkbox, no config key can re-enable this. See
    // PROGRESS.md for the historical record if revisited.

    // NOTE: the DISCARD injection that used to live here has moved into
    // HookedTexLockRect. Texture::LockRect no longer routes through this
    // generic thunk at all - it has a dedicated replacement function so the
    // staging path can decline to call the original - so a slotId test for it
    // here would now never fire.
    return 1;
}

// Texture::LockRect logged 2.64s of total stall across the session, worst
// single call 343ms - the largest cost found anywhere in the D3D9 layer by a
// wide margin (everything else, including Present, never even crossed the
// reporting threshold). The mechanism that would explain everything else
// found so far (main thread parked inside AMDXN32.DLL calling a KERNELBASE
// wait, invisible to the EnterCriticalSection hook because it's a lock
// INSIDE the driver, not a kernel32 one the game calls) is
// D3DCREATE_MULTITHREADED: the reference HD-texture mod's own source
// confirms "FF13 creates its device with D3DCREATE_MULTITHREADED", which
// makes the AMD driver serialize ALL D3D9 calls from ALL threads behind one
// internal lock. If a Loader/decompression thread calls LockRect on newly-
// streamed texture data without D3DLOCK_DISCARD (forcing the driver to wait
// for the GPU to finish with that resource before handing back a pointer),
// every OTHER thread - including the main thread trying to Present or draw -
// would stall behind the SAME internal lock for the same duration. That is
// exactly the v10 signature.
// This is a single, cheap, targeted check to confirm or kill that
// mechanism directly: only on the rare slow path (>5ms) does this touch
// LogLine at all, so the 13,750 fast LockRect calls this session pay
// nothing beyond the branch itself.
// Tracks distinct texture pointers seen through the DISCARD-injection check
// below, so it can skip a texture's first qualifying lock and only inject
// DISCARD from its second onward (see the comment at the injection site for
// why).
// Keyed on (texture pointer, mip level) TOGETHER, not the pointer alone.
// Bug caught by the user's own observation: moving the camera closer made
// Lightning's textures/hair reappear - a distance/LOD signature, meaning
// different MIP LEVELS of the same texture are involved, not different
// textures. All mip levels of one texture share the SAME IDirect3DTexture9
// pointer, distinguished only by the Level argument to LockRect - keying on
// the pointer alone meant locking level 0 (used up close) marked the WHOLE
// texture "seen," so level 3's (used at a distance) true first-ever lock
// got incorrectly treated as already-safe and had DISCARD applied to it,
// corrupting exactly the far-distance mip while the near one stayed fine.
// Matches the original evidence too: the stalling captures showed BOTH a
// 64x64 and a 128x128 mip level of the same DXT5 texture.
//
// CAPACITY BUG (found from the town-run log, fixed here). The table above was
// a 512-entry linear array whose insert was guarded by
// `if (!found && n < MAX_TRACKED_TEXTURES)`. Once 512 distinct (tex,level)
// pairs had been seen, that guard stopped admitting new keys - and because
// "seen" is the ONLY thing that authorizes the injection, every texture first
// locked after the table filled was permanently classified as never-seen and
// could never be given DISCARD. The fix silently stopped working instead of
// failing loudly.
// The log confirms it exactly: all 3 DISCARD injections of a 258-second run
// happened in the first 7.5% of the session, then zero for the remaining
// 92.5% - and all 15 SLOW LockRect stalls (including one 386ms lock on the
// main thread) happened AFTER injections stopped. The fix died early and the
// stalls it exists to prevent all landed afterwards.
// Replaced with an open-addressed hash set, which also removes the O(n)
// linear scan that ran under a critical section on every single LockRect
// (up to 290 calls per window x 512 comparisons).
// On high load factor the table is CLEARED rather than allowed to fill.
// Clearing is the safe direction: a forgotten texture looks new again, so it
// merely skips one DISCARD before being re-learned. The opposite error -
// treating a genuinely-new texture as seen - is the one that corrupts, and
// clearing can never cause it.
#define SEEN_TEX_SLOTS 16384          // power of two
#define SEEN_TEX_MAX_LOAD 12288       // 75% - clear and relearn past this
typedef struct {
    DWORD texPtr;                     // 0 = empty slot
    UINT level;
    // Shape of the resource this entry was recorded against. A destroyed
    // texture can be replaced by a NEW one at the same address, and a stale
    // "seen" entry would then wrongly authorize DISCARD on that new
    // texture's genuine first lock - the exact corruption this whole design
    // exists to avoid. The old 512-entry table was accidentally shielded
    // from this because it filled up and stopped matching anything; now that
    // the table actually works, the exposure is real and lasts all session.
    // GetLevelDesc is already called at the injection site, so validating
    // the shape costs nothing extra. Same address + same level but different
    // dimensions or format means the pointer was recycled.
    UINT width, height;
    DWORD format;
} SeenTextureKey;
static SeenTextureKey g_seenTextures[SEEN_TEX_SLOTS];
static LONG g_seenTextureCount = 0;
static CRITICAL_SECTION g_seenTextureLock;
static volatile LONG g_seenTextureLockState = 0; // 0=not started, 1=initializing, 2=ready
static unsigned SeenTexHash(DWORD texPtr, UINT level)
{
    // Texture pointers are allocator-aligned, so the low bits carry almost no
    // entropy - mix before masking or everything piles into a few buckets.
    unsigned h = (unsigned)texPtr ^ ((unsigned)level * 0x9E3779B1u);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h & (SEEN_TEX_SLOTS - 1);
}

static int HasSeenTextureBefore(DWORD texPtr, UINT level, UINT width, UINT height, DWORD format)
{
    // Lazy one-time init rather than threading this through the various
    // install functions - cheap, and correctness doesn't depend on WHEN it
    // happens, only that it happens before first use. Three-state instead
    // of a plain bool: if a second thread hits this while the first is
    // still inside InitializeCriticalSection, a plain "already claimed"
    // flag would let it straight through to EnterCriticalSection on a
    // not-yet-initialized object - undefined behavior. Spinning until the
    // winner marks it READY closes that window.
    if (InterlockedCompareExchange(&g_seenTextureLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_seenTextureLock);
        g_seenTextureLockState = 2;
    } else {
        while (g_seenTextureLockState != 2) Sleep(0);
    }
    if (texPtr == 0) return 0;  // 0 is the empty-slot marker, never a real key

    EnterCriticalSection(&g_seenTextureLock);

    unsigned idx = SeenTexHash(texPtr, level);
    int found = 0;
    int present = 0;   // an entry for this key already exists - do not insert
    for (unsigned probe = 0; probe < SEEN_TEX_SLOTS; probe++) {
        SeenTextureKey *e = &g_seenTextures[idx];
        if (e->texPtr == 0) break;                 // empty slot: key is absent
        if (e->texPtr == texPtr && e->level == level) {
            present = 1;
            if (e->width == width && e->height == height && e->format == format) {
                found = 1;
            } else {
                // Pointer recycled onto a different resource. Re-point the
                // entry at the new shape and report NOT seen, so this
                // texture's real first lock is left alone.
                e->width = width; e->height = height; e->format = format;
                found = 0;
            }
            break;
        }
        idx = (idx + 1) & (SEEN_TEX_SLOTS - 1);
    }

    if (!present) {
        if (g_seenTextureCount >= SEEN_TEX_MAX_LOAD) {
            // Full enough that probe chains get long. Drop everything and
            // relearn - see the note above on why this direction is safe.
            memset(g_seenTextures, 0, sizeof(g_seenTextures));
            g_seenTextureCount = 0;
            InterlockedIncrement(&g_seenTexClears);
            idx = SeenTexHash(texPtr, level);
        }
        // idx is either the empty slot the search stopped on, or a fresh
        // hash after a clear; walk to the first empty slot from there.
        while (g_seenTextures[idx].texPtr != 0) idx = (idx + 1) & (SEEN_TEX_SLOTS - 1);
        g_seenTextures[idx].texPtr = texPtr;
        g_seenTextures[idx].level = level;
        g_seenTextures[idx].width = width;
        g_seenTextures[idx].height = height;
        g_seenTextures[idx].format = format;
        g_seenTextureCount++;
    }

    LeaveCriticalSection(&g_seenTextureLock);
    return found;
}

// Used by the texture pool (further down) when handing a released texture
// back out for reuse: from this table's own perspective that is exactly a
// "pointer recycled onto a different resource" event, the same case the
// shape-mismatch branch above already handles safely (forces the next
// lookup to report NOT seen, so DISCARD is correctly skipped again on the
// reused texture's first genuine lock in its new life). Forcing width to a
// value no real texture can have reuses that exact, already-proven-safe
// path instead of adding a second way to reach the same state.
static void InvalidateSeenTextureForReuse(DWORD texPtr, UINT levels)
{
    if (InterlockedCompareExchange(&g_seenTextureLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_seenTextureLock);
        g_seenTextureLockState = 2;
    } else {
        while (g_seenTextureLockState != 2) Sleep(0);
    }
    EnterCriticalSection(&g_seenTextureLock);
    for (UINT level = 0; level < levels; level++) {
        unsigned idx = SeenTexHash(texPtr, level);
        for (unsigned probe = 0; probe < SEEN_TEX_SLOTS; probe++) {
            SeenTextureKey *e = &g_seenTextures[idx];
            if (e->texPtr == 0) break;
            if (e->texPtr == texPtr && e->level == level) {
                e->width = 0xFFFFFFFFu;   // no real texture has this width
                break;
            }
            idx = (idx + 1) & (SEEN_TEX_SLOTS - 1);
        }
    }
    LeaveCriticalSection(&g_seenTextureLock);
}

static void *__cdecl OnD3DReturn(void)
{
    D3DThreadStack *ts = GetD3DStack();
    ts->top--;
    D3DRetFrame f = ts->frames[ts->top];
    if (g_cyclesPerUsec > 0.0) {
        LONG usec = (LONG)((double)(__rdtsc() - f.entryTsc) / g_cyclesPerUsec);
        D3DSlot *s = &g_d3dSlots[f.slotId];
        InterlockedIncrement(&s->count);
        InterlockedExchangeAdd(&s->sumUsec, usec);
        if (usec > s->maxUsec) s->maxUsec = usec;

        if (usec > 5000) {
            DWORD tid = GetCurrentThreadId();
            const char *name = (tid == (DWORD)g_mainThreadId) ? "MAIN" : "?";
            if (name[0] == '?') {
                EnterCriticalSection(&g_threadNameLock);
                for (LONG j = 0; j < g_threadNameCount; j++) {
                    if (g_threadNames[j].threadId == tid) { name = g_threadNames[j].name; break; }
                }
                LeaveCriticalSection(&g_threadNameLock);
            }
            char line[256];
            sprintf(line, "[d3d9] SLOW %s: %ld us on thread %lu (%s)",
                    s->name, usec, tid, name);
            LogLine(line);

            // LockRect specifically: decode which texture, and with what
            // flags. D3DLOCK_DISCARD/NOOVERWRITE tell the driver it never
            // needs to wait for the GPU to finish with this resource before
            // handing back a CPU pointer - locking WITHOUT either forces a
            // full sync if the GPU still has the resource in flight, which is
            // one of the most well-known D3D9 stall patterns and exactly
            // consistent with a duration in the hundreds of milliseconds.
            // LockRect slow-path detail logging also moved into
            // HookedTexLockRect along with the rest of that method.
        }
    }
    return f.trueRetAddr;
}

// Preserves EAX across the bookkeeping call - it carries the HRESULT that
// every one of these methods returns.
__declspec(naked) void OnD3DReturnStub(void)
{
    __asm {
        push eax
        call OnD3DReturn
        mov ecx, eax
        pop eax
        jmp ecx
    }
}

__declspec(naked) void GenericD3DEntry(void)
{
    __asm {
        ; On entry, esp points at the UNTOUCHED original stack frame:
        ; [esp]=return address, [esp+4]=this, [esp+8..]=explicit args. Save
        ; that address in edx before disturbing anything, so OnD3DEnter can
        ; read it directly (works for any method's argument layout, since it
        ; is just raw stack, not yet reinterpreted).
        lea edx, [esp]
        push eax                       ; save slot id
        push edx                       ; arg3: raw stack pointer (original esp)
        push eax                       ; arg2: slot id
        push dword ptr [esp + 12]      ; arg1: true return address (*rawStack)
        call OnD3DEnter
        add esp, 12
        test eax, eax
        jz skip_d3d_hijack
        mov dword ptr [esp + 4], offset OnD3DReturnStub
    skip_d3d_hijack:
        pop eax                        ; restore slot id
        jmp dword ptr g_d3dOrig[eax * 4]
    }
}

// `replacement` non-NULL installs a real C function in the vtable slot
// INSTEAD of the timing thunk, so the hook can decide not to call the
// original at all. The generic thunk always ends in `jmp orig`, which is
// fine for measuring but cannot substitute behaviour - and substituting
// behaviour is exactly what the staging-upload path below has to do.
static int HookVtableSlotEx(void **vtable, int slot, const char *name, void *replacement)
{
    LONG id = g_d3dSlotCount;
    if (id >= MAX_D3D_SLOTS || !vtable) return -1;

    void **entry = &vtable[slot];
    g_d3dSlots[id].entry = entry;
    g_d3dSlots[id].orig = *entry;
    g_d3dOrig[id] = *entry;
    g_d3dSlots[id].name = name;

    if (replacement) {
        DWORD oldProt;
        if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProt)) return -1;
        *entry = replacement;
        VirtualProtect(entry, sizeof(void *), oldProt, &oldProt);
        g_d3dSlotCount = id + 1;
        return (int)id;
    }

    unsigned char *thunk = g_d3dThunks + id * 16;
    thunk[0] = 0xB8;                                  // mov eax, imm32
    *(LONG *)(thunk + 1) = id;
    thunk[5] = 0xE9;                                  // jmp rel32
    *(int *)(thunk + 6) = (int)(void *)GenericD3DEntry - (int)(thunk + 10);

    DWORD oldProtect;
    if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect)) return -1;
    *entry = thunk;
    VirtualProtect(entry, sizeof(void *), oldProtect, &oldProtect);

    g_d3dSlotCount = id + 1;
    return (int)id;
}

static int HookVtableSlot(void **vtable, int slot, const char *name)
{
    return HookVtableSlotEx(vtable, slot, name, NULL);
}

// ---- Staging-upload path (the real fix for the per-mip streaming stall) ---
//
// Everything measured so far says the dominant stutter is the main thread
// blocked inside AMDXN32.DLL waiting for the GPU to release a texture the
// engine wants to write. DISCARD is the usual answer, but it cannot work
// here: the engine streams INDIVIDUAL mip levels on demand, and DISCARD
// invalidates the whole resource, destroying the levels it is not writing.
// Measured directly - discarding every level cut per-call cost 168us -> 113us
// but corrupted distant LOD; restricting to level 0 was visually clean and
// bought nothing (166us) because only ~5.5% of mip locks are level 0.
//
// So instead of trying to make the stalling lock cheap, this removes the
// stalling lock entirely. On an eligible LockRect the engine is handed a
// D3DPOOL_SYSTEMMEM staging surface - plain CPU memory the GPU has never
// seen, so locking it can never wait on anything - and the real DEFAULT-pool
// surface is never locked at all. On UnlockRect the data is transferred with
// UpdateSurface, which the driver schedules in GPU command order instead of
// blocking the CPU until the GPU catches up.
//
// This is a genuine behaviour change rather than a flag tweak, so: default
// OFF behind its own toggle (F6 / StagingUpload), and every failure path
// falls back to the original LockRect rather than failing the call.
static volatile LONG g_stagingHits = 0, g_stagingFallback = 0, g_stagingUpdateFail = 0;
static volatile LONG g_stagingCreated = 0;
static volatile LONG g_stagingRecycled = 0;
static LONG g_stagingBytes = 0;

typedef struct {
    UINT w, h;
    D3DFORMAT fmt;
    IDirect3DTexture9 *tex;   // SYSTEMMEM
    UINT level;               // which mip level of tex is the (w,h) surface
    int inUse;
} StagingTex;

typedef struct {
    IDirect3DTexture9 *tex;   // the real DEFAULT-pool texture
    UINT level;
    LONG stagingIdx;
} InFlightLock;

#define MAX_STAGING 512
#define MAX_INFLIGHT 64
static StagingTex g_staging[MAX_STAGING];
static LONG g_stagingCount = 0;
static InFlightLock g_inflight[MAX_INFLIGHT];
static LONG g_inflightCount = 0;
static CRITICAL_SECTION g_stagingLock;
static volatile LONG g_stagingLockState = 0;
static IDirect3DDevice9 *g_stagingDevice = NULL;

static void EnsureStagingInit(void)
{
    if (InterlockedCompareExchange(&g_stagingLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_stagingLock);
        g_stagingLockState = 2;
    } else {
        while (g_stagingLockState != 2) Sleep(0);
    }
}

// Block-compressed formats cannot exist as a standalone texture smaller than
// one 4x4 block. Confirmed empirically: every surface-staging rejection was a
// tiny DXT1/DXT5 mip (1x1, 2x2, 2x4, 4x2...), CreateTexture failed on all of
// them, and the pool sat stuck at 248 entries because no new ones could be
// created. Those tiny mips were 84% of all surface locks, so this single
// limitation was defeating almost the whole surface fix.
//
// Fix: allocate a legal 4x4-aligned BASE and mip down to the requested size,
// then use the level whose dimensions match EXACTLY. Scaling both axes by the
// same 2^k preserves aspect, so level k is precisely (w,h) - which matters
// because UpdateSurface requires source and destination dimensions to agree.
//   1x1 -> base 4x4, 3 levels, use level 2
//   2x4 -> base 4x8, 2 levels, use level 1
static int IsBlockCompressed(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 ||
           f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

// Pitch is bytes per row of TEXELS for uncompressed formats, but bytes per
// row of 4x4 BLOCKS for compressed ones - so multiplying by Height
// overcounts DXT by 4x. The first measurement run did exactly that and
// reported ~3.1GB where the true figure is far lower; ratios were unaffected
// (same bias both sides) but absolute volume was not. Corrected here.
static LONG UploadKBFor(UINT pitch, UINT height, D3DFORMAT fmt)
{
    UINT rows = IsBlockCompressed(fmt) ? ((height + 3) / 4) : height;
    return (LONG)(((unsigned __int64)pitch * rows) / 1024);
}

// Returns which mip level of the created texture corresponds to (w,h).
//
// The constraint is DIVISIBLE by 4, not merely >= 4. First version used ">= 4"
// and still got rejections for 14x14, 7x7, 3x3, 14x28 - all mips of
// non-power-of-two DXT textures (a 28x28 chain chain mips 14 -> 7 -> 3), which
// clear ">= 4" but are not block-aligned. Doubling until both axes are
// multiples of 4 fixes those: 7x7 -> 28x28 (28 = 4*7) at level 2, 3x3 ->
// 12x12 at level 2, 14x14 -> 28x28 at level 1.
static UINT StagingMipPlan(UINT w, UINT h, UINT *baseW, UINT *baseH, UINT *levels)
{
    UINT k = 0;
    while (k < 12) {
        UINT bw = w << k, bh = h << k;
        if (bw >= 4 && bh >= 4 && (bw & 3) == 0 && (bh & 3) == 0) break;
        k++;
    }
    *baseW = w << k; *baseH = h << k; *levels = k + 1;
    return k;
}

// Caller must hold g_stagingLock.
static LONG AcquireStaging(UINT w, UINT h, D3DFORMAT fmt)
{
    for (LONG i = 0; i < g_stagingCount; i++) {
        StagingTex *s = &g_staging[i];
        if (!s->inUse && s->w == w && s->h == h && s->fmt == fmt) { s->inUse = 1; return i; }
    }

    // Pool full: RECYCLE a free entry instead of giving up.
    //
    // This pool used to only ever grow to MAX_STAGING and then return -1
    // forever, which is fine while one caller uses a handful of sizes - but
    // adding Surface::LockRect staging pushed it to 192/192 and the failure
    // was silent and severe: the texture path's fallback tripled (7,264 ->
    // 19,263) because surfaces had consumed every slot, and a 423ms stall
    // reappeared. Falling back is "safe" but it silently un-fixes whichever
    // caller loses the race for slots.
    //
    // Recycling always succeeds in practice: at most MAX_INFLIGHT (64)
    // entries can be in use simultaneously, well under the pool size, so a
    // free entry always exists. Releasing and recreating costs one
    // allocation, paid only when a genuinely new (w,h,fmt) shows up.
    if (g_stagingCount >= MAX_STAGING && g_stagingDevice) {
        for (LONG i = 0; i < g_stagingCount; i++) {
            StagingTex *s = &g_staging[i];
            if (s->inUse) continue;
            IDirect3DTexture9 *repl = NULL;
            UINT rlevel = 0;
            if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, w, h, 1, 0, fmt,
                                                      D3DPOOL_SYSTEMMEM, &repl, NULL)) || !repl) {
                if (IsBlockCompressed(fmt)) {
                    UINT bw, bh, lv;
                    rlevel = StagingMipPlan(w, h, &bw, &bh, &lv);
                    if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, bw, bh, lv, 0, fmt,
                                                              D3DPOOL_SYSTEMMEM, &repl, NULL)) || !repl) {
                        return -1;
                    }
                } else {
                    return -1;   // fall back rather than lose the old entry
                }
            }
            if (s->tex) IDirect3DTexture9_Release(s->tex);
            s->tex = repl; s->w = w; s->h = h; s->fmt = fmt; s->level = rlevel; s->inUse = 1;
            InterlockedIncrement(&g_stagingRecycled);
            return i;
        }
    }

    if (g_stagingCount >= MAX_STAGING || !g_stagingDevice) return -1;
    IDirect3DTexture9 *t = NULL;
    UINT useLevel = 0;
    // SYSTEMMEM + no usage flags is the only combination UpdateSurface will
    // accept as a source. Single level normally: each mip level of the real
    // texture is staged independently, which is how the engine streams them.
    if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, w, h, 1, 0, fmt,
                                              D3DPOOL_SYSTEMMEM, &t, NULL)) || !t) {
        // Block-compressed formats below one 4x4 block cannot be created
        // standalone - allocate a legal base and mip down to the exact size.
        if (!IsBlockCompressed(fmt)) return -1;
        UINT bw, bh, lv;
        useLevel = StagingMipPlan(w, h, &bw, &bh, &lv);
        if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, bw, bh, lv, 0, fmt,
                                                  D3DPOOL_SYSTEMMEM, &t, NULL)) || !t) {
            return -1;
        }
    }
    LONG i = g_stagingCount;
    g_staging[i].w = w; g_staging[i].h = h; g_staging[i].fmt = fmt;
    g_staging[i].tex = t; g_staging[i].level = useLevel; g_staging[i].inUse = 1;
    g_stagingCount = i + 1;
    InterlockedIncrement(&g_stagingCreated);
    return i;
}


// ---- Deferred GPU upload queue -------------------------------------------
// Holds ownership of BOTH surface refs and the staging pool slot until the
// copy is actually issued - releasing any of them early would either free a
// surface the GPU still needs or hand the staging texture to another upload
// while its data is still pending.
//
// Capacity is deliberately well under MAX_STAGING: every queued entry keeps
// a staging slot reserved, so a queue as large as the pool would starve
// AcquireStaging and force the (correct but slower) fallback path. 192 of
// 512 leaves ample headroom.
#define MAX_PENDING_UPLOADS 192
typedef struct {
    IDirect3DSurface9 *src;
    IDirect3DSurface9 *dst;
    LONG stagingIdx;
} PendingUpload;
static PendingUpload g_uploadQueue[MAX_PENDING_UPLOADS];
static LONG g_uploadHead = 0;    // ring: next to drain
static LONG g_uploadCount = 0;
static CRITICAL_SECTION g_uploadQueueLock;
static volatile LONG g_uploadQueueLockState = 0;
static volatile LONG g_deferQueued = 0, g_deferIssued = 0, g_deferOverflow = 0;
static volatile LONG g_uploadQueueDepth = 0;

static void EnsureUploadQueueInit(void)
{
    if (InterlockedCompareExchange(&g_uploadQueueLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_uploadQueueLock);
        g_uploadQueueLockState = 2;
    } else {
        while (g_uploadQueueLockState != 2) Sleep(0);
    }
}

// Issues the copy and releases everything. Used both for the immediate path
// and when draining the queue, so ownership rules live in exactly one place.
static void DoUploadNow(IDirect3DSurface9 *src, IDirect3DSurface9 *dst, LONG stagingIdx)
{
    HRESULT hr = E_FAIL;
    if (g_stagingDevice && src && dst) {
        hr = IDirect3DDevice9_UpdateSurface(g_stagingDevice, src, NULL, dst, NULL);
    }
    if (FAILED(hr)) InterlockedIncrement(&g_stagingUpdateFail);
    if (src) IDirect3DSurface9_Release(src);
    if (dst) IDirect3DSurface9_Release(dst);
    if (stagingIdx >= 0) {
        EnterCriticalSection(&g_stagingLock);
        g_staging[stagingIdx].inUse = 0;
        LeaveCriticalSection(&g_stagingLock);
    }
}

// Takes ownership of src/dst refs and the staging slot.
static void SubmitStagedUpload(IDirect3DSurface9 *src, IDirect3DSurface9 *dst, LONG stagingIdx)
{
#if ENABLE_DEFER_UPLOADS
    if (g_deferUploadsEnabled) {
        EnsureUploadQueueInit();
        EnterCriticalSection(&g_uploadQueueLock);
        if (g_uploadCount < MAX_PENDING_UPLOADS) {
            LONG slot = (g_uploadHead + g_uploadCount) % MAX_PENDING_UPLOADS;
            g_uploadQueue[slot].src = src;
            g_uploadQueue[slot].dst = dst;
            g_uploadQueue[slot].stagingIdx = stagingIdx;
            g_uploadCount++;
            g_uploadQueueDepth = g_uploadCount;
            LeaveCriticalSection(&g_uploadQueueLock);
            InterlockedIncrement(&g_deferQueued);
            return;
        }
        LeaveCriticalSection(&g_uploadQueueLock);
        // Queue full - issue immediately rather than drop. Falling back
        // costs a stall but never loses texture data.
        InterlockedIncrement(&g_deferOverflow);
    }
#endif  // ENABLE_DEFER_UPLOADS
    DoUploadNow(src, dst, stagingIdx);
}

// Called once per frame from the main thread (the ac3040 frame hook), which
// is also where the engine itself issues D3D work - so the copies land in
// the same command stream, just spread across frames instead of bursting.
static void DrainStagedUploads(void)
{
    if (g_uploadQueueLockState != 2) return;
    LONG budget = g_deferPerFrame;
    if (budget < 1) budget = 1;
    for (LONG i = 0; i < budget; i++) {
        PendingUpload p;
        EnterCriticalSection(&g_uploadQueueLock);
        if (g_uploadCount == 0) { LeaveCriticalSection(&g_uploadQueueLock); return; }
        p = g_uploadQueue[g_uploadHead];
        g_uploadHead = (g_uploadHead + 1) % MAX_PENDING_UPLOADS;
        g_uploadCount--;
        g_uploadQueueDepth = g_uploadCount;
        LeaveCriticalSection(&g_uploadQueueLock);
        DoUploadNow(p.src, p.dst, p.stagingIdx);
        InterlockedIncrement(&g_deferIssued);
    }
}

typedef HRESULT (STDMETHODCALLTYPE *LockRectFn)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *UnlockRectFn)(IDirect3DTexture9 *, UINT);
static LockRectFn g_origLockRect = NULL;
static UnlockRectFn g_origUnlockRect = NULL;
static LONG g_unlockRectSlotId = -1;

// Replicates the bookkeeping the generic thunk would have done, so the
// existing [d3d9] per-window stats keep working for these two methods.
static LONG D3DSlotRecord(LONG slotId, unsigned __int64 t0)
{
    if (slotId < 0 || g_cyclesPerUsec <= 0.0) return 0;
    LONG usec = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
    D3DSlot *s = &g_d3dSlots[slotId];
    InterlockedIncrement(&s->count);
    InterlockedExchangeAdd(&s->sumUsec, usec);
    if (usec > s->maxUsec) s->maxUsec = usec;
    return usec;
}

static HRESULT STDMETHODCALLTYPE HookedTexLockRect(IDirect3DTexture9 *This, UINT Level,
                                                   D3DLOCKED_RECT *pLockedRect,
                                                   const RECT *pRect, DWORD Flags)
{
    D3DSURFACE_DESC desc;
    int haveDesc = 0;
    memset(&desc, 0, sizeof(desc));
    if (This && SUCCEEDED(IDirect3DTexture9_GetLevelDesc(This, Level, &desc))) haveDesc = 1;

    // Traversal-stutter investigation: 27/28 ZwWaitForAlertByThreadId
    // captures spread across a whole run (not just startup) share this exact
    // return address, which decompiles to FUN_00a6e360 doing
    // LockRect(level0,flags=0)+memcpy(16KB)+UnlockRect with no DISCARD. No
    // individual SLOW LockRect fired for it (checked: none this run), so the
    // cost is not one call crossing 5ms - something about repetition, the
    // memcpy itself, or whatever this stalls on inside AMDXN32 is the real
    // question. Logs full detail the first few times this exact call site is
    // seen, staged or not, to find out WHY staging isn't already absorbing it
    // (the outer eligibility gate below has no rejection diagnostic at all -
    // silent rejection here is the same blind spot as the DiscardMip0/
    // ThresholdUs missing-save-line bugs: works or doesn't with no visible
    // difference until specifically instrumented).
    if (haveDesc) {
        DWORD retAddr = (DWORD)(UINT_PTR)_ReturnAddress();
        DWORD ghidraVA = retAddr - g_mainModBase + 0x00400000;
        if (ghidraVA >= 0x00a6e360 && ghidraVA < 0x00a6e360 + 190) {
            static volatile LONG seen = 0;
            LONG n = InterlockedIncrement(&seen);
            if (n <= 6) {
                char l[256];
                sprintf(l, "[d3d9] FUN_00a6e360 LockRect call #%ld: %ux%u fmt=%d pool=%d usage=0x%lX "
                          "flags=0x%lX fullRect=%d stagingEligible=%d",
                        n, desc.Width, desc.Height, (int)desc.Format, (int)desc.Pool,
                        (unsigned long)desc.Usage, (unsigned long)Flags, pRect == NULL,
                        (g_stagingUploadEnabled && pLockedRect && pRect == NULL &&
                         !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) &&
                         desc.Pool == D3DPOOL_DEFAULT &&
                         !(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))));
                LogLine(l);
            }
        }
    }

    // --- staging path -----------------------------------------------------
    // Only full-surface writes to a DEFAULT-pool, non-rendertarget surface.
    // READONLY is excluded outright: the engine would be reading, and a fresh
    // staging surface holds no meaningful contents to read back.
    if (g_stagingUploadEnabled && haveDesc && pLockedRect && pRect == NULL &&
        !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) &&
        desc.Pool == D3DPOOL_DEFAULT &&
        !(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {

        EnsureStagingInit();
        EnterCriticalSection(&g_stagingLock);
        if (!g_stagingDevice) {
            // Take the device from the texture itself - the vtable was
            // obtained from a throwaway probe device, so that one is NOT the
            // device these textures belong to. The extra reference is kept
            // deliberately for the process lifetime.
            IDirect3DTexture9_GetDevice(This, &g_stagingDevice);

            // The device-vtable Present hook has NEVER fired in this
            // project's entire history (confirmed: zero Present/BeginScene/
            // EndScene/Clear/Draw* stat lines across the whole log, versus
            // thousands for Texture/VB/IB Lock) - meaning the probe device's
            // vtable is not shared with the real one, almost certainly
            // because this game creates its device via Direct3DCreate9Ex /
            // CreateDeviceEx, a genuinely different COM object from the
            // IDirect3DDevice9 our probe creates. Resource vtables (Texture,
            // VB, IB) have no Ex variant, so THOSE stayed shared - which is
            // exactly why LockRect/staging have real data throughout. This
            // g_stagingDevice pointer, by contrast, came from GetDevice() on
            // a texture the REAL game created, so it unambiguously IS the
            // real device - used here to answer the presentation-interval
            // question the broken Present hook could not.
            if (g_stagingDevice && InterlockedCompareExchange(&g_presentParamsLogged, 1, 0) == 0) {
                IDirect3DSwapChain9 *sc = NULL;
                if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(g_stagingDevice, 0, &sc)) && sc) {
                    D3DPRESENT_PARAMETERS pp;
                    memset(&pp, 0, sizeof(pp));
                    if (SUCCEEDED(IDirect3DSwapChain9_GetPresentParameters(sc, &pp))) {
                        char line[256];
                        const char *interval =
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) ? "IMMEDIATE(no cap)" :
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE)       ? "ONE(vsync, requested by GAME)" :
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_TWO)       ? "TWO(half refresh)" :
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT)   ? "DEFAULT(driver decides)" : "OTHER";
                        sprintf(line, "[d3d9] REAL device swapchain (via GetDevice, not the broken Present hook): "
                                      "Windowed=%d SwapEffect=%d BackBufferCount=%d RefreshRate=%luHz "
                                      "PresentationInterval=0x%lX %s",
                                pp.Windowed, (int)pp.SwapEffect, pp.BackBufferCount,
                                (unsigned long)pp.FullScreen_RefreshRateInHz,
                                (unsigned long)pp.PresentationInterval, interval);
                        LogLine(line);
                    }
                    IDirect3DSwapChain9_Release(sc);
                }
            }
        }
        LONG idx = AcquireStaging(desc.Width, desc.Height, desc.Format);
        IDirect3DTexture9 *stage = (idx >= 0) ? g_staging[idx].tex : NULL;
        UINT stageLevel = (idx >= 0) ? g_staging[idx].level : 0;
        LeaveCriticalSection(&g_stagingLock);
        // Lock released before touching D3D: the driver takes its own locks
        // and there is no reason to hold ours across that.

        if (stage) {
            D3DLOCKED_RECT lr;
            memset(&lr, 0, sizeof(lr));
            // Deliberately the ORIGINAL, not the IDirect3DTexture9_LockRect
            // macro. The staging texture shares the very vtable this function
            // is installed into, so the macro would re-enter this hook. The
            // SYSTEMMEM pool test above would bounce it straight back out, so
            // it would not actually recurse forever - but resting recursion
            // safety on that coincidence is far too subtle to leave standing.
            int ok = 0;
            if (SUCCEEDED(g_origLockRect(stage, stageLevel, &lr, NULL, 0))) {
                EnterCriticalSection(&g_stagingLock);
                if (g_inflightCount < MAX_INFLIGHT) {
                    g_inflight[g_inflightCount].tex = This;
                    g_inflight[g_inflightCount].level = Level;
                    g_inflight[g_inflightCount].stagingIdx = idx;
                    g_inflightCount++;
                    ok = 1;
                }
                LeaveCriticalSection(&g_stagingLock);
                if (ok) {
                    *pLockedRect = lr;           // engine writes into CPU memory
                    InterlockedIncrement(&g_stagingHits);
                    { LONG _kb = UploadKBFor(lr.Pitch, desc.Height, desc.Format);
                          InterlockedExchangeAdd(&g_uploadKB, _kb);
                          if ((LONG)GetCurrentThreadId() == g_mainThreadId)
                              InterlockedExchangeAdd(&g_uploadKBMain, _kb); }
                    D3DSlotRecord(g_lockRectSlotId, __rdtsc());  // ~0us by construction
                    return S_OK;                 // real surface never locked
                }
                g_origUnlockRect(stage, stageLevel);   // no slot to track it, undo
            }
            EnterCriticalSection(&g_stagingLock);
            g_staging[idx].inUse = 0;            // give the staging texture back
            LeaveCriticalSection(&g_stagingLock);
        }
        InterlockedIncrement(&g_stagingFallback);
        // falls through to the normal path below
    }

    // --- original path, with the existing DISCARD injection ----------------
    DWORD flags = Flags;
    if (g_discardFixEnabled && This && pRect == NULL &&
        !(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) &&
        haveDesc && (desc.Usage & D3DUSAGE_DYNAMIC)) {
        InterlockedIncrement(&g_lockRectEligible);
        UINT levels = IDirect3DTexture9_GetLevelCount(This);
        if (levels != 1) {
            InterlockedIncrement(&g_lockRectMipSkipped);
        } else if (HasSeenTextureBefore((DWORD)(UINT_PTR)This, Level,
                                        desc.Width, desc.Height, (DWORD)desc.Format)) {
            flags |= D3DLOCK_DISCARD;
            InterlockedIncrement(&g_lockRectDiscardInjected);
        } else {
            InterlockedIncrement(&g_lockRectFirstSeen);
        }
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origLockRect(This, Level, pLockedRect, pRect, flags);
    LONG usec = D3DSlotRecord(g_lockRectSlotId, t0);

    if (usec > 5000 && haveDesc) {
        char line[256];
        DWORD tid = GetCurrentThreadId();
        sprintf(line, "[d3d9] SLOW Texture::LockRect: %ld us on thread %lu (%s)",
                usec, tid, (tid == (DWORD)g_mainThreadId) ? "MAIN" : "?");
        LogLine(line);
        sprintf(line, "[d3d9]   LockRect detail: tex=%p level=%u/%lu %ux%u fmt=%d pool=%d "
                      "usage=0x%lX flags=0x%lX DISCARD=%d fullRect=%d",
                (void *)This, Level, (unsigned long)IDirect3DTexture9_GetLevelCount(This),
                desc.Width, desc.Height, (int)desc.Format, (int)desc.Pool,
                (unsigned long)desc.Usage, (unsigned long)flags,
                (flags & D3DLOCK_DISCARD) != 0, pRect == NULL);
        LogLine(line);
    }
    return hr;
}

// ---- IDirect3DSurface9::LockRect - the last unmeasured lock path ---------
//
// After staging removed every SLOW Texture::LockRect (confirmed: zero such
// events in the last two runs) and UpdateSurface measured at ~0us/call,
// 68.2% of remaining stutter STILL shows AMDXN32.DLL blocking with the same
// texture-upload chain on the stack (FUN_00aa28d0 / FUN_00aa3250). The
// return address 0x00aa335e lands immediately after `00aa335c CALL EDX` - an
// indirect COM vtable call - so something in that chain is calling an
// interface method we have never hooked.
//
// IDirect3DSurface9::LockRect is the obvious candidate and a genuine blind
// spot: it lives on a COMPLETELY SEPARATE vtable from
// IDirect3DTexture9::LockRect, so neither our instrumentation nor the
// staging interception has ever seen it. An engine that calls
// GetSurfaceLevel() and then locks the surface directly would bypass every
// texture-level fix in this file while producing exactly the observed
// signature.
//
// Taken from a REAL game surface (the destination in the staging transfer)
// rather than a throwaway probe object, because the probe-device approach is
// confirmed not to share vtables with the real objects - that mistake cost
// this project every device-level measurement it thought it had (see the
// "Methodological finding" section in PROGRESS.md). Diagnostic only for now:
// it times the call and never alters behaviour.
// MEASURED, and it is the remaining stutter: 9,519 calls, 2.50s total,
// **262.7us per call**, worst 13,136us - against 64us/call for the
// staging-fixed texture path, in a session whose entire stutter stall was
// 6.78s. The engine locks surfaces directly (GetSurfaceLevel then LockRect),
// bypassing every texture-level fix in this file.
//
// Same bug, same fix: hand the engine a SYSTEMMEM staging surface instead of
// letting it lock the DEFAULT-pool one, then transfer with UpdateSurface on
// unlock. The existing staging TEXTURE pool is reused rather than adding a
// second pool - its level-0 surface is exactly the SYSTEMMEM source
// UpdateSurface wants, and the pooling/eviction logic is already proven.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfaceLockRect)(
    IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfaceUnlockRect)(IDirect3DSurface9 *);
static PFN_SurfaceLockRect g_origSurfaceLockRect = NULL;
static PFN_SurfaceUnlockRect g_origSurfaceUnlockRect = NULL;
static volatile LONG g_surfLockHooked = 0;
static volatile LONG g_surfLockCount = 0, g_surfLockSumUsec = 0, g_surfLockMaxUsec = 0;
static volatile LONG g_surfStagingHits = 0, g_surfStagingFallback = 0, g_surfStagingUpdateFail = 0;

typedef struct {
    IDirect3DSurface9 *surf;
    LONG stagingIdx;
} InFlightSurf;
static InFlightSurf g_inflightSurf[MAX_INFLIGHT];
static LONG g_inflightSurfCount = 0;

static HRESULT STDMETHODCALLTYPE HookedSurfaceLockRect(
    IDirect3DSurface9 *This, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    if (g_stagingSurfaceEnabled && This && pLockedRect && pRect == NULL &&
        !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) {
        D3DSURFACE_DESC d;
        memset(&d, 0, sizeof(d));
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(This, &d)) &&
            d.Pool == D3DPOOL_DEFAULT &&
            !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {

            EnsureStagingInit();
            EnterCriticalSection(&g_stagingLock);
            if (!g_stagingDevice) IDirect3DSurface9_GetDevice(This, &g_stagingDevice);
            LONG idx = AcquireStaging(d.Width, d.Height, d.Format);
            IDirect3DTexture9 *stage = (idx >= 0) ? g_staging[idx].tex : NULL;
            UINT stageLevel = (idx >= 0) ? g_staging[idx].level : 0;
            LeaveCriticalSection(&g_stagingLock);

            if (stage) {
                D3DLOCKED_RECT lr;
                memset(&lr, 0, sizeof(lr));
                // Original, not the macro: the staging texture shares the
                // vtable this file already patched (same reasoning as the
                // texture staging path).
                if (SUCCEEDED(g_origLockRect(stage, stageLevel, &lr, NULL, 0))) {
                    int ok = 0;
                    EnterCriticalSection(&g_stagingLock);
                    if (g_inflightSurfCount < MAX_INFLIGHT) {
                        g_inflightSurf[g_inflightSurfCount].surf = This;
                        g_inflightSurf[g_inflightSurfCount].stagingIdx = idx;
                        g_inflightSurfCount++;
                        ok = 1;
                    }
                    LeaveCriticalSection(&g_stagingLock);
                    if (ok) {
                        *pLockedRect = lr;
                        InterlockedIncrement(&g_surfStagingHits);
                        { LONG _kb = UploadKBFor(lr.Pitch, d.Height, d.Format);
                          InterlockedExchangeAdd(&g_uploadKB, _kb);
                          if ((LONG)GetCurrentThreadId() == g_mainThreadId)
                              InterlockedExchangeAdd(&g_uploadKBMain, _kb); }
                        return S_OK;    // real surface never locked
                    }
                    g_origUnlockRect(stage, stageLevel);
                }
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            InterlockedIncrement(&g_surfStagingFallback);
            // Why did this fail? The pool now has spare capacity
            // (248/384, recycled=0) so exhaustion is ruled out - meaning
            // AcquireStaging's CreateTexture is rejecting these specific
            // shapes, or the in-flight table is full. Log the first few
            // DISTINCT shapes rather than guessing again. MultiSampleType is
            // included because a multisampled surface can be neither created
            // as a SYSTEMMEM texture nor used with UpdateSurface, which would
            // explain a large, consistent rejection count.
            {
                static DWORD seen[8]; static LONG seenN = 0;
                DWORD sig = (DWORD)d.Format ^ (d.Width << 4) ^ (d.Height << 16);
                int known = 0;
                for (LONG i = 0; i < seenN; i++) if (seen[i] == sig) { known = 1; break; }
                if (!known && seenN < 8) {
                    seen[seenN++] = sig;
                    char l2[224];
                    sprintf(l2, "[d3d9] surface staging REJECT: %ux%u fmt=%d pool=%d usage=0x%lX msaa=%d inflight=%ld",
                            d.Width, d.Height, (int)d.Format, (int)d.Pool,
                            (unsigned long)d.Usage, (int)d.MultiSampleType, g_inflightSurfCount);
                    LogLine(l2);
                }
            }
        }
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origSurfaceLockRect(This, pLockedRect, pRect, Flags);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_surfLockCount);
        InterlockedExchangeAdd(&g_surfLockSumUsec, us);
        if (us > g_surfLockMaxUsec) g_surfLockMaxUsec = us;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedSurfaceUnlockRect(IDirect3DSurface9 *This)
{
    if (g_stagingLockState == 2) {
        EnterCriticalSection(&g_stagingLock);
        LONG found = -1;
        for (LONG i = 0; i < g_inflightSurfCount; i++) {
            if (g_inflightSurf[i].surf == This) { found = i; break; }
        }
        if (found >= 0) {
            LONG idx = g_inflightSurf[found].stagingIdx;
            g_inflightSurf[found] = g_inflightSurf[g_inflightSurfCount - 1];
            g_inflightSurfCount--;
            IDirect3DTexture9 *stage = g_staging[idx].tex;
            UINT stageLevel = g_staging[idx].level;
            LeaveCriticalSection(&g_stagingLock);

            g_origUnlockRect(stage, stageLevel);

            IDirect3DSurface9 *src = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(stage, stageLevel, &src)) && src) {
                // AddRef the destination: previously it was used immediately
                // and needed no reference, but a deferred copy must keep it
                // alive until the copy actually issues.
                IDirect3DSurface9_AddRef(This);
                SubmitStagedUpload(src, This, idx);   // takes both refs + slot
            } else {
                InterlockedIncrement(&g_surfStagingUpdateFail);
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            return S_OK;
        }
        LeaveCriticalSection(&g_stagingLock);
    }
    return g_origSurfaceUnlockRect(This);
}

static void HookRealSurfaceLockRect(IDirect3DSurface9 *surf)
{
    if (!surf) return;
    if (InterlockedCompareExchange(&g_surfLockHooked, 1, 0) != 0) return;
    void **vtbl = *(void ***)surf;
    DWORD oldProtect;

    int slot = offsetof(IDirect3DSurface9Vtbl, LockRect) / sizeof(void *);
    g_origSurfaceLockRect = (PFN_SurfaceLockRect)ResolveOrigSlot(vtbl[slot]);
    if (VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slot] = (void *)HookedSurfaceLockRect;
        VirtualProtect(&vtbl[slot], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotU = offsetof(IDirect3DSurface9Vtbl, UnlockRect) / sizeof(void *);
    g_origSurfaceUnlockRect = (PFN_SurfaceUnlockRect)ResolveOrigSlot(vtbl[slotU]);
    if (VirtualProtect(&vtbl[slotU], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotU] = (void *)HookedSurfaceUnlockRect;
        VirtualProtect(&vtbl[slotU], sizeof(void *), oldProtect, &oldProtect);
    }

    LogLine("[d3d9] REAL Surface::LockRect/UnlockRect hooks installed (from a live game surface)");
}

static HRESULT STDMETHODCALLTYPE HookedTexUnlockRect(IDirect3DTexture9 *This, UINT Level)
{
    if (g_stagingLockState == 2) {
        EnterCriticalSection(&g_stagingLock);
        LONG found = -1;
        for (LONG i = 0; i < g_inflightCount; i++) {
            if (g_inflight[i].tex == This && g_inflight[i].level == Level) { found = i; break; }
        }
        if (found >= 0) {
            LONG idx = g_inflight[found].stagingIdx;
            g_inflight[found] = g_inflight[g_inflightCount - 1];
            g_inflightCount--;
            IDirect3DTexture9 *stage = g_staging[idx].tex;
            UINT stageLevel = g_staging[idx].level;
            LeaveCriticalSection(&g_stagingLock);

            g_origUnlockRect(stage, stageLevel);   // original, not the macro - see LockRect

            // HD GUI mod interop: this is the only moment the engine's pixels
            // exist CPU-side (the real texture only ever receives a GPU-side
            // copy), so push them across BEFORE SubmitStagedUpload can recycle
            // the staging slot. The re-lock is SYSTEMMEM: no GPU sync by
            // construction. Level 0 only - that is all its hashing reads.
            if (g_hdTexNotify && g_hdTexPushEnabled && Level == 0) {
                D3DSURFACE_DESC hdDesc;
                D3DLOCKED_RECT hdLr;
                if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(This, Level, &hdDesc)) &&
                    SUCCEEDED(g_origLockRect(stage, stageLevel, &hdLr, NULL, D3DLOCK_READONLY))) {
                    g_hdTexNotify(This, Level, hdLr.pBits, (unsigned)hdLr.Pitch,
                                  hdDesc.Width, hdDesc.Height, (unsigned)hdDesc.Format);
                    g_origUnlockRect(stage, stageLevel);
                    InterlockedIncrement(&g_hdTexPushCount);
                }
            }

            // The transfer itself. UpdateSurface is queued into the command
            // stream, so it orders against GPU work instead of waiting for it
            // - that is the entire point of this path.
            IDirect3DSurface9 *src = NULL, *dst = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(stage, stageLevel, &src)) &&
                SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(This, Level, &dst))) {
                // Opportunistically hook IDirect3DSurface9::LockRect off a
                // REAL game surface. Must happen BEFORE handing the ref to
                // SubmitStagedUpload, which takes ownership of it.
                HookRealSurfaceLockRect(dst);
                SubmitStagedUpload(src, dst, idx);   // takes both refs + slot
            } else {
                if (src) IDirect3DSurface9_Release(src);
                if (dst) IDirect3DSurface9_Release(dst);
                InterlockedIncrement(&g_stagingUpdateFail);
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }

            D3DSlotRecord(g_unlockRectSlotId, __rdtsc());
            return S_OK;   // the real surface was never locked, nothing to unlock
        }
        LeaveCriticalSection(&g_stagingLock);
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origUnlockRect(This, Level);
    D3DSlotRecord(g_unlockRectSlotId, t0);
    return hr;
}

// ---- IDirect3DCubeTexture9::LockRect - another unhooked vtable ------------
//
// The FUN_00a6e360 call-site diagnostic recorded ZERO hits, despite 27/28
// traversal-stutter captures naming that function. The decompile reads
// vtable +0x4C / +0x50, which IS LockRect/UnlockRect on IDirect3DTexture9 -
// but it is ALSO LockRect/UnlockRect at the identical offsets on
// IDirect3DCubeTexture9, which is a completely separate vtable this project
// has never touched (confirmed: zero references to CreateCubeTexture or
// IDirect3DCubeTexture9 anywhere in this file before now).
//
// Corroborating: the very first decompile of this investigation found
// FUN_00aa28d0 looping over SIX FACES, and this function copies 16KB
// (64x64x4) - a typical cubemap face. Cubemaps are also exactly what a
// traversal trigger would touch (environment/reflection probes updating as
// the camera moves).
//
// Measurement ONLY for now - no staging redirect. Same discipline as the
// Surface::LockRect work: confirm it is expensive and actually called before
// changing behaviour. Cube LockRect takes an extra FaceType argument, so it
// needs its own signature; it cannot reuse the 2D texture hook.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CubeLockRect)(
    IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CubeUnlockRect)(
    IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT);
static PFN_CubeLockRect g_origCubeLockRect = NULL;
static PFN_CubeUnlockRect g_origCubeUnlockRect = NULL;
static volatile LONG g_cubeLockCount=0, g_cubeLockSumUsec=0, g_cubeLockMaxUsec=0;
static volatile LONG g_cubeStagingHits=0, g_cubeStagingFallback=0, g_cubeStagingUpdateFail=0;

// A cubemap upload locks all six faces in sequence, so the in-flight key must
// include the face - (texture, level) alone would collide across faces.
typedef struct {
    IDirect3DCubeTexture9 *tex;
    D3DCUBEMAP_FACES face;
    UINT level;
    LONG stagingIdx;
} InFlightCube;
static InFlightCube g_inflightCube[MAX_INFLIGHT];
static LONG g_inflightCubeCount = 0;
static volatile LONG g_cubeLockLogged = 0;

static HRESULT STDMETHODCALLTYPE HookedCubeLockRect(
    IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES Face, UINT Level,
    D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    // Staging redirect - same shape as the 2D texture and surface paths.
    if (g_stagingCubeEnabled && This && pLockedRect && pRect == NULL &&
        !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) {
        D3DSURFACE_DESC cd;
        memset(&cd, 0, sizeof(cd));
        if (SUCCEEDED(IDirect3DCubeTexture9_GetLevelDesc(This, Level, &cd)) &&
            cd.Pool == D3DPOOL_DEFAULT &&
            !(cd.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {

            EnsureStagingInit();
            EnterCriticalSection(&g_stagingLock);
            if (!g_stagingDevice) IDirect3DCubeTexture9_GetDevice(This, &g_stagingDevice);
            LONG idx = AcquireStaging(cd.Width, cd.Height, cd.Format);
            IDirect3DTexture9 *stage = (idx >= 0) ? g_staging[idx].tex : NULL;
            UINT stageLevel = (idx >= 0) ? g_staging[idx].level : 0;
            LeaveCriticalSection(&g_stagingLock);

            if (stage) {
                D3DLOCKED_RECT lr;
                memset(&lr, 0, sizeof(lr));
                if (SUCCEEDED(g_origLockRect(stage, stageLevel, &lr, NULL, 0))) {
                    int ok = 0;
                    EnterCriticalSection(&g_stagingLock);
                    if (g_inflightCubeCount < MAX_INFLIGHT) {
                        g_inflightCube[g_inflightCubeCount].tex = This;
                        g_inflightCube[g_inflightCubeCount].face = Face;
                        g_inflightCube[g_inflightCubeCount].level = Level;
                        g_inflightCube[g_inflightCubeCount].stagingIdx = idx;
                        g_inflightCubeCount++;
                        ok = 1;
                    }
                    LeaveCriticalSection(&g_stagingLock);
                    if (ok) {
                        *pLockedRect = lr;
                        InterlockedIncrement(&g_cubeStagingHits);
                        { LONG _kb = UploadKBFor(lr.Pitch, cd.Height, cd.Format);
                          InterlockedExchangeAdd(&g_uploadKB, _kb);
                          if ((LONG)GetCurrentThreadId() == g_mainThreadId)
                              InterlockedExchangeAdd(&g_uploadKBMain, _kb); }
                        return S_OK;   // real cube face never locked
                    }
                    g_origUnlockRect(stage, stageLevel);
                }
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            InterlockedIncrement(&g_cubeStagingFallback);
        }
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCubeLockRect(This, Face, Level, pLockedRect, pRect, Flags);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_cubeLockCount);
        InterlockedExchangeAdd(&g_cubeLockSumUsec, us);
        if (us > g_cubeLockMaxUsec) g_cubeLockMaxUsec = us;
        // Log the first few with full detail, so if this IS the trigger we
        // already know the shape needed to stage it (pool/usage/format
        // decide eligibility) without another run.
        if (InterlockedIncrement(&g_cubeLockLogged) <= 6) {
            D3DSURFACE_DESC d;
            memset(&d, 0, sizeof(d));
            char l[224];
            if (SUCCEEDED(IDirect3DCubeTexture9_GetLevelDesc(This, Level, &d))) {
                sprintf(l, "[d3d9] CubeTexture::LockRect #%ld: face=%d level=%u %ux%u fmt=%d pool=%d usage=0x%lX flags=0x%lX fullRect=%d took=%ldus",
                        g_cubeLockLogged, (int)Face, Level, d.Width, d.Height, (int)d.Format,
                        (int)d.Pool, (unsigned long)d.Usage, (unsigned long)Flags, pRect == NULL, us);
            } else {
                sprintf(l, "[d3d9] CubeTexture::LockRect #%ld: face=%d level=%u (GetLevelDesc failed) flags=0x%lX took=%ldus",
                        g_cubeLockLogged, (int)Face, Level, (unsigned long)Flags, us);
            }
            LogLine(l);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCubeUnlockRect(
    IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES Face, UINT Level)
{
    if (g_stagingLockState == 2) {
        EnterCriticalSection(&g_stagingLock);
        LONG found = -1;
        for (LONG i = 0; i < g_inflightCubeCount; i++) {
            if (g_inflightCube[i].tex == This && g_inflightCube[i].face == Face &&
                g_inflightCube[i].level == Level) { found = i; break; }
        }
        if (found >= 0) {
            LONG idx = g_inflightCube[found].stagingIdx;
            g_inflightCube[found] = g_inflightCube[g_inflightCubeCount - 1];
            g_inflightCubeCount--;
            IDirect3DTexture9 *stage = g_staging[idx].tex;
            UINT stageLevel = g_staging[idx].level;
            LeaveCriticalSection(&g_stagingLock);

            g_origUnlockRect(stage, stageLevel);

            IDirect3DSurface9 *src = NULL, *dst = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(stage, stageLevel, &src)) &&
                SUCCEEDED(IDirect3DCubeTexture9_GetCubeMapSurface(This, Face, Level, &dst))) {
                SubmitStagedUpload(src, dst, idx);   // takes both refs + slot
            } else {
                if (src) IDirect3DSurface9_Release(src);
                if (dst) IDirect3DSurface9_Release(dst);
                InterlockedIncrement(&g_cubeStagingUpdateFail);
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            return S_OK;
        }
        LeaveCriticalSection(&g_stagingLock);
    }
    return g_origCubeUnlockRect(This, Face, Level);
}

// ---- Texture lifetime + upload-volume probes ------------------------------
// Deciding whether a TEXTURE POOL is worth building. A pool prevents the
// driver from tearing down and re-establishing GPU heaps (the plausible
// cause of the measured 8.8ms CreateTexture for a 64-byte texture), but it
// only ever gets used if the game actually RELEASES textures and later
// creates matching ones. If the game simply caches everything (which is what
// "second visit through an area is smoother" hints at), a pool would sit
// empty and buy nothing.
//
// So: count real destructions (Release returning 0), not just creations.
//   creates >> destroys  -> textures are cached, pooling is pointless.
//   creates ~= destroys  -> genuine churn, pooling should help.
//
// Also accumulate UPLOAD BYTES through the staging paths. CreateTexture is
// only ~0.12s/run against 1.2-2.8s of stall, so it cannot be the dominant
// first-visit cost; the engine's own memcpy of pixel data into the surfaces
// we hand it has never been measured and is the larger suspect. If a chunk
// load pushes tens of MB through a handful of frames, no pool addresses that
// and the effort belongs elsewhere.
typedef ULONG (STDMETHODCALLTYPE *PFN_TexRelease)(IDirect3DTexture9 *);
static PFN_TexRelease g_origTexRelease = NULL;
// (declared as tentative definitions near the top - see there)

// ---- Texture pool ----------------------------------------------------------
// Reuse a texture whose game-visible refcount just reached zero instead of
// letting the driver tear it down, and hand it back out on a later
// CreateTexture call that asks for the exact same shape. Two confirmed
// findings point here: creation-count correlates with slow frames
// (1.68-2.62x, fading but real once the cache is warm) and a single
// CreateTexture for a 64-byte texture measured 8.8ms - allocation cost
// tracks the driver's GPU heap bookkeeping, not the data size, so a repeat
// of a shape it has already allocated should be far cheaper. See
// PROGRESS.md "Texture-pool question ANSWERED" for the full measurement and
// the ~1/3-of-remaining-stutter ceiling estimate.
//
// Correctness rests on a property this project already relies on
// everywhere else: a texture is useless to the engine until it has been
// populated via LockRect/UnlockRect (or the staging UpdateSurface path), so
// stale pixel data left over from a reused texture's previous life is never
// visible - draw calls only ever happen after repopulation. RENDERTARGET,
// DEPTHSTENCIL and AUTOGENMIPMAP textures are excluded below precisely
// because that guarantee does NOT hold for them (the pipeline draws INTO a
// render target rather than populating it via Lock), and only
// MANAGED/DEFAULT pool textures with an explicit (non-zero) Levels count are
// considered, to avoid needing to replicate the driver's own mip-chain-size
// computation for the auto-chain (Levels=0) case.
//
// Interaction with the DISCARD fix (cause 1): that fix's HasSeenTextureBefore
// table already treats "same pointer, different shape" as a recycled
// address and correctly resets to "not seen" for it - the exact mechanism
// this pool's reuse needs, since a pool-hit reuse has the SAME shape by
// construction and would otherwise be wrongly treated as an already-seen
// texture, letting DISCARD apply to its first lock in its new life (the
// same corruption class already found and fixed once for cause 1).
// InvalidateSeenTextureForReuse (defined next to HasSeenTextureBefore)
// forces that same shape-mismatch path on every reuse.
// RETIRED (ENABLE_TEXTURE_POOL) - see the gate at the top of this file.
#if ENABLE_TEXTURE_POOL
#define TEXPOOL_MAX_ENTRIES 256
#define TEXPOOL_MAX_PER_KEY 8

typedef struct {
    IDirect3DTexture9 *tex;
    UINT width, height, levels;
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD usage;
    int inUse;
} TexPoolEntry;

static TexPoolEntry g_texPool[TEXPOOL_MAX_ENTRIES];
static LONG g_texPoolCount = 0;
static CRITICAL_SECTION g_texPoolLock;
static volatile LONG g_texPoolLockState = 0;
static volatile LONG g_texPoolHits = 0, g_texPoolMisses = 0;
static volatile LONG g_texPoolReturned = 0, g_texPoolEvicted = 0;

static void EnsureTexPoolInit(void)
{
    if (InterlockedCompareExchange(&g_texPoolLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_texPoolLock);
        g_texPoolLockState = 2;
    } else {
        while (g_texPoolLockState != 2) Sleep(0);
    }
}

static int IsPoolableTexture(DWORD usage, D3DPOOL pool)
{
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))
        return 0;
    if (pool != D3DPOOL_MANAGED && pool != D3DPOOL_DEFAULT)
        return 0;
    return 1;
}

static BOOL TryAcquireFromPool(UINT w, UINT h, UINT levels, D3DFORMAT fmt, D3DPOOL pool, DWORD usage,
                                IDirect3DTexture9 **outTex)
{
    EnsureTexPoolInit();
    EnterCriticalSection(&g_texPoolLock);
    for (LONG i = 0; i < g_texPoolCount; i++) {
        TexPoolEntry *e = &g_texPool[i];
        if (!e->inUse && e->width == w && e->height == h && e->levels == levels &&
            e->format == fmt && e->pool == pool && e->usage == usage) {
            e->inUse = 1;
            IDirect3DTexture9_AddRef(e->tex);
            *outTex = e->tex;
            LeaveCriticalSection(&g_texPoolLock);
            InvalidateSeenTextureForReuse((DWORD)(UINT_PTR)e->tex, levels);
            InterlockedIncrement(&g_texPoolHits);
            return TRUE;
        }
    }
    LeaveCriticalSection(&g_texPoolLock);
    InterlockedIncrement(&g_texPoolMisses);
    return FALSE;
}

// Called with a texture that is still safely alive (see HookedTexRelease's
// temp-AddRef pattern) whose game-visible refcount just reached zero.
// Returns TRUE if the pool took ownership (caller must NOT release it any
// further - the pool's ref stands in for the game's departed one), FALSE if
// the caller should let it die exactly like the unmodified path would.
static BOOL ReturnToPoolOrEvict(IDirect3DTexture9 *tex)
{
    EnsureTexPoolInit();
    EnterCriticalSection(&g_texPoolLock);
    for (LONG i = 0; i < g_texPoolCount; i++) {
        if (g_texPool[i].tex == tex) {
            g_texPool[i].inUse = 0;
            LeaveCriticalSection(&g_texPoolLock);
            InterlockedIncrement(&g_texPoolReturned);
            return TRUE;
        }
    }
    LeaveCriticalSection(&g_texPoolLock);

    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &desc))) return FALSE;
    if (!IsPoolableTexture(desc.Usage, desc.Pool)) return FALSE;
    UINT levels = IDirect3DTexture9_GetLevelCount(tex);
    if (levels == 0) return FALSE;

    EnterCriticalSection(&g_texPoolLock);
    if (g_texPoolCount >= TEXPOOL_MAX_ENTRIES) {
        LeaveCriticalSection(&g_texPoolLock);
        InterlockedIncrement(&g_texPoolEvicted);
        return FALSE;
    }
    LONG sameKey = 0;
    for (LONG i = 0; i < g_texPoolCount; i++) {
        TexPoolEntry *e = &g_texPool[i];
        if (e->width == desc.Width && e->height == desc.Height && e->levels == levels &&
            e->format == desc.Format && e->pool == desc.Pool && e->usage == desc.Usage)
            sameKey++;
    }
    if (sameKey >= TEXPOOL_MAX_PER_KEY) {
        LeaveCriticalSection(&g_texPoolLock);
        InterlockedIncrement(&g_texPoolEvicted);
        return FALSE;
    }
    TexPoolEntry *e = &g_texPool[g_texPoolCount++];
    e->tex = tex;
    e->width = desc.Width; e->height = desc.Height; e->levels = levels;
    e->format = desc.Format; e->pool = desc.Pool; e->usage = desc.Usage;
    e->inUse = 0;
    LeaveCriticalSection(&g_texPoolLock);
    return TRUE;
}

#endif  // ENABLE_TEXTURE_POOL

static ULONG STDMETHODCALLTYPE HookedTexRelease(IDirect3DTexture9 *This)
{
#if !ENABLE_TEXTURE_POOL
    // Pool retired: this is now just the destroy counter. Release is extremely
    // hot - every SetTexture can AddRef/Release - so with the pool gone the
    // branch it used to need goes with it.
    ULONG rc = g_origTexRelease(This);
    if (rc == 0) InterlockedIncrement(&g_texDestroyCount);
    return rc;
#else
    if (!g_texturePoolEnabled) {
        // Release is extremely hot (every SetTexture can AddRef/Release), so
        // this stays to a single call-through plus one compare on the common
        // path.
        ULONG rc = g_origTexRelease(This);
        if (rc == 0) InterlockedIncrement(&g_texDestroyCount);
        return rc;
    }
    // Pool path: hold a temp AddRef across the game's own Release so the
    // real refcount can never actually reach zero while we decide whether to
    // keep the object alive - touching it after a real zero-crossing would
    // be use-after-free, since a typical Release implementation frees the
    // object inside the same call that returns 0.
    IDirect3DTexture9_AddRef(This);
    ULONG rc = g_origTexRelease(This);   // performs the game's actual, intended decrement
    if (rc == 1) {
        // Only our temp ref is left: this WAS the game's last reference.
        // Safe to inspect (GetLevelDesc etc.) since we still hold a live ref.
        if (ReturnToPoolOrEvict(This)) {
            return 0;   // matches what a normal Release-to-zero returns
        }
        ULONG rc2 = g_origTexRelease(This);   // release our temp ref for real
        if (rc2 == 0) InterlockedIncrement(&g_texDestroyCount);
        return rc2;
    }
    // Other references remain - undo our temp ref and return the count a
    // normal, unmodified Release call would have reported.
    return g_origTexRelease(This);
#endif  // !ENABLE_TEXTURE_POOL
}

// Resource Lock methods live on their own per-type vtables, not the device's,
// and a Lock on a resource the GPU still owns is one of the classic D3D9
// stalls - so they must be covered. Creating one throwaway resource of each
// type is the cleanest way to obtain those vtables with correct types, rather
// than trying to intercept the first game-created resource.
//
// Called from two places (whichever runs first wins): the throwaway probe
// device on native (cheap there - see ProbeD3D9ForVtable), and the game's OWN
// real device via HookRealDevicePresent - the latter is now the ONLY path
// under a third-party d3d9.dll, since a second real device is what crashes
// DXVK (see the g_d3d9IsThirdParty comment and "First real WER crash dump").
// Idempotent so both call sites can be unconditional.
static void HookResourceVtables(IDirect3DDevice9 *dev)
{
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) != 0) return;

    IDirect3DTexture9 *tex = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL)) && tex) {
        void **vt = *(void ***)tex;
        // These two get real replacement functions rather than timing thunks:
        // the staging path has to be able to NOT call the original.
        {
            DWORD oldProtR;
            int slotR = offsetof(IDirect3DTexture9Vtbl, Release) / sizeof(void *);
            g_origTexRelease = (PFN_TexRelease)vt[slotR];
            if (VirtualProtect(&vt[slotR], sizeof(void *), PAGE_READWRITE, &oldProtR)) {
                vt[slotR] = (void *)HookedTexRelease;
                VirtualProtect(&vt[slotR], sizeof(void *), oldProtR, &oldProtR);
            }
        }
        g_lockRectSlotId = HookVtableSlotEx(vt, offsetof(IDirect3DTexture9Vtbl, LockRect) / sizeof(void *),
                                            "Texture::LockRect", (void *)HookedTexLockRect);
        if (g_lockRectSlotId >= 0) g_origLockRect = (LockRectFn)g_d3dSlots[g_lockRectSlotId].orig;
        g_unlockRectSlotId = HookVtableSlotEx(vt, offsetof(IDirect3DTexture9Vtbl, UnlockRect) / sizeof(void *),
                                              "Texture::UnlockRect", (void *)HookedTexUnlockRect);
        if (g_unlockRectSlotId >= 0) g_origUnlockRect = (UnlockRectFn)g_d3dSlots[g_unlockRectSlotId].orig;
        IDirect3DTexture9_Release(tex);
    }
    IDirect3DVertexBuffer9 *vb = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(dev, 256, 0, 0, D3DPOOL_MANAGED, &vb, NULL)) && vb) {
        void **vt = *(void ***)vb;
        HookVtableSlot(vt, offsetof(IDirect3DVertexBuffer9Vtbl, Lock) / sizeof(void *), "VB::Lock");
        IDirect3DVertexBuffer9_Release(vb);
    }
    // Cube textures: never instrumented before this point. LockRect sits at
    // the SAME vtable offset as IDirect3DTexture9's (+0x4C), which is why the
    // decompile of the traversal trigger was ambiguous between the two.
    IDirect3DCubeTexture9 *cube = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateCubeTexture(dev, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL)) && cube) {
        void **vt = *(void ***)cube;
        DWORD oldProt;
        int sl = offsetof(IDirect3DCubeTexture9Vtbl, LockRect) / sizeof(void *);
        g_origCubeLockRect = (PFN_CubeLockRect)vt[sl];
        if (VirtualProtect(&vt[sl], sizeof(void *), PAGE_READWRITE, &oldProt)) {
            vt[sl] = (void *)HookedCubeLockRect;
            VirtualProtect(&vt[sl], sizeof(void *), oldProt, &oldProt);
        }
        int su = offsetof(IDirect3DCubeTexture9Vtbl, UnlockRect) / sizeof(void *);
        g_origCubeUnlockRect = (PFN_CubeUnlockRect)vt[su];
        if (VirtualProtect(&vt[su], sizeof(void *), PAGE_READWRITE, &oldProt)) {
            vt[su] = (void *)HookedCubeUnlockRect;
            VirtualProtect(&vt[su], sizeof(void *), oldProt, &oldProt);
        }
        IDirect3DCubeTexture9_Release(cube);
        char l[128];
        sprintf(l, "[d3d9] CubeTexture LockRect/UnlockRect hooks installed (slots %d/%d)", sl, su);
        LogLine(l);
    }

    IDirect3DIndexBuffer9 *ib = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateIndexBuffer(dev, 256, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL)) && ib) {
        void **vt = *(void ***)ib;
        HookVtableSlot(vt, offsetof(IDirect3DIndexBuffer9Vtbl, Lock) / sizeof(void *), "IB::Lock");
        IDirect3DIndexBuffer9_Release(ib);
    }
}

#define HOOK_DEV(m) HookVtableSlot(vt, offsetof(IDirect3DDevice9Vtbl, m) / sizeof(void *), #m)

static void HookDeviceVtable(IDirect3DDevice9 *dev)
{
    void **vt = *(void ***)dev;

    // The resource hooks are the ones that actually do work (Texture/Cube/
    // Surface LockRect - the staging fixes) and they are safe everywhere:
    // resource classes are shared across device classes even on native d3d9,
    // which is exactly why they fire on the game's textures today. They go in
    // unconditionally.
    //
    // The device-slot timing thunks below are the opposite: on native they
    // have never fired once (the probe device is SOFTWARE-vertex-processing,
    // the game's is HARDWARE, and Microsoft's runtime gives those different
    // classes), so they cost nothing and prove nothing. Under a single-class
    // wrapper like DXVK the same code would land on the game's live device -
    // untested return-address hijacks across the entire draw path, plus a
    // double-patch race with HookRealDevicePresent. Off by default there.
    //
    // d3d9.dll provenance alone cannot catch every single-class case: the HD
    // GUI mod (version.dll) wraps EVERY device CreateDevice returns in one
    // C++ proxy class, so probe and game devices share a vtable even though
    // d3d9.dll itself is genuinely System32's. The decisive test is where
    // this device's vtable actually LIVES: a native device vtable is inside
    // d3d9.dll; anywhere else means some wrapper owns this object, and the
    // DXVK reasoning applies unchanged. Resource hooks are unaffected either
    // way - they are read from real resource objects, which no wrapper here
    // proxies.
    HMODULE vtOwner = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)vt, &vtOwner);
    {
        HMODULE d3d9Mod = GetModuleHandleA("d3d9.dll");
        int vtIsWrapped = (vtOwner != d3d9Mod);   // unresolvable owner counts as wrapped
        if (vtIsWrapped) {
            char l[320]; char ownerName[MAX_PATH];
            ownerName[0] = '\0';
            if (vtOwner) GetModuleFileNameA(vtOwner, ownerName, MAX_PATH);
            sprintf(l, "[d3d9] device vtable lives in '%s', not d3d9.dll - wrapped device",
                    ownerName[0] ? ownerName : "<unknown>");
            LogLine(l);
        }
        int wantThunks = (g_probeDeviceThunks < 0)
                             ? (!g_d3d9IsThirdParty && !vtIsWrapped)
                             : (g_probeDeviceThunks != 0);
        if (!wantThunks) {
            HookResourceVtables(dev);
            LogLine("[d3d9] device-slot timing thunks SKIPPED (wrapped device or "
                    "third-party d3d9.dll); resource hooks installed as normal");
            return;
        }
    }

    g_presentSlotId = HOOK_DEV(Present);
    HOOK_DEV(Reset);
    HOOK_DEV(TestCooperativeLevel);
    HOOK_DEV(EvictManagedResources);
    HOOK_DEV(CreateTexture);
    HOOK_DEV(CreateVertexBuffer);
    HOOK_DEV(CreateIndexBuffer);
    HOOK_DEV(CreateRenderTarget);
    HOOK_DEV(CreateDepthStencilSurface);
    HOOK_DEV(CreateOffscreenPlainSurface);
    HOOK_DEV(CreateVertexShader);
    HOOK_DEV(CreatePixelShader);
    HOOK_DEV(CreateQuery);
    HOOK_DEV(UpdateSurface);
    HOOK_DEV(UpdateTexture);
    HOOK_DEV(GetRenderTargetData);
    HOOK_DEV(StretchRect);
    HOOK_DEV(ColorFill);
    HOOK_DEV(SetRenderTarget);
    HOOK_DEV(BeginScene);
    HOOK_DEV(EndScene);
    HOOK_DEV(Clear);
    HOOK_DEV(DrawPrimitive);
    HOOK_DEV(DrawIndexedPrimitive);
    HOOK_DEV(DrawPrimitiveUP);
    HOOK_DEV(DrawIndexedPrimitiveUP);

    HookResourceVtables(dev);

    char line[192];
    sprintf(line, "[d3d9] device vtable hooked, %ld slots instrumented", g_d3dSlotCount);
    LogLine(line);
}

// Defined here rather than inline in MonitorThread because the D3D9 state it
// reads is declared below the monitor in this file.
// ---- Present timing on the REAL device -----------------------------------
// The generic device-vtable instrumentation has never worked (see the
// "Methodological finding" section in PROGRESS.md): it was installed on a
// throwaway PROBE device whose vtable the game's real D3D9Ex device does not
// share, so Present/BeginScene/Draw* have never produced a single sample.
// CreateDeviceEx now hands us the real device, so Present can finally be
// measured. Deliberately a dedicated replacement rather than the generic
// thunk: this fires before the deferred thread has allocated the thunk
// arena, so reusing that machinery here would be an ordering hazard for no
// benefit.
// The question it answers: with PresentationInterval confirmed IMMEDIATE and
// the 60Hz frame-time clustering gone, the ~60fps ceiling is still there. If
// Present itself blocks ~16ms/frame, the cap is presentation-side (windowed
// D3D9Ex is composited by DWM, which paces to refresh unless it can promote
// the window to an independent flip - overlays and the game's own WinUI menu
// layer are exactly the things that prevent that). If Present returns fast,
// the ceiling is internal to the engine and the search moves to its own
// timing loop.
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevicePresent)(
    IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static PFN_DevicePresent g_origDevicePresent = NULL;
static volatile LONG g_presentCount = 0, g_presentSumUsec = 0, g_presentMaxUsec = 0;
static volatile LONG g_devicePresentHooked = 0;

// Defined with the MSAA block further down; the Present hooks below use them
// as a backstop so an unresolved scene can never reach the screen.
static void MsaaResolve(IDirect3DDevice9 *dev);
static void MsaaResolveR32f(IDirect3DDevice9 *dev);
static void MsaaRestoreDepth(IDirect3DDevice9 *dev);
// One-shot pipeline splitter for the 4K black screen (defined with the MSAA
// block): MsaaResolve arms state 2 after dumping the resolved scene texture;
// the next Present dumps the backbuffer, i.e. what the user actually sees.
static void DumpSurfaceToBmp(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, const char *name);
static volatile LONG g_msDumpState;   // 0 idle, 2 backbuffer pending, 3 done

static HRESULT STDMETHODCALLTYPE HookedDevicePresent(
    IDirect3DDevice9 *This, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
    // Backstop: if the pass ended without any later SetRenderTarget, the
    // accumulated MS scene would never reach the engine's texture. Resolving
    // here is late but cannot be missed.
    if (g_msHasContent) { MsaaResolve(This); MsaaRestoreDepth(This); }
    if (g_msR32fHasContent) MsaaResolveR32f(This);
    if (g_msDumpState == 2 && InterlockedCompareExchange(&g_msDumpState, 3, 2) == 2) {
        IDirect3DSurface9 *bb = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(This, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            DumpSurfaceToBmp(This, bb, "msaa_backbuf.bmp");
            IDirect3DSurface9_Release(bb);
        }
    }
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origDevicePresent(This, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_presentCount);
        InterlockedExchangeAdd(&g_presentSumUsec, us);
        if (us > g_presentMaxUsec) g_presentMaxUsec = us;
    }
    return hr;
}

// A D3D9Ex device has BOTH Present (inherited, slot 17) and PresentEx
// (slot 121). The first attempt hooked only Present and recorded zero calls
// across a whole session, which means the game presents through PresentEx -
// so both are hooked now, and the counters are shared so the per-window
// report reads the same either way.
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevicePresentEx)(
    IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static PFN_DevicePresentEx g_origDevicePresentEx = NULL;

// ---- main-thread Sleep probe ---------------------------------------------
// The frame is 17,778us; FUN_00ac3040 (the per-frame tick) accounts for
// 13,191us of it, and main-thread WaitForSingleObject only 603us - leaving
// ~4.6ms per frame unaccounted for and NOT spent blocking on an event. A
// sleep-based frame limiter is the obvious candidate, and this measures it
// directly rather than inferring it: read-only, counts main-thread Sleep
// calls and how long they actually took.
typedef void (WINAPI *PFN_Sleep)(DWORD);
static PFN_Sleep g_realSleep = NULL;
static volatile LONG g_mainSleepCount = 0, g_mainSleepUsec = 0, g_mainSleepMaxUsec = 0;

// The probe found the main thread calling Sleep ~12,000 times PER FRAME, each
// averaging under a microsecond - i.e. Sleep(0) in a spin-wait, burning
// ~5.2ms/frame, which accounts for essentially all of the ~4.6ms that was
// unexplained. Finding WHICH loop is now the whole question, so the call
// site is sampled here.
// Sampled, not recorded on every call: at ~680,000 calls/second a linear
// scan per call would itself distort the timing being measured. One in 256
// still yields thousands of samples per second, far more than enough to rank
// call sites.
#define SLEEP_CALLER_SLOTS 24
static struct { DWORD addr; LONG count; } g_sleepCallers[SLEEP_CALLER_SLOTS];
static LONG g_sleepCallerUsed = 0;
static volatile LONG g_sleepSampleTick = 0;
static DWORD g_sleepTopPrinted[4];

static void RecordSleepCaller(DWORD ret)
{
    LONG n = g_sleepCallerUsed;
    for (LONG i = 0; i < n; i++) {
        if (g_sleepCallers[i].addr == ret) { InterlockedIncrement(&g_sleepCallers[i].count); return; }
    }
    if (n < SLEEP_CALLER_SLOTS) {
        g_sleepCallers[n].addr = ret;
        g_sleepCallers[n].count = 1;
        g_sleepCallerUsed = n + 1;
    }
}

static void WINAPI HookedSleep(DWORD dwMilliseconds)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    DWORD ret = isMain ? (DWORD)(UINT_PTR)_ReturnAddress() : 0;
    unsigned __int64 t0 = isMain ? __rdtsc() : 0;
    g_realSleep(dwMilliseconds);
    if (isMain && g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_mainSleepCount);
        InterlockedExchangeAdd(&g_mainSleepUsec, us);
        if (us > g_mainSleepMaxUsec) g_mainSleepMaxUsec = us;
        if ((InterlockedIncrement(&g_sleepSampleTick) & 0xFF) == 0) RecordSleepCaller(ret);
    }
}

// ---- Real-device UpdateSurface/UpdateTexture/CreateTexture timing --------
// Run-A analysis (framerate unlocked, RTSS-capped): 68% of remaining
// stutter-watchdog stall still shows AMDXN32.DLL blocking with the SAME hot
// game-code addresses as the original LockRect investigation
// (FUN_00aa28d0/FUN_00aa3250, the cubemap texture upload chain) - but SLOW
// LockRect count is now ZERO. Staging fully absorbed the LockRect stalls, so
// whatever is still blocking in that same code region is calling something
// ELSE. UpdateSurface/UpdateTexture are the obvious candidates (the D3D9-
// standard non-stalling upload path staging itself uses to push data across)
// and CreateTexture (resource creation can also stall on some drivers). All
// three are DEVICE-level methods, and every device-level method hooked via
// the old probe-device technique has been confirmed dead for the entire
// project (see "Methodological finding" above) - so, like Present/PresentEx,
// they are measured here on the REAL device instead, for the first time.
typedef HRESULT (STDMETHODCALLTYPE *PFN_UpdateSurface)(
    IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_UpdateTexture)(
    IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(
    IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);

static PFN_UpdateSurface g_origUpdateSurface = NULL;
static PFN_UpdateTexture g_origUpdateTexture = NULL;
static PFN_CreateTexture g_origCreateTexture = NULL;
// Chunk-load stutters show NtGdiDdDDICreateAllocation (kernel GPU memory
// allocation). CreateTexture is measured and cheap (13us), but buffer
// creation has only ever been hooked on the dead probe vtable - so if a map
// chunk creates vertex/index buffers, that cost is completely unmeasured.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVB)(
    IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateIB)(
    IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
static PFN_CreateVB g_origCreateVB = NULL;
static PFN_CreateIB g_origCreateIB = NULL;
static volatile LONG g_cvbCount=0, g_cvbSumUsec=0, g_cvbMaxUsec=0;
static volatile LONG g_cibCount=0, g_cibSumUsec=0, g_cibMaxUsec=0;

static HRESULT STDMETHODCALLTYPE HookedCreateVB(
    IDirect3DDevice9 *This, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9 **ppVB, HANDLE *pShared)
{
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCreateVB(This, Length, Usage, FVF, Pool, ppVB, pShared);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_cvbCount);
        InterlockedExchangeAdd(&g_cvbSumUsec, us);
        if (us > g_cvbMaxUsec) g_cvbMaxUsec = us;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateIB(
    IDirect3DDevice9 *This, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9 **ppIB, HANDLE *pShared)
{
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCreateIB(This, Length, Usage, Format, Pool, ppIB, pShared);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_cibCount);
        InterlockedExchangeAdd(&g_cibSumUsec, us);
        if (us > g_cibMaxUsec) g_cibMaxUsec = us;
    }
    return hr;
}
static volatile LONG g_updSurfCount=0, g_updSurfSumUsec=0, g_updSurfMaxUsec=0;
static volatile LONG g_updTexCount=0, g_updTexSumUsec=0, g_updTexMaxUsec=0;
static volatile LONG g_createTexSumUsec=0, g_createTexMaxUsec=0;

static HRESULT STDMETHODCALLTYPE HookedUpdateSurface(
    IDirect3DDevice9 *This, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
    IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint)
{
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origUpdateSurface(This, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_updSurfCount);
        InterlockedExchangeAdd(&g_updSurfSumUsec, us);
        if (us > g_updSurfMaxUsec) g_updSurfMaxUsec = us;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedUpdateTexture(
    IDirect3DDevice9 *This, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
{
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origUpdateTexture(This, pSourceTexture, pDestinationTexture);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_updTexCount);
        InterlockedExchangeAdd(&g_updTexSumUsec, us);
        if (us > g_updTexMaxUsec) g_updTexMaxUsec = us;
    }
    return hr;
}

// Shadow-map identification. Deliberately conservative: every screen-derived
// render target in this engine is 16:9, so requiring a power-of-two width
// with a 1:1 or 1:2 aspect excludes the linear-depth R32F targets and the
// screen-sized D24S8 without needing to know any call sites. The 512 floor
// keeps small utility textures out.
// Level-0 surfaces of the scaled shadow textures, so SetRenderTarget can
// recognise when one is bound and SetViewport can compensate. Defined here
// rather than with the other shadow globals because it needs D3D9 types.
static IDirect3DSurface9 *g_shadowSurfaces[MAX_SHADOW_SURFACES];

// STEP 1 of the cascade plan: a shadow-pass signal that cannot break Reset.
//
// The previous version kept the reference GetSurfaceLevel hands back, for the
// whole process lifetime. Shadow textures are D3DPOOL_DEFAULT, and D3D9
// requires every DEFAULT-pool resource to be released before Reset can
// succeed - and this game DOES Reset during startup (1x1 -> 3840x2160). A
// held reference can therefore fail Reset, which is a strong candidate for
// the attempt-2 startup crash. (Not proven: an earlier build held the same
// references and still booted. It is a real bug either way.)
//
// So: take the pointer, record its VALUE, release the reference immediately.
// Nothing is ever dereferenced - the table is only compared against the
// pointer SetRenderTarget is handed - so no reference is needed to use it,
// only to obtain it.
//
// Residual risk is a destroyed surface's address being reused by something
// else, producing a false positive. Bounded by clearing the table on Reset
// (see HookedDeviceReset) and re-capturing on every shadow-texture creation.
static void RememberShadowSurface(IDirect3DTexture9 *tex)
{
    if (!tex) return;
    IDirect3DSurface9 *surf = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf)) || !surf) return;
    LONG idx = InterlockedIncrement(&g_shadowSurfaceCount) - 1;
    if (idx < MAX_SHADOW_SURFACES) {
        g_shadowSurfaces[idx] = surf;      // value only, see above
    } else {
        g_shadowSurfaceCount = MAX_SHADOW_SURFACES;
    }
    IDirect3DSurface9_Release(surf);       // never hold it - Reset must stay possible
}

static void ClearShadowSurfaces(void)
{
    for (int i = 0; i < MAX_SHADOW_SURFACES; i++) g_shadowSurfaces[i] = NULL;
    g_shadowSurfaceCount = 0;
    g_shadowRtActive = 0;
}

static int IsShadowSurface(IDirect3DSurface9 *s)
{
    if (!s) return 0;
    LONG n = g_shadowSurfaceCount;
    if (n > MAX_SHADOW_SURFACES) n = MAX_SHADOW_SURFACES;
    for (LONG i = 0; i < n; i++) if (g_shadowSurfaces[i] == s) return 1;
    return 0;
}

static int IsShadowMapCreate(UINT W, UINT H, D3DFORMAT Fmt, DWORD Usage)
{
    if (W < 512) return 0;
    if ((W & (W - 1)) != 0) return 0;              // power of two only
    if (!(H == W || H == W * 2)) return 0;         // single map, or 2-cascade atlas
    if ((Usage & D3DUSAGE_RENDERTARGET) && Fmt == D3DFMT_R32F) return 1;
    if ((Usage & D3DUSAGE_DEPTHSTENCIL) && Fmt == D3DFMT_D24S8) return 1;
    return 0;
}

static HRESULT STDMETHODCALLTYPE HookedCreateTexture(
    IDirect3DDevice9 *This, UINT Width, UINT Height, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
{
    // Scale the shadow set at creation. The cascade RTs and their paired
    // depth surfaces must stay dimensionally identical to each other, which
    // happens naturally here because the same multiplier is applied to both.
    // Capture the shadow-pass signal regardless of scaling. Deliberately NOT
    // an early return this time - attempt 2 restructured this function and the
    // crash could not be attributed. This only observes.
    if (g_shadowScale <= 1 && (Usage & D3DUSAGE_RENDERTARGET)
        && IsShadowMapCreate(Width, Height, Format, Usage)) {
        g_pendingShadowCapture = 1;
    }
#if ENABLE_SHADOW_SCALE
    // RETIRED (ENABLE_SHADOW_SCALE). Superseded by ShadowMapRes, which writes
    // the engine's own resolution field so the texture, the light projection
    // and the PCF taps all change together. Scaling the texture alone - what
    // this does - grew the surface while every calculation consuming it kept
    // the old size, giving shadows confined to a camera-tracking square.
    if (g_shadowScale > 1 && IsShadowMapCreate(Width, Height, Format, Usage)) {
        UINT nW = Width * (UINT)g_shadowScale, nH = Height * (UINT)g_shadowScale;
        HRESULT shr = g_origCreateTexture(This, nW, nH, Levels, Usage, Format, Pool,
                                          ppTexture, pSharedHandle);
        if (SUCCEEDED(shr)) {
            InterlockedIncrement(&g_shadowScaled);
            // Only colour targets are ever bound as a render target and given
            // a viewport; the paired depth surfaces follow whatever the
            // colour target sets, so tracking those would add nothing.
            if (Usage & D3DUSAGE_RENDERTARGET) RememberShadowSurface(*ppTexture);
            char l[192];
            sprintf(l, "[shadow] scaled %ux%u -> %ux%u fmt=%d usage=0x%lX",
                    Width, Height, nW, nH, (int)Format, (unsigned long)Usage);
            LogLine(l);
            return shr;
        }
        // Out of VRAM or an unsupported size: fall through and create exactly
        // what was asked for, so a bad multiplier degrades to stock shadows
        // rather than a missing surface.
        InterlockedIncrement(&g_shadowScaleFail);
        char l[192];
        sprintf(l, "[shadow] scale %ux%u -> %ux%u FAILED hr=0x%08lX - using original size",
                Width, Height, nW, nH, (unsigned long)shr);
        LogLine(l);
    }
#endif  // ENABLE_SHADOW_SCALE
    // Levels==0 (auto full mip chain) and shared-handle requests are left to
    // the real path - see the texture pool's comment block for why. This is
    // a plain-flag toggle check on the hot path, and TryAcquireFromPool's own
    // scan/lock only runs for requests that already look poolable.
#if ENABLE_TEXTURE_POOL
    if (g_texturePoolEnabled && Levels != 0 && !pSharedHandle && IsPoolableTexture(Usage, Pool)) {
        IDirect3DTexture9 *pooled = NULL;
        if (TryAcquireFromPool(Width, Height, Levels, Format, Pool, Usage, &pooled)) {
            *ppTexture = pooled;
            return D3D_OK;
        }
    }
#endif
    // DEFAULT -> MANAGED rewrite. Only legal at all on a non-Ex device, hence
    // the ForceStdD3D9 gate. RENDERTARGET/DEPTHSTENCIL cannot be MANAGED, and
    // D3DUSAGE_DYNAMIC is mutually exclusive with it, so that bit is dropped.
    // The point: a MANAGED texture is locked against the runtime's own
    // system-memory copy with no GPU sync, which is the stall this whole
    // project exists to remove - and the runtime, not us, schedules the VRAM
    // upload. Our SYSTEMMEM staging redirect gates on Pool == D3DPOOL_DEFAULT,
    // so it steps aside on its own for anything rewritten here.
    // Name the creator of every half-presentation-size render target. Capped,
    // so this is a handful of lines at load and nothing thereafter. See the
    // g_backbufW declaration for why the creator matters more than the size.
    if (g_backbufW && (Usage & D3DUSAGE_RENDERTARGET) &&
        Width * 2 == g_backbufW && Height * 2 == g_backbufH &&
        InterlockedIncrement(&g_halfResLogged) <= 8) {
        DWORD ra = (DWORD)(UINT_PTR)_ReturnAddress();
        char hl[192];
        sprintf(hl, "[halfres] %ux%u fmt=%d usage=0x%lX immediate caller %08X",
                Width, Height, (int)Format, (unsigned long)Usage,
                (g_mainModBase && ra > g_mainModBase && ra < g_mainModBase + g_mainModSize)
                    ? ra - g_mainModBase + 0x00400000 : ra);
        LogLine(hl);
        // The immediate caller was FUN_00aa3960, a GENERIC render-target
        // creator - it receives dimensions that are already halved, and the
        // FUN_00aa3010 applied to them is only an align-up
        // ((x + n-1) & ~(n-1)), not a divide. Its own caller FUN_00aa3ce0 is
        // just a TextureImp constructor copying a descriptor. So the /2 is
        // further up a generic path shared by every texture in the engine,
        // which static caller-walking cannot narrow.
        //
        // A raw stack scan was tried here and FAILED, exactly as it failed
        // during the cascade hunt: it returned FUN_00b454a0, which is a memory
        // ALLOCATOR wrapper ("Memory allocation fail, %u bytes, for %s, in
        // %s."), i.e. stale residue from hot utility code rather than the live
        // chain. Frame-pointer omission means there is no EBP chain to walk and
        // scanning cannot tell a live return address from a dead one.
        //
        // Replaced by Detour_texImpCtor below, which hooks the TextureImp
        // constructor - it receives the descriptor with the dimensions ALREADY
        // decided, so its own return address is the game code that decided
        // them. Ask directly instead of guessing at the stack.
    }

    // (ShadowBufPct no longer intervenes here. Two compensation attempts at
    // this level both failed with the ShadowScale signature - scaling the D3D
    // texture alone broke the viewport, and scaling the viewport on a size
    // match broke the pre-Reset targets. Root cause found in FUN_00b00f10,
    // the screen-buffer allocator: the halving is inline `((dim+1)>>1)+1 & ~1`
    // and the result flows into the TEXTURE DESCRIPTOR, which the TextureImp
    // wrapper copies into its own fields - so scaling underneath the wrapper
    // left every consumer of the wrapper's recorded size (viewport, texel
    // maths) at the old dimensions. The intervention now happens in
    // OnTexImpCtor_C, where scaling the descriptor keeps the wrapper, the D3D
    // texture and everything derived from them consistent.)

    D3DPOOL origPool = Pool;
    DWORD origUsage = Usage;
    int rewrote = 0;
    if ((unsigned)origPool < 4) InterlockedIncrement(&g_texPoolRequested[origPool]);
#if ENABLE_MANAGED_POOL
    if (g_managedPoolEnabled && g_forceStdD3D9 && Pool == D3DPOOL_DEFAULT && !pSharedHandle &&
        !(Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        Pool = D3DPOOL_MANAGED;
        Usage &= ~D3DUSAGE_DYNAMIC;
        rewrote = 1;
    }
#endif
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCreateTexture(This, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
#if ENABLE_MANAGED_POOL
    if (rewrote) {
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_managedRewriteCount);
        } else {
            // Never let a guess here cost the game a texture: put the original
            // parameters back and create exactly what was asked for.
            InterlockedIncrement(&g_managedRewriteFail);
            hr = g_origCreateTexture(This, Width, Height, Levels, origUsage, Format, origPool,
                                     ppTexture, pSharedHandle);
        }
    }
#endif  // ENABLE_MANAGED_POOL
    if (g_pendingShadowCapture) {
        g_pendingShadowCapture = 0;
        if (SUCCEEDED(hr) && ppTexture && *ppTexture) {
            RememberShadowSurface(*ppTexture);
            char sl[160];
            sprintf(sl, "[shadowsig] tracked shadow RT %ux%u (surfaces now %ld)",
                    Width, Height, g_shadowSurfaceCount);
            LogLine(sl);
        }
    }
    // Render-target / depth-stencil textures: the actual inventory for both
    // the deferred-vs-forward question and finding the shadow map. Logged
    // here rather than at CreateRenderTarget because this engine never calls
    // that - see the note by g_rtTexLogged.
    if ((origUsage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
        InterlockedIncrement(&g_rtTexLogged) <= 96) {
        char l[224];
        sprintf(l, "[rt] RT-texture #%ld: %ux%u levels=%u usage=0x%lX fmt=%d pool=%d hr=0x%08lX",
                g_rtTexLogged, Width, Height, Levels, (unsigned long)origUsage,
                (int)Format, (int)origPool, (unsigned long)hr);
        LogLine(l);
    }
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_createTexCount);
        InterlockedExchangeAdd(&g_createTexSumUsec, us);
        if (us > g_createTexMaxUsec) g_createTexMaxUsec = us;
        // A single CreateTexture measured 18,733us - squarely in the 17-19.6ms
        // band every remaining gameplay hitch occupies, and consistent with
        // NtGdiDdDDICreateAllocation (kernel GPU memory allocation) appearing
        // in those same stutter records. Average is only 61us, so this is a
        // rare spike, not a uniform cost: log WHICH creations are slow before
        // considering any pooling/reuse scheme, since the answer decides
        // whether the fix is "pool one specific huge texture" or something
        // structural.
        if (us > 1000) {
            static volatile LONG slowTexLogged = 0;
            if (InterlockedIncrement(&slowTexLogged) <= 12) {
                char l[224];
                sprintf(l, "[d3d9] SLOW CreateTexture: %ldus  %ux%u levels=%u usage=0x%lX fmt=%d pool=%d",
                        us, Width, Height, Levels, (unsigned long)Usage, (int)Format, (int)Pool);
                LogLine(l);
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedDevicePresentEx(
    IDirect3DDevice9Ex *This, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
{
    if (g_msHasContent) { MsaaResolve((IDirect3DDevice9 *)This); MsaaRestoreDepth((IDirect3DDevice9 *)This); }
    if (g_msR32fHasContent) MsaaResolveR32f((IDirect3DDevice9 *)This);
    if (g_msDumpState == 2 && InterlockedCompareExchange(&g_msDumpState, 3, 2) == 2) {
        IDirect3DSurface9 *bb = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer((IDirect3DDevice9 *)This, 0, 0,
                                                     D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            DumpSurfaceToBmp((IDirect3DDevice9 *)This, bb, "msaa_backbuf.bmp");
            IDirect3DSurface9_Release(bb);
        }
    }
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origDevicePresentEx(This, pSourceRect, pDestRect, hDestWindowOverride,
                                       pDirtyRegion, dwFlags);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_presentCount);
        InterlockedExchangeAdd(&g_presentSumUsec, us);
        if (us > g_presentMaxUsec) g_presentMaxUsec = us;
    }
    return hr;
}

// ---- Real-device shader creation timing ----------------------------------
// `ZwWaitForAlertByThreadId` inside AMDXN32 is the single largest remaining
// stall bucket, and the textbook cause is driver-side shader compilation:
// D3D9 has no async compile path, so CreateVertexShader/CreatePixelShader
// hand DXBC to the driver, which compiles on worker threads and BLOCKS the
// caller until done.
//
// This has never been measured. Both methods live on the DEVICE vtable, and
// every device-level hook installed via the old throwaway-probe technique has
// been confirmed dead for the entire project - the same blind spot that hid
// Present, UpdateSurface and CreateTexture. Measured here on the REAL device.
//
// What this must answer before any fix is attempted:
//   - do shader creations happen DURING gameplay, or only at load?
//   - what does one cost, and how many are there?
// Only if they are frequent and expensive mid-gameplay is pre-warming (dump
// bytecode once, re-create during a loading screen so the later in-game
// create is a driver cache hit) worth building. Compilation cannot be
// eliminated - it can only be relocated - so its timing is the whole
// question.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVertexShader)(
    IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreatePixelShader)(
    IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
static PFN_CreateVertexShader g_origCreateVS = NULL;
static PFN_CreatePixelShader g_origCreatePS = NULL;
static volatile LONG g_vsCount=0, g_vsSumUsec=0, g_vsMaxUsec=0, g_vsMainCount=0;
static volatile LONG g_psCount=0, g_psSumUsec=0, g_psMaxUsec=0, g_psMainCount=0;

static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(
    IDirect3DDevice9 *This, const DWORD *pFunction, IDirect3DVertexShader9 **ppShader)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCreateVS(This, pFunction, ppShader);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_vsCount);
        InterlockedExchangeAdd(&g_vsSumUsec, us);
        if (us > g_vsMaxUsec) g_vsMaxUsec = us;
        if (isMain) InterlockedIncrement(&g_vsMainCount);
    }
    return hr;
}

// ---- Pixel-shader dump (FXAA hunt / shader injection foundation) ----------
// The game's built-in FXAA is not exposed in any menu - a scan of every
// Graphics_* setting name found FrameRate, TextureFiltering, Lighting,
// Shadowing, DepthOfField, Glare, ColorCorrection, Scaling, Resolution and
// Presentation, and nothing for antialiasing. So there is no value to write,
// unlike ShadowMapRes; killing FXAA means reaching the shader itself.
//
// DRAW_FILTER turned out NOT to be the AA pass - its handler opens with the
// same shadow-enable gate as DRAW_SHADOW ([0x0511558c]+0x2c/+0x2d), so it is
// shadow filtering. FXAA lives in DRAW_POST_EFFECT.
//
// Rather than guess which shader that is, dump them all. Each unique bytecode
// is written once to shaders/ps_<hash>.bin and logged with its size and tap
// count. FXAA is identifiable offline by shape: a fullscreen pass with many
// texture fetches (5-12+) that reads luminance. Once identified by hash, it
// can be replaced with a passthrough at creation time.
//
// This is also the foundation for the shader work already planned for the
// shadow filter, so it is built to be generally useful rather than
// FXAA-specific.
static volatile LONG g_psDumped = 0;
// 256 was EXACTLY hit, then 1024 was EXACTLY hit - the population is larger
// still, and both times the truncation was read as a complete census (the
// second time it produced a published "no post-AA exists" conclusion while an
// unmapped shader was drawing 9,010 fullscreen quads per session under the
// hash 00000000). 4096, and the report line says CAP HIT so a third
// truncation cannot masquerade as completeness.
#define PS_HASH_MAX 4096
static DWORD g_psHashes[PS_HASH_MAX];
static volatile LONG g_psHashCount = 0;
// Every created pixel shader, hash + tap count, so a DRAW can be attributed
// to a shader identity at report time. The kill-candidate hunt over the top
// tap-count shaders came back empty (tone-grade pass, glyph filter, and
// no-ops) - the real post-AA is either past the old 256 cap or low-tap
// (SM3 gradient-based), so candidates must come from what is actually BOUND
// during post-window fullscreen draws, not from static ranking.
// (PsMapEntry, g_psMap, the pointer lookup and the identify ranking are
// declared with the early tentative definitions near the top of this file -
// the GUI and the config table both reference them and sit above here.)
static void * volatile g_curPsObj = NULL;   // shader bound right now (render thread)

static LONG PsLookupSlot(void *p)
{
    LONG h = (LONG)(((UINT_PTR)p >> 4) & (PS_LOOKUP_SIZE - 1));
    for (LONG probe = 0; probe < PS_LOOKUP_SIZE; probe++) {
        LONG s = (h + probe) & (PS_LOOKUP_SIZE - 1);
        LONG v = g_psLookup[s];
        if (!v) return s;                                   // free slot
        if (g_psMap[v - 1].obj == p) return s;              // found
    }
    return -1;
}
static LONG PsIndexOf(void *p)
{
    if (!p) return -1;
    LONG s = PsLookupSlot(p);
    if (s < 0 || !g_psLookup[s]) return -1;
    return g_psLookup[s] - 1;
}
// Fullscreen-quad draws onto the scene-sized colour target during the post
// window, bucketed by shader object. Small: the post chain is a dozen passes.
// v2 gate. The first version required the target to be A8R8G8B8 (fmt 21) and
// the pass tag to be DRAW_FILTER - and came back with NOTHING above 2 taps,
// because the swap chain's backbuffer is fmt 22 (X8R8G8B8, straight from the
// Reset log) and a final post-AA writes THERE. The gate excluded the exact
// draw it was built to find. Now: any fullscreen-sized target, any format,
// any pass, provided the draw is a fullscreen QUAD (primCount <= 2) - and
// the format and pass are recorded per entry instead of assumed.
#define POST_PS_MAX 96
typedef struct {
    void *obj; LONG draws; LONG fmt; LONG w; LONG h; LONG passMask;
} PostPsSeen;
static PostPsSeen g_postPsSeen[POST_PS_MAX];
static volatile LONG g_postPsSeenCount = 0;

// D3D9 bytecode is a DWORD token stream terminated by 0x0000FFFF.
static UINT ShaderTokenBytes(const DWORD *p)
{
    UINT n = 0;
    if (!p) return 0;
    __try {
        while (n < 65536 && p[n] != 0x0000FFFF) n++;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (n + 1) * 4;
}

static void DumpPixelShader(const DWORD *pFunction)
{
    if (!pFunction) return;
    __try {
        UINT bytes = ShaderTokenBytes(pFunction);
        if (bytes < 8 || bytes > 262144) return;

        // FNV-1a over the bytecode: identity for dedup and the handle used to
        // name a shader for later replacement.
        DWORD h = 2166136261u;
        const unsigned char *b = (const unsigned char *)pFunction;
        for (UINT i = 0; i < bytes; i++) { h ^= b[i]; h *= 16777619u; }

        LONG n = g_psHashCount, i;
        if (n > PS_HASH_MAX) n = PS_HASH_MAX;
        for (i = 0; i < n; i++) if (g_psHashes[i] == h) return;   // already dumped
        if (n >= PS_HASH_MAX) return;
        g_psHashes[n] = h;
        g_psHashCount = n + 1;

        // Count texture fetches - the cheap FXAA discriminator. Opcode is in
        // the low 16 bits; 0x42 is TEX/TEXLD in ps_2.0+. Instruction length is
        // in bits 24-27 for shader model 2+.
        UINT taps = 0, dcls = 0;
        UINT toks = bytes / 4;
        for (UINT t = 1; t < toks; ) {
            DWORD tok = pFunction[t];
            if (tok == 0x0000FFFF) break;
            UINT op = tok & 0xFFFF;
            UINT len = (tok >> 24) & 0x0F;
            if (op == 0x42) taps++;
            if (op == 0x1F) dcls++;
            t += (len ? len + 1 : 1);
        }

        char dir[MAX_PATH], path[MAX_PATH];
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *slash = strrchr(dir, '\\');
        if (slash) *(slash + 1) = 0;
        strcat(dir, "shaders");
        CreateDirectoryA(dir, NULL);
        sprintf(path, "%s\\ps_%08X.bin", dir, h);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(pFunction, 1, bytes, f); fclose(f); }

        LONG cnt = InterlockedIncrement(&g_psDumped);
        char l[192];
        sprintf(l, "[shaderdump] ps_%08X.bin  %u bytes  taps=%u dcl=%u  (#%ld)",
                h, bytes, taps, dcls, cnt);
        LogLine(l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- FXAA removal ---------------------------------------------------------
// Identified offline from the 256 dumped shaders by constant fingerprint:
// ps_072C19AF is 952 bytes of ps_3_0 with THIRTEEN dependent taps of one
// sampler and the constant pair 1/8 + 1/16 - FXAA's EDGE_THRESHOLD and
// EDGE_THRESHOLD_MIN, in a shader far too small to be a material. (The
// runner-up, ps_90868A80, is its 1-tap Rec.601 luma prepass - harmless once
// the consumer is gone.)
//
// Removal is at BIND time, not creation: shader objects are immutable but the
// binding isn't, so recognising the object at creation and substituting a
// 1-instruction passthrough (tex2D of the same sampler/texcoord) at
// SetPixelShader makes the whole thing hot-togglable - and if the hash
// identification is WRONG, flipping the toggle shows exactly what the shader
// really was, in-place.
#include "passthrough_ps.h"
#include "tint_ps.h"
// The REAL FXAA shader, found by walking the identify list in game rather
// than by any static heuristic (all four of those failed). See the removal
// block in HookedSetPixelShader for the disassembly evidence.
#define FXAA_SHADER_HASH_REAL 0xA082B248u
// Known-identity shaders, recorded as they are confirmed:
//   ps_A082B248  FXAA (the post-process AA)
//   ps_1245A11B  colour correction / tonemap: 3D LUT + glare + dither
//   ps_DCD57A17  glyph outline filter (the HD GUI mod's lever)
//   ps_A26BF0E2  Lightning's hair
//   ps_8676670C  vegetation, 1-tap cutout - depth/prepass variant
//   ps_658CC589  vegetation/grass, also depth (tinting it blacks the ground)
// (Candidate hash table + labels are declared with the GUI block - the
// dropdown needs them and sits earlier in this file.)
typedef struct { void *obj; LONG cand; } PsKillObj;
#define PS_KILL_OBJ_MAX 64
static PsKillObj g_psKillObjs[PS_KILL_OBJ_MAX];
static volatile LONG g_psKillObjCount = 0;
static IDirect3DPixelShader9 *g_fxaaPassthrough = NULL;
static IDirect3DPixelShader9 *g_psTintObj = NULL;
static volatile LONG g_fxaaSubs = 0;

static DWORD PsFnv1a(const DWORD *pFunction)
{
    UINT bytes = ShaderTokenBytes(pFunction);
    if (bytes < 8 || bytes > 262144) return 0;
    DWORD h = 2166136261u;
    const unsigned char *b = (const unsigned char *)pFunction;
    for (UINT i = 0; i < bytes; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// Correct token walk. A COMMENT token (0xFFFE) carries its length in bits
// 16-30, NOT in bits 24-27 like an instruction - and every D3D9 shader opens
// with a CTAB comment holding the constant table. Walking that payload as
// code produced bogus opcode counts in the first version of this and in the
// offline scans built on the same mistake.
// Taps counted are TEXLD(66/0x42), TEXLDD(93/0x5D), TEXLDL(95/0x5F).
static UINT PsCountTaps(const DWORD *pFunction)
{
    UINT bytes = ShaderTokenBytes(pFunction);
    if (bytes < 8) return 0;
    UINT taps = 0, toks = bytes / 4, t = 1;
    while (t < toks) {
        DWORD tok = pFunction[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) {           // comment: skip its payload
            t += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        UINT op = tok & 0xFFFF;
        if (op == 0x42 || op == 0x5D || op == 0x5F) taps++;
        t += 1 + ((tok >> 24) & 0x0F);
    }
    return taps;
}

// ---- Alpha-to-coverage: synthesise a coverage-writing shader variant ------
// Vegetation, fences and barriers are alpha-TESTED cutouts. MSAA cannot
// smooth them (it samples geometry coverage, not the texture), and the vendor
// A2C render-state hack alone does nothing here for two reasons found by
// measurement: the engine never writes ALPHATESTENABLE (a2c counter stayed 0
// for whole sessions), and its cutout shaders end with
//
//     texld r0, v1, s0        ; r0.w = texture alpha
//     add  r1, r0.w, -c28.x   ; alpha - alphaTestThreshold
//     texkill r1              ; cutout, binary
//     ...
//     mov_pp oC0.w, c31.w     ; alpha OUT is a CONSTANT
//
// so even with A2C enabled there is no per-pixel alpha in the output for
// coverage to be derived from. The fix is to rewrite the bytecode:
//
//   1. before the texkill, stash the tested value (alpha - threshold)
//   2. drop the texkill - coverage 0 replaces it, and a zero-coverage pixel
//      writes neither colour nor depth, which is what the kill achieved
//   3. replace the constant alpha write with
//         mad_sat oC0.w, stashed, sharpness, 0.5
//      a ramp centred exactly where the alpha test used to flip, so the
//      silhouette lands in the same place but resolves across samples
//
// Done at CreatePixelShader for every shader matching the pattern, so the
// whole cutout family is covered without hand-authoring anything and without
// loading files from disk - the variants are synthesised in memory.
//
// D3D9 token encoding used below: instruction token has opcode in bits 0-15
// and length (DWORDs following) in bits 24-27; register tokens have bit 31
// set, register number in bits 0-10, and the register TYPE split across bits
// 28-30 (low 3) and 11-12 (high 2). Comment tokens (0xFFFE) carry their
// length in bits 16-30 - the trap that corrupted three earlier scans here.
#define D3DSIO_NOP_     0
#define D3DSIO_MOV_     1
#define D3DSIO_MAD_     4
#define D3DSIO_TEXKILL_ 65
#define D3DSIO_DEF_     81
#define D3DSPR_TEMP_     0
#define D3DSPR_CONST_    2
#define D3DSPR_COLOROUT_ 8

static DWORD RegTok(DWORD type, DWORD num)
{
    return 0x80000000u | (num & 0x7FF)
         | ((type & 7) << 28) | (((type >> 3) & 3) << 11);
}
static DWORD DstTok(DWORD type, DWORD num, DWORD mask, int saturate)
{
    return RegTok(type, num) | ((mask & 0xF) << 16) | (saturate ? 0x00100000u : 0u);
}
static DWORD SrcTok(DWORD type, DWORD num, DWORD swizzle)
{
    return RegTok(type, num) | ((swizzle & 0xFF) << 16);
}
// Swizzle byte: 2 bits per output component, replicated.
#define SWZ_XXXX 0x00
#define SWZ_YYYY 0x55
#define SWZ_WWWW 0xFF
#define MASK_W   0x8
#define MASK_ALL 0xF

// Returns 1 and fills out/outLen if the shader matched the cutout pattern.
static int BuildA2cVariant(const DWORD *src, DWORD *out, UINT outMax, UINT *outLen)
{
    UINT bytes = ShaderTokenBytes(src);
    if (bytes < 12) return 0;
    UINT toks = bytes / 4;

    // Pass 1: locate the first texkill, the last oC0.w write, the highest
    // temp register and the highest const register actually referenced.
    UINT killAt = 0, alphaOutAt = 0;
    DWORD killReg = 0;
    DWORD maxTemp = 0, maxConst = 0;
    int alphaOutIsConst = 0;   // oC0.w written by MOV from a CONSTANT?
    UINT t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT op = tok & 0xFFFF, len = (tok >> 24) & 0x0F;
        for (UINT k = 1; k <= len && t + k < toks; k++) {
            DWORD rt = src[t + k];
            if (!(rt & 0x80000000u)) continue;
            DWORD type = ((rt >> 28) & 7) | (((rt >> 11) & 3) << 3);
            DWORD num = rt & 0x7FF;
            if (type == D3DSPR_TEMP_ && num > maxTemp) maxTemp = num;
            if (type == D3DSPR_CONST_ && num > maxConst) maxConst = num;
        }
        if (op == D3DSIO_TEXKILL_ && !killAt && len >= 1) {
            killAt = t; killReg = src[t + 1];
        }
        if (len >= 1) {
            DWORD d = src[t + 1];
            if ((d & 0x80000000u)) {
                DWORD type = ((d >> 28) & 7) | (((d >> 11) & 3) << 3);
                if (type == D3DSPR_COLOROUT_ && (d & 0x7FF) == 0 && ((d >> 16) & MASK_W)) {
                    alphaOutAt = t;
                    // Is the alpha a CONSTANT? That is the opaque-cutout
                    // signature: the shader has no per-pixel alpha of its own
                    // and the silhouette is entirely the texkill's doing, so
                    // repurposing oC0.w for coverage costs nothing.
                    //
                    // If instead alpha is COMPUTED (from a texture or temp),
                    // the draw is alpha-BLENDED and that value is its blend
                    // factor - overwriting it destroys the transparency, which
                    // is exactly what happened to Lightning's hair.
                    alphaOutIsConst = 0;
                    if (op == D3DSIO_MOV_ && len >= 2) {
                        DWORD s0 = src[t + 2];
                        DWORD st = ((s0 >> 28) & 7) | (((s0 >> 11) & 3) << 3);
                        if (st == D3DSPR_CONST_) alphaOutIsConst = 1;
                    }
                }
            }
        }
        t += 1 + len;
    }
    // Not a cutout shader, or nothing to write coverage into.
    //
    // alphaOutIsConst is NOT used as a filter any more. It excluded every
    // shader that computes its alpha - which is most of them, and plausibly
    // the grass and barrier shaders that showed no effect at all. The real
    // question is whether the DRAW is blended, and that is a render state,
    // not a property of the bytecode: the bind path requires
    // ALPHABLENDENABLE to be FALSE, which keeps blended material (hair) safe
    // while letting opaque cutouts through however they compute alpha.
    if (!killAt || !alphaOutAt) return 0;
    (void)alphaOutIsConst;
    DWORD rFree = maxTemp + 1;
    DWORD cScale = maxConst + 1;
    if (rFree > 27 || cScale > 220) {           // no headroom; leave it alone
        InterlockedIncrement(&g_a2cSkipNoRoom);
        return 0;
    }

    // Pass 2: emit. Copy verbatim except at the three points of interest.
    UINT o = 0;
    if (outMax < toks + 16) return 0;
    out[o++] = src[0];                       // version token

    // Our own def goes first so it is valid wherever it is used. Register is
    // one past the highest the shader references, so it cannot collide with
    // anything the engine uploads for THIS shader.
    float sharp = (float)g_a2cSharpness;
    if (sharp < 1.0f) sharp = 1.0f;
    out[o++] = D3DSIO_DEF_ | (5u << 24);
    out[o++] = DstTok(D3DSPR_CONST_, cScale, MASK_ALL, 0);
    { float v0 = sharp, v1 = 0.5f, v2 = 0.0f, v3 = 1.0f;
      memcpy(&out[o + 0], &v0, 4); memcpy(&out[o + 1], &v1, 4);
      memcpy(&out[o + 2], &v2, 4); memcpy(&out[o + 3], &v3, 4); o += 4; }

    t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) {
            UINT clen = 1 + ((tok >> 16) & 0x7FFF);
            t += clen;                        // drop comments (CTAB etc.)
            continue;
        }
        UINT len = (tok >> 24) & 0x0F;
        if (t == killAt) {
            // Stash the tested value, then DROP the texkill. texkill's
            // operand is register-encoded; read component .x, which is what
            // the add wrote across all channels.
            DWORD type = ((killReg >> 28) & 7) | (((killReg >> 11) & 3) << 3);
            DWORD num = killReg & 0x7FF;
            out[o++] = D3DSIO_MOV_ | (2u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rFree, MASK_W, 0);
            out[o++] = SrcTok(type, num, SWZ_XXXX);
            t += 1 + len;
            continue;
        }
        if (t == alphaOutAt) {
            // Keep the original write, then OVERRIDE ONLY .w after it.
            // The first version REPLACED this instruction, which is correct
            // only when it writes alpha alone. Character shaders commonly end
            // with `mov oC0, rN` writing all four channels, so replacing it
            // discarded RGB entirely - that is what turned NPCs into black
            // silhouettes. Appending touches nothing but alpha.
            for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
            // coverage = saturate((alpha - threshold) * sharpness + 0.5)
            out[o++] = D3DSIO_MAD_ | (4u << 24);
            out[o++] = DstTok(D3DSPR_COLOROUT_, 0, MASK_W, 1);
            out[o++] = SrcTok(D3DSPR_TEMP_, rFree, SWZ_WWWW);
            out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_XXXX);
            out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_YYYY);   // the 0.5
            // DEBUG VISUALISE: also write the coverage into RGB. The variants
            // are provably bound (hundreds of thousands of times) yet nothing
            // changes on screen, which means either the rewritten shader is
            // not really executing or the vendor coverage state is inert.
            // Painting coverage as colour separates those two outright: if
            // foliage turns into a grey ramp, the shader IS running and the
            // A2C state is the dead part. Build-time flag, so it is set in the
            // ini before launch.
            if (g_a2cDebugVis) {
                out[o++] = D3DSIO_MAD_ | (4u << 24);
                out[o++] = DstTok(D3DSPR_COLOROUT_, 0, 0x7 /* xyz */, 1);
                out[o++] = SrcTok(D3DSPR_TEMP_, rFree, SWZ_WWWW);
                out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_XXXX);
                out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_YYYY);
            }
            t += 1 + len;
            continue;
        }
        for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
        t += 1 + len;
    }
    out[o++] = 0x0000FFFF;
    *outLen = o;
    return 1;
}

// ---- Foliage SSAA variant -------------------------------------------------
// Alpha-to-coverage is dead on this driver, but per-sample masking works
// (proven: restricting foliage to one of eight samples visibly changed the
// resolved image). So the cutout can be supersampled directly:
//
//   for each sample i:  MULTISAMPLEMASK = 1<<i
//                       cOff = sub-pixel offset of sample i
//                       draw
//
// with the shader sampling its alpha texture at that sub-pixel offset. Each
// pass evaluates the SAME binary cutout at a different position inside the
// pixel, and the MSAA resolve averages them - real supersampling of the alpha
// test, on foliage draws only.
//
// The offset is applied in TEXTURE space via screen-space derivatives, so it
// stays correct at any distance and needs no access to the projection matrix:
//
//   dsx rDx, uv        ; how much uv changes across one pixel horizontally
//   dsy rDy, uv
//   mad rUV, rDx, cOff.x, uv
//   mad rUV, rDy, cOff.y, rUV
//   texld dst, rUV, s  ; the original fetch, at the jittered position
//
// texkill is deliberately KEPT here - unlike the A2C rewrite, this wants the
// original binary cutout, just evaluated N times per pixel.
static int BuildSsaaVariant(const DWORD *src, DWORD *out, UINT outMax,
                            UINT *outLen, DWORD *offsetReg)
{
    UINT bytes = ShaderTokenBytes(src);
    if (bytes < 12) return 0;
    UINT toks = bytes / 4;

    UINT firstTexld = 0, killAt = 0;
    DWORD maxTemp = 0, maxConst = 0;
    UINT t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT op = tok & 0xFFFF, len = (tok >> 24) & 0x0F;
        for (UINT k = 1; k <= len && t + k < toks; k++) {
            DWORD rt = src[t + k];
            if (!(rt & 0x80000000u)) continue;
            DWORD type = ((rt >> 28) & 7) | (((rt >> 11) & 3) << 3);
            DWORD num = rt & 0x7FF;
            if (type == D3DSPR_TEMP_ && num > maxTemp) maxTemp = num;
            if (type == D3DSPR_CONST_ && num > maxConst) maxConst = num;
        }
        if (op == 0x42 && !firstTexld && len >= 3) firstTexld = t;
        if (op == D3DSIO_TEXKILL_ && !killAt) killAt = t;
        t += 1 + len;
    }
    // Only worth doing for an alpha-tested shader whose cutout comes from the
    // first fetch - which is the shape every foliage shader here has.
    if (!firstTexld || !killAt) return 0;
    DWORD rUV = maxTemp + 1, rDx = maxTemp + 2, rDy = maxTemp + 3;
    DWORD cOff = maxConst + 1;
    if (rDy > 27 || cOff > 220) return 0;
    if (outMax < toks + 24) return 0;

    UINT o = 0;
    out[o++] = src[0];
    t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT len = (tok >> 24) & 0x0F;
        if (t == firstTexld) {
            DWORD dst = src[t + 1], uv = src[t + 2], smp = src[t + 3];
            // Derivatives of the ORIGINAL uv, then offset along them.
            out[o++] = 0x5Bu | (2u << 24);                    // dsx
            out[o++] = DstTok(D3DSPR_TEMP_, rDx, MASK_ALL, 0);
            out[o++] = uv;
            out[o++] = 0x5Cu | (2u << 24);                    // dsy
            out[o++] = DstTok(D3DSPR_TEMP_, rDy, MASK_ALL, 0);
            out[o++] = uv;
            out[o++] = D3DSIO_MAD_ | (4u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rUV, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rDx, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cOff, SWZ_XXXX);
            out[o++] = uv;
            out[o++] = D3DSIO_MAD_ | (4u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rUV, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rDy, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cOff, SWZ_YYYY);
            out[o++] = SrcTok(D3DSPR_TEMP_, rUV, 0xE4);
            // The original fetch, now reading the jittered coordinate.
            out[o++] = tok;
            out[o++] = dst;
            out[o++] = SrcTok(D3DSPR_TEMP_, rUV, 0xE4);
            out[o++] = smp;
            t += 1 + len;
            continue;
        }
        for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
        t += 1 + len;
    }
    out[o++] = 0x0000FFFF;
    *outLen = o;
    *offsetReg = cOff;
    return 1;
}

// ---- Foliage FRINGE variant ----------------------------------------------
// Last credible in-engine route to smooth cutouts, and the standard one when
// alpha-to-coverage is unavailable (which it is: both vendor hacks are dead
// on this driver, proven with a coverage ramp wide enough to be unmissable).
//
// The engine's own draw stays exactly as it is and paints the solid interior.
// Then the SAME geometry is drawn again with ordinary alpha blending, using a
// shader that keeps ONLY the transition band around the alpha threshold and
// outputs a smooth alpha across it. The result is an antialiased edge built
// from blending, which no driver feature can refuse to honour.
//
//   v      = (alpha - threshold) * sharpness + 0.5     [unsaturated]
//   kill v < 0        -> outside the shape entirely
//   kill (1 - v) < 0  -> solid interior, already drawn by the engine's pass
//   oC0.w = saturate(v)                                -> the fringe ramp
//
// Both kills write all components: texkill tests x, y AND z, so leaving any
// of them holding garbage would discard pixels at random.
static int BuildFringeVariant(const DWORD *src, DWORD *out, UINT outMax, UINT *outLen)
{
    UINT bytes = ShaderTokenBytes(src);
    if (bytes < 12) return 0;
    UINT toks = bytes / 4;

    UINT killAt = 0, alphaOutAt = 0;
    DWORD killReg = 0, maxTemp = 0, maxConst = 0;
    UINT t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT op = tok & 0xFFFF, len = (tok >> 24) & 0x0F;
        for (UINT k = 1; k <= len && t + k < toks; k++) {
            DWORD rt = src[t + k];
            if (!(rt & 0x80000000u)) continue;
            DWORD ty = ((rt >> 28) & 7) | (((rt >> 11) & 3) << 3);
            DWORD nu = rt & 0x7FF;
            if (ty == D3DSPR_TEMP_ && nu > maxTemp) maxTemp = nu;
            if (ty == D3DSPR_CONST_ && nu > maxConst) maxConst = nu;
        }
        if (op == D3DSIO_TEXKILL_ && !killAt && len >= 1) { killAt = t; killReg = src[t + 1]; }
        if (len >= 1) {
            DWORD d = src[t + 1];
            if (d & 0x80000000u) {
                DWORD ty = ((d >> 28) & 7) | (((d >> 11) & 3) << 3);
                if (ty == D3DSPR_COLOROUT_ && (d & 0x7FF) == 0 && ((d >> 16) & MASK_W))
                    alphaOutAt = t;
            }
        }
        t += 1 + len;
    }
    if (!killAt || !alphaOutAt) return 0;
    DWORD rStash = maxTemp + 1, rA = maxTemp + 2, rB = maxTemp + 3,
          rT = maxTemp + 4, cS = maxConst + 1;
    if (rT > 27 || cS > 220) return 0;
    if (outMax < toks + 48) return 0;

    UINT o = 0;
    out[o++] = src[0];
    out[o++] = D3DSIO_DEF_ | (5u << 24);
    out[o++] = DstTok(D3DSPR_CONST_, cS, MASK_ALL, 0);
    // .x unused now (the fixed sharpness it held is what made this fail at
    // distance), .y = 0.5, .z = epsilon so a flat region cannot divide by
    // zero, .w = 1.0
    { float a = 0.0f, b = 0.5f, c = 1e-5f, d = 1.0f;
      memcpy(&out[o+0], &a, 4); memcpy(&out[o+1], &b, 4);
      memcpy(&out[o+2], &c, 4); memcpy(&out[o+3], &d, 4); o += 4; }

    t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT len = (tok >> 24) & 0x0F;
        if (t == killAt) {
            DWORD ty = ((killReg >> 28) & 7) | (((killReg >> 11) & 3) << 3);
            DWORD nu = killReg & 0x7FF;
            // v = alpha - threshold, replicated so texkill's x/y/z all agree.
            out[o++] = D3DSIO_MOV_ | (2u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rStash, MASK_ALL, 0);
            out[o++] = SrcTok(ty, nu, SWZ_XXXX);
            // fwidth(v) = |ddx(v)| + |ddy(v)|. This is the whole fix: it
            // scales with minification, so the coverage ramp stays ~1 pixel
            // wide at any distance. A FIXED band (what this used before)
            // collapses below a pixel exactly where foliage aliases worst.
            out[o++] = 0x5Bu | (2u << 24);                    // dsx
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rStash, 0xE4);
            out[o++] = 0x5Cu | (2u << 24);                    // dsy
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rStash, 0xE4);
            out[o++] = 0x23u | (2u << 24);                    // abs
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, 0xE4);
            out[o++] = 0x23u | (2u << 24);                    // abs
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rB, 0xE4);
            out[o++] = D3DSIO_ADD_ | (3u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, 0xE4);
            out[o++] = SrcTok(D3DSPR_TEMP_, rB, 0xE4);
            out[o++] = D3DSIO_ADD_ | (3u << 24);              // + epsilon
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cS, 0xAA);       // .zzzz
            out[o++] = 0x06u | (2u << 24);                    // rcp
            out[o++] = DstTok(D3DSPR_TEMP_, rA, 0x1 /* .x */, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, SWZ_XXXX);
            // t = v / fwidth + 0.5
            out[o++] = 0x05u | (3u << 24);                    // mul
            out[o++] = DstTok(D3DSPR_TEMP_, rT, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rStash, 0xE4);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, SWZ_XXXX);
            out[o++] = D3DSIO_ADD_ | (3u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rT, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rT, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cS, SWZ_YYYY);
            out[o++] = D3DSIO_TEXKILL_ | (1u << 24);          // t < 0: outside
            out[o++] = DstTok(D3DSPR_TEMP_, rT, MASK_ALL, 0);
            out[o++] = D3DSIO_ADD_ | (3u << 24);              // 1 - t
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_CONST_, cS, SWZ_WWWW);
            out[o++] = SrcTok(D3DSPR_TEMP_, rT, 0xE4) | 0x01000000u;
            out[o++] = D3DSIO_TEXKILL_ | (1u << 24);          // interior
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            t += 1 + len;
            continue;
        }
        if (t == alphaOutAt) {
            for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
            // Blend alpha = the derivative-normalised coverage computed above.
            out[o++] = D3DSIO_MOV_ | (2u << 24);
            out[o++] = DstTok(D3DSPR_COLOROUT_, 0, MASK_W, 1);
            out[o++] = SrcTok(D3DSPR_TEMP_, rT, SWZ_XXXX);
            t += 1 + len;
            continue;
        }
        for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
        t += 1 + len;
    }
    out[o++] = 0x0000FFFF;
    *outLen = o;
    return 1;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(
    IDirect3DDevice9 *This, const DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    if (g_dumpShaders) DumpPixelShader(pFunction);
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCreatePS(This, pFunction, ppShader);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_psCount);
        InterlockedExchangeAdd(&g_psSumUsec, us);
        if (us > g_psMaxUsec) g_psMaxUsec = us;
        if (isMain) InterlockedIncrement(&g_psMainCount);
    }
    // Recognise every kill-candidate shader by bytecode hash, whatever object
    // the engine wraps it in this session, and stage the substitutes on the
    // first sight of any.
    if (SUCCEEDED(hr) && ppShader && *ppShader && pFunction) {
        DWORD h = PsFnv1a(pFunction);
        // The passthrough is needed by the FXAA toggle whether or not any
        // kill-list candidate was ever created, so build it on first sight of
        // the FXAA shader too.
        if (h == FXAA_SHADER_HASH_REAL) {
            if (!g_fxaaPassthrough)
                g_origCreatePS(This, (const DWORD *)g_psPassthrough, &g_fxaaPassthrough);
            char fl[144];
            sprintf(fl, "[fxaa] FXAA shader ps_%08X created; passthrough %s", h,
                    g_fxaaPassthrough ? "ready" : "FAILED");
            LogLine(fl);
        }
        // Alpha-to-coverage variant, synthesised now so the bind path is a
        // pointer lookup. Built unconditionally (cheap, once per shader) so
        // the feature can be toggled at runtime without a relaunch.
        // Gated on the feature flag AND on the allow-list. Building a variant
        // for every cutout shader meant thousands of rewrites per session
        // handed to the driver for shaders we never intended to change; now
        // only the confirmed foliage shaders are touched at all, which keeps
        // the blast radius equal to the feature's actual scope.
        // EITHER feature needs the variants built. The SSAA variant is
        // constructed in this same block, so gating the block on A2cEnable
        // alone meant turning A2C off (as the SSAA test did) silently built
        // nothing and the multi-draw path had no shader to bind - it reported
        // multiDraws=0 and looked exactly like the technique failing.
        int a2cWanted = 0;
#if ENABLE_CUTOUT_AA
        if (g_a2cEnable || g_ssaaFoliage || g_fringeFoliage) {
            for (size_t a = 0; a < A2C_ALLOW_COUNT; a++)
                if (g_a2cAllow[a] == h) { a2cWanted = 1; break; }
        }
#endif
        // Each variant is built INDEPENDENTLY. They used to be nested inside
        // the A2C variant's success branch, so a failure - or simply having
        // A2C switched off - silently produced no fringe and no SSAA shader
        // either, and the features reported "0 draws" exactly as though the
        // technique had failed. One slot, three optional variants.
        if (a2cWanted && g_a2cVariantCount < A2C_MAX) {
            LONG n = InterlockedIncrement(&g_a2cVariantCount) - 1;
            if (n >= A2C_MAX) {
                g_a2cVariantCount = A2C_MAX;
            } else {
                // STACK buffers, not static: several loader threads compile
                // shaders at once and a shared scratch would interleave.
                DWORD buf[4096];
                UINT len = 0, offReg = 0;
                IDirect3DPixelShader9 *sh = NULL;
                char l[192];

                g_a2cVariants[n].orig = (void *)*ppShader;
                g_a2cVariants[n].hash = h;
                g_a2cVariants[n].draws = 0;
                if (!g_psTintObj)
                    g_origCreatePS(This, (const DWORD *)g_psTint, &g_psTintObj);

                if (BuildA2cVariant(pFunction, buf, 4096, &len) &&
                    SUCCEEDED(g_origCreatePS(This, buf, &sh)) && sh) {
                    g_a2cVariants[n].variant = (void *)sh;
                } else if (InterlockedIncrement(&g_a2cBuildFails) <= 4) {
                    sprintf(l, "[a2c] variant build/create FAILED for ps_%08X", h);
                    LogLine(l);
                }
                sh = NULL; len = 0;
                if (BuildFringeVariant(pFunction, buf, 4096, &len) &&
                    SUCCEEDED(g_origCreatePS(This, buf, &sh)) && sh) {
                    g_a2cVariants[n].fringe = (void *)sh;
                    if (n < 6) {
                        sprintf(l, "[fringe] variant built for ps_%08X (%u tokens)", h, len);
                        LogLine(l);
                    }
                } else if (InterlockedIncrement(&g_fringeBuildFails) <= 4) {
                    sprintf(l, "[fringe] variant build/create FAILED for ps_%08X", h);
                    LogLine(l);
                }
                sh = NULL; len = 0;
                if (BuildSsaaVariant(pFunction, buf, 4096, &len, &offReg) &&
                    SUCCEEDED(g_origCreatePS(This, buf, &sh)) && sh) {
                    g_a2cVariants[n].ssaa = (void *)sh;
                    g_a2cVariants[n].ssaaOffReg = offReg;
                    g_ssaaOffsetReg = (LONG)offReg;
                } else if (InterlockedIncrement(&g_ssaaBuildFails) <= 4) {
                    sprintf(l, "[ssaa] variant build/create FAILED for ps_%08X", h);
                    LogLine(l);
                }
                if (n < 6) {
                    sprintf(l, "[a2c] slot #%ld for ps_%08X: a2c=%s fringe=%s ssaa=%s",
                            n + 1, h,
                            g_a2cVariants[n].variant ? "yes" : "no",
                            g_a2cVariants[n].fringe  ? "yes" : "no",
                            g_a2cVariants[n].ssaa    ? "yes" : "no");
                    LogLine(l);
                }
            }
        }
        // Identity map for draw attribution, every shader.
        // Reserve the slot atomically: shader creation runs on several
        // threads here (3000+ compiles at boot across worker threads), so
        // "read count, write, store count+1" hands two threads the same slot
        // and corrupts both the map and the pointer lookup.
        LONG mi = InterlockedIncrement(&g_psMapCount) - 1;
        if (mi < PS_MAP_MAX) {
            g_psMap[mi].obj = (void *)*ppShader;
            g_psMap[mi].hash = h;
            g_psMap[mi].taps = PsCountTaps(pFunction);
            g_psMap[mi].draws = 0;
            g_psMap[mi].prevDraws = 0;
            g_psMap[mi].recent = 0;
            LONG s = PsLookupSlot((void *)*ppShader);
            if (s >= 0) g_psLookup[s] = mi + 1;
        } else {
            g_psMapCount = PS_MAP_MAX;      // clamp, do not wrap
        }
        for (size_t ci = 0; ci < NUM_PS_KILL; ci++) {
            if (g_psKillCandidates[ci] != h) continue;
            LONG n = g_psKillObjCount;
            if (n < PS_KILL_OBJ_MAX) {
                g_psKillObjs[n].obj = (void *)*ppShader;
                g_psKillObjs[n].cand = (LONG)ci;
                g_psKillObjCount = n + 1;
            }
            if (!g_fxaaPassthrough)
                g_origCreatePS(This, (const DWORD *)g_psPassthrough, &g_fxaaPassthrough);
            if (!g_psTintObj)
                g_origCreatePS(This, (const DWORD *)g_psTint, &g_psTintObj);
            char l[160];
            sprintf(l, "[fxaa] kill candidate %u (ps_%08X) created: obj %p (%ld tracked)",
                    (unsigned)(ci + 1), h, (void *)*ppShader, g_psKillObjCount);
            LogLine(l);
            break;
        }
    }
    return hr;
}

// Bind-time substitution - one pointer compare per SetPixelShader when the
// toggle is off, a tiny loop when on.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetPixelShader)(
    IDirect3DDevice9 *, IDirect3DPixelShader9 *);
static PFN_SetPixelShader g_origSetPixelShader = NULL;

// Tentative definitions: the A2C bind path needs the MS colour surface and
// the SetRenderState original, both of which are defined with the MSAA block
// further down. Same pattern as the other early blocks in this file.
static IDirect3DSurface9 *g_msColour;
static volatile LONG g_msActive;
static HRESULT (STDMETHODCALLTYPE *g_origSetRenderState)(
    IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);

static HRESULT STDMETHODCALLTYPE HookedSetPixelShader(
    IDirect3DDevice9 *This, IDirect3DPixelShader9 *pShader)
{
    g_curPsObj = (void *)pShader;
    g_curPsIdx = PsIndexOf((void *)pShader);

    // ---- General shader identify ----------------------------------------
    // Deliberately NOT restricted to cutout candidates or to blending being
    // off: the previous version was, which is why stepping it never lit up
    // the grass - the grass shader was never in that set. This covers every
    // pixel shader the game binds, ranked by draws in the LAST WINDOW so the
    // list reflects what is on screen right now.
    // ---- FXAA off -------------------------------------------------------
    // ps_A082B248 IS the game's anti-aliasing pass, identified from its
    // disassembly: 9 taps of the scene sampler, 4 of them diagonal
    // neighbours, luma carried in alpha, local luma range -> edge strength
    // via pow(range, 0.25), edge DIRECTION from the diagonal luma
    // differences, normalised and clamped to +/-2 texels, then samples along
    // that direction and cmp-selects blended vs original. Textbook FXAA.
    //
    // It carries no texel-size literals - the offsets arrive at runtime in
    // c0 ($s_neighborhoodValue) - which is why every constant-fingerprint
    // search for it came back empty.
    //
    // Substituting the plain centre-tap passthrough removes the filter while
    // leaving the pass, its target and everything downstream intact.
    // psIdx is SNAPSHOTTED into a local before use. g_curPsIdx is a global
    // shared by every thread that binds shaders, so re-reading it between the
    // bounds check and the array index is a race: another thread setting it
    // to -1 in that gap indexes g_psMap[-1]. That is what crashed the game on
    // boot, during the ~3200 shader creations where binds are densest.
    LONG psIdx = g_curPsIdx;
    if (psIdx < 0 || psIdx >= g_psMapCount || psIdx >= PS_MAP_MAX) psIdx = -1;

    if (g_fxaaOff && g_fxaaPassthrough && psIdx >= 0 &&
        g_psMap[psIdx].hash == FXAA_SHADER_HASH_REAL) {
        InterlockedIncrement(&g_fxaaSubs);
        return g_origSetPixelShader(This, g_fxaaPassthrough);
    }

    // Selection is the shader's CREATION index, which never changes for the
    // life of the process. Ranking by draw activity made entries swap places
    // while walking the list, which made it unusable; a stable list can be
    // stepped through quickly even if it is long.
    if (g_psIdentify > 0 && g_psTintObj && psIdx >= 0 &&
        psIdx == g_psIdentify - 1)
        return g_origSetPixelShader(This, g_psTintObj);

    // ---- Alpha-to-coverage bind path ------------------------------------
    // g_msActive, not merely "MSAA is on": the variant may ONLY be bound while
    // our multisampled colour target is the current render target. The first
    // version gated on the feature being enabled, so the rewritten shaders
    // were also used for the SHADOW pass - which renders into a plain
    // non-multisampled shadow map where coverage does nothing, while the
    // texkill the transform removes is the only thing making foliage shadows
    // see-through. Result: solid-block vegetation shadows.
    // The vendor A2C state is enabled only while a rewritten shader is
    // current and switched off immediately afterwards.
    if (g_msActive && pShader && !g_alphaBlendOn && g_a2cVariantCount) {
        LONG n = g_a2cVariantCount, i;
        if (n > A2C_MAX) n = A2C_MAX;
        for (i = 0; i < n; i++) {
            if (g_a2cVariants[i].orig != (void *)pShader) continue;
            InterlockedIncrement(&g_a2cVariants[i].draws);
            // (Identify now happens earlier in this function, over ALL
            // shaders rather than only cutout candidates - see g_psIdentify.)
            // Allow-list only. An unlisted shader is left completely alone,
            // which is why this can no longer damage NPCs or water.
            if (!g_a2cEnable || !g_gpuVendor || g_msaaSamples < 2) break;
            int allowed = 0;
            for (size_t a = 0; a < A2C_ALLOW_COUNT; a++)
                if (g_a2cAllow[a] && g_a2cAllow[a] == g_a2cVariants[i].hash) { allowed = 1; break; }
            if (!allowed) break;
            if (!g_a2cStateOn) {
                // Several drivers gate alpha-to-coverage on ALPHATESTENABLE
                // being TRUE - NVIDIA's ATOC documents it outright, and AMD's
                // A2M is reported to want it too. The engine never enables
                // alpha test (it cuts out with texkill instead), so the state
                // was FALSE for every one of the 329,954 substituted binds,
                // which would make the coverage hack silently inert. ALWAYS
                // as the compare function keeps it from rejecting anything.
                g_origSetRenderState(This, D3DRS_ALPHATESTENABLE, TRUE);
                g_origSetRenderState(This, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
                g_origSetRenderState(This, D3DRS_MULTISAMPLEANTIALIAS, TRUE);
                // BOTH vendor dialects, unconditionally. The AMD 'A2M' hack
                // is from ~2007 and current drivers may simply ignore it;
                // some AMD drivers are reported to accept NVIDIA's 'ATOC'
                // spelling as well. Setting the wrong one is harmless - the
                // driver that does not recognise it treats the write as an
                // ordinary (and unused) render state - so trying both costs
                // nothing and removes a guess.
                g_origSetRenderState(This, D3DRS_POINTSIZE, MAKEFOURCC('A','2','M','1'));
                g_origSetRenderState(This, D3DRS_ADAPTIVETESS_Y, MAKEFOURCC('A','T','O','C'));
                // ---- MULTISAMPLEMASK feasibility probe ---------------------
                // Alpha-to-coverage is dead on this driver (both vendor
                // dialects ignored, proven with a coverage ramp so wide it
                // could not have been missed). The remaining route to smooth
                // cutouts is sample-masked jittered multi-draw: draw foliage
                // once per sample with the projection nudged by that sample's
                // sub-pixel offset, letting the MSAA resolve average them -
                // real supersampling of the alpha test, on foliage only.
                //
                // That is a lot of machinery to build on an assumption, and
                // the last assumption of this kind (the vendor A2C hack) cost
                // a day. So probe the mechanism first: restrict foliage to
                // ONE of the N samples. If the mask works, foliage renders at
                // 1/N coverage and comes out of the resolve obviously faint.
                // If it looks completely normal, per-sample control is not
                // available either and the approach is dead before it is
                // written.
                if (g_a2cMaskTest)
                    g_origSetRenderState(This, D3DRS_MULTISAMPLEMASK, 0x1);
                g_a2cStateOn = 1;
            }
            InterlockedIncrement(&g_a2cBinds);
            return g_origSetPixelShader(This,
                       (IDirect3DPixelShader9 *)g_a2cVariants[i].variant);
        }
    }
    // Count the case where a rewritten shader WAS current material but the
    // draw is blended - distinguishes "never matched" from "matched but
    // always rejected at bind", which are very different failures.
    if (g_a2cEnable && g_alphaBlendOn && pShader && g_a2cVariantCount) {
        LONG n = g_a2cVariantCount, i;
        if (n > A2C_MAX) n = A2C_MAX;
        for (i = 0; i < n; i++)
            if (g_a2cVariants[i].orig == (void *)pShader) {
                InterlockedIncrement(&g_a2cBlockedBlend);
                break;
            }
    }
    if (g_a2cStateOn) {
        g_origSetRenderState(This, D3DRS_POINTSIZE, MAKEFOURCC('A','2','M','0'));
        g_origSetRenderState(This, D3DRS_ADAPTIVETESS_Y, D3DFMT_UNKNOWN);
        if (g_a2cMaskTest)
            g_origSetRenderState(This, D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
        // Put alpha test back where the engine had it. It never enables it,
        // so FALSE is the correct restore - but track it anyway rather than
        // assume, since leaving alpha test on would silently change every
        // draw that follows.
        g_origSetRenderState(This, D3DRS_ALPHATESTENABLE, g_alphaTestWasOn ? TRUE : FALSE);
        g_a2cStateOn = 0;
    }

    LONG pick = g_fxaaPick;
    if (pick >= 1 && pShader && g_fxaaPassthrough) {
        LONG n = g_psKillObjCount, i;
        if (n > PS_KILL_OBJ_MAX) n = PS_KILL_OBJ_MAX;
        for (i = 0; i < n; i++) {
            if (g_psKillObjs[i].obj == (void *)pShader &&
                g_psKillObjs[i].cand == pick - 1) {
                InterlockedIncrement(&g_fxaaSubs);
                // MsaaDebugClear doubles as the footprint instrument: solid
                // magenta instead of a passthrough shows exactly which pixels
                // this pass writes, which is how the real AA pass gets found.
                return g_origSetPixelShader(This,
                    (g_msaaDebugClear && g_psTintObj) ? g_psTintObj : g_fxaaPassthrough);
            }
        }
    }
    return g_origSetPixelShader(This, pShader);
}

// ---- Render-target inventory probe (AA + shadow feature work) ------------
// Answers two questions at once, both currently resting on assumption:
//
// 1. IS THIS ENGINE DEFERRED? I claimed it was (from era + heavy
//    post-processing) with no evidence. User pushed back: XIII-1 and XIII-2
//    both force MSAA and their transparency effects depend on it, which is a
//    FORWARD-renderer signature, and LR shares that engine lineage
//    (SQEX::CDev::Engine::Dw). Decisive test: a deferred renderer binds
//    multiple render targets at once (MRT / G-buffer). Tracking the highest
//    RenderTargetIndex ever passed to SetRenderTarget settles it - index 0
//    only means forward, index 1+ means MRT.
//    If forward, real MSAA becomes plausible instead of "probably breaks".
//
// 2. WHERE IS THE SHADOW MAP? Raising shadow resolution means finding the
//    surface first. Logging every render target / depth-stencil creation
//    with full parameters makes it identifiable by signature (square,
//    depth or single-channel float, created once at load).
//
// Also logs whether the game ASKS for MSAA at device creation, which tells
// us whether an MSAA path exists in its own code at all.
//
// SetRenderTarget is extremely hot, so that hook is one compare against a
// running max on the common path - no logging, no lock.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRenderTarget)(
    IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
    IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDepthStencil)(
    IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
    IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)(
    IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
// SetViewport RE-ADDED, log-only. It was retired after crashing the game at
// startup while trying to COMPENSATE enlarged shadow maps; that crash was
// never diagnosed, so this version does nothing but observe:
//   - installed with the identical pattern to SetRenderTarget, which is proven
//   - body is a pure pass-through unless g_logPassRts is on
//   - no scaling, no state, no early return
//
// It answers a specific question. Scaling the screen-space shadow buffer gave
// blocky+shifted shadows at 50% and a uniformly dark world at 200%, both of
// which mean something still assumes 1920x1080. A viewport left at the old
// size would produce exactly the 200% symptom: render into part of a larger
// surface, sample all of it, read "shadowed" everywhere.
//
// Note D3D9 resets the viewport to the full render target on every
// SetRenderTarget, so if the engine never calls SetViewport the viewport is
// automatically correct at any size - and the fault is the shader's UV scale
// instead. Either answer tells us where to aim next.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(
    IDirect3DDevice9 *, const D3DVIEWPORT9 *);
static PFN_SetViewport g_origSetViewport = NULL;
#define VP_SEEN_MAX 64
typedef struct { LONG pass; DWORD w, h, x, y; } VpSeen;
static VpSeen g_vpSeen[VP_SEEN_MAX];
static volatile LONG g_vpSeenCount = 0;

static HRESULT STDMETHODCALLTYPE HookedSetViewport(
    IDirect3DDevice9 *This, const D3DVIEWPORT9 *pVp)
{
    // LOG-ONLY, again and deliberately. A scaling version lived here for one
    // build and made things worse: it matched on the viewport's SIZE, which
    // caught every 1920x1080 viewport - including those belonging to targets
    // created before the Reset that were never enlarged - so the mismatch was
    // recreated for everything except the three surfaces it meant to fix.
    //
    // The real fix scales the texture DESCRIPTOR before the TextureImp wrapper
    // records it (OnTexImpCtor_C): the engine then computes the enlarged
    // viewport itself from its own bookkeeping. The [vp] lines below are how
    // that is verified rather than assumed.
    if (g_logPassRts && pVp) {
        LONG p = g_curPass;
        if (p < 0 || p >= PASS_COUNT) p = PASS_NONE;
        LONG n = g_vpSeenCount, i, found = 0;
        if (n > VP_SEEN_MAX) n = VP_SEEN_MAX;
        for (i = 0; i < n; i++)
            if (g_vpSeen[i].pass == p && g_vpSeen[i].w == pVp->Width &&
                g_vpSeen[i].h == pVp->Height && g_vpSeen[i].x == pVp->X &&
                g_vpSeen[i].y == pVp->Y) { found = 1; break; }
        if (!found && n < VP_SEEN_MAX) {
            g_vpSeen[n].pass = p; g_vpSeen[n].w = pVp->Width;
            g_vpSeen[n].h = pVp->Height; g_vpSeen[n].x = pVp->X;
            g_vpSeen[n].y = pVp->Y;
            g_vpSeenCount = n + 1;
            char vl[192];
            sprintf(vl, "[vp] %-18s x=%lu y=%lu %lux%lu",
                    g_passNames[p], (unsigned long)pVp->X, (unsigned long)pVp->Y,
                    (unsigned long)pVp->Width, (unsigned long)pVp->Height);
            LogLine(vl);
        }
    }
    return g_origSetViewport(This, pVp);
}
static PFN_CreateRenderTarget g_origCreateRT = NULL;
static PFN_CreateDepthStencil g_origCreateDS = NULL;
static PFN_SetRenderTarget    g_origSetRT = NULL;
// Depth-stencil binding, per draw pass. Needed before any MSAA attempt: a
// multisampled colour target requires a depth surface with a MATCHING sample
// count, so the MS pair cannot be created without knowing this one's format
// and dimensions. Log-only, same seen-table pattern as the RT and viewport
// probes, and behind the same LogPassRts flag.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetDepthStencilSurface)(
    IDirect3DDevice9 *, IDirect3DSurface9 *);
static PFN_SetDepthStencilSurface g_origSetDS = NULL;
#define DS_SEEN_MAX 48
typedef struct { LONG pass; void *surf; } DsSeen;
static DsSeen g_dsSeen[DS_SEEN_MAX];
static volatile LONG g_dsSeenCount = 0;

// ---- MSAA on the scene colour pass ---------------------------------------
// Both original objections are gone: `real_binds=0` proves a single-target
// FORWARD renderer (the old "deferred" verdict counted SetRenderTarget(n,NULL)
// unbinds), and the capability survey reports A8R8G8B8 / D24S8 / R32F all
// supporting up to x8 on this device.
//
// What the probes established, and what the design rests on:
//   - MULTI_SAMPLE binds 3840x2160 fmt=21 (scene colour) and fmt=114 (R32F
//     linear depth) BOTH AT INDEX 0, i.e. sequential sub-passes
//   - it binds one 3840x2160 D24S8 depth surface, ms=0
//   - format therefore discriminates the sub-pass: 21 = colour, 114 = depth
//
// Plan: substitute a multisampled colour+depth pair for the fmt=21 sub-pass
// only, then StretchRect-resolve back into the engine's own texture when the
// pass moves on. The R32F sub-pass is left entirely alone - averaging linear
// depth across an edge produces values that were never in the scene, which
// would corrupt fog/DOF for no AA benefit.
//
// D3D9 requires the render target and depth-stencil to share a sample count,
// so the depth substitution is not optional; it is bound as part of the same
// swap rather than waiting for the engine's next SetDepthStencilSurface.
//
// Resolve timing - v2, and the first version was wrong. It resolved on every
// SetRenderTarget that moved away from our MS surface, on the assumption that
// the scene is drawn in ONE episode. The counters said otherwise: ~4
// substitutions per frame, i.e. the engine binds the scene colour target
// several times with other bindings in between. Because D3D9 cannot
// StretchRect non-MS -> MS, there is no way to copy the resolved image back
// into the MS surface, so episodes 2..N each began from stale content and
// their resolve discarded everything before them. Result: black 3D, correct
// UI, and a perfectly clean 1:1 substitution/resolve count - the mechanism
// working exactly as instructed, which is why the counters looked healthy.
//
// v2 keeps the MS surface as the accumulating target for the whole pass and
// resolves ONCE, when the pass tag leaves MULTI_SAMPLE. Between episodes the
// only thing that must be undone is the DEPTH binding, since a non-MS target
// paired with our MS depth is an illegal draw.
//
// Residual risk, unavoidable at this layer: if the engine SAMPLES the scene
// texture mid-pass it will read unresolved (stale) content. Nothing in the
// probes suggests it does, but that is an absence of evidence.
static volatile LONG g_msaaSamples;          // 0 off, 2/4/8; tentative def above
static IDirect3DSurface9 *g_msColour = NULL;
static IDirect3DSurface9 *g_msDepth = NULL;
static IDirect3DSurface9 *g_msResolveTo = NULL;   // engine surface awaiting resolve
static IDirect3DSurface9 *g_msOrigDepth = NULL;
static UINT g_msW = 0, g_msH = 0;
static volatile LONG g_msActive = 0;       // MS colour is the bound target right now
static volatile LONG g_msDepthBound = 0;   // MS depth is current; must be undone for non-MS targets
// (g_msHasContent is a tentative def near the top - the Present hooks need it.)
static volatile LONG g_msSubstitutions = 0, g_msResolves = 0, g_msFailures = 0;
static volatile LONG g_msReported = 0;
// ---- v3 diagnostics: what is ONLY true of a multisampled target? ----------
// The Discard theory died on evidence: Discard=FALSE was accepted (its refusal
// line never printed), subs/resolves ran 1:1, resolveFail=0, failedMS=0 - and
// the screen still went black the moment the latch fired. The non-MS control
// renders correctly through the IDENTICAL code path, so the cause must be a
// property that exists only when the surface is multisampled. Three qualify:
//
//  1. Mixed sample counts are illegal. If the engine CAPTURES current bindings
//     (GetRenderTarget/GetDepthStencilSurface save, SetRenderTarget/-DS
//     restore - the standard push/pop around a sub-pass) it captures OUR MS
//     surface and replays it later against non-MS partners. In the control
//     every such mix is legal; with MS every draw in the replayed pass fails
//     silently - and failedMS only counts failures while g_msActive, so a
//     failure caused by our MS depth re-bound in a LATER pass was invisible.
//     Countered by LYING in the Get methods (return the engine's own surface)
//     and counted by g_msGetRtLies/g_msGetDsLies. If the lies count and the
//     scene renders, this was the mechanism.
//  2. D3DRS_MULTISAMPLEMASK. Ignored on non-MS targets; on an MS target a
//     mask of 0 means draws "succeed" and update ZERO samples. Clear ignores
//     the mask - which would fit magenta-clear-works / draws-don't exactly.
//     Counted and forced to 0xFFFFFFFF while our MS pair exists.
//  3. D3DRS_MULTISAMPLEANTIALIAS=FALSE - would give a dim (1/N brightness)
//     image rather than black, so unlikely, but the same hook covers it.
static volatile LONG g_msGetRtLies = 0, g_msGetDsLies = 0;
static volatile LONG g_msDsOutsideRebind = 0;   // engine bound OUR MS depth while inactive
static volatile LONG g_drawsFailedAll = 0;      // failures ANYWHERE, not just while MS bound
static volatile LONG g_drawFailAllLogged = 0;
static volatile LONG g_rsMsWrites = 0, g_rsMsForced = 0, g_rsMsLogged = 0;
// v4: the black screen is 4K-SPECIFIC (user-tested: every lower resolution
// renders correctly with MSAA applied - the mechanism works). The MS pair at
// 3840x2160 x4 is ~265MB in one gulp inside a 32-bit process; at 1440p it is
// ~118MB. Track what the driver says it has left, sampled on the render
// thread (GetAvailableTextureMem is a device method; the monitor thread must
// never touch the device).
// [v5 correction: BOTH v4 premises died on the log. Every Reset in every
// session is 3840x2160 - the game runs borderless at desktop res and the menu
// changes only the INTERNAL targets, so below "4K" the identity latch simply
// never fires and MSAA was OFF, not working. And the "no magenta" verdict was
// void: the per-frame Clear ran between the colour and depth binds, an
// MS/non-MS mismatch that failed the whole Clear unchecked.]
static volatile LONG g_availTexMemMB = -1;
// v5: with the clear fixed, the screen came back - minus the OPAQUE geometry
// (terrain, buildings, bodies), while sky/water/hair/UI render and the debug
// magenta leaks exactly where the missing pieces belong. That pattern is the
// signature of a DEPTH-PREPASS engine: opaque colour draws depth-test with
// ZFUNC=EQUAL against depth laid down by the prepass - which runs while a
// non-scene target is bound, so it lands in the ENGINE's depth surface, never
// in ours. Against our per-frame-cleared MS depth every EQUAL test compares
// with 1.0 and fails; LESSEQUAL/no-prepass materials (sky, water, hair) pass.
// Rewriting EQUAL -> LESSEQUAL while the substitution exists is
// outcome-identical where the two passes emit the same depth (equal passes
// lessequal) and renders normally where there was no prepass.
// Caveat measured, not assumed: state blocks Apply() without going through
// SetRenderState, so if the engine sets ZFUNC that way the rewrite misses it
// - which the seen/forced counters would show as a hole.
static volatile LONG g_zfuncSeenMask = 0;   // bit N = D3DCMP value N observed
static volatile LONG g_zfuncForced = 0;     // EQUAL rewrites while MS pair exists
static volatile LONG g_stencilNonAlways = 0; // STENCILFUNC != ALWAYS writes (rival theory)
// Film-strip episode dump (see MsaaResolve): 0 idle, 1 armed/running, 2 done.
static volatile LONG g_msEpDump = 0, g_msEpDumpFrame = 0, g_msEpIdx = 0;
// ---- Frame-stage capture: WHERE does the edge become smooth? --------------
// The shader hunt keeps coming back empty (every multi-tap fullscreen "quad"
// turned out to be flat world geometry running a material shader), so stop
// asking WHICH shader and ask WHERE. This captures the latched scene target
// at every draw-pass transition of one frame, plus the backbuffer, and works
// with MSAA OFF - which is the configuration the user says still shows the
// filter. Comparing the same edge across the strip localises the smoothing to
// one stage; if it is already smooth in the earliest capture, no post filter
// exists and the softness is in how the scene is rendered.
static IDirect3DDevice9 *g_dev = NULL;   // latched in HookedSetRenderTarget
static volatile LONG g_stageDumpState = 0;   // 0 idle, 1 running, 2 done
static volatile LONG g_stageDumpFrame = 0, g_stageDumpIdx = 0;
// (g_captureRequest is a tentative def near the top - the GUI thread's timer
// poll sets it and sits earlier in this file.)
static volatile LONG g_captureSeq = 0;
// v6: the depth-prepass substitution (see the tracking block near the top).
// MS R32F target sharing g_msDepth, so MS_DEPTH's prepass builds our depth.
static IDirect3DSurface9 *g_msR32f = NULL;
static IDirect3DSurface9 *g_msR32fResolveTo = NULL;
static volatile LONG g_msR32fActive = 0;
// (g_msR32fHasContent is a tentative def near the top - the Present hooks
// use it in their backstop.)
static volatile LONG g_msR32fSubs = 0, g_msR32fResolves = 0, g_msR32fFails = 0;
static volatile LONG g_a2cMirrored = 0;   // alpha-test toggles mirrored to vendor A2C
static volatile LONG g_a2cIssuedOn = 0;   // vendor A2C currently enabled (for off self-heal)
static volatile LONG g_msCreatedSamples = 0;   // sample count the current surfaces were built at
// Declared here rather than at their hooks below: MsaaNoteDraw sits earlier
// in this file than the Get hooks and needs the ORIG pointers for diagnosis.
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetRenderTarget)(
    IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDepthStencilSurface)(
    IDirect3DDevice9 *, IDirect3DSurface9 **);
static PFN_GetRenderTarget g_origGetRenderTarget = NULL;
static PFN_GetDepthStencilSurface g_origGetDepthStencil = NULL;

// Surfaces only - the identity latches survive. This is the GUI hot-toggle
// path (MSAA off, or sample count changed): re-earning the latch costs 2000
// draws of unsubstituted rendering, and nothing about the latched IDENTITY
// went stale. Render thread only, and only while none of ours are bound.
static void MsaaReleaseSurfaces(void)
{
    if (g_msColour) { IDirect3DSurface9_Release(g_msColour); g_msColour = NULL; }
    if (g_msDepth)  { IDirect3DSurface9_Release(g_msDepth);  g_msDepth  = NULL; }
    if (g_msR32f)   { IDirect3DSurface9_Release(g_msR32f);   g_msR32f   = NULL; }
    g_msResolveTo = NULL;
    g_msR32fResolveTo = NULL;
    g_msOrigDepth = NULL;
    g_msActive = 0;
    g_msR32fActive = 0;
    g_msDepthBound = 0;
    g_msHasContent = 0;
    g_msR32fHasContent = 0;
    g_msW = g_msH = 0;
}

static void MsaaRelease(void)
{
    MsaaReleaseSurfaces();
    // Drop the latched scene target too. A device Reset recreates the engine's
    // surfaces, so the old pointer is dead - keeping it means substitution
    // silently stops for the rest of the session (which is precisely why
    // changing resolution in the menu "fixed" the black screen). Re-latching
    // makes the feature survive an area change or alt-tab.
    g_sceneRtMain = NULL;
    g_sceneRtCount = 0;
    g_curSceneRtIdx = -1;
    g_depthRtMain = NULL;
    g_depthRtCount = 0;
    g_curDepthRtIdx = -1;
    for (int i = 0; i < SCENE_RT_MAX; i++) {
        g_sceneRts[i] = NULL;
        g_sceneRtDraws[i] = 0;
        g_sceneRtDrawsMs[i] = 0;
        g_depthRts[i] = NULL;
        g_depthRtDrawsMsd[i] = 0;
    }
}

// ---- One-shot surface dump: LOOK at the pipeline instead of inferring it --
// Every counter reports success and the screen is black; the counters have
// nothing left to say. This writes the actual CONTENT of a surface to disk:
//   msaa_resolved.bmp - the engine's scene texture right after our resolve.
//       Scene visible in it  -> draws and resolve both work; the loss is in
//                               the engine's post chain / display path.
//       Magenta only         -> the (reordered, now valid) debug clear works
//                               but DRAWS write nothing into the MS surface.
//       Black                -> even the clear's content is lost - the
//                               resolve copies nothing despite S_OK.
//   msaa_backbuf.bmp  - the backbuffer at the following Present (ground
//       truth of what the user sees, UI included).
// GetRenderTargetData needs a same-size same-format SYSTEMMEM surface; the
// scene texture and backbuffer are both non-MS, which it requires. Render
// thread only, fires once per session (resolve #300, well past the latch).
// stride: 1 = full resolution, 4 = every 4th pixel (960x540 from 4K) - the
// film-strip dump writes many files per frame and only needs enough pixels to
// SEE which episode carries the opaque geometry.
// cropW/cropH of 0 means "whole surface". A CROP is the right instrument for
// edge inspection: downscaling to fit a whole 4K frame in a file destroys the
// single-pixel edge detail the whole question is about.
static void DumpSurfaceCropToBmp(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf,
                                 const char *name, UINT stride,
                                 UINT cropX, UINT cropY, UINT cropW, UINT cropH)
{
    D3DSURFACE_DESC sd;
    if (!dev || !surf || FAILED(IDirect3DSurface9_GetDesc(surf, &sd))) return;
    if (stride < 1) stride = 1;
    char path[MAX_PATH], msg[MAX_PATH + 96];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *sl = strrchr(path, '\\');
    if (!sl || (sl - path) + strlen(name) + 2 >= MAX_PATH) return;
    strcpy(sl + 1, name);

    IDirect3DSurface9 *sys = NULL;
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
        dev, sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(hr) || !sys) {
        sprintf(msg, "[dump] %s: CreateOffscreenPlainSurface 0x%08lX", name, (unsigned long)hr);
        LogLine(msg);
        return;
    }
    hr = IDirect3DDevice9_GetRenderTargetData(dev, surf, sys);
    if (SUCCEEDED(hr)) {
        D3DLOCKED_RECT lr;
        hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            FILE *f = fopen(path, "wb");
            if (f) {
                // 24-bit bottom-up BMP; rows at W*3 padded to 4 bytes.
                UINT srcX = cropX, srcY = cropY;
                UINT spanW = cropW ? cropW : sd.Width;
                UINT spanH = cropH ? cropH : sd.Height;
                if (srcX >= sd.Width)  srcX = 0;
                if (srcY >= sd.Height) srcY = 0;
                if (srcX + spanW > sd.Width)  spanW = sd.Width  - srcX;
                if (srcY + spanH > sd.Height) spanH = sd.Height - srcY;
                UINT outW = spanW / stride, outH = spanH / stride;
                DWORD rowOut = (outW * 3 + 3) & ~3u;
                BITMAPFILEHEADER bfh; BITMAPINFOHEADER bih;
                memset(&bfh, 0, sizeof(bfh)); memset(&bih, 0, sizeof(bih));
                bfh.bfType = 0x4D42;
                bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
                bfh.bfSize = bfh.bfOffBits + rowOut * outH;
                bih.biSize = sizeof(bih);
                bih.biWidth = (LONG)outW;
                bih.biHeight = (LONG)outH;   // positive = bottom-up
                bih.biPlanes = 1;
                bih.biBitCount = 24;
                fwrite(&bfh, sizeof(bfh), 1, f);
                fwrite(&bih, sizeof(bih), 1, f);
                // Stack row buffer (no stdlib.h in this file); 16K covers any
                // width up to ~5460px, and 4K is 11,520 bytes.
                unsigned char row[16384];
                if (rowOut <= sizeof(row)) {
                    memset(row, 0, rowOut);
                    for (LONG y = (LONG)outH - 1; y >= 0; y--) {
                        const unsigned char *src = (const unsigned char *)lr.pBits
                            + (size_t)(srcY + (UINT)y * stride) * lr.Pitch
                            + (size_t)srcX * 4;
                        for (UINT x = 0; x < outW; x++) {
                            row[x * 3 + 0] = src[x * stride * 4 + 0];   // B
                            row[x * 3 + 1] = src[x * stride * 4 + 1];   // G
                            row[x * 3 + 2] = src[x * stride * 4 + 2];   // R
                        }
                        fwrite(row, rowOut, 1, f);
                    }
                }
                fclose(f);
                sprintf(msg, "[dump] wrote %s (%ux%u fmt=%d)", name, outW, outH, (int)sd.Format);
                LogLine(msg);
            }
            IDirect3DSurface9_UnlockRect(sys);
        }
    } else {
        sprintf(msg, "[dump] %s: GetRenderTargetData 0x%08lX", name, (unsigned long)hr);
        LogLine(msg);
    }
    IDirect3DSurface9_Release(sys);
}

static void DumpSurfaceToBmpEx(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf,
                               const char *name, UINT stride)
{
    DumpSurfaceCropToBmp(dev, surf, name, stride, 0, 0, 0, 0);
}

static void DumpSurfaceToBmp(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, const char *name)
{
    DumpSurfaceCropToBmp(dev, surf, name, 1, 0, 0, 0, 0);
}

// ---- Whole-frame edge statistics ------------------------------------------
// Images forced a choice between covering the frame and keeping full-res
// pixels; a NUMBER has neither limit. Measured over the entire surface, so
// whatever the user is looking at (the bridge at the back, in this case) is
// included wherever it happens to be.
//
//   grad      mean |horizontal luma gradient| - overall sharpness
//   hardStep  fraction of strong edges that are a single hard jump with flat
//             neighbours, i.e. an UNANTIALIASED staircase. Anti-aliasing
//             turns those into 2-3px ramps, so an AA stage makes this FALL
//             sharply while the total edge count stays similar. This is the
//             discriminator: blur lowers grad AND hardStep, AA lowers
//             hardStep while leaving grad roughly intact.
//
// Every other row only - keeps horizontal adjacency (which the metric needs)
// while halving the cost of an 8.3Mpx scan.
static void AnalyseSurfaceStage(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf,
                                const char *label)
{
    D3DSURFACE_DESC sd;
    if (!dev || !surf || FAILED(IDirect3DSurface9_GetDesc(surf, &sd))) return;
    if (sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8) return;
    IDirect3DSurface9 *sys = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(
            dev, sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys)
        return;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, surf, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            unsigned __int64 gsum = 0; unsigned int gn = 0;
            unsigned int edges = 0, hard = 0;
            for (UINT y = 0; y < sd.Height; y += 2) {
                const unsigned char *p = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
                int lprev2 = -1, lprev = -1;
                for (UINT x = 0; x < sd.Width; x++) {
                    int l = (p[x*4+2] * 77 + p[x*4+1] * 151 + p[x*4+0] * 28) >> 8;
                    if (lprev >= 0) {
                        int d = l - lprev; if (d < 0) d = -d;
                        gsum += (unsigned)d; gn++;
                        if (d > 32) {
                            edges++;
                            // flat on both sides => a hard staircase step
                            if (lprev2 >= 0) {
                                int dl = lprev - lprev2; if (dl < 0) dl = -dl;
                                int dr = (x + 1 < sd.Width)
                                    ? ((p[(x+1)*4+2]*77 + p[(x+1)*4+1]*151 + p[(x+1)*4+0]*28) >> 8) - l
                                    : 0;
                                if (dr < 0) dr = -dr;
                                if (dl < 8 && dr < 8) hard++;
                            }
                        }
                    }
                    lprev2 = lprev; lprev = l;
                }
            }
            char l[224];
            sprintf(l, "[edge] %-28s %ux%u  grad=%.3f  edges=%u  hardStep=%.3f",
                    label, sd.Width, sd.Height,
                    gn ? (double)gsum / gn : 0.0, edges,
                    edges ? (double)hard / edges : 0.0);
            LogLine(l);
            IDirect3DSurface9_UnlockRect(sys);
        }
    }
    IDirect3DSurface9_Release(sys);
}

// Resolve the multisampled colour back into the engine's texture. MS -> non-MS
// StretchRect requires identical dimensions and D3DTEXF_NONE.
// Resolve the accumulated MS colour into the engine's texture. Called once per
// frame, when the draw-pass tag leaves MULTI_SAMPLE (with a Present-time
// backstop in case the next pass never binds a render target).
static void MsaaResolve(IDirect3DDevice9 *dev)
{
    if (!g_msHasContent || !g_msColour || !g_msResolveTo) {
        g_msHasContent = 0;
        return;
    }
    HRESULT hr = IDirect3DDevice9_StretchRect(dev, g_msColour, NULL,
                                              g_msResolveTo, NULL, D3DTEXF_NONE);
    if (SUCCEEDED(hr)) {
        LONG n = InterlockedIncrement(&g_msResolves);
        // Periodic VRAM sample for the 4K memory theory - cheap, render
        // thread, roughly twice a second at ~16 episodes/frame.
        if ((n & 511) == 1)
            g_availTexMemMB = (LONG)(IDirect3DDevice9_GetAvailableTextureMem(dev) >> 20);
        // One-shot pipeline split (see DumpSurfaceToBmp): capture the scene
        // texture as it is right after this resolve, and arm the backbuffer
        // capture for the frame's Present. #300 is ~20 frames past the latch,
        // deep enough that the black state is fully established.
        // Diagnostic-mode only (MsaaDebugClear) now that the black-screen and
        // missing-geometry hunts are closed - a normal session should not
        // write BMPs to the game folder.
        if (n == 300 && g_msDumpState == 0 && g_msaaDebugClear) {
            DumpSurfaceToBmp(dev, g_msResolveTo, "msaa_resolved.bmp");
            g_msDumpState = 2;
        }
        // Film-strip: every episode boundary of ONE frame, quarter res. The
        // opaque geometry lands in the MS surface (541k draws counted) yet is
        // absent from the final resolved image while sky/water/hair survive -
        // and no depth/stencil test can reject anything against a far-cleared
        // buffer (zfuncSeen=LESSEQUAL only). So the loss must be visible as a
        // TRANSITION between episodes: either an episode paints opaque and a
        // later one arrives with it gone, or it never appears at all. The
        // strip shows which, and in which pass (logged per file).
        if (g_msEpDump == 0 && n >= 300 && g_msaaDebugClear) {
            g_msEpDump = 1;
            g_msEpDumpFrame = g_msFrameSeq + 1;   // start clean at next frame
        } else if (g_msEpDump == 1 && g_msFrameSeq == g_msEpDumpFrame) {
            LONG e = ++g_msEpIdx;
            if (e <= 24) {
                char nm[48], el[128];
                sprintf(nm, "msaa_ep%02ld.bmp", e);
                DumpSurfaceToBmpEx(dev, g_msResolveTo, nm, 4);
                sprintf(el, "[dump] episode %ld resolved during pass %s", e,
                        g_passNames[(g_curPass >= 0 && g_curPass < PASS_COUNT) ? g_curPass : 0]);
                LogLine(el);
            }
        } else if (g_msEpDump == 1 && g_msFrameSeq > g_msEpDumpFrame && g_msEpIdx > 0) {
            g_msEpDump = 2;
            // Frame over: capture the sibling full-screen targets too, in
            // case the opaque content lives in #2/#3 rather than #1. Their
            // pointers come from SetRenderTarget tracking and could in
            // principle be stale, hence the SEH guard.
            __try {
                if (g_sceneRts[1]) DumpSurfaceToBmpEx(dev, (IDirect3DSurface9 *)g_sceneRts[1], "msaa_rt2.bmp", 4);
                if (g_sceneRts[2]) DumpSurfaceToBmpEx(dev, (IDirect3DSurface9 *)g_sceneRts[2], "msaa_rt3.bmp", 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LogLine("[dump] rt2/rt3 dump faulted (stale pointer) - skipped");
            }
            char dl[96];
            sprintf(dl, "[dump] film strip complete: %ld episodes in frame %ld",
                    g_msEpIdx, g_msEpDumpFrame);
            LogLine(dl);
        }
    }
    else if (InterlockedIncrement(&g_msFailures) <= 4) {
        char l[128];
        sprintf(l, "[msaa] resolve FAILED hr=0x%08lX", (unsigned long)hr);
        LogLine(l);
    }
    g_msHasContent = 0;
    g_msResolveTo = NULL;
}

// R32F counterpart: hand the prepass' linear depth back to the engine, which
// samples it for post effects (fog, DoF, soft particles).
static void MsaaResolveR32f(IDirect3DDevice9 *dev)
{
    if (!g_msR32fHasContent || !g_msR32f || !g_msR32fResolveTo) {
        g_msR32fHasContent = 0;
        return;
    }
    HRESULT hr = IDirect3DDevice9_StretchRect(dev, g_msR32f, NULL,
                                              g_msR32fResolveTo, NULL, D3DTEXF_NONE);
    if (SUCCEEDED(hr)) InterlockedIncrement(&g_msR32fResolves);
    else if (InterlockedIncrement(&g_msR32fFails) <= 4) {
        char l[128];
        sprintf(l, "[msaa] R32F resolve FAILED hr=0x%08lX", (unsigned long)hr);
        LogLine(l);
    }
    g_msR32fHasContent = 0;
    g_msR32fResolveTo = NULL;
}

// Put the engine's own depth surface back. Needed whenever a NON-MS target is
// bound while our MS depth is still current, because D3D9 rejects a draw whose
// render target and depth-stencil disagree on sample count.
static void MsaaRestoreDepth(IDirect3DDevice9 *dev)
{
    if (!g_msDepthBound) return;
    g_origSetDS(dev, g_msOrigDepth);   // NULL is legal and means "no depth"
    g_msDepthBound = 0;
}

// ---- Draw-call accounting for the MSAA hunt ------------------------------
// Three models of why the scene is black have now been wrong (resolve timing,
// episode accumulation, depth clear). Every one was reasoning about where
// pixels go without establishing that draws happen at all. The magenta test
// proved the resolve path works end to end; this establishes the other half.
//
//   drawsWhileMs ~= 0        -> the main geometry is NOT drawn while our
//                               surface is bound, so substituting during
//                               MULTI_SAMPLE targets the wrong window and our
//                               resolve is overwriting the real scene
//   drawsWhileMs large, ok   -> draws succeed; the loss is after them
//   drawsWhileMs large, fail -> D3D9 is rejecting them, and the HRESULT says
//                               exactly why (mismatched sample counts give
//                               D3DERR_INVALIDCALL)
//
// Deliberately hooked on the REAL device vtable, not the generic timing
// thunks - those have never fired on this game's device (the SWVP/HWVP class
// split), which is the same blind spot that hid Present and CreateTexture.
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitive)(
    IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitive)(
    IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static PFN_DrawIndexedPrimitive g_origDrawIndexed = NULL;
static PFN_DrawPrimitive g_origDraw = NULL;
static volatile LONG g_drawsTotal = 0, g_drawsWhileMs = 0, g_drawsFailedMs = 0;
static volatile LONG g_drawFailLogged = 0;

// Where does the geometry actually go? Only ~6% of draws happen while the MS
// surface is bound, so substituting during MULTI_SAMPLE catches a small slice
// and the resolve overwrites the real scene with it. Bucketing every draw by
// draw-pass AND by the format/size of the bound render target names the pass
// and surface that carry the bulk of the geometry - which is where the
// substitution belongs.
static volatile LONG g_drawsByPass[PASS_COUNT];
static volatile LONG g_drawsRtFmt21 = 0;    // A8R8G8B8, full screen
static volatile LONG g_drawsRtFmt114 = 0;   // R32F linear depth
static volatile LONG g_drawsRtShadow = 0;   // the big square shadow maps
static volatile LONG g_drawsRtOther = 0;
static volatile LONG g_curRtFmt = 0, g_curRtW = 0, g_curRtH = 0;
// (scene-target tracking is declared near the top of the file - MsaaRelease
// needs to reset it and sits earlier than this block.)

static void MsaaNoteDraw(IDirect3DDevice9 *This, HRESULT hr, UINT primCount)
{
    InterlockedIncrement(&g_drawsTotal);
    // Per-shader draw accounting for the identify walk. O(1): the index was
    // resolved once at bind time.
    {
        LONG pi = g_curPsIdx;
        if (pi >= 0 && pi < PS_MAP_MAX) InterlockedIncrement(&g_psMap[pi].draws);
    }
    // Fullscreen-quad shader attribution (see the v2 gate note above): every
    // pixel shader that draws a fullscreen quad onto a fullscreen-sized
    // target, whatever the format or pass. The post-AA MUST appear here.
    // Gated behind DumpShaders - a discovery instrument, not a ship counter.
    // v3: ANY target at least 256px wide, not just fullscreen-sized - a post
    // filter running at half or quarter res would have slipped the v2 gate.
    // With the shader census now proving no multi-tap fullscreen filter
    // exists at full res, this is the last place one could hide.
#if ENABLE_SHADER_DIAG
    if (g_dumpShaders && primCount <= 2 && g_curRtW >= 256) {
        void *ps = g_curPsObj;
        if (ps) {
            LONG n = g_postPsSeenCount, i, found = -1;
            if (n > POST_PS_MAX) n = POST_PS_MAX;
            for (i = 0; i < n; i++) if (g_postPsSeen[i].obj == ps) { found = i; break; }
            if (found < 0 && n < POST_PS_MAX) {
                found = n;
                g_postPsSeen[n].obj = ps;
                g_postPsSeen[n].draws = 0;
                g_postPsSeen[n].fmt = g_curRtFmt;
                g_postPsSeen[n].w = g_curRtW;
                g_postPsSeen[n].h = g_curRtH;
                g_postPsSeen[n].passMask = 0;
                g_postPsSeenCount = n + 1;
            }
            if (found >= 0) {
                InterlockedIncrement(&g_postPsSeen[found].draws);
                LONG p = g_curPass;
                if (p >= 0 && p < PASS_COUNT)
                    InterlockedOr(&g_postPsSeen[found].passMask, 1L << p);
            }
        }
    }
#endif  // ENABLE_SHADER_DIAG
    // A failure ANYWHERE, not just while our MS colour is bound. failedMS=0
    // with a black screen is precisely what a failure in a LATER pass looks
    // like (e.g. our MS depth replayed against a non-MS target) - the old
    // counter was blind to it by construction. Diagnosis uses the ORIG get
    // methods: the hooked ones lie about the substitution on purpose.
    if (FAILED(hr)) {
        InterlockedIncrement(&g_drawsFailedAll);
        if (InterlockedIncrement(&g_drawFailAllLogged) <= 6 && This) {
            IDirect3DSurface9 *rt = NULL, *ds = NULL;
            if (g_origGetRenderTarget) { g_origGetRenderTarget(This, 0, &rt); if (rt) IDirect3DSurface9_Release(rt); }
            if (g_origGetDepthStencil) { g_origGetDepthStencil(This, &ds); if (ds) IDirect3DSurface9_Release(ds); }
            char l[224];
            sprintf(l, "[draw] FAILED hr=0x%08lX pass=%s rt=%p%s ds=%p%s",
                    (unsigned long)hr,
                    g_passNames[(g_curPass >= 0 && g_curPass < PASS_COUNT) ? g_curPass : 0],
                    (void *)rt, (rt && rt == g_msColour) ? "(OUR MS COLOUR)" : "",
                    (void *)ds, (ds && ds == g_msDepth) ? "(OUR MS DEPTH)" : "");
            LogLine(l);
        }
    }
    {
        LONG p = g_curPass;
        if (p >= 0 && p < PASS_COUNT) InterlockedIncrement(&g_drawsByPass[p]);
        if (g_curRtW == (LONG)g_backbufW && g_curRtFmt == D3DFMT_A8R8G8B8)
            InterlockedIncrement(&g_drawsRtFmt21);
        else if (g_curRtFmt == D3DFMT_R32F && g_curRtW == (LONG)g_backbufW)
            InterlockedIncrement(&g_drawsRtFmt114);
        else if (g_curRtW == g_curRtH && g_curRtW >= 1024)
            InterlockedIncrement(&g_drawsRtShadow);
        else
            InterlockedIncrement(&g_drawsRtOther);
        // Per-surface, so the scene target can be picked by identity.
        LONG si = g_curSceneRtIdx;
        if (si >= 0 && si < SCENE_RT_MAX) {
            InterlockedIncrement(&g_sceneRtDraws[si]);
            if (g_curPass == PASS_MS) {
                LONG c = InterlockedIncrement(&g_sceneRtDrawsMs[si]);
                // Latch the scene target by identity once it has proven itself.
                // A threshold rather than the first hit, so a stray draw into
                // a composite target cannot win the race.
                if (!g_sceneRtMain && c >= SCENE_RT_LATCH_DRAWS) {
                    g_sceneRtMain = g_sceneRts[si];
                    char ll[160];
                    sprintf(ll, "[scenert] latched scene target #%ld (%p) after %ld draws in MULTI_SAMPLE",
                            si + 1, g_sceneRtMain, c);
                    LogLine(ll);
                }
            }
        }
        // Same latch for the linear-depth target, by MS_DEPTH draws - the
        // depth prepass that must also run against our MS depth (v6).
        LONG di = g_curDepthRtIdx;
        if (di >= 0 && di < SCENE_RT_MAX && g_curPass == PASS_MS_DEPTH) {
            LONG c = InterlockedIncrement(&g_depthRtDrawsMsd[di]);
            if (!g_depthRtMain && c >= SCENE_RT_LATCH_DRAWS) {
                g_depthRtMain = g_depthRts[di];
                char ll[160];
                sprintf(ll, "[depthrt] latched linear-depth target #%ld (%p) after %ld draws in MS_DEPTH",
                        di + 1, g_depthRtMain, c);
                LogLine(ll);
            }
        }
    }
    if (!g_msActive && !g_msR32fActive) return;
    InterlockedIncrement(&g_drawsWhileMs);
    if (FAILED(hr)) {
        InterlockedIncrement(&g_drawsFailedMs);
        if (InterlockedIncrement(&g_drawFailLogged) <= 4) {
            char l[128];
            sprintf(l, "[msaa] draw FAILED while MS bound: hr=0x%08lX", (unsigned long)hr);
            LogLine(l);
        }
    }
}

// Mirror the engine's own depth-clear VALUE. Our MS depth is cleared once per
// frame, and the value was hardcoded to 1.0 on the assumption of a standard
// LESS depth test. If this engine uses reversed-Z (clear 0.0, GREATER test),
// 1.0 rejects every draw - which is exactly the symptom: draws succeed
// (failedMS=0) and write nothing. Capturing whatever the engine passes to
// Clear removes the guess entirely, and also picks up the stencil value.
typedef HRESULT (STDMETHODCALLTYPE *PFN_Clear)(
    IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
static PFN_Clear g_origClear = NULL;
static float g_lastClearZ = 1.0f;
static DWORD g_lastClearStencil = 0;
static volatile LONG g_clearZSeen = 0;

static HRESULT STDMETHODCALLTYPE HookedClear(
    IDirect3DDevice9 *This, DWORD Count, const D3DRECT *pRects, DWORD Flags,
    D3DCOLOR Color, float Z, DWORD Stencil)
{
    if (Flags & D3DCLEAR_ZBUFFER) {
        g_lastClearZ = Z;
        g_lastClearStencil = Stencil;
        if (InterlockedIncrement(&g_clearZSeen) == 1) {
            char l[128];
            sprintf(l, "[msaa] engine depth clear value Z=%.3f stencil=%lu"
                       " (reversed-Z if 0.0)", Z, (unsigned long)Stencil);
            LogLine(l);
        }
    }
    return g_origClear(This, Count, pRects, Flags, Color, Z, Stencil);
}

// Sub-pixel sample offsets, in pixels. A rotated grid beats an axis-aligned
// one for the near-horizontal and near-vertical edges that dominate foliage.
static const float g_ssaaOffsets[8][2] = {
    { -0.375f, -0.125f }, {  0.125f, -0.375f }, {  0.375f,  0.125f }, { -0.125f,  0.375f },
    { -0.250f, -0.250f }, {  0.250f, -0.250f }, {  0.250f,  0.250f }, { -0.250f,  0.250f },
};

// Returns 1 if the draw was handled as N masked passes.
static int SsaaMultiDraw(IDirect3DDevice9 *This, int indexed,
                         D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
                         UINT MinVertexIndex, UINT NumVertices,
                         UINT StartIndex, UINT PrimitiveCount, HRESULT *hrOut)
{
#if !ENABLE_CUTOUT_AA
    (void)This; (void)indexed; (void)Type; (void)BaseVertexIndex;
    (void)MinVertexIndex; (void)NumVertices; (void)StartIndex;
    (void)PrimitiveCount; (void)hrOut;
    return 0;   // retired - see ENABLE_CUTOUT_AA
#else
    LONG n = g_msaaSamples;
    if (!g_ssaaFoliage || !g_msActive || n < 2 || n > 8) return 0;
    LONG pi = g_curPsIdx;
    if (pi < 0 || pi >= PS_MAP_MAX) return 0;
    void *cur = g_psMap[pi].obj;
    void *ss = NULL;
    DWORD offReg = 0;
    LONG vn = g_a2cVariantCount, i;
    if (vn > A2C_MAX) vn = A2C_MAX;
    for (i = 0; i < vn; i++)
        if (g_a2cVariants[i].orig == cur) {
            ss = g_a2cVariants[i].ssaa;
            offReg = g_a2cVariants[i].ssaaOffReg;
            break;
        }
    if (!ss || !offReg || !g_origSetPixelShader || !g_origSetRenderState) return 0;

    g_origSetPixelShader(This, (IDirect3DPixelShader9 *)ss);
    HRESULT hr = D3D_OK;
    for (LONG s = 0; s < n; s++) {
        float off[4] = { g_ssaaOffsets[s][0], g_ssaaOffsets[s][1], 0.0f, 0.0f };
        IDirect3DDevice9_SetPixelShaderConstantF(This, offReg, off, 1);
        g_origSetRenderState(This, D3DRS_MULTISAMPLEMASK, 1u << s);
        hr = indexed
            ? g_origDrawIndexed(This, Type, BaseVertexIndex, MinVertexIndex,
                                NumVertices, StartIndex, PrimitiveCount)
            : g_origDraw(This, Type, StartIndex, PrimitiveCount);
    }
    // Full mask back on, and the original shader rebound, so nothing after
    // this draw inherits either.
    g_origSetRenderState(This, D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
    g_origSetPixelShader(This, (IDirect3DPixelShader9 *)cur);
    InterlockedIncrement(&g_ssaaDraws);
    *hrOut = hr;
    return 1;
#endif
}

// Second pass: same geometry, blended, fringe-only shader. Depth WRITE off
// (the engine's pass already laid the interior's depth) but depth TEST on, so
// the fringe still hides behind anything in front of it.
static void FringePass(IDirect3DDevice9 *This, int indexed,
                       D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
                       UINT MinVertexIndex, UINT NumVertices,
                       UINT StartIndex, UINT PrimitiveCount)
{
#if !ENABLE_CUTOUT_AA
    (void)This; (void)indexed; (void)Type; (void)BaseVertexIndex;
    (void)MinVertexIndex; (void)NumVertices; (void)StartIndex; (void)PrimitiveCount;
    return;   // retired - see ENABLE_CUTOUT_AA
#else
    if (!g_fringeFoliage || !g_msActive || g_alphaBlendOn) return;
    LONG pi = g_curPsIdx;
    if (pi < 0 || pi >= PS_MAP_MAX) return;
    void *cur = g_psMap[pi].obj, *fr = NULL;
    LONG vn = g_a2cVariantCount, i;
    if (vn > A2C_MAX) vn = A2C_MAX;
    for (i = 0; i < vn; i++)
        if (g_a2cVariants[i].orig == cur) { fr = g_a2cVariants[i].fringe; break; }
    if (!fr || !g_origSetPixelShader || !g_origSetRenderState) return;

    DWORD oldSrc = D3DBLEND_ONE, oldDst = D3DBLEND_ZERO, oldZW = TRUE;
    IDirect3DDevice9_GetRenderState(This, D3DRS_SRCBLEND, &oldSrc);
    IDirect3DDevice9_GetRenderState(This, D3DRS_DESTBLEND, &oldDst);
    IDirect3DDevice9_GetRenderState(This, D3DRS_ZWRITEENABLE, &oldZW);

    g_origSetRenderState(This, D3DRS_ALPHABLENDENABLE, TRUE);
    g_origSetRenderState(This, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_origSetRenderState(This, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_origSetRenderState(This, D3DRS_ZWRITEENABLE, FALSE);
    g_origSetPixelShader(This, (IDirect3DPixelShader9 *)fr);

    if (indexed)
        g_origDrawIndexed(This, Type, BaseVertexIndex, MinVertexIndex,
                          NumVertices, StartIndex, PrimitiveCount);
    else
        g_origDraw(This, Type, StartIndex, PrimitiveCount);

    g_origSetPixelShader(This, (IDirect3DPixelShader9 *)cur);
    g_origSetRenderState(This, D3DRS_ZWRITEENABLE, oldZW);
    g_origSetRenderState(This, D3DRS_DESTBLEND, oldDst);
    g_origSetRenderState(This, D3DRS_SRCBLEND, oldSrc);
    g_origSetRenderState(This, D3DRS_ALPHABLENDENABLE, FALSE);
    InterlockedIncrement(&g_fringeDraws);
#endif
}

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive(
    IDirect3DDevice9 *This, D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
    UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    HRESULT hr;
    if (SsaaMultiDraw(This, 1, Type, BaseVertexIndex, MinVertexIndex,
                      NumVertices, StartIndex, PrimitiveCount, &hr)) {
        MsaaNoteDraw(This, hr, PrimitiveCount);
        return hr;
    }
    hr = g_origDrawIndexed(This, Type, BaseVertexIndex, MinVertexIndex,
                           NumVertices, StartIndex, PrimitiveCount);
    FringePass(This, 1, Type, BaseVertexIndex, MinVertexIndex,
               NumVertices, StartIndex, PrimitiveCount);
    MsaaNoteDraw(This, hr, PrimitiveCount);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedDrawPrimitive(
    IDirect3DDevice9 *This, D3DPRIMITIVETYPE Type, UINT StartVertex, UINT PrimitiveCount)
{
    HRESULT hr;
    if (SsaaMultiDraw(This, 0, Type, 0, 0, 0, StartVertex, PrimitiveCount, &hr)) {
        MsaaNoteDraw(This, hr, PrimitiveCount);
        return hr;
    }
    hr = g_origDraw(This, Type, StartVertex, PrimitiveCount);
    FringePass(This, 0, Type, 0, 0, 0, StartVertex, PrimitiveCount);
    MsaaNoteDraw(This, hr, PrimitiveCount);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedSetDepthStencilSurface(
    IDirect3DDevice9 *This, IDirect3DSurface9 *pDS)
{
    // While either of our MS targets (colour or R32F prepass) is bound, the
    // engine's non-MS depth would be an illegal pairing - redirect it to ours
    // and remember what it asked for so it can be restored on the way out.
    if ((g_msActive || g_msR32fActive) && g_msDepth && pDS != g_msDepth) {
        g_msOrigDepth = pDS;
        g_msDepthBound = 1;
        return g_origSetDS(This, g_msDepth);
    }
    // The engine binding OUR MS depth while our MS colour is NOT current is
    // the smoking gun of the Get-capture theory (see the v3 block above): it
    // can only have obtained that pointer from GetDepthStencilSurface, and
    // replaying it against a non-MS target silently kills every draw that
    // follows. The Get lies make acquisition impossible, so this counter
    // staying 0 confirms the seal; it firing means there is another leak.
    if (!g_msActive && g_msDepth && pDS == g_msDepth)
        InterlockedIncrement(&g_msDsOutsideRebind);
    if (g_logPassRts) {
        LONG p = g_curPass;
        if (p < 0 || p >= PASS_COUNT) p = PASS_NONE;
        LONG n = g_dsSeenCount, i, found = 0;
        if (n > DS_SEEN_MAX) n = DS_SEEN_MAX;
        for (i = 0; i < n; i++)
            if (g_dsSeen[i].pass == p && g_dsSeen[i].surf == pDS) { found = 1; break; }
        if (!found && n < DS_SEEN_MAX) {
            g_dsSeen[n].pass = p;
            g_dsSeen[n].surf = pDS;
            g_dsSeenCount = n + 1;
            char dl[192];
            if (!pDS) {
                sprintf(dl, "[ds] %-18s (none - depth unbound)", g_passNames[p]);
            } else {
                D3DSURFACE_DESC sd;
                if (SUCCEEDED(IDirect3DSurface9_GetDesc(pDS, &sd)))
                    sprintf(dl, "[ds] %-18s %ux%u fmt=%d ms=%d qual=%lu",
                            g_passNames[p], sd.Width, sd.Height, (int)sd.Format,
                            (int)sd.MultiSampleType, (unsigned long)sd.MultiSampleQuality);
                else
                    sprintf(dl, "[ds] %-18s (GetDesc failed)", g_passNames[p]);
            }
            LogLine(dl);
        }
    }
    return g_origSetDS(This, pDS);
}

// ---- Substitution transparency: lie in the Get methods --------------------
// See theory 1 in the v3 block. While our surfaces are bound, the engine must
// never learn it: GetRenderTarget(0) returns the engine's own scene surface
// (the one awaiting resolve) and GetDepthStencilSurface returns the depth the
// engine last asked for. Anything the engine captures and replays is then its
// own surface, which the identity substitution redirects as usual. This also
// closes a latent use-after-free: an engine holding OUR pointer across a
// device Reset would replay a surface MsaaRelease() has already released.
// (Typedefs and orig pointers are up in the v3 counter block - MsaaNoteDraw
// needs them and sits earlier in the file.)
static HRESULT STDMETHODCALLTYPE HookedGetRenderTarget(
    IDirect3DDevice9 *This, DWORD Index, IDirect3DSurface9 **ppRT)
{
    HRESULT hr = g_origGetRenderTarget(This, Index, ppRT);
    if (SUCCEEDED(hr) && Index == 0 && ppRT && *ppRT &&
        *ppRT == g_msColour && g_msResolveTo) {
        InterlockedIncrement(&g_msGetRtLies);
        IDirect3DSurface9_Release(*ppRT);
        *ppRT = g_msResolveTo;
        IDirect3DSurface9_AddRef(*ppRT);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedGetDepthStencilSurface(
    IDirect3DDevice9 *This, IDirect3DSurface9 **ppDS)
{
    HRESULT hr = g_origGetDepthStencil(This, ppDS);
    if (SUCCEEDED(hr) && ppDS && *ppDS && *ppDS == g_msDepth) {
        InterlockedIncrement(&g_msGetDsLies);
        IDirect3DSurface9_Release(*ppDS);
        if (g_msOrigDepth) {
            *ppDS = g_msOrigDepth;
            IDirect3DSurface9_AddRef(*ppDS);
        } else {
            // The engine had no depth bound when we substituted; mirror what
            // the real API reports for "no depth-stencil surface is set".
            *ppDS = NULL;
            hr = D3DERR_NOTFOUND;
        }
    }
    return hr;
}

// ---- Multisample render states: theory 2/3 in the v3 block ----------------
// Both states are IGNORED on non-multisampled targets, so an engine that never
// ran with MSAA on PC may carry any value in them without consequence - until
// our MS target makes them potent. Forced only while our MS pair exists, and
// every engine write is counted so the log says whether the engine touches
// them at all (if it never does, both theories are dead and the force inert).
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderState)(
    IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
// (g_origSetRenderState is a tentative def above HookedSetPixelShader, which
// needs it for the alpha-to-coverage bind path.)

static HRESULT STDMETHODCALLTYPE HookedSetRenderState(
    IDirect3DDevice9 *This, D3DRENDERSTATETYPE State, DWORD Value)
{
    if (State == D3DRS_MULTISAMPLEMASK || State == D3DRS_MULTISAMPLEANTIALIAS) {
        DWORD want = (State == D3DRS_MULTISAMPLEMASK) ? 0xFFFFFFFFu : (DWORD)TRUE;
        InterlockedIncrement(&g_rsMsWrites);
        if (Value != want && InterlockedIncrement(&g_rsMsLogged) <= 8) {
            char l[144];
            sprintf(l, "[rs] engine wrote %s = 0x%08lX%s",
                    (State == D3DRS_MULTISAMPLEMASK) ? "MULTISAMPLEMASK"
                                                     : "MULTISAMPLEANTIALIAS",
                    (unsigned long)Value,
                    (State == D3DRS_MULTISAMPLEMASK && Value == 0)
                        ? "  *** mask 0: every draw to an MS target updates ZERO samples ***" : "");
            LogLine(l);
        }
        if (g_msColour && g_msaaSamples >= 2 && Value != want) {
            InterlockedIncrement(&g_rsMsForced);
            Value = want;
        }
    }
    // Depth-prepass compensation - see the v5 block above the counters.
    if (State == D3DRS_ZFUNC) {
        LONG bit = (Value <= 31) ? (LONG)(1UL << Value) : 0;
        if (bit && !(g_zfuncSeenMask & bit)) {
            InterlockedOr(&g_zfuncSeenMask, bit);
            char l[96];
            sprintf(l, "[rs] ZFUNC value %lu first seen", (unsigned long)Value);
            LogLine(l);
        }
        if (Value == D3DCMP_EQUAL && g_msColour && g_msaaSamples >= 2) {
            InterlockedIncrement(&g_zfuncForced);
            Value = D3DCMP_LESSEQUAL;
        }
    }
    // Rival theory, count only: prepass laying down STENCIL that the colour
    // pass tests. Our stencil is cleared to the engine's own value per frame,
    // so a non-ALWAYS stencil func while we substitute would fail the same
    // way EQUAL depth does.
    if (State == D3DRS_STENCILFUNC && Value != D3DCMP_ALWAYS)
        InterlockedIncrement(&g_stencilNonAlways);
    // Alpha-to-coverage gate: a cutout draw is OPAQUE (blending off), while
    // blended material - hair especially - must keep its own alpha. Tracking
    // the state is what lets the coverage rewrite apply to any cutout shader
    // regardless of how it computes alpha.
    if (State == D3DRS_ALPHABLENDENABLE) g_alphaBlendOn = (Value != 0);
    // Remember the engine's own alpha-test state so the A2C bind path can put
    // it back exactly, instead of assuming it was off.
    if (State == D3DRS_ALPHATESTENABLE && !g_a2cStateOn) g_alphaTestWasOn = (Value != 0);
    // Alpha-to-coverage: mirror the engine's own alpha-test toggle with the
    // vendor A2C switch while a multisampled target of ours can be bound.
    // The alpha value then drives a per-sample coverage mask, so foliage and
    // fence edges come out of the resolve as gradients instead of cutouts.
    // Issued AFTER the engine's own write so nothing can reorder it away.
    // When the feature is toggled off (GUI) with the vendor state last left
    // enabled, the next alpha-test write heals it - the engine writes this
    // state many times per frame, so the stale window is sub-frame.
    if (State == D3DRS_ALPHATESTENABLE && g_gpuVendor) {
        int want = (g_alphaToCoverage && g_msColour && g_msaaSamples >= 2 && Value) ? 1 : 0;
        if (want != g_a2cIssuedOn) {
            HRESULT hr = g_origSetRenderState(This, State, Value);
            if (g_gpuVendor == 0x1002) {            // AMD: A2M1 / A2M0 on POINTSIZE
                g_origSetRenderState(This, D3DRS_POINTSIZE,
                    want ? MAKEFOURCC('A','2','M','1') : MAKEFOURCC('A','2','M','0'));
            } else if (g_gpuVendor == 0x10DE) {     // NVIDIA: ATOC on ADAPTIVETESS_Y
                g_origSetRenderState(This, D3DRS_ADAPTIVETESS_Y,
                    want ? MAKEFOURCC('A','T','O','C') : D3DFMT_UNKNOWN);
            }
            g_a2cIssuedOn = want;
            InterlockedIncrement(&g_a2cMirrored);
            return hr;
        }
    }
    return g_origSetRenderState(This, State, Value);
}
#if ENABLE_CASCADE_HUNT
static void LogRtChange(void *pRT);   // defined with the constant probe below
#endif
static volatile LONG g_maxRtIndexSeen = 0;
// Separates "bound a real surface to slot N>0" (genuine MRT) from
// "SetRenderTarget(N>0, NULL)" (just clearing a slot). See HookedSetRenderTarget.
static volatile LONG g_mrtRealBinds = 0;
static volatile LONG g_mrtNullUnbinds = 0;
static volatile LONG g_maxRealRtIndex = 0;
// (g_backbufW/H and g_halfResLogged are declared near the top -
// HookedCreateTexture sits earlier in this file than this block.)

// ---- TextureImp constructor probe: who decides the half size? -------------
// FUN_00aa3ce0 is
// SQEX::CDev::Engine::Dw::RenderInterface::D3d9::TextureImp::TextureImp - it
// copies a descriptor (param_2) into the object and calls the creator. The
// dimensions are already decided when it is entered, so its RETURN ADDRESS is
// the game code that decided them. That is one level above the generic
// plumbing and, unlike a stack scan, it cannot pick up stale frames.
//
// Descriptor layout, derived from the constructor's own copies:
//   *(param_1+1) = *param_2         -> TextureImp +0x04 from desc +0x00
//   *(param_1+3) = param_2[1]       -> TextureImp +0x0c from desc +0x08
//   *(param_1+5) = param_2[2]       -> TextureImp +0x14 from desc +0x10
// and FUN_00aa3960 reads height from [ESI+0x10] and width from [ESI+0x14],
// so in the DESCRIPTOR: height is at +0x0c and width at +0x10.
//
// Prologue is 55 8B EC 8B 45 08 (PUSH EBP / MOV EBP,ESP / MOV EAX,[EBP+8]) -
// a clean 6-byte boundary. __thiscall, so the descriptor is the first stack
// argument.
#define TEXIMP_CTOR_RVA (0x00aa3ce0 - 0x00400000)
static void *g_trampoline_texImpCtor = NULL;
static volatile LONG g_texImpLogged = 0;

// v2. The first version assumed width/height sat at descriptor +0x0c/+0x10,
// derived by reading the constructor's copy statements against the offsets
// FUN_00aa3960 loads. It produced ZERO hits while CreateTexture was plainly
// receiving 1920x1080, so the mapping was wrong.
//
// Rather than guess again, SEARCH the descriptor for the value and report the
// offset it was found at. Self-correcting: if half the presentation width
// appears anywhere in the first 0x40 bytes, that is where the width lives, and
// the log says so. A raw hex dump comes with it, so the layout can be read off
// directly instead of inferred a third time.
__declspec(noinline) void __cdecl OnTexImpCtor_C(void *desc, void *retAddr)
{
    if (!desc) return;

    // ---- ShadowBufPct, at the actual source --------------------------------
    // FUN_00b00f10 (the screen-buffer allocator) computes the half size inline
    //     half = ((dim + 1) >> 1) + 1 & ~1
    // from the settings object's screen fields ([0x0511558c] + 0x10/0x14) and
    // feeds it into this descriptor. The TextureImp wrapper copies the
    // descriptor into its own fields, and the engine derives the viewport and
    // texel maths from the WRAPPER - which is why scaling the D3D texture
    // underneath it failed twice (dark world at 200%, blocky shift at 50%).
    // Rewriting the descriptor here, before the ctor copies it, keeps the
    // wrapper, the texture and everything derived from them consistent.
    //
    // The screen size is read LIVE from the settings object rather than from
    // g_backbufW: the buffers are first created before the device Reset, when
    // g_backbufW is still 0 - that gap is what produced the mixed scaled/
    // unscaled state last attempt. Matching the allocator's own rounding
    // formula, at whatever the current screen size is, catches both the
    // pre-Reset and post-Reset creations.
    __try {
        // ---- SSAA descriptor mode (SsaaMode=1) ---------------------------
        // The route that does NOT touch the resolution field, and therefore
        // cannot resize the window or the swap chain - which is what sank the
        // resolution-write route everywhere except fullscreen-native.
        //
        // Same provenance gate as ShadowBufPct below: only descriptors created
        // while FUN_00b00f10 is on this thread's stack. That allocator builds
        // the whole scene set - full colour, D24S8 and R32F, plus the half trio
        // and the quarter chain - so scaling EVERY member it creates by the
        // same factor enlarges the scene while leaving every internal ratio
        // (half stays half, quarter stays quarter) intact. Scaling only the
        // full members would leave the half trio at half of the UNSCALED
        // screen, i.e. a different ratio than the engine believes it has.
        //
        // The engine then derives viewports and texel maths from the TextureImp
        // wrapper's recorded dimensions, which is exactly what ShadowBufPct
        // proved end-to-end at 200% ("the engine derives the enlarged viewports
        // itself"), and is why this is descriptor-level rather than a scale
        // applied underneath the wrapper - that failed twice.
        //
        // NOTE: while this is active the ShadowBufPct block below will not
        // additionally fire. Its test is "smaller than the screen", and a
        // half-res member scaled 2x is no longer smaller than the (unscaled)
        // screen it compares against. That is acceptable: SSAA already
        // supersamples the shadow buffers along with everything else, which is
        // what ShadowBufPct was for.
        if (g_ssaaMode == 1 && g_ssaaScale > 100 &&
            g_inScreenBufAlloc && g_sbufAllocTid == GetCurrentThreadId()) {
            unsigned int *dw = (unsigned int *)((char *)desc + 0x0c);
            unsigned int *dh = (unsigned int *)((char *)desc + 0x10);
            if (*dw >= 16 && *dh >= 16 && *dw <= 16384 && *dh <= 16384) {
                unsigned int nw = (unsigned int)(((unsigned __int64)*dw *
                                   (unsigned)g_ssaaScale) / 100) & ~3u;
                unsigned int nh = (unsigned int)(((unsigned __int64)*dh *
                                   (unsigned)g_ssaaScale) / 100) & ~3u;
                if (nw < 16) nw = 16;
                if (nh < 16) nh = 16;
                if (nw > 16384) nw = 16384;   // D3D9 texture dimension ceiling
                if (nh > 16384) nh = 16384;
                LONG n = InterlockedIncrement(&g_ssaaDescScaled);
                if (n <= 12) {
                    char sl2[160];
                    sprintf(sl2, "[ssaa] desc %ux%u -> %ux%u (%ld%%, descriptor mode)",
                            *dw, *dh, nw, nh, g_ssaaScale);
                    LogLine(sl2);
                }
                *dw = nw;
                *dh = nh;
            }
        }

        // Provenance gate: only descriptors created while FUN_00b00f10 (the
        // screen-space buffer allocator) is on this thread's stack. Within it,
        // scale every member SMALLER than the screen - the half trio AND the
        // quarter pair, which is the downsample/blur chain the half buffers
        // feed - and leave the full-res members untouched. Keeping the whole
        // chain in proportion matters: scaling only the half level while its
        // blur chain stayed quarter-res would cap the visible result at the
        // blur's resolution anyway.
        // Config value is a percentage of the SCREEN; the engine's own buffers
        // start at half screen, so the descriptor multiplier is twice it.
        // 50 -> x1 (engine default, no-op), 100 -> x2, 200 -> x4.
        LONG pct = g_shadowBufResPct * 2;
        if (pct > 0 && pct != 100 && g_mainModBase &&
            g_inScreenBufAlloc && g_sbufAllocTid == GetCurrentThreadId()) {
            DWORD so = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
            if (so) {
                unsigned int sw = *(unsigned int *)(so + 0x10);
                unsigned int sh = *(unsigned int *)(so + 0x14);
                unsigned int *dw = (unsigned int *)((char *)desc + 0x0c);
                unsigned int *dh = (unsigned int *)((char *)desc + 0x10);
                if (sw >= 320 && sw <= 16384 && sh >= 200 && sh <= 16384 &&
                    *dw >= 16 && *dh >= 16 && (*dw < sw || *dh < sh)) {
                    unsigned int nw = (unsigned int)(((unsigned __int64)*dw * (unsigned)pct) / 100) & ~1u;
                    unsigned int nh = (unsigned int)(((unsigned __int64)*dh * (unsigned)pct) / 100) & ~1u;
                    if (nw < 16) nw = 16;
                    if (nh < 16) nh = 16;
                    // NOT clamped to the screen - supersampling past full
                    // screen is where the visible gain actually appears, so a
                    // screen clamp would silently cap the best setting. Capped
                    // at 16384 instead, the D3D9 texture-dimension ceiling.
                    if (nw > 16384) nw = 16384;
                    if (nh > 16384) nh = 16384;
                    LONG cnt2 = InterlockedIncrement(&g_sbufCtorScaled);
                    if (cnt2 <= 12) {
                        char l2[192];
                        sprintf(l2, "[shadowbuf] alloc desc %ux%u -> %ux%u (%ld%% of screen, screen %ux%u)",
                                *dw, *dh, nw, nh, g_shadowBufResPct, sw, sh);
                        LogLine(l2);
                    }
                    *dw = nw;
                    *dh = nh;
                    return;
                }
            }
        }

        if (!g_backbufW) return;
        unsigned int halfW = g_backbufW / 2, halfH = g_backbufH / 2;
        const unsigned int *d = (const unsigned int *)desc;
        int wOff = -1, hOff = -1, i;
        for (i = 0; i < 16; i++) {
            if (d[i] == halfW && wOff < 0) wOff = i * 4;
            if (d[i] == halfH && hOff < 0) hOff = i * 4;
        }
        if (wOff < 0 || hOff < 0) return;      // not a half-res descriptor
        if (InterlockedIncrement(&g_texImpLogged) > 8) return;

        DWORD ra = (DWORD)(UINT_PTR)retAddr;
        int inMod = (g_mainModBase && ra > g_mainModBase &&
                     g_mainModSize && ra < g_mainModBase + g_mainModSize);
        char l[320];
        int o = sprintf(l, "[halfres] desc %ux%u (w@+0x%02X h@+0x%02X) decided by %08X%s | raw:",
                        halfW, halfH, wOff, hOff,
                        inMod ? ra - g_mainModBase + 0x00400000 : ra,
                        inMod ? " (ghidra)" : " (outside module)");
        for (i = 0; i < 12; i++) o += sprintf(l + o, " %08X", d[i]);
        LogLine(l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void Detour_texImpCtor(void)
{
    __asm {
        pushad
        pushfd
        // After PUSHAD(32) + PUSHFD(4): [esp+36] = return address,
        // [esp+40] = the descriptor (first stack arg of the __thiscall).
        mov eax, [esp + 36]
        push eax
        mov eax, [esp + 44]
        push eax
        call OnTexImpCtor_C
        add esp, 8
        popfd
        popad
        jmp dword ptr [g_trampoline_texImpCtor]
    }
}
static volatile LONG g_rtLogged = 0;
// First run came back with ZERO CreateRenderTarget/CreateDepthStencilSurface
// calls despite MRT index 3 being bound - so this engine creates its render
// targets as TEXTURES with D3DUSAGE_RENDERTARGET and takes a surface off
// them via GetSurfaceLevel. That is the normal approach when the RT must
// later be SAMPLED (exactly what a G-buffer or a shadow map needs), and it
// means the inventory has to be collected at CreateTexture instead.
// (g_rtTexLogged is a tentative definition near the top - HookedCreateTexture
// sits earlier in this file than this block and needs it.)
// The only CreateDevice seen was 1x1 with MULTISAMPLE=0 - far too small to
// be the real swap chain, so it is a caps/dummy device and its MSAA value
// says nothing about the real one. The real presentation parameters must
// arrive later via Reset (or an additional swap chain), so hook that too
// before drawing any conclusion about whether MSAA is requested.
typedef HRESULT (STDMETHODCALLTYPE *PFN_DeviceReset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static PFN_DeviceReset g_origReset = NULL;

// ---- StretchRect probe: the present path -------------------------------
// Whole-frame edge stats proved the image is SOFTENED between the finished
// scene target (hardStep 0.323, 23,401 edges) and the presented backbuffer
// (0.147, 10,268) while the post chain leaves the scene target untouched.
// No quad is ever drawn to the backbuffer, so the copy that gets it there is
// a StretchRect - and if its source is smaller than its destination, the
// "AA filter" is simply a bilinear UPSCALE. This logs the copies with sizes,
// formats and filter so the path can be read rather than guessed.
typedef HRESULT (STDMETHODCALLTYPE *PFN_StretchRect)(
    IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
    IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
static PFN_StretchRect g_origStretchRect = NULL;
static volatile LONG g_srLogged = 0;
// Distinct-blit signatures already reported by the SSAA probe below.
#define SR_SEEN_MAX 64
static unsigned int g_srSeen[SR_SEEN_MAX];
static volatile LONG g_srSeenCount = 0;
// SSAA downsample accounting: blits upgraded to LINEAR, and blits where the
// driver refused it and the engine's own filter was used instead. A non-zero
// fail count with a black or decimated image is the first thing to check.
static volatile LONG g_ssaaLinearBlits = 0;
static volatile LONG g_ssaaLinearFails = 0;
// ---- Honouring the chosen resolution in borderless fullscreen -------------
// The game is ALWAYS borderless (every Reset logs windowed=1), so the
// backbuffer is always the desktop size and a lower resolution setting is
// rendered small and upscaled into it. With SSAA that produces a surprise: at
// a 1080p setting on a 4K desktop, 2x renders exactly 3840x2160, lands 1:1 on
// the backbuffer, and the player who asked for 1080p silently gets native 4K
// at 4K cost.
//
// If someone selects 1080p, 1080p is the promise. So when the chosen
// resolution is BELOW the presented size, the final copy is routed through an
// intermediate at the chosen resolution: scene -> intermediate (the
// supersample resolve) -> backbuffer (the presentation upscale). The result is
// a genuinely 4x-supersampled 1080p image, filling a 4K screen.
//
// Both steps are LINEAR. The upscale is deliberately NOT point/integer: an
// unfiltered 1080p->4K doubling was tested earlier in this work and the
// verdict was "raw pixels" - disliked - so smooth wins over crisp here on
// evidence rather than taste.
static IDirect3DSurface9 *g_ssaaInter = NULL;
static UINT g_ssaaInterW = 0, g_ssaaInterH = 0;
static volatile LONG g_ssaaInterBlits = 0;
static volatile LONG g_ssaaInterFails = 0;
// 1 = honour the chosen resolution (downsample to it, then upscale to the
// display). 0 = present the supersampled render directly at display size,
// i.e. the older behaviour where the resolution setting is effectively
// overridden. Config-only; the sensible default is to do what was asked.
static volatile LONG g_ssaaOutputRes = 1;

static void SsaaReleaseIntermediate(void)
{
    if (g_ssaaInter) {
        IDirect3DSurface9_Release(g_ssaaInter);
        g_ssaaInter = NULL;
    }
    g_ssaaInterW = g_ssaaInterH = 0;
}
// Swap-chain resizes intercepted and held at the pre-scale size. 0 while a
// scale is set means the presentation followed the scaled resolution and no
// supersampling is happening - the exact failure seen in the first test.
static volatile LONG g_ssaaPins = 0;

static HRESULT STDMETHODCALLTYPE HookedStretchRect(
    IDirect3DDevice9 *This, IDirect3DSurface9 *pSrc, const RECT *pSrcRect,
    IDirect3DSurface9 *pDst, const RECT *pDstRect, D3DTEXTUREFILTERTYPE Filter)
{
    // ---- SSAA reconnaissance (2026-08-12) --------------------------------
    // Deliberately NOT inside ENABLE_SURFACE_DIAG below: that block does
    // surface analysis and BMP dumps, which is far more than this needs and
    // is retired. This is the cheap half - sizes, filter, and the pass it
    // happens in - and it answers the question that decides whether native
    // SSAA is even worth building:
    //
    //   The present path was measured as a 1:1 copy with D3DTEXF_POINT. If it
    //   STAYS point while downscaling, a supersampled scene gets DECIMATED,
    //   not filtered - three of every four pixels thrown away, zero
    //   antialiasing, full cost. In that case SSAA needs its own downsample
    //   pass (or the filter forced to LINEAR) before it can work at all.
    //
    // Filter values for reading the log: 0=NONE, 1=POINT, 2=LINEAR.
    // Capped, and only while LogPassRts is on, so this is one branch
    // otherwise. g_msFrameSeq > 600 skips the loading-screen blits, which
    // previously produced a log full of all-black 1:1 copies.
#if ENABLE_PASS_PROBE
    // DEDUPED, not capped-by-count: the first version logged the first 40
    // blits and every one came back identical, which burns the sample on one
    // repeating copy and says nothing about the ones that differ. This keeps
    // one line per DISTINCT (pass, sizes, formats, filter) combination - the
    // same approach the [pass] RT inventory already uses, for the same reason.
    // Formats are logged because they are what identifies the destination:
    // fmt 22 = X8R8G8B8 is the backbuffer, 21 = A8R8G8B8 is a scene target,
    // and without them "3840x2160 -> 3840x2160" cannot be told apart from an
    // intermediate post-chain copy.
    if (g_logPassRts && pSrc && pDst && g_msFrameSeq > 600) {
        D3DSURFACE_DESC s, d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(pSrc, &s)) &&
            SUCCEEDED(IDirect3DSurface9_GetDesc(pDst, &d))) {
            LONG sw = pSrcRect ? (pSrcRect->right - pSrcRect->left) : (LONG)s.Width;
            LONG sh = pSrcRect ? (pSrcRect->bottom - pSrcRect->top) : (LONG)s.Height;
            LONG dw = pDstRect ? (pDstRect->right - pDstRect->left) : (LONG)d.Width;
            LONG dh = pDstRect ? (pDstRect->bottom - pDstRect->top) : (LONG)d.Height;
            LONG p = g_curPass;
            if (p < 0 || p >= PASS_COUNT) p = PASS_NONE;

            // Cheap signature of everything the line reports.
            unsigned int sig = (unsigned int)p * 2654435761u
                             ^ ((unsigned int)sw << 1) ^ ((unsigned int)sh << 3)
                             ^ ((unsigned int)dw << 5) ^ ((unsigned int)dh << 7)
                             ^ ((unsigned int)s.Format << 11)
                             ^ ((unsigned int)d.Format << 13)
                             ^ ((unsigned int)Filter << 17);
            LONG n = g_srSeenCount, i, found = 0;
            if (n > SR_SEEN_MAX) n = SR_SEEN_MAX;
            for (i = 0; i < n; i++) if (g_srSeen[i] == sig) { found = 1; break; }
            if (!found && n < SR_SEEN_MAX) {
                g_srSeen[n] = sig;
                g_srSeenCount = n + 1;
                const char *fname = (Filter == D3DTEXF_NONE)  ? "NONE"
                                  : (Filter == D3DTEXF_POINT) ? "POINT"
                                  : (Filter == D3DTEXF_LINEAR) ? "LINEAR" : "other";
                char l[288];
                sprintf(l, "[ssaa-probe] %-18s %ldx%ld fmt=%d -> %ldx%ld fmt=%d  filter=%s(%d)%s%s%s",
                        g_passNames[p], sw, sh, (int)s.Format, dw, dh, (int)d.Format,
                        fname, (int)Filter,
                        (sw != dw || sh != dh) ? "  *** SCALING ***" : "  (1:1)",
                        (d.Format == D3DFMT_X8R8G8B8) ? "  [dst=BACKBUFFER fmt]" : "",
                        (pSrc == (IDirect3DSurface9 *)g_sceneRtMain) ? "  [src=SCENE-LATCHED]" : "");
                LogLine(l);
            }
        }
    }
#endif  // ENABLE_PASS_PROBE
    // 40 was too few to show VARIETY - every line came back identical, which
    // could equally mean "one copy per frame" or "the interesting copies
    // happened after the cap". 200 showed a single repeating pattern, so the
    // present path really is one 1:1 point copy per frame; the log now starts
    // only once gameplay is up, so the sample is of real frames.
#if ENABLE_SURFACE_DIAG
    if (g_dumpShaders && pSrc && pDst && g_msFrameSeq > 600 &&
        InterlockedIncrement(&g_srLogged) <= 60) {
        D3DSURFACE_DESC s, d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(pSrc, &s)) &&
            SUCCEEDED(IDirect3DSurface9_GetDesc(pDst, &d))) {
            // Rects matter as much as surface sizes: a full-size surface can
            // still be copied from a sub-rect, which scales just the same.
            LONG sw = pSrcRect ? (pSrcRect->right - pSrcRect->left) : (LONG)s.Width;
            LONG sh = pSrcRect ? (pSrcRect->bottom - pSrcRect->top) : (LONG)s.Height;
            LONG dw = pDstRect ? (pDstRect->right - pDstRect->left) : (LONG)d.Width;
            LONG dh = pDstRect ? (pDstRect->bottom - pDstRect->top) : (LONG)d.Height;
            int ours = (pSrc == g_msColour || pSrc == g_msR32f);
            // WHICH surface is it? The final blit's source turned out NOT to
            // be the latched scene target, so the composite the user actually
            // sees lives on another surface. Name it against the tracked set.
            LONG si = -1, nrt = g_sceneRtCount;
            if (nrt > SCENE_RT_MAX) nrt = SCENE_RT_MAX;
            for (LONG i = 0; i < nrt; i++)
                if (g_sceneRts[i] == (void *)pSrc) { si = i + 1; break; }
            char tag[64];
            if (si > 0) sprintf(tag, "(tracked #%ld, %ld draws)", si, g_sceneRtDraws[si - 1]);
            else strcpy(tag, "(UNTRACKED - never bound as RT0)");
            char l[320];
            sprintf(l, "[stretch] src=%p%s%s %ldx%ld fmt=%d -> %ldx%ld fmt=%d filter=%d%s%s%s",
                    (void *)pSrc,
                    (pSrc == (IDirect3DSurface9 *)g_sceneRtMain) ? "(SCENE-LATCHED)" : "",
                    tag,
                    sw, sh, (int)s.Format, dw, dh, (int)d.Format, (int)Filter,
                    (sw != dw || sh != dh) ? "  *** SCALING ***" : "",
                    (d.Format == D3DFMT_X8R8G8B8) ? "  [->BACKBUFFER fmt]" : "",
                    ours ? "  (our MSAA resolve)" : "");
            LogLine(l);
            // Measure the SOURCE of the final blit once: if it is already as
            // soft as the presented frame (hardStep ~0.15 vs the scene
            // target's ~0.32), the smoothing happened while this surface was
            // produced - and that is the pass to attack.
            // Deliberately NOT the first blit: the first one happens during a
            // black loading screen, and analysing it returned an all-zero
            // frame (grad=0, edges=0) before the scene target had even been
            // latched. Wait for real gameplay content.
            if (d.Format == D3DFMT_X8R8G8B8 && sw == dw && sh == dh &&
                g_msFrameSeq > 600 && g_sceneRtMain) {
                static LONG analysed = 0;
                if (InterlockedCompareExchange(&analysed, 1, 0) == 0) {
                    AnalyseSurfaceStage(This, pSrc, "FINAL-BLIT-SOURCE");
                    D3DSURFACE_DESC fd;
                    if (SUCCEEDED(IDirect3DSurface9_GetDesc(pSrc, &fd))) {
                        UINT cw = 960, ch = 540;
                        UINT cx = (fd.Width > cw) ? (fd.Width - cw) / 2 : 0;
                        UINT cy = (fd.Height > ch) ? (fd.Height - ch) / 2 : 0;
                        DumpSurfaceCropToBmp(This, pSrc, "stage50_finalblitsrc.bmp", 1, cx, cy, cw, ch);
                    }
                    if (g_sceneRtMain) {
                        AnalyseSurfaceStage(This, (IDirect3DSurface9 *)g_sceneRtMain,
                                            "SCENE-TARGET-at-same-moment");
                        // Same crop from both surfaces at the same instant -
                        // a direct A/B of "what was rendered" against "what is
                        // presented", which is the whole question.
                        D3DSURFACE_DESC md;
                        if (SUCCEEDED(IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)g_sceneRtMain, &md))) {
                            UINT cw = 960, ch = 540;
                            UINT cx = (md.Width > cw) ? (md.Width - cw) / 2 : 0;
                            UINT cy = (md.Height > ch) ? (md.Height - ch) / 2 : 0;
                            DumpSurfaceCropToBmp(This, (IDirect3DSurface9 *)g_sceneRtMain,
                                                 "stage51_scenetarget.bmp", 1, cx, cy, cw, ch);
                        }
                    }
                }
            }
        }
    }
#endif  // ENABLE_SURFACE_DIAG
    // ---- SSAA: honour the chosen resolution ------------------------------
    // Only the final copy to the backbuffer, only when supersampling is live,
    // and only when the chosen resolution is genuinely below the presented
    // size. Whole-surface copies only (NULL rects): a sub-rect copy is some
    // other operation and must not be rerouted.
    if (g_ssaaActive && g_ssaaOutputRes && pSrc && pDst && !pSrcRect && !pDstRect &&
        g_mainModBase) {
        D3DSURFACE_DESC s, d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(pSrc, &s)) &&
            SUCCEEDED(IDirect3DSurface9_GetDesc(pDst, &d)) &&
            d.Format == D3DFMT_X8R8G8B8) {          // the backbuffer
            LONG chosenW = 0, chosenH = 0;
            __try {
                DWORD so = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
                if (so) {
                    chosenW = *(LONG *)(so + 0x10);
                    chosenH = *(LONG *)(so + 0x14);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { chosenW = chosenH = 0; }

            if (chosenW >= 320 && chosenH >= 200 &&
                (UINT)chosenW < d.Width && (UINT)chosenH < d.Height &&
                s.Width > (UINT)chosenW) {
                if (g_ssaaInter && (g_ssaaInterW != (UINT)chosenW ||
                                    g_ssaaInterH != (UINT)chosenH))
                    SsaaReleaseIntermediate();
                if (!g_ssaaInter) {
                    // D3DPOOL_DEFAULT render target, so it must be released
                    // before any device Reset - handled next to MsaaRelease.
                    if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(
                            This, (UINT)chosenW, (UINT)chosenH, d.Format,
                            D3DMULTISAMPLE_NONE, 0, FALSE, &g_ssaaInter, NULL))) {
                        g_ssaaInterW = (UINT)chosenW;
                        g_ssaaInterH = (UINT)chosenH;
                        char il[176];
                        sprintf(il, "[ssaa] output-res intermediate %ldx%ld created "
                                    "(scene %ux%u -> %ldx%ld -> screen %ux%u)",
                                chosenW, chosenH, s.Width, s.Height,
                                chosenW, chosenH, d.Width, d.Height);
                        LogLine(il);
                    } else {
                        InterlockedIncrement(&g_ssaaInterFails);
                    }
                }
                if (g_ssaaInter) {
                    HRESULT h1 = g_origStretchRect(This, pSrc, NULL, g_ssaaInter, NULL,
                                                   D3DTEXF_LINEAR);
                    if (SUCCEEDED(h1)) {
                        HRESULT h2 = g_origStretchRect(This, g_ssaaInter, NULL, pDst, NULL,
                                                       D3DTEXF_LINEAR);
                        if (SUCCEEDED(h2)) {
                            InterlockedIncrement(&g_ssaaInterBlits);
                            return h2;
                        }
                    }
                    // Either leg failing means the two-step route is not
                    // viable here; fall through to the engine's own single
                    // copy rather than dropping the frame.
                    InterlockedIncrement(&g_ssaaInterFails);
                }
            }
        }
    }

    // ---- SSAA downsample filter -----------------------------------------
    // The scene->backbuffer copy is POINT. That is correct for the 1:1 copy
    // it normally performs, but while supersampling it becomes a DOWNSCALE,
    // and POINT downscaling decimates - three of every four pixels discarded
    // at 2x, i.e. zero antialiasing for the entire cost. Forcing LINEAR makes
    // it a real box filter (exactly a 4-tap average at an integer 2x ratio).
    //
    // Applied to every blit that SHRINKS while SSAA is active, not just the
    // present copy: the bloom/post pyramid also takes a POINT downscale
    // straight off the scene target, which at a supersampled size would alias
    // the bloom source. Only downscales are touched - upscales and 1:1 copies
    // keep the engine's own choice, so vanilla behaviour is bit-identical
    // whenever SsaaScale is 100.
    //
    // The engine itself already uses LINEAR for its own scaling copies (the
    // menu blur chain), so this is consistent with how it filters elsewhere
    // rather than an outside imposition.
    if (g_ssaaActive && pSrc && pDst &&
        (Filter == D3DTEXF_NONE || Filter == D3DTEXF_POINT)) {
        D3DSURFACE_DESC s, d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(pSrc, &s)) &&
            SUCCEEDED(IDirect3DSurface9_GetDesc(pDst, &d))) {
            LONG sw = pSrcRect ? (pSrcRect->right - pSrcRect->left) : (LONG)s.Width;
            LONG sh = pSrcRect ? (pSrcRect->bottom - pSrcRect->top) : (LONG)s.Height;
            LONG dw = pDstRect ? (pDstRect->right - pDstRect->left) : (LONG)d.Width;
            LONG dh = pDstRect ? (pDstRect->bottom - pDstRect->top) : (LONG)d.Height;
            if (sw > dw || sh > dh) {
                HRESULT hrl = g_origStretchRect(This, pSrc, pSrcRect, pDst, pDstRect,
                                                D3DTEXF_LINEAR);
                if (SUCCEEDED(hrl)) {
                    InterlockedIncrement(&g_ssaaLinearBlits);
                    return hrl;
                }
                // Not every surface/driver combination will filter on
                // StretchRect (it needs D3DPTFILTERCAPS_MINFLINEAR for the
                // source). Fall back to the engine's own filter rather than
                // failing the copy - a decimated frame beats a missing one,
                // and the counter below makes the fallback visible.
                InterlockedIncrement(&g_ssaaLinearFails);
            }
        }
    }
    return g_origStretchRect(This, pSrc, pSrcRect, pDst, pDstRect, Filter);
}

static HRESULT STDMETHODCALLTYPE HookedCreateRenderTarget(
    IDirect3DDevice9 *This, UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS,
    DWORD MSQual, BOOL Lockable, IDirect3DSurface9 **ppSurf, HANDLE *pShared)
{
    HRESULT hr = g_origCreateRT(This, W, H, Fmt, MS, MSQual, Lockable, ppSurf, pShared);
    if (InterlockedIncrement(&g_rtLogged) <= 64) {
        char l[192];
        sprintf(l, "[rt] CreateRenderTarget %ux%u fmt=%d multisample=%d qual=%lu lockable=%d hr=0x%08lX",
                W, H, (int)Fmt, (int)MS, (unsigned long)MSQual, (int)Lockable, (unsigned long)hr);
        LogLine(l);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDepthStencil(
    IDirect3DDevice9 *This, UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS,
    DWORD MSQual, BOOL Discard, IDirect3DSurface9 **ppSurf, HANDLE *pShared)
{
    HRESULT hr = g_origCreateDS(This, W, H, Fmt, MS, MSQual, Discard, ppSurf, pShared);
    if (InterlockedIncrement(&g_rtLogged) <= 64) {
        char l[192];
        sprintf(l, "[rt] CreateDepthStencil %ux%u fmt=%d multisample=%d qual=%lu discard=%d hr=0x%08lX",
                W, H, (int)Fmt, (int)MS, (unsigned long)MSQual, (int)Discard, (unsigned long)hr);
        LogLine(l);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedSetRenderTarget(
    IDirect3DDevice9 *This, DWORD RenderTargetIndex, IDirect3DSurface9 *pRT)
{
    // ALWAYS-ON, and the distinction matters: the original probe raised this
    // max on the INDEX alone, counting SetRenderTarget(n, NULL) - which is
    // UNBINDING a slot, not using it. That inflated max index to 3 and was
    // read as "MRT / G-buffer in use (deferred)". The two counters below
    // separate real binds from unbinds so the claim can be checked instead of
    // assumed.
    if ((LONG)RenderTargetIndex > g_maxRtIndexSeen) g_maxRtIndexSeen = (LONG)RenderTargetIndex;
    g_dev = This;

    // ---- Frame-stage capture v2 (see the block above g_stageDumpState) ----
    // v1 fired on PASS TRANSITIONS, which turned out to sample the scene
    // target at moments when it was empty or stale (stages 1-3 held 30 edges
    // total) - the timing was ambiguous, so the numbers meant nothing.
    //
    // v2 fires whenever the engine UNBINDS the scene target, which is exactly
    // "something just finished drawing into it" - unambiguous, and independent
    // of MSAA so it works in the MSAA-off configuration in question. Each
    // capture reports whole-frame edge statistics (no crop, so the bridge at
    // the back is measured wherever it is) and the first/last also write a
    // full-res centre crop for eyeballing.
#if ENABLE_SURFACE_DIAG
    // F9 capture: full frame, full resolution, no crop - taken at a scene
    // target unbind so the frame is complete. Runs regardless of DumpShaders
    // so it is always available for aiming at a specific view.
    //
    // v2: the previous version tracked the previously-bound target INSIDE a
    // branch that required the scene target NOT to be bound, so the tracker
    // could never hold the scene target and the capture was unreachable. The
    // tracker is now updated on every slot-0 bind, unconditionally.
    if (RenderTargetIndex == 0) {
        if (g_captureRequest && g_sceneRtMain &&
            g_prevRt0 == g_sceneRtMain && (void *)pRT != g_sceneRtMain &&
            InterlockedCompareExchange(&g_captureRequest, 0, 1) == 1) {
            LONG n = InterlockedIncrement(&g_captureSeq);
            char nm[64];
            sprintf(nm, "capture%02ld_scene.bmp", n);
            DumpSurfaceCropToBmp(This, (IDirect3DSurface9 *)g_sceneRtMain, nm, 1, 0, 0, 0, 0);
            AnalyseSurfaceStage(This, (IDirect3DSurface9 *)g_sceneRtMain, "F9-CAPTURE");
            char cl[176];
            sprintf(cl, "[capture] wrote %s (full frame, MsaaSamples=%ld)", nm, g_msaaSamples);
            LogLine(cl);
        }
        g_prevRt0 = (void *)pRT;
    }

    if (g_dumpShaders && g_sceneRtMain && RenderTargetIndex == 0) {
        static void *lastBound = NULL;
        if (g_stageDumpState == 0 && g_msFrameSeq > 600) {
            g_stageDumpState = 1;
            g_stageDumpFrame = g_msFrameSeq + 1;
        } else if (g_stageDumpState == 1 && g_msFrameSeq == g_stageDumpFrame &&
                   lastBound == g_sceneRtMain && (void *)pRT != g_sceneRtMain) {
            LONG idx = ++g_stageDumpIdx;
            if (idx <= 10) {
                LONG p = g_curPass;
                char lab[96];
                sprintf(lab, "%02ld_after_%s", idx,
                        (p >= 0 && p < PASS_COUNT) ? g_passNames[p] : "none");
                AnalyseSurfaceStage(This, (IDirect3DSurface9 *)g_sceneRtMain, lab);
                if (idx <= 2 || idx >= 6) {
                    unsigned int rw, rh;
                    GetInternalRenderSize(&rw, &rh);
                    UINT cw = 960, ch = 540;
                    UINT cx = (rw > cw) ? (rw - cw) / 2 : 0;
                    UINT cy = (rh > ch) ? (rh - ch) / 2 : 0;
                    char nm[112];
                    sprintf(nm, "stage%s.bmp", lab);
                    for (char *q = nm; *q; q++)
                        if (*q=='('||*q==')'||*q=='+'||*q=='/'||*q==' ') *q = '_';
                    DumpSurfaceCropToBmp(This, (IDirect3DSurface9 *)g_sceneRtMain,
                                         nm, 1, cx, cy, cw, ch);
                }
            }
        } else if (g_stageDumpState == 1 && g_msFrameSeq > g_stageDumpFrame && g_stageDumpIdx > 0) {
            g_stageDumpState = 2;
            // The backbuffer now holds the fully presented previous frame -
            // ground truth for what actually reached the screen. If its
            // hardStep is far below the scene target's, the smoothing happens
            // in the present path, outside every hook in this file.
            IDirect3DSurface9 *bb = NULL;
            if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(This, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                AnalyseSurfaceStage(This, bb, "99_BACKBUFFER(presented)");
                D3DSURFACE_DESC bd;
                if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &bd))) {
                    UINT cw = 960, ch = 540;
                    UINT cx = (bd.Width > cw) ? (bd.Width - cw) / 2 : 0;
                    UINT cy = (bd.Height > ch) ? (bd.Height - ch) / 2 : 0;
                    DumpSurfaceCropToBmp(This, bb, "stage99_backbuffer.bmp", 1, cx, cy, cw, ch);
                }
                IDirect3DSurface9_Release(bb);
            }
            LogLine("[stage] frame-stage capture complete");
        }
        if (pRT) lastBound = (void *)pRT;
    }
#endif  // ENABLE_SURFACE_DIAG

    // How many DISTINCT full-screen A8R8G8B8 surfaces get bound at slot 0?
    // The MSAA substitution matches on format+size, not identity, so if the
    // engine uses several of the six such targets the RT inventory found, all
    // of them are redirected into ONE MS surface and the resolve returns the
    // combined result to whichever was bound last. Everything else is lost -
    // which fits capturing only ~8% of the 450k draws that reach full-screen
    // colour targets.
    if (RenderTargetIndex == 0) {
        g_curSceneRtIdx = -1;
        g_curDepthRtIdx = -1;
        if (pRT && g_backbufW) {
            // Match candidates against the engine's INTERNAL render size, not
            // the backbuffer: borderless keeps the backbuffer at desktop res
            // while the menu resizes the internal targets, and requiring the
            // two to be equal is exactly why MSAA used to engage only at
            // native. The latch itself stays identity + draw-count based.
            unsigned int rw, rh;
            GetInternalRenderSize(&rw, &rh);
            D3DSURFACE_DESC id;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(pRT, &id)) &&
                id.Width == rw && id.Height == rh) {
                if (id.Format == D3DFMT_A8R8G8B8) {
                    LONG n = g_sceneRtCount, i, idx = -1;
                    if (n > SCENE_RT_MAX) n = SCENE_RT_MAX;
                    for (i = 0; i < n; i++) if (g_sceneRts[i] == (void *)pRT) { idx = i; break; }
                    if (idx < 0 && n < SCENE_RT_MAX) {
                        idx = n;
                        g_sceneRts[n] = (void *)pRT;
                        g_sceneRtCount = n + 1;
                        char sl2[176];
                        sprintf(sl2, "[scenert] distinct full-screen A8R8G8B8 target #%ld: %p"
                                     " (first bound in pass %s)",
                                n + 1, (void *)pRT,
                                g_passNames[(g_curPass >= 0 && g_curPass < PASS_COUNT) ? g_curPass : 0]);
                        LogLine(sl2);
                    }
                    g_curSceneRtIdx = idx;
                } else if (id.Format == D3DFMT_R32F) {
                    // Fullscreen R32F: the linear-depth prepass target family.
                    LONG n = g_depthRtCount, i, idx = -1;
                    if (n > SCENE_RT_MAX) n = SCENE_RT_MAX;
                    for (i = 0; i < n; i++) if (g_depthRts[i] == (void *)pRT) { idx = i; break; }
                    if (idx < 0 && n < SCENE_RT_MAX) {
                        idx = n;
                        g_depthRts[n] = (void *)pRT;
                        g_depthRtCount = n + 1;
                        char sl2[176];
                        sprintf(sl2, "[depthrt] distinct full-screen R32F target #%ld: %p"
                                     " (first bound in pass %s)",
                                n + 1, (void *)pRT,
                                g_passNames[(g_curPass >= 0 && g_curPass < PASS_COUNT) ? g_curPass : 0]);
                        LogLine(sl2);
                    }
                    g_curDepthRtIdx = idx;
                }
            }
        }
    }

    // Remember what is bound at slot 0 so draws can be attributed to a target.
    if (RenderTargetIndex == 0) {
        if (pRT) {
            D3DSURFACE_DESC cd;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(pRT, &cd))) {
                g_curRtFmt = (LONG)cd.Format;
                g_curRtW = (LONG)cd.Width;
                g_curRtH = (LONG)cd.Height;
            }
        } else {
            g_curRtFmt = g_curRtW = g_curRtH = 0;
        }
    }

    // ---- MSAA substitution / resolve (see the MSAA block above) ----------
    if (RenderTargetIndex == 0) {
        // Binding away from our MS surface: sync it back to the engine's
        // texture, because the engine may sample that texture at any point.
        //
        // Safe to do on every unbind now that substitution is by IDENTITY: the
        // MS surface ACCUMULATES everything ever drawn to the scene target and
        // is never reset except by the engine's own Clear, so each resolve
        // writes the complete content, not a fragment. The engine can never
        // draw into the real surface behind our back, because every bind of it
        // is redirected. (Resolving per-unbind was wrong under shape matching,
        // where three surfaces shared one MS buffer.)
        if (pRT != g_msColour && g_msActive) {
            if (g_msHasContent) MsaaResolve(This);
            g_msActive = 0;
        }
        if (pRT != g_msR32f && g_msR32fActive) {
            if (g_msR32fHasContent) MsaaResolveR32f(This);
            g_msR32fActive = 0;
        }
        // Depth restore only when leaving BOTH of our targets - the prepass
        // and colour episodes deliberately share the MS depth, and restoring
        // between them would discard the prepass Z the whole v6 fix exists
        // to preserve. (It survives on the MS depth surface itself; what must
        // not happen is the ENGINE depth being current while ours is expected.)
        if (pRT != g_msColour && pRT != g_msR32f)
            MsaaRestoreDepth(This);

        // Hot-toggle OFF: once nothing of ours is bound, give the ~130-790MB
        // back. Render thread, between episodes - the only safe place.
        if (g_msColour && g_msaaSamples == 0 &&
            !g_msActive && !g_msR32fActive && !g_msDepthBound) {
            MsaaReleaseSurfaces();
            LogLine("[msaa] surfaces released (toggled off in GUI)");
        }

        // Substitute by IDENTITY - only the latched scene target, in any pass.
        // The previous condition matched "any full-screen A8R8G8B8 during
        // MULTI_SAMPLE", which is both too broad (three surfaces share that
        // shape) and too narrow (the pass tag has a blind window before the
        // first handler of each frame runs, and the scene target also takes
        // draws outside MULTI_SAMPLE - 1,024,071 total vs 566,220 in-pass).
        // samples == 1 is a CONTROL, not a setting: it substitutes a plain
        // non-multisampled surface through the identical path (same
        // substitution, same depth pairing, same StretchRect resolve). It
        // isolates the two remaining explanations for the black screen -
        //   renders correctly at 1 -> the plumbing is sound and something
        //                             about a MULTISAMPLED target is the issue
        //   still black at 1       -> the substitution logic itself is wrong
        // The engine renders briefly then goes black exactly when the latch
        // fires, and a resolution change (which releases our surfaces and
        // stops substitution) restores it - so substitution is certainly the
        // cause; this says which half.
        LONG samples = g_msaaSamples;
        if (samples >= 1 && pRT && g_sceneRtMain && (void *)pRT == g_sceneRtMain) {
            D3DSURFACE_DESC sd;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(pRT, &sd)) &&
                sd.MultiSampleType == D3DMULTISAMPLE_NONE) {
                // Lazily build the pair, and rebuild if the size changed (a
                // device Reset releases them, but an area change can resize)
                // or the GUI changed the sample count mid-session.
                // SSAA and MSAA multiply, and the product is quadratic in the
                // SSAA scale: the MS surfaces are built at the (now scaled)
                // internal render size, so 2x SSAA + 8x MSAA at 4K means an
                // MS pair at 7680x4320 x8 - over 2 GB before the scene targets
                // and shadow maps are counted. A stress test at those settings
                // reached 5+ GB and ended in a device Reset storm.
                //
                // Not blocked - the user's explicit call is that people may
                // combine these if they want (photo modes). But the projected
                // cost is logged BEFORE the allocation, so when a session does
                // fall over the log says why instead of leaving it a mystery.
                if (!g_msColour || g_msW != sd.Width || g_msH != sd.Height ||
                    g_msCreatedSamples != samples) {
                    if (samples > 1) {
                        unsigned __int64 pairMB2 =
                            ((unsigned __int64)sd.Width * sd.Height * 4 *
                             (unsigned)samples * 2) >> 20;
                        if (pairMB2 > 1024) {
                            char wl[224];
                            sprintf(wl, "[msaa] large MS pair: %ux%u x%ld needs ~%llu MB"
                                        " (SSAA scale %ld%% multiplies this)",
                                    sd.Width, sd.Height, samples, pairMB2, g_ssaaScale);
                            LogLine(wl);
                        }
                    }
                    MsaaReleaseSurfaces();
                    D3DMULTISAMPLE_TYPE mst = (samples <= 1)
                        ? D3DMULTISAMPLE_NONE : (D3DMULTISAMPLE_TYPE)samples;
                    HRESULT c1 = g_origCreateRT
                        ? g_origCreateRT(This, sd.Width, sd.Height, D3DFMT_A8R8G8B8,
                                         mst, 0, FALSE, &g_msColour, NULL)
                        : E_FAIL;
                    // Discard=FALSE, deliberately. With TRUE, D3D9 declares the
                    // depth contents INVALID after any SetDepthStencilSurface
                    // that binds a different surface - and we swap the depth
                    // on every substitute/restore cycle, thousands of times a
                    // session. That destroys depth continuously, so geometry
                    // fails the test and draws succeed while writing nothing:
                    // exactly the observed failedMS=0 black screen.
                    //
                    // The non-MS control renders correctly through this same
                    // path, which fits - drivers generally ignore Discard for
                    // non-multisampled depth, so only the MS case was affected.
                    //
                    // If a driver refuses multisampled depth with Discard=FALSE
                    // the creation fails, is logged, and MSAA falls back to off
                    // rather than rendering incorrectly.
                    HRESULT c2 = g_origCreateDS
                        ? g_origCreateDS(This, sd.Width, sd.Height, D3DFMT_D24S8,
                                         mst, 0, FALSE, &g_msDepth, NULL)
                        : E_FAIL;
                    if (FAILED(c2) && g_origCreateDS) {
                        // Retry with Discard=TRUE so a refusal is distinguishable
                        // from an unsupported format/size combination.
                        HRESULT c2b = g_origCreateDS(This, sd.Width, sd.Height, D3DFMT_D24S8,
                                                     mst, 0, TRUE, &g_msDepth, NULL);
                        char l[176];
                        sprintf(l, "[msaa] MS depth Discard=FALSE refused (0x%08lX); "
                                   "Discard=TRUE %s", (unsigned long)c2,
                                SUCCEEDED(c2b) ? "accepted - depth will be discarded on every"
                                                 " surface swap, expect black" : "also failed");
                        LogLine(l);
                        c2 = c2b;
                    }
                    if (SUCCEEDED(c1) && SUCCEEDED(c2)) {
                        g_msW = sd.Width; g_msH = sd.Height;
                        g_msCreatedSamples = samples;
                        // Memory snapshot for the 4K theory: the pair is
                        // ~2*(W*H*4*samples) bytes. If the driver's remaining
                        // texture memory or the 32-bit process's free address
                        // space is close to that, the black screen is a
                        // memory cliff, not a rendering bug.
                        DWORD texFreeMB =
                            (DWORD)(IDirect3DDevice9_GetAvailableTextureMem(This) >> 20);
                        g_availTexMemMB = (LONG)texFreeMB;
                        MEMORYSTATUSEX mst2; mst2.dwLength = sizeof(mst2);
                        DWORD vaFreeMB = 0;
                        if (GlobalMemoryStatusEx(&mst2))
                            vaFreeMB = (DWORD)(mst2.ullAvailVirtual >> 20);
                        DWORD pairMB = (DWORD)(((unsigned __int64)sd.Width * sd.Height
                                                * 4 * (samples < 1 ? 1 : samples) * 2) >> 20);
                        char l[224];
                        sprintf(l, "[msaa] created %ux%u A8R8G8B8 + D24S8 at x%ld"
                                   " (~%lu MB pair) | driver texmem free %lu MB |"
                                   " process VA free %lu MB",
                                sd.Width, sd.Height, samples,
                                (unsigned long)pairMB, (unsigned long)texFreeMB,
                                (unsigned long)vaFreeMB);
                        LogLine(l);
                    } else {
                        MsaaRelease();
                        if (InterlockedIncrement(&g_msFailures) <= 4) {
                            char l[176];
                            sprintf(l, "[msaa] create FAILED colour=0x%08lX depth=0x%08lX at x%ld"
                                       " - falling back to no MSAA",
                                    (unsigned long)c1, (unsigned long)c2, samples);
                            LogLine(l);
                        }
                    }
                }
                if (g_msColour && g_msDepth) {
                    // Remember the engine surface to resolve into. It is the
                    // same one every episode; recording it each time is
                    // harmless and survives the engine swapping targets.
                    g_msResolveTo = pRT;
                    g_msActive = 1;
                    InterlockedIncrement(&g_msSubstitutions);
                    HRESULT sr = g_origSetRT(This, 0, g_msColour);
                    g_msHasContent = 1;
                    // Pair the MS depth immediately - D3D9 rejects a draw with
                    // mismatched sample counts, and the engine may not call
                    // SetDepthStencilSurface again before drawing. Capture the
                    // engine's current depth first so it can be restored; the
                    // Get adds a reference, so release it straight away and
                    // keep only the pointer (the engine owns its lifetime).
                    if (!g_msDepthBound) {
                        IDirect3DSurface9 *cur = NULL;
                        if (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(This, &cur)) && cur) {
                            g_msOrigDepth = cur;
                            IDirect3DSurface9_Release(cur);
                        } else {
                            g_msOrigDepth = NULL;
                        }
                        g_origSetDS(This, g_msDepth);
                        g_msDepthBound = 1;
                    }
                    // Clear our MS DEPTH once per FRAME, driven by a flag the
                    // frame hook sets - not by "first substitution since the
                    // last resolve", which fires many times per frame and
                    // would wipe depth mid-scene.
                    //
                    // AFTER the depth bind, deliberately - this block used to
                    // sit before it, which put the Clear at a moment when the
                    // MS colour was bound against the engine's NON-MS depth.
                    // Mismatched sample counts make the entire Clear fail
                    // INVALIDCALL (unchecked, at the time), so the MS depth
                    // was never cleared by us and - worse for diagnosis - the
                    // MsaaDebugClear magenta was never painted either: the
                    // "no magenta at 4K" verdict from that build is void. The
                    // samples=1 control never exposed this because its pairing
                    // was legal everywhere. Now the pairing is MS/MS (legal)
                    // and the result is checked.
                    if (g_msNeedDepthClear) {
                        g_msNeedDepthClear = 0;
                        DWORD flags = D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
                        if (g_msaaDebugClear) flags |= D3DCLEAR_TARGET;
                        HRESULT ch = IDirect3DDevice9_Clear(This, 0, NULL, flags,
                                               D3DCOLOR_ARGB(255, 255, 0, 255),
                                               g_lastClearZ, g_lastClearStencil);
                        if (FAILED(ch) && InterlockedIncrement(&g_msFailures) <= 8) {
                            char cl[96];
                            sprintf(cl, "[msaa] per-frame clear FAILED 0x%08lX", (unsigned long)ch);
                            LogLine(cl);
                        }
                    }
                    return sr;
                }
            }
        }

        // ---- v6: substitute the linear-depth PREPASS target ---------------
        // Same MS depth as the colour episodes, deliberately - MS_DEPTH's
        // 600k-draw prepass is what fills it with the opaque world's Z, so
        // that the colour pass (z-write off, LESSEQUAL) keeps geometry and
        // rejects the sky/sea quads exactly as it does against the engine's
        // own depth. Requires the colour pair to exist already (the depth is
        // shared); if this latch fires first, substitution starts one colour
        // episode later.
        if (samples >= 1 && pRT && g_depthRtMain && (void *)pRT == g_depthRtMain &&
            g_msDepth && g_msW) {
            D3DSURFACE_DESC dd;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(pRT, &dd)) &&
                dd.MultiSampleType == D3DMULTISAMPLE_NONE &&
                dd.Width == g_msW && dd.Height == g_msH) {
                if (!g_msR32f) {
                    D3DMULTISAMPLE_TYPE mst = (samples <= 1)
                        ? D3DMULTISAMPLE_NONE : (D3DMULTISAMPLE_TYPE)samples;
                    HRESULT cr = g_origCreateRT
                        ? g_origCreateRT(This, dd.Width, dd.Height, D3DFMT_R32F,
                                         mst, 0, FALSE, &g_msR32f, NULL)
                        : E_FAIL;
                    char l[160];
                    sprintf(l, "[msaa] R32F prepass surface %ux%u at x%ld: 0x%08lX",
                            dd.Width, dd.Height, samples, (unsigned long)cr);
                    LogLine(l);
                    if (FAILED(cr)) g_msR32f = NULL;
                }
                if (g_msR32f) {
                    g_msR32fResolveTo = pRT;
                    g_msR32fActive = 1;
                    InterlockedIncrement(&g_msR32fSubs);
                    HRESULT sr2 = g_origSetRT(This, 0, g_msR32f);
                    if (!g_msDepthBound) {
                        IDirect3DSurface9 *cur = NULL;
                        if (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(This, &cur)) && cur) {
                            g_msOrigDepth = cur;
                            IDirect3DSurface9_Release(cur);
                        } else {
                            g_msOrigDepth = NULL;
                        }
                        g_origSetDS(This, g_msDepth);
                        g_msDepthBound = 1;
                    }
                    // First substitution of the frame may be the prepass:
                    // consume the per-frame clear here so the prepass starts
                    // from far depth rather than last frame's. Depth/stencil
                    // only - the magenta debug clear belongs to the colour
                    // surface, not the linear-depth data.
                    if (g_msNeedDepthClear) {
                        g_msNeedDepthClear = 0;
                        IDirect3DDevice9_Clear(This, 0, NULL,
                                               D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                                               0, g_lastClearZ, g_lastClearStencil);
                    }
                    g_msR32fHasContent = 1;
                    return sr2;
                }
            }
        }
    }

    if (RenderTargetIndex > 0) {
        if (pRT) {
            InterlockedIncrement(&g_mrtRealBinds);
            if ((LONG)RenderTargetIndex > g_maxRealRtIndex)
                g_maxRealRtIndex = (LONG)RenderTargetIndex;
        } else {
            InterlockedIncrement(&g_mrtNullUnbinds);
        }
    }
    // Per-pass render-target inventory. GetDesc is a COM call, so it only runs
    // the FIRST time a given (pass, surface) pair is seen - after warmup this
    // is a short pointer scan on an otherwise hot path, and the whole thing is
    // behind a flag that is off unless someone asked for the inventory.
    if (g_logPassRts && pRT) {
        LONG p = g_curPass;
        if (p < 0 || p >= PASS_COUNT) p = PASS_NONE;
        if ((LONG)RenderTargetIndex > g_passMaxRtIndex[p])
            g_passMaxRtIndex[p] = (LONG)RenderTargetIndex;
        LONG n = g_passRtSeenCount, i, found = 0;
        if (n > PASS_RT_SEEN_MAX) n = PASS_RT_SEEN_MAX;
        for (i = 0; i < n; i++)
            if (g_passRtSeen[i].pass == p && g_passRtSeen[i].surf == pRT) { found = 1; break; }
        if (!found && n < PASS_RT_SEEN_MAX) {
            g_passRtSeen[n].pass = p;
            g_passRtSeen[n].surf = pRT;
            g_passRtSeenCount = n + 1;
            D3DSURFACE_DESC sd;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(pRT, &sd))) {
                char pl[192];
                sprintf(pl, "[pass] %-18s rt[%lu] %ux%u fmt=%d ms=%d",
                        g_passNames[p], (unsigned long)RenderTargetIndex,
                        sd.Width, sd.Height, (int)sd.Format, (int)sd.MultiSampleType);
                LogLine(pl);
            }
        }
    }
    // Only index 0 defines the viewport, so only index 0 governs the flag.
    if (RenderTargetIndex == 0)
        g_shadowRtActive = IsShadowSurface(pRT);
#if ENABLE_CASCADE_HUNT
    if (RenderTargetIndex == 0 && g_logShaderConsts) LogRtChange(pRT);
#endif
    return g_origSetRT(This, RenderTargetIndex, pRT);
}

// (HookedSetViewport removed - see the retirement note at its former install
// site in HookRealDevicePresent.)

#if ENABLE_CASCADE_HUNT
// ---- Shader constant probe (cascade split hunt) --------------------------
// RETIRED (ENABLE_CASCADE_HUNT). Everything from here to the end of
// HookedSetPSConstF served the search for the cascade split, and that search
// SUCCEEDED - by a different route. The runtime call-site trace below pinned
// the upload to FUN_00a84e70+0x8e, which reads the split straight out of the
// scene object; patching those fields is ApplyCascadeSplitSource, near the top
// of this file, and it needs none of this. What is left here is the scaffolding
// (the two logging toggles) plus the two rewrite paths that were tried and
// failed - MaybeRewriteSplit (right value shape, wrong lever: the shader only
// uses the scalar for fade) and MaybeWidenCascade/MaybePropagateCascade (VS c8
// turned out to be a per-object world*lightViewProj, not the cascade
// projection, so scaling it moved every caster).
//
// Worth retiring rather than leaving switched off: it puts a hook on
// SetVertexShaderConstantF/SetPixelShaderConstantF, which the generic thunk
// block above deliberately refuses to touch precisely because they run
// hundreds of thousands of times a second.
//
// `shadowSplitRange` is a shader constant, so its VALUE is uploaded every
// frame through SetVertexShaderConstantF / SetPixelShaderConstantF. Ghidra
// found where the engine binds its HANDLE (FUN_00c5c050, struct at
// *(param_1+0x90), offset +8) but not where the value is computed - and
// chasing that statically means guessing at the consumer.
//
// Watching the uploads instead answers it directly: dump each register once
// with its first float4, then look for the one carrying plausible cascade
// distances. Once the register is known, overriding it is a one-line rewrite
// in this same hook.
//
// These two calls are extremely hot, so the fast path is a single flag test.
// When enabled, each register logs exactly once (a 256-entry seen-table),
// which bounds the output to a snapshot of the constant layout rather than a
// per-frame flood.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetShaderConstF)(
    IDirect3DDevice9 *, UINT, const float *, UINT);
static PFN_SetShaderConstF g_origSetVSConstF = NULL;
static PFN_SetShaderConstF g_origSetPSConstF = NULL;
static unsigned char g_vsRegSeen[256];
static unsigned char g_psRegSeen[256];

// ---- Cascade split override ----------------------------------------------
// The dump found it: `shadowSplitRange` uploads as (near, far, 0, 0) and read
// **10.0, 79.2** - so the near cascade ends at 10 units and everything from
// there to 79.2 is covered by the second, far coarser cascade. That 10 IS the
// user's "shadows degrade sharply a few meters out".
//
// It arrives on several registers (c36, c40-c43, c46), because each shader
// wants it in its own slot, so keying on a register number would miss most of
// them. Matching the VALUE SHAPE instead catches every upload wherever it
// lands: exactly (x, y, 0, 0) with a positive near, a far at least twice the
// near, and both inside a sane world-distance range. Deliberately narrow -
// a matrix row or a colour will not satisfy all four conditions at once.
//
// The far value is likely per-area, so it is scaled by a percentage rather
// than replaced with an absolute, and the near split is pushed out by its own
// factor since that is the boundary actually being complained about.
static volatile LONG g_splitRewrites = 0;
static volatile LONG g_splitLogged = 0;

// ---- Split-range call-site trace -----------------------------------------
// Eight decompilation rounds could not reach the cascade fit statically: the
// render pass only consumes, the settings path only sizes surfaces, and the
// constant upload reaches its handle through a pointer chain that offset
// scanning cannot follow.
//
// So ask the running game instead. `shadowSplitRange` uploads as
// (10.0, 79.2) through SetPixelShaderConstantF, which is already hooked -
// walking the EBP chain at that moment names the exact game function doing
// the upload, and the code that COMPUTED the value is that function or its
// caller. Same trick the stutter watchdog uses, pointed at a different event.
//
// Capped hard: this fires on a hot path and only a handful of samples are
// needed to identify a call site.
static volatile LONG g_splitStackLogged = 0;

static int LooksLikeSplitRange(const float *v, UINT count)
{
    if (!v || count != 1) return 0;
    if (!(v[2] == 0.0f && v[3] == 0.0f)) return 0;
    return (v[0] > 0.5f && v[1] > v[0] * 2.0f && v[1] < 4000.0f);
}

static void LogSplitCallSite(const char *which, UINT reg, const float *v, DWORD ra)
{
    if (InterlockedIncrement(&g_splitStackLogged) > 6) return;
    // Attempt 3 at this. EBP walking failed (frame-pointer omission), and a
    // raw stack scan returned only generic vector getters with a dozen
    // unrelated callers - stale residue from hot utility code, not the live
    // chain. Both were indirect guesses at something we can just ask for:
    // our hook sits ON the device vtable, so _ReturnAddress() IS the game's
    // call site, no walking or scanning required.
    {
        // ra is captured by the HOOK, not here. Taking _ReturnAddress() inside
        // this helper returned VERSION.dll+5059 - the helper's own caller,
        // i.e. our hook - which is true but useless. The game's call site is
        // one frame further out, so the hook has to sample it and pass it in.
        char raStr[64];
        if (g_mainModBase && ra > g_mainModBase && ra < g_mainModBase + g_mainModSize)
            sprintf(raStr, "%08X (ghidra)", ra - g_mainModBase + 0x00400000);
        else
            sprintf(raStr, "%08X (outside game module)", ra);
        char rl[160];
        sprintf(rl, "[splitstack] DIRECT CALLER of the upload: %s", raStr);
        LogLine(rl);
    }
    // First attempt walked EBP and got nothing but our own DLL and d3d9 -
    // this game is built with frame-pointer omission, so there is no valid
    // EBP chain to follow. The stutter watchdog hit the same wall and solves
    // it by SCANNING raw stack memory for values that land inside the game
    // module; those are the return addresses FPO hid. Same approach here.
    DWORD espVal = 0;
    __asm { mov espVal, esp }
    char buf[900];
    int so = 0;
    buf[0] = 0;
    __try {
        DWORD *sp = (DWORD *)espVal;
        for (int i = 0; i < 400 && so < 760; i++) {
            DWORD val = sp[i];
            if (g_mainModBase && val > g_mainModBase &&
                val < g_mainModBase + g_mainModSize) {
                // report as a Ghidra VA so it can be looked up directly
                so += sprintf(buf + so, " %08X", val - g_mainModBase + 0x00400000);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    char line[1100];
    sprintf(line, "[splitstack] %s c%u (%.2f, %.2f) stack(ghidra VAs):%s",
            which, reg, v[0], v[1], buf);
    LogLine(line);
}

static int MaybeRewriteSplit(const char *which, UINT reg, const float *v, UINT count, float *out)
{
    if ((!g_shadowSplitNearPct && !g_shadowSplitFarPct) || !v || count != 1) return 0;
    float nearD = v[0], farD = v[1];
    if (!(v[2] == 0.0f && v[3] == 0.0f)) return 0;
    if (!(nearD > 0.5f && farD > nearD * 2.0f && farD < 4000.0f)) return 0;
    out[0] = g_shadowSplitNearPct ? nearD * (g_shadowSplitNearPct / 100.0f) : nearD;
    out[1] = g_shadowSplitFarPct  ? farD  * (g_shadowSplitFarPct  / 100.0f) : farD;
    out[2] = v[2]; out[3] = v[3];
    InterlockedIncrement(&g_splitRewrites);
    if (InterlockedIncrement(&g_splitLogged) <= 12) {
        char l[192];
        sprintf(l, "[split] %s c%u : (%.2f, %.2f) -> (%.2f, %.2f)",
                which, reg, nearD, farD, out[0], out[1]);
        LogLine(l);
    }
    return 1;
}

// Third attempt, and deliberately the DUMBEST one that can work: no surface
// table, no SetRenderTarget gating, no restructuring of CreateTexture - all
// three of which were changed at once last time and produced a startup crash
// nobody could attribute.
//
// This is pure logging. A bounded burst of EVERY constant upload in call
// order, plus a marker whenever the bound render target pointer changes
// (pointer compare only - no COM calls, nothing dereferenced). That gives an
// interleaved timeline:
//
//   [seq] RT -> 0x1234ABCD
//   [seq] VS c8 count=4 : ...
//
// The shadow pass is then identifiable offline by structure rather than by
// asking the device anything: it is the render target that gets bound twice
// per frame (two cascades) with a 4-register matrix upload each time. Once
// its pointer is known, the matrices under it are the cascade projections.
//
// Hard-capped so it cannot run away, and the giant count=128 array uploads
// are skipped since they are bone/skinning palettes, never shadow setup.
#define CONST_DUMP_MAX 4000

static void LogShaderConst(const char *which, UINT start, const float *v, UINT count,
                           unsigned char *seen)
{
    (void)seen;
    if (!v || count == 0) return;
    if (count > 8) return;                       // skinning palettes, not shadow data
    LONG n = InterlockedIncrement(&g_constSeq);
    if (n > CONST_DUMP_MAX) return;
    char l[240];
    sprintf(l, "[seq %ld] %s c%u count=%u : %.4f %.4f %.4f %.4f",
            n, which, start, count, v[0], v[1], v[2], v[3]);
    LogLine(l);
}

// Marker only. Compares the incoming pointer against the last one seen and
// logs the transition - never dereferences it, never calls into D3D.
static void LogRtChange(void *pRT)
{
    static void *last = NULL;
    if (!g_logShaderConsts || pRT == last) return;
    last = pRT;
    LONG n = InterlockedIncrement(&g_constSeq);
    if (n > CONST_DUMP_MAX) return;
    char l[96];
    sprintf(l, "[seq %ld] RT -> 0x%p", n, pRT);
    LogLine(l);
}

// ---- Near-cascade widening -----------------------------------------------
// The gameplay constant capture found the cascade projections: `VS c8`, a
// 4-register (4x4 matrix) upload, once per cascade render target.
//   near cascade: row0 = (-0.1282, 0.0595, ...) -> |scale| 0.1414 -> ~7.1 units
//   far  cascade: row0 = (-0.0167, 0.0078, ...) -> |scale| 0.0184 -> ~54 units
// The ~7.7x ratio matches the (10.0, 79.2) `shadowSplitRange` pair almost
// exactly, which is why overwriting that scalar did nothing: the shader picks
// a cascade by projecting through THESE matrices and bounds-testing. The
// matrix is the control; the scalar only describes it.
//
// Widening = scaling the projection DOWN. D3D9 uses row-vector convention
// (p * M), so the X extent lives in COLUMN 0 (elements 0,4,8,12) and Y in
// COLUMN 1 (1,5,9,13) - scaling whole rows would skew the matrix instead.
//
// Discriminator: `VS c8` is not shadow-exclusive (an earlier capture caught
// `VS c8 count=4 : 249.9999 ...` from some other pass), so this only fires
// when the row-0 scale magnitude sits in the near-cascade band. The far
// cascade (0.0184) is below it and ordinary transforms are far above, so
// both are excluded. The band is deliberately wide because the user reports
// cascade distances differing by location - the engine computes these
// per-scene, and a multiplier preserves that instead of pinning it.
static volatile LONG g_cascadeRewrites = 0;
static volatile LONG g_cascadeLogged = 0;
#define CASCADE_BAND_LO 0.04f
#define CASCADE_BAND_HI 0.80f

// The cascade matrix as the engine originally supplied it, captured during
// the shadow pass. Used to recognise the SAME matrix when it reappears in the
// lighting pass so both sides get scaled identically.
//
// Matching against a specific recorded matrix - not a value range - is the
// whole point. Attempt 4 failed because a magnitude band caught every prop's
// world matrix; sixteen floats agreeing to within an epsilon cannot collide
// by accident.
static float g_cascadeOrig[16];
static volatile LONG g_cascadeOrigValid = 0;
static volatile LONG g_propagations = 0;
static volatile LONG g_propLogged = 0;

static int MatricesMatch(const float *a, const float *b)
{
    for (int i = 0; i < 16; i++) {
        float d = a[i] - b[i];
        if (d < 0) d = -d;
        float scale = (a[i] < 0 ? -a[i] : a[i]);
        if (scale < 1.0f) scale = 1.0f;
        if (d > 1e-5f * scale) return 0;
    }
    return 1;
}

// Runs for uploads OUTSIDE the shadow pass. If the payload is the cascade
// matrix we just scaled, scale it the same way so the lookup agrees with how
// the depth was written.
static int MaybePropagateCascade(const float *v, UINT count, float *out)
{
    if (g_nearCascadePct <= 0 || g_nearCascadePct == 100) return 0;
    if (!g_cascadeOrigValid || count != 4 || !v) return 0;
    if (g_shadowRtActive) return 0;             // render side, handled elsewhere
    if (!MatricesMatch(v, g_cascadeOrig)) return 0;
    for (int i = 0; i < 16; i++) out[i] = v[i];
    float k = 100.0f / (float)g_nearCascadePct;
    for (int r = 0; r < 4; r++) { out[r*4 + 0] *= k; out[r*4 + 1] *= k; }
    InterlockedIncrement(&g_propagations);
    if (InterlockedIncrement(&g_propLogged) <= 8) {
        char l[192];
        sprintf(l, "[cascade] propagated to sampling side: row0 %.4f %.4f -> %.4f %.4f",
                v[0], v[1], out[0], out[1]);
        LogLine(l);
    }
    return 1;
}

static int MaybeWidenCascade(UINT reg, const float *v, UINT count, float *out)
{
    if (g_nearCascadePct <= 0 || g_nearCascadePct == 100) return 0;
    if (reg != 8 || count != 4 || !v) return 0;
    // STEP 2: gate on RENDER-TARGET CONTEXT, not on the values.
    //
    // The magnitude band this used to rely on was hopeless: `VS c8` is the
    // general world/view transform, so "scale between 0.04 and 0.80" matched
    // the world matrix of every small prop as well as the cascade projection.
    // That is what made every model in the game grow and shrink.
    //
    // A shadow map being the bound render target is unambiguous - ordinary
    // geometry is never drawn into one - and step 1 established that signal
    // safely (pointer values, no references held, table cleared on Reset).
    if (!g_shadowRtActive) return 0;
    float mag = (float)sqrt((double)(v[0]*v[0] + v[1]*v[1]));
    // Kept only as a sanity floor so a degenerate matrix cannot be scaled into
    // nonsense; the render-target check above is what actually discriminates.
    if (mag <= 0.0f || mag > CASCADE_BAND_HI) return 0;
    for (int i = 0; i < 16; i++) out[i] = v[i];
    float k = 100.0f / (float)g_nearCascadePct;   // >100% => k<1 => wider extent
    for (int r = 0; r < 4; r++) { out[r*4 + 0] *= k; out[r*4 + 1] *= k; }
    // Remember the ORIGINAL so the sampling side can be matched exactly. The
    // lighting pass looks this same matrix up to project world positions into
    // the shadow map; if only the render side is scaled the two disagree and
    // shadows drift with the camera - which is exactly what step 2 produced.
    for (int i = 0; i < 16; i++) g_cascadeOrig[i] = v[i];
    g_cascadeOrigValid = 1;
    InterlockedIncrement(&g_cascadeRewrites);
    if (InterlockedIncrement(&g_cascadeLogged) <= 8) {
        char l[224];
        sprintf(l, "[cascade] c8 |scale|=%.4f (~%.1f units) x%ld -> row0 %.4f %.4f (was %.4f %.4f)",
                mag, mag > 0.0f ? 1.0f/mag : 0.0f, g_nearCascadePct,
                out[0], out[1], v[0], v[1]);
        LogLine(l);
    }
    return 1;
}

static HRESULT STDMETHODCALLTYPE HookedSetVSConstF(
    IDirect3DDevice9 *This, UINT StartRegister, const float *pData, UINT Vector4fCount)
{
    float wide[16];
    if (MaybeWidenCascade(StartRegister, pData, Vector4fCount, wide))
        return g_origSetVSConstF(This, StartRegister, wide, Vector4fCount);
    if (MaybePropagateCascade(pData, Vector4fCount, wide))
        return g_origSetVSConstF(This, StartRegister, wide, Vector4fCount);
    if (g_logShaderConsts) LogShaderConst("VS", StartRegister, pData, Vector4fCount, g_vsRegSeen);
    if (g_traceSplitCall && LooksLikeSplitRange(pData, Vector4fCount))
        LogSplitCallSite("VS", StartRegister, pData,
                         (DWORD)(UINT_PTR)_ReturnAddress());
    float patched[4];
    if (MaybeRewriteSplit("VS", StartRegister, pData, Vector4fCount, patched))
        return g_origSetVSConstF(This, StartRegister, patched, Vector4fCount);
    return g_origSetVSConstF(This, StartRegister, pData, Vector4fCount);
}

static HRESULT STDMETHODCALLTYPE HookedSetPSConstF(
    IDirect3DDevice9 *This, UINT StartRegister, const float *pData, UINT Vector4fCount)
{
    if (g_logShaderConsts) LogShaderConst("PS", StartRegister, pData, Vector4fCount, g_psRegSeen);
    if (g_traceSplitCall && LooksLikeSplitRange(pData, Vector4fCount))
        LogSplitCallSite("PS", StartRegister, pData,
                         (DWORD)(UINT_PTR)_ReturnAddress());
    float patched[4];
    if (MaybeRewriteSplit("PS", StartRegister, pData, Vector4fCount, patched))
        return g_origSetPSConstF(This, StartRegister, patched, Vector4fCount);
    float pwide[16];
    if (MaybePropagateCascade(pData, Vector4fCount, pwide))
        return g_origSetPSConstF(This, StartRegister, pwide, Vector4fCount);
    return g_origSetPSConstF(This, StartRegister, pData, Vector4fCount);
}
#endif  // ENABLE_CASCADE_HUNT

static HRESULT STDMETHODCALLTYPE HookedDeviceReset(
    IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pPP)
{
    // MSAA surfaces are D3DPOOL_DEFAULT and MUST be released before Reset -
    // holding one blocks the Reset outright, which is exactly what crashed the
    // shadow-surface tracking attempt (FEATURES.md, "Attempt 5 step 1"). They
    // are rebuilt lazily on the next substitution.
    MsaaRelease();
    // Same rule as the MSAA surfaces: D3DPOOL_DEFAULT, so holding it across a
    // Reset would block the Reset outright.
    SsaaReleaseIntermediate();
    if (pPP) {
        // ---- SSAA: pin the PRESENTATION size ----------------------------
        // The field ApplySsaaScale writes is the game's RESOLUTION, and the
        // engine resizes its swap chain (and window) to match. So raising it
        // normally just renders bigger and presents bigger - no supersampling
        // at all. It only produced real SSAA at fullscreen desktop
        // resolution, where the presentation happened to be CLAMPED by the
        // display and could not follow, which is exactly the "works at native
        // 4K, gives raw 4K at 1080p" behaviour observed in testing.
        //
        // Pinning the backbuffer to the pre-scale size reproduces that clamp
        // deliberately at any resolution: the engine still allocates its scene
        // targets from the scaled settings field, the swap chain stays at the
        // size the player actually chose, and the engine's own
        // scene->backbuffer copy becomes the downsample. Nothing here changes
        // when SsaaScale is 100 - g_ssaaActive is 0 and the params pass
        // through untouched.
        // The condition is deliberately narrow: pin ONLY when the swap chain
        // is being resized to the exact value we wrote. A Reset to any other
        // size is the player changing resolution (or the display clamping,
        // which is what already works at fullscreen native) and must pass
        // through untouched - ApplySsaaScale then re-bases off it. Pinning on
        // "size != base" instead would fight every genuine resolution change,
        // and would misfire in the window between the menu writing a new
        // resolution and the next monitor tick re-basing.
        // Retired (ENABLE_SSAA_PRESENT_PIN 0): measured pins=0 across a full
        // run - it tested for our exact target while the engine kept arriving
        // at decoration-adjusted client sizes - and in the one configuration
        // where it did engage it produced a 1080p swap chain inside a 4K
        // window: unfiltered pixel-doubling plus a 1:1 blit into the top-left
        // quarter with a menu open. The display's own clamp at fullscreen does
        // this job correctly and needs no help.
#if ENABLE_SSAA_PRESENT_PIN
        if (g_ssaaActive && g_backbufW > 0 && g_backbufH > 0 &&
            g_ssaaCurW > 0 && g_ssaaCurH > 0 &&
            pPP->BackBufferWidth == (UINT)g_ssaaCurW &&
            pPP->BackBufferHeight == (UINT)g_ssaaCurH &&
            ((LONG)g_backbufW != g_ssaaCurW || (LONG)g_backbufH != g_ssaaCurH)) {
            char pl[192];
            sprintf(pl, "[ssaa] Reset: pinning backbuffer %ux%u -> %ux%u"
                        " (presentation held while the scene renders scaled)",
                    pPP->BackBufferWidth, pPP->BackBufferHeight,
                    g_backbufW, g_backbufH);
            LogLine(pl);
            pPP->BackBufferWidth = g_backbufW;
            pPP->BackBufferHeight = g_backbufH;
            InterlockedIncrement(&g_ssaaPins);
        }
#endif  // ENABLE_SSAA_PRESENT_PIN
        if (pPP->hDeviceWindow) g_gameHwnd = pPP->hDeviceWindow;
        // Remember the presentation size so half-res render targets can be
        // recognised by ratio rather than by hardcoded numbers.
        g_backbufW = pPP->BackBufferWidth;
        g_backbufH = pPP->BackBufferHeight;
        char l[224];
        sprintf(l, "[rt] Reset: %ux%u backbufFmt=%d MULTISAMPLE=%d qual=%lu autoDepth=%d depthFmt=%d windowed=%d interval=0x%lX",
                pPP->BackBufferWidth, pPP->BackBufferHeight, (int)pPP->BackBufferFormat,
                (int)pPP->MultiSampleType, (unsigned long)pPP->MultiSampleQuality,
                (int)pPP->EnableAutoDepthStencil, (int)pPP->AutoDepthStencilFormat,
                (int)pPP->Windowed, (unsigned long)pPP->PresentationInterval);
        LogLine(l);
    }
    ClearShadowSurfaces();   // recorded addresses are meaningless across a Reset
    return g_origReset(This, pPP);
}

// Vtables already patched by HookRealDevicePresent. This used to be a single
// global one-shot flag, which was wrong whenever the process creates MORE THAN
// ONE device: the first call consumed the flag and every later device went
// unhooked. That is not hypothetical - with ForceStdD3D9 and the HD GUI mod
// both in play the log shows two CreateDevice calls, and the game uses the
// second one. The result was that every device-level feature silently died
// (MSAA substitution, FXAA passthrough, the SSAA output-res reroute, and the
// Reset hook - hence backbuf=0x0 and no [rt] Reset lines) while the resource
// hooks kept working, which made it look like an SSAA bug.
//
// Per-VTABLE rather than per-device on purpose: two devices of the same class
// share one vtable, and patching it twice would make ResolveOrigSlot resolve
// our own hook as the "original" - an infinite recursion. Keyed on the vtable,
// a shared vtable is patched exactly once and a distinct one still gets its
// own patch.
#define HOOKED_VT_MAX 8
static void *g_hookedDevVtables[HOOKED_VT_MAX];
static volatile LONG g_hookedDevVtableCount = 0;

static int DevVtableAlreadyHooked(void **vtbl)
{
    LONG n = g_hookedDevVtableCount;
    if (n > HOOKED_VT_MAX) n = HOOKED_VT_MAX;
    for (LONG i = 0; i < n; i++)
        if (g_hookedDevVtables[i] == (void *)vtbl) return 1;
    if (g_hookedDevVtableCount < HOOKED_VT_MAX) {
        g_hookedDevVtables[g_hookedDevVtableCount] = (void *)vtbl;
        InterlockedIncrement(&g_hookedDevVtableCount);
    }
    return 0;
}

// REVERTED 2026-08-13. The per-vtable version below was built and shipped and
// the game crashed on startup with it. The reasoning (two CreateDevice calls,
// one-shot consumed by the first) is still supported by the log, but the
// conclusion "so patch both vtables" was wrong or incomplete: widening what
// gets patched is exactly what broke it, and under the HD GUI mod the second
// vtable is the mod's own proxy class. Patching a wrapper's vtable with
// functions that assume a native device is not a safe substitution.
//
// Back to the original single-shot until the two devices are actually
// identified (which is which, in what order, and which one the game draws
// with) instead of inferred from a duplicated log line.
#define ENABLE_PER_VTABLE_DEVICE_HOOKS 0

static void HookRealDevicePresent(IDirect3DDevice9 *dev)
{
    if (!dev) return;
#if ENABLE_PER_VTABLE_DEVICE_HOOKS
    {
        void **vtProbe = *(void ***)dev;
        if (DevVtableAlreadyHooked(vtProbe)) return;
        InterlockedExchange(&g_devicePresentHooked, 1);
    }
#else
    if (InterlockedCompareExchange(&g_devicePresentHooked, 1, 0) != 0) return;
#endif
    void **vtbl = *(void ***)dev;
    DWORD oldProtect;
    char line[160];

    // Resource-vtable hooks (Texture/Surface/CubeTexture LockRect - the
    // staging fix, the part of this mod that actually does work) normally
    // come from ProbeD3D9ForVtable's throwaway device. Under a third-party
    // d3d9.dll that device is never created at all (see the comment there),
    // so this is the only source. HookResourceVtables is idempotent, so
    // calling it unconditionally here is a harmless no-op on native (where
    // the probe typically wins the race) and the sole install path on DXVK.
    // The device just returned to the caller from CreateDevice/CreateDeviceEx
    // is fully constructed at this point - calling CreateTexture on it here
    // is no different from the game doing the same a moment later.
    HookResourceVtables(dev);

    int slot = offsetof(IDirect3DDevice9Vtbl, Present) / sizeof(void *);
    g_origDevicePresent = (PFN_DevicePresent)ResolveOrigSlot(vtbl[slot]);
    if (VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slot] = (void *)HookedDevicePresent;
        VirtualProtect(&vtbl[slot], sizeof(void *), oldProtect, &oldProtect);
    }

    // PresentEx lives at slot 132, past the 119 entries an IDirect3DDevice9
    // vtable is required to have. Patching it unconditionally was safe only
    // as long as every device here was really an Ex device - which stopped
    // being true when ForceStdD3D9 shipped, since that deliberately drives
    // the game down the plain-CreateDevice path. On a genuinely non-Ex vtable
    // this writes ~52 bytes past its end, into whatever the runtime happens
    // to have placed next in .rdata. Ask the object instead of assuming.
    static const GUID kIID_IDirect3DDevice9Ex =
        { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };
    IDirect3DDevice9Ex *devEx = NULL;
    if (SUCCEEDED(IDirect3DDevice9_QueryInterface(dev, &kIID_IDirect3DDevice9Ex, (void **)&devEx)) && devEx) {
        IDirect3DDevice9Ex_Release(devEx);
        int slotEx = offsetof(IDirect3DDevice9ExVtbl, PresentEx) / sizeof(void *);
        g_origDevicePresentEx = (PFN_DevicePresentEx)ResolveOrigSlot(vtbl[slotEx]);
        if (VirtualProtect(&vtbl[slotEx], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
            vtbl[slotEx] = (void *)HookedDevicePresentEx;
            VirtualProtect(&vtbl[slotEx], sizeof(void *), oldProtect, &oldProtect);
        }
    } else {
        g_origDevicePresentEx = NULL;
        LogLine("[d3d9] device is not IDirect3DDevice9Ex - PresentEx hook skipped "
                "(would have written past the end of the vtable)");
    }

    // Same technique as Present/PresentEx above (real replacement function on
    // the REAL device's vtable slot) - these three have been confirmed safe
    // by that precedent, unlike the swap-chain Present hook retired below,
    // which crashed for a still-undiagnosed reason specific to that object.
    int slotUS = offsetof(IDirect3DDevice9Vtbl, UpdateSurface) / sizeof(void *);
    g_origUpdateSurface = (PFN_UpdateSurface)ResolveOrigSlot(vtbl[slotUS]);
    if (VirtualProtect(&vtbl[slotUS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotUS] = (void *)HookedUpdateSurface;
        VirtualProtect(&vtbl[slotUS], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotUT = offsetof(IDirect3DDevice9Vtbl, UpdateTexture) / sizeof(void *);
    g_origUpdateTexture = (PFN_UpdateTexture)ResolveOrigSlot(vtbl[slotUT]);
    if (VirtualProtect(&vtbl[slotUT], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotUT] = (void *)HookedUpdateTexture;
        VirtualProtect(&vtbl[slotUT], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotVS = offsetof(IDirect3DDevice9Vtbl, CreateVertexShader) / sizeof(void *);
    g_origCreateVS = (PFN_CreateVertexShader)ResolveOrigSlot(vtbl[slotVS]);
    if (VirtualProtect(&vtbl[slotVS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotVS] = (void *)HookedCreateVertexShader;
        VirtualProtect(&vtbl[slotVS], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotPS = offsetof(IDirect3DDevice9Vtbl, CreatePixelShader) / sizeof(void *);
    g_origCreatePS = (PFN_CreatePixelShader)ResolveOrigSlot(vtbl[slotPS]);
    if (VirtualProtect(&vtbl[slotPS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotPS] = (void *)HookedCreatePixelShader;
        VirtualProtect(&vtbl[slotPS], sizeof(void *), oldProtect, &oldProtect);
    }
    // StretchRect - the present path probe (see HookedStretchRect). Also the
    // method our own MSAA resolve goes through, hence the "(our MSAA resolve)"
    // tag so the two are never confused in the log.
    int slotSR = offsetof(IDirect3DDevice9Vtbl, StretchRect) / sizeof(void *);
    g_origStretchRect = (PFN_StretchRect)ResolveOrigSlot(vtbl[slotSR]);
    if (VirtualProtect(&vtbl[slotSR], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotSR] = (void *)HookedStretchRect;
        VirtualProtect(&vtbl[slotSR], sizeof(void *), oldProtect, &oldProtect);
    }
    // Real swap-chain geometry: if the backbuffer is smaller than the window
    // client area, the runtime scales at Present and that alone would soften
    // every edge - a possibility no shader hunt could ever have surfaced.
    {
        IDirect3DSwapChain9 *sc = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) && sc) {
            D3DPRESENT_PARAMETERS pp;
            if (SUCCEEDED(IDirect3DSwapChain9_GetPresentParameters(sc, &pp))) {
                HWND hw = pp.hDeviceWindow;
                RECT rc; rc.left = rc.top = rc.right = rc.bottom = 0;
                if (hw) GetClientRect(hw, &rc);
                char sl[256];
                sprintf(sl, "[swap] backbuffer %ux%u fmt=%d count=%u windowed=%d"
                            " | window client %ldx%ld%s",
                        pp.BackBufferWidth, pp.BackBufferHeight, (int)pp.BackBufferFormat,
                        pp.BackBufferCount, (int)pp.Windowed,
                        rc.right - rc.left, rc.bottom - rc.top,
                        (rc.right - rc.left && (LONG)pp.BackBufferWidth != rc.right - rc.left)
                            ? "  *** MISMATCH: runtime scales at Present ***" : "");
                LogLine(sl);
            }
            IDirect3DSwapChain9_Release(sc);
        }
    }
    // FXAA bind-time substitution (see HookedSetPixelShader).
    int slotSPS = offsetof(IDirect3DDevice9Vtbl, SetPixelShader) / sizeof(void *);
    g_origSetPixelShader = (PFN_SetPixelShader)ResolveOrigSlot(vtbl[slotSPS]);
    if (VirtualProtect(&vtbl[slotSPS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotSPS] = (void *)HookedSetPixelShader;
        VirtualProtect(&vtbl[slotSPS], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotVB = offsetof(IDirect3DDevice9Vtbl, CreateVertexBuffer) / sizeof(void *);
    g_origCreateVB = (PFN_CreateVB)ResolveOrigSlot(vtbl[slotVB]);
    if (VirtualProtect(&vtbl[slotVB], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotVB] = (void *)HookedCreateVB;
        VirtualProtect(&vtbl[slotVB], sizeof(void *), oldProtect, &oldProtect);
    }
    int slotIB = offsetof(IDirect3DDevice9Vtbl, CreateIndexBuffer) / sizeof(void *);
    g_origCreateIB = (PFN_CreateIB)ResolveOrigSlot(vtbl[slotIB]);
    if (VirtualProtect(&vtbl[slotIB], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotIB] = (void *)HookedCreateIB;
        VirtualProtect(&vtbl[slotIB], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotCT = offsetof(IDirect3DDevice9Vtbl, CreateTexture) / sizeof(void *);
    g_origCreateTexture = (PFN_CreateTexture)ResolveOrigSlot(vtbl[slotCT]);
    if (VirtualProtect(&vtbl[slotCT], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotCT] = (void *)HookedCreateTexture;
        VirtualProtect(&vtbl[slotCT], sizeof(void *), oldProtect, &oldProtect);
    }

    // Render-target inventory probe - see the comment block above these
    // three functions. Note these go on the REAL device; the generic
    // HOOK_DEV thunks for the same slots sit on the throwaway probe device's
    // vtable, which has been confirmed dead since early in this project.
    int slotRT = offsetof(IDirect3DDevice9Vtbl, CreateRenderTarget) / sizeof(void *);
    g_origCreateRT = (PFN_CreateRenderTarget)ResolveOrigSlot(vtbl[slotRT]);
    if (VirtualProtect(&vtbl[slotRT], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotRT] = (void *)HookedCreateRenderTarget;
        VirtualProtect(&vtbl[slotRT], sizeof(void *), oldProtect, &oldProtect);
    }
    int slotDS = offsetof(IDirect3DDevice9Vtbl, CreateDepthStencilSurface) / sizeof(void *);
    g_origCreateDS = (PFN_CreateDepthStencil)ResolveOrigSlot(vtbl[slotDS]);
    if (VirtualProtect(&vtbl[slotDS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotDS] = (void *)HookedCreateDepthStencil;
        VirtualProtect(&vtbl[slotDS], sizeof(void *), oldProtect, &oldProtect);
    }
    // Clear - captures the engine's depth-clear value so our MS depth is
    // cleared to the same convention (standard vs reversed-Z).
    int slotClr = offsetof(IDirect3DDevice9Vtbl, Clear) / sizeof(void *);
    g_origClear = (PFN_Clear)ResolveOrigSlot(vtbl[slotClr]);
    if (VirtualProtect(&vtbl[slotClr], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotClr] = (void *)HookedClear;
        VirtualProtect(&vtbl[slotClr], sizeof(void *), oldProtect, &oldProtect);
    }

    // Draw-call accounting, for the MSAA diagnosis. Hot path, but the body is
    // a compare plus one or two increments.
    int slotDIP = offsetof(IDirect3DDevice9Vtbl, DrawIndexedPrimitive) / sizeof(void *);
    g_origDrawIndexed = (PFN_DrawIndexedPrimitive)ResolveOrigSlot(vtbl[slotDIP]);
    if (VirtualProtect(&vtbl[slotDIP], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotDIP] = (void *)HookedDrawIndexedPrimitive;
        VirtualProtect(&vtbl[slotDIP], sizeof(void *), oldProtect, &oldProtect);
    }
    int slotDP = offsetof(IDirect3DDevice9Vtbl, DrawPrimitive) / sizeof(void *);
    g_origDraw = (PFN_DrawPrimitive)ResolveOrigSlot(vtbl[slotDP]);
    if (VirtualProtect(&vtbl[slotDP], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotDP] = (void *)HookedDrawPrimitive;
        VirtualProtect(&vtbl[slotDP], sizeof(void *), oldProtect, &oldProtect);
    }

    // Depth-stencil binding probe - prerequisite for MSAA (the MS depth
    // surface must match the MS colour target's sample count).
    int slotSDS = offsetof(IDirect3DDevice9Vtbl, SetDepthStencilSurface) / sizeof(void *);
    g_origSetDS = (PFN_SetDepthStencilSurface)ResolveOrigSlot(vtbl[slotSDS]);
    if (VirtualProtect(&vtbl[slotSDS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotSDS] = (void *)HookedSetDepthStencilSurface;
        VirtualProtect(&vtbl[slotSDS], sizeof(void *), oldProtect, &oldProtect);
    }
    int slotSRT = offsetof(IDirect3DDevice9Vtbl, SetRenderTarget) / sizeof(void *);
    g_origSetRT = (PFN_SetRenderTarget)ResolveOrigSlot(vtbl[slotSRT]);
    if (VirtualProtect(&vtbl[slotSRT], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotSRT] = (void *)HookedSetRenderTarget;
        VirtualProtect(&vtbl[slotSRT], sizeof(void *), oldProtect, &oldProtect);
    }
    // Substitution transparency (MSAA v3): the engine must never see our
    // surfaces through the Get methods, or it captures and replays them
    // against non-MS partners. Same install pattern as SetRenderTarget above.
    int slotGRT = offsetof(IDirect3DDevice9Vtbl, GetRenderTarget) / sizeof(void *);
    g_origGetRenderTarget = (PFN_GetRenderTarget)ResolveOrigSlot(vtbl[slotGRT]);
    if (VirtualProtect(&vtbl[slotGRT], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotGRT] = (void *)HookedGetRenderTarget;
        VirtualProtect(&vtbl[slotGRT], sizeof(void *), oldProtect, &oldProtect);
    }
    int slotGDS = offsetof(IDirect3DDevice9Vtbl, GetDepthStencilSurface) / sizeof(void *);
    g_origGetDepthStencil = (PFN_GetDepthStencilSurface)ResolveOrigSlot(vtbl[slotGDS]);
    if (VirtualProtect(&vtbl[slotGDS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotGDS] = (void *)HookedGetDepthStencilSurface;
        VirtualProtect(&vtbl[slotGDS], sizeof(void *), oldProtect, &oldProtect);
    }
    // Multisample render states (MSAA v3 theories 2/3). Hot path, but the body
    // is two compares for every state other than the two multisample ones.
    int slotSRS = offsetof(IDirect3DDevice9Vtbl, SetRenderState) / sizeof(void *);
    g_origSetRenderState = (PFN_SetRenderState)ResolveOrigSlot(vtbl[slotSRS]);
    if (VirtualProtect(&vtbl[slotSRS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotSRS] = (void *)HookedSetRenderState;
        VirtualProtect(&vtbl[slotSRS], sizeof(void *), oldProtect, &oldProtect);
    }
    // SetViewport, log-only. Same install pattern as SetRenderTarget above.
    // The retired version that crashed also SCALED the viewport; this one only
    // observes, so if a crash reappears the installation itself is the cause
    // rather than the compensation logic - which the original never separated.
    int slotVP = offsetof(IDirect3DDevice9Vtbl, SetViewport) / sizeof(void *);
    g_origSetViewport = (PFN_SetViewport)ResolveOrigSlot(vtbl[slotVP]);
    if (VirtualProtect(&vtbl[slotVP], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotVP] = (void *)HookedSetViewport;
        VirtualProtect(&vtbl[slotVP], sizeof(void *), oldProtect, &oldProtect);
    }
    // RETIRED: a SetViewport hook was installed here to compensate the
    // enlarged shadow maps. It CRASHED the game at startup, and the same run
    // reported `viewports_scaled=0` - so it never once did the job it was
    // added for while still being fatal. Removed outright rather than left
    // behind a flag, matching how the NOOVERWRITE experiment was handled: a
    // confirmed crash does not get to stay as a loaded gun.
    //
    // The zero count is itself the useful finding: the engine does NOT call
    // SetViewport while a scaled shadow surface is bound at index 0, so the
    // "square that tracks the camera" is not a viewport problem at all and
    // this whole approach was aimed at the wrong mechanism.
#if ENABLE_CASCADE_HUNT
    // Two hooks on the hottest methods in the API, kept only while the cascade
    // hunt needed them. See the retirement note at the probe itself.
    int slotVSC = offsetof(IDirect3DDevice9Vtbl, SetVertexShaderConstantF) / sizeof(void *);
    g_origSetVSConstF = (PFN_SetShaderConstF)ResolveOrigSlot(vtbl[slotVSC]);
    if (VirtualProtect(&vtbl[slotVSC], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotVSC] = (void *)HookedSetVSConstF;
        VirtualProtect(&vtbl[slotVSC], sizeof(void *), oldProtect, &oldProtect);
    }
    int slotPSC = offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantF) / sizeof(void *);
    g_origSetPSConstF = (PFN_SetShaderConstF)ResolveOrigSlot(vtbl[slotPSC]);
    if (VirtualProtect(&vtbl[slotPSC], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotPSC] = (void *)HookedSetPSConstF;
        VirtualProtect(&vtbl[slotPSC], sizeof(void *), oldProtect, &oldProtect);
    }
#endif
    int slotRst = offsetof(IDirect3DDevice9Vtbl, Reset) / sizeof(void *);
    g_origReset = (PFN_DeviceReset)ResolveOrigSlot(vtbl[slotRst]);
    if (VirtualProtect(&vtbl[slotRst], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotRst] = (void *)HookedDeviceReset;
        VirtualProtect(&vtbl[slotRst], sizeof(void *), oldProtect, &oldProtect);
    }
    LogLine("[rt] render-target inventory probe installed (CreateRenderTarget/CreateDepthStencilSurface/SetRenderTarget)");

    // GPU vendor, for the alpha-to-coverage backdoor (AMD and NVIDIA spell it
    // differently and each other's spelling is at best a no-op, at worst a
    // garbage point size). Asked of the device's own adapter, not adapter 0.
    {
        D3DDEVICE_CREATION_PARAMETERS cp;
        IDirect3D9 *d3d = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)) &&
            SUCCEEDED(IDirect3DDevice9_GetDirect3D(dev, &d3d)) && d3d) {
            D3DADAPTER_IDENTIFIER9 ai;
            if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, cp.AdapterOrdinal, 0, &ai))) {
                g_gpuVendor = (LONG)ai.VendorId;
                char al[640];   // Description alone can be 512 bytes
                sprintf(al, "[d3d9] adapter: %.500s (vendor 0x%04lX) - alpha-to-coverage dialect %s",
                        ai.Description, (unsigned long)ai.VendorId,
                        ai.VendorId == 0x1002 ? "AMD A2M" :
                        ai.VendorId == 0x10DE ? "NVIDIA ATOC" : "none (A2C inert)");
                LogLine(al);
            }
            IDirect3D9_Release(d3d);
        }
    }

    // RETIRED: an IDirect3DSwapChain9::Present (slot 3) hook was added here as
    // a third candidate, after neither device-level present method fired.
    // It CRASHED the game at the first frame - the log reached render start
    // (shaders compiled, worker threads named) and died before the first
    // FRAME line, which is exactly when a present would first occur. Removed
    // rather than left behind a flag: the device Present/PresentEx hooks are
    // harmless (they simply never fire) and cost nothing to keep, but this
    // one is a confirmed crash and is not worth another attempt for what was
    // only an exploratory question. Where presentation happens is a
    // side-question anyway; the Sleep probe below is what actually
    // investigates the 60fps cap.
    sprintf(line, "[d3d9] present hooks installed: Present slot=%d PresentEx=%s", slot,
            g_origDevicePresentEx ? "hooked" : "skipped (device is not Ex)");
    LogLine(line);
}

// ---- 59.94fps frame limiter unlock ---------------------------------------
//
// Located by sampling main-thread Sleep call sites: the dominant one is
// FUN_00ac3040+0x188, the engine's per-frame tick. Decompiled, it is a
// textbook hybrid frame limiter:
//
//   ticksPerFrame = QPC_freq * 1001 / 60000;      // 60000/1001 = 59.94 fps
//   deadline      = prevDeadline + n * ticksPerFrame;
//   while (now < deadline) {
//       Sleep(remaining < granularity ? 0 : 1);   // spin close, sleep 1ms far
//       QueryPerformanceCounter(&now);
//   }
//
// 1001/60000 is the NTSC rate, hardcoded - this is the 60fps lock, and the
// Sleep(0) branch is the ~12,000 calls/frame the probe measured.
//
// ticksPerFrame lives in a global (Ghidra 0x05115570/74, with an init flag at
// 0x05115578), recomputed only once and thereafter read-then-written-back
// unchanged every frame - so simply overwriting it sticks, and no code
// patching is needed.
//
// NOT set to zero: the surrounding arithmetic DIVIDES by this value
// (__aulldiv(..., fVar10, uVar8)), so zero would be an immediate
// divide-by-zero crash. A 1ms target is used instead - effectively unlocked
// while staying a legal divisor.
//
// The standing risk, unchanged from when this was first flagged: engines of
// this era frequently tie animation, physics and cutscene timing to an
// assumed fixed cadence. Removing the cap can desync those in ways far
// subtler than a dropped frame. Default OFF, one config key.
static volatile LONG g_frameUnlockApplied = 0;

static void ApplyFramerateUnlock(void)
{
    if (!g_unlockFramerateEnabled || !g_mainModBase) return;
    volatile unsigned int *initFlag =
        (volatile unsigned int *)(g_mainModBase + FRAME_TARGET_INIT_RVA);
    // Only once the engine has computed its own value - writing earlier would
    // just be overwritten by its one-time init.
    if ((*initFlag & 1) == 0) return;

    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return;

    LONG fpsX100 = g_targetFpsX100;
    unsigned __int64 target;
    const char *desc;
    if (fpsX100 > 0) {
        // ticksPerFrame = freq * 100 / fpsX100 (fpsX100 is fps*100, so this
        // is freq / (fpsX100/100) done in integer-safe order).
        target = ((unsigned __int64)f.QuadPart * 100) / (unsigned __int64)fpsX100;
        desc = "locked";
    } else {
        target = (unsigned __int64)f.QuadPart / 1000;   // 1ms - effectively unlocked
        desc = "unlocked (1ms target)";
    }
    if (target == 0) target = 1;

    volatile unsigned int *ticks =
        (volatile unsigned int *)(g_mainModBase + FRAME_TARGET_TICKS_RVA);
    if (ticks[0] == (unsigned int)target && ticks[1] == (unsigned int)(target >> 32)) return;
    ticks[0] = (unsigned int)target;
    ticks[1] = (unsigned int)(target >> 32);
    if (InterlockedCompareExchange(&g_frameUnlockApplied, 1, 0) == 0 || fpsX100 > 0) {
        char line[192];
        sprintf(line, "[probe] framerate target applied: ticksPerFrame -> %llu (%s, was 59.94fps)",
                (unsigned long long)target, desc);
        LogLine(line);
    }
}

// ---- Draw-pass render-target inventory ------------------------------------
//
// FrameworkDrawManager registers the frame graph by name; its constructor
// (FUN_00ac72d0) pairs each name with a handler:
//
//   DRAW_SHADOW                   -> FUN_00ac6040    (shadow MAP render)
//   DRAW_MULTI_SAMPLE_SCHEDULE    -> FUN_00ac5ee0
//   DRAW_MULTI_SAMPLE_PROPAGATION -> FUN_00ac5f50
//   DRAW_MULTI_SAMPLE_DEPTH       -> FUN_00ac6890
//   DRAW_MULTI_SAMPLE_SHADOW      -> FUN_00ac6b00    (screen-space shadows)
//   DRAW_MULTI_SAMPLE             -> FUN_00ac6e50
//   DRAW_FILTER                   -> FUN_00ac7030
//
// NOTE "MULTI_SAMPLE" here is NOT scene MSAA. SCHEDULE and PROPAGATION open
// with the same shadow-enable gate as DRAW_SHADOW
// ([DAT_0511558c + 0x2c] && +0x2d), and FUN_00ac6b00 reads the cascade split
// values at DAT_05107a00 +0x42c/+0x430. The whole stage is shadow work.
//
// Tagging the current pass and logging the render target bound during it
// answers two things at once:
//   - what RESOLUTION each stage runs at (the "shadows are half res" claim -
//     the inventory has 1920x1080 targets at 3840x2160 output, but half res is
//     equally standard for SSAO/volumetrics, so dimensions alone cannot say)
//   - how many simultaneous targets each stage binds, which is the actual
//     MRT/G-buffer evidence rather than one global max
//
// Limitation worth knowing when reading the output: the tag is set on pass
// ENTRY and never cleared, so render targets bound AFTER DRAW_FILTER (post
// effect, composite, back buffer) are attributed to DRAW_FILTER. The frame
// hook resets it to (none) each frame, so anything before DRAW_SHADOW is
// labelled (none) rather than mislabelled.
// (enum, name table, g_curPass, g_passMaxRtIndex and the seen table are
// declared near the top of this file - the frame hook and
// HookedSetRenderTarget both need them and sit earlier.)
static void *g_trampoline_msSchedule = NULL;
static void *g_trampoline_msPropagation = NULL;
static void *g_trampoline_msDepth = NULL;
static void *g_trampoline_msShadow = NULL;
static void *g_trampoline_ms = NULL;
static void *g_trampoline_filter = NULL;
static void *g_trampoline_menu = NULL;
static void *g_trampoline_backbuf = NULL;

// One tiny naked detour per pass: stamp the tag, jump to the trampoline. No
// return hijack needed - the tag only has to be correct while the pass runs,
// and the next pass overwrites it. Written out longhand because this file
// already records that MSVC's inline assembler does not survive macro
// expansion (C2400).
__declspec(naked) void Detour_msSchedule(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_SCHEDULE
          jmp dword ptr [g_trampoline_msSchedule] } }
__declspec(naked) void Detour_msPropagation(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_PROPAGATION
          jmp dword ptr [g_trampoline_msPropagation] } }
__declspec(naked) void Detour_msDepth(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_DEPTH
          jmp dword ptr [g_trampoline_msDepth] } }
__declspec(naked) void Detour_msShadow(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_SHADOW
          jmp dword ptr [g_trampoline_msShadow] } }
__declspec(naked) void Detour_ms(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS
          jmp dword ptr [g_trampoline_ms] } }
__declspec(naked) void Detour_filter(void)
{ __asm { mov dword ptr [g_curPass], PASS_FILTER
          jmp dword ptr [g_trampoline_filter] } }
__declspec(naked) void Detour_menu(void)
{ __asm { mov dword ptr [g_curPass], PASS_MENU
          jmp dword ptr [g_trampoline_menu] } }
__declspec(naked) void Detour_backbuf(void)
{ __asm { mov dword ptr [g_curPass], PASS_BACKBUF
          jmp dword ptr [g_trampoline_backbuf] } }

// ---- Per-frame GPU fence skip (the frametime square wave) -----------------
//
// FUN_00a96090, called every frame from FUN_00ac3040, is an IDirect3DQuery9
// fence that busy-waits for the GPU to drain:
//
//     query->Issue(D3DISSUE_END);                      // vtable +0x18
//     hr = query->GetData(NULL, 0, D3DGETDATA_FLUSH);  // vtable +0x1c
//     while (hr != S_OK && hr != D3DERR_DEVICELOST && hr != D3DERR_INVALIDCALL) {
//         Sleep(0);
//         hr = query->GetData(NULL, 0, D3DGETDATA_FLUSH);
//     }
//
// (0x88760868 = D3DERR_DEVICELOST, 0x8876086C = D3DERR_INVALIDCALL; GetData
// returns S_FALSE while the GPU has not reached the query.)
//
// So the CPU is never allowed to run ahead of the GPU - a full pipeline flush
// every frame, where D3D9 would normally allow 1-3 frames of buffering. Per-
// frame capture pinned the entire frametime square wave to the enclosing call:
// r=+0.993 against frame time, 1148us on fast frames vs 7480us on slow, and
// that 6332us swing accounts for the whole 6235us frametime gap. Every other
// counter measured (allocations, file reads, WaitForSingleObject calls, shadow
// pass time) was flat - because a spin loop is not work, and Sleep(0) is not a
// wait object.
//
// Skipping it lets the CPU pipeline ahead. RISKS, and why this is default OFF:
//   - the fence may exist so the engine can safely touch resources the GPU is
//     still reading; removing it could corrupt or tear rendering
//   - it may be load-bearing for frame-limiter timing accuracy
//   - Issue() is skipped too, so anything else expecting that query to have
//     been issued sees it un-issued
// Expect more input latency if it works, since latency is exactly what a
// per-frame flush trades away.
#define GPU_FENCE_RVA (0x00a96090 - 0x00400000)
static void *g_trampoline_gpuFence = NULL;
static volatile LONG g_gpuFenceSkipped = 0;

__declspec(naked) void Detour_gpuFence(void)
{
    __asm {
        cmp dword ptr [g_gpuSyncSkip], 0
        jz  run_gpu_fence
        lock inc dword ptr [g_gpuFenceSkipped]
        ret                                    // skip the fence entirely
    run_gpu_fence:
        jmp dword ptr [g_trampoline_gpuFence]
    }
}

// ---- DRAW_SHADOW pass timer (frametime square-wave diagnostic) ------------
// FUN_00ac6040 is the DRAW_SHADOW pass handler registered by
// FrameworkDrawManager; it wraps FUN_00a32a00, the shadow render proper (see
// FEATURES.md, "Decomp round 2"). Hooked here rather than FUN_00a32a00 itself
// because that one opens with 53 8B DC 83 EC 08 83 E4 F0 - it realigns the
// stack with AND ESP,-0x10 and addresses through EBX, so a return-address
// hijack there would have to find the return slot after a realignment. The
// handler has an ordinary 55 8B EC 51 56 prologue: a clean 5-byte patch (JMP
// rel32 is exactly 5, so no NOP padding) and the return address sits at [esp]
// on entry, the same shape every other paired hook here relies on.
#define DRAW_SHADOW_RVA (0x00ac6040 - 0x00400000)
static void *g_trampoline_drawShadow = NULL;
static DWORD g_dsTid = 0;
static DWORD g_dsRet = 0;
static unsigned __int64 g_dsEnter = 0;

__declspec(noinline) int __cdecl OnEnter_drawShadow_C(void *retAddr)
{
    g_curPass = PASS_SHADOW;             // pass tag for the RT inventory
    if (!g_logFrameTimes) return 0;      // only costs anything while capturing
    g_dsTid = GetCurrentThreadId();
    g_dsRet = (DWORD)retAddr;
    g_dsEnter = __rdtsc();
    return 1;
}

__declspec(noinline) void *__cdecl OnReturn_drawShadow_C(void)
{
    void *ret = (void *)g_dsRet;
    if (g_dsTid == GetCurrentThreadId() && g_dsEnter && g_cyclesPerUsec > 0.0)
        g_shadowPassUsec = (LONG)((double)(__rdtsc() - g_dsEnter) / g_cyclesPerUsec);
    return ret;
}

__declspec(naked) void OnReturn_drawShadow(void)
{
    __asm {
        push eax
        pushfd
        call OnReturn_drawShadow_C
        mov  ecx, eax
        popfd
        pop  eax
        jmp  ecx
    }
}

__declspec(naked) void Detour_drawShadow(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]          // return address -> only param
        push eax
        call OnEnter_drawShadow_C
        add esp, 4
        test eax, eax
        jz skip_drawShadow
        mov dword ptr [esp + 8], offset OnReturn_drawShadow
    skip_drawShadow:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_drawShadow]
    }
}

// ---- Frame-limiter sleep granularity: a monotonic high-water mark ---------
//
// Explains an observation that looks impossible at first: locking to 100fps
// yields ~70fps with erratic frametimes, while leaving it unlocked yields
// ~110fps. A cap that makes the game SLOWER than no cap.
//
// The limiter's inner loop (FUN_00ac3040, decompiled in
// ghidra_output/frame_delta.txt) decides how to wait like this:
//
//     remaining = deadline - now;
//     if (remaining < [0x05115568])  Sleep(0);     // busy-spin
//     else                           Sleep(1);     // real sleep
//     ...
//     elapsed = now_after - now_before;
//     [0x05115568] = max([0x05115568], elapsed);   // <-- RUNNING MAXIMUM
//
// [0x05115568/6c] is the engine's estimate of how coarse Sleep(1) is, and it
// is a HIGH-WATER MARK: only ever raised, never reset and never decayed. A
// single bad Sleep - a loading screen, an alt-tab, any scheduler hiccup -
// raises it permanently for the rest of the session.
//
// Against the frame interval:
//     59.94fps -> 16.68ms/frame; a ~2ms mark leaves most of the frame asleep
//     100fps   -> 10.00ms/frame; once the mark passes 10ms, `remaining` is
//                 ALWAYS below it, so every frame becomes a pure busy-spin
//     unlocked -> ticksPerFrame is 1ms, `now` is already past the deadline,
//                 so the loop body never executes at all - the limiter is free
//
// The spin was measured at ~31% of the MAIN thread earlier in this project
// (see PROGRESS.md and the UnlockFramerate notes) - and the main thread is
// what dispatches all other work. Burning a third of it is enough to turn
// ~110fps into ~70.
//
// Diagnostic first: log the mark every monitor window before changing
// anything. If it reads well under the frame interval, this theory is wrong
// and the slowdown is something else.
//
// SpinGuardUs then caps it. The cap must be close to the REAL Sleep(1)
// resolution - the timer probe measured this system at 1.0000ms - because
// setting it too low makes the limiter sleep when it should have spun and
// overshoot the deadline instead, which trades a throughput problem for a
// pacing one. 0 = off (default: observe before intervening).
#define SLEEP_GRAN_RVA (0x05115568 - 0x00400000)
static volatile LONG g_spinGuardUs = 0;
static volatile LONG g_spinGuardWrites = 0;
static volatile LONG g_lastGranUs = 0;

static void ReportAndClampSleepGranularity(void)
{
    if (g_mainModBase == 0) return;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return;
    __try {
        volatile unsigned int *gran =
            (volatile unsigned int *)(g_mainModBase + SLEEP_GRAN_RVA);
        unsigned __int64 cur =
            ((unsigned __int64)gran[1] << 32) | (unsigned __int64)gran[0];
        // Sanity: before the limiter has run even once this is 0, and a wild
        // value would mean the address is not what we think it is.
        unsigned __int64 us = (cur * 1000000ULL) / (unsigned __int64)f.QuadPart;
        if (us > 10000000ULL) return;            // >10s - not a sleep duration
        g_lastGranUs = (LONG)us;

        LONG capUs = g_spinGuardUs;
        if (capUs > 0 && us > (unsigned __int64)capUs) {
            unsigned __int64 want =
                ((unsigned __int64)capUs * (unsigned __int64)f.QuadPart) / 1000000ULL;
            if (want == 0) want = 1;
            gran[0] = (unsigned int)want;
            gran[1] = (unsigned int)(want >> 32);
            InterlockedIncrement(&g_spinGuardWrites);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- SIM DELTA: unquantise the per-frame simulation delta -----------------
//
// Reported by another modder working on the same 60fps bug class, then
// verified here against the decompile (ghidra_output/frame_delta.txt).
//
// FUN_00ac33b0 is the frame-delta function. It is called once per frame from
// FUN_00ab93f0, immediately after the limiter FUN_00ac3040, and its output
// lands in the sim struct:
//
//     FUN_00ac3040();                            // limiter sleeps to deadline
//     FUN_00ac3a90();
//     piVar3 = (int *)FUN_00ac33b0(&local_1c);   // delta
//     param_1[0xc] = *piVar3;
//
// What it computes:
//
//     bucket = qpcFreq * 1001 / 60000;      // 0x3e9 = 1001; ONE 59.94Hz frame
//     qnow   = (now / bucket) * bucket;     // <-- TRUNCATES THE TIMESTAMP
//     delta  = (qnow - qprev) * 300000 / qpcFreq;
//     *out   = ((delta + 0x9c6) / 0x138d) * 0x138d;    // 2502 / 5005
//
// The final rounding is NOT the problem - it is integer-division cleanup.
// The defect is the line above it: `now` is truncated to a whole number of
// 59.94Hz frame periods BEFORE differencing, so consecutive deltas can only
// ever be 0, 5005, 10010... Nothing between is representable.
//
// TWO effects stack, and the second is the one that makes this a STOCK bug.
//
// 1. Systematic rate mismatch. Buckets advanced per frame = 59.94 / F:
//        F = 30     -> 1.998
//        F = 59.94  -> 1.000
//        F = 60.00  -> 0.999   one ZERO every ~1000 frames
//        F = 120    -> 0.4995  0, 5005, 0, 5005 ...
//    The 120 row is exactly the reported "0ms, 16.683ms, 0ms, 16.683ms".
//
// 2. Frame-time JITTER. The limiter's deadline advances by exactly one bucket,
//    but `now` is sampled after its Sleep-spin loop exits and overshoots by a
//    variable amount. With now_k = deadline_k + jitter_k, a long frame followed
//    by a short one can put both truncated timestamps in the SAME bucket - a
//    zero delta - at ANY framerate.
//
// An earlier version of this comment claimed zeros were impossible at or below
// 59.94. That was wrong: it assumed a perfectly regular frame period. These
// bugs do occur in the stock game at its base 59.94 rate ("60fps" is rounding).
// What changes with framerate is only how much jitter it takes:
//        F = 30     -> a ~33ms negative swing; very rare
//        F = 59.94  -> enough to cross the phase boundary; sub-millisecond
//        F > 59.94  -> none needed, it happens on its own
//
// This is the better model because it explains the one property nothing else
// did: these bugs are PROBABILISTIC. Each has a chance to fail rather than
// failing reliably, and jitter-triggered bucket collisions produce exactly
// that. It also explains why 30fps mode fixes all four without zeros needing
// to be impossible - just orders of magnitude rarer.
//
// Consequence: this fix helps at the STOCK 59.94 rate too, not only under
// UnlockFramerate.
//
// NOTE this is a SEPARATE cached bucket from the one ApplyFramerateUnlock
// writes. The limiter caches at 0x05115570/74 (flag 0x05115578); this function
// caches its own at 0x05115580/84 (flag 0x05115588). Both compute
// freq*1001/60000. So unlocking the framerate speeds up pacing while leaving
// the delta quantum pinned at 59.94 - i.e. UnlockFramerate makes an existing
// stock defect fire constantly instead of occasionally. It does not create it.
//
// DO NOT "fix" this by writing the delta's own bucket global to match the real
// framerate. The 5005 rounding downstream is hardcoded: at 120fps the true
// delta becomes 2500 ticks, and round-to-nearest(2500, 5005) is ZERO. That
// makes it permanently broken instead of intermittently.
//
// The only workable intervention is to replace the OUTPUT with the true
// unquantised delta. `this+0x30` holds the raw 64-bit QPC timestamp the
// function itself uses as `now`, so the detour keeps its own previous raw
// value and recomputes:  (now - prev) * 300000 / qpcFreq.
//
// CONFIRMED IN GAME (2026-08-10) and therefore default ON. It fixes two
// independent symptoms at once:
//   - the interact prompt failing above 59.94fps
//   - the stamina bar not draining at higher framerates
// The stamina one is the more informative of the two: it is a continuous
// per-frame accumulation rather than a discrete event, which is direct
// evidence that the zero-deltas are being consumed as real elapsed time by
// ordinary gameplay code, not just by some edge-case state machine.
//
// RESIDUAL RISK, unproven either way: the engine has only ever seen deltas
// that are exact multiples of 5005. Anything downstream doing `delta / 5005`
// to derive a frame count would get 0 at 60fps where it used to get 1. No
// regression has been observed, but absence of a report is not absence of a
// bug - the flag stays live and hot-toggleable so any suspected regression can
// be A/B'd on the spot rather than requiring a rebuild.
#define SIM_DELTA_RVA (0x00ac33b0 - 0x00400000)
static void *g_trampoline_simDelta = NULL;
static DWORD g_sdTid = 0;
static DWORD g_sdRet = 0;
static unsigned int g_sdThis = 0, g_sdOut = 0;
static unsigned __int64 g_sdPrevRaw = 0;
static int g_sdHavePrev = 0;
static LONGLONG g_sdFreq = 0;
static volatile LONG g_sdReplaced = 0;   // frames whose delta we rewrote
static volatile LONG g_sdZeroFixed = 0;  // of those, ones the engine reported as 0
static volatile LONG g_sdLastOrig = 0, g_sdLastNew = 0;
// Cap a replacement at 8 frames' worth. After a loading screen or a long
// stall the true elapsed time can be seconds, and handing the simulation a
// single multi-second delta is its own class of disaster (physics tunnelling,
// timers firing en masse). The stock code cannot produce that because its
// deadline logic clamps first; ours has to clamp explicitly.
#define SD_MAX_DELTA (5005 * 8)

__declspec(noinline) int __cdecl OnEnter_simDelta_C(void *thisPtr, void *outPtr, void *retAddr)
{
    if (!g_simDeltaFix || !thisPtr || !outPtr) return 0;
    g_sdTid  = GetCurrentThreadId();
    g_sdRet  = (DWORD)retAddr;
    g_sdThis = (unsigned int)thisPtr;
    g_sdOut  = (unsigned int)outPtr;
    return 1;
}

__declspec(noinline) void *__cdecl OnReturn_simDelta_C(void)
{
    void *ret = (void *)g_sdRet;
    __try {
        if (g_sdTid != GetCurrentThreadId() || !g_sdThis || !g_sdOut) return ret;
        if (g_sdFreq == 0) {
            LARGE_INTEGER f;
            if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) g_sdFreq = f.QuadPart;
            else return ret;
        }
        unsigned __int64 now = *(volatile unsigned __int64 *)(g_sdThis + 0x30);
        LONG *out = (LONG *)g_sdOut;
        LONG orig = *out;
        if (g_sdHavePrev && now > g_sdPrevRaw) {
            unsigned __int64 d = now - g_sdPrevRaw;
            unsigned __int64 t = (d * 300000ULL) / (unsigned __int64)g_sdFreq;
            if (t > SD_MAX_DELTA) t = SD_MAX_DELTA;
            *out = (LONG)t;
            g_sdLastOrig = orig;
            g_sdLastNew  = (LONG)t;
            InterlockedIncrement(&g_sdReplaced);
            if (orig == 0) InterlockedIncrement(&g_sdZeroFixed);
        }
        g_sdPrevRaw = now;
        g_sdHavePrev = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// EAX must survive: the caller is decompiled as reading a pointer back out of
// it (`piVar3 = FUN_00ac33b0(...); param_1[0xc] = *piVar3;`). Whether that is
// real or a decompiler artefact of the trailing store, clobbering it would be
// the kind of bug that only shows up as garbage frame timing.
__declspec(naked) void OnReturn_simDelta(void)
{
    __asm {
        push eax                      // preserve the callee's return value
        pushfd
        call OnReturn_simDelta_C      // -> eax = the real return address
        mov  ecx, eax                 // ECX is volatile here, safe scratch
        popfd
        pop  eax                      // restore the return value
        jmp  ecx
    }
}

__declspec(naked) void Detour_simDelta(void)
{
    __asm {
        push ecx
        push edx
        // After both pushes: [esp+4]=this(ECX), [esp+8]=return address,
        // [esp+12]=the out-pointer stack arg. __cdecl wants them pushed
        // right-to-left to arrive as (thisPtr, outPtr, retAddr).
        mov eax, [esp + 8]       // return address -> 3rd
        push eax
        mov eax, [esp + 16]      // out-pointer    -> 2nd
        push eax
        mov eax, [esp + 12]      // this           -> 1st
        push eax
        call OnEnter_simDelta_C
        add esp, 12
        test eax, eax
        jz skip_simDelta
        mov dword ptr [esp + 8], offset OnReturn_simDelta
    skip_simDelta:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_simDelta]
    }
}

// ---- main-thread file-open probe -----------------------------------------
// `ZwCreateFile` (7 records, 0.15s) and `ZwClose` (4, 0.09s) appeared in the
// stutter attribution: the main thread is opening files SYNCHRONOUSLY
// mid-frame. `ReadFile` has been instrumented since this project began;
// `CreateFile` never has, so this has been invisible the entire time. A file
// open is a kernel round-trip that can touch the filesystem (directory walk,
// antivirus filter, cold metadata) and doing it on the render thread is a
// classic hitch source.
//
// The exe imports the ANSI variants (`CreateFileA`, `CloseHandle`) - checked
// against its own import table rather than assumed, since hooking
// `CreateFileW` would have silently done nothing.
//
// Diagnostic only: times main-thread calls and records the slowest distinct
// paths. No behaviour change.
typedef HANDLE (WINAPI *PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *PFN_CloseHandle)(HANDLE);
static PFN_CreateFileA g_realCreateFileA = NULL;
static PFN_CloseHandle g_realCloseHandle = NULL;
static volatile LONG g_openCount=0, g_openSumUsec=0, g_openMaxUsec=0;
static volatile LONG g_closeCount=0, g_closeSumUsec=0, g_closeMaxUsec=0;

// Slowest distinct opens seen, for identifying WHAT is being opened.
#define SLOW_OPEN_SLOTS 10
#define SLOW_OPEN_MIN_USEC 300
static struct { LONG usec; char path[160]; } g_slowOpens[SLOW_OPEN_SLOTS];
static LONG g_slowOpenCount = 0;
static CRITICAL_SECTION g_slowOpenLock;
static volatile LONG g_slowOpenLockState = 0;

static void RecordSlowOpen(const char *path, LONG usec)
{
    if (!path || usec < SLOW_OPEN_MIN_USEC) return;
    if (InterlockedCompareExchange(&g_slowOpenLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_slowOpenLock);
        g_slowOpenLockState = 2;
    } else {
        while (g_slowOpenLockState != 2) Sleep(0);
    }
    EnterCriticalSection(&g_slowOpenLock);
    // Keep only the worst offenders: if full, replace the fastest entry, so
    // the table converges on the genuinely expensive opens rather than
    // whichever happened first.
    LONG target = -1;
    if (g_slowOpenCount < SLOW_OPEN_SLOTS) {
        target = g_slowOpenCount++;
    } else {
        LONG minIdx = 0;
        for (LONG i = 1; i < SLOW_OPEN_SLOTS; i++)
            if (g_slowOpens[i].usec < g_slowOpens[minIdx].usec) minIdx = i;
        if (usec > g_slowOpens[minIdx].usec) target = minIdx;
    }
    if (target >= 0) {
        g_slowOpens[target].usec = usec;
        strncpy(g_slowOpens[target].path, path, sizeof(g_slowOpens[0].path) - 1);
        g_slowOpens[target].path[sizeof(g_slowOpens[0].path) - 1] = 0;
    }
    LeaveCriticalSection(&g_slowOpenLock);
}

static HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                       LPSECURITY_ATTRIBUTES lpSec, DWORD dwCreationDisposition,
                                       DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    unsigned __int64 t0 = isMain ? __rdtsc() : 0;
    HANDLE h = g_realCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSec,
                                 dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    if (isMain && g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_openCount);
        InterlockedExchangeAdd(&g_openSumUsec, us);
        if (us > g_openMaxUsec) g_openMaxUsec = us;
        RecordSlowOpen(lpFileName, us);
    }
    return h;
}

static BOOL WINAPI HookedCloseHandle(HANDLE hObject)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    unsigned __int64 t0 = isMain ? __rdtsc() : 0;
    BOOL ok = g_realCloseHandle(hObject);
    if (isMain && g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_closeCount);
        InterlockedExchangeAdd(&g_closeSumUsec, us);
        if (us > g_closeMaxUsec) g_closeMaxUsec = us;
    }
    return ok;
}

static void LogD3DWindow(void)
{
    ApplyFramerateUnlock();
    char line[256];
    LONG nD3d = g_d3dSlotCount;
    for (LONG i = 0; i < nD3d; i++) {
        D3DSlot *sl = &g_d3dSlots[i];
        LONG c = sl->count, sum = sl->sumUsec;
        LONG wc = c - g_prevD3dCount[i], ws = sum - g_prevD3dSum[i];
        g_prevD3dCount[i] = c;
        g_prevD3dSum[i] = sum;
        if (wc <= 0) continue;
        LONG mx = InterlockedExchange(&sl->maxUsec, 0);
        // Only report methods that actually cost something this window -
        // a microsecond-level call rate is noise here.
        if (ws < 1000 && mx < 2000) continue;
        sprintf(line, "[d3d9] %-28s calls=%-6ld total_usec=%-8ld max_usec=%ld",
                sl->name, wc, ws, mx);
        LogLine(line);
    }

    LONG discardTotal = g_lockRectDiscardInjected;
    LONG discardWindow = discardTotal - g_prevLockRectDiscardInjected;
    g_prevLockRectDiscardInjected = discardTotal;
    // Report unconditionally once anything eligible has been seen, including
    // windows where nothing was injected. A silent window used to be
    // ambiguous between "no dynamic textures locked" and "the fix is dead";
    // eligible/firstseen/tracked tell those apart at a glance.
    LONG elig = g_lockRectEligible, firstSeen = g_lockRectFirstSeen;
    if (elig > 0) {
        sprintf(line, "[d3d9] DISCARD inject=%ld this window (total=%ld) | eligible=%ld firstseen=%ld mipskip=%ld tracked=%ld/%d clears=%ld",
                discardWindow, discardTotal, elig, firstSeen, g_lockRectMipSkipped,
                g_seenTextureCount, SEEN_TEX_SLOTS, g_seenTexClears);
        LogLine(line);
    }
    {
        // Present timing on the real device. Reported as its own line rather
        // than folded into the D3DSlot table because it is hooked directly,
        // not through the slot machinery.
        static LONG prevPresentCount = 0, prevPresentSum = 0;
        LONG pc = g_presentCount, ps = g_presentSumUsec;
        LONG dc = pc - prevPresentCount, ds = ps - prevPresentSum;
        prevPresentCount = pc; prevPresentSum = ps;
        if (dc > 0) {
            LONG pmax = InterlockedExchange(&g_presentMaxUsec, 0);
            sprintf(line, "[d3d9] Present: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                    dc, ds / dc, pmax, pc);
            LogLine(line);
        }

        // Real-device UpdateSurface/UpdateTexture/CreateTexture - see the
        // comment above HookedUpdateSurface for why these are being measured
        // for the first time in this run.
        {
            static LONG prevUS=0, prevUSsum=0, prevUT=0, prevUTsum=0, prevCT=0, prevCTsum=0;
            LONG usC=g_updSurfCount, usS=g_updSurfSumUsec, dUS=usC-prevUS, dUSs=usS-prevUSsum;
            prevUS=usC; prevUSsum=usS;
            if (dUS > 0) {
                LONG mx = InterlockedExchange(&g_updSurfMaxUsec, 0);
                sprintf(line, "[d3d9] UpdateSurface: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dUS, dUSs/dUS, mx, usC);
                LogLine(line);
            }
            LONG utC=g_updTexCount, utS=g_updTexSumUsec, dUT=utC-prevUT, dUTs=utS-prevUTsum;
            prevUT=utC; prevUTsum=utS;
            if (dUT > 0) {
                LONG mx = InterlockedExchange(&g_updTexMaxUsec, 0);
                sprintf(line, "[d3d9] UpdateTexture: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dUT, dUTs/dUT, mx, utC);
                LogLine(line);
            }
            static LONG prevVB=0, prevVBs=0, prevIB=0, prevIBs=0;
            LONG vbC=g_cvbCount, vbS=g_cvbSumUsec, dVB=vbC-prevVB, dVBs=vbS-prevVBs;
            prevVB=vbC; prevVBs=vbS;
            if (dVB > 0) {
                LONG mx = InterlockedExchange(&g_cvbMaxUsec, 0);
                sprintf(line, "[d3d9] CreateVertexBuffer: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)", dVB, dVBs/dVB, mx, vbC);
                LogLine(line);
            }
            LONG ibC=g_cibCount, ibS=g_cibSumUsec, dIB=ibC-prevIB, dIBs=ibS-prevIBs;
            prevIB=ibC; prevIBs=ibS;
            if (dIB > 0) {
                LONG mx = InterlockedExchange(&g_cibMaxUsec, 0);
                sprintf(line, "[d3d9] CreateIndexBuffer: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)", dIB, dIBs/dIB, mx, ibC);
                LogLine(line);
            }
            LONG ctC=g_createTexCount, ctS=g_createTexSumUsec, dCT=ctC-prevCT, dCTs=ctS-prevCTsum;
            prevCT=ctC; prevCTsum=ctS;
            if (dCT > 0) {
                LONG mx = InterlockedExchange(&g_createTexMaxUsec, 0);
                sprintf(line, "[d3d9] CreateTexture: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dCT, dCTs/dCT, mx, ctC);
                LogLine(line);
            }
            // Shader creation. Reported per window AND cumulatively so the
            // load-time-vs-gameplay split is visible: a big cumulative total
            // with near-zero per-window counts means it all happens at load
            // and pre-warming would gain nothing.
            static LONG prevVS=0, prevVSsum=0, prevPS=0, prevPSsum=0;
            LONG vsC=g_vsCount, vsS=g_vsSumUsec, dVS=vsC-prevVS, dVSs=vsS-prevVSsum;
            prevVS=vsC; prevVSsum=vsS;
            if (dVS > 0) {
                LONG mx = InterlockedExchange(&g_vsMaxUsec, 0);
                sprintf(line, "[d3d9] CreateVertexShader: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld mainthread=%ld)",
                        dVS, dVSs/dVS, mx, vsC, g_vsMainCount);
                LogLine(line);
            }
            LONG psC=g_psCount, psS=g_psSumUsec, dPS=psC-prevPS, dPSs=psS-prevPSsum;
            prevPS=psC; prevPSsum=psS;
            if (dPS > 0) {
                LONG mx = InterlockedExchange(&g_psMaxUsec, 0);
                sprintf(line, "[d3d9] CreatePixelShader: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld mainthread=%ld)",
                        dPS, dPSs/dPS, mx, psC, g_psMainCount);
                LogLine(line);
            }

            static LONG prevSL=0, prevSLsum=0;
            LONG slC=g_surfLockCount, slS=g_surfLockSumUsec, dSL=slC-prevSL, dSLs=slS-prevSLsum;
            prevSL=slC; prevSLsum=slS;
            static LONG prevCube=0, prevCubeSum=0;
            LONG cbC=g_cubeLockCount, cbS=g_cubeLockSumUsec, dCB=cbC-prevCube, dCBs=cbS-prevCubeSum;
            prevCube=cbC; prevCubeSum=cbS;
            if (dCB > 0) {
                LONG mx = InterlockedExchange(&g_cubeLockMaxUsec, 0);
                sprintf(line, "[d3d9] CubeTexture::LockRect: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dCB, dCBs/dCB, mx, cbC);
                LogLine(line);
            }
#if ENABLE_DEFER_UPLOADS
            if (g_deferQueued || g_deferIssued) {
                sprintf(line, "[d3d9] deferred uploads: queued=%ld issued=%ld overflow=%ld depth=%ld/%d (limit %ld/frame)",
                        g_deferQueued, g_deferIssued, g_deferOverflow,
                        g_uploadQueueDepth, MAX_PENDING_UPLOADS, g_deferPerFrame);
                LogLine(line);
            }
#endif
            {
                // The deferred-vs-forward verdict. The original version keyed
                // this off g_maxRtIndexSeen alone and printed "MRT / G-buffer
                // in use (deferred)" - but that max counts
                // SetRenderTarget(n, NULL), i.e. slots being CLEARED. Only a
                // non-NULL bind above slot 0 is real MRT.
                static LONG lastReal = -1, lastAny = -1;
                if (g_maxRealRtIndex != lastReal || g_maxRtIndexSeen != lastAny) {
                    lastReal = g_maxRealRtIndex;
                    lastAny = g_maxRtIndexSeen;
                    sprintf(line, "[rt] slot>0: real_binds=%ld null_unbinds=%ld | "
                                  "max index any=%ld REAL=%ld -> %s",
                            g_mrtRealBinds, g_mrtNullUnbinds,
                            g_maxRtIndexSeen, g_maxRealRtIndex,
                            g_mrtRealBinds == 0
                                ? "SINGLE RT ONLY (forward - MSAA worth investigating)"
                                : "genuine MRT / G-buffer (deferred - MSAA problematic)");
                    LogLine(line);
                }
            }
#if ENABLE_CASCADE_HUNT
            if (g_cascadeRewrites || g_propagations) {
                sprintf(line, "[cascade] near x%ld : render-side rewrites=%ld  sampling-side propagations=%ld",
                        g_nearCascadePct, g_cascadeRewrites, g_propagations);
                LogLine(line);
            }
#endif
            if (g_splitSrcWrites || g_splitSrcSeenNear > 0.0f) {
                sprintf(line, "[split-src] engine near=%.2f far=%.2f (product) | writes=%ld near%%=%ld far%%=%ld",
                        g_splitSrcSeenNear, g_splitSrcSeenFar, g_splitSrcWrites,
                        g_shadowSplitNearPct, g_shadowSplitFarPct);
                LogLine(line);
            }
            if (g_shadowMapRes > 0) {
                sprintf(line, "[shadow] engine res value: want=%ld last_seen=%ld writes=%ld",
                        g_shadowMapRes, g_shadowResLastSeen, g_shadowResWrites);
                LogLine(line);
            }
#if ENABLE_SCALING_MODE
            // Always reported, even when not overriding: the game's OWN
            // scaling default is the thing worth knowing here.
            {
                const char *cur = "?";
                if (g_scalingSeen20 == 3 && g_scalingSeen40 == 0) cur = "None";
                else if (g_scalingSeen20 == 1 && g_scalingSeen40 == 0) cur = "Standard";
                else if (g_scalingSeen20 == 1 && g_scalingSeen40 == 1) cur = "Advanced";
                sprintf(line, "[scaling] engine is %s (+0x20=%ld +0x40=%ld) | want=%ld writes=%ld",
                        cur, g_scalingSeen20, g_scalingSeen40, g_scalingMode, g_scalingWrites);
                LogLine(line);
            }
#endif  // ENABLE_SCALING_MODE
            // Always reported when set, so "is it actually on?" never has to be
            // guessed at again - the last attempt was judged by eye while the
            // toggle was off the whole time.
            // Draw attribution is reported whenever draws exist, NOT only when
            // MSAA is on. The first version gated it behind the MSAA counters,
            // so setting MsaaSamples=0 to survey the UNMODIFIED renderer also
            // switched off the survey - the measurement disabled by the very
            // control it was meant to be independent of.
            if (g_drawsTotal) {
                char dl[320];
                int dopos = sprintf(dl, "[draws] by pass:");
                for (int pi = 0; pi < PASS_COUNT; pi++)
                    dopos += sprintf(dl + dopos, " %s=%ld",
                                     g_passNames[pi], g_drawsByPass[pi]);
                LogLine(dl);
                sprintf(dl, "[draws] by target: sceneColour=%ld linearDepth=%ld "
                            "shadowMaps=%ld other=%ld  (total=%ld) | distinct full-screen "
                            "A8R8G8B8 targets=%ld",
                        g_drawsRtFmt21, g_drawsRtFmt114, g_drawsRtShadow,
                        g_drawsRtOther, g_drawsTotal, g_sceneRtCount);
                LogLine(dl);
                {
                    LONG n = g_sceneRtCount, i;
                    if (n > SCENE_RT_MAX) n = SCENE_RT_MAX;
                    int so2 = sprintf(dl, "[scenert] per-surface draws (total/inMULTI_SAMPLE):");
                    for (i = 0; i < n; i++)
                        so2 += sprintf(dl + so2, "  #%ld=%ld/%ld",
                                       i + 1, g_sceneRtDraws[i], g_sceneRtDrawsMs[i]);
                    LogLine(dl);
                }
            }
            if (g_msaaSamples >= 2 || g_msSubstitutions) {
                // substitutions and resolves should track 1:1 per frame; a gap
                // means the pass ended without the resolve boundary firing and
                // the engine is sampling an unresolved (stale) texture.
                // getLies counts engine captures of our surfaces through the
                // Get methods (theory 1 - nonzero means it DOES push/pop
                // bindings); dsOutside must stay 0 now the lies seal the leak;
                // failedALL covers failures in passes where failedMS is blind;
                // rsWrites/forced say whether the engine touches the two
                // multisample render states at all (theories 2/3).
                char ml[384];
                sprintf(ml, "[msaa] x%ld subs=%ld resolves=%ld resolveFail=%ld %ux%u | "
                            "draws total=%ld whileMS=%ld failedMS=%ld failedALL=%ld | "
                            "getLies rt=%ld ds=%ld dsOutside=%ld | rsWrites=%ld forced=%ld | "
                            "clearZ=%.3f | texMemFreeMB=%ld | zfuncSeen=0x%lX zfuncForced=%ld stencilNE=%ld | "
                            "r32f subs=%ld resolves=%ld fails=%ld | a2c=%ld",
                        g_msaaSamples, g_msSubstitutions, g_msResolves, g_msFailures,
                        g_msW, g_msH, g_drawsTotal, g_drawsWhileMs, g_drawsFailedMs,
                        g_drawsFailedAll, g_msGetRtLies, g_msGetDsLies,
                        g_msDsOutsideRebind, g_rsMsWrites, g_rsMsForced, g_lastClearZ,
                        g_availTexMemMB, (unsigned long)g_zfuncSeenMask, g_zfuncForced,
                        g_stencilNonAlways, g_msR32fSubs, g_msR32fResolves, g_msR32fFails,
                        g_a2cMirrored);
                LogLine(ml);
            }
            // SSAA status, on its OWN condition - reported whenever a scale is
            // set even if nothing else is active, because "the option is on and
            // nothing happened" is precisely the case that needs to be
            // distinguishable from "the option is off". The three numbers
            // answer three different failure modes:
            //   internal size  - did the engine ACCEPT a size above the
            //                    backbuffer, or clamp/ignore it
            //   linearBlits    - is the downsample actually being filtered
            //                    (0 while active means POINT decimation, i.e.
            //                    full cost and no antialiasing whatsoever)
            //   linearFails    - the driver refused LINEAR on StretchRect and
            //                    the engine's own filter was used instead
            if (g_ssaaScale != 100 || g_ssaaWrites) {
                unsigned int iw = 0, ih = 0;
                GetInternalRenderSize(&iw, &ih);
                char sl[256];
                sprintf(sl, "[ssaa] scale=%ld%% mode=%s effective=%ux%u backbuf=%ux%u | "
                            "descScaled=%ld rebuilds=%ld/blocked=%ld writes=%ld "
                            "linearBlits=%ld linearFails=%ld outRes=%ux%u/%ld/fail=%ld%s",
                        g_ssaaScale,
                        g_ssaaMode == 1 ? "descriptor" : "resolution",
                        iw, ih, g_backbufW, g_backbufH,
                        g_ssaaDescScaled, g_ssaaRebuildsAllowed, g_ssaaRebuildsBlocked,
                        g_ssaaWrites,
                        g_ssaaLinearBlits, g_ssaaLinearFails,
                        g_ssaaInterW, g_ssaaInterH, g_ssaaInterBlits, g_ssaaInterFails,
                        // The one-line verdict, and it is deliberately derived
                        // from what is TRUE at runtime rather than from which
                        // mode was requested: supersampling happens only when
                        // the rendered size exceeds the presented size.
                        // `effective` comes from GetInternalRenderSize, which
                        // both modes keep honest, so this reads correctly for
                        // either.
                        (g_ssaaScale != 100 && g_backbufW && iw > g_backbufW)
                            ? "  SUPERSAMPLING" : "  (no downsample - not supersampling)");
                LogLine(sl);
            }
            // subs counting while a pick is active proves the hash identified
            // a shader the engine actually binds; subs staying 0 with objs>0
            // means that candidate is not bound right now (e.g. a menu);
            // objs=0 means none of the candidate shaders were created.
            // variants>0 proves the pattern matcher recognised the cutout
            // shaders; binds>0 proves they are actually being drawn with.
            // buildFails>0 means the synthesised bytecode was rejected by the
            // runtime, i.e. the transform itself is wrong.
            // Boot survived; back to buffered logging - UNLESS LogFlush=1.
            // Buffering is what makes a crash log stop at an arbitrary point:
            // the last lines sit unflushed and die with the process, so the
            // log's ending looks like the crash site and is not. That misread
            // cost real time chasing the intermittent startup failure.
            // LogFlush=1 keeps every line on disk at the documented cost
            // below, which is the right trade while reproducing a crash.
            if (!g_logFlushAlways) g_bootFlush = 0;
            // Reported on their OWN flags. These were nested inside the A2C
            // condition, so with A2C off and no variants yet they printed
            // nothing at all - which is indistinguishable from the feature
            // running and doing nothing, and cost a wasted test run.
#if ENABLE_CUTOUT_AA
            if (g_fringeFoliage || g_fringeDraws) {
                sprintf(line, "[fringe] on=%ld passes=%ld buildFails=%ld sharpness=%ld variants=%ld",
                        g_fringeFoliage, g_fringeDraws, g_fringeBuildFails,
                        g_a2cSharpness, g_a2cVariantCount);
                LogLine(line);
            }
            if (g_a2cEnable || g_a2cVariantCount) {
                sprintf(line, "[ssaa] on=%ld multiDraws=%ld buildFails=%ld offsetReg=c%ld samples=%ld",
                        g_ssaaFoliage, g_ssaaDraws, g_ssaaBuildFails,
                        g_ssaaOffsetReg, g_msaaSamples);
                LogLine(line);
                sprintf(line, "[a2c] enable=%ld variants=%ld binds=%ld buildFails=%ld "
                              "noRoom=%ld blockedByBlend=%ld sharpness=%ld (MsaaSamples=%ld)",
                        g_a2cEnable, g_a2cVariantCount, g_a2cBinds, g_a2cBuildFails,
                        g_a2cSkipNoRoom, g_a2cBlockedBlend,
                        g_a2cSharpness, g_msaaSamples);
                LogLine(line);
            }
            // Candidate list for the identify walk: only cutout shaders that
            // are ACTUALLY DRAWN in the scene pass with blending off, ranked
            // by draw count. Grass and barriers are drawn constantly, so they
            // sit near the top - which turns 1344 texkill shaders into a
            // handful worth stepping through by hand.
            for (LONG r = 0; r < g_psRankCount && r < 14; r++) {
                LONG idx = g_psRankIdx[r];
                if (idx < 0 || idx >= PS_MAP_MAX) continue;
                sprintf(line, "[shader] #%ld ps_%08X recent=%ld taps=%u %s%s",
                        r + 1, g_psMap[idx].hash, g_psMap[idx].recent,
                        g_psMap[idx].taps,
                        g_psMap[idx].hasVariant ? "CUTOUT" : "-",
                        (g_psIdentify == r + 1) ? "   <== MAGENTA" : "");
                LogLine(line);
            }
            if (g_fxaaPick || g_fxaaSubs) {
                sprintf(line, "[fxaa] pick=%ld objs=%ld subs=%ld passthrough=%s tint=%s",
                        g_fxaaPick, g_psKillObjCount, g_fxaaSubs,
                        g_fxaaPassthrough ? "ready" : "none",
                        g_psTintObj ? "ready" : "none");
                LogLine(line);
            }
            // Post-chain shader table (discovery instrument, DumpShaders only):
            // every pixel shader that drew onto the scene-sized colour target
            // during the post window, with identity hash + tap count from the
            // creation-time map. minPrim <= 2 marks fullscreen-quad passes -
            // the post-AA MUST be one of those lines.
            if (g_dumpShaders && g_postPsSeenCount) {
                // Truncation must announce itself - see the PS_HASH_MAX note.
                if (g_psMapCount >= PS_MAP_MAX || g_psHashCount >= PS_HASH_MAX)
                    LogLine("[postps] *** CAP HIT: shader map/dump truncated, census NOT complete ***");
                LONG n = g_postPsSeenCount;
                if (n > POST_PS_MAX) n = POST_PS_MAX;
                for (LONG i = 0; i < n; i++) {
                    void *obj = g_postPsSeen[i].obj;
                    DWORD hsh = 0; UINT taps = 0;
                    LONG m = g_psMapCount;
                    if (m > PS_MAP_MAX) m = PS_MAP_MAX;
                    for (LONG j = 0; j < m; j++)
                        if (g_psMap[j].obj == obj) { hsh = g_psMap[j].hash; taps = g_psMap[j].taps; break; }
                    // Pass names as a compact list, and the target format:
                    // fmt 21 = A8R8G8B8 (scene), 22 = X8R8G8B8 (BACKBUFFER -
                    // where a final post-AA writes, and what the old gate
                    // wrongly excluded).
                    char pl[128]; int po = 0; pl[0] = 0;
                    for (int pi = 0; pi < PASS_COUNT; pi++)
                        if (g_postPsSeen[i].passMask & (1L << pi))
                            po += sprintf(pl + po, "%s%s", po ? "," : "", g_passNames[pi]);
                    // taps here is the CREATION-time count from the corrected
                    // walker; >=5 on a quad pass would be the filter shape
                    // the offline census says does not exist at full res.
                    sprintf(line, "[postps] #%ld ps_%08X draws=%ld taps=%u rt=%ldx%ld fmt=%ld%s%s [%s]",
                            i + 1, hsh, g_postPsSeen[i].draws, taps,
                            g_postPsSeen[i].w, g_postPsSeen[i].h, g_postPsSeen[i].fmt,
                            (g_postPsSeen[i].fmt == 22) ? "(BACKBUFFER)" : "",
                            (taps >= 5) ? "  <== MULTI-TAP QUAD (filter shape)" : "", pl);
                    LogLine(line);
                }
            }
#endif  // ENABLE_CUTOUT_AA / shader-diag reporting
            if (g_shadowBufResPct > 0) {
                // viewports_scaled is the number that matters: textures and
                // viewports must move together or the pass renders into the
                // wrong area. If textures=N and viewports=0, that is the exact
                // failure already seen (dark world / blocky shifted shadows).
                sprintf(line, "[shadowbuf] pct=%ld descriptors_scaled=%ld (stock %ux%u)",
                        g_shadowBufResPct, g_sbufCtorScaled,
                        g_backbufW / 2, g_backbufH / 2);
                LogLine(line);
            }
            // zero_deltas is the number that matters: it counts frames where
            // the engine's own delta came out as literally 0, i.e. the failure
            // this fix exists for. If it stays 0 while the fix is on, the game
            // is not actually running above 59.94 and the A/B proves nothing.
            // The limiter spins instead of sleeping whenever the frame's
            // remaining time is below this mark, so compare it against the
            // frame interval: 16.68ms at 59.94, 10.00ms at 100fps. A mark at
            // or above the interval means EVERY frame is a busy-spin.
            {
                LONG fpsX100 = g_targetFpsX100;
                LONG intervalUs = (fpsX100 > 0) ? (LONG)(100000000LL / fpsX100) : 0;
                sprintf(line, "[limiter] sleep granularity high-water = %ld us | "
                              "interval %ld us | guard=%ld us writes=%ld%s",
                        g_lastGranUs, intervalUs, g_spinGuardUs, g_spinGuardWrites,
                        (intervalUs > 0 && g_lastGranUs >= intervalUs)
                            ? "  *** >= INTERVAL: limiter busy-spins every frame ***" : "");
                LogLine(line);
            }
            // Simultaneous-target count per pass. The old "max SetRenderTarget
            // index = 3" was one global number and could not say WHICH stage
            // binds an MRT set - which is the actual forward-vs-deferred
            // evidence.
            if (g_logPassRts) {
                char pl[256];
                int po = sprintf(pl, "[pass] max simultaneous RT index per pass:");
                for (int pi = 0; pi < PASS_COUNT; pi++)
                    po += sprintf(pl + po, " %s=%ld", g_passNames[pi], g_passMaxRtIndex[pi]);
                LogLine(pl);
            }
            if (g_gpuSyncSkip || g_gpuFenceSkipped) {
                sprintf(line, "[gpufence] skip=%ld fences_skipped=%ld | FUN_00ac3040 avg=%.0fus max=%ldus",
                        g_gpuSyncSkip, g_gpuFenceSkipped,
                        g_funcs[FN_AC3040].durationCount
                            ? (double)g_funcs[FN_AC3040].durationSumCycles
                              / (double)g_funcs[FN_AC3040].durationCount
                              / (g_cyclesPerUsec > 0.0 ? g_cyclesPerUsec : 1.0)
                            : 0.0,
                        g_funcs[FN_AC3040].maxUsec);
                LogLine(line);
            }
            if (g_simDeltaFix || g_sdReplaced) {
                sprintf(line, "[simdelta] fix=%ld replaced=%ld zero_deltas_fixed=%ld last %ld -> %ld (5005 = one 59.94 frame)",
                        g_simDeltaFix, g_sdReplaced, g_sdZeroFixed,
                        g_sdLastOrig, g_sdLastNew);
                LogLine(line);
            }
#if ENABLE_TALK_TIMER
            if (g_talkTimerPct > 0 || g_talkStepPatched) {
                sprintf(line, "[talk] timer rate=%ld%% patched=%d step=%.4f/frame "
                              "(stock 0.05) orig_operand=0x%08X",
                        g_talkTimerPct, g_talkStepPatched, g_talkStep,
                        g_talkStepOrigDisp);
                LogLine(line);
            }
#endif
#if ENABLE_SHADOW_SCALE
            if (g_shadowScaled || g_shadowScaleFail) {
                sprintf(line, "[shadow] scaling: applied=%ld failed_fellback=%ld (x%ld) | tracked_surfaces=%ld viewports_scaled=%ld",
                        g_shadowScaled, g_shadowScaleFail, g_shadowScale,
                        g_shadowSurfaceCount, g_viewportScaled);
                LogLine(line);
            }
#endif
#if ENABLE_MANAGED_POOL
            if (g_managedRewriteCount || g_managedRewriteFail) {
                sprintf(line, "[d3d9] MANAGED pool rewrite: applied=%ld failed_reverted=%ld",
                        g_managedRewriteCount, g_managedRewriteFail);
                LogLine(line);
            }
#endif
            {
                LONG pDefault = g_texPoolRequested[0], pManaged = g_texPoolRequested[1];
                LONG pSysmem = g_texPoolRequested[2], pScratch = g_texPoolRequested[3];
                if (pDefault || pManaged || pSysmem || pScratch) {
                    sprintf(line, "[d3d9] CreateTexture pool histogram (cumulative): DEFAULT=%ld MANAGED=%ld SYSTEMMEM=%ld SCRATCH=%ld",
                            pDefault, pManaged, pSysmem, pScratch);
                    LogLine(line);
                }
            }
#if ENABLE_TEXTURE_POOL
            if (g_texPoolHits || g_texPoolMisses) {
                sprintf(line, "[texpool] hits=%ld misses=%ld returned=%ld evicted=%ld pooled=%ld/%d",
                        g_texPoolHits, g_texPoolMisses, g_texPoolReturned, g_texPoolEvicted,
                        g_texPoolCount, TEXPOOL_MAX_ENTRIES);
                LogLine(line);
            }
#endif
            if (g_cubeStagingHits || g_cubeStagingFallback) {
                sprintf(line, "[d3d9] cube staging: hits=%ld fallback=%ld updatefail=%ld inflight=%ld",
                        g_cubeStagingHits, g_cubeStagingFallback, g_cubeStagingUpdateFail,
                        g_inflightCubeCount);
                LogLine(line);
            }
            if (dSL > 0) {
                LONG mx = InterlockedExchange(&g_surfLockMaxUsec, 0);
                sprintf(line, "[d3d9] Surface::LockRect: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dSL, dSLs/dSL, mx, slC);
                LogLine(line);
            }
            if (g_surfStagingHits || g_surfStagingFallback) {
                sprintf(line, "[d3d9] surface staging: hits=%ld fallback=%ld updatefail=%ld inflight=%ld",
                        g_surfStagingHits, g_surfStagingFallback, g_surfStagingUpdateFail,
                        g_inflightSurfCount);
                LogLine(line);
            }
        }

        {
            static LONG prevOpen=0, prevOpenSum=0, prevClose=0, prevCloseSum=0;
            LONG oc=g_openCount, os=g_openSumUsec, dO=oc-prevOpen, dOs=os-prevOpenSum;
            prevOpen=oc; prevOpenSum=os;
            if (dO > 0) {
                LONG mx = InterlockedExchange(&g_openMaxUsec, 0);
                sprintf(line, "[probe] main-thread CreateFileA: calls=%ld total_usec=%ld avg=%ld max=%ld (cum=%ld)",
                        dO, dOs, dOs/dO, mx, oc);
                LogLine(line);
            }
            LONG cc=g_closeCount, cs=g_closeSumUsec, dC=cc-prevClose, dCs=cs-prevCloseSum;
            prevClose=cc; prevCloseSum=cs;
            if (dC > 0) {
                LONG mx = InterlockedExchange(&g_closeMaxUsec, 0);
                sprintf(line, "[probe] main-thread CloseHandle: calls=%ld total_usec=%ld avg=%ld max=%ld (cum=%ld)",
                        dC, dCs, dCs/dC, mx, cc);
                LogLine(line);
            }
            // Dump the worst offenders periodically - this is what identifies
            // WHICH files are expensive, which is the whole point.
            static LONG dumpTick = 0;
            if (g_slowOpenLockState == 2 && ++dumpTick % 20 == 0) {
                EnterCriticalSection(&g_slowOpenLock);
                for (LONG i = 0; i < g_slowOpenCount; i++) {
                    sprintf(line, "[probe]   slow open %ldus: %s", g_slowOpens[i].usec, g_slowOpens[i].path);
                    LogLine(line);
                }
                LeaveCriticalSection(&g_slowOpenLock);
            }
        }

        static LONG prevSleepCount = 0, prevSleepUsec = 0;
        LONG sc2 = g_mainSleepCount, su = g_mainSleepUsec;
        LONG dsc = sc2 - prevSleepCount, dsu = su - prevSleepUsec;
        prevSleepCount = sc2; prevSleepUsec = su;
        if (dsc > 0) {
            LONG smax = InterlockedExchange(&g_mainSleepMaxUsec, 0);
            sprintf(line, "[probe] main-thread Sleep: calls=%ld total_usec=%ld avg_usec=%ld max_usec=%ld",
                    dsc, dsu, dsu / dsc, smax);
            LogLine(line);

            // Ranked call sites (sampled 1:256), converted to Ghidra VAs the
            // same way every other address in this file is
            // (runtime - module base + 0x00400000). The first version logged
            // RAW runtime addresses with a comment claiming they mapped
            // directly onto the listing - they do not, the module is based at
            // 0x00100000 here, and the resulting 0x300000 error pointed at a
            // small destructor that never calls Sleep at all.
            LONG used = g_sleepCallerUsed;
            for (LONG rank = 0; rank < 4; rank++) {
                LONG best = -1, bestCount = 0;
                for (LONG i = 0; i < used; i++) {
                    if (g_sleepCallers[i].count > bestCount) {
                        int already = 0;
                        for (LONG r = 0; r < rank; r++) {
                            // skip ones already printed this window
                            if (g_sleepCallers[i].addr == g_sleepTopPrinted[r]) { already = 1; break; }
                        }
                        if (!already) { best = i; bestCount = g_sleepCallers[i].count; }
                    }
                }
                if (best < 0) break;
                g_sleepTopPrinted[rank] = g_sleepCallers[best].addr;
                sprintf(line, "[probe]   sleep caller #%ld: ghidra=%08lX samples=%ld",
                        rank + 1,
                        (unsigned long)(g_sleepCallers[best].addr - g_mainModBase + 0x00400000),
                        bestCount);
                LogLine(line);
            }
        }
    }

    if (g_stagingHits || g_stagingFallback) {
        sprintf(line, "[d3d9] staging: hits=%ld fallback=%ld updatefail=%ld pool=%ld/%d recycled=%ld inflight=%ld",
                g_stagingHits, g_stagingFallback, g_stagingUpdateFail,
                g_stagingCount, MAX_STAGING, g_stagingRecycled, g_inflightCount);
        LogLine(line);
    }

}

// v12: the export-hook approach (v11) was proven wrong by its own integrity
// check - something (RTSS and/or Steam Overlay, both confirmed present in
// the captured D3D9 stacks) re-patched Direct3DCreate9's export with a JMP
// into its own VirtualAlloc'd trampoline (target address didn't resolve to
// any loaded module), completely replacing our JMP rather than chaining
// through it. Waiting to install "after" it is not reliable - no fixed delay
// can be trusted, and the overlay could equally well install first and get
// overwritten in turn by us or a third party.
//
// The fix is to stop needing to intercept the game's OWN CreateDevice call
// at all. COM vtables are shared per implementation CLASS, not per instance
// - IDirect3DDevice9's Present/CreateTexture/Lock/etc. slots point at the
// same driver code for every HAL device on the same adapter, regardless of
// which caller created it. So instead of racing to catch the game's device,
// this creates a small throwaway HAL device of our own (invisible, windowed,
// on a background thread, never in DllMain - see below for why), reads ITS
// vtable, and patches the slots there. That patches the exact same memory
// the game's real device will use, with no dependency on being first, last,
// or even present in whatever chain of hooks a third-party overlay builds.
// This is the same technique tools like ReShade use for this exact reason.
//
// Must run from a background thread, not DllMain: this ACTUALLY CALLS
// Direct3DCreate9/CreateDevice (unlike the abandoned export-patch approach,
// which only touched memory) and the reference mod's own comment about
// d3d9.dll loading additional DLLs on first init - a real deadlock risk
// against the loader lock DllMain holds - applies directly here.
// Which d3d9.dll actually backs the process: Microsoft's, or a wrapper
// (DXVK, ReShade, a HelixMod shim...) dropped in the game folder. The test is
// the module's own path, not its name - every wrapper is called d3d9.dll by
// necessity, and only the real one lives in System32. The DXVK sub-check is
// informational: the behaviour gates key off "not Microsoft's", because the
// vtable-sharing hazard belongs to any wrapper that implements the whole API
// in one class, not to DXVK specifically.
static void IdentifyD3D9Provider(HMODULE hD3D9)
{
    char modPath[MAX_PATH] = "";
    char sysDir[MAX_PATH] = "";
    if (!GetModuleFileNameA(hD3D9, modPath, MAX_PATH)) {
        LogLine("[d3d9] provider: GetModuleFileName failed - assuming system d3d9.dll");
        return;
    }
    GetSystemDirectoryA(sysDir, MAX_PATH);
    size_t sysLen = strlen(sysDir);
    int inSystem = (sysLen > 0 && _strnicmp(modPath, sysDir, sysLen) == 0);
    if (!inSystem) g_d3d9IsThirdParty = 1;

    // DXVK stamps "DXVK" into its version resource. Deliberately NOT read via
    // GetFileVersionInfo: this module IS version.dll, so those names resolve
    // back into proxy.c (or, worse, produce a self-import) - a needless piece
    // of re-entrancy for a line of log text. The resource is already mapped
    // as part of the module image, so scan that directly, bounded by the PE
    // header's own SizeOfImage.
    if (g_d3d9IsThirdParty) {
        const unsigned char *base = (const unsigned char *)hD3D9;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                DWORD span = nt->OptionalHeader.SizeOfImage;
                const IMAGE_DATA_DIRECTORY *res =
                    &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
                DWORD from = 0;
                if (res->VirtualAddress && res->Size && res->VirtualAddress + res->Size <= span) {
                    from = res->VirtualAddress;      // narrow to .rsrc when we can
                    span = res->VirtualAddress + res->Size;
                }
                // UTF-16LE "DXVK". Non-executable mapped pages, but the walk is
                // guarded anyway - a malformed third-party PE must not be fatal.
                __try {
                    for (DWORD i = from; i + 8 <= span; i++) {
                        if (base[i] == 'D' && base[i + 1] == 0 &&
                            base[i + 2] == 'X' && base[i + 3] == 0 &&
                            base[i + 4] == 'V' && base[i + 5] == 0 &&
                            base[i + 6] == 'K' && base[i + 7] == 0) {
                            g_d3d9IsDxvk = 1;
                            break;
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    LogLine("[d3d9] provider: image scan faulted, DXVK marker undetermined");
                }
            }
        }
    }

    char line[MAX_PATH + 96];
    sprintf(line, "[d3d9] provider: %s (system=%d thirdparty=%ld dxvk=%ld)",
            modPath, inSystem, g_d3d9IsThirdParty, g_d3d9IsDxvk);
    LogLine(line);

    if (g_d3d9IsDxvk) {
        // Not enforced - just stated, because it is a real behavioural fork
        // and silently rewriting the user's config would hide it. The whole
        // staging/DISCARD family exists to route around AMDXN32's blocking
        // lock path (PROGRESS.md, "This explains DXVK completely"), which
        // does not exist once d3d9 calls terminate in Vulkan instead.
        LogLine("[d3d9] DXVK detected: the staging/DISCARD fixes target the native AMD "
                "D3D9 driver stall, which DXVK removes outright - consider StagingUpload/"
                "StagingSurface/StagingCube/DiscardFix=0 under DXVK");
    }
}

static void ProbeD3D9ForVtable(void)
{
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) { LogLine("[d3d9] probe: d3d9.dll not found"); return; }

    IdentifyD3D9Provider(hD3D9);

    // Found via an actual WER crash dump (2026-08-07): ACCESS_VIOLATION
    // (0xC0000005) inside vulkan-1.dll, early in mod startup, reproducible
    // under DXVK with mod+DXVK running. The throwaway device below is
    // "harmless" ONLY because Microsoft's d3d9.dll hands the probe a cheap,
    // genuinely separate software-VP device - true for System32's DLL,
    // never verified for a wrapper. Under DXVK there is no such thing: this
    // call creates a SECOND real, independent Vulkan-backed device (own
    // shader-compiler threads, own swapchain) racing the game's own real
    // device through the same DXVK/Vulkan-loader machinery in one process -
    // a configuration DXVK was never designed to see, and evidently doesn't
    // tolerate. ProbeDeviceThunks (the earlier, narrower gate on installing
    // thunks on this device's vtable) does not help here: it only gates what
    // happens AFTER this device already exists, not whether creating it in
    // the first place is safe - which is the part that was actually
    // crashing. Skip creating it outright under any third-party d3d9.dll;
    // HookRealDevicePresent below sources the resource-vtable hooks (the
    // actual staging fix) from the game's own real device instead, so
    // nothing functional is lost.
    // ...and the same applies to a device-WRAPPING mod, which the
    // third-party-d3d9 test above cannot see. The HD GUI mod is a version.dll:
    // d3d9.dll really is System32's (the log says system=1 thirdparty=0), so
    // that gate never fires, yet the mod wraps EVERY device CreateDevice
    // returns - including this throwaway one. That is precisely the
    // "never verified for a wrapper" case the comment above flags.
    //
    // Evidence it is not theoretical (2026-08-13): 7 crashes in 21 launches,
    // every one an access violation inside VERSION.dll, at two different
    // sites. Diffing crashed against working runs showed the difference is
    // WHICH DEVICE IS CREATED FIRST - working runs log this 4x4 probe device,
    // crashed runs have the game's real device ahead of it. A race between our
    // deferred thread and the game's main thread, through a wrapper that is
    // hooking CreateDevice at the same moment.
    //
    // That also explains what nothing else could: why the failure rate moved
    // with UNRELATED I/O changes (per-line log flushing, a config write, the
    // HD mod's texture dumping) - all of them shift thread timing - while
    // HDTexPush, ForceStdD3D9 and the staging redirects made no difference at
    // all, because none of them touch the race.
    //
    // Nothing functional is lost: HookRealDevicePresent sources the
    // resource-vtable hooks from the game's own real device.
    // Re-checked HERE rather than trusting g_hdTexNotify alone: that flag is
    // set by DetectHdTexInterop earlier on this thread, and whether version.dll
    // was loaded at THAT moment is itself timing-dependent - exactly the kind
    // of assumption this whole investigation was built on and burned by.
    // Asking the loader directly, at the point of use, cannot be stale.
    // Detected by WHERE version.dll loaded from, not by an export.
    //
    // The first version of this guard asked for HDTex_InteropVersion, which
    // only the interop-enabled BUILD of the HD GUI mod exports. That build
    // exists on this machine; the PUBLIC one does not export it (verified:
    // the symbol is present in the deployed version.dll and absent from
    // version.dll.bak-20260628, the stock release). So on any normal user's
    // install the guard would not fire, the probe device would be created,
    // and the exact race this whole fix exists to prevent would come back -
    // a release blocker that would have shipped invisibly, because the only
    // machine it was ever tested on is the one that cannot reproduce it.
    //
    // What actually matters is not "is this the HD GUI mod" but "is something
    // proxying a system DLL from the game directory", because that is what
    // wraps the device. A version.dll resolved from anywhere other than the
    // system directory is by definition a proxy - the same test
    // IdentifyD3D9Provider already applies to d3d9.dll. This also covers
    // wrappers we have never heard of, at no cost.
    int wrapperPresent = 0;
    {
        HMODULE hv = GetModuleHandleA("version.dll");
        if (hv) {
            char modPath[MAX_PATH] = "", sysDir[MAX_PATH] = "";
            if (GetModuleFileNameA(hv, modPath, MAX_PATH)) {
                GetSystemDirectoryA(sysDir, MAX_PATH);
                size_t sysLen = strlen(sysDir);
                // Unknown path is treated as a proxy: skipping the probe costs
                // nothing (HookRealDevicePresent covers the same ground), while
                // guessing wrong the other way crashes the game on startup.
                if (sysLen == 0 || _strnicmp(modPath, sysDir, sysLen) != 0)
                    wrapperPresent = 1;
            } else {
                wrapperPresent = 1;
            }
        }
    }
    if (g_d3d9IsThirdParty || g_hdTexNotify || wrapperPresent) {
        LogLine(g_d3d9IsThirdParty
                ? "[d3d9] probe SKIPPED (third-party d3d9.dll) - a second device here "
                  "is what crashed DXVK; resource hooks now come from the game's own "
                  "real device via HookRealDevicePresent"
                : "[d3d9] probe SKIPPED (device-wrapping mod present) - a second device "
                  "racing the game's own through the wrapper is what crashes it; "
                  "resource hooks come from the real device via HookRealDevicePresent");
        return;
    }

    typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);
    PFN_Direct3DCreate9 create9 = (PFN_Direct3DCreate9)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!create9) { LogLine("[d3d9] probe: Direct3DCreate9 export not found"); return; }

    IDirect3D9 *d3d = create9(D3D_SDK_VERSION);
    if (!d3d) { LogLine("[d3d9] probe: Direct3DCreate9 returned NULL"); return; }

    // GetDesktopWindow() as the focus window and Windowed=TRUE means this
    // never touches exclusive-fullscreen mode or the game's own window/device
    // in any way - a fully independent, throwaway object that exists only
    // long enough to read its vtable.
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 4;
    pp.BackBufferHeight = 4;

    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                         &pp, &dev);
    if (FAILED(hr) || !dev) {
        char line[128];
        sprintf(line, "[d3d9] probe: throwaway CreateDevice failed hr=0x%08lX", (unsigned long)hr);
        LogLine(line);
        IDirect3D9_Release(d3d);
        return;
    }

    LogLine("[d3d9] probe: throwaway HAL device created, patching shared vtable");
    HookDeviceVtable(dev);

    // Safe to tear down immediately: the vtable itself lives in the driver's
    // static data, not in this instance, so releasing the object we used to
    // reach it does not undo the patch.
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
}

// ---- Shader identity capture (v19) -----------------------------------------
// v18 confirmed the shader-compile queue costs real time (up to 22ms/call)
// and correlates with roughly half of the worst frames. Open question from
// there: is the same shader recompiling on every reproducible crossing
// (a bounded cache getting evicted between visits), or is a genuinely
// different shader variant/permutation needed each time (feature-combination
// explosion)? Those have different implications and this distinguishes them
// directly - track every compiled shader's identity (the tag argument
// FUN_00a957a0 passes into the renderer's create-shader call) and flag
// whenever the SAME identity compiles more than once.
//
// FUN_00a957a0 (the shader object constructor) has an SEH prologue
// (PUSH -1; PUSH <handler>; MOV EAX,FS:[0], confirmed via disassembly - the
// same pattern that crashed the return-hijack hooks on FN_A01A00/FN_A015B0
// early in this investigation). Return-address hijacking is NOT used here -
// this only needs to READ one incoming argument, not time the call, so the
// detour reads it and jumps straight to a trampoline of the stolen bytes
// without ever touching the return address. The function's own SEH setup
// therefore executes exactly as it would unpatched, just relocated into the
// trampoline - the specific failure mode that broke the two SEH functions
// (hijacking mid-unwind) cannot occur when the return path is never touched.
// v19 result: total=4349 compiles in one session blew straight through a
// 1024-slot table (distinct saturated at exactly 1024), meaning most of the
// session's activity went untracked for repeat-detection after that point -
// all 18 confirmed repeats were also seenCount=2 at last_seen=0.0s (the same
// instant, not "revisited a minute later"), which isn't yet evidence either
// way on the actual question. Bumped generously: even 16384 entries is
// ~196KB, trivial, and a linear scan over that per (rare, multi-millisecond)
// compile call is negligible next to the cost being measured.
#define MAX_SHADER_IDS 16384
typedef struct {
    DWORD id;
    LONG lastSeenUsec;
    LONG seenCount;
    DWORD contentSum; // checksum of bytes AT the id pointer, not the pointer itself
    int contentValid;
} ShaderIdEntry;
static ShaderIdEntry g_shaderIds[MAX_SHADER_IDS];
static CRITICAL_SECTION g_shaderIdLock;

static void *g_trampoline_a957a0_observe = NULL;

// id is used directly as a pointer elsewhere in the real function (passed to
// a "create shader from bytecode" virtual call), so it may be a pointer into
// a reused scratch/pool buffer rather than a stable per-shader identity - two
// DIFFERENT shaders could coincidentally get the same address if one is freed
// and another allocated in its place between calls, which would make a raw
// pointer-value "repeat" a false positive. This checksums the actual bytes
// AT that address (bounded, SEH-guarded read - id is untrusted, could be
// unmapped or not really a pointer in some code path) so a "repeat" can be
// confirmed as the same CONTENT, not just the same numeric value.
static DWORD SafeChecksum(DWORD id, int *validOut)
{
    DWORD sum = 0;
    *validOut = 0;
    __try {
        volatile const unsigned char *p = (volatile const unsigned char *)(UINT_PTR)id;
        for (int i = 0; i < 32; i++) {
            sum = (sum << 1 | sum >> 31) ^ p[i]; // rotate-xor, order-sensitive
        }
        *validOut = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sum = 0;
    }
    return sum;
}

static void __cdecl OnShaderCreate_C(DWORD id)
{
    InterlockedIncrement(&g_shaderCompileTotal);
    LONG now = NowUsec();
    int contentValid = 0;
    DWORD contentSum = SafeChecksum(id, &contentValid);

    EnterCriticalSection(&g_shaderIdLock);
    LONG n = g_shaderIdCount;
    int found = -1;
    for (LONG i = 0; i < n; i++) {
        if (g_shaderIds[i].id == id) { found = i; break; }
    }
    if (found >= 0) {
        LONG agoUsec = now - g_shaderIds[found].lastSeenUsec;
        int prevValid = g_shaderIds[found].contentValid;
        DWORD prevSum = g_shaderIds[found].contentSum;
        g_shaderIds[found].lastSeenUsec = now;
        g_shaderIds[found].seenCount++;
        g_shaderIds[found].contentSum = contentSum;
        g_shaderIds[found].contentValid = contentValid;
        LONG seenCount = g_shaderIds[found].seenCount;
        LeaveCriticalSection(&g_shaderIdLock);
        InterlockedIncrement(&g_shaderCompileRepeats);
        const char *verdict = (!prevValid || !contentValid) ? "UNVERIFIED(unreadable)"
                             : (prevSum == contentSum) ? "CONTENT_MATCH(genuine repeat)"
                             : "CONTENT_DIFFERS(address reuse, NOT a real repeat)";
        char line[224];
        sprintf(line, "[shader] REPEAT compile id=%08lX seenCount=%ld last_seen=%.1fs_ago %s",
                (unsigned long)id, seenCount, (double)agoUsec / 1000000.0, verdict);
        LogLine(line);
    } else {
        if (n < MAX_SHADER_IDS) {
            g_shaderIds[n].id = id;
            g_shaderIds[n].lastSeenUsec = now;
            g_shaderIds[n].seenCount = 1;
            g_shaderIds[n].contentSum = contentSum;
            g_shaderIds[n].contentValid = contentValid;
            g_shaderIdCount = n + 1;
        }
        LeaveCriticalSection(&g_shaderIdLock);
    }
}

// Entry-only: reads param_2 (the shader identity/tag), then jumps straight
// to the trampoline. No return-address hijack, no timing - deliberately as
// close to a no-op as possible given the SEH prologue this sits in front of.
__declspec(naked) void Detour_a957a0_observe(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 16]     ; original [esp+8] = param_2, before our 2 pushes
        push eax
        call OnShaderCreate_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a957a0_observe]
    }
}

#define SHADER_CREATE_RVA (0x00a957a0 - 0x00400000)
#define SHADER_CREATE_PATCH_LEN 5 // PUSH EBP; MOV EBP,ESP; PUSH -1 - exactly 3 whole instructions

static int InstallShaderIdentityHook(void)
{
    InitializeCriticalSection(&g_shaderIdLock);

    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = base + SHADER_CREATE_RVA;

    unsigned char saved[SHADER_CREATE_PATCH_LEN];
    memcpy(saved, target, SHADER_CREATE_PATCH_LEN);

    unsigned char *tramp = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;
    memcpy(tramp, saved, SHADER_CREATE_PATCH_LEN);
    tramp[SHADER_CREATE_PATCH_LEN] = 0xE9;
    *(int *)(tramp + SHADER_CREATE_PATCH_LEN + 1) =
        (int)(target + SHADER_CREATE_PATCH_LEN) - (int)(tramp + SHADER_CREATE_PATCH_LEN + 5);
    g_trampoline_a957a0_observe = tramp;

    DWORD oldProtect;
    if (!VirtualProtect(target, SHADER_CREATE_PATCH_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;
    target[0] = 0xE9;
    *(int *)(target + 1) = (int)(void *)Detour_a957a0_observe - (int)(target + 5);
    VirtualProtect(target, SHADER_CREATE_PATCH_LEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, SHADER_CREATE_PATCH_LEN);
    return 1;
}

// ---- ForceImmediatePresent: intercept the game's OWN Direct3DCreate9 ------
//
// Investigation (see PROGRESS.md, "CONFIRMED: the 60Hz clustering is vsync")
// found the frame-time distribution clustered sharply on multiples of
// 16.7ms rather than the smooth curve a CPU-bound hitch produces, and a
// one-shot diagnostic (via GetDevice() on a real game texture, since the
// device-vtable probe below does not reach the real device - see the
// "Methodological finding" section) confirmed the real swap chain requests
// PresentationInterval=D3DPRESENT_INTERVAL_DEFAULT (0x0), Windowed=1. The
// D3D9 spec defines DEFAULT as behaving identically to INTERVAL_ONE (vsync).
// AMD Adrenalin's vsync-off setting not helping is consistent with this:
// that control has historically applied to exclusive-fullscreen
// presentation, not windowed, where the DWM compositor paces frames instead.
//
// Confirmed via LRFF13.exe's own import table that it imports plain
// `Direct3DCreate9` from d3d9.dll (not Ex) - so, unlike the abandoned
// Direct3DCreate9 EXPORT-hook approach (v11, defeated by RTSS/Steam overlay
// re-patching the export - see the v12 comment on ProbeD3D9ForVtable), this
// uses PatchIat() on the GAME'S OWN IAT entry for Direct3DCreate9. That is a
// private copy in this process's own import table, not a shared resource
// overlays fight over, so it sidesteps that problem entirely. It also means
// this does NOT depend on the device-vtable-sharing question that broke the
// Present diagnostic: this intercepts device CREATION itself, before any
// vtable-sharing assumption could matter.
//
// Real risk, not just a performance knob: engines from this console-port era
// often tie internal timing (animation speed, physics tick, cutscene sync)
// to an assumed fixed-cadence Present. Forcibly removing that cadence can
// desync those systems in ways much harder to notice than a dropped frame.
// Default OFF, one config key (F5 / ForceImmediatePresent), instantly
// revertible - same discipline as every other risky fix in this file.
typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_IDirect3D9CreateDevice)(
    IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);

static PFN_Direct3DCreate9 g_realDirect3DCreate9 = NULL;
static PFN_IDirect3D9CreateDevice g_origIDirect3D9CreateDevice = NULL;
static volatile LONG g_createDeviceHookInstalled = 0;
static volatile LONG g_forceImmediateApplied = 0;
static volatile LONG g_msaaSurveyed = 0;   // capability survey runs once

static HRESULT STDMETHODCALLTYPE HookedIDirect3D9CreateDevice(
    IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    if (g_forceImmediatePresentEnabled && pPP &&
        pPP->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        char line[160];
        sprintf(line, "[d3d9] ForceImmediatePresent: rewriting PresentationInterval 0x%lX -> IMMEDIATE (Windowed=%d)",
                (unsigned long)pPP->PresentationInterval, pPP->Windowed);
        LogLine(line);
        pPP->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        InterlockedIncrement(&g_forceImmediateApplied);
    }
    // Does the game's OWN code ask for MSAA? XIII-1/XIII-2 reportedly force
    // it (their transparency effects depend on it), so whether this engine
    // still requests it here is direct evidence about whether an MSAA path
    // exists in the renderer at all.
    if (pPP) {
        // Presentation size captured HERE as well as at Reset. It used to come
        // only from Reset, which is wrong whenever the device is created at its
        // final size and never Reset - a real, observed case (one session
        // logged zero Resets). g_backbufW then stayed 0, which made the [ssaa]
        // status line report "not supersampling" regardless of the truth, and
        // left anything keyed off the presented size working from nothing.
        if (pPP->BackBufferWidth && pPP->BackBufferHeight) {
            g_backbufW = pPP->BackBufferWidth;
            g_backbufH = pPP->BackBufferHeight;
        }
        if (pPP->hDeviceWindow) g_gameHwnd = pPP->hDeviceWindow;
        char l[192];
        sprintf(l, "[rt] CreateDevice: %ux%u backbufFmt=%d MULTISAMPLE=%d qual=%lu autoDepth=%d depthFmt=%d windowed=%d",
                pPP->BackBufferWidth, pPP->BackBufferHeight, (int)pPP->BackBufferFormat,
                (int)pPP->MultiSampleType, (unsigned long)pPP->MultiSampleQuality,
                (int)pPP->EnableAutoDepthStencil, (int)pPP->AutoDepthStencilFormat,
                (int)pPP->Windowed);
        LogLine(l);
    }
    // ---- MSAA capability survey ------------------------------------------
    // Bounds the whole MSAA question before any work is spent on it. Runs once
    // at device creation, where we hold the IDirect3D9 the game is actually
    // using - so the answer covers whatever backs d3d9.dll (Microsoft, DXVK, a
    // wrapper), not a probe device that might differ.
    //
    // Why it is worth asking at all: the "deferred renderer, MSAA impossible"
    // verdict was a measurement bug - `max SetRenderTarget index = 3` counted
    // SetRenderTarget(n, NULL), i.e. slots being CLEARED. Corrected, real
    // multi-target binds are ZERO: this is a single-target FORWARD renderer,
    // which is the configuration where MSAA is normally viable.
    //
    // Surveys the formats the frame graph actually uses: A8R8G8B8 (the scene
    // colour target MULTI_SAMPLE renders into), D24S8 (its depth), and R32F
    // (the linear-depth companion). R32F matters because if the engine samples
    // it per-pixel, an MSAA scene target needs a resolve for it too.
    if (!g_msaaSurveyed) {
        g_msaaSurveyed = 1;
        struct { D3DFORMAT fmt; const char *name; } fmts[] = {
            { D3DFMT_A8R8G8B8, "A8R8G8B8 (scene colour)" },
            { D3DFMT_D24S8,    "D24S8 (depth)" },
            { D3DFMT_R32F,     "R32F (linear depth)" },
        };
        D3DMULTISAMPLE_TYPE lv[] = { D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES,
                                     D3DMULTISAMPLE_8_SAMPLES };
        for (int fi = 0; fi < 3; fi++) {
            char l[256];
            int o = sprintf(l, "[msaa] %-26s :", fmts[fi].name);
            for (int li = 0; li < 3; li++) {
                DWORD q = 0;
                HRESULT chr = IDirect3D9_CheckDeviceMultiSampleType(
                    This, Adapter, DeviceType, fmts[fi].fmt,
                    pPP ? pPP->Windowed : TRUE, lv[li], &q);
                o += sprintf(l + o, "  x%d=%s", 2 << li,
                             SUCCEEDED(chr) ? "YES" : "no");
                if (SUCCEEDED(chr)) o += sprintf(l + o, "(q=%lu)", (unsigned long)q);
            }
            LogLine(l);
        }
        LogLine("[msaa] NOTE: support here only means the DRIVER can create such a surface. "
                "D3D9 still cannot SAMPLE a multisampled surface - the engine renders the "
                "scene to a texture and samples it in post, so a StretchRect resolve would "
                "have to be injected regardless.");
    }

    HRESULT hr = g_origIDirect3D9CreateDevice(This, Adapter, DeviceType, hFocusWindow,
                                              BehaviorFlags, pPP, ppReturnedDeviceInterface);
    // Until ForceStdD3D9 this path was believed dead (two prior test runs
    // never saw it fire) and was left as ForceImmediatePresent-only. It is
    // no longer provably dead, so it must install everything the Ex path
    // does - HookRealDevicePresent is the single entry point for staging,
    // DISCARD, CreateTexture/ManagedPool, shader timing, all of it. Without
    // this call, ForceStdD3D9 would boot with only the presentation-interval
    // rewrite active and every other fix silently inert - the same "load
    // path fine, other path missing" bug class this project keeps finding,
    // caught here before a test run rather than after one.
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookRealDevicePresent(*ppReturnedDeviceInterface);
    }
    return hr;
}

// IDirect3D9::CreateDevice is vtable slot 16 (QueryInterface/AddRef/Release
// = 0-2, then 13 IDirect3D9-specific methods before CreateDevice).
#define IDIRECT3D9_VTBL_SLOT_CREATEDEVICE 16

static void InstallCreateDeviceHookOn(IDirect3D9 *d3d)
{
    if (InterlockedCompareExchange(&g_createDeviceHookInstalled, 1, 0) != 0) return;
    void **vtbl = *(void ***)d3d;
    g_origIDirect3D9CreateDevice = (PFN_IDirect3D9CreateDevice)vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE];
    DWORD oldProtect;
    if (VirtualProtect(&vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE] = (void *)HookedIDirect3D9CreateDevice;
        VirtualProtect(&vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE], sizeof(void *), oldProtect, &oldProtect);
        LogLine("[d3d9] IDirect3D9::CreateDevice vtable hook installed (ForceImmediatePresent path)");
    } else {
        g_createDeviceHookInstalled = 0;
        LogLine("[d3d9] IDirect3D9::CreateDevice vtable hook FAILED (VirtualProtect)");
    }
}

// This fires only for calls through the GAME's own compiled
// call-through-IAT instruction. ProbeD3D9ForVtable below resolves
// Direct3DCreate9 itself via GetProcAddress and calls that pointer directly,
// bypassing the exe's IAT entirely - so the two paths are independent, and
// the probe's own throwaway device creation never touches this hook at all.
static IDirect3D9 *WINAPI HookedDirect3DCreate9(UINT SDKVersion)
{
    IDirect3D9 *real = g_realDirect3DCreate9(SDKVersion);
    // This firing at all is the entire result of the ForceStdD3D9 experiment:
    // it means the game's decade-dormant non-Ex fallback path is alive. If
    // ForceStdD3D9 is on and this line never appears in the log, the fallback
    // does not exist and the approach is dead regardless of anything else.
    if (g_forceStdD3D9) {
        char l[128];
        sprintf(l, "[d3d9] ForceStdD3D9: plain Direct3DCreate9 CALLED - fallback path is alive (result=%s)",
                real ? "ok" : "NULL");
        LogLine(l);
    }
    if (real) InstallCreateDeviceHookOn(real);
    return real;
}

// ---- Direct3DCreate9Ex path ----------------------------------------------
//
// Two test runs proved the game does NOT reach d3d9 through the exe's static
// `Direct3DCreate9` import: the IAT hook installed (first from the deferred
// thread, then from DllMain, ruling out a startup race) and never fired once,
// while the device demonstrably existed. The exe imports `LoadLibraryA` and
// `GetProcAddress`, and does NOT statically import `Direct3DCreate9Ex` at all
// - so whichever entry point it uses is resolved dynamically at runtime.
//
// Hooking GetProcAddress in the exe's own IAT catches that resolution
// regardless of which name is requested, and does it without patching the
// bytes of a shared d3d9.dll export - deliberately avoiding the hook war that
// defeated the v11 export-patch approach (RTSS/Steam Overlay re-patching
// Direct3DCreate9 and replacing our JMP outright). An IAT entry is this
// process's own private table; nothing else contends for it.
//
// IDirect3D9Ex vtable layout: IDirect3D9 occupies slots 0-16 (CreateDevice is
// 16), and IDirect3D9Ex appends GetAdapterModeCountEx(17),
// EnumAdapterModesEx(18), GetAdapterDisplayModeEx(19), CreateDeviceEx(20).
#define IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX 20

typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_IDirect3D9CreateDeviceEx)(
    IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
    D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **);

static PFN_Direct3DCreate9Ex g_realDirect3DCreate9Ex = NULL;
static PFN_IDirect3D9CreateDeviceEx g_origCreateDeviceEx = NULL;
static volatile LONG g_createDeviceExHookInstalled = 0;

static HRESULT STDMETHODCALLTYPE HookedIDirect3D9CreateDeviceEx(
    IDirect3D9Ex *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
    D3DDISPLAYMODEEX *pFullscreenDisplayMode, IDirect3DDevice9Ex **ppReturnedDeviceInterface)
{
    if (g_forceImmediatePresentEnabled && pPP &&
        pPP->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        char line[176];
        sprintf(line, "[d3d9] ForceImmediatePresent: CreateDeviceEx rewriting PresentationInterval 0x%lX -> IMMEDIATE (Windowed=%d)",
                (unsigned long)pPP->PresentationInterval, pPP->Windowed);
        LogLine(line);
        pPP->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        InterlockedIncrement(&g_forceImmediateApplied);
    }
    HRESULT hr = g_origCreateDeviceEx(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                      pPP, pFullscreenDisplayMode, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookRealDevicePresent((IDirect3DDevice9 *)*ppReturnedDeviceInterface);
    }
    return hr;
}

static void InstallCreateDeviceExHookOn(IDirect3D9Ex *d3dEx)
{
    if (InterlockedCompareExchange(&g_createDeviceExHookInstalled, 1, 0) != 0) return;
    void **vtbl = *(void ***)d3dEx;
    g_origCreateDeviceEx = (PFN_IDirect3D9CreateDeviceEx)vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX];
    DWORD oldProtect;
    if (VirtualProtect(&vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX] = (void *)HookedIDirect3D9CreateDeviceEx;
        VirtualProtect(&vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX], sizeof(void *), oldProtect, &oldProtect);
        LogLine("[d3d9] IDirect3D9Ex::CreateDeviceEx vtable hook installed (ForceImmediatePresent path)");
    } else {
        g_createDeviceExHookInstalled = 0;
        LogLine("[d3d9] IDirect3D9Ex::CreateDeviceEx vtable hook FAILED (VirtualProtect)");
    }
}

static HRESULT WINAPI HookedDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D)
{
    HRESULT hr = g_realDirect3DCreate9Ex(SDKVersion, ppD3D);
    if (SUCCEEDED(hr) && ppD3D && *ppD3D) InstallCreateDeviceExHookOn(*ppD3D);
    return hr;
}

static FARPROC (WINAPI *g_realGetProcAddress)(HMODULE, LPCSTR) = NULL;

static FARPROC WINAPI HookedGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC real = g_realGetProcAddress(hModule, lpProcName);
    // Ordinal lookups pass a small integer in the pointer, not a string -
    // dereferencing that would fault.
    if (real && lpProcName && (ULONG_PTR)lpProcName > 0xFFFF) {
        if (strcmp(lpProcName, "Direct3DCreate9Ex") == 0) {
            g_realDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)real;
            if (g_forceStdD3D9) {
                // Report the symbol as missing so the game takes the same
                // path it would on an OS without D3D9Ex. Its plain
                // Direct3DCreate9 import is already patched (and already
                // proven present), so if a fallback exists at all we will
                // see HookedDirect3DCreate9 fire right after this line.
                LogLine("[d3d9] ForceStdD3D9: hiding Direct3DCreate9Ex - expecting plain D3D9 fallback");
                return NULL;
            }
            LogLine("[d3d9] game resolved Direct3DCreate9Ex via GetProcAddress - returning wrapper");
            return (FARPROC)HookedDirect3DCreate9Ex;
        }
        if (strcmp(lpProcName, "Direct3DCreate9") == 0) {
            g_realDirect3DCreate9 = (PFN_Direct3DCreate9)real;
            LogLine("[d3d9] game resolved Direct3DCreate9 via GetProcAddress - returning wrapper");
            return (FARPROC)HookedDirect3DCreate9;
        }
    }
    return real;
}

// Idempotent: called from DllMain (where it must succeed, to beat the game's
// own Direct3DCreate9 call) and again from the deferred install thread as a
// fallback in case the early attempt ever fails.
//
// Deliberately does NOT require d3d9.dll to be loaded. All this needs is the
// exe's own import table entry, which the loader has already snapped before
// any DllMain runs, and PatchIat reads the current value straight out of it -
// so there is no dependency on d3d9.dll's module handle, and no
// GetProcAddress call against another module from inside the loader lock.

static void InstallForceImmediatePresentHook(void)
{
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) != 0) return;

    HMODULE hExe = GetModuleHandleA(NULL);
    if (!hExe) {
        installed = 0;
        LogLine("[d3d9] ForceImmediatePresent: exe module handle missing, skipped");
        return;
    }
    // Kept for completeness in case a static-import path is ever used, but
    // two test runs proved the game does not reach d3d9 this way.
    void *realFn = PatchIat(hExe, "d3d9.dll", "Direct3DCreate9", (void *)HookedDirect3DCreate9);
    if (realFn) {
        g_realDirect3DCreate9 = (PFN_Direct3DCreate9)realFn;
        LogLine("[d3d9] Direct3DCreate9 IAT hook installed (game exe's own import table)");
    } else {
        LogLine("[d3d9] Direct3DCreate9 IAT hook not applied (import not found in exe)");
    }

    void *realCf = PatchIat(hExe, "kernel32.dll", "CreateFileA", (void *)HookedCreateFileA);
    if (realCf) {
        g_realCreateFileA = (PFN_CreateFileA)realCf;
        LogLine("[probe] CreateFileA IAT hook installed (main-thread file-open probe)");
    }
    void *realCh = PatchIat(hExe, "kernel32.dll", "CloseHandle", (void *)HookedCloseHandle);
    if (realCh) g_realCloseHandle = (PFN_CloseHandle)realCh;

    void *realSleep = PatchIat(hExe, "kernel32.dll", "Sleep", (void *)HookedSleep);
    if (realSleep) {
        g_realSleep = (PFN_Sleep)realSleep;
        LogLine("[probe] Sleep IAT hook installed (main-thread frame-limiter probe)");
    }

    // The one that actually matters: the game resolves its d3d9 entry point
    // dynamically, so intercept the resolution itself.
    void *realGpa = PatchIat(hExe, "kernel32.dll", "GetProcAddress", (void *)HookedGetProcAddress);
    if (realGpa) {
        g_realGetProcAddress = (FARPROC (WINAPI *)(HMODULE, LPCSTR))realGpa;
        LogLine("[d3d9] GetProcAddress IAT hook installed - will intercept dynamic d3d9 resolution");
    } else {
        installed = 0;   // allow the deferred-thread fallback to retry
        LogLine("[d3d9] GetProcAddress IAT hook FAILED (import not found in exe)");
    }
}

// ---- Crash reporter -------------------------------------------------------
// Replaces LogFlush=1 as the way to find out where a startup crash happens.
// Flushing every line DID make the last line truthful, but it also put ~40
// synchronous disk writes into the boot window, from several threads, inside
// a lock the main thread takes - and the intermittent startup failure got
// dramatically WORSE with it on. That is the observer effect this file already
// documents twice; an instrument that changes the failure rate is not
// measuring the failure.
//
// This costs nothing until the process actually dies: no I/O, no locks, no
// per-line work. When it does die it records the exception code, the faulting
// address, and - the part that actually narrows the search - WHICH MODULE the
// address belongs to. "Crashed in version.dll" and "crashed in LRFF13.exe" are
// completely different investigations, and right now we cannot tell them
// apart.
//
// Chains to whatever filter was already installed (the HD GUI mod may have its
// own) rather than replacing it, and only reports once.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter = NULL;
static volatile LONG g_crashReported = 0;

static void LogFlushNow(void)
{
    if (g_logFile && g_logLockState == 2) {
        EnterCriticalSection(&g_logLock);
        fflush(g_logFile);
        LeaveCriticalSection(&g_logLock);
    }
}

static LONG WINAPI ModCrashFilter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord &&
        InterlockedCompareExchange(&g_crashReported, 1, 0) == 0) {
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        char modPath[MAX_PATH];
        HMODULE hm = NULL;
        DWORD off = 0;
        modPath[0] = '\0';
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &hm) && hm) {
            GetModuleFileNameA(hm, modPath, MAX_PATH);
            off = (DWORD)((unsigned char *)addr - (unsigned char *)hm);
        }
        {
            const char *name = modPath;
            const char *slash = strrchr(modPath, '\\');
            if (slash) name = slash + 1;
            char l[400];
            sprintf(l, "[crash] code=0x%08lX addr=%p in %s+0x%lX thread=%lu",
                    (unsigned long)ep->ExceptionRecord->ExceptionCode, addr,
                    modPath[0] ? name : "<unknown module>",
                    (unsigned long)off, (unsigned long)GetCurrentThreadId());
            LogLine(l);
        }
        LogFlushNow();
    }
    return g_prevUnhandledFilter ? g_prevUnhandledFilter(ep)
                                 : EXCEPTION_CONTINUE_SEARCH;
}

// The unhandled-exception filter above caught NOTHING on a reproduced crash -
// no [crash] line at all. That is itself the finding: the process is not dying
// from an ordinary unhandled exception. Things that bypass that filter:
//
//   - /GS stack-cookie failure (STATUS_STACK_BUFFER_OVERRUN 0xC0000409), which
//     fail-fasts. This project has already lost days to exactly that once, in
//     LoadConfig's char[300].
//   - CRT abort() / invalid-parameter handler.
//   - Someone else's handler catching it and calling ExitProcess.
//   - TerminateProcess from outside.
//
// A VECTORED handler runs before any of that, on the FIRST chance, so it sees
// the exception whatever happens to it afterwards. Registered "first" (1) so
// it also beats any handler the HD GUI mod installs.
//
// Filtered to fatal codes only, and reports once. First-chance handlers see
// every benign C++ throw in the process, and logging those would be both
// enormous and - given what per-line flushing already did to the failure rate -
// actively harmful.
static PVOID g_crashVeh = NULL;

static LONG CALLBACK ModCrashVeh(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        int fatal = (code == 0xC0000005u)   /* access violation        */
                 || (code == 0xC0000409u)   /* stack buffer overrun    */
                 || (code == 0xC00000FDu)   /* stack overflow          */
                 || (code == 0xC000001Du)   /* illegal instruction     */
                 || (code == 0xC0000094u)   /* integer divide by zero  */
                 || (code == 0xC0000096u);  /* privileged instruction  */
        if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedCompareExchange(&g_crashReported, 1, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;

        void *addr = ep->ExceptionRecord->ExceptionAddress;
        char modPath[MAX_PATH];
        HMODULE hm = NULL;
        DWORD off = 0;
        modPath[0] = '\0';
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &hm) && hm) {
            GetModuleFileNameA(hm, modPath, MAX_PATH);
            off = (DWORD)((unsigned char *)addr - (unsigned char *)hm);
        }
        {
            const char *name = modPath;
            const char *slash = strrchr(modPath, '\\');
            if (slash) name = slash + 1;
            char l[400];
            sprintf(l, "[crash] FIRST-CHANCE code=0x%08lX addr=%p in %s+0x%lX thread=%lu",
                    (unsigned long)code, addr,
                    modPath[0] ? name : "<unknown module>",
                    (unsigned long)off, (unsigned long)GetCurrentThreadId());
            LogLine(l);
        }
        LogFlushNow();
    }
    return EXCEPTION_CONTINUE_SEARCH;   // never swallow it
}

void InstallEarlyHooks(void)
{
    // Both installed first, before anything else can fault. Function calls
    // only - no allocation, no I/O - so they are safe on the DllMain path.
    g_prevUnhandledFilter = SetUnhandledExceptionFilter(ModCrashFilter);
    g_crashVeh = AddVectoredExceptionHandler(1, ModCrashVeh);
    // The config must be read here too, not just on the deferred thread: the
    // flag is consulted inside HookedIDirect3D9CreateDevice, which can fire
    // before that thread has had a chance to run. LoadConfig is idempotent
    // (it only re-reads the same file), so the deferred call is harmless.
    LoadConfig();
    LogLine("[boot] early: config loaded, installing present hook");
    InstallForceImmediatePresentHook();
    LogLine("[boot] early: present hook done");
}

static int InstallD3D9Hook(void)
{
    g_d3dStackTls = TlsAlloc();
    g_d3dThunks = (unsigned char *)VirtualAlloc(NULL, MAX_D3D_SLOTS * 16,
                                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_d3dThunks) return 0;
    ProbeD3D9ForVtable();
    return g_d3dSlotCount > 0;
}

void InstallPrefetchHook(void)
{
    char line[256];
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base) {
        LogLine("InstallPrefetchHook: GetModuleHandleA(NULL) failed");
        return;
    }

    CalibrateTsc();
    LoadConfig();
    // The "write the config back so every key is visible" step deliberately
    // does NOT happen here. This runs moments after DllMain, while the game's
    // own startup and any other proxy DLL (the HD GUI mod loads from
    // version.dll at the same time) are still initialising. Doing file I/O
    // there put a write on the same ini that LogAppendRequested may be reading
    // from another thread, and coincided with intermittent startup failures -
    // one launch where the HD mod did not load, one crash before the window
    // appeared, one clean run. Not proven, but the startup write buys nothing
    // at that instant, so it is moved to the monitor thread's first tick,
    // well after the process has settled. See g_configRewritten.
    // Before any texture hook can fire: the push pointer must be resolved by
    // the time the first staged upload completes. Runs on this deferred
    // thread, not DllMain, so GetProcAddress is outside the loader lock.
    DetectHdTexInterop();
    // The overlay driver always runs (it owns the overlay window and its
    // message pump). The debug panel is deprecated and only starts when
    // ENABLE_GUI_PANEL is compiled back in.
    CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
#if ENABLE_GUI_PANEL
    LogLine("[boot] prefetch: starting GUI thread");
    CreateThread(NULL, 0, GuiThread, NULL, 0, NULL);
    LogLine("[boot] prefetch: GUI thread started");
#endif

    for (int i = 0; i < NUM_FNS; i++) {
        g_funcs[i].target = base + g_funcs[i].rva;
        g_stackTls[i] = TlsAlloc();
    }

    g_mainModBase = (unsigned int)base;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        g_mainModSize = nt->OptionalHeader.SizeOfImage;
    }
    sprintf(line, "Module base = 0x%08X size = 0x%08X", (unsigned int)base, g_mainModSize);
    LogLine(line);

#if ENABLE_GAME_MENU
    // Patch the menu-tree build call site now, well before the game's
    // framework init (FUN_00ab8210) constructs the window and runs the first
    // FUN_00abc310 rebuild - the menu exists with our entries from frame one.
    InstallGameMenuHook();
#endif

    // FA-object schedule diagnostic. Installed unconditionally (the detour
    // itself checks g_logFaSchedule and returns immediately when off, so it
    // can be toggled live from the GUI without a relaunch). Prologue verified
    // against the shipped exe: 55 8B EC 83 EC 14 - a standard PUSH EBP /
    // MOV EBP,ESP / SUB ESP, i.e. a clean 6-byte patch boundary with no
    // relative operands, the same shape as every other hook here.
    //
    // RETIRED (see ENABLE_GYSAHL_DIAG at the top of this file). "Unconditional"
    // is exactly why this had to be gated rather than just switched off: with
    // every toggle at 0 the block still patched seven game functions and put a
    // handler at the FRONT of the process exception chain. The bug it served is
    // fixed in scr104.clb.
#if ENABLE_GYSAHL_DIAG
    {
        HookedFunc faHf;
        faHf.name = "FUN_009c0490";
        faHf.target = base + FA_SCHED_RVA;
        faHf.rva = FA_SCHED_RVA;
        faHf.patchLen = 6;
        g_faSchedTarget = faHf.target;
        int faOk = InstallJmpHook(&faHf, (void *)Detour_faSched, &g_trampoline_faSched);
        sprintf(line, "[fa] changeFaObjectSchedule hook @ 0x%08X: %s",
                (unsigned int)faHf.target, faOk ? "installed" : "FAILED");
        LogLine(line);

        HookedFunc tcHf;
        tcHf.name = "FUN_009c22a0";
        tcHf.target = base + TIMER_CB_RVA;
        tcHf.rva = TIMER_CB_RVA;
        tcHf.patchLen = 6;
        int tcOk = InstallJmpHook(&tcHf, (void *)Detour_timerCb, &g_trampoline_timerCb);
        sprintf(line, "[fa] setRelativeTimerCallback hook @ 0x%08X: %s",
                (unsigned int)tcHf.target, tcOk ? "installed" : "FAILED");
        LogLine(line);

        // Window bisection. hideWindow needs a 7-byte patch: its prologue is
        // 55 8B EC 56 8B 75 08, so a 6-byte cut would split the final
        // MOV ESI,[EBP+8]. The other two are clean 6-byte PUSH EBP/MOV/SUB.
        HookedFunc wHf;
        int wOk;
        wHf.name = "showMessageWindow"; wHf.rva = WIN_SHOW_RVA;
        wHf.target = base + WIN_SHOW_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winShow, &g_trampoline_winShow);
        sprintf(line, "[fa] showMessageWindow hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "hideWindow"; wHf.rva = WIN_HIDE_RVA;
        wHf.target = base + WIN_HIDE_RVA; wHf.patchLen = 7;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winHide, &g_trampoline_winHide);
        sprintf(line, "[fa] hideWindow hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "isWaitingDecideOrCancel"; wHf.rva = WIN_WAIT_RVA;
        wHf.target = base + WIN_WAIT_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winWait, &g_trampoline_winWait);
        sprintf(line, "[fa] isWaitingDecideOrCancel hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "isWindowClosing"; wHf.rva = WIN_CLOSING_RVA;
        wHf.target = base + WIN_CLOSING_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winClosing, &g_trampoline_winClosing);
        sprintf(line, "[fa] isWindowClosing hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        // Vectored handler for the hardware watchpoint. Installed first so it
        // is ahead of anything else that might claim the exception.
        g_vehHandle = AddVectoredExceptionHandler(1, PlantWatchVeh);
        sprintf(line, "[fa] watchpoint VEH: %s", g_vehHandle ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "stringComp"; wHf.rva = STRCMP_RVA;
        wHf.target = base + STRCMP_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_strCmp, &g_trampoline_strCmp);
        sprintf(line, "[fa] stringComp hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);
    }
#endif  // ENABLE_GYSAHL_DIAG

    int ok[NUM_FNS];
    ok[FN_AACF10] = InstallJmpHook(&g_funcs[FN_AACF10], (void *)Detour_aacf10, &g_trampoline_aacf10);
    ok[FN_A2ADA0] = InstallJmpHook(&g_funcs[FN_A2ADA0], (void *)Detour_a2ada0, &g_trampoline_a2ada0);
    ok[FN_D19A00] = InstallJmpHook(&g_funcs[FN_D19A00], (void *)Detour_d19a00, &g_trampoline_d19a00);
    // FUN_00ac3040 (frame pacer) re-added: no SEH prologue (unlike
    // FUN_00a01a00/FUN_00a015b0, the two suspected of causing the earlier
    // crash), called once/frame from the main thread only - not a
    // multi-thread-contended hot spinlock like FUN_00a015b0, where adding
    // hook overhead could itself perturb contention timing. Lowest-risk of
    // the three new targets and the highest-value one (consistently the
    // single hottest function by raw sample count in every capture across
    // this entire investigation).
    ok[FN_AC3040] = InstallJmpHook(&g_funcs[FN_AC3040], (void *)Detour_ac3040, &g_trampoline_ac3040);

    // Sim-delta unquantiser. Same frame, 0x370 bytes further on than the
    // limiter above. Installed unconditionally because the detour's first act
    // is to check g_simDeltaFix and bail, so it can be A/B'd live from the GUI
    // without a relaunch - which is the whole point for a change whose effect
    // has to be judged by playing. Prologue verified against the shipped exe:
    // 55 8B EC 83 EC 08 (PUSH EBP / MOV EBP,ESP / SUB ESP,8), a clean 6-byte
    // boundary with no relative operands.
    {
        HookedFunc sdHf;
        sdHf.name = "FUN_00ac33b0";
        sdHf.rva = SIM_DELTA_RVA;
        sdHf.target = base + SIM_DELTA_RVA;
        sdHf.patchLen = 6;
        int sdOk = InstallJmpHook(&sdHf, (void *)Detour_simDelta, &g_trampoline_simDelta);
        sprintf(line, "[simdelta] frame-delta hook @ 0x%08X: %s",
                (unsigned int)sdHf.target, sdOk ? "installed" : "FAILED");
        LogLine(line);

        // DRAW_SHADOW pass timer. patchLen 5, not 6: the prologue is
        // 55 8B EC 51 56 8B F1, so a 6-byte cut would land inside MOV ESI,ECX.
        HookedFunc dsHf;
        dsHf.name = "FUN_00ac6040";
        dsHf.rva = DRAW_SHADOW_RVA;
        dsHf.target = base + DRAW_SHADOW_RVA;
        dsHf.patchLen = 5;
        int dsOk = InstallJmpHook(&dsHf, (void *)Detour_drawShadow, &g_trampoline_drawShadow);
        sprintf(line, "[ft] DRAW_SHADOW pass timer @ 0x%08X: %s",
                (unsigned int)dsHf.target, dsOk ? "installed" : "FAILED");
        LogLine(line);

        // Shadow-render skip (ShadowsOff diagnostic). Separate from the timer
        // above: that one wraps the pass HANDLER, this one skips the RENDER
        // inside it, so the handler's completion work still runs.
        HookedFunc srHf;
        srHf.name = "FUN_00a32a00";
        srHf.rva = SHADOW_RENDER_RVA;
        srHf.target = base + SHADOW_RENDER_RVA;
        srHf.patchLen = 6;
        int srOk = InstallJmpHook(&srHf, (void *)Detour_shadowRender, &g_trampoline_shadowRender);
        sprintf(line, "[shadow] shadow-render skip hook @ 0x%08X: %s",
                (unsigned int)srHf.target, srOk ? "installed" : "FAILED");
        LogLine(line);

        // Per-frame GPU fence. patchLen 5: the prologue is A1 60 E9 10 05
        // (MOV EAX,[0x0510E960]) - one 5-byte instruction, and JMP rel32 is
        // exactly 5, so nothing is split and no NOP padding is needed. The
        // operand is an absolute address, so it relocates into the trampoline
        // unchanged.
        HookedFunc gfHf;
        gfHf.name = "FUN_00a96090";
        gfHf.rva = GPU_FENCE_RVA;
        gfHf.target = base + GPU_FENCE_RVA;
        gfHf.patchLen = 5;
        int gfOk = InstallJmpHook(&gfHf, (void *)Detour_gpuFence, &g_trampoline_gpuFence);
        sprintf(line, "[gpufence] per-frame GPU fence hook @ 0x%08X: %s",
                (unsigned int)gfHf.target, gfOk ? "installed" : "FAILED");
        LogLine(line);

        // Draw-pass tags. patchLen differs per handler because their prologues
        // do, and cutting mid-instruction would corrupt the trampoline:
        //   ac5ee0/ac5f50/ac7030 : 55 8B EC 51 A1 xx xx xx xx  -> 9
        //   ac6890/ac6b00        : 55 8B EC 83 EC nn           -> 6
        //   ac6e50               : 55 8B EC 51 53              -> 5
        //   ac6150 (DRAW_MENU)   : 55 8B EC 51 A1 xx xx xx xx  -> 9
        //   ab7810 (BACK_BUFFER) : 55 8B EC 8B 45 10           -> 6
        // The last two were verified against the shipped exe on 2026-08-12
        // (ghidra_output/menu_pass_prologue.txt lists each instruction with
        // its cumulative offset, so the boundary is read rather than assumed).
        // ab7810 is only 22 bytes long in total, which is fine - the patch
        // needs 6 relocatable leading bytes, not a large function.
        struct { const char *name; unsigned int rva; int len; void *detour; void **tramp; }
        passHooks[] = {
            { "FUN_00ac5ee0", 0x00ac5ee0 - 0x00400000, 9, (void *)Detour_msSchedule,    &g_trampoline_msSchedule },
            { "FUN_00ac5f50", 0x00ac5f50 - 0x00400000, 9, (void *)Detour_msPropagation, &g_trampoline_msPropagation },
            { "FUN_00ac6890", 0x00ac6890 - 0x00400000, 6, (void *)Detour_msDepth,       &g_trampoline_msDepth },
            { "FUN_00ac6b00", 0x00ac6b00 - 0x00400000, 6, (void *)Detour_msShadow,      &g_trampoline_msShadow },
            { "FUN_00ac6e50", 0x00ac6e50 - 0x00400000, 5, (void *)Detour_ms,            &g_trampoline_ms },
            { "FUN_00ac7030", 0x00ac7030 - 0x00400000, 9, (void *)Detour_filter,        &g_trampoline_filter },
            { "FUN_00ac6150", 0x00ac6150 - 0x00400000, 9, (void *)Detour_menu,          &g_trampoline_menu },
            { "FUN_00ab7810", 0x00ab7810 - 0x00400000, 6, (void *)Detour_backbuf,       &g_trampoline_backbuf },
        };
        // Screen-space buffer allocator - the provenance gate for ShadowBufPct.
        // Prologue 55 8B EC 83 EC 10, clean 6-byte boundary.
        HookedFunc sbHf;
        sbHf.name = "FUN_00b00f10";
        sbHf.rva = 0x00b00f10 - 0x00400000;
        sbHf.target = base + sbHf.rva;
        sbHf.patchLen = 6;
        int sbOk = InstallJmpHook(&sbHf, (void *)Detour_sbufAlloc, &g_trampoline_sbufAlloc);
        sprintf(line, "[shadowbuf] screen-buffer allocator hook @ 0x%08X: %s",
                (unsigned int)sbHf.target, sbOk ? "installed" : "FAILED");
        LogLine(line);

        // Screen-set rebuild gate - kills the per-frame rebuild loop that
        // descriptor-mode SSAA otherwise provokes. Prologue 56 8B F1 8B 4E 6C
        // (PUSH ESI / MOV ESI,ECX / MOV ECX,[ESI+0x6c]) - 6 bytes, verified
        // against the shipped exe, no relative operands.
        HookedFunc rbHf;
        rbHf.name = "FUN_00b00810";
        rbHf.rva = 0x00b00810 - 0x00400000;
        rbHf.target = base + rbHf.rva;
        rbHf.patchLen = 6;
        int rbOk = InstallJmpHook(&rbHf, (void *)Detour_screenSetRebuild,
                                  &g_trampoline_screenSetRebuild);
        sprintf(line, "[ssaa] screen-set rebuild gate @ 0x%08X: %s",
                (unsigned int)rbHf.target, rbOk ? "installed" : "FAILED");
        LogLine(line);

        // Texture factory entry probe. patchLen 5: the prologue is
        // 55 8B EC 6A FF (PUSH EBP / MOV EBP,ESP / PUSH -1) - verified against
        // the shipped exe, NOT the bare SEH form assumed first. Exactly 5
        // bytes, no relative operands.
        HookedFunc tfHf;
        tfHf.name = "FUN_00a94770";
        tfHf.rva = 0x00a94770 - 0x00400000;
        tfHf.target = base + tfHf.rva;
        tfHf.patchLen = 5;
        int tfOk = InstallJmpHook(&tfHf, (void *)Detour_texFactory, &g_trampoline_texFactory);
        sprintf(line, "[halfres] texture-factory probe @ 0x%08X: %s",
                (unsigned int)tfHf.target, tfOk ? "installed" : "FAILED");
        LogLine(line);

        // TextureImp constructor - names the code that decided each texture's
        // dimensions, one level above the engine's generic creation path.
        HookedFunc tiHf;
        tiHf.name = "FUN_00aa3ce0";
        tiHf.rva = TEXIMP_CTOR_RVA;
        tiHf.target = base + TEXIMP_CTOR_RVA;
        tiHf.patchLen = 6;
        int tiOk = InstallJmpHook(&tiHf, (void *)Detour_texImpCtor, &g_trampoline_texImpCtor);
        sprintf(line, "[halfres] TextureImp ctor probe @ 0x%08X: %s",
                (unsigned int)tiHf.target, tiOk ? "installed" : "FAILED");
        LogLine(line);

        // sizeof-derived, NOT a literal: this loop was hardcoded to 6 and the
        // two handlers added for the SSAA probe (DRAW_MENU, DRAW_BACK_BUFFER)
        // compiled into the table above but were silently never installed -
        // the log showed six "tag hook ... installed" lines and no failure,
        // so it read as success. A count that cannot drift from the table is
        // the only version of this that stays correct when the table grows.
        for (int ph = 0; ph < (int)(sizeof(passHooks) / sizeof(passHooks[0])); ph++) {
            HookedFunc phf;
            phf.name = passHooks[ph].name;
            phf.rva = passHooks[ph].rva;
            phf.target = base + passHooks[ph].rva;
            phf.patchLen = passHooks[ph].len;
            int ok2 = InstallJmpHook(&phf, passHooks[ph].detour, passHooks[ph].tramp);
            sprintf(line, "[pass] tag hook %s @ 0x%08X (len %d): %s",
                    passHooks[ph].name, (unsigned int)phf.target,
                    passHooks[ph].len, ok2 ? "installed" : "FAILED");
            LogLine(line);
        }
    }
    // FN_A01A00/FN_A015B0 remain DISABLED pending a safer approach (either
    // a verified SEH-safety check, or the non-return-hijacking entry-only
    // technique instead) - not worth re-risking a crash on these two until
    // that exists.
    ok[FN_A01A00] = 0;
    ok[FN_A015B0] = 0;
    // FUN_00a41570 (v17): confirmed safe via the same "no inbound refs into
    // the prologue, standard PUSH EBP/MOV EBP,ESP/SUB ESP prologue" check
    // that has been correct for every hook except FN_A01A00/FN_A015B0 (which
    // both have SEH), so this one is enabled.
    ok[FN_A41570] = InstallJmpHook(&g_funcs[FN_A41570], (void *)Detour_a41570, &g_trampoline_a41570);
    // FUN_00aa7850 (v17b): shader-compile queue drain, safety-checked the
    // same way as FN_A41570 (standard prologue, no SEH, no external inbound
    // refs into the patched bytes).
    ok[FN_AA7850] = InstallJmpHook(&g_funcs[FN_AA7850], (void *)Detour_aa7850, &g_trampoline_aa7850);

    sprintf(line, "Hooks installed: aacf10=%d a2ada0=%d d19a00=%d ac3040=%d a01a00=%d a015b0=%d a41570=%d aa7850=%d",
            ok[0], ok[1], ok[2], ok[3], ok[4], ok[5], ok[6], ok[7]);
    LogLine(line);

    int csOk = InstallCriticalSectionHook();
    sprintf(line, "EnterCriticalSection IAT hook installed: %d", csOk);
    LogLine(line);

    int wfsoOk = InstallWfsoHook();
    sprintf(line, "WaitForSingleObject IAT hook installed: %d", wfsoOk);
    LogLine(line);

    int raiseOk = InstallRaiseExceptionHook();
    sprintf(line, "RaiseException IAT hook installed: %d", raiseOk);
    LogLine(line);

    int idealOk = InstallIdealProcessorHook();
    sprintf(line, "SetThreadIdealProcessor IAT hook installed: %d", idealOk);
    LogLine(line);

    // Install alloc tracking BEFORE the throttle call-site patch, so there's
    // no window where dispatches are already routed through our wrapper
    // (which calls Begin/EndDispatchTracking) while HeapAlloc/VirtualAlloc
    // aren't hooked yet - a miss there would just mean an untracked
    // allocation, not a crash, but there's no reason to accept the gap.
    int allocTrackOk = InstallAllocTrackingHooks();
    sprintf(line, "HeapAlloc/VirtualAlloc tracking hooks installed: %d", allocTrackOk);
    LogLine(line);

    // The engine's own named-heap allocator - installed before the throttle
    // call-site patch for the same reason as the OS-allocator hooks above
    // (no window where dispatches route through our wrapper before this is
    // ready to catch what it calls).
    int allocatorHookOk = InstallAllocatorHook();
    sprintf(line, "FUN_00b454a0 (named-heap allocator) hook installed: %d", allocatorHookOk);
    LogLine(line);

    // v12: runs here on the deferred thread, not from DllMain - the
    // throwaway-device probe actually calls into d3d9.dll, which carries a
    // real deadlock risk from DllMain (see ProbeD3D9ForVtable's comment).
    int d3dOk = InstallD3D9Hook();
    sprintf(line, "D3D9 instrumentation installed (throwaway-device vtable probe): %d", d3dOk);
    LogLine(line);

    InstallForceImmediatePresentHook();

    int throttleOk = InstallLoaderThrottle();
    sprintf(line, "Loader dispatch throttle installed (max=%d concurrent): %d", LOADER_THROTTLE_MAX, throttleOk);
    LogLine(line);

    int shaderIdOk = InstallShaderIdentityHook();
    sprintf(line, "Shader identity capture installed (entry-only, SEH-safe): %d", shaderIdOk);
    LogLine(line);

    int anyOk = d3dOk | csOk | wfsoOk | raiseOk | idealOk | throttleOk | allocTrackOk | allocatorHookOk | shaderIdOk;
    for (int i = 0; i < NUM_FNS; i++) anyOk |= ok[i];
    if (anyOk) {
        CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
        CreateThread(NULL, 0, StutterWatchdogThread, NULL, 0, NULL);
    }
}
