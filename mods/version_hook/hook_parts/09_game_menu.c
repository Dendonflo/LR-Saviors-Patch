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
// Texture Filtering. Both handlers write settings+0x38 and nothing else:
// Advanced = 8, Standard = 1. That is the game's ENTIRE anisotropy range,
// which is what motivated replacing the pair - see 08d_texfilter.c.
#define MENU_RVA_TEXFLT_ADV 0x006CAD50  // Graphics_TextureFiltering_Advanced
#define MENU_RVA_TEXFLT_STD 0x006CAD80  // Graphics_TextureFiltering_Standard

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

// Defined below with the deferred poll; declared here because MenuH_ResetAll
// needs the game window to own its confirmation dialog and sits earlier.
static HWND GameMenuFindWindow(void);

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
GAMEMENU_TOGGLE(MenuH_Status,      g_statusEnabled)
// Heap-compaction deferral. Lives in Optimization, which is only reachable by
// a user who set AdvancedMenu=1 by hand - at that point they have opted into
// experimenting, and the tuning knobs (CompactorBudgetUs, CompactorCooldown)
// are one file away anyway. Default OFF and unproven: see the long note at
// g_compactorDeferEnabled in 03_render_state.c.
GAMEMENU_TOGGLE(MenuH_Compactor,   g_compactorDeferEnabled)
#if ENABLE_AO_RECON
// AO tint probe: the bands make everything look weird by design, which makes
// on/off comparison the only readable way to judge them - so it needs a live
// toggle. Rides the normal toggle machinery; menu rebuild = instant effect.
GAMEMENU_TOGGLE(MenuH_AoTint,      g_aoTint)
// Momentary button (same shape as Mark Log): writes the untinted composite
// to ao_buffer.bmp for offline channel analysis.
static char __cdecl MenuH_AoDump(char apply)
{
    if (apply) InterlockedExchange(&g_aoDumpRequest, 1);
    return 0;
}
#if ENABLE_AO_SSAO
// Captures whatever the raw view is currently showing (needs Raw View on).
static char __cdecl MenuH_AoStageDump(char apply)
{
    if (apply) InterlockedExchange(&g_aoStageDumpRequest, 1);
    return 0;
}
#endif
#if ENABLE_AO_SSAO
// Graphics > Ambient Occlusion: Off / SSAO as a radio pair, the shape every
// other graphics group uses. "HBAO" joins as a third value when the
// estimator swap exists - g_aoEnable=2 is reserved for it.
GAMEMENU_VALUE(MenuH_AoOff,  g_aoEnable, 0)
GAMEMENU_VALUE(MenuH_AoSsao, g_aoEnable, 1)
GAMEMENU_VALUE(MenuH_AoHbao, g_aoEnable, 2)
// AO buffer resolution moved to a dropdown in the tuning window
// (10_overlay.c, AOTW_COMBO_ID): it interacts with the blur reach and the
// upsample, both of which are tuned there, so splitting it across two
// surfaces meant tuning with half the controls out of sight.
// AO Quality menu RETIRED 2026-08-16 - the estimator is locked to the High
// tier. AO Resolution stays as the performance lever.
GAMEMENU_TOGGLE(MenuH_AoDebug,  g_aoDebug)
// Bilateral blur A/B lever: default ON (it IS the noise cure), the toggle
// exists so raw-vs-blurred can be compared live while tuning.
GAMEMENU_TOGGLE(MenuH_AoBlur,   g_aoBlur)
// Live A/B for the engine's [0.5..1] shadow envelope. Inverted sense: the
// menu entry offers the RISKY option, so an unchecked box is the safe
// default, same shape as the other experiment toggles here.
static char __cdecl MenuH_AoFloor(char apply)
{
    if (apply) InterlockedExchange(&g_aoRespectFloor, g_aoRespectFloor ? 0 : 1);
    return (char)(g_aoRespectFloor ? 0 : 1);
}
// Black-model bisect levels (see g_aoBisect in 01). A radio group: walk
// down the levels in the equip menu and note the FIRST one where the
// newly-selected weapon stops rendering black.
GAMEMENU_VALUE(MenuH_AoBis0, g_aoBisect, 0)
GAMEMENU_VALUE(MenuH_AoBis1, g_aoBisect, 1)
GAMEMENU_VALUE(MenuH_AoBis2, g_aoBisect, 2)
GAMEMENU_VALUE(MenuH_AoBis3, g_aoBisect, 3)
GAMEMENU_VALUE(MenuH_AoBis4, g_aoBisect, 4)
GAMEMENU_VALUE(MenuH_AoBis5, g_aoBisect, 5)
GAMEMENU_VALUE(MenuH_AoBis6, g_aoBisect, 6)
// Flat-write test values (see g_aoFlatTest in 01).
GAMEMENU_VALUE(MenuH_AoFlatOff, g_aoFlatTest, 0)
GAMEMENU_VALUE(MenuH_AoFlat90,  g_aoFlatTest, 90)
GAMEMENU_VALUE(MenuH_AoFlat60,  g_aoFlatTest, 60)
// The live tuning window (10_overlay.c) opens from a leaf inside the
// Ambient Occlusion group itself. MOMENTARY, like Mark Log: it always
// reports itself unchecked, so picking it opens the window without
// disturbing which estimator the group has selected. Closing is done from
// the window's own close box, which writes the flag back.
static char __cdecl MenuH_AoPanel(char apply)
{
    if (apply) {
        InterlockedExchange(&g_aoTweakOpen, 1);
        // The click is the first link in the chain the in-game panel hangs
        // off; logging it separates "handler never fired" from every failure
        // further along (the trap the panel's first flight fell into).
        LogLine("[menu] AO tuning panel: open clicked");
    }
    return 0;
}
// The raw-AO view moved INTO that window as a checkbox (see AOTW_CHECK_ID
// in 10_overlay.c) - it is a tuning aid, and reaching it through the game
// menu meant leaving the sliders every time.
// Which stage the raw view paints. Normals is the one that matters right
// now: it is the estimator's only derived input and the last thing that
// can vary row by row without the depth buffer doing so.
GAMEMENU_VALUE(MenuH_AoStage0, g_aoDebugStage, 0)
GAMEMENU_VALUE(MenuH_AoStage1, g_aoDebugStage, 1)
GAMEMENU_VALUE(MenuH_AoStage2, g_aoDebugStage, 2)
GAMEMENU_VALUE(MenuH_AoStage3, g_aoDebugStage, 3)
#endif
#endif

