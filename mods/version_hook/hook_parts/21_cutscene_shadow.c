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

// ---- cut-slot layout, read off the pass-2 capture -------------------------
// The player object at CinemaController+0x28 holds an array of slots. Two
// were live simultaneously during the captured cutscene, and their fields
// line up on a clean 0x40 stride:
//        slot 0        slot 1
//   id   +0x18         +0x58      (0xffffffff when free)
//   act  +0x20         +0x60
//   name +0x38         +0x78      (NUL-terminated ASCII)
// which gives base 0x18, stride 0x40, and within a slot: active +0x08,
// name +0x20. Eight slots are scanned; the array is certainly not smaller
// (the object is 0x2030 bytes) and a free slot reads as inactive anyway.
#define CUT_SLOT_BASE    0x18
#define CUT_SLOT_STRIDE  0x40
#define CUT_SLOT_ACTIVE  0x08
#define CUT_SLOT_NAME    0x20
#define CUT_SLOTS        8
#define CUT_NAME_MAX     24

// Accepted cinematic prefixes. "cut_" is what the confirmed cinematic
// cutscene used ("cut_f406") while conversations used "en_npc_*" and bare
// character names. If a run turns up a cinematic with a different prefix,
// the [cutname] lines below will show it and it gets added here.
static const char *g_cutPrefixes[] = { "cut_" };
#define CUT_PREFIX_N (sizeof(g_cutPrefixes) / sizeof(g_cutPrefixes[0]))

static char g_cutsceneNamePend[CUT_NAME_MAX];

static int CutsceneNameIsCinematic(const char *nm)
{
    size_t i;
    for (i = 0; i < CUT_PREFIX_N; i++) {
        size_t n = strlen(g_cutPrefixes[i]);
        if (_strnicmp(nm, g_cutPrefixes[i], n) == 0) return 1;
    }
    return 0;
}

// Log each DISTINCT cut name once. This is the safety net for the prefix
// list: whatever the game actually calls its cinematics, one run puts the
// full vocabulary in the log, with a mark showing which ones we treated as
// cinematic. Cheap - the distinct set is small and it is capped.
#define CUT_NAME_SEEN_MAX 48
static char g_cutSeen[CUT_NAME_SEEN_MAX][CUT_NAME_MAX];
static LONG g_cutSeenN = 0;

static void CutsceneNoteName(const char *nm)
{
    LONG i, n = g_cutSeenN;
    if (n >= CUT_NAME_SEEN_MAX) return;
    for (i = 0; i < n; i++) if (strcmp(g_cutSeen[i], nm) == 0) return;
    strncpy(g_cutSeen[n], nm, CUT_NAME_MAX - 1);
    g_cutSeen[n][CUT_NAME_MAX - 1] = 0;
    g_cutSeenN = n + 1;
    {
        char l[128];
        sprintf(l, "[cutname] \"%s\" -> %s", nm,
                CutsceneNameIsCinematic(nm) ? "CINEMATIC (shadow distance held)"
                                            : "ignored (dialogue/prompt/other)");
        LogLine(l);
    }
}

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
// PASS 2. Pass 1 watched 8 controller fields and found the opposite of what
// was assumed: across the run's long cutscene every one of them was FROZEN,
// while during gameplay they churned every few frames. So that churn is
// gameplay-side cinema activity (automatic camera / ambient contexts) and
// none of those fields is a play flag - the window was simply too narrow.
//
// The controller's constructor says where to look instead. It allocates three
// sub-objects into the pointers that stayed rock-constant all run:
//    +0x24 -> 0x2fc0 bytes (FUN_00909000)
//    +0x28 -> 0x2030 bytes (FUN_0090fd50)
//    +0x2c -> 0x80   bytes (FUN_00916ad0)
// The cutscene player's state machine (STATE_PREFETCH/PLAY/END...) lives
// inside one of those, not in the controller itself. So sample a wide window
// of the controller AND dereference the three sub-objects.
//
// NOISE CONTROL: a wide window logged naively would bury the signal. Each
// watched dword carries its own change counter and STOPS being logged once it
// has changed more than CUT_NOISY times - so per-frame churn silences itself
// after a few lines while rare, state-like transitions keep printing for the
// whole run. A cutscene boundary is exactly a rare transition, and this run's
// single long cutscene should show up as one field flipping twice.
#define CUT_R0_N     64      /* controller  +0x000..+0x0fc */
#define CUT_SUB_N    32      /* each sub-object +0x00..+0x7c */
#define CUT_DIAG_N   (CUT_R0_N + 3 * CUT_SUB_N)
#define CUT_DIAG_MAX 700
#define CUT_NOISY    10      /* changes after which a dword is written off as churn */

