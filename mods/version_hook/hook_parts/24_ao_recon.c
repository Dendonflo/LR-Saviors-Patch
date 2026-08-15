// ---- AO injection recon (v22 diagnostic) ----------------------------------
// Question being answered (2026-08-15): can ambient occlusion be injected by
// piggybacking the engine's own screen-space shadow buffer?
//
// The plan it serves ("option 2"): the MS_SHADOW pass renders a screen-space
// shadow buffer that the MATERIAL shaders sample during MULTI_SAMPLE (the
// foliage shader's tap list literally includes screenShadowMap). If we
// multiply an SSAO term into that buffer after the engine fills it, AO gets
// applied inside lighting, per-material, before fog and post - and
// ScreenShadowResPct becomes the AO resolution control for free.
//
// What must be TRUE for that to work, and what this part measures:
//   1. The MS_SHADOW output is a texture we can identify and bind
//      -> latch RT0 during MS_SHADOW, resolve its container texture.
//   2. The material pass actually samples that texture
//      -> count SetTexture(stage, thatTexture) calls during MULTI_SAMPLE,
//         per stage. Zero means the composite happens some other way and
//         the whole approach dies here.
//   3. The buffer is not overwritten between our would-be injection point
//      and the sampling -> count rebinds of that surface as RT0 in passes
//      AFTER MULTI_SAMPLE began. Nonzero means lifetime problems.
//   4. The R32F linear-depth target (the SSAO input) is itself bindable
//      -> resolve its container too. Also count whether materials sample
//         depth directly during MULTI_SAMPLE - interesting either way.
//
// What this deliberately cannot answer: whether the buffer modulates only
// DIRECT sunlight (in which case AO injected there darkens the wrong term
// and the option is a dead end visually). That is the NEXT probe - painting
// the buffer and looking at the game - and only worth building if this one
// says the plumbing works.
//
// Cost: one SetTexture hook, which this project has deliberately avoided
// until now (ultra-hot path). The body is a pass check plus pointer
// compares; acceptable for a recon build, OFF for release regardless.

#if ENABLE_AO_RECON

typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTexture)(
    IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
static PFN_SetTexture g_origSetTexture = NULL;

// Local IID so the build does not gain a dxguid.lib dependency for one call.
static const GUID g_aoIidTexture9 =
    { 0x85C31227, 0x3DE5, 0x4f00, { 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5 } };

static void *g_aoShadowSurf = NULL;    // MS_SHADOW RT0 surface (identity)
static void *g_aoShadowTex  = NULL;    // its container texture
static LONG  g_aoShadowW, g_aoShadowH, g_aoShadowFmt;
static void *g_aoDepthTex   = NULL;    // container of the linear-depth RT

#define AO_READ_MAX 12
static struct { DWORD stage; void *tex; volatile LONG count; } g_aoReads[AO_READ_MAX];
static volatile LONG g_aoReadCount = 0;

static volatile LONG g_aoMsStage[16];      // shadow-tex samples in MULTI_SAMPLE, per stage
// v22b: the first flight produced NO report - the MULTI_SAMPLE condition
// never accumulated 120 frames, and there was no partial output to say which
// prerequisite failed. Two additions: shadow-tex sightings counted in EVERY
// pass (if the composite happens in DRAW_FILTER rather than the material
// pass, this is what says so), and a timeout report from the monitor thread
// so the recon always speaks.
static volatile LONG g_aoTexPass[PASS_COUNT + 1];
static volatile LONG g_aoSetTexCalls = 0;      // proves the hook is live at all
static volatile LONG g_aoShadowPassCalls = 0;  // SetTexture calls seen inside MS_SHADOW
static volatile LONG g_aoMsDepthSamples = 0;
static volatile LONG g_aoLateRebinds = 0;  // shadow surf as RT0 after MULTI_SAMPLE began
static volatile LONG g_aoFramesSampled = 0;
static LONG g_aoLastSampleFrame = -1;
static volatile LONG g_aoReported = 0;

static void *AoResolveContainer(void *surf)
{
    void *tex = NULL;
    __try {
        if (surf && SUCCEEDED(IDirect3DSurface9_GetContainer(
                (IDirect3DSurface9 *)surf, &g_aoIidTexture9, &tex)) && tex) {
            // GetContainer AddRefs; drop it immediately - only the identity is
            // kept, same never-hold rule as every surface table in this mod.
            IDirect3DBaseTexture9_Release((IDirect3DBaseTexture9 *)tex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { tex = NULL; }
    return tex;
}

static void AoReconReport(const char *how)
{
    char l[224];
    sprintf(l, "[aorecon] ---- report (%s): settex_calls=%ld ms_shadow_settex=%ld ----",
            how, g_aoSetTexCalls, g_aoShadowPassCalls);
    LogLine(l);
    sprintf(l, "[aorecon] MS_SHADOW rt0=%p %ldx%ld fmt=%ld container=%p (%s)",
            g_aoShadowSurf, g_aoShadowW, g_aoShadowH, g_aoShadowFmt, g_aoShadowTex,
            g_aoShadowTex ? "texture - BINDABLE" : "no container - NOT bindable, option 2 dead");
    LogLine(l);
    sprintf(l, "[aorecon] depth prepass rt=%p container=%p (%s)",
            g_depthRtMain, g_aoDepthTex,
            g_aoDepthTex ? "texture - usable as SSAO input" : "no container - SSAO input problem");
    LogLine(l);
    {
        LONG n = g_aoReadCount;
        if (n > AO_READ_MAX) n = AO_READ_MAX;
        for (LONG i = 0; i < n; i++) {
            sprintf(l, "[aorecon] MS_SHADOW reads: stage %lu tex=%p x%ld%s",
                    (unsigned long)g_aoReads[i].stage, g_aoReads[i].tex, g_aoReads[i].count,
                    g_aoReads[i].tex == g_aoDepthTex ? "  <== the DEPTH texture" : "");
            LogLine(l);
        }
    }
    {
        char buf[160];
        int o = sprintf(buf, "[aorecon] MULTI_SAMPLE samples shadow tex:");
        int any = 0;
        for (int s = 0; s < 16; s++)
            if (g_aoMsStage[s]) { o += sprintf(buf + o, " s%d=%ld", s, g_aoMsStage[s]); any = 1; }
        if (!any) o += sprintf(buf + o, " NEVER - composite is not via material sampling, option 2 dead");
        LogLine(buf);
    }
    sprintf(l, "[aorecon] frames_with_samples=%ld  late_rebinds_after_MS=%ld (%s)  depth_sampled_in_MS=%ld",
            g_aoFramesSampled, g_aoLateRebinds,
            g_aoLateRebinds ? "LIFETIME PROBLEM" : "lifetime OK",
            g_aoMsDepthSamples);
    LogLine(l);
    {
        // Which passes bind the shadow container at all - the question the
        // first flight could not answer when MULTI_SAMPLE came up empty.
        char buf[200];
        int o = sprintf(buf, "[aorecon] shadow tex bound during:");
        int any = 0;
        for (int p = 0; p <= PASS_COUNT; p++)
            if (g_aoTexPass[p]) { o += sprintf(buf + o, " pass%d=%ld", p, g_aoTexPass[p]); any = 1; }
        if (!any) o += sprintf(buf + o, " (never bound via SetTexture in ANY pass)");
        LogLine(buf);
    }
    LogLine("[aorecon] recon complete - hook stays passthrough for the rest of the session");
}

// Timeout path, driven from the monitor thread (which always runs): if the
// success condition has not fired, report whatever was gathered anyway.
//
// 60 seconds, by the user's explicit call ("keep the timeout at 60, I'll
// stand around") - a longer observation window beats a faster report, and
// they adjust the test to fit rather than the other way around. The v22b
// lesson still stands in general: the FIRST flight of this recon died to a
// 60s timeout in a ~35s session, so the timeout length is now a deliberate
// choice, not an accident.
static void AoReconTick(void)
{
    static LONG ticks = 0;
    if (g_aoReported) return;
    if (++ticks == 120 &&
        InterlockedCompareExchange(&g_aoReported, 1, 0) == 0)
        AoReconReport("TIMEOUT at 60s - success condition not met by then");
}

static HRESULT STDMETHODCALLTYPE HookedSetTexture(
    IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *tex)
{
    LONG pass = g_curPass;
    g_aoSetTexCalls++;   // racy increment is fine for a liveness counter

    // Track every pass that binds the shadow container, not just MULTI_SAMPLE.
    if (tex && (void *)tex == g_aoShadowTex && g_aoShadowTex) {
        LONG p = pass;
        if (p < 0 || p > PASS_COUNT) p = PASS_COUNT;
        InterlockedIncrement(&g_aoTexPass[p]);
    }

    if (pass == PASS_MS_SHADOW) {
        g_aoShadowPassCalls++;
        // Latch the pass's RT0 once, with descriptor and container.
        //
        // v22d: NOT via g_prevRt0. The v22c flight reported rt0=00000000
        // after 24,706 SetTexture calls inside the pass - because the
        // "updated on every slot-0 bind, unconditionally" tracker lives
        // inside #if ENABLE_SURFACE_DIAG, which is 0 in this build. Its
        // comment was written from inside that gate. Ask the device
        // directly instead, through g_origGetRenderTarget - the REAL
        // vtable entry, deliberately bypassing HookedGetRenderTarget,
        // whose whole job is to lie to the engine while MSAA substitution
        // is active. One COM call for the whole session.
        if (!g_aoShadowSurf && g_origGetRenderTarget) {
            IDirect3DSurface9 *s = NULL;
            __try {
                if (SUCCEEDED(g_origGetRenderTarget(dev, 0, &s)) && s) {
                    D3DSURFACE_DESC d;
                    if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) {
                        g_aoShadowW = (LONG)d.Width;
                        g_aoShadowH = (LONG)d.Height;
                        g_aoShadowFmt = (LONG)d.Format;
                    }
                    g_aoShadowTex = AoResolveContainer(s);
                    g_aoShadowSurf = (void *)s;   // identity only
                    IDirect3DSurface9_Release(s); // GetRenderTarget AddRefs
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (!g_aoDepthTex && g_depthRtMain)
            g_aoDepthTex = AoResolveContainer(g_depthRtMain);
        // Record what this pass READS (distinct stage+texture pairs).
        if (tex) {
            LONG n = g_aoReadCount;
            if (n > AO_READ_MAX) n = AO_READ_MAX;
            LONG i;
            for (i = 0; i < n; i++)
                if (g_aoReads[i].stage == stage && g_aoReads[i].tex == (void *)tex) {
                    InterlockedIncrement(&g_aoReads[i].count);
                    break;
                }
            if (i == n && n < AO_READ_MAX) {
                g_aoReads[n].stage = stage;
                g_aoReads[n].tex = (void *)tex;
                g_aoReads[n].count = 1;
                g_aoReadCount = n + 1;
            }
        }
    } else if (pass == PASS_MS && !g_aoReported) {
        if (tex && (void *)tex == g_aoShadowTex) {
            InterlockedIncrement(&g_aoMsStage[stage & 15]);
            LONG fr = g_msFrameSeq;
            if (fr != g_aoLastSampleFrame) {
                g_aoLastSampleFrame = fr;
                LONG fs = InterlockedIncrement(&g_aoFramesSampled);
                if (fs >= 120 &&
                    InterlockedCompareExchange(&g_aoReported, 1, 0) == 0)
                    AoReconReport("SUCCESS - sampled in MULTI_SAMPLE across 120 frames");
                // If the timeout report already went out and sampling only
                // began afterwards (e.g. the buffer is only consumed in some
                // areas), say so once instead of staying silent about it.
                else if (fs == 120 && g_aoReported) {
                    static volatile LONG lateOnce = 0;
                    if (InterlockedCompareExchange(&lateOnce, 1, 0) == 0)
                        LogLine("[aorecon] LATE SUCCESS - MULTI_SAMPLE sampling began after the timeout report");
                }
            }
        }
        if (tex && (void *)tex == g_aoDepthTex)
            InterlockedIncrement(&g_aoMsDepthSamples);
    } else if (pass > PASS_MS && g_aoShadowSurf) {
        // Lifetime check 3: the shadow buffer re-targeted after the material
        // pass began would mean injected content gets overwritten. Also
        // rebuilt off g_prevRt0 in v22d - polled at pass CHANGES only (one
        // real GetRenderTarget per pass transition, not per SetTexture).
        static LONG lastPolledPass = -1;
        if (pass != lastPolledPass) {
            lastPolledPass = pass;
            IDirect3DSurface9 *s = NULL;
            __try {
                if (g_origGetRenderTarget &&
                    SUCCEEDED(g_origGetRenderTarget(dev, 0, &s)) && s) {
                    if ((void *)s == g_aoShadowSurf)
                        InterlockedIncrement(&g_aoLateRebinds);
                    IDirect3DSurface9_Release(s);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    return g_origSetTexture(dev, stage, tex);
}

// Called from the device-vtable install block in 16_output_res_cascade.c
// (forward-declared in 01_config_gates.c) - same pattern, same timing as
// every other device hook.
static void InstallAoReconHook(void **vtbl)
{
    DWORD oldProtect;
    int slot = offsetof(IDirect3DDevice9Vtbl, SetTexture) / sizeof(void *);
    g_origSetTexture = (PFN_SetTexture)ResolveOrigSlot(vtbl[slot]);
    if (VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slot] = (void *)HookedSetTexture;
        VirtualProtect(&vtbl[slot], sizeof(void *), oldProtect, &oldProtect);
    }
    LogLine("[aorecon] SetTexture hook installed (screen-shadow plumbing recon)");
}

#endif // ENABLE_AO_RECON
