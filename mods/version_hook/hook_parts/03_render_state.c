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
// Presentation mode and backbuffer format, captured alongside the size at
// CreateDevice and Reset. -1 = not seen yet, distinct from "windowed" (0).
//
// NOT SUITABLE AS A USER-FACING RESOLUTION. Tried on the status panel and
// removed: at a 1080p fullscreen setting on a 4K display these report
// 3840x2160 and "windowed", because the engine presents into a desktop-sized
// backbuffer and uses a borderless window rather than exclusive fullscreen.
// The values are what D3D was actually handed; they simply do not match what
// the player set. Anything wanting a recognisable resolution has to derive it
// from the engine's internal size instead.
static volatile LONG g_presentWindowed = -1;
static volatile LONG g_presentFmt = 0;
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
// RETIRED 2026-08-13, confirmed broken AND aimed at the wrong mechanism.
//
// The experiment: leave the MS_DEPTH prepass on the engine's own 1x surfaces,
// so the screen-space shadow pass would read exact per-pixel depth rather
// than a sample-averaged one. Two results, both useful:
//
//  1. Occlusion broke exactly as predicted - sky/sea over the world. So the
//     colour pass really does depend on the prepass having filled the MS
//     depth (z-write off, LESSEQUAL against prepass Z). That dependency is
//     now CONFIRMED rather than inferred, which is worth keeping.
//
//  2. The premise was wrong anyway. The theory was "the resolve averages
//     depth, so edge pixels get a depth belonging to neither surface". But
//     NVIDIA's SGSSAA - per-sample shading, per-sample depth, the exact
//     thing that theory says would fix it - STILL shows the artifact, merely
//     antialiased (user-observed). If per-sample depth does not remove it,
//     depth averaging is not the cause.
//
// The mechanism that survives both observations is resolution, not sampling:
// the engine reconstructs shadows into a SCREEN-SPACE buffer that is not
// multisampled and defaults to HALF resolution. One shadow verdict covers a
// 2x2 pixel neighbourhood, so at a foreground/background boundary the whole
// neighbourhood inherits whichever surface dominates the texel - the
// background's shadow goes missing behind a partially-covered edge. That
// predicts every observation: MSAA cannot help (the buffer is not MS),
// SGSSAA only shrinks and averages the artifact (everything is bigger, the
// artifact included), and ShadowBufResPct should reduce it directly.
//
// Left as a dead flag rather than deleted: re-adding its line to g_toggles[]
// is all it takes to run the experiment again. Not in the table today, so it
// cannot be switched on from the ini and cannot break anyone's game.
static volatile LONG g_msaaDepth1x = 0;
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

// Cutscene-aware override. Cutscenes are authored against the engine's own
// cascade splits, so scaling them breaks shadows in some of them (reported in
// game). While a cutscene is playing an active percentage is neutralised to
// 100 - i.e. the engine's own value, scaled by 1.0 - rather than switched off,
// so the write stays on the guarded, idempotent, baseline-tracking path.
// See the long note in ApplyCascadeSplitSource for why 100 and not 0. The
// user's setting is untouched and resumes automatically on the way out.
//
// Detection lives in 21_cutscene_shadow.c and is deliberately data-driven
// (offset + mask from the ini) so candidate signals can be tried without a
// rebuild while the right one is being pinned down.
// Frametime overlay position, same scheme as the status panel below. The
// four-corner "Overlay Position" menu was retired in favour of dragging the
// window directly - a corner picker is a poor substitute for putting it
// exactly where you want it, and two windows with different placement models
// was one model too many. g_overlayPos is kept (ini key OverlayPos) only so
// an existing config carrying it is still parsed rather than rejected;
// nothing reads it any more.
static volatile LONG g_overlayX = -1;
static volatile LONG g_overlayY = -1;

// Status panel position, persisted so it stays where it was dragged. -1/-1
// means "never moved": the panel auto-places itself opposite the frametime
// graph until the user puts it somewhere, and from then on its own position
// wins. Without this it would snap back to the corner every launch, and
// jump across the screen whenever the graph's corner setting changed.
static volatile LONG g_statusX = -1;
static volatile LONG g_statusY = -1;

// Tentative def; real one in 14_aa_shaders.c, which the manifest includes
// after the overlay. The status panel needs it to distinguish "FXAA removal
// is switched on" from "the passthrough has actually been substituted".
static volatile LONG g_fxaaSubs;

// Tentative def; the real one (with its initialiser) is in 15_msaa.c, which
// the manifest includes after the overlay. The status panel needs it to show
// whether MSAA actually latched onto the scene target, so it has to be
// visible this early - same pattern as g_mainModBase below.
static volatile LONG g_msSubstitutions;

