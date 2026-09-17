// ---- Talk-entry lifetime: a frame-denominated countdown --------------------
// FieldTalkManager::update (FUN_005de2c0) runs once per frame out of the field
// subsystem list (FUN_005b5050) over a fixed pool of 4 talk entries, each
// driven by a 16-state machine. State 0 seeds the entry's timer:
//
//     entry[0x24] = rand(50000) * 0.0001f + 5.0f;        // [5.0, 10.0)
//
// and state 10 counts it down, tearing the entry down when it hits zero:
//
//     entry[0x24] -= 0.05f;                              // <-- per FRAME
//     if (entry[0x24] <= 0.0f) state = 0xc;              // -> FUN_005dbc70,
//                                                        //    frees the slot
//
// There is no delta-time term anywhere in that path. The step is a literal,
// confirmed against the instruction stream rather than trusted from the
// decompiler (the decompiler said "- 0.05" while the float constant sweep
// found no 0.05f reader in this function - the constant is a DOUBLE):
//
//     005e0986  SUBSD XMM0, qword ptr [0x00df8fe0]
//     [0x00df8fe0] = 0.05000000074505806                 // a 0.05f widened
//
// entry[0x24] has exactly three touch points in the whole function - init to
// 0, init to the random seed, and this decrement - so the reading is not
// ambiguous. A talk entry therefore lives 100-200 FRAMES: 3.3-6.7s at 30fps
// but only 1.7-3.3s at 60. The lifetime is halved, and the random seed makes
// failure probabilistic rather than certain - which is the exact shape of the
// reported 60fps bugs (each has a CHANCE to fail; 30fps mode fixes all four).
//
// This rewrites the instruction's disp32 to point at a double we own, so the
// step can be rescaled live from the GUI without ever writing code again. The
// engine's 0.05 constant is SHARED with four unrelated ADDSD sites, which is
// exactly why the constant must not be edited in place.
//
// RETIRED (ENABLE_TALK_TIMER). The mechanism above is real and the analysis
// holds - this genuinely is a frame-denominated countdown with no delta-time
// term - but it was never connected to a bug. It is NOT the Gysahl cause
// (that was a script-side yield bug, fixed in scr104.clb), and user testing
// found no effect on anything else. Retiring it also removes the only place
// this mod rewrites an instruction operand in the game's code section.
#define TALK_STEP_INSN_RVA (0x005e0986 - 0x00400000)
static volatile LONG g_talkTimerPct = 0;   // 0 = untouched, 50 = half speed
static double g_talkStep = 0.05;           // the constant the game will read
static DWORD  g_talkStepOrigDisp = 0;
static int    g_talkStepPatched = 0;       // 0 = not yet, 1 = done, -1 = refused