// Log marker. A MOMENTARY ACTION, not a toggle: it writes a line and always
// reports itself unchecked, so the menu entry behaves like a button.
//
// This replaces marking runs by toggling shadow distance. That worked, but it
// changed a real graphics setting to do it - the marked run then had to be
// toggled back to the intended value, and a mistimed second toggle silently
// altered what was being measured. Marks matter more than they look: analysis
// MUST slice at the last mark, because whole-log stutter stats fold in boot,
// save loading and teleports, whose causes (class loader, DDS decode, driver
// shader compilation) do not occur in gameplay at all. Reading unsliced logs
// once produced a cause roster in which three loading-only families looked
// like major gameplay stutter sources. See tools/analyze_run.py.
//
// Advanced-only: it exists to make a diagnostic capture readable, and a
// player who never reads the log has no use for it.
static volatile LONG g_markCount = 0;

// Momentary. Confirmed, because it is destructive and not undoable - the ini
// is rewritten immediately - and because a mis-click here would silently undo
// a player's whole configuration.
static char __cdecl MenuH_ResetAll(char apply)
{
    if (apply) {
        // OWNED by the game window, and topmost. With a NULL owner (and
        // MB_TASKMODAL, which owns nothing by definition) this dialog opened
        // BEHIND the game in fullscreen - the same Z-order lesson the tuning
        // panel taught: the game's window is topmost, and an unowned window
        // has no claim above it. An owned dialog sits above its owner by
        // construction. MB_SETFOREGROUND makes sure it is also the thing
        // receiving the keystroke that answers it.
        HWND owner = g_gameHwnd ? g_gameHwnd : GameMenuFindWindow();
        if (MessageBoxW(owner, TR(S_RESET_ASK_ALL), MOD_NAME_W,
                        MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
            CfgResetDefaults(0);
    }
    return 0;
}

static char __cdecl MenuH_LogMark(char apply)
{
    if (apply) {
        SYSTEMTIME st;
        char l[160];
        LONG n = InterlockedIncrement(&g_markCount);
        GetLocalTime(&st);
        // Numbered so several marks in one session stay distinguishable, and
        // stamped the same way the frametime line is so the two line up.
        sprintf(l, "[mark] #%ld  (%02d:%02d:%02d  up=%.1fs  frame=%ld)",
                n, st.wHour, st.wMinute, st.wSecond,
                GetTickCount() / 1000.0, g_frameSeq);
        LogLine(l);
        LogFlushNow();
    }
    return 0;   // never checked - this is a button, not a state
}

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
// Retired with the "Overlay Position" submenu - the overlay is dragged now.
// Kept compiled (they cost nothing) so the corner behaviour is one menu line
// away if it is ever wanted back.
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
// ONE control, both splits. The far split tested well at 200% (2026-08-15),
// which also explains the old 300% glitching: that was the NEAR split pushed
// to 300 with far left at its stock distance, so the near cascade was being
// stretched to cover ground the technique could not hold on its own. Moving
// both together keeps the cascade proportioned.
//
// Pairs are near/far: 150/125, 200/150, 300/200. The label shows the near
// percentage only - two numbers in a menu entry would invite tuning the ratio,
// which is exactly what this collapses.
//
// The separate far entries (MenuH_Far*) and the FarPct ini key both still
// work for hand-tuning; only the menu group is gone.
static void GameMenuSetSplit(LONG nearPct, LONG farPct)
{
    InterlockedExchange(&g_shadowSplitNearPct, nearPct);
    InterlockedExchange(&g_shadowSplitFarPct, farPct);
    SaveConfig();
    // This used to also write the run marker, because it was the cheapest
    // one-button action reachable mid-run. Retired 2026-08-15 in favour of a
    // dedicated "Mark Log" entry (MenuH_LogMark): marking a run should not
    // change a graphics setting, and having to toggle back afterwards was a
    // standing chance of measuring the wrong configuration.
}
static char __cdecl MenuH_DistStd(char apply)
{
    if (apply) GameMenuSetSplit(0, 0);
    return (char)(g_shadowSplitNearPct == 0);
}
static char __cdecl MenuH_Dist150(char apply)
{
    if (apply) GameMenuSetSplit(150, 125);
    return (char)(g_shadowSplitNearPct == 150);
}
static char __cdecl MenuH_Dist200(char apply)
{
    if (apply) GameMenuSetSplit(200, 150);
    return (char)(g_shadowSplitNearPct == 200);
}
static char __cdecl MenuH_Dist300(char apply)
{
    if (apply) GameMenuSetSplit(300, 200);
    return (char)(g_shadowSplitNearPct == 300);
}

// FAR cascade split. Scales the first of the two fields whose PRODUCT is the
// far distance (scene+0x430, with +0x438 left alone so whatever per-area
// meaning it carries survives) - see ApplyCascadeSplitSource.
//
// Exposed 2026-08-15 at the user's request, for testing. It was deliberately
// menu-less before: the far split moved things around in ways that were hard
// to predict, so only the near split shipped. Kept to modest steps for the
// same reason - if it turns out to behave, the range can grow.
//
// Also the leading candidate for whatever draws the OTHER, longer-range
// high-quality shadows the user has observed: those ignore the near split
// entirely but do respond to shadow map resolution, so they are a separate
// projection reading a separate distance - and far is the distance we know
// about that is not near.
static void GameMenuSetFarSplit(LONG pct)
{
    InterlockedExchange(&g_shadowSplitFarPct, pct);
    SaveConfig();
}
static char __cdecl MenuH_FarStd(char apply)
{
    if (apply) GameMenuSetFarSplit(0);
    return (char)(g_shadowSplitFarPct == 0);
}
static char __cdecl MenuH_Far125(char apply)
{
    if (apply) GameMenuSetFarSplit(125);
    return (char)(g_shadowSplitFarPct == 125);
}
static char __cdecl MenuH_Far150(char apply)
{
    if (apply) GameMenuSetFarSplit(150);
    return (char)(g_shadowSplitFarPct == 150);
}
static char __cdecl MenuH_Far200(char apply)
{
    if (apply) GameMenuSetFarSplit(200);
    return (char)(g_shadowSplitFarPct == 200);
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

// Texture Filtering popup REPLACEMENT. Off / 2x / 4x / 8x / 16x, all on the
// mod's own enforcement (08d_texfilter.c) rather than on the engine field,
// for the reasons in that file's header. GAMEMENU_VALUE is not reused here
// because each of these has to mark the sampler shadows dirty as well: a
// stage the engine configures once at load would otherwise keep the old
// level until something re-set it, which reads as "the option does nothing".
static void GameMenuSetAniso(LONG level)
{
    InterlockedExchange(&g_anisoLevel, level);
    TexFilterMarkDirty();
    SaveConfig();
}
static char __cdecl MenuH_Aniso1(char apply)
{ if (apply) GameMenuSetAniso(1);  return (char)(g_anisoLevel == 1); }
static char __cdecl MenuH_Aniso2(char apply)
{ if (apply) GameMenuSetAniso(2);  return (char)(g_anisoLevel == 2); }
static char __cdecl MenuH_Aniso4(char apply)
{ if (apply) GameMenuSetAniso(4);  return (char)(g_anisoLevel == 4); }
static char __cdecl MenuH_Aniso8(char apply)
{ if (apply) GameMenuSetAniso(8);  return (char)(g_anisoLevel == 8); }
static char __cdecl MenuH_Aniso16(char apply)
{ if (apply) GameMenuSetAniso(16); return (char)(g_anisoLevel == 16); }
// RETIRED from the menu 2026-08-19 (see the insert site). Kept compiled: it
// costs nothing, and it is the only code path that names the "leave the
// engine alone" state, which the ini can still select.
static char __cdecl MenuH_AnisoEngine(char apply)
{ if (apply) GameMenuSetAniso(0);  return (char)(g_anisoLevel == 0); }

// Mip LOD bias floor. Its own group rather than more entries in the Texture
// Filtering popup: that popup is a single radio list of anisotropy levels,
// and a second, unrelated radio list sharing it would read as one setting.
static void GameMenuSetMipBias(LONG mode)
{
    InterlockedExchange(&g_mipBiasMode, mode);
    SaveConfig();
    // No TexFilterMarkDirty: unlike the anisotropy level, this state is
    // rewritten by the engine constantly, so both directions are live within
    // a frame on their own.
}
static char __cdecl MenuH_Bias0(char apply)
{ if (apply) GameMenuSetMipBias(0); return (char)(g_mipBiasMode == 0); }
static char __cdecl MenuH_BiasNeutral(char apply)
{ if (apply) GameMenuSetMipBias(1); return (char)(g_mipBiasMode == 1); }
static char __cdecl MenuH_BiasHalf(char apply)
{ if (apply) GameMenuSetMipBias(2); return (char)(g_mipBiasMode == 2); }
static char __cdecl MenuH_BiasOne(char apply)
{ if (apply) GameMenuSetMipBias(3); return (char)(g_mipBiasMode == 3); }

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

static volatile LONG g_fdfForces = 0, g_fdfLogged = 0;

// ---- engine framerate mode: force Dynamic -----------------------------------
// The vanilla FrameRate popup is replaced wholesale below, so "Variable" and
// "Stability" are no longer clickable - but the ENGINE's own setting lives in
// the game's own storage and survives that. Sitting on Stability halves the
// mod limiter's effective target, and with no menu entry left to show or undo
// it, the symptom reads as "the mod stopped working". The usual way in is the
// game resetting its settings to low after an unclean exit.
//
// Same convention as every vanilla handler here: handler(0) queries, and
// handler(1) applies AND persists through the game's own path - so this is the
// engine changing its own setting, not us writing a field behind its back.
//
// Deliberately conditional: a session already on Variable performs no write at
// all. The result is checked by re-querying, but note what that check is worth
// - see the Nova finding below.
//
// NOT one-shot, and that is the 2026-08-18 correction. The first version fired
// once at the first menu build, logged "forced to Dynamic" on a verified
// re-query, and the game still wrote `Graphics_FrameRate = Stability` to
// Configuration.ini on exit. The verification was not wrong, it was EARLY:
// the user launches through Nova Launcher, which applies its own settings
// after the game is up, so our force landed first and was overwritten
// afterwards. A single shot at boot cannot win a race it does not know it is
// in - hence the deferred poll below, which simply keeps checking.
static int GameMenuForceDynamicNow(const char *when)
{
    unsigned char *base = (unsigned char *)g_mainModBase;
    GameMenuHandler var, stab;
    if (!g_forceDynamicFps || !base) return 0;
    var  = (GameMenuHandler)(base + MENU_RVA_FRATE_VAR);
    stab = (GameMenuHandler)(base + MENU_RVA_FRATE_STAB);
    if (!stab(0)) return 0;                  // already Dynamic - nothing to do
    var(1);
    InterlockedIncrement(&g_fdfForces);
    if (InterlockedIncrement(&g_fdfLogged) <= 4) {
        char l[224];
        sprintf(l, "[menu] engine framerate mode was Fixed - forced to Dynamic (%s)%s",
                when, stab(0) ? "  *** STILL READS FIXED ***" : "");
        LogLine(l);
    }
    return 1;
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
    // Resolve the menu language BEFORE inserting anything: the vanilla tree
    // has just been built, so its own localised popup labels are sitting on
    // the bar waiting to be read, and every label we add below goes through
    // TR() using the result. This is the whole detection mechanism - see
    // 08c_lang_detect.c for why the game's own menu is the only trustworthy
    // source.
    LangDetectFromMenu(root);
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
        mOpen(mgr, NULL, "Mod_Optimization"); GameMenuFixLabel(mgr, L"Dev Tools");   // renamed from "Optimization"
        // 2026-08-16: it holds diagnostics and experiments, not user
        // options, and the old name suggested settings worth touching.
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
        mAdd(mgr, NULL, "Mod_Compactor", (void *)MenuH_Compactor);
        GameMenuFixLabel(mgr, L"Defer Heap Compaction (unproven)");
#if ENABLE_AO_RECON
        mAdd(mgr, NULL, "Mod_AoTint", (void *)MenuH_AoTint);
        GameMenuFixLabel(mgr, L"AO Tint Probe (bands)");
        mAdd(mgr, NULL, "Mod_AoDump", (void *)MenuH_AoDump);
        GameMenuFixLabel(mgr, L"AO Dump Buffer (ao_buffer.bmp)");
#if ENABLE_AO_SSAO
        mAdd(mgr, NULL, "Mod_AoBlur", (void *)MenuH_AoBlur);
        GameMenuFixLabel(mgr, L"AO Blur (bilateral)");
        mAdd(mgr, NULL, "Mod_AoFloor", (void *)MenuH_AoFloor);
        GameMenuFixLabel(mgr, L"AO Past Engine Floor");
        mOpen(mgr, NULL, "Mod_AoBisect"); GameMenuFixLabel(mgr, L"AO Bisect (black-model diag)");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_AoBis0", (void *)MenuH_AoBis0); GameMenuFixLabel(mgr, L"0 Full Pipeline");
        mAdd(mgr, NULL, "Mod_AoBis1", (void *)MenuH_AoBis1); GameMenuFixLabel(mgr, L"1 No Composite Write");
        mAdd(mgr, NULL, "Mod_AoBis2", (void *)MenuH_AoBis2); GameMenuFixLabel(mgr, L"2 + No Snapshot Copy");
        mAdd(mgr, NULL, "Mod_AoBis3", (void *)MenuH_AoBis3); GameMenuFixLabel(mgr, L"3 + No Blur Draws");
        mAdd(mgr, NULL, "Mod_AoBis4", (void *)MenuH_AoBis4); GameMenuFixLabel(mgr, L"4 Setup Only");
        mAdd(mgr, NULL, "Mod_AoBis5", (void *)MenuH_AoBis5); GameMenuFixLabel(mgr, L"5 State Block Only");
        mAdd(mgr, NULL, "Mod_AoBis6", (void *)MenuH_AoBis6); GameMenuFixLabel(mgr, L"6 Nothing (control)");
        mClose(mgr, NULL);
        mOpen(mgr, NULL, "Mod_AoFlat"); GameMenuFixLabel(mgr, L"AO Flat Write Test");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_AoFlatOff", (void *)MenuH_AoFlatOff); GameMenuFixLabel(mgr, L"Off (normal AO)");
        mAdd(mgr, NULL, "Mod_AoFlat90",  (void *)MenuH_AoFlat90);  GameMenuFixLabel(mgr, L"Flat x0.90");
        mAdd(mgr, NULL, "Mod_AoFlat60",  (void *)MenuH_AoFlat60);  GameMenuFixLabel(mgr, L"Flat x0.60");
        mClose(mgr, NULL);
        // The raw-view TOGGLE moved into the tuning window as a checkbox.
        // The STAGE selector stays here: normals/depth/occlusion are
        // pipeline diagnostics, not tuning aids, and they only mean
        // anything while the raw view is on - which is now one click away
        // in that window.
        mOpen(mgr, NULL, "Mod_AoStage"); GameMenuFixLabel(mgr, L"Raw View Stage");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_AoStage0", (void *)MenuH_AoStage0); GameMenuFixLabel(mgr, L"AO Term");
        mAdd(mgr, NULL, "Mod_AoStage1", (void *)MenuH_AoStage1); GameMenuFixLabel(mgr, L"Depth");
        mAdd(mgr, NULL, "Mod_AoStage2", (void *)MenuH_AoStage2); GameMenuFixLabel(mgr, L"Normals");
        mAdd(mgr, NULL, "Mod_AoStage3", (void *)MenuH_AoStage3); GameMenuFixLabel(mgr, L"Occlusion");
        mClose(mgr, NULL);
        mAdd(mgr, NULL, "Mod_AoStageDump", (void *)MenuH_AoStageDump);
        GameMenuFixLabel(mgr, L"Dump Raw View Stage");
        mAdd(mgr, NULL, "Mod_AoDebug", (void *)MenuH_AoDebug);
        GameMenuFixLabel(mgr, L"SSAO Pipeline Bands (diagnostic)");
#endif
#endif
        mAdd(mgr, NULL, "Mod_SimDelta",  (void *)MenuH_SimDelta);  GameMenuFixLabel(mgr, L"Sim Delta Fix");
        mAdd(mgr, NULL, "Mod_StdD3D9",   (void *)MenuH_StdD3D9);   GameMenuFixLabel(mgr, L"Force Plain D3D9 (restart)");
        mOpen(mgr, NULL, "Mod_Watchdog"); GameMenuFixLabel(mgr, L"Stutter Watchdog");
        mBegin(mgr, NULL);
        mAdd(mgr, NULL, "Mod_WdOff", (void *)MenuH_WdOff); GameMenuFixLabel(mgr, TR(S_OFF));
        mAdd(mgr, NULL, "Mod_Wd4",   (void *)MenuH_Wd4);   GameMenuFixLabel(mgr, L"4 ms");
        mAdd(mgr, NULL, "Mod_Wd8",   (void *)MenuH_Wd8);   GameMenuFixLabel(mgr, L"8 ms");
        mAdd(mgr, NULL, "Mod_Wd16",  (void *)MenuH_Wd16);  GameMenuFixLabel(mgr, L"16 ms");
        mAdd(mgr, NULL, "Mod_Wd33",  (void *)MenuH_Wd33);  GameMenuFixLabel(mgr, L"33 ms");
        mClose(mgr, NULL);
        // Sits directly under the watchdog: the two are used together, since
        // a mark is only useful when something is being captured.
        mAdd(mgr, NULL, "Mod_LogMark", (void *)MenuH_LogMark);
        GameMenuFixLabel(mgr, L"Mark Log (run start)");
        mClose(mgr, NULL);
    }

    // -- 1b. top-level "Other" popup - always visible. Home of the frametime
    //        overlay, which ships as a user feature (the only honest view of
    //        the engine tick), separate from the advanced-only fix toggles.
    mOpen(mgr, NULL, "Mod_Other"); GameMenuFixLabel(mgr, TR(S_OTHER));
    mBegin(mgr, NULL);
    mAdd(mgr, NULL, "Mod_Overlay", (void *)MenuH_Overlay); GameMenuFixLabel(mgr, TR(S_FRAMETIME));
    // Every setting beside what is actually in force, including the live
    // cutscene/gameplay state. Sits next to the graph deliberately: same
    // "show me what is happening" family, different question.
    mAdd(mgr, NULL, "Mod_Status", (void *)MenuH_Status); GameMenuFixLabel(mgr, TR(S_STATUS_PANEL));
    // The way back from a bad configuration for players who have been told not
    // to touch the ini. Restores the compile-time defaults, AO included.
    mAdd(mgr, NULL, "Mod_ResetAll", (void *)MenuH_ResetAll); GameMenuFixLabel(mgr, TR(S_RESET_ALL));
    // "Overlay Position" (four corners) retired: both windows are dragged
    // directly now, which places them exactly rather than approximately.
    // The MenuH_Ovl* handlers are kept above but no longer reachable.
    mClose(mgr, NULL);

    // -- 2. locate the vanilla Graphics anchors BEFORE any surgery (the
    //       replacements below delete the very items the needles point at).
    {
        HMENU mPresPop = NULL, mScalePop = NULL, mShadPop = NULL, mFratePop = NULL;
        HMENU mTexPop = NULL;
        HMENU mTmp = NULL;
        int pTmp = -1, pStd = -1, pAdv = -1, pVar = -1, pStab = -1;
        int pTStd = -1, pTAdv = -1;

        GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_PRES_FS), &mPresPop, &pTmp);
        GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_SCALE_ADV), &mScalePop, &pTmp);
        if (!GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_SHADOW_ADV), &mShadPop, &pAdv) ||
            !GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_SHADOW_STD), &mTmp, &pStd) ||
            mTmp != mShadPop) { mShadPop = NULL; }
        if (!GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_FRATE_VAR), &mFratePop, &pVar) ||
            !GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_FRATE_STAB), &mTmp, &pStab) ||
            mTmp != mFratePop) { mFratePop = NULL; }
        if (!GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_TEXFLT_ADV), &mTexPop, &pTAdv) ||
            !GameMenuFindByData(root, (ULONG_PTR)(base + MENU_RVA_TEXFLT_STD), &mTmp, &pTStd) ||
            mTmp != mTexPop) { mTexPop = NULL; }

        // -- 3. Shadowing popup: Standard/Advanced replaced by four
        //       resolutions on the same engine field.
        if (mShadPop) {
            int lo = pStd < pAdv ? pStd : pAdv;
            DeleteMenu(mShadPop, (UINT)(pStd > pAdv ? pStd : pAdv), MF_BYPOSITION);
            DeleteMenu(mShadPop, (UINT)lo, MF_BYPOSITION);
            GameMenuInsertLeaf(mShadPop, lo + 0, id++, TrSuffix(S_STANDARD, L"(1024)"), MenuH_Shadow1024);
            GameMenuInsertLeaf(mShadPop, lo + 1, id++, TrSuffix(S_ADVANCED, L"(2048)"), MenuH_Shadow2048);
            GameMenuInsertLeaf(mShadPop, lo + 2, id++, TrSuffix(S_HIGH,     L"(4096)"),     MenuH_Shadow4096);
            // The oscillating stutter once blamed on 8192 was the per-frame
            // GPU fence serialising CPU and GPU (user, 2026-08-11) - fixed
            // by GpuSyncSkip, so 8192 carries no warning label.
            GameMenuInsertLeaf(mShadPop, lo + 3, id++, TrSuffix(S_ULTRA,    L"(8192)"), MenuH_Shadow8192);
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
                               TR(S_UNLIMITED), MenuH_FrUnlimited);
            repFrate = 1;
        }

        // -- 4b. Texture Filtering popup: Standard/Advanced (1x and 8x, and
        //        nothing else exists) replaced by the full range. Same shape
        //        as the Shadowing replacement above - delete the higher
        //        position first so the lower one stays valid.
        if (mTexPop) {
            int lo = pTStd < pTAdv ? pTStd : pTAdv;
            int n = 0;
            DeleteMenu(mTexPop, (UINT)(pTStd > pTAdv ? pTStd : pTAdv), MF_BYPOSITION);
            DeleteMenu(mTexPop, (UINT)lo, MF_BYPOSITION);
            GameMenuInsertLeaf(mTexPop, lo + n++, id++, TR(S_OFF), MenuH_Aniso1);
            GameMenuInsertLeaf(mTexPop, lo + n++, id++, L"2x",  MenuH_Aniso2);
            GameMenuInsertLeaf(mTexPop, lo + n++, id++, L"4x",  MenuH_Aniso4);
            GameMenuInsertLeaf(mTexPop, lo + n++, id++, L"8x",  MenuH_Aniso8);
            GameMenuInsertLeaf(mTexPop, lo + n++, id++, L"16x", MenuH_Aniso16);
            // The "Game default (census baseline)" entry that sat here is
            // RETIRED (2026-08-19): it existed to give the census build a way
            // back to unmodified filtering for the comparison run, that run is
            // done, and with 16x as the default an entry meaning "defer to a
            // menu whose entries we just deleted" is a trap. MenuH_AnisoEngine
            // is kept compiled - AnisoLevel=0 still works from the ini.
        }

        // -- 5. new groups inside the Graphics popup, slotted next to their
        //       vanilla relatives: VSync after Presentation, MSAA+FXAA after
        //       Scaling, the two shadow extras after Shadowing. Inserting at
        //       the HIGHEST anchor first keeps the earlier positions valid.
        {
            HMENU gfx = NULL, gfxCheck = NULL, sub;
            int presPos = -1, scalePos = -1, shadPos = -1, texPos = -1;

            if (mPresPop) GameMenuFindPopupItem(root, mPresPop, &gfx, &presPos);
            if (mScalePop && GameMenuFindPopupItem(root, mScalePop, &gfxCheck, &scalePos) &&
                gfx && gfxCheck != gfx) scalePos = -1;   // different parent: skip anchor
            if (!gfx) gfx = gfxCheck;
            gfxCheck = NULL;
            if (mShadPop && GameMenuFindPopupItem(root, mShadPop, &gfxCheck, &shadPos) &&
                gfx && gfxCheck != gfx) shadPos = -1;
            if (!gfx) gfx = gfxCheck;
            gfxCheck = NULL;
            if (mTexPop && GameMenuFindPopupItem(root, mTexPop, &gfxCheck, &texPos) &&
                gfx && gfxCheck != gfx) texPos = -1;
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
                mii.dwTypeData = (LPWSTR)TR(S_SHADOWS);
                SetMenuItemInfoW(gfx, (UINT)shadPos, TRUE, &mii);
            }

            if (gfx) {
                int endPos = GetMenuItemCount(gfx);
                int at;

                at = (shadPos >= 0) ? shadPos + 1 : endPos;
                sub = GameMenuInsertGroup(gfx, at, TR(S_SHADOW_DIST));
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, TR(S_STANDARD), MenuH_DistStd);
                    GameMenuInsertLeaf(sub, 1, id++, L"150%",     MenuH_Dist150);
                    GameMenuInsertLeaf(sub, 2, id++, L"200%",     MenuH_Dist200);
                    GameMenuInsertLeaf(sub, 3, id++, L"300%",     MenuH_Dist300);
                    groups++;
                }

                // The separate "Shadow Distance (Far)" group existed for one
                // afternoon (2026-08-15). Far tested well at 200%, so the two
                // splits are now driven together from the group above and the
                // standalone far group is retired - the MenuH_Far* handlers
                // are kept, and the FarPct ini key still works, for anyone
                // wanting to tune the ratio by hand.
                // Screen-Space Shadows REMOVED from the menu 2026-08-12: the
                // effect is too subtle to perceive (verified during the
                // ShadowBufPct work - 200% was indistinguishable from stock
                // because the content is band-limited by PCF plus the
                // MULTI_SAMPLE interleave), and it only applies on area change
                // or restart. A setting that does nothing visible and needs a
                // reload is a trap in a user-facing menu. ScreenShadowResPct
                // survives as an ini key for anyone who wants it.

                // Mip LOD bias, directly under Texture Filtering: the two are
                // the same subject, and during A/B testing they get toggled
                // against each other.
                at = (texPos >= 0) ? texPos + 1 : endPos;
                sub = GameMenuInsertGroup(gfx, at, L"Mip LOD Bias");
                if (sub) {
                    // NOT localised yet, deliberately - this ships as an A/B
                    // instrument and may not survive testing. If it becomes a
                    // default, these four need entries in gen_i18n.py first.
                    GameMenuInsertLeaf(sub, 0, id++, L"Engine (vanilla)",  MenuH_Bias0);
                    GameMenuInsertLeaf(sub, 1, id++, L"Neutral (0.0)",     MenuH_BiasNeutral);
                    GameMenuInsertLeaf(sub, 2, id++, L"Allow -0.5",        MenuH_BiasHalf);
                    GameMenuInsertLeaf(sub, 3, id++, L"Allow -1.0",        MenuH_BiasOne);
                    groups++;
                }

                at = (scalePos >= 0) ? scalePos + 1 : endPos;
                // Named "SSAA" rather than "Supersampling": the user maintains
                // a guide explaining each option, so the menu carries the term
                // people will search for rather than a description.
                sub = GameMenuInsertGroup(gfx, at, L"SSAA");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, TR(S_OFF),   MenuH_Ssaa100);
                    GameMenuInsertLeaf(sub, 1, id++, L"1.25x", MenuH_Ssaa125);
                    GameMenuInsertLeaf(sub, 2, id++, L"1.5x",  MenuH_Ssaa150);
                    GameMenuInsertLeaf(sub, 3, id++, L"2x",    MenuH_Ssaa200);
                    groups++;
                }
                sub = GameMenuInsertGroup(gfx, at + 1, L"MSAA");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, TR(S_OFF), MenuH_Msaa0);
                    GameMenuInsertLeaf(sub, 1, id++, L"2x",  MenuH_Msaa2);
                    GameMenuInsertLeaf(sub, 2, id++, L"4x",  MenuH_Msaa4);
                    GameMenuInsertLeaf(sub, 3, id++, L"8x",  MenuH_Msaa8);
                    groups++;
                }
                sub = GameMenuInsertGroup(gfx, at + 2, L"FXAA");
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, TR(S_ON),  MenuH_FxaaOn);
                    GameMenuInsertLeaf(sub, 1, id++, TR(S_OFF), MenuH_FxaaOff);
                    groups++;
                }
