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

#if ENABLE_AO_SSAO
static void SsaoApply(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex, int raw);  // 25_ssao.c
#endif

// Local IID so the build does not gain a dxguid.lib dependency for one call.
static const GUID g_aoIidTexture9 =
    { 0x85C31227, 0x3DE5, 0x4f00, { 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5 } };

static void *g_aoShadowSurf = NULL;    // FIRST MS_SHADOW RT0 (kept for report continuity)
static void *g_aoShadowTex  = NULL;    // its container texture
static LONG  g_aoShadowW, g_aoShadowH, g_aoShadowFmt;
static void *g_aoDepthTex   = NULL;    // container of the linear-depth RT

// v22f: the pass PING-PONGS - it samples its own container at two stages
// while rendering, which requires at least two buffers alternating as RT0.
// Latching only the first meant every downstream check ran against what is
// probably an INTERMEDIATE, not the final output (consistent with the pass
// handler publishing its result handle from the settings object, not from
// RT0-at-entry). Track the full set of RT0s the pass uses, and which one is
// LAST each frame - that one is the output the engine publishes.
#define AO_RT_MAX 4
static struct {
    void *surf, *tex;
    LONG w, h, fmt;
    volatile LONG lastCount;   // frames where this was the final RT0 of the pass
} g_aoRts[AO_RT_MAX];
static volatile LONG g_aoRtCount = 0;
static LONG g_aoLastRtIdx = -1;        // RT0 index seen most recently in-pass
static LONG g_aoLastRtFrame = -1;

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
// v22e: draw-level consumption. The v22d flight showed the shadow buffer is
// only ever BOUND inside its own pass (stage 1 being the last state as the
// pass ends) - and D3D9 sampler bindings persist across passes, so material
// draws can consume it with zero SetTexture traffic. These count draws in
// PASS_MS that execute WITH the shadow texture live on a sampler.
static volatile LONG g_aoStageMask = 0;        // bit N = shadow tex bound at stage N
static volatile LONG g_aoMsDrawStage[16];      // draws in PASS_MS per live stage
static volatile LONG g_aoMsDrawFrames = 0;     // frames with at least one such draw
static LONG g_aoLastDrawFrame = -1;
static volatile LONG g_aoLateRebinds = 0;  // shadow surf as RT0 after MULTI_SAMPLE began
static volatile LONG g_aoFramesSampled = 0;
static LONG g_aoLastSampleFrame = -1;
static volatile LONG g_aoReported = 0;

// Device Reset destroys every render target and recreates them as NEW
// objects, so every latched identity here is a dead pointer afterwards.
// Called from HookedDeviceReset; everything re-latches on the first
// MS_SHADOW pass of the new device. Counters and the one-shot report are
// deliberately left alone - they describe the session, not the device.
static void AoReconReset(void)
{
    for (int i = 0; i < AO_RT_MAX; i++) {
        g_aoRts[i].surf = NULL;
        g_aoRts[i].tex = NULL;
    }
    g_aoRtCount = 0;
    g_aoLastRtIdx = -1;
    g_aoShadowSurf = NULL;
    g_aoShadowTex = NULL;
    g_aoDepthTex = NULL;
    g_aoStageMask = 0;
    LogLine("[aorecon] device Reset - AO latches cleared, will re-latch");
}

// A texture counts as "the shadow buffer" if it is the container of ANY
// render target the ping-pong touched.
static int AoIsShadowTex(void *tex)
{
    if (!tex) return 0;
    LONG n = g_aoRtCount;
    if (n > AO_RT_MAX) n = AO_RT_MAX;
    for (LONG i = 0; i < n; i++)
        if (g_aoRts[i].tex == tex) return 1;
    return 0;
}

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
    {
        // The full ping-pong set. The one with the dominant lastCount is the
        // pass's true OUTPUT - the injection target if the approach works.
        LONG n = g_aoRtCount;
        if (n > AO_RT_MAX) n = AO_RT_MAX;
        for (LONG i = 0; i < n; i++) {
            sprintf(l, "[aorecon] pass RT[%ld]: surf=%p %ldx%ld fmt=%ld container=%p last_of_pass=%ld frames%s",
                    i, g_aoRts[i].surf, g_aoRts[i].w, g_aoRts[i].h, g_aoRts[i].fmt,
                    g_aoRts[i].tex, g_aoRts[i].lastCount,
                    g_aoRts[i].tex ? "" : "  (NO container)");
            LogLine(l);
        }
    }
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
        // Draw-level consumption - the line that actually decides option 2.
        char buf[200];
        int o = sprintf(buf, "[aorecon] PASS_MS draws with shadow tex live:");
        int any = 0;
        for (int s = 0; s < 16; s++)
            if (g_aoMsDrawStage[s]) { o += sprintf(buf + o, " s%d=%ld", s, g_aoMsDrawStage[s]); any = 1; }
        if (!any) o += sprintf(buf + o, " none");
        o += sprintf(buf + o, "  frames=%ld", g_aoMsDrawFrames);
        LogLine(buf);
    }
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

