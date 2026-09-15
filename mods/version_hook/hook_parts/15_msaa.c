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

// ---- Engine-state shadow (v25h) -------------------------------------------
// Why this exists: the AO bisect proved that CreateStateBlock(D3DSBT_ALL) +
// Apply - with NOTHING in between - is not an identity operation on this
// device. One bracket per frame was enough to break the newest-loaded
// model's rendering (flat unlit geometry) and derail env-map matrices
// (user: reflections moving at 5x camera speed). So injected passes may not
// use state blocks AT ALL; they restore engine state explicitly instead.
//
// WHY it is not identity, confirmed 2026-08-16: the device is created with
// D3DCREATE_MULTITHREADED (BehaviorFlags=0x44, logged at creation; note
// PUREDEVICE is ABSENT, which refutes the first theory that state-block
// recording was failing on a Get-less device). The game drives D3D from
// more than one thread, so between our capture and our Apply another
// thread - the asset loader - can upload a freshly equipped model's
// textures, constants and matrices. Apply then writes the ENTIRE captured
// register file back, reverting that upload. The loader never repeats
// itself, so the model renders with missing constants forever: flat unlit
// geometry, and env-map matrices stuck at stale values. Equipment menus
// are the worst case because switching gear IS loading models.
//
// The lesson generalises: a full-state restore is only safe if this thread
// owns all device state, and here it does not. Restore ONLY what you
// touched. The values come from these shadows, written by OUR OWN HOOKS -
// the mod already intercepts every engine call to these methods, and
// injected code calls through g_orig* which bypasses the hooks, so the
// shadows hold engine truth by construction, with no Get* calls anywhere.
static DWORD g_esRs[256];                    // last engine value per render state
static unsigned char g_esRsKnown[256];       // 0 = engine never set it (use default)
static D3DVIEWPORT9 g_esVp;                  // last engine viewport
static volatile LONG g_esVpKnown = 0;
static void *g_esVs = NULL;                  // engine's current vertex shader
static void *g_esDecl = NULL;                // current vertex declaration
static DWORD g_esFvf = 0;                    // current FVF
static LONG  g_esDeclIsFvf = 0;              // which of decl/FVF was set last
static void *g_esStreamVb = NULL;            // stream 0 binding (UP draws clobber it)
static UINT  g_esStreamOffset = 0, g_esStreamStride = 0;
static void *g_esTex[16];                    // engine textures per stage (written in
                                             //   24's SetTexture hook)
static DWORD EsRs(D3DRENDERSTATETYPE s, DWORD def)
{
    return ((DWORD)s < 256 && g_esRsKnown[s]) ? g_esRs[s] : def;
}

// Shadow-only passthrough hooks for the state the AO bracket touches that
// nothing else intercepted. All low-frequency (per-material at worst).
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVertexShader)(
    IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetFVF)(IDirect3DDevice9 *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVertexDecl)(
    IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetStreamSource)(
    IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT);
static PFN_SetVertexShader g_origSetVertexShader = NULL;
static PFN_SetFVF g_origSetFVF = NULL;
static PFN_SetVertexDecl g_origSetVertexDecl = NULL;
static PFN_SetStreamSource g_origSetStreamSource = NULL;