static volatile LONG g_cutsceneActive;      // 1 while a cutscene is detected
static volatile LONG g_cutsceneEdges;       // transition count, for the log
static volatile LONG g_cutsceneSuppressed;  // frames we forced the split off
// ini-tunable; registered in 08_config_persist.c, which is included before
// 21_cutscene_shadow.c, so these have to be defined here rather than there.
//
// CONFIRMED 2026-08-14 by the pass-2 correlation run: CinemaController+0x5c
// is a clean "a cinema is active" boolean. Across a whole session it had
// exactly four transitions and no noise at all - 1 for a ~57-frame talk
// cinema, then 1 for frames 3321..18623, which is precisely the long cutscene
// the run was built around. Mask 1, offset 0x5c (92 decimal in the ini).
static volatile LONG g_cutsceneRevert   = 1;     // feature master switch
static volatile LONG g_cutsceneFlagOff  = 0x5c;  // CinemaController +0x5c (mode 1 only)
static volatile LONG g_cutsceneFlagMask = 1;     // bit 0 (mode 1 only)
// Detection mode. 1 = the controller flag at +0x5c; 2 = named cut slots.
//
// Mode 1 was the first shipping attempt and is WRONG for this purpose:
// confirmed in game that +0x5c also rises for ordinary NPC dialogue and even
// for UI prompts (the teleporter asking whether to go back down), none of
// which involve a cinematic camera. It is really "a cinema context exists",
// not "a cutscene is playing". Kept selectable rather than deleted.
//
// Mode 2 reads the cut player's slots and requires the cut's NAME to look
// cinematic - the real cutscene was "cut_f406" while conversations were
// "en_npc_*" and bare character names. Default.
static volatile LONG g_cutsceneMode = 2;
// Name of the cut currently holding the shadow distance, "" when none. Shown
// on the status panel so the reason for a trigger is visible at a glance
// instead of needing the log.
static char g_cutsceneName[24];
// Detection is INSTANT in both directions, by request. A frame-count debounce
// was built and removed: the flag also rises for very short conversation
// cinemas (the correlation run caught one of ~57 frames, cut name "en_npc_0"),
// so the concern was a visible shadow pop twice per NPC chat. Reacting
// immediately is the shipped behaviour; if talk cinemas do turn out to pop,
// the fix is a hold counter here, not a change anywhere else.
// ---- Heap compactor deferral (v18b, EXPERIMENTAL, default OFF) -----------
// SQEX::CDev::Engine::Memory::Alternative::SeparateHeapSpace runs its
// compactor (FUN_00b46c20, vtable +0x14 wrapper FUN_00b47ac0) ~2x/frame
// from the main-thread job dispatcher. Normally 0.2-0.5us; in NPC-dense
// areas single passes balloon to 3-10ms (measured 2026-08-15, Ruffian) and
// consecutive ballooned passes are the dominant remaining stutter. A pass
// cannot be interrupted midway, but it CAN be skipped entirely: the block
// list is consistent between passes and an uncompacted heap just stays
// fragmented until the next tick - compaction is housekeeping, not part of
// any allocation's success path (fixed cadence regardless of load).
// Two mechanisms, both per-frame:
//   - budget: once passes have cost g_compactorBudgetUs in one frame,
//     further passes that frame are skipped (bounds the per-frame total,
//     minus one unavoidable overrun since a running pass can't be stopped).
//   - cooldown: after any single pass exceeds the budget, skip ALL passes
//     for COMPACTOR_COOLDOWN_FRAMES frames - turns "8ms every frame for 9
//     seconds" into "8ms every Nth frame", spreading the storm.
// Risk being tested: sustained deferral during heavy churn could let
// fragmentation grow until some allocation fails in a way the engine
// handles badly. Default OFF until a Ruffian A/B says otherwise.
static volatile LONG g_compactorDeferEnabled = 0;
static volatile LONG g_compactorBudgetUs = 2000;
static volatile LONG g_compactorSkips;       // window counter, monitor resets
static volatile LONG g_compactorFrameUs;     // spent this frame
static volatile LONG g_compactorSeq;         // frame the accumulator belongs to
static volatile LONG g_compactorCooldown;    // frames left to skip
// Configurable since the first Ruffian A/B (2026-08-15): at 4, the gate
// erased the 30ms+ heavy tail (17 -> 6 captures, none of the survivors
// compactor-related). The second A/B settled the value: 8 deferred twice
// the passes (465 vs 224) for an identical severity histogram (64 vs 66
// captures, same shape) - the surviving compactor events are the FIRST
// pass after each cooldown expiry, which no cooldown length can prevent,
// and they were already down to 11 of 64 records (from 108/222 ungated).
// 4 is the shipping value: same result as 8 with half the fragmentation
// window. The remaining stutter population is d3dx9 texture processing
// and driver waits - different families, different levers.
static volatile LONG g_compactorCooldownFrames = 4;