// ---- Tint probe (v23) -----------------------------------------------------
// The recon confirmed the plumbing: the pass's final output is a FULL-RES
// A8R8G8B8 texture, bound once per frame at stage 14, sampled by ~119
// material draws per frame, never touched after its pass. What binds/draws
// cannot say is the SEMANTICS: what does darkening this buffer darken?
// ARGB means it may carry several lighting terms across channels.
//
// One run answers it visually. Once per frame, right as the engine binds the
// buffer at s14, draw five vertical multiply bands into it:
//   [ 0-20%]  all channels x0.35   - does darkening darken ambient or sun?
//   [20-40%]  R x0.2               - what does the R channel carry?
//   [40-60%]  G x0.2
//   [60-80%]  B x0.2
//   [80-100%] untouched            - reference
// The user reads the screen like a legend. Fixed-function quads (XYZRHW +
// DIFFUSE, ZERO/SRCCOLOR blend = dst*src), no shaders involved; a state
// block wraps the whole thing. AoTint=0 in the ini disarms without rebuild.
// g_aoTint declared in 01_config_gates.c (the config table in 08 needs it).
static volatile LONG g_aoTints = 0;

// ---- Buffer dump (v23b) ---------------------------------------------------
// The tint probe's verbal readings ("darker around everything, some elements
// more than others") are exactly the kind of observation that should be read
// off the actual pixels instead of through descriptions of a moving screen.
// One menu click writes the UNTINTED composite to ao_buffer.bmp next to the
// exe, 32bpp so the alpha channel survives for offline channel analysis.
// g_aoDumpRequest declared in 01_config_gates.c (menu part 09 needs it).

// v24b: the SSAO first flight drew pure white - plumbing confirmed working,
// occlusion never found, which points straight at the depth UNITS assumption
// (world-Z vs normalized). Stop assuming: read the depth RT and print its
// actual value distribution. This is the whole diagnosis in one log block.
static void AoDumpDepthStats(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *sys = NULL;
    __try {
        if (!g_depthRtMain) { LogLine("[aodepth] no depth RT latched"); return; }
        D3DSURFACE_DESC d;
        if (FAILED(IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)g_depthRtMain, &d)))
            return;
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(
                dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys)
            return;
        if (FAILED(IDirect3DDevice9_GetRenderTargetData(
                dev, (IDirect3DSurface9 *)g_depthRtMain, sys))) goto done;
        {
            D3DLOCKED_RECT lr;
            if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
                goto done;
            {
                double sum = 0.0;
                float mn = 3.4e38f, mx = -3.4e38f;
                LONG n = 0, b1 = 0, b10 = 0, b100 = 0, b1k = 0, b10k = 0;
                for (UINT y = 0; y < d.Height; y += 4) {           // 1/16 sample grid
                    const float *row = (const float *)((const unsigned char *)lr.pBits + y * lr.Pitch);
                    for (UINT x = 0; x < d.Width; x += 4) {
                        float v = row[x];
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                        sum += v; n++;
                        if (v < 1.0f) b1++;
                        else if (v < 10.0f) b10++;
                        else if (v < 100.0f) b100++;
                        else if (v < 1000.0f) b1k++;
                        else b10k++;
                    }
                }
                {
                    const float *midRow = (const float *)((const unsigned char *)lr.pBits
                                          + (d.Height / 2) * lr.Pitch);
                    char l[256];
                    sprintf(l, "[aodepth] %lux%lu R32F: min=%g max=%g mean=%g centre=%g",
                            d.Width, d.Height, mn, mx, n ? sum / n : 0.0,
                            midRow[d.Width / 2]);
                    LogLine(l);
                    sprintf(l, "[aodepth] value bands: <1=%ld  1-10=%ld  10-100=%ld  100-1000=%ld  >=1000=%ld  (of %ld sampled)",
                            b1, b10, b100, b1k, b10k, n);
                    LogLine(l);
                    LogFlushNow();
                }
            }
            IDirect3DSurface9_UnlockRect(sys);
        }
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (sys) IDirect3DSurface9_Release(sys);
}