#if ENABLE_AO_SSAO
                sub = GameMenuInsertGroup(gfx, at + 3, TR(S_AO));
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, TR(S_OFF),  MenuH_AoOff);
                    GameMenuInsertLeaf(sub, 1, id++, L"SSAO", MenuH_AoSsao);
                    GameMenuInsertLeaf(sub, 2, id++, L"HBAO+", MenuH_AoHbao);
                    GameMenuInsertLeaf(sub, 3, id++, TR(S_TUNING_PANEL), MenuH_AoPanel);
                    groups++;
                }
#endif

                at = (presPos >= 0) ? presPos + 1 : endPos;
                sub = GameMenuInsertGroup(gfx, at, TR(S_VSYNC));
                if (sub) {
                    GameMenuInsertLeaf(sub, 0, id++, TR(S_ON),  MenuH_VsyncOn);
                    GameMenuInsertLeaf(sub, 1, id++, TR(S_OFF), MenuH_VsyncOff);
                    groups++;
                }
            }
        }
    }

    // Function level, not inside the anchor-surgery block above: the force is
    // about the ENGINE's setting and must still happen on a build where the
    // vanilla popups were not found (a stale RVA would skip the surgery but
    // says nothing about the handlers themselves). The deferred poll repeats
    // this - see its comment for why once at boot is not enough.
    GameMenuForceDynamicNow("menu build");

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