static DWORD g_cutDiagLast[CUT_DIAG_N];
static LONG  g_cutDiagChg[CUT_DIAG_N];
static LONG  g_cutDiagLines = 0;
static LONG  g_cutDiagInit = 0;
static char  g_cutDiagName[40];

// Region index -> human label and the base pointer it samples.
static DWORD CutDiagBase(DWORD ctrl, int region)
{
    DWORD p;
    if (region == 0) return ctrl;
    __try {
        p = *(DWORD *)(ctrl + (region == 1 ? 0x24 : region == 2 ? 0x28 : 0x2c));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (p < 0x10000 || (p & 3)) return 0;
    return p;
}

static void CutsceneDiagTick(DWORD ctrl)
{
    DWORD cur[CUT_DIAG_N];
    DWORD base[4];
    int r, i, idx;
    if (g_cutDiagLines >= CUT_DIAG_MAX) return;

    for (r = 0; r < 4; r++) base[r] = CutDiagBase(ctrl, r);
    if (!base[0]) return;

    __try {
        idx = 0;
        for (i = 0; i < CUT_R0_N; i++) cur[idx++] = *(DWORD *)(base[0] + i * 4);
        for (r = 1; r < 4; r++)
            for (i = 0; i < CUT_SUB_N; i++)
                cur[idx++] = base[r] ? *(DWORD *)(base[r] + i * 4) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (!g_cutDiagInit) {
        char l[224];
        g_cutDiagInit = 1;
        for (i = 0; i < CUT_DIAG_N; i++) g_cutDiagLast[i] = cur[i];
        sprintf(l, "[cutdiag] pass2 baseline: ctrl=%08lx sub24=%08lx sub28=%08lx sub2c=%08lx"
                   " | R0=ctrl+0x000..0fc, R1/R2/R3=sub+0x00..7c, noisy cutoff=%d",
                (unsigned long)base[0], (unsigned long)base[1],
                (unsigned long)base[2], (unsigned long)base[3], CUT_NOISY);
        LogLine(l);
        return;
    }

    for (i = 0; i < CUT_DIAG_N; i++) {
        if (cur[i] == g_cutDiagLast[i]) continue;
        {
            DWORD old = g_cutDiagLast[i];
            LONG  c = ++g_cutDiagChg[i];
            g_cutDiagLast[i] = cur[i];
            if (c > CUT_NOISY) continue;             /* churn - written off */
            r = (i < CUT_R0_N) ? 0 : 1 + (i - CUT_R0_N) / CUT_SUB_N;
            {
                int off = (i < CUT_R0_N) ? i * 4 : ((i - CUT_R0_N) % CUT_SUB_N) * 4;
                char l[200];
                sprintf(l, "[cutdiag] f=%-6ld R%d+0x%02x  %08lx -> %08lx  (chg %ld%s)",
                        (long)g_frameSeq, r, off,
                        (unsigned long)old, (unsigned long)cur[i], c,
                        c == CUT_NOISY ? ", now muted as churn" : "");
                LogLine(l);
                if (InterlockedIncrement(&g_cutDiagLines) >= CUT_DIAG_MAX) {
                    LogLine("[cutdiag] line cap reached - no further [cutdiag] output this run");
                    return;
                }
            }
        }
    }

    // The controller holds a short name buffer around +0x40 (pass 1 saw ASCII
    // tails there: "dd", "es", "on2", "d2z"). Print it whole when it changes -
    // if it names the playing cutscene it is a signal in its own right.
    __try {
        char nm[36];
        int k;
        for (k = 0; k < 32; k++) {
            char ch = *(char *)(base[0] + 0x40 + k);
            nm[k] = (ch >= 0x20 && ch < 0x7f) ? ch : (ch == 0 ? 0 : '.');
            if (!ch) break;
        }
        nm[k >= 32 ? 32 : k] = 0;
        if (strcmp(nm, g_cutDiagName) != 0) {
            char l[120];
            strcpy(g_cutDiagName, nm);
            sprintf(l, "[cutdiag] f=%-6ld name@+0x40 = \"%s\"", (long)g_frameSeq, nm);
            LogLine(l);
            InterlockedIncrement(&g_cutDiagLines);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    if (!g_cutsceneRevert) {
        if (g_cutsceneActive) InterlockedExchange(&g_cutsceneActive, 0);
        return;
    }

    if (g_cutsceneMode == 1) {
        // MODE 1 - the controller flag at +0x5c. Kept because it is the
        // broadest signal available and one ini key away, but it is NOT
        // "a cutscene is playing": confirmed in game to also rise for
        // ordinary NPC dialogue and even for UI prompts like the teleporter
        // asking whether to go back down. Mode 2 is the shipping default.
        if (g_cutsceneFlagMask == 0) {
            if (g_cutsceneActive) InterlockedExchange(&g_cutsceneActive, 0);
            return;
        }
        __try {
            DWORD v = *(DWORD *)(ctrl + (DWORD)g_cutsceneFlagOff);
            want = (v & (DWORD)g_cutsceneFlagMask) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        g_cutsceneName[0] = 0;
    } else {
        // MODE 2 (default) - NAMED CUT SLOTS. The player object at
        // controller+0x28 holds an array of slots; each carries an id, an
        // active flag and the cut's NAME, and the name is what separates a
        // real cinematic from a conversation:
        //     "cut_f406"    <- the cinematic cutscene
        //     "en_npc_0..." <- talking to an NPC
        //     "lightning"   <- short character cinema
        // so a slot only counts when its name starts with an accepted
        // cinematic prefix. Every activating name is logged (capped), so if
        // some cutscene uses a prefix not listed here, one run says so
        // instead of leaving it a mystery.
        want = 0;
        __try {
            DWORD player = *(DWORD *)(ctrl + 0x28);
            if (player >= 0x10000 && !(player & 3)) {
                int slot;
                for (slot = 0; slot < CUT_SLOTS; slot++) {
                    DWORD sb = player + CUT_SLOT_BASE + (DWORD)slot * CUT_SLOT_STRIDE;
                    char nm[CUT_NAME_MAX];
                    int k;
                    if (*(DWORD *)(sb + CUT_SLOT_ACTIVE) == 0) continue;
                    for (k = 0; k < CUT_NAME_MAX - 1; k++) {
                        char ch = *(char *)(sb + CUT_SLOT_NAME + k);
                        if (ch == 0) break;
                        if (ch < 0x20 || ch >= 0x7f) { k = 0; break; }   /* not a name */
                        nm[k] = ch;
                    }
                    nm[k] = 0;
                    if (!k) continue;
                    CutsceneNoteName(nm);
                    if (CutsceneNameIsCinematic(nm)) {
                        want = 1;
                        strncpy(g_cutsceneNamePend, nm, sizeof(g_cutsceneNamePend) - 1);
                        g_cutsceneNamePend[sizeof(g_cutsceneNamePend) - 1] = 0;
                        break;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
    }

    // Instant in both directions - no hold counter. See the note in
    // 03_render_state.c for the debounce that was considered and dropped.
    if (want != g_cutsceneActive) {
        InterlockedExchange(&g_cutsceneActive, want);
        InterlockedIncrement(&g_cutsceneEdges);
        if (want) strcpy(g_cutsceneName, g_cutsceneNamePend);
        else      g_cutsceneName[0] = 0;
        {
            char l[220];
            sprintf(l, "[cutscene] %s%s%s (edge #%ld, split neutralised %ld frames so far)",
                    want ? "ENTER - shadow distance neutralised to 100%"
                         : "EXIT - shadow distance restored to user setting",
                    want && g_cutsceneName[0] ? " cut=" : "",
                    want ? g_cutsceneName : "",
                    g_cutsceneEdges, g_cutsceneSuppressed);
            LogLine(l);
        }
    }
}