#if ENABLE_TALK_TIMER
static void ApplyTalkTimerScale(void)
{
    if (g_mainModBase == 0) return;
    LONG pct = g_talkTimerPct;
    if (pct <= 0) return;                  // off: never patch at all

    // Refresh our constant every call, so a GUI change lands on the next frame
    // with no further code writes.
    g_talkStep = 0.05 * (double)pct / 100.0;
    if (g_talkStepPatched) return;

    __try {
        unsigned char *insn = (unsigned char *)(g_mainModBase + TALK_STEP_INSN_RVA);
        if (insn[0] != 0xF2 || insn[1] != 0x0F ||
            insn[2] != 0x5C || insn[3] != 0x05) {
            LogLine("[talk] step tweak REFUSED: not SUBSD XMM0,[disp32] at the "
                    "expected RVA - wrong exe build?");
            g_talkStepPatched = -1;
            return;
        }
        DWORD disp = *(DWORD *)(insn + 4);
        double cur = *(double *)disp;
        // Guard against a relocation/version surprise: if the operand is not
        // the 0.05 we identified, this is not the instruction we think it is.
        if (cur < 0.0499 || cur > 0.0501) {
            char l[160];
            sprintf(l, "[talk] step tweak REFUSED: operand at 0x%08X is %.6f, "
                       "expected 0.05", disp, cur);
            LogLine(l);
            g_talkStepPatched = -1;
            return;
        }
        DWORD oldProtect;
        if (!VirtualProtect(insn + 4, 4, CODE_PAGE_WRITABLE, &oldProtect)) {
            LogLine("[talk] step tweak: VirtualProtect failed");
            return;                        // retry on the next tick
        }
        g_talkStepOrigDisp = disp;
        *(DWORD *)(insn + 4) = (DWORD)&g_talkStep;
        VirtualProtect(insn + 4, 4, oldProtect, &oldProtect);
        g_talkStepPatched = 1;
        {
            char l[192];
            sprintf(l, "[talk] step tweaked: 0.05 -> %.4f (%ld%%) | operand "
                       "0x%08X -> 0x%08X | talk entries now live %.1f-%.1f "
                       "frames", g_talkStep, pct, g_talkStepOrigDisp,
                    (DWORD)&g_talkStep, 5.0 / g_talkStep, 10.0 / g_talkStep);
            LogLine(l);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#endif  // ENABLE_TALK_TIMER

#if ENABLE_GYSAHL_DIAG
// ---- FA-object schedule diagnostic ----------------------------------------
// The Gysahl Green plot bug: planting sets the PLOT inert (script line 1139)
// and is then supposed to switch the separate PLANT entity on (line 1150).
// Both are the same call - Field.changeFaObjectSchedule - and the first works
// while the second sometimes does not. That native no-ops SILENTLY when its
// name lookup fails, so from the script side the failure is invisible.
//
//   FUN_009c0490:
//     iVar6 = FUN_0073cfb0(objName);       // resolve name -> string-table entry
//     if (iVar6 != 0) {                    // <-- everything is inside this
//         uVar7  = *(uint *)(iVar6 + 0x14) & 0xffff;      // u16Index
//         piVar9 = DAT_024d252c + 0x82450 + uVar7 * 0x30; // registry entry
//         ...write the new schedule...
//     }                                    // no else, returns void
//
// Script natives all take one arg: the VM context. Both string arguments are
// readable from its stack, per the same decompile:
//     stack = *(int *)(ctx + 0x958);
//     schedName = *(char **)(stack + 4);   objName = *(char **)(stack + 0xc);
//
// Logging the call alone cannot distinguish "the write failed" from "the write
// landed and something reverted it later", so this dumps the target's registry
// entry BEFORE and AFTER, using the return-address hijack already used by the
// pacer hooks. Indices come from r_fao_def.json (u16Index), which is why only
// the eight known farm entities are decoded - anything else still logs, just
// without a registry dump.
#define FA_SCHED_RVA        (0x009c0490 - 0x00400000)
#define FA_REGISTRY_PTR_RVA (0x024d252c - 0x00400000)
#define FA_REGISTRY_BASE_OFF 0x82450
#define FA_ENTRY_STRIDE      0x30

static volatile LONG g_logFaSchedule = 0;
static volatile LONG g_faSchedCalls = 0;
// Tentative definitions - the real ones sit with the window-trace block below,
// which needs to come after the detours it references. C merges them.
static volatile LONG g_plantWatchUntil;
static volatile LONG g_winWaitPolls;
static volatile LONG g_winClosePolls;
static volatile LONG g_faCmpFix;
static volatile LONG g_watchPlantBuf;
static void ArmWatchpointSelf(unsigned int addr);
static void DisarmWatchpointSelf(void);
static char g_faPlantPlot[40];
static const char *g_faPlantPlotPtr;
static void *g_faSchedTarget = NULL;
static void *g_trampoline_faSched = NULL;

// r_fao_def.json u16Index. Plots 0009-0012 -> 421-424, plants 0016-0019 -> 427-430.
static int FaIndexForName(const char *n)
{
    if (!n || strncmp(n, "pm_faoF03_00", 12) != 0) return -1;
    if (strcmp(n, "pm_faoF03_0009") == 0) return 421;
    if (strcmp(n, "pm_faoF03_0010") == 0) return 422;
    if (strcmp(n, "pm_faoF03_0011") == 0) return 423;
    if (strcmp(n, "pm_faoF03_0012") == 0) return 424;
    if (strcmp(n, "pm_faoF03_0016") == 0) return 427;
    if (strcmp(n, "pm_faoF03_0017") == 0) return 428;
    if (strcmp(n, "pm_faoF03_0018") == 0) return 429;
    if (strcmp(n, "pm_faoF03_0019") == 0) return 430;
    return -1;
}

// One pending call at a time: the script VM is single-threaded and this native
// is a leaf, so it cannot nest. Thread id is recorded anyway so a surprise
// concurrent call is dropped rather than corrupting the pairing.
static DWORD g_faPendTid = 0;
static DWORD g_faPendRet = 0;
static char  g_faPendObj[40];
static char  g_faPendSch[40];
static int   g_faPendIdx = -1;
static DWORD g_faPendBefore[6];

static DWORD *FaEntryPtr(int idx)
{
    if (idx < 0 || g_mainModBase == 0) return NULL;
    DWORD regPtr = *(DWORD *)(g_mainModBase + FA_REGISTRY_PTR_RVA);
    if (!regPtr) return NULL;
    return (DWORD *)(regPtr + FA_REGISTRY_BASE_OFF + (DWORD)idx * FA_ENTRY_STRIDE);
}

static void FaSnapshot(int idx, DWORD *out6)
{
    int i;
    for (i = 0; i < 6; i++) out6[i] = 0xDEADBEEF;
    __try {
        DWORD *e = FaEntryPtr(idx);
        if (!e) return;
        for (i = 0; i < 6; i++) out6[i] = e[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// v2. The first build logged NOTHING despite the hook installing correctly.
// Cause was here, not in the game: the whole body sat in ONE __try, so when
// the stack-argument guess faulted, control jumped straight to __except and
// the ECX fallback never executed. Each candidate now gets its own guarded
// probe, and the first 20 calls dump their raw inputs so a wrong convention
// is visible in the log instead of silently producing nothing.
static volatile LONG g_faRawLogged = 0;

// UNSIGNED throughout. v2 read the context correctly and then threw it away:
// the probe returned 0xDD81F228 (this exe is large-address-aware, so its heap
// sits above 2GB) and the validity test `stack > 0x10000` was a SIGNED compare,
// which is false for any pointer with the high bit set. Every call failed the
// check despite the read having worked.
static unsigned int ProbeVmStack(void *ctx)
{
    unsigned int v = 0;
    if (!ctx) return 0;
    __try { v = *(unsigned int *)((unsigned int)ctx + 0x958); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

// Returns 1 to request the return hijack (so the AFTER snapshot can be taken).
__declspec(noinline) int __cdecl OnEnter_faSched_C(void *ctxStack, void *ctxEcx, void *retAddr)
{
    // Runs for the fix as well as for logging: the repair needs the plot name
    // remembered from the hatake3 write even when diagnostics are off.
    if ((!g_logFaSchedule && !g_faCmpFix) || g_mainModBase == 0) return 0;

    unsigned int stackA = ProbeVmStack(ctxStack);
    unsigned int stackB = ProbeVmStack(ctxEcx);
    unsigned int ctx = 0, stack = 0;
    if (stackA > 0x10000)      { ctx = (unsigned int)ctxStack; stack = stackA; }
    else if (stackB > 0x10000) { ctx = (unsigned int)ctxEcx;   stack = stackB; }

    if (g_logFaSchedule && InterlockedIncrement(&g_faRawLogged) <= 20) {
        char l[220];
        sprintf(l, "[fa-raw] #%ld ctxStack=%p ctxEcx=%p stackA=0x%08X stackB=0x%08X -> ctx=0x%08X",
                g_faRawLogged, ctxStack, ctxEcx, (unsigned)stackA, (unsigned)stackB,
                (unsigned)ctx);
        LogLine(l);
    }
    if (!ctx) return 0;

    __try {
        const char *sch = *(const char **)(stack + 4);
        const char *obj = *(const char **)(stack + 0xc);
        if (!obj || !sch) return 0;

        // Log the first few name pairs unfiltered too - if the argument slots
        // are not where the decompile suggested, that shows up here as
        // garbage rather than as an empty log.
        if (g_logFaSchedule && g_faRawLogged <= 20) {
            char l[220];
            sprintf(l, "[fa-raw]   obj='%.32s' sch='%.32s'", obj, sch);
            LogLine(l);
        }

        if (strncmp(obj, "pm_fao", 6) != 0) return 0;   // farm/field objects only

        // Arm on the plot going inert - that IS the planting action, and the
        // name is still intact at this moment.
        if (strcmp(sch, "fsh_fao_hatake3") == 0) {
            // Event-scoped, not time-scoped. The old 30s deadline was
            // arbitrary and silently disabled the repair if the player left
            // the message box open longer than that - a real hole, since the
            // captured data shows wait length has NO bearing on whether the
            // bug fires (bugged at 4 and 15 polls; healthy across 4-18).
            // The long deadline below is only a backstop so a planting that
            // never completes cannot leave this armed for the whole session.
            g_plantWatchUntil = (LONG)(GetTickCount() + 600000);
            g_winWaitPolls = 0;
            g_winClosePolls = 0;
            lstrcpynA(g_faPlantPlot, obj, sizeof(g_faPlantPlot));
            g_faPlantPlotPtr = obj;
            if (g_watchPlantBuf) ArmWatchpointSelf((unsigned int)obj);
            if (g_logFaSchedule)
                LogLine("[fa-win] --- planting started, watch armed ---");
        }
        // Disarm on success: the plant switching on is the last step of the
        // sequence, so nothing after it needs repairing.
        else if (strcmp(sch, "fsh_fao_yasai1") == 0 && g_plantWatchUntil != 0) {
            g_plantWatchUntil = 0;
            g_faPlantPlotPtr = NULL;
            g_faPlantPlot[0] = '\0';
            if (g_logFaSchedule)
                LogLine("[fa-win] --- plant activated, watch disarmed ---");
        }

        if (!g_logFaSchedule) return 0;   // fix-only mode: no before/after dump
        InterlockedIncrement(&g_faSchedCalls);
        g_faPendTid = GetCurrentThreadId();
        g_faPendRet = (DWORD)retAddr;
        lstrcpynA(g_faPendObj, obj, sizeof(g_faPendObj));
        lstrcpynA(g_faPendSch, sch, sizeof(g_faPendSch));
        g_faPendIdx = FaIndexForName(obj);
        FaSnapshot(g_faPendIdx, g_faPendBefore);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

__declspec(noinline) void *__cdecl OnReturn_faSched_C(void)
{
    void *ret = (void *)g_faPendRet;
    __try {
        if (g_faPendTid == GetCurrentThreadId()) {
            DWORD after[6];
            FaSnapshot(g_faPendIdx, after);
            int changed = 0, i;
            for (i = 0; i < 6; i++) if (after[i] != g_faPendBefore[i]) changed = 1;
            char line[320];
            sprintf(line,
                "[fa] %s -> %s | idx=%d | %s | before %08X %08X %08X %08X %08X %08X"
                " | after %08X %08X %08X %08X %08X %08X",
                g_faPendObj, g_faPendSch, g_faPendIdx,
                g_faPendIdx < 0 ? "NO-INDEX"
                                : (changed ? "CHANGED" : "*** UNCHANGED ***"),
                g_faPendBefore[0], g_faPendBefore[1], g_faPendBefore[2],
                g_faPendBefore[3], g_faPendBefore[4], g_faPendBefore[5],
                after[0], after[1], after[2], after[3], after[4], after[5]);
            LogLine(line);
            g_faPendTid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

__declspec(naked) void OnReturn_faSched(void)
{
    __asm {
        pushfd
        call OnReturn_faSched_C
        popfd
        jmp eax
    }
}

__declspec(naked) void Detour_faSched(void)
{
    __asm {
        push ecx
        push edx
        // After the two pushes: [esp+4]=saved ECX, [esp+8]=return address,
        // [esp+12]=the native's stack arg (the VM context). ECX is passed too
        // in case the convention differs from what the decompiler inferred.
        // __cdecl args go right-to-left, so push retAddr, ecx, ctx in that
        // order to land as (ctxStack, ctxEcx, retAddr).
        mov eax, [esp + 8]      // return address        -> 3rd param
        push eax
        mov eax, [esp + 8]      // saved ECX             -> 2nd param
        push eax
        mov eax, [esp + 20]     // VM context stack arg  -> 1st param
        push eax
        call OnEnter_faSched_C
        add esp, 12
        test eax, eax
        jz skip_faSched
        mov dword ptr [esp + 8], offset OnReturn_faSched
    skip_faSched:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_faSched]
    }
}

// ---- Script timer-callback registration, same investigation ---------------
// The first bugged planting captured in the log showed the PLOT write
// (script line 1139) present and the PLANT write (line 1154) ENTIRELY ABSENT -
// not a silent no-op, simply never called. So execution stopped, or the branch
// was never taken, between those two points:
//
//   1151  } else if (sfIsSameStrings(string, "pm_faoF03_0010")) {
//   1152      sfSetTimerCallBack("YasaiTimer2");      <-- this hook
//   1153      sfSetTimerCallBack("YasaiTimerH2");     <-- and this
//   1154      sfSetFaAiSch("pm_faoF03_0018", ...);    <-- known missing
//
// The timers are registered BEFORE the failing call, so their presence splits
// "entered the branch" from "never got there". Entry-only: we only need to
// know whether it ran, not what it returned.
//
// Prologue verified in the shipped exe: 55 8B EC 83 EC 08, and 8B 55 08 /
// 8B 8A 58 09 00 00 confirms the arg really is a stack parameter holding the
// VM context read at +0x958 - the same layout this file already assumes.
#define TIMER_CB_RVA (0x009c22a0 - 0x00400000)
static void *g_trampoline_timerCb = NULL;

__declspec(noinline) void __cdecl OnEnter_timerCb_C(void *ctxStack)
{
    if (!g_logFaSchedule) return;
    unsigned int stack = ProbeVmStack(ctxStack);
    if (stack <= 0x10000) return;
    __try {
        const char *name = *(const char **)(stack + 4);
        if (!name) return;
        // Only the farm timers matter here; the game registers many others.
        if (strncmp(name, "Yasai", 5) != 0) return;
        char l[120];
        sprintf(l, "[fa-timer] setRelativeTimerCallback('%.32s')", name);
        LogLine(l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void Detour_timerCb(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 12]     // VM context stack arg
        push eax
        call OnEnter_timerCb_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_timerCb]
    }
}

// ---- Bisecting sfShowWindowWithKeyWait ------------------------------------
// Runtime capture: plot write (line 1139) present, timers and plant write
// ABSENT. So execution dies between 1139 and 1152, and the only substantial
// thing there is the blocking message box at line 1140:
//
//   1866  n = Window.showMessageWindow(...)
//   1868  while (Window.isWindowOpening(n))              { delay(); }        loop 1
//   1872  while (Window.isWaitingDecideOrCancel(name))   { delay(); delay(); } loop 2
//   1877  Window.hideWindow(n)
//   1878  while (Window.isWindowClosing(name))           { delay(); }        loop 3
//
// All three predicates were checked in the disassembly and all return FALSE
// for a missing window (isWaitingDecideOrCancel zeroes EBX up front and only
// sets it on the success path), so none can spin on a destroyed window - the
// hang theory is dead. Bisect it live instead: hideWindow is called exactly
// once, between loops 2 and 3, so its presence splits "stuck in loop 1/2"
// from "the wait finished and the problem is after line 1140".
//
// Only traced while a planting is in flight - armed by the hatake3 write - so
// the game's constant window traffic does not drown the signal.
#define WIN_SHOW_RVA  (0x009da120 - 0x00400000)
#define WIN_HIDE_RVA  (0x009d7c70 - 0x00400000)
#define WIN_WAIT_RVA  (0x009dac70 - 0x00400000)

static void *g_trampoline_winShow = NULL;
static void *g_trampoline_winHide = NULL;
static void *g_trampoline_winWait = NULL;
// (declared tentatively up in the FA-schedule block, which references them)

static int PlantWatchActive(void)
{
    LONG until = g_plantWatchUntil;
    return until != 0 && (LONG)(GetTickCount() - (DWORD)until) < 0;
}

__declspec(noinline) void __cdecl OnWinShow_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    g_winWaitPolls = 0;
    LogLine("[fa-win] showMessageWindow");
}

__declspec(noinline) void __cdecl OnWinHide_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    char l[96];
    sprintf(l, "[fa-win] hideWindow  (loop2 polls=%ld)", g_winWaitPolls);
    LogLine(l);
}

// Spin detector: loop 2 polls this. A healthy wait is however many frames the
// player takes to press the button; an unbounded count means we never left it.
__declspec(noinline) void __cdecl OnWinWait_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    LONG n = InterlockedIncrement(&g_winWaitPolls);
    if (n == 1 || n == 100 || n == 500 || (n % 2000) == 0) {
        char l[96];
        sprintf(l, "[fa-win] isWaitingDecideOrCancel poll #%ld", n);
        LogLine(l);
    }
}

__declspec(naked) void Detour_winShow(void)
{
    __asm {
        pushad
        pushfd
        call OnWinShow_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winShow]
    }
}
__declspec(naked) void Detour_winHide(void)
{
    __asm {
        pushad
        pushfd
        call OnWinHide_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winHide]
    }
}
__declspec(naked) void Detour_winWait(void)
{
    __asm {
        pushad
        pushfd
        call OnWinWait_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winWait]
    }
}

// ---- Final bisection: loop 3, and the branch dispatch after it ------------
// hideWindow is REACHED in the bugged case, so loops 1 and 2 complete. All
// four wait predicates were disassembled and every one returns false for a
// missing window, so none can spin for that reason. Two possibilities remain:
//
//   * loop 3 spins for some other reason
//         while (Window.isWindowClosing(name)) { White.delay(); }
//   * the wait returns fine and the script simply stops being resumed, i.e.
//     the VM drops the coroutine - in which case nothing after line 1140 runs
//
// isWindowClosing gets a poll counter (an unbounded climb = stuck in loop 3).
// White.stringComp is what sfIsSameStrings compiles to, so it is the branch
// dispatch at script lines 1147/1151 - seeing it prove the chain was reached
// separates "died in the wait" from "died in the dispatch".
#define WIN_CLOSING_RVA (0x009db870 - 0x00400000)
#define STRCMP_RVA      (0x009d2130 - 0x00400000)
static void *g_trampoline_winClosing = NULL;
static void *g_trampoline_strCmp = NULL;
static volatile LONG g_winClosePolls;

__declspec(noinline) void __cdecl OnWinClosing_C(void)
{
    if (!g_logFaSchedule || !PlantWatchActive()) return;
    LONG n = InterlockedIncrement(&g_winClosePolls);
    if (n == 1 || n == 100 || n == 500 || (n % 2000) == 0) {
        char l[96];
        sprintf(l, "[fa-win] isWindowClosing poll #%ld", n);
        LogLine(l);
    }
}

// ---- CONFIRMED MECHANISM (capture v30, log line 23156) --------------------
// Bugged planting: the branch chain RAN, but the script's `string` parameter
// was an EMPTY STRING after the blocking message box:
//     stringComp('','pm_faoF03_0009') ... ('','pm_faoF03_0012')  - all fail
// versus healthy: stringComp('pm_faoF03_0009','pm_faoF03_0009').
// The same variable was intact at the hatake3 plot write moments earlier, so
// its backing storage is invalidated DURING the multi-frame wait. With no
// else clause in the script, all four compares miss and the plant is never
// activated. Root cause of the Gysahl Green plot bug.
//
// The repair: the plot name is known - it is the object of the hatake3 write
// we just logged. When, inside the plant-watch window, a compare arrives with
// an EMPTY first arg and a plot literal as the second, rewrite the first-arg
// VM cell to point at the remembered name. The native then routes the script
// into the correct branch itself; lines 1148-1163 use only literals, so the
// dead variable is never needed again. Tightly scoped + loudly logged.
static volatile LONG g_faCmpFix = 1;          // "FaCmpFix" toggle
static volatile LONG g_faCmpFixCount = 0;
static char g_faPlantPlot[40];                // last plot set to hatake3
static const char *g_faPlantPlotPtr = NULL;   // the pointer the VM used then
static char g_faRepairBuf[40];                // stable storage for the rewrite

// Two string args, same VM stack layout as changeFaObjectSchedule:
// arg2 (literal) at +4, arg1 (the script variable) at +0xc.
__declspec(noinline) void __cdecl OnStrCmp_C(void *ctxStack)
{
    if ((!g_logFaSchedule && !g_faCmpFix) || !PlantWatchActive()) return;
    unsigned int stack = ProbeVmStack(ctxStack);
    if (stack <= 0x10000) return;
    __try {
        const char *b = *(const char **)(stack + 4);
        const char *a = *(const char **)(stack + 0xc);
        if (!a || !b) return;
        if (strncmp(a, "pm_faoF03_", 10) != 0 && strncmp(b, "pm_faoF03_", 10) != 0) return;
        if (g_logFaSchedule) {
            // Pointers included: same pointer as the plot write but empty
            // content = backing storage zeroed; different pointer = the VM
            // stack slot itself was overwritten. Distinguishes the two
            // remaining explanations for free on the next capture.
            char l[200];
            sprintf(l, "[fa-cmp] stringComp(a=%p'%.24s', b=%p'%.24s') plotPtr=%p",
                    a, a, b, b, g_faPlantPlotPtr);
            LogLine(l);
        }
        // Repair, gated as tightly as the evidence allows:
        //   1. armed  - a planting is in flight and has not completed
        //   2. a == the EXACT buffer the plot write used (every capture shows
        //      the script re-reads the same address, 006FF39C, on all four
        //      compares, so this holds for the whole chain)
        //   3. that buffer is genuinely empty
        //   4. the comparand is a plot literal
        // Condition 2 is what makes this safe: without it the rule was "any
        // empty string vs any plot name", which could catch an unrelated
        // script. With it we only ever touch the one buffer we watched the
        // game populate seconds earlier.
        // v33: test for MISMATCH, not emptiness. The first capture happened to
        // show an empty buffer, so the guard was written as a[0]=='\0' - too
        // narrow. A later capture had the same buffer holding the raw bytes
        // A8 E0 7F 01, i.e. the pointer 0x017FE0A8 written into it. The
        // clobber is "this stack slot got reused", and the reused content is
        // arbitrary; sometimes it is zeros, sometimes a pointer. Comparing
        // against the remembered name covers every form.
        if (g_faCmpFix && a == g_faPlantPlotPtr && g_faPlantPlot[0] != '\0' &&
            strcmp(a, g_faPlantPlot) != 0 && strncmp(b, "pm_faoF03_00", 12) == 0) {
            lstrcpynA(g_faRepairBuf, g_faPlantPlot, sizeof(g_faRepairBuf));
            // Point the VM's argument cell at our copy. We never write into
            // the game's own buffer: if that stack frame has been handed to
            // something else, writing there would corrupt whatever now owns it.
            *(const char **)(stack + 0xc) = g_faRepairBuf;
            InterlockedIncrement(&g_faCmpFixCount);
            // Dump what the clobber actually left behind. Zeros vs a pointer
            // vs something else is the best lead we have on WHO reuses this
            // stack slot, and it costs nothing to collect on every repair.
            unsigned char *raw = (unsigned char *)a;
            char l[220];
            sprintf(l, "[fa-FIX] repaired to '%s' (vs '%.24s') fix#%ld "
                       "| clobber bytes %02X %02X %02X %02X %02X %02X %02X %02X",
                    g_faRepairBuf, b, g_faCmpFixCount,
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
            LogLine(l);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void Detour_winClosing(void)
{
    __asm {
        pushad
        pushfd
        call OnWinClosing_C
        popfd
        popad
        jmp dword ptr [g_trampoline_winClosing]
    }
}
__declspec(naked) void Detour_strCmp(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 12]
        push eax
        call OnStrCmp_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_strCmp]
    }
}

// ---- Which yield/resume actually corrupts the buffer? ---------------------
// The captured poll counts kill the "every re-entry is a chance" model:
// bugged at 4 and 15 loop iterations, healthy across 4-18. A per-yield dice
// roll would compound with iteration count and cluster the failures at high
// counts. Flat distribution = a SINGLE event at one specific transition.
//
// So find that transition. FUN_009e3670 is the JavaVmCode.cpp interpreter -
// the function whose prologue pushes 0x00D9E0A8, the exact value that ends up
// in the plot-name buffer:
//     55 8B EC        PUSH EBP / MOV EBP,ESP
//     6A FF           PUSH -1
//     68 A8 E0 D9 00  PUSH 0x00D9E0A8      <-- the clobber, in the prologue
//
// Log the tracked buffer on every interpreter entry, but ONLY when its
// contents change - so a whole planting produces two or three lines instead of
// thousands, and the exact entry that destroys it is named. ESP is captured
// too: if the corrupting entry sits at a different stack depth than the
// healthy ones, that is the frame-layout hypothesis confirmed.
// ---- REMOVED: VM interpreter trace (v34) ----------------------------------
// Hooking FUN_009e3670 (the JavaVmCode.cpp bytecode interpreter) BROKE THE
// GAME: with it installed, the planting message box accepted no input and the
// game looped an error sound. That function is re-entered constantly and at
// wildly varying stack depths, and the VM's yield/resume relies on reproducing
// its frame layout exactly - a detour that pushes 36 bytes and makes a call on
// every entry perturbs the very mechanism it was measuring. Do not re-add it.
//
// It did earn its keep before being removed. The trace showed the tracked
// buffer does NOT get destroyed once: it OSCILLATES, dozens of times per
// planting, between the plot name and other frames' data -
//     entry #618  <junk>          -> pm_faoF03_0011   (restored)
//     entry #626  pm_faoF03_0011  -> <junk>           (clobbered)
//     entry #642  <junk>          -> pm_faoF03_0011   (restored)
//     ... and the compares happened to land on a restored moment, so it worked.
// So the VM does save/restore script state across yields, and the bug is a
// RACE: whether the branch compares fall on a restored or a clobbered moment.
// ESP deltas between entries ranged over 1600 bytes, so the same address falls
// inside different frames depending on entry depth.
//
// The right tool for the remaining question is a hardware watchpoint (debug
// registers DR0-DR3): it traps writes without adding a single instruction to
// the hot path, so it cannot perturb what it observes.

// ---- Hardware watchpoint on the plot-name buffer --------------------------
// The software probe (v34, hooking the interpreter) broke the game: it added
// instructions to a hot, re-entrant path whose frame layout is exactly what
// the bug depends on. Debug registers avoid that entirely - the CPU traps the
// write itself, so ZERO instructions are added to the observed code.
//
// DR0 holds the address, DR7 configures it (local enable, write-only, 4 bytes),
// and a vectored exception handler catches the resulting EXCEPTION_SINGLE_STEP
// and records the EIP of whoever wrote. That names the writer directly instead
// of inferring it.
//
// Armed on the hatake3 plot write (we know the buffer address then), capped at
// WATCH_MAX_HITS so the oscillation cannot flood the log or the exception
// machinery, and disarmed when the plant activates.
#define WATCH_MAX_HITS 40
static volatile LONG g_watchPlantBuf = 0;
static volatile LONG g_watchHits = 0;
static PVOID g_vehHandle = NULL;
static unsigned int g_watchAddr = 0;

static void DisarmWatchpointSelf(void)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) return;
    c.Dr0 = 0;
    c.Dr7 &= ~(DWORD)0xF0003;
    SetThreadContext(th, &c);
    g_watchAddr = 0;
}

static void ArmWatchpointSelf(unsigned int addr)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) { LogLine("[fa-wp] GetThreadContext failed"); return; }
    addr &= ~3u;                       // LEN=4 requires 4-byte alignment
    c.Dr0 = addr;
    c.Dr6 = 0;
    // clear DR0's old config, then: L0=1, RW0=01 (write), LEN0=11 (4 bytes)
    c.Dr7 = (c.Dr7 & ~(DWORD)0xF0003) | 0x1 | (0x1u << 16) | (0x3u << 18);
    if (!SetThreadContext(th, &c)) { LogLine("[fa-wp] SetThreadContext failed"); return; }
    g_watchAddr = addr;
    g_watchHits = 0;
    {
        // Read back - setting debug registers on the CURRENT thread is not
        // formally guaranteed, so prove it took rather than assume.
        CONTEXT v;
        memset(&v, 0, sizeof(v));
        v.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        char l[160];
        if (GetThreadContext(th, &v))
            sprintf(l, "[fa-wp] armed at %08X | readback Dr0=%08X Dr7=%08X tid=%lu",
                    addr, (unsigned)v.Dr0, (unsigned)v.Dr7, GetCurrentThreadId());
        else
            sprintf(l, "[fa-wp] armed at %08X (readback failed)", addr);
        LogLine(l);
    }
}

static LONG CALLBACK PlantWatchVeh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if ((ep->ContextRecord->Dr6 & 0x1) == 0)
        return EXCEPTION_CONTINUE_SEARCH;   // not our DR0 breakpoint

    LONG n = InterlockedIncrement(&g_watchHits);
    if (n <= WATCH_MAX_HITS) {
        unsigned int eip = (unsigned int)ep->ContextRecord->Eip;
        unsigned int ghidra = (g_mainModBase && eip > g_mainModBase)
                              ? (eip - g_mainModBase + 0x00400000) : 0;
        const unsigned char *p = (const unsigned char *)g_watchAddr;
        char l[240];
        // A data breakpoint traps AFTER the write, so EIP is the instruction
        // following the store - close enough to identify the writer.
        sprintf(l, "[fa-wp] hit #%ld eip=%08X (ghidra %08X) esp=%08X | now %02X %02X %02X %02X",
                n, eip, ghidra, (unsigned int)ep->ContextRecord->Esp,
                p[0], p[1], p[2], p[3]);
        LogLine(l);
    }
    ep->ContextRecord->Dr6 = 0;
    if (n >= WATCH_MAX_HITS) {
        ep->ContextRecord->Dr7 &= ~(DWORD)0xF0003;   // self-disarm
        LogLine("[fa-wp] hit cap reached, disarmed");
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif  // ENABLE_GYSAHL_DIAG

static volatile LONG g_deadlineClampCount = 0;
static void __cdecl ClampAc3040Deadline(void *pacer);
// DEFAULT ON as of 2026-08-15 (was opt-in). Rewrites PresentationInterval to
// IMMEDIATE at the game's own IDirect3D9::CreateDevice call. See the
// injection site (HookedIDirect3D9CreateDevice) for the full mechanism.
//
// The game never asks for vsync explicitly - it requests
// D3DPRESENT_INTERVAL_DEFAULT, which the D3D9 spec defines as identical to
// INTERVAL_ONE, and being Windowed means DWM paces it (which is also why the
// driver-level "vsync off" toggle does nothing here). The cost is
// quantisation: every gameplay stutter this project measured sits at
// 21-23ms, i.e. "missed the 16.7ms budget by a few ms", and under a 60Hz
// compositor there is no 21ms frame - it waits a full extra interval and
// becomes 33.3ms. The median frame is already 16.5-17.0ms, right on the
// deadline. See RUN_2026-08-15_FOUR_ZONES.md.
//
// The tradeoff, and it is real: without VRR (FreeSync/G-Sync) this tears.
// Shipped on anyway by the user's call - the frame-pacing win applies to
// everyone, tearing only bothers some, and Graphics > Vsync > On restores
// the vanilla behaviour in one click. Worth stating in the mod description
// rather than leaving users to find it.
//
// Residual risk, unchanged: this can desync any engine timing that assumes a
// fixed-cadence Present.
static volatile LONG g_forceImmediatePresentEnabled = 1;
// Opt-in, default OFF. Overwrites the engine's own frame-pacing target.
// See ApplyFramerateUnlock() for the decompiled limiter and the risks.
static volatile LONG g_unlockFramerateEnabled = 1;
// Default ON, deliberately, and the 07_timing_watchdog.c comment saying
// "starts at 0" is STALE HISTORY - do not act on it (2026-08-15: acted on
// it, briefly flipped this to 0, walked it back within the hour). The
// throttle was blamed for the v23 black-geometry regression, then
// exonerated: corruption persisted with it fully OFF and the real cause
// was the DISCARD fix's pointer-only key (PROGRESS.md "CORRECTION:
// ShaderThrottle is NOT harmful"). The user runs 1 on purpose. Whether an
// ENGAGED cap can still cause brief TRANSIENT blackouts (2026-08-15:
// Lightning fully black for a few frames near Ruffian while the queue
// churned) is a separate, open question - A/B via the panel toggle, not
// by editing this default.
static volatile LONG g_shaderThrottleEnabled = 1;
// Default ON - confirmed engaging (bytes prefetched) but never shown to
// measurably help; runtime-toggleable for benchmark isolation like
// everything else, not because it's suspected of causing a problem.
static volatile LONG g_allocatorWarmEnabled = 1;
// Alternative to the DISCARD fix, not a companion to it - see the injection
// site for the reasoning. Default OFF, untested until a user tries it.
// g_noOverwriteFixEnabled removed - the NOOVERWRITE alternative it gated
// caused graphical corruption AND a game crash under user testing. See the
// retirement note at its former injection site (search PROGRESS.md for
// "NOOVERWRITE") for the full account. Deliberately not left as a disabled
// flag - no toggle for this exists anymore.
static void *PatchIat(HMODULE hostModule, const char *moduleName, const char *funcName, void *newFunc);

// The v6/v7 allocation census + memory warmer is defined further down, next
// to the engine-allocator hook it grew out of, but the OS-level allocation
// hooks sit above it and use it - hence these forward declarations.
#define ALLOC_SRC_NAMEDHEAP 0
#define ALLOC_SRC_HEAP      1
#define ALLOC_SRC_VIRTUAL   2
#define ALLOC_SRC_MAPVIEW   3
#define ALLOC_SRC_READFILE  4
#define NUM_ALLOC_SRC       5
static void CensusRecord(int src, size_t bytes);
static void EnqueueWarm(void *ptr, size_t size);
static DWORD WINAPI WarmerThread(LPVOID param);
// Tentative definitions - the real ones sit with the rest of the census
// state further down; C merges them into a single object.
// Texture churn / upload-volume probe counters follow the same pattern: they
// are written deep in the D3D9 hooks but read by the monitor loop, which
// sits earlier in the file.
static volatile LONG g_createTexCount;
static volatile LONG g_texDestroyCount;
static volatile LONG g_uploadKB;
// Split upload volume by thread. Decides whether upload volume can be
// THROTTLED at all: if the engine's memcpy into our staging surfaces runs on
// the main thread, delaying it just moves the stall into the frame we are
// trying to protect. If it runs on loader/worker threads, a byte-rate limit
// on uploads becomes viable the same way PaceRead limits file reads.
static volatile LONG g_uploadKBMain;

static volatile LONG g_mainThreadId;
static DWORD g_censusTls;
static CRITICAL_SECTION g_allocThreadLock;
static CRITICAL_SECTION g_allocImplLock;
static CRITICAL_SECTION g_warmLock;
static double g_cyclesPerUsec;
static volatile LONG g_readFileCount;
static volatile LONGLONG g_readFileBytes;
static volatile LONG g_namedHeapCount;
#define MAX_THREAD_NAMES 64
typedef struct {
    DWORD threadId;
    char name[64];
} ThreadNameEntry;
static ThreadNameEntry g_threadNames[MAX_THREAD_NAMES];
static volatile LONG g_threadNameCount;
static CRITICAL_SECTION g_threadNameLock;
static volatile LONG g_shaderCompileTotal;
static LONG g_shaderIdCount;
static volatile LONG g_shaderCompileRepeats;
static volatile LONG g_readPaceDelayTotalUsec; // monotonic; the windowed one is reset by the monitor
static volatile LONG g_mainCsWaitStartUsec;
static volatile LONG g_mainCsPtr;
static volatile LONG g_mainCsOwner;
static volatile LONG g_mainWfsoStartUsec;
static volatile LONG g_mainWfsoHandle;
static volatile LONG g_mainCsWaitTotalUsec;
static volatile LONG g_mainCsWaitCount;
static volatile LONG g_mainWfsoWaitTotalUsec;
static unsigned int g_mainModBase;
// Moved up from beside ApplyFramerateUnlock so ClampAc3040Deadline (which
// sits earlier in the file, right at the Detour_ac3040 hook) can see them
// too - both features read/write the same engine-owned ticksPerFrame value.
#define FRAME_TARGET_TICKS_RVA (0x05115570 - 0x00400000)
#define FRAME_TARGET_INIT_RVA  (0x05115578 - 0x00400000)
static unsigned int g_mainModSize;
static LONG NowUsec(void);

static void LogD3DWindow(void);
static int InstallD3D9Hook(void);
static unsigned __int64 g_tscBase;
static DWORD WINAPI StutterWatchdogThread(LPVOID param);

