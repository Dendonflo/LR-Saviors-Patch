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
// In-game tuning panel (26_ingame_ui.c, later in the TU). Runs at Present so
// the panel is part of the frame - the whole point of its existence is that
// no Win32 window is involved. AFTER the MSAA backstop, so it draws onto the
// resolved image.
static void IgPresent(IDirect3DDevice9 *dev);
// One-shot pipeline splitter for the 4K black screen (defined with the MSAA
// block): MsaaResolve arms state 2 after dumping the resolved scene texture;
// the next Present dumps the backbuffer, i.e. what the user actually sees.
static void DumpSurfaceToBmp(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, const char *name);
static volatile LONG g_msDumpState;   // 0 idle, 2 backbuffer pending, 3 done

// One-shot proof that presents actually route through these hooks - the
// in-game panel renders here, so "panel never appears" with no other line is
// indistinguishable from "Present never hooked" without it. Shared by both
// Present and PresentEx, and [boot]-tagged to survive the release filter.
static volatile LONG g_presentAliveLogged = 0;
#define PRESENT_ALIVE_ONCE(which) \
    do { if (InterlockedCompareExchange(&g_presentAliveLogged, 1, 0) == 0) \
        LogLine("[boot] present hook alive (" which ")"); } while (0)

// ---- EndScene: the injection point that actually fires ---------------------
// The chain trace for the in-game panel proved what the retirement note in 16
// had already recorded: NEITHER device-level Present has ever fired in this
// game (the "present hook alive" one-shot stayed silent from frame 1), and
// the swap-chain Present hook is a confirmed first-frame crash, retired
// undiagnosed. So anything that must run once per frame inside the frame -
// the panel, and the MSAA resolve backstop that silently never ran either -
// hooks EndScene instead: the canonical D3D9 overlay point (the Steam
// overlay's own choice), guaranteed inside a Begin/EndScene bracket, on the
// same vtable where our other slot patches are proven safe by precedent.
//
// Drawn at EVERY EndScene deliberately: if the engine brackets the frame
// once (the common case) this is exactly "after all drawing, before
// present"; if it brackets per-pass, the last bracket's draw is the one
// that survives, and the earlier ones cost a quad each. Measure before
// optimising - the alive line says which world we are in.
typedef HRESULT (STDMETHODCALLTYPE *PFN_EndScene)(IDirect3DDevice9 *);
static PFN_EndScene g_origEndScene = NULL;

static HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9 *This)
{
    static volatile LONG esAlive = 0;
    if (InterlockedCompareExchange(&esAlive, 1, 0) == 0)
        LogLine("[boot] EndScene hook alive (panel + MSAA backstop moved here)");
    // The MSAA backstop, relocated from the Present hooks where it never ran.
    // Resolving here is legal and idempotent: hasContent set means the scene
    // episode is still open, the resolve syncs the engine's texture, and the
    // next bind of the scene target re-substitutes exactly as after any
    // resolve.
    if (g_msHasContent) { MsaaResolve(This); MsaaRestoreDepth(This); }
    if (g_msR32fHasContent) MsaaResolveR32f(This);
    // End of frame is the right moment for this: what it pushes survives
    // into the next frame's draws until the engine overwrites it, which is
    // the case it exists for. No-op unless the level actually changed.
    TexFilterFrameTick(This);
    IgPresent(This);
    return g_origEndScene(This);
}

static HRESULT STDMETHODCALLTYPE HookedDevicePresent(
    IDirect3DDevice9 *This, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
    PRESENT_ALIVE_ONCE("Present");
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
    IgPresent(This);
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
    // Mip-chain census (08d_texfilter.c). Counts only; the "mipmapping
    // issues" reports need a way to tell a sampler problem from a content
    // problem, and a large surface texture created with exactly one level is
    // the content one.
    TexFilterNoteTexture(Width, Height, Levels, Usage, Format);
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
    // A failed creation was previously SILENT unless it hit a special path.
    // The MSAA grab-effect hunt needs this visible: if an effect module
    // allocates its buffers on demand and the allocation fails under the MS
    // pair's memory pressure, the engine skips the effect gracefully and no
    // other counter moves. FAILED keeps it past the release log filter.
    if (FAILED(hr)) {
        static volatile LONG texFailLogged = 0;
        if (InterlockedIncrement(&texFailLogged) <= 12) {
            char l[224];
            sprintf(l, "[d3d9] CreateTexture FAILED hr=0x%08lX %ux%u levels=%u"
                       " usage=0x%lX fmt=%d pool=%d",
                    (unsigned long)hr, Width, Height, Levels,
                    (unsigned long)origUsage, (int)Format, (int)origPool);
            LogLine(l);
        }
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
    PRESENT_ALIVE_ONCE("PresentEx");
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
    IgPresent((IDirect3DDevice9 *)This);
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