// ---- deferred poll: things that are not settled at menu-build time ---------
// Both of the problems this exists for have the SAME shape: the mod acts at
// the first menu build, and the state it acts on is not final yet.
//
//   framerate - Nova Launcher applies its own settings after the game is up,
//               so a boot-time force is silently reverted (confirmed: the log
//               said "forced to Dynamic" and the game still wrote Stability to
//               Configuration.ini on exit).
//   language  - every top-level menu label reads "*" at build time (the
//               builder's own lookup fallback) while the window plainly shows
//               localised names later, so detection falls through to the OS
//               language every session. TWO candidate explanations, and this
//               probe separates them rather than picking one: either the
//               labels are filled in after the build, or mgr[+0x08] is not the
//               menu the window actually shows. Both menus are read, both are
//               logged with a timestamp in frames, and the answer decides the
//               fix.
//
// Main thread only - called from the frame tick, which is where g_mainThreadId
// is established. Once per second, for the first minute; the framerate check
// is two calls into the game's own handler and the language probe stops
// entirely once a real label has matched.
#define GAMEMENU_POLL_FRAMES 3600
#define GAMEMENU_POLL_EVERY  60

// g_gameHwnd came back NULL in the probe's first flight - the game passes no
// hDeviceWindow in its presentation parameters, so D3D uses the focus window
// and our only recorded handle is never set. Find the real one instead: the
// process's own visible top-level window, preferring one that HAS a menu bar,
// which is exactly the window this probe is about.
typedef struct { DWORD pid; HWND withMenu; HWND anyVisible; } GameMenuWndSearch;