// ---- D3DX call attribution state (v19, 22_d3dx_diag.c) -------------------
// Declared here rather than in part 22 because the monitor (part 11) prints
// the per-window line and compiles first in the TU. All logic lives in 22.
#if ENABLE_D3DX_DIAG
typedef struct {
    const char *name;
    void *real;
    volatile LONG calls;
    volatile LONG slowCalls;       // > 1ms
    volatile LONG sumUsec;         // windowed - monitor resets
    volatile LONG maxUsec;         // windowed - monitor resets
} D3dxFn;

enum {
    DX_CompileShaderFromFileA, DX_GetShaderConstantTable, DX_DebugMute,
    DX_CompileShader, DX_GetPixelShaderProfile, DX_GetVertexShaderProfile,
    DX_LoadSurfaceFromMemory, DX_LoadVolumeFromMemory,
    DX_CreateTextureFromFileInMemoryEx, DX_COUNT
};

static D3dxFn g_d3dxFns[DX_COUNT] = {
    { "D3DXCompileShaderFromFileA" },
    { "D3DXGetShaderConstantTable" },
    { "D3DXDebugMute" },
    { "D3DXCompileShader" },
    { "D3DXGetPixelShaderProfile" },
    { "D3DXGetVertexShaderProfile" },
    { "D3DXLoadSurfaceFromMemory" },
    { "D3DXLoadVolumeFromMemory" },
    { "D3DXCreateTextureFromMemEx" },   // shortened for log width
};
#endif

// Texture-upload census counters (23_upload_gate.c). Tentative definitions:
// the monitor in part 11 prints them and compiles before part 23 defines the
// rest of the machinery. Same pattern as g_fxaaSubs / g_msSubstitutions.
#if ENABLE_UPLOAD_GATE
static volatile LONG g_ugTotal;
static volatile LONG g_ugFast;
static volatile LONG g_ugNpot;
static volatile LONG g_ugFmt;
static volatile LONG g_ugBoth;
static volatile LONG g_ugFastUsec;   // cumulative memcpy-path time
static volatile LONG g_ugSlowUsec;   // cumulative D3DX-path time
static volatile LONG g_ugSlowMaxUsec;
static volatile LONG g_tcTotal;      // DDS textures created
static volatile LONG g_tcNpot;       // ...of which non-power-of-two
#endif

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
    // EFFECTIVE percentages. Everything below reads these rather than the
    // user's settings, so a cutscene neutralises the option without touching
    // what the player chose - it is still there when the cutscene ends.
    //
    // 100, NOT 0. Both end up writing the engine's own value, but they take
    // different routes and only one of them is safe:
    //   100 -> the scaling path. want == baseNear * 1.0, the write is guarded
    //          by `*nearF != want` so it is idempotent, and lastWroteNear is
    //          kept in sync - which is what lets the next frame tell OUR value
    //          apart from a fresh engine value and keep the baseline tracking
    //          correctly (including any cutscene-specific value the engine
    //          installs).
    //     0 -> the restore path at the bottom, which writes unconditionally
    //          every frame and never updates lastWroteNear. The baseline would
    //          then be re-derived from our own output every frame - a no-op
    //          numerically today, but exactly the poisoned-baseline failure
    //          mode documented below, and it depends on the engine refreshing
    //          the field, which is not guaranteed for a setting like this.
    //
    // Only percentages that are ACTUALLY ACTIVE are neutralised. Forcing 100
    // unconditionally would start writing these fields for players who have
    // the option switched off, which we otherwise never touch at all.
    LONG nearPct = g_shadowSplitNearPct;
    LONG farPct  = g_shadowSplitFarPct;
    if (g_cutsceneActive && (nearPct > 0 || farPct > 0)) {
        if (nearPct > 0) nearPct = 100;
        if (farPct  > 0) farPct  = 100;
        InterlockedIncrement(&g_cutsceneSuppressed);
    }
    if (g_mainModBase == 0) return;
    if (nearPct <= 0 && farPct <= 0) return;
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
        if (nearPct > 0 && baseNear > 0.0f) {
            float want = baseNear * (nearPct / 100.0f);
            if (*nearF != want) {
                *nearF = want;
                lastWroteNear = want;
                InterlockedIncrement(&g_splitSrcWrites);
            }
        }
        if (farPct > 0 && baseFarA > 0.0f) {
            // scale one factor only - the other is left alone so whatever
            // per-area meaning it carries is preserved
            float want = baseFarA * (farPct / 100.0f);
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
        if (nearPct <= 0) { *nearF = baseNear > 0.0f ? baseNear : n; }
        if (farPct  <= 0) { *farA  = baseFarA > 0.0f ? baseFarA : a; }
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

