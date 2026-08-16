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

// ---- In-place scene resolve (2026-08-13, the 720p fix) ---------------------
// The backbuffer-copy reroute below turned out to depend on a coincidence:
// the engine only presents via StretchRect when the internal resolution
// EQUALS the backbuffer size (chosen x scale == desktop - i.e. 1080p x2 on a
// 4K display, and nothing else). Everywhere else the final hop is the
// engine's own scaling DRAW (all three of its Scaling modes; probed
// 2026-08-13: zero backbuffer-bound StretchRects at 720p x2), which a blit
// hook cannot see.
//
// So stop chasing the final copy. At the entry of the DRAW_FILTER pass - the
// scene is complete, post has not read it yet - downsample the scene target
// to the CHOSEN resolution and blit straight back up, both LINEAR. The
// engine then presents through whatever path it likes, at any resolution and
// any Scaling mode, but the image it presents carries chosen-res information
// with real box-filter AA. Two bonuses over the old reroute: post/bloom
// operate on the antialiased image, and the UI - drawn after this - stays
// native-crisp, which was the original "scene only, excluding UI" goal (the
// old reroute softened UI along with the scene).
//
// MSAA interplay: under MSAA the engine's scene target may be stale at this
// instant (content still in the substituted MS target until its resolve
// fires on the next RT switch), so this engages only with MsaaSamples=0; the
// old reroute below stays as the fallback for the MSAA+SSAA combination.
static IDirect3DSurface9 *g_ssaaResolveRt = NULL;
static UINT g_ssaaResolveW = 0, g_ssaaResolveH = 0;
static volatile LONG g_ssaaResolves = 0;
static volatile LONG g_ssaaResolveFails = 0;

static void SsaaReleaseIntermediate(void)
{
    if (g_ssaaInter) {
        IDirect3DSurface9_Release(g_ssaaInter);
        g_ssaaInter = NULL;
    }
    g_ssaaInterW = g_ssaaInterH = 0;
    // Same lifecycle: D3DPOOL_DEFAULT, must not survive a Reset.
    if (g_ssaaResolveRt) {
        IDirect3DSurface9_Release(g_ssaaResolveRt);
        g_ssaaResolveRt = NULL;
    }
    g_ssaaResolveW = g_ssaaResolveH = 0;
}