static BOOL CALLBACK GameMenuEnumWndProc(HWND h, LPARAM lp)
{
    GameMenuWndSearch *s = (GameMenuWndSearch *)lp;
    DWORD pid = 0;
    char cls[64];
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid || !IsWindowVisible(h)) return TRUE;
    // Skip the mod's OWN windows. They live in this process and are visible,
    // so without this the "first visible window" fallback can return the
    // frametime overlay or the tuning panel itself - and that fallback is
    // exactly what runs in fullscreen, where the game's menu bar is detached
    // with SetMenu(NULL) and the preferred has-a-menu test cannot match.
    // Making a window its own owner, or the owner of its own owner, is not a
    // mistake the window manager forgives.
    if (GetClassNameA(h, cls, sizeof(cls)) > 0 &&
        (strncmp(cls, "LRSavior", 8) == 0 || strncmp(cls, "LRStutter", 9) == 0))
        return TRUE;
    if (GetMenu(h)) { s->withMenu = h; return FALSE; }
    if (!s->anyVisible) s->anyVisible = h;
    return TRUE;
}

static HWND GameMenuFindWindow(void)
{
    GameMenuWndSearch s;
    s.pid = GetCurrentProcessId();
    s.withMenu = NULL;
    s.anyVisible = NULL;
    EnumWindows(GameMenuEnumWndProc, (LPARAM)&s);
    return s.withMenu ? s.withMenu : s.anyVisible;
}