static void AoDumpBuffer(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex)
{
    IDirect3DSurface9 *surf = NULL, *sys = NULL;
    FILE *f = NULL;
    __try {
        D3DSURFACE_DESC d;
        if (FAILED(IDirect3DTexture9_GetSurfaceLevel(
                (IDirect3DTexture9 *)tex, 0, &surf)) || !surf) goto done;
        if (FAILED(IDirect3DSurface9_GetDesc(surf, &d))) goto done;
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(
                dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys)
            goto done;
        if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, surf, sys))) goto done;
        {
            D3DLOCKED_RECT lr;
            if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
                goto done;
            {
                char path[MAX_PATH];
                GetModuleFileNameA(NULL, path, MAX_PATH);
                char *slash = strrchr(path, '\\');
                if (slash) strcpy(slash + 1, "ao_buffer.bmp");
                f = fopen(path, "wb");
                if (f) {
                    // 32bpp BMP, BI_RGB, rows bottom-up.
                    DWORD rowBytes = d.Width * 4;
                    DWORD imgBytes = rowBytes * d.Height;
                    BITMAPFILEHEADER fh;
                    BITMAPINFOHEADER ih;
                    memset(&fh, 0, sizeof(fh));
                    memset(&ih, 0, sizeof(ih));
                    fh.bfType = 0x4D42;
                    fh.bfOffBits = sizeof(fh) + sizeof(ih);
                    fh.bfSize = fh.bfOffBits + imgBytes;
                    ih.biSize = sizeof(ih);
                    ih.biWidth = (LONG)d.Width;
                    ih.biHeight = (LONG)d.Height;   // positive = bottom-up
                    ih.biPlanes = 1;
                    ih.biBitCount = 32;
                    fwrite(&fh, sizeof(fh), 1, f);
                    fwrite(&ih, sizeof(ih), 1, f);
                    for (LONG y = (LONG)d.Height - 1; y >= 0; y--)
                        fwrite((unsigned char *)lr.pBits + y * lr.Pitch, rowBytes, 1, f);
                    fclose(f); f = NULL;
                    {
                        char l[224];
                        sprintf(l, "[aodump] wrote %s (%lux%lu fmt=%d)",
                                path, d.Width, d.Height, (int)d.Format);
                        LogLine(l);
                        LogFlushNow();
                    }
                }
            }
            IDirect3DSurface9_UnlockRect(sys);
        }
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (f) fclose(f);
    if (sys) IDirect3DSurface9_Release(sys);
    if (surf) IDirect3DSurface9_Release(surf);
}