// Called from Detour_filter at the pass handler's entry: main thread, mid
// frame, between passes - the same context class the MSAA resolves already
// run device calls from.
void __cdecl SsaaInPlaceResolve_C(void)
{
    static LONG lastSeq = -1;
    static LONG loggedW = 0, loggedH = 0;
    if (!g_ssaaActive || !g_ssaaOutputRes || g_msaaSamples != 0) return;
    if (!g_dev || !g_sceneRtMain || !g_mainModBase) return;
    if (g_msFrameSeq == lastSeq) return;      // once per frame
    lastSeq = g_msFrameSeq;

    LONG chosenW = 0, chosenH = 0;
    __try {
        DWORD so = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
        if (so) { chosenW = *(LONG *)(so + 0x10); chosenH = *(LONG *)(so + 0x14); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (chosenW < 320 || chosenH < 200) return;

    IDirect3DSurface9 *scene = (IDirect3DSurface9 *)g_sceneRtMain;
    D3DSURFACE_DESC sd;
    if (FAILED(IDirect3DSurface9_GetDesc(scene, &sd))) return;
    // Only when the scene genuinely overshoots the chosen resolution.
    if (sd.Width <= (UINT)chosenW || sd.Height <= (UINT)chosenH) return;

    if (g_ssaaResolveRt && (g_ssaaResolveW != (UINT)chosenW ||
                            g_ssaaResolveH != (UINT)chosenH)) {
        IDirect3DSurface9_Release(g_ssaaResolveRt);
        g_ssaaResolveRt = NULL;
        g_ssaaResolveW = g_ssaaResolveH = 0;
    }
    if (!g_ssaaResolveRt) {
        // The SCENE's format (A8R8G8B8), not the backbuffer's - this surface
        // round-trips scene content, it never touches the swap chain.
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(
                g_dev, (UINT)chosenW, (UINT)chosenH, sd.Format,
                D3DMULTISAMPLE_NONE, 0, FALSE, &g_ssaaResolveRt, NULL))) {
            InterlockedIncrement(&g_ssaaResolveFails);
            return;
        }
        g_ssaaResolveW = (UINT)chosenW;
        g_ssaaResolveH = (UINT)chosenH;
    }

    HRESULT h1 = g_origStretchRect(g_dev, scene, NULL, g_ssaaResolveRt, NULL,
                                   D3DTEXF_LINEAR);
    HRESULT h2 = FAILED(h1) ? h1
               : g_origStretchRect(g_dev, g_ssaaResolveRt, NULL, scene, NULL,
                                   D3DTEXF_LINEAR);
    if (SUCCEEDED(h2)) {
        InterlockedIncrement(&g_ssaaResolves);
        if (loggedW != chosenW || loggedH != chosenH) {
            loggedW = chosenW; loggedH = chosenH;
            char l[176];
            sprintf(l, "[ssaa] in-place resolve engaged: scene %ux%u -> %ldx%ld -> back"
                       " (pre-post, pre-UI; presentation path untouched)",
                    sd.Width, sd.Height, chosenW, chosenH);
            LogLine(l);
        }
    } else {
        InterlockedIncrement(&g_ssaaResolveFails);
    }
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
                             ^ ((unsigned int)Filter << 17)
                             // Rect PRESENCE is part of the shape: a full-surface
                             // rect computes the same sw/sh as a NULL rect, but the
                             // output-res reroute only accepts NULL - which is the
                             // 720p bug hypothesis this probe exists to test.
                             ^ (pSrcRect ? 1u << 21 : 0) ^ (pDstRect ? 1u << 22 : 0);
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
                sprintf(l, "[ssaa-probe] %-18s %ldx%ld fmt=%d -> %ldx%ld fmt=%d  filter=%s(%d)  rects=%s/%s%s%s%s",
                        g_passNames[p], sw, sh, (int)s.Format, dw, dh, (int)d.Format,
                        fname, (int)Filter,
                        pSrcRect ? "SRC" : "null", pDstRect ? "DST" : "null",
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
    // ---- SSAA: honour the chosen resolution (MSAA fallback path) ---------
    // DEMOTED 2026-08-13: this only ever fires when the engine presents via
    // StretchRect, which requires internal size == backbuffer size - i.e.
    // chosen x scale == desktop, one arithmetic coincidence (1080p x2 on 4K).
    // Everywhere else the final hop is the engine's scaling DRAW and this
    // hook never sees it. SsaaInPlaceResolve_C above is the real fix; this
    // block remains only for the MSAA+SSAA combination, where the in-place
    // resolve must not run (the scene target can be stale mid-frame under MS
    // substitution) - so MSAA users keep the old geometry-limited behaviour
    // rather than losing the feature outright.
    if (g_msaaSamples != 0 &&
        g_ssaaActive && g_ssaaOutputRes && pSrc && pDst && !pSrcRect && !pDstRect &&
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

            // Decision transparency (2026-08-13, the 720p bug). The probe run
            // proved the reroute declines at a 720p setting while every
            // condition LOOKS satisfiable from outside, so log the actual
            // tuple it evaluates, deduplicated per distinct combination. One
            // eval per frame at most, a handful of distinct tuples per
            // session - noise-free by construction.
            {
                static unsigned int evalSeen[16];
                static LONG evalCount = 0;
                unsigned int esig = ((unsigned int)chosenW << 1) ^ ((unsigned int)chosenH << 5)
                                  ^ (s.Width << 9) ^ (s.Height << 13)
                                  ^ (d.Width << 17) ^ (d.Height << 21);
                LONG en = evalCount, ei, efound = 0;
                if (en > 16) en = 16;
                for (ei = 0; ei < en; ei++) if (evalSeen[ei] == esig) { efound = 1; break; }
                if (!efound && en < 16) {
                    evalSeen[en] = esig;
                    evalCount = en + 1;
                    char el[192];
                    sprintf(el, "[ssaa] reroute-eval chosen=%ldx%ld src=%ux%u dst=%ux%u -> %s",
                            chosenW, chosenH, s.Width, s.Height, d.Width, d.Height,
                            (chosenW >= 320 && chosenH >= 200 &&
                             (UINT)chosenW < d.Width && (UINT)chosenH < d.Height &&
                             s.Width > (UINT)chosenW) ? "REROUTE" : "decline");
                    LogLine(el);
                }
            }
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
        // g_msaaDepth1x is the retired prepass-bypass experiment (see its
        // declaration: broke occlusion, and its premise was disproved by
        // SGSSAA showing the same artifact). Tested LAST on purpose -
        // SetRenderTarget is extremely hot, and this way the volatile load
        // only happens once the depth-RT latch has already matched, which is
        // a handful of times per frame rather than every call.
        if (samples >= 1 && pRT && g_depthRtMain && (void *)pRT == g_depthRtMain &&
            g_msDepth && g_msW && !g_msaaDepth1x) {
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
// ---- PARKED (2026-08-15): shadow shimmer, and why this is the lever -------
// User report: shadows "shimmer when looked at an angle... lots of shadow
// detail at an angle almost looks like aliasing". Parked at their request -
// documented here rather than in a notes file because this function is where
// any fix would go, and whoever reads it next needs to know that.
//
// There are TWO distinct phenomena behind that description and they need
// opposite fixes. Which one it is has not been established:
//
//   TEMPORAL crawl (edges swim while the camera moves, stable when still).
//   Cause: the cascade projection is recomputed each frame with sub-texel
//   offsets, so the whole depth map slides under the geometry. Standard fix
//   is texel snapping, and THIS FUNCTION is the place: D3D9 row-vector
//   convention puts the projection's translation at elements 12 (X) and 13
//   (Y), so rounding those to whole shadow-map texels
//     texel   = 2.0f / shadowMapRes          (clip space spans -1..1)
//     m[12]   = roundf(m[12] / texel) * texel
//   stabilises it. Two hard requirements, both learned here already:
//     - the SAMPLING side must get the identical adjustment, or shadows
//       drift with the camera (the documented step-2 failure). Compute the
//       offset from the ORIGINAL matrix so MaybePropagateCascade can derive
//       exactly the same value.
//     - snapping only fully works if the cascade EXTENT is stable frame to
//       frame. The engine computes extents per-scene, so this holds within
//       an area but should be verified before trusting the result.
//   Note the gate below returns early unless g_nearCascadePct is set, so a
//   stabilisation option needs its own gate - it must work at Standard too.
//
//   SPATIAL aliasing (grainy/jagged at grazing angles, present when still).
//   Cause: perspective aliasing - at a grazing angle one shadow texel covers
//   many screen pixels. NOT fixable here; the projection is not the problem.
//   The lever is the shadow sampling shader (wider or better PCF), reachable
//   the same way the FXAA passthrough was.
//
// Resolution does not fix either one on its own: raising ShadowMapRes scales
// every cascade equally (confirmed - at 8192 the tracked surfaces are
// 8192x16384 plus 8192x8192 twice), so near and far keep the same relative
// texel density and far stays proportionally soft. Rebalancing THAT would
// need per-cascade resolution, which no known field provides.
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
        g_presentWindowed = (LONG)(pPP->Windowed ? 1 : 0);
        g_presentFmt = (LONG)pPP->BackBufferFormat;
        char l[224];
        sprintf(l, "[rt] Reset: %ux%u backbufFmt=%d MULTISAMPLE=%d qual=%lu autoDepth=%d depthFmt=%d windowed=%d interval=0x%lX",
                pPP->BackBufferWidth, pPP->BackBufferHeight, (int)pPP->BackBufferFormat,
                (int)pPP->MultiSampleType, (unsigned long)pPP->MultiSampleQuality,
                (int)pPP->EnableAutoDepthStencil, (int)pPP->AutoDepthStencilFormat,
                (int)pPP->Windowed, (unsigned long)pPP->PresentationInterval);
        LogLine(l);
    }
    ClearShadowSurfaces();   // recorded addresses are meaningless across a Reset
#if ENABLE_AO_RECON
    // Same rule for the AO latches: Reset destroys and recreates every
    // render target, so the shadow-buffer set and depth container are dead
    // pointers afterwards - and stale latches never match the new objects,
    // which left SSAO silently OFF for the rest of the session (user-found,
    // 2026-08-16: any resolution change or focus-elevation broke it). The
    // latches rebuild automatically on the first MS_SHADOW pass after Reset.
    AoReconReset();
#endif
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
    // Engine-state shadow hooks (v25h, see 15_msaa.c): shadow-only
    // passthroughs on SetVertexShader/SetFVF/SetVertexDeclaration/
    // SetStreamSource, so the AO bracket can restore engine state without a
    // state block (whose Apply the bisect proved non-identity on this
    // device) and without Get* calls (which a pure device would lie to).
    {
        int slotVSh = offsetof(IDirect3DDevice9Vtbl, SetVertexShader) / sizeof(void *);
        g_origSetVertexShader = (PFN_SetVertexShader)ResolveOrigSlot(vtbl[slotVSh]);
        if (VirtualProtect(&vtbl[slotVSh], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
            vtbl[slotVSh] = (void *)HookedSetVertexShader;
            VirtualProtect(&vtbl[slotVSh], sizeof(void *), oldProtect, &oldProtect);
        }
        int slotFvf = offsetof(IDirect3DDevice9Vtbl, SetFVF) / sizeof(void *);
        g_origSetFVF = (PFN_SetFVF)ResolveOrigSlot(vtbl[slotFvf]);
        if (VirtualProtect(&vtbl[slotFvf], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
            vtbl[slotFvf] = (void *)HookedSetFVF;
            VirtualProtect(&vtbl[slotFvf], sizeof(void *), oldProtect, &oldProtect);
        }
        int slotVD = offsetof(IDirect3DDevice9Vtbl, SetVertexDeclaration) / sizeof(void *);
        g_origSetVertexDecl = (PFN_SetVertexDecl)ResolveOrigSlot(vtbl[slotVD]);
        if (VirtualProtect(&vtbl[slotVD], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
            vtbl[slotVD] = (void *)HookedSetVertexDecl;
            VirtualProtect(&vtbl[slotVD], sizeof(void *), oldProtect, &oldProtect);
        }
        int slotSS = offsetof(IDirect3DDevice9Vtbl, SetStreamSource) / sizeof(void *);
        g_origSetStreamSource = (PFN_SetStreamSource)ResolveOrigSlot(vtbl[slotSS]);
        if (VirtualProtect(&vtbl[slotSS], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
            vtbl[slotSS] = (void *)HookedSetStreamSource;
            VirtualProtect(&vtbl[slotSS], sizeof(void *), oldProtect, &oldProtect);
        }
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
#if ENABLE_AO_RECON
    InstallAoReconHook(vtbl);
#endif
#if ENABLE_FOV_PROBE
    // Only free to take because ENABLE_CASCADE_HUNT is 0 - it owns this slot
    // when armed. If that ever comes back, one of the two has to give.
    InstallFovProbe(vtbl);
#endif
    LogLine("[rt] render-target inventory probe installed (CreateRenderTarget/CreateDepthStencilSurface/SetRenderTarget)");

    // GPU vendor, for the alpha-to-coverage backdoor (AMD and NVIDIA spell it
    // differently and each other's spelling is at best a no-op, at worst a
    // garbage point size). Asked of the device's own adapter, not adapter 0.
    {
        D3DDEVICE_CREATION_PARAMETERS cp;
        IDirect3D9 *d3d = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)) &&
            SUCCEEDED(IDirect3DDevice9_GetDirect3D(dev, &d3d)) && d3d) {
            {
                // The state-block postmortem's key evidence. Measured 0x44 =
                // HARDWARE_VERTEXPROCESSING | MULTITHREADED, with PUREDEVICE
                // ABSENT - which killed the first theory (Get-less pure
                // device breaking CreateStateBlock's recording) and named the
                // real one: multithreaded device access, so a full-state
                // Apply reverts whatever another thread did in the meantime.
                // See the note at the engine-state shadows in 15_msaa.c.
                char l[96];
                sprintf(l, "[rt] device BehaviorFlags=0x%08lX%s",
                        (unsigned long)cp.BehaviorFlags,
                        (cp.BehaviorFlags & D3DCREATE_PUREDEVICE) ? "  (PUREDEVICE)" : "");
                LogLine(l);
            }
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