static HRESULT STDMETHODCALLTYPE HookedSetVertexShader(
    IDirect3DDevice9 *This, IDirect3DVertexShader9 *sh)
{
    g_esVs = (void *)sh;
    return g_origSetVertexShader(This, sh);
}
static HRESULT STDMETHODCALLTYPE HookedSetFVF(IDirect3DDevice9 *This, DWORD fvf)
{
    g_esFvf = fvf;
    g_esDeclIsFvf = 1;
    return g_origSetFVF(This, fvf);
}
static HRESULT STDMETHODCALLTYPE HookedSetVertexDecl(
    IDirect3DDevice9 *This, IDirect3DVertexDeclaration9 *d)
{
    g_esDecl = (void *)d;
    g_esDeclIsFvf = 0;
    return g_origSetVertexDecl(This, d);
}
static HRESULT STDMETHODCALLTYPE HookedSetStreamSource(
    IDirect3DDevice9 *This, UINT num, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{
    if (num == 0) {
        g_esStreamVb = (void *)vb;
        g_esStreamOffset = off;
        g_esStreamStride = stride;
    }
    return g_origSetStreamSource(This, num, vb, off, stride);
}
#define VP_SEEN_MAX 64
typedef struct { LONG pass; DWORD w, h, x, y; } VpSeen;
static VpSeen g_vpSeen[VP_SEEN_MAX];
static volatile LONG g_vpSeenCount = 0;

static HRESULT STDMETHODCALLTYPE HookedSetViewport(
    IDirect3DDevice9 *This, const D3DVIEWPORT9 *pVp)
{
    // Engine-state shadow (v25h): the AO bracket restores this viewport.
    if (pVp) { g_esVp = *pVp; g_esVpKnown = 1; }
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
// Failed-combination latch. Creation failure releases the pair, which nulls
// g_msColour, which is exactly the condition the create branch tests - so a
// failing configuration retried on EVERY scene-target bind, several times a
// frame. At 15360x8640 x8 (8K + SSAA 2x + MSAA 8x) the colour surface
// SUCCEEDS at about 4 GB and only the depth fails, so each retry allocated
// and freed multiple gigabytes. User-measured cost: 54 fps -> 24, with large
// regular spikes, on a configuration where MSAA was not even engaged.
//
// Remembering the exact triple that failed is what makes the latch safe: it
// suppresses only the combination already proven impossible, so changing
// resolution, SSAA scale or sample count all retry normally, and a device
// Reset clears it outright.
static LONG g_msFailW = 0, g_msFailH = 0, g_msFailSamples = 0;
static volatile LONG g_msReported = 0;
// Grab-effect intervention counters (the [grab] wrapper in 16): sync-resolves
// before a mid-episode read (hole A), foreign writes into the latched scene
// surface (hole C), and substitutions suppressed while a foreign write's
// frame plays out.
// (tentative defs in 03_render_state.c - the status panel prints these and
// compiles earlier; assigning here would make two definitions.)
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
    // Full release means a Reset or a teardown, i.e. the conditions that made
    // creation fail may be gone. Only MsaaReleaseSurfaces (the rebuild path)
    // leaves the latch standing.
    g_msFailW = g_msFailH = g_msFailSamples = 0;
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

#if ENABLE_CUTOUT_PROBE
// Six compares on the draw path, diagnostic build only - see CutoutProbeTick.
static void CutoutProbeDraw(void)
{
    LONG pi = g_curPsIdx, p = g_curPass, i;
    DWORD h;
    if (pi < 0 || pi >= PS_MAP_MAX) return;
    if (p < 0 || p >= PASS_COUNT) return;
    h = g_psMap[pi].hash;
    for (i = 0; i < (LONG)CP_N; i++)
        if (g_cpHashes[i] == h) { g_cpDraws[i][p]++; return; }
}
#endif

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive(
    IDirect3DDevice9 *This, D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
    UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    HRESULT hr;
#if ENABLE_CASCADE_WATCH
    CascadeWatchDraw();
#endif
#if ENABLE_CUTOUT_PROBE
    CutoutProbeDraw();
#endif
#if ENABLE_AO_RECON
    // Draw-level consumption check (24_ao_recon.c). Bind-level counting gave
    // a false NEVER: D3D9 sampler state persists across passes, so a texture
    // bound at the end of MS_SHADOW is sampled by every material draw without
    // any SetTexture call in the material pass. Only draws can answer it.
    AoDrawTick();
#endif
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
#if ENABLE_CASCADE_WATCH
    CascadeWatchDraw();
#endif
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
    // Engine-state shadow (v25h): one store on the hot path. The AO bracket
    // restores its touched render states from this instead of a state block.
    if ((DWORD)State < 256) { g_esRs[State] = Value; g_esRsKnown[State] = 1; }
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
