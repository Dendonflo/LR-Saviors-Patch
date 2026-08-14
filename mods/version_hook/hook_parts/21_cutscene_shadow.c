// ---- Cutscene detection, for the shadow-distance auto-revert -------------
// PROBLEM (reported in game): the shadow-distance option (ShadowSplitNearPct,
// which scales the near cascade split - see ApplyCascadeSplitSource) breaks
// shadow rendering in some cutscenes. Cutscenes are authored against the
// engine's own split values, so scaling them moves shadows out from under
// hand-placed cameras. Cascades in this engine have been consistently
// fragile, so rather than fight it: detect cinema and stand down for its
// duration, then resume whatever the player set.
//
// WHERE THE SIGNAL COMES FROM
// white::cinema::CinemaController is a real, fully-implemented, boot-time
// subsystem - unlike the gutted debug menu, its vtable slots are distinct
// real functions. It is constructed by FUN_00910ac0 (called from the boot
// init FUN_00432200) and stored in the singleton DAT_050f5a90. It owns the
// CinemaContext list, the cut scheduler and the cutscene player, and it has a
// flags word at +0x1c (FUN_008f5470 reads bit 8 of it; FUN_005c3500 clears
// bit 6), which is the leading candidate for "playing".
//
// WHY THIS IS DATA-DRIVEN RATHER THAN HARDCODED
// Most cinema code reaches the controller through `this` pointers rather than
// the singleton, so which exact bit means "a cutscene is playing" is not
// settled from decompilation alone - and this project does not ship
// conclusions that have not been confirmed in game. So the offset and mask
// are ini keys, and ENABLE_CUTSCENE_DIAG logs the candidate fields on change
// so ONE run through a cutscene identifies the bit. Once known, it is an ini
// edit, not a rebuild.
//
// Until CutsceneFlagMask is set to something non-zero, detection reports
// "no cutscene" forever, so the feature is inert and cannot affect anyone.

#define CINEMA_CTRL_PTR_RVA (0x050f5a90 - 0x00400000)

// g_cutsceneRevert / g_cutsceneFlagOff / g_cutsceneFlagMask are defined in
// 03_render_state.c with the other cutscene state: the config table in
// 08_config_persist.c registers them as ini keys, and that part is included
// before this one, so their definitions have to precede it.

// Returns the CinemaController instance, or 0 if it does not exist yet
// (it is built during boot init, so this is null for the first seconds).
static DWORD CinemaController(void)
{
    DWORD p;
    if (!g_mainModBase) return 0;
    __try {
        p = *(DWORD *)(g_mainModBase + CINEMA_CTRL_PTR_RVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    // Sanity: a plausible heap pointer. A wild value must never be dereferenced.
    if (p < 0x10000 || (p & 3)) return 0;
    return p;
}

#if ENABLE_CUTSCENE_DIAG
// Fields sampled for the correlation run. Kept small and fixed so one log
// line stays readable, and capped so a long session cannot flood the log.
static const int g_cutDiagOffs[] = { 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x44, 0x88 };
#define CUT_DIAG_N     (sizeof(g_cutDiagOffs) / sizeof(g_cutDiagOffs[0]))
#define CUT_DIAG_MAX   400
static DWORD g_cutDiagLast[CUT_DIAG_N];
static LONG  g_cutDiagLines = 0;
static LONG  g_cutDiagInit = 0;

static void CutsceneDiagTick(DWORD ctrl)
{
    DWORD cur[CUT_DIAG_N];
    int i, changed = 0;
    if (g_cutDiagLines >= CUT_DIAG_MAX) return;
    __try {
        for (i = 0; i < (int)CUT_DIAG_N; i++) cur[i] = *(DWORD *)(ctrl + g_cutDiagOffs[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!g_cutDiagInit) {
        g_cutDiagInit = 1;
        for (i = 0; i < (int)CUT_DIAG_N; i++) g_cutDiagLast[i] = cur[i];
        LogLine("[cutdiag] baseline captured - watching CinemaController fields"
                " (+1c +20 +24 +28 +2c +30 +44 +88)");
        return;
    }
    for (i = 0; i < (int)CUT_DIAG_N; i++) if (cur[i] != g_cutDiagLast[i]) { changed = 1; break; }
    if (!changed) return;
    {
        char l[320];
        int n = sprintf(l, "[cutdiag] f=%ld", (long)g_frameSeq);
        for (i = 0; i < (int)CUT_DIAG_N; i++) {
            n += sprintf(l + n, " +%02x=%08lx%s", g_cutDiagOffs[i],
                         (unsigned long)cur[i], cur[i] != g_cutDiagLast[i] ? "*" : "");
            g_cutDiagLast[i] = cur[i];
        }
        LogLine(l);
        if (InterlockedIncrement(&g_cutDiagLines) == CUT_DIAG_MAX)
            LogLine("[cutdiag] line cap reached - no further [cutdiag] output this run");
    }
}
#endif  // ENABLE_CUTSCENE_DIAG

// Per-frame, main thread, called immediately before ApplyCascadeSplitSource
// so the split decision always uses this frame's cinema state - a 500ms
// monitor tick would leave half a second of wrong shadows at each boundary.
static void CutsceneDetectTick(void)
{
    DWORD ctrl = CinemaController();
    LONG want;
    if (!ctrl) {
        // No controller yet (or gone): definitively not in a cutscene. Clear
        // rather than leave stale, so a teardown can never strand the
        // override on and silently disable the option for the rest of the run.
        if (g_cutsceneActive) InterlockedExchange(&g_cutsceneActive, 0);
        return;
    }
#if ENABLE_CUTSCENE_DIAG
    CutsceneDiagTick(ctrl);
#endif
    if (!g_cutsceneRevert || g_cutsceneFlagMask == 0) {
        if (g_cutsceneActive) InterlockedExchange(&g_cutsceneActive, 0);
        return;
    }
    __try {
        DWORD v = *(DWORD *)(ctrl + (DWORD)g_cutsceneFlagOff);
        want = (v & (DWORD)g_cutsceneFlagMask) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (want != g_cutsceneActive) {
        InterlockedExchange(&g_cutsceneActive, want);
        InterlockedIncrement(&g_cutsceneEdges);
        {
            char l[160];
            sprintf(l, "[cutscene] %s (edge #%ld, split suppressed %ld frames so far)",
                    want ? "ENTER - shadow distance held at engine default"
                         : "EXIT - shadow distance restored to user setting",
                    g_cutsceneEdges, g_cutsceneSuppressed);
            LogLine(l);
        }
    }
}
