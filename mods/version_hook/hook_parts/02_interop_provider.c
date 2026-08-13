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