static void AoTintBuffer(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex)
{
    static LONG lastFrame = -1;
    LONG fr = g_msFrameSeq;
    if (fr == lastFrame) return;
    lastFrame = fr;

    IDirect3DSurface9 *surf = NULL, *oldRt = NULL;
    IDirect3DStateBlock9 *sb = NULL;
    __try {
        if (FAILED(IDirect3DTexture9_GetSurfaceLevel(
                (IDirect3DTexture9 *)tex, 0, &surf)) || !surf) goto done;
        // State block FIRST: if anything below fails, Apply still restores.
        // D3DSBT_ALL covers every state touched here except the render
        // target, which state blocks never record - restored by hand.
        if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb)) || !sb)
            goto done;
        if (FAILED(g_origGetRenderTarget(dev, 0, &oldRt)) || !oldRt) goto done;
        if (FAILED(g_origSetRT(dev, 0, surf))) goto done;

        D3DSURFACE_DESC d;
        if (FAILED(IDirect3DSurface9_GetDesc(surf, &d))) goto done;
        {
            D3DVIEWPORT9 vp;
            vp.X = 0; vp.Y = 0;
            vp.Width = d.Width; vp.Height = d.Height;
            vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
            g_origSetViewport(dev, &vp);
        }

        IDirect3DDevice9_SetPixelShader(dev, NULL);
        IDirect3DDevice9_SetVertexShader(dev, NULL);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        g_origSetTexture(dev, 0, NULL);
        g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        g_origSetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ZERO);
        g_origSetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        // Same separate-alpha pin as the SSAO pass (see 25_ssao.c): the
        // engine leaves SEPARATEALPHABLENDENABLE on with its own factors,
        // and an inherited alpha-replace wipes the world sun-shadow mask.
        g_origSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        g_origSetRenderState(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        g_origSetRenderState(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
        g_origSetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
        g_origSetRenderState(dev, D3DRS_ZENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0F);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);

        {
            // Multiply colours, ARGB: grey 0.35, then R/G/B each cut to 0.2
            // with the other channels left at 1.0.
            static const DWORD bandColour[4] =
                { 0xFF595959, 0xFF33FFFF, 0xFFFF33FF, 0xFFFFFF33 };
            struct { float x, y, z, w; DWORD c; } v[4];
            float wBand = (float)d.Width / 5.0f;
            for (int b = 0; b < 4; b++) {
                float x0 = wBand * b, x1 = wBand * (b + 1);
                for (int k = 0; k < 4; k++) {
                    v[k].x = (k & 1) ? x1 : x0;
                    v[k].y = (k & 2) ? (float)d.Height : 0.0f;
                    v[k].z = 0.0f; v[k].w = 1.0f;
                    v[k].c = bandColour[b];
                }
                IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
            }
        }
        InterlockedIncrement(&g_aoTints);
        if (g_aoTints == 1)
            LogLine("[aotint] engaged: bands L->R = x0.35 all / R x0.2 / G x0.2 / B x0.2 / untouched");
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (oldRt) { g_origSetRT(dev, 0, oldRt); IDirect3DSurface9_Release(oldRt); }
    if (sb) { IDirect3DStateBlock9_Apply(sb); IDirect3DStateBlock9_Release(sb); }
    if (surf) IDirect3DSurface9_Release(surf);
}