// Watchdog deadline in monitor ticks (2 per second).
#define LANG_WATCHDOG_TICKS 20

// WATCHDOG, not a detector - detection itself works. It runs from the MONITOR
// thread (11_monitor.c) twice a second and stays silent unless the labels
// never resolve at all.
//
// The investigation that produced this is worth recording, because every step
// of it was a misread of the log rather than a bug in the code:
//
//   "the language detection is failing" - it was not. The vanilla labels are
//   the builder's "*" fallback for the first menu builds and become real a
//   build or two later, at which point detection matches them. The success was
//   INVISIBLE because the announcement only fires when the language CHANGES,
//   and here the OS fallback had already guessed the same answer. Three
//   miss lines and no success line read as a permanent failure. Both sides of
//   that are now logged explicitly.
//
//   "the frame tick fires once and stops" - it does not. It runs at a clean
//   60fps (measured: ~30 engine frames per 500ms monitor tick). The probe
//   logged at frame 1 and never again because the match landed at the third
//   menu build, a moment later, and closed the gate above - the later samples
//   were correctly skipped, not lost.
//
//   "the log is being truncated" - it is not. That was the next wrong theory,
//   from four sessions ending at a similar size; the ending is just where the
//   deduped first-occurrence lines run out. Measured with LogFlush=1 against
//   LogFlush=0: 22 non-heartbeat lines either way, byte for byte the same
//   content.
//
// The lesson for anything added here later: three separate "it never ran"
// conclusions were all absence-of-evidence about code that ran exactly as
// written. Before believing that one, make the code state its own case - an
// entry line ahead of every early return is what finally settled it, in one
// run, after several spent guessing.
//
// Reading menu labels is pure Win32 and safe from any thread; the framerate
// handlers are not, and stay on the main thread.
static void GameMenuLangProbe(void)
{
    static volatile LONG attempts = 0;
    unsigned char *base = (unsigned char *)g_mainModBase;
    char fromMgr[192], fromWnd[192];
    HMENU mMgr = NULL, mWnd = NULL;
    HWND wnd;
    int lMgr, lWnd, lang;
    LONG n;
    void *mgr;

    if (g_langCfg > 0) return;              // forced by ini - nothing to detect
    if (g_langFromLabel) return;            // resolved - the normal outcome
    if (!base) return;

    // Only two moments are worth a line: the deadline passing with no match,
    // and a match arriving after it. Everything before that is the labels
    // simply not being populated yet, which is normal and was already logged
    // twice by the detection path.
    n = InterlockedIncrement(&attempts);
    if (n < LANG_WATCHDOG_TICKS) return;
    mgr = *(void **)(base + MENU_RVA_MGR_PTR);
    if (mgr) mMgr = *(HMENU *)((char *)mgr + MENUMGR_ROOT_HMENU);
    wnd = g_gameHwnd ? g_gameHwnd : GameMenuFindWindow();
    if (wnd) mWnd = GetMenu(wnd);

    lMgr = LangProbeMenuBar(mMgr, fromMgr, sizeof(fromMgr));
    lWnd = LangProbeMenuBar(mWnd, fromWnd, sizeof(fromWnd));
    lang = (lMgr >= 0) ? lMgr : lWnd;

    // One line at the deadline, naming what both candidate menus actually
    // contain. This is the case that matters to a player: the mod's labels are
    // stuck on the OS-language guess, which is wrong for anyone playing in a
    // language other than their desktop.
    if (n == LANG_WATCHDOG_TICKS) {
        char l[576];
        sprintf(l, "[i18n] labels still unmatched after %d seconds - using the OS"
                   " language as a guess. mgr[%p]: %s | hwnd %p menu[%p]: %s",
                LANG_WATCHDOG_TICKS / 2, (void *)mMgr, fromMgr[0] ? fromMgr : "(empty)",
                (void *)wnd, (void *)mWnd, fromWnd[0] ? fromWnd : "(empty)");
        LogLine(l);
    }
    if (lang < 0) return;

    InterlockedExchange(&g_langFromLabel, 1);
    if (lang != g_langIdx) {
        char l[256];
        InterlockedExchange(&g_langIdx, lang);
        sprintf(l, "[i18n] language corrected to index %d from the %s menu on probe #%ld"
                   " - labels already inserted keep the old language until the"
                   " next menu rebuild", lang, (lMgr >= 0) ? "manager" : "window", n);
        LogLine(l);
    } else {
        char l[192];
        sprintf(l, "[i18n] label match on probe #%ld confirms the current language"
                   " (the OS fallback had guessed right)", n);
        LogLine(l);
    }
}

static void GameMenuDeferredPoll(LONG frame)
{
    if (frame <= 0 || frame > GAMEMENU_POLL_FRAMES) return;
    // Frame 1 and 30 before the once-a-second cadence: the first flight logged
    // nothing at all, and a probe whose earliest possible evidence is a whole
    // second in cannot distinguish "never ran" from "ran and found nothing".
    // The first sample now lands while boot-flushing is still on, so it
    // reaches disk even if the session ends badly.
    if (!(frame == 1 || frame == 30 || (frame % GAMEMENU_POLL_EVERY) == 0)) return;
    // Framerate only. The language probe moved to the monitor thread - see
    // GameMenuLangProbe for why this clock turned out not to be dependable.
    // The force stays here (and on the menu build) because it calls the game's
    // own handlers and must not leave the main thread.
    GameMenuForceDynamicNow("runtime poll");
}
#else
static void GameMenuDeferredPoll(LONG frame) { (void)frame; }
static void GameMenuLangProbe(void) { }
static HWND GameMenuFindWindow(void) { return NULL; }
#endif // ENABLE_GAME_MENU