// Called from HookedDrawIndexedPrimitive (15_msaa.c). Counts material-pass
// draws that execute with the shadow texture live on a sampler - the
// question binds cannot answer. Success = 120 frames with such draws.
static void AoDrawTick(void)
{
    LONG mask = g_aoStageMask;
    if (!mask || g_curPass != PASS_MS) return;
    for (int s = 0; s < 16; s++)
        if (mask & (1L << s)) InterlockedIncrement(&g_aoMsDrawStage[s]);
    {
        LONG fr = g_msFrameSeq;
        if (fr != g_aoLastDrawFrame) {
            g_aoLastDrawFrame = fr;
            if (InterlockedIncrement(&g_aoMsDrawFrames) >= 120 &&
                InterlockedCompareExchange(&g_aoReported, 1, 0) == 0)
                AoReconReport("SUCCESS - material draws execute with the shadow tex bound");
        }
    }
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
    if (tex && AoIsShadowTex((void *)tex)) {
        LONG p = pass;
        if (p < 0 || p > PASS_COUNT) p = PASS_COUNT;
        InterlockedIncrement(&g_aoTexPass[p]);
    }
    // Maintain the live per-stage mask for the draw-level check. Bindings
    // persist across passes, which is the entire point.
    if (g_aoRtCount && stage < 16) {
        if (AoIsShadowTex((void *)tex))
            InterlockedOr(&g_aoStageMask, 1L << stage);
        else if (g_aoStageMask & (1L << stage))
            InterlockedAnd(&g_aoStageMask, ~(1L << stage));
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
        // Poll RT0 on every SetTexture inside the pass (~380/s - trivial) and
        // maintain the SET of targets the ping-pong touches, plus which one
        // was last each frame.
        if (g_origGetRenderTarget) {
            IDirect3DSurface9 *s = NULL;
            __try {
                if (SUCCEEDED(g_origGetRenderTarget(dev, 0, &s)) && s) {
                    LONG n = g_aoRtCount, i;
                    if (n > AO_RT_MAX) n = AO_RT_MAX;
                    for (i = 0; i < n; i++)
                        if (g_aoRts[i].surf == (void *)s) break;
                    if (i == n && n < AO_RT_MAX) {
                        D3DSURFACE_DESC d;
                        if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) {
                            g_aoRts[n].w = (LONG)d.Width;
                            g_aoRts[n].h = (LONG)d.Height;
                            g_aoRts[n].fmt = (LONG)d.Format;
                        }
                        g_aoRts[n].tex = AoResolveContainer(s);
                        g_aoRts[n].surf = (void *)s;
                        g_aoRtCount = n + 1;
                        if (!g_aoShadowSurf) {   // report-continuity fields
                            g_aoShadowSurf = (void *)s;
                            g_aoShadowTex = g_aoRts[n].tex;
                            g_aoShadowW = g_aoRts[n].w;
                            g_aoShadowH = g_aoRts[n].h;
                            g_aoShadowFmt = g_aoRts[n].fmt;
                        }
                    }
                    if (i < AO_RT_MAX) {
                        // "Last RT0 of the pass this frame" - credited when the
                        // frame moves on (next frame's first poll).
                        LONG fr = g_msFrameSeq;
                        if (fr != g_aoLastRtFrame && g_aoLastRtIdx >= 0)
                            InterlockedIncrement(&g_aoRts[g_aoLastRtIdx].lastCount);
                        g_aoLastRtFrame = fr;
                        g_aoLastRtIdx = i;
                    }
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
    } else if (pass == PASS_MS) {
        // Tint probe: fire exactly at the once-per-frame s14 bind of the
        // final buffer - it is complete at this moment and about to be
        // consumed. Runs BEFORE forwarding the bind; the content is tinted
        // either way since the texture identity does not change.
        if (stage == 14 && tex && AoIsShadowTex((void *)tex)) {
            // v24g: dump AFTER the injection, not before. The open question
            // is no longer "what does the engine write" (answered by the
            // first dump) but "what did OUR draw do to it" - NORMAL mode
            // demonstrably renders yet shadows still vanish, which multiply
            // blending cannot mathematically cause. The post-injection BMP
            // is ground truth on whether the blend was honoured.
            int wantDump = (InterlockedCompareExchange(&g_aoDumpRequest, 0, 1) == 1);
#if ENABLE_AO_SSAO
            // Effective-mode logging (v24f). The entire v24..v24e loop was
            // spent chasing a "broken blend" that was actually the DEBUG
            // view running: the menu toggles persist to the ini instantly,
            // so a debug toggle flipped during one test armed every later
            // launch, and the debug replace erases engine shadows BY DESIGN
            // - which reads exactly like broken SSAO. The log now states
            // the effective mode on every change, so a session documents
            // which branch actually rendered.
            {
                static LONG lastMode = -1;
                LONG mode = !g_aoEnable ? 0 : (g_aoDebug ? 2 : 1);
                if (mode != lastMode) {
                    lastMode = mode;
                    LogLine(mode == 0 ? "[ssao] mode: OFF"
                          : mode == 1 ? "[ssao] mode: NORMAL (multiply into shadow term)"
                                      : "[ssao] mode: DEBUG (4-band diagnostic replaces shadows)");
                }
            }
            if (g_aoEnable)
                SsaoApply(dev, tex, 0);
#endif
            if (g_aoTint)
                AoTintBuffer(dev, tex);
            if (wantDump) {
                AoDumpBuffer(dev, tex);
                AoDumpDepthStats(dev);
            }
        }
        if (g_aoReported) return g_origSetTexture(dev, stage, tex);
        if (tex && AoIsShadowTex((void *)tex)) {
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
    } else if (pass == PASS_MENU && g_aoRawView && g_aoEnable) {
#if ENABLE_AO_SSAO
        // True-raw AO over the finished frame, at the start of the UI pass
        // so menus stay readable on top.
        SsaoApply(dev, NULL, 1);
#endif
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
