// ---- RDTSC -> microseconds calibration -----------------------------------

static double g_cyclesPerUsec = 0.0;

static void CalibrateTsc(void)
{
    LARGE_INTEGER qpcFreq, qpc1, qpc2;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpc1);
    unsigned __int64 tsc1 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&qpc2);
    unsigned __int64 tsc2 = __rdtsc();

    g_tscBase = tsc1;
    double qpcDeltaSec = (double)(qpc2.QuadPart - qpc1.QuadPart) / (double)qpcFreq.QuadPart;
    double tscDelta = (double)(tsc2 - tsc1);
    g_cyclesPerUsec = tscDelta / (qpcDeltaSec * 1000000.0);

    char line[128];
    sprintf(line, "TSC calibration: %.3f cycles/usec", g_cyclesPerUsec);
    LogLine(line);
}

// ---- per-function state -----------------------------------------------------

#define FN_AACF10  0
#define FN_A2ADA0  1
#define FN_D19A00  2
#define FN_AC3040  3
#define FN_A01A00  4
#define FN_A015B0  5
// v17: FUN_00a41570 - the per-entity update loop found in the class of
// gameplay-era stutters remaining after causes 1/2 were fixed/mitigated and
// the overlay-software theory was directly disconfirmed by the user's test.
// Its only reference Ghidra could find is a DATA address (an indirect
// call/vtable slot, not a call instruction), so static analysis can't trace
// its caller any further - hooking it directly gets the true caller
// (already captured by the existing return-hijack machinery as
// trueRetAddr, just needs logging once) plus real call rate and duration,
// empirically, the same way every other lead in this investigation that hit
// a static-analysis wall was eventually resolved.
#define FN_A41570  6
// v17b: FUN_00aa7850 - the shader-compile queue drain, found via the
// FUN_00a95860/FUN_00a57c70/FUN_00a58630 chain (.vpo/.fpo shader container
// parsing, "CDev.Dw.Renderer.Shader") that a watchdog capture caught live in
// an actual stutter. Its own exit check (FUN_00aa7420) is a genuine 80%-of-
// budget time check between queue items - already throttled in principle -
// but that check only runs BETWEEN items, never during one, so a single
// expensive shader compile could still cost a whole frame regardless of the
// budget. Timing this function directly tests that theory: does duration
// spike during stutters (one slow item), or does call count spike (a large
// backlog all becoming eligible at once)?
#define FN_AA7850  7
// v18 (2026-08-15): FUN_00b46c20 - the heap compactor. The Wild Lands /
// Dead Dunes run settled its rank empirically: 108 of 222 watchdog records
// share the identical chain memmove <- FUN_00b46c20 <- FUN_00b47ac0
// (vtable wrapper, ret 00B47AD9) <- FUN_00a01a00+0x14A (main-thread job
// dispatcher) - the COMPACTION_MANAGER interval stage. Background rate is
// ~1 capture / 30-60s everywhere, spiking to 13-in-2s and ~27-in-9s at the
// Ruffian entry NPC storm, each capture a 21-22ms frame with reads=0 -
// pure in-memory block moves, not I/O, and NOT the class loader (its
// [loader] line stayed loads=0 through the entire stretch). Safety survey
// in ghidra_output/defrag_hook.txt: standard 55 8B EC 83 EC 30 prologue
// (clean 6-byte cut), no SEH, no inbound refs into the stolen bytes, only
// two callers (00b47ad4, 00b47b1b), both direct CALLs.
#define FN_B46C20  8
// v20b: FUN_00aa3250, the texture-upload path. Timed (not just counted) so
// the slow D3DX branch can be priced against the memcpy branch - see
// 23_upload_gate.c. Prologue verified in ghidra_output/upload_gate.txt.
#define FN_AA3250  9
#define NUM_FNS    10

typedef struct {
    const char *name;
    void *target;
    unsigned int rva;
    int patchLen;
    volatile LONG callCount;
    volatile LONG durationCount;
    volatile LONGLONG durationSumCycles; // not perfectly atomic on 32-bit; fine for a diagnostic average
    volatile LONG maxUsec; // windowed - monitor resets each report, same pattern as D3DSlot
} HookedFunc;

static HookedFunc g_funcs[NUM_FNS] = {
    { "FUN_00aacf10", NULL, 0x00aacf10 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00a2ada0", NULL, 0x00a2ada0 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00d19a00", NULL, 0x00d19a00 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00ac3040", NULL, 0x00ac3040 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00a01a00", NULL, 0x00a01a00 - 0x00400000, 5, 0, 0, 0 },
    { "FUN_00a015b0", NULL, 0x00a015b0 - 0x00400000, 5, 0, 0, 0 },
    { "FUN_00a41570", NULL, 0x00a41570 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00aa7850", NULL, 0x00aa7850 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00b46c20", NULL, 0x00b46c20 - 0x00400000, 6, 0, 0, 0 },
    { "FUN_00aa3250", NULL, 0x00aa3250 - 0x00400000, 6, 0, 0, 0 },
};

static void *g_trampoline_aacf10 = NULL;
static void *g_trampoline_a2ada0 = NULL;
static void *g_trampoline_d19a00 = NULL;
static void *g_trampoline_ac3040 = NULL;
static void *g_trampoline_a01a00 = NULL;
static void *g_trampoline_a015b0 = NULL;
static void *g_trampoline_a41570 = NULL;
static void *g_trampoline_aa7850 = NULL;
static void *g_trampoline_b46c20 = NULL;

#define STACK_DEPTH 16
typedef struct {
    void *trueRetAddr;
    unsigned __int64 entryTsc;
    // Classification decided at ENTRY and read back at return. Lives on the
    // per-thread stack rather than in a global because texture uploads run
    // on the loader thread as well as the main one, so a global flag would
    // be corrupted by interleaving. Only FN_AA3250 uses it (0 = fast memcpy
    // path, 1 = D3DX conversion path).
    LONG tag;
} RetFrame;
typedef struct {
    int top;
    RetFrame frames[STACK_DEPTH];
} ThreadStack;

static DWORD g_stackTls[NUM_FNS];

static ThreadStack *GetThreadStack(int fnIdx)
{
    ThreadStack *ts = (ThreadStack *)TlsGetValue(g_stackTls[fnIdx]);
    if (!ts) {
        ts = (ThreadStack *)calloc(1, sizeof(ThreadStack));
        if (ts) {
            TlsSetValue(g_stackTls[fnIdx], ts);
        }
    }
    return ts;
}

// Returns 1 if a frame was recorded (caller should hijack the return
// address), 0 if the per-thread stack was full (caller must NOT hijack -
// leave the true return address alone for this call).
static int OnEnterBookkeeping(void *trueRetAddr, int fnIdx)
{
    InterlockedIncrement(&g_funcs[fnIdx].callCount);
    ThreadStack *ts = GetThreadStack(fnIdx);
    if (!ts || ts->top >= STACK_DEPTH) {
        return 0;
    }
    ts->frames[ts->top].trueRetAddr = trueRetAddr;
    ts->frames[ts->top].entryTsc = __rdtsc();
    ts->frames[ts->top].tag = 0;
    ts->top++;
    return 1;
}

// Same, plus a classification carried through to the return. Used by the
// texture-upload census so a call's duration can be charged to the path it
// actually took.
static int OnEnterBookkeepingTagged(void *trueRetAddr, int fnIdx, LONG tag)
{
    int r = OnEnterBookkeeping(trueRetAddr, fnIdx);
    if (r) {
        ThreadStack *ts = GetThreadStack(fnIdx);
        if (ts && ts->top > 0) ts->frames[ts->top - 1].tag = tag;
    }
    return r;
}

// Structurally only ever called for a call that OnEnterBookkeeping
// successfully recorded (see asm: hijack is conditional on its return
// value), so ts->top > 0 is guaranteed here.
// Tentative def: the frametime-capture block that owns this sits further down,
// but OnReturnBookkeeping below needs to write it.
static volatile LONG g_lastAc3040Usec;

static void *OnReturnBookkeeping(int fnIdx)
{
    ThreadStack *ts = GetThreadStack(fnIdx);
    ts->top--;
    RetFrame f = ts->frames[ts->top];
    unsigned __int64 elapsed = __rdtsc() - f.entryTsc;

    HookedFunc *hf = &g_funcs[fnIdx];
    InterlockedIncrement(&hf->durationCount);
    hf->durationSumCycles += (LONGLONG)elapsed;
    if (g_cyclesPerUsec > 0.0) {
        LONG usec = (LONG)((double)elapsed / g_cyclesPerUsec);
        if (usec > hf->maxUsec) hf->maxUsec = usec;
        // Keep the LAST duration of the frame function specifically, so the
        // frametime capture can carry it as a column. The monitor's windowed
        // average already showed FUN_00ac3040 eating ~4013us of a ~10930us
        // frame with a max of 8434us - a 2.1x spread that closely matches the
        // frametime's own 1.76x fast/slow ratio. Per-frame values settle
        // whether the 9-frame wave lives INSIDE this call or outside it.
        if (fnIdx == FN_AC3040) g_lastAc3040Usec = usec;
        // Compactor deferral bookkeeping: charge this pass against the
        // frame budget, and an over-budget single pass arms the cooldown
        // (the pass itself couldn't be stopped - what CAN be prevented is
        // the next few frames repeating it back to back).
        if (fnIdx == FN_B46C20 && g_compactorDeferEnabled) {
            g_compactorFrameUs += usec;
            if (usec > g_compactorBudgetUs)
                g_compactorCooldown = g_compactorCooldownFrames;
        }
#if ENABLE_UPLOAD_GATE
        // Charge this upload's wall time to the path it took. Counts alone
        // could not say whether the slow path is worth attacking - 1658
        // slow uploads a run is only meaningful next to what they cost.
        if (fnIdx == FN_AA3250) {
            if (f.tag) { InterlockedExchangeAdd(&g_ugSlowUsec, usec);
                         if (usec > g_ugSlowMaxUsec) g_ugSlowMaxUsec = usec; }
            else       { InterlockedExchangeAdd(&g_ugFastUsec, usec); }
        }
#endif
    }

    return f.trueRetAddr;
}

// Written out explicitly rather than macro-generated - MSVC's inline
// assembler doesn't reliably handle multi-line __asm blocks inside macro
// expansion (confirmed: it does not, build caught it immediately with
// C2400 syntax errors). Repetitive, but this is the reliable form.

__declspec(noinline) int __cdecl OnEnter_aacf10_C(void *r) { return OnEnterBookkeeping(r, FN_AACF10); }
__declspec(noinline) void *__cdecl OnReturn_aacf10_C(void) { return OnReturnBookkeeping(FN_AACF10); }
__declspec(naked) void OnReturn_aacf10(void)
{
    __asm {
        pushfd
        call OnReturn_aacf10_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_a2ada0_C(void *r) { return OnEnterBookkeeping(r, FN_A2ADA0); }
__declspec(noinline) void *__cdecl OnReturn_a2ada0_C(void) { return OnReturnBookkeeping(FN_A2ADA0); }
__declspec(naked) void OnReturn_a2ada0(void)
{
    __asm {
        pushfd
        call OnReturn_a2ada0_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_d19a00_C(void *r) { return OnEnterBookkeeping(r, FN_D19A00); }
__declspec(noinline) void *__cdecl OnReturn_d19a00_C(void) { return OnReturnBookkeeping(FN_D19A00); }
__declspec(naked) void OnReturn_d19a00(void)
{
    __asm {
        pushfd
        call OnReturn_d19a00_C
        popfd
        jmp eax
    }
}

// FUN_00ac3040 is the frame pacer: once per frame, main thread only. Its
// caller's thread id therefore identifies the main thread, which the
// allocation warmer needs in order to leave the main thread's own
// allocations alone.
// Confirmed from the v7 log: exactly 30 calls per 500ms window at a locked
// 60fps, so the interval between consecutive entries IS the frame time. That
// makes this the cheapest possible in-process frame-time probe, and - unlike
// PresentMon - it lands in the SAME log, in the SAME 500ms windows, as the
// I/O census. Correlating a stutter with a streaming burst therefore needs no
// clock alignment between two tools.
// ---- Stutter watchdog (v9) ------------------------------------------------
// v8's frame probe overturned the eviction model: in the heaviest streaming
// windows the AVERAGE frame stays at 16.7-17.9ms while a single frame hits
// 30-56ms. One window streamed 8.5MB with a worst frame of 17.0ms; another
// streamed 50MB and still averaged 17.9ms with one 56ms spike. Cache eviction
// would drag every frame down uniformly - it does not. An isolated long frame
// surrounded by perfect ones is a discrete BLOCKING event on the main thread.
//
// So: catch it in the act. The main thread stamps a timestamp and a sequence
// number at each frame boundary; this watchdog polls every 2ms and, once a
// frame has been running longer than STUTTER_THRESHOLD_USEC, suspends the
// main thread, grabs its EIP and walks its EBP chain, and resumes it. Up to
// three samples per bad frame gives a mini-profile of a single stutter.
//
// Addresses are logged already converted to Ghidra VAs, so they can be looked
// up directly without redoing the ASLR arithmetic by hand each session.
//
// Safety: while the main thread is suspended this code calls only
// GetThreadContext (kernel-side, takes no user-mode lock) and reads raw stack
// memory under SEH. It must never take a lock the main thread could be
// holding, and never logs - records go to a ring that the monitor thread
// drains afterwards. Suspension lasts on the order of microseconds and only
// ever happens during a frame that is already ruined.
// Lowered 22000 -> 16700 now that the engine's 59.94fps limiter is bypassed:
// uncapped the game runs 90-130fps (7.7-11.1ms frames), so 16.7ms is a
// genuine 1.5-2x overshoot rather than the normal frame time it used to be.
// Anything over this is, by definition, a frame that failed to sustain 60fps.
//
// This threshold is ONLY safe while the game is genuinely running well above
// 60fps. If a scene is GPU-bound down to ~60, every frame crosses it and the
// watchdog - which SuspendThreads the main thread and walks its stack - would
// fire constantly and become a significant stutter source itself. The rate
// limit below bounds that: detection of every slow frame is the histogram's
// job (free), and this expensive path only needs enough samples to attribute
// causes.
#define STUTTER_THRESHOLD_USEC 16700
// Never suspend the main thread more than this many times per second,
// however many frames cross the threshold.
#define STUTTER_MAX_CAPTURES_PER_SEC 25
static volatile LONG g_stutterRateLimited = 0;   // slow frames whose capture was skipped by the rate limit above
#define STUTTER_MAX_SAMPLES_PER_FRAME 3
#define MAX_STUTTER_RECS 64
#define STUTTER_STACK_DEPTH 16
#define STUTTER_SCAN_DEPTH 14

typedef struct {
    LONG elapsedUsec;
    DWORD eip;
    DWORD esp;
    DWORD stack[STUTTER_STACK_DEPTH];
    LONG stackCount;
    DWORD scan[STUTTER_SCAN_DEPTH];
    LONG scanCount;
    LONG csWaitUsec;
    DWORD csPtr;
    DWORD csOwnerTid;
    LONG wfsoWaitUsec;
    DWORD wfsoHandle;
    LONG readCountInFrame;
    LONG readKbInFrame;
    LONG paceUsecInFrame;
    LONG allocsInFrame;
    volatile LONG ready;
} StutterRec;

static StutterRec g_stutterRecs[MAX_STUTTER_RECS];
static volatile LONG g_stutterWrite = 0;
static volatile LONG g_stutterMissed = 0;

// What the main thread is currently blocked on, if anything. Written only by
// the main thread, read by the watchdog - a torn read costs at worst one
// misleading diagnostic line, never correctness.
// Per-window aggregates of main-thread blocking, so the cost shows up even in
// frames the watchdog did not happen to sample.

static HANDLE g_mainThreadHandle = NULL;
static volatile LONG g_frameStartUsec = 0;
static volatile LONG g_frameSeq = 0;
static unsigned __int64 g_tscBase = 0;

// Frame-scoped deltas, so a captured stutter can be attributed to what
// happened during that specific frame rather than that 500ms window.
static LONG g_frameBaseReadCount = 0;
static LONGLONG g_frameBaseReadBytes = 0;
static LONG g_frameBasePaceUsec = 0;
static LONG g_frameBaseAllocs = 0;

static LONG NowUsec(void)
{
    if (g_cyclesPerUsec <= 0.0) return 0;
    return (LONG)((double)(__rdtsc() - g_tscBase) / g_cyclesPerUsec);
}

static unsigned __int64 g_lastFrameTsc = 0;
static volatile LONG g_maxFrameUsec = 0;   // monitor resets each window
static volatile LONG g_frameCount = 0;
static volatile LONG g_frameSumUsec = 0;
// 0.5ms buckets up to 64ms, last bucket is the overflow catch-all.
#define FRAME_HIST_BUCKETS 129
static LONG g_frameHist[FRAME_HIST_BUCKETS];
// Last window's computed stats, published for the GUI panel's live readout.
// RTSS cannot show these: it measures PRESENTS, and this game's presentation
// runs on its own cadence independent of the logic tick - which is exactly
// why its graph stays flat through a visible hitch. These are the engine's
// real per-tick numbers.
static volatile LONG g_liveP50 = 0, g_liveP99 = 0, g_liveOver16 = 0;
static volatile LONG g_liveFrames = 0, g_liveWorst = 0;

// Ring of recent per-tick frame times for the graph overlay. Written by the
// main thread at the frame boundary (one store, no lock), read by the GUI
// thread while painting. A torn read would at worst draw one wrong bar for
// one repaint, which is not worth a lock in the frame path.
#define FRAME_RING 240
static LONG g_frameRing[FRAME_RING];
static volatile LONG g_frameRingPos = 0;

// ---- Frametime burst capture ---------------------------------------------
// The overlay shows a clearly PERIODIC pattern - clusters of fast frames
// alternating with clusters of slow ones - which is not jitter (jitter gives a
// smooth distribution, this gives two populations). 240 bars eyeballed on a
// screenshot cannot give the period reliably, and the period is the whole
// diagnostic:
//
//   period constant in FRAMES  -> a task that runs every N frames
//   period constant in TIME    -> beating against a fixed-rate process
//                                 (the 59.94Hz sim grid, display refresh, a timer)
//
// Those two want completely different fixes, and telling them apart only needs
// the same capture taken at two different framerates. So: dump raw consecutive
// durations and measure it offline instead of guessing from a picture.
//
// Toggling the flag ON restarts the capture, so it is a "capture from here"
// button - the same pattern the shader-constant dump ended up needing.
#define FT_CAPTURE_MAX 2048
static LONG g_ftBuf[FT_CAPTURE_MAX];
// Second column: CPU time spent in the DRAW_SHADOW pass on the same frame.
// The frametime period turned out to be a fixed 9 FRAMES (invariant across
// 105/101/86 fps), so it is a frame-counted task rather than a beat - and the
// heavy phase's cost scales hard with scene and shadow load (gap 2751us at
// 105fps vs 6425us at 86fps). That points at a periodic render task, and the
// user independently observed the same square pattern appearing/disappearing
// with the shadow resolution setting. Correlating the two directly settles it:
// if the shadow pass carries the same 9-frame square wave, it is the source;
// if it is flat, shadows are innocent and the cycle lives elsewhere.
//
// CAVEAT worth remembering when reading the result: this measures CPU time
// spent issuing the pass, not GPU time executing it. D3D9 queues work, so a
// GPU-bound pass can look cheap here while the cost surfaces later as a block
// in whatever call fills the queue. A flat reading therefore rules the shadow
// pass out as a CPU cost, NOT as a GPU cost.
static LONG g_ftShadow[FT_CAPTURE_MAX];
// Further columns, all deltas over the same frame. Instant EIP sampling turned
// out to be the wrong instrument for "where did the extra 6ms go": the main
// thread makes ~1300 WaitForSingleObject calls per frame averaging 0.63us, so
// a stack sample lands in the wait a third of the time purely on volume, while
// the actual waits total well under 200us. Per-frame COUNTS correlated against
// the wave say what is different about the slow frames; a single instantaneous
// stack cannot.
static LONG g_ftAllocs[FT_CAPTURE_MAX];
static LONG g_ftReads[FT_CAPTURE_MAX];
static LONG g_ftWfso[FT_CAPTURE_MAX];
// Duration of FUN_00ac3040 itself on the same frame. It is measured
// entry-to-return, while the frame time is measured entry-to-ENTRY, so the two
// are independent: if this column carries the 9-frame wave the cost is inside
// the call, if it is flat the cost is in the rest of the frame.
static LONG g_ftPacer[FT_CAPTURE_MAX];
static volatile LONG g_ftCount = 0;
static volatile LONG g_ftDumped = 0;
static volatile LONG g_logFrameTimes;   // tentative def; toggle table is earlier
static volatile LONG g_shadowPassUsec = 0;
// Previous-frame bases for the delta columns above (main thread only).
static LONG g_ftPrevAllocs = 0, g_ftPrevReads = 0, g_ftPrevWfso = 0;

// Defined in 09_game_menu.c (later in the single translation unit): the
// once-a-second re-check of things that are not settled at menu-build time.
static void GameMenuDeferredPoll(LONG frame);

__declspec(noinline) int __cdecl OnEnter_ac3040_C(void *r)
{
    if (!g_mainThreadId) g_mainThreadId = (LONG)GetCurrentThreadId();
    // Frame boundary: clear the draw-pass tag so targets bound before
    // DRAW_SHADOW are labelled (none) rather than carrying last frame's pass.
    g_curPass = PASS_NONE;
    // ...and arm the MSAA depth clear, so it happens exactly once per frame
    // regardless of how many times the scene target is bound.
    g_msNeedDepthClear = 1;
    // Foreign-write suppression ends with the frame that was written into;
    // the new frame's full overdraw re-founds the MS accumulation.
    g_msSuppressFrame = 0;
    g_msFrameSeq++;
    // One-shot proof of life. The deferred poll below produced NOTHING in its
    // first flight - no probe lines, no runtime-poll forces - and every
    // explanation for that reduces to one question: does this function run at
    // all? Reasoning could not settle it (the hook is installed
    // unconditionally, but its result was logged on an untagged line the
    // release filter dropped), so it is now stated outright, once, in the log.
    {
        static volatile LONG tickAnnounced = 0;
        if (InterlockedCompareExchange(&tickAnnounced, 1, 0) == 0)
            LogLine("[boot] frame tick alive (engine pacer link is running)");
    }
    // Deferred menu/settings poll (defined in 09_game_menu.c, which is later in
    // the TU). THIS is the main thread by definition - g_mainThreadId is
    // established right above - which is why the poll lives here and not on the
    // monitor thread: it calls the game's own setting handlers.
    GameMenuDeferredPoll(g_msFrameSeq);
    unsigned __int64 now = __rdtsc();
    if (g_lastFrameTsc != 0 && g_cyclesPerUsec > 0.0) {
        LONG usec = (LONG)((double)(now - g_lastFrameTsc) / g_cyclesPerUsec);
        // Main thread only, once per frame - no contention, plain compares are
        // fine; the monitor thread is the only other reader/writer and it only
        // ever resets.
        if (usec > g_maxFrameUsec) g_maxFrameUsec = usec;
        g_frameSumUsec += usec;
        g_frameCount++;
        // Full frame-time histogram, 0.5ms buckets. Detection is deliberately
        // separated from attribution here: this costs one array increment on
        // the main thread and catches EVERYTHING including sub-threshold
        // jitter, whereas the stutter watchdog (which suspends the main
        // thread and walks its stack) is far too invasive to fire on every
        // frame. Percentiles are computed from this offline, so no ordering
        // or storage of individual samples is needed.
        {
            LONG b = usec / 500;
            if (b < 0) b = 0;
            if (b >= FRAME_HIST_BUCKETS) b = FRAME_HIST_BUCKETS - 1;
            g_frameHist[b]++;
            LONG rp = g_frameRingPos;
            g_frameRing[rp % FRAME_RING] = usec;
            g_frameRingPos = rp + 1;
            // One predictable branch and a store on the frame path; the
            // formatting all happens on the monitor thread.
            if (g_logFrameTimes) {
                LONG n = g_ftCount;
                LONG aNow = g_namedHeapCount, rNow = g_readFileCount, wNow = g_wfsoCallCount;
                if (n < FT_CAPTURE_MAX) {
                    g_ftBuf[n]    = usec;
                    g_ftShadow[n] = g_shadowPassUsec;
                    g_ftAllocs[n] = aNow - g_ftPrevAllocs;
                    g_ftReads[n]  = rNow - g_ftPrevReads;
                    g_ftWfso[n]   = wNow - g_ftPrevWfso;
                    // Duration of the PREVIOUS frame's FUN_00ac3040 call - we
                    // are at its entry, so its own return has not happened yet.
                    g_ftPacer[n]  = g_lastAc3040Usec;
                    g_ftCount = n + 1;
                }
                g_ftPrevAllocs = aNow; g_ftPrevReads = rNow; g_ftPrevWfso = wNow;
            }
        }
    }
    g_lastFrameTsc = now;

    // Frame boundary: rebase the per-frame deltas and republish the frame
    // start, which is what the watchdog polls against.
    g_frameBaseReadCount = g_readFileCount;
    g_frameBaseReadBytes = g_readFileBytes;
    g_frameBasePaceUsec = g_readPaceDelayTotalUsec;
    g_frameBaseAllocs = g_namedHeapCount;
    // Issue a bounded batch of deferred GPU copies here: once per frame, on
    // the main thread, in the same command stream the engine itself uses.
    DrainStagedUploads();

    // Per-frame, not on the 500ms monitor tick. The engine recomputes the
    // cascade splits every frame, so writing twice a second produced exactly
    // twice-a-second blinking - our value for one frame, the engine's for the
    // rest. Writing on the engine's own frame boundary keeps it applied, and
    // confirms the fields are both correct and visibly effective.
    // Immediately before the split write, so the decision always uses THIS
    // frame's cinema state. On the 500ms monitor tick instead, every cutscene
    // boundary would carry up to half a second of wrong shadows.
    CutsceneDetectTick();
    ApplyCascadeSplitSource();
    ApplyShadowFilterRadius();

    g_frameStartUsec = NowUsec();
    InterlockedIncrement(&g_frameSeq);

    return OnEnterBookkeeping(r, FN_AC3040);
}

static DWORD WINAPI StutterWatchdogThread(LPVOID param)
{
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LONG lastSeq = -1;
    int samplesThisFrame = 0;
    for (;;) {
        // Adaptive poll. At the shipping threshold (1s = the menu's "Off")
        // this thread used to wake 500 times a second only to compare two
        // numbers - harmless on a desktop CPU, pure waste on a weak one, and
        // a 2026-08-13 A/B showed the ARMED watchdog itself is a measurable
        // stutter source near the frame cap (79 suspend+stackwalk captures
        // at thr=20ms vs 1 at 100ms; the same route felt visibly smoother).
        // Disarmed it now idles at 10Hz; arming it from the menu takes
        // effect within one long tick, which is nothing for a diagnostic.
        Sleep(g_stutterThresholdUsec >= 500000 ? 100 : 2);
        if (!g_mainThreadHandle || g_cyclesPerUsec <= 0.0) continue;

        LONG seq = g_frameSeq;
        if (seq != lastSeq) { lastSeq = seq; samplesThisFrame = 0; }
        if (samplesThisFrame >= STUTTER_MAX_SAMPLES_PER_FRAME) continue;

        LONG elapsed = NowUsec() - g_frameStartUsec;
        if (elapsed < g_stutterThresholdUsec + samplesThisFrame * 20000) continue;

        // Rate limit: bound how often the main thread gets suspended, so a
        // stretch where every frame crosses the threshold cannot turn this
        // diagnostic into a stutter source of its own.
        {
            static LONG windowStartUsec = 0;
            static LONG capturesThisSec = 0;
            LONG nowUs = NowUsec();
            if (windowStartUsec == 0 || (nowUs - windowStartUsec) >= 1000000) {
                windowStartUsec = nowUs;
                capturesThisSec = 0;
            }
            if (capturesThisSec >= STUTTER_MAX_CAPTURES_PER_SEC) {
                // Distinguishes "no slow frames happened" from "slow frames
                // happened but attribution was rate-limited" - the two look
                // identical in the [stutter] record count alone. Read
                // alongside [frametime]'s over-threshold count: if this stays
                // near zero, the watchdog's records/stall totals are a
                // complete account of what the histogram detected. If it
                // climbs, they are an undercount - the histogram still caught
                // every frame, but some got no stack trace and are not in any
                // records=N/stall=Xs total quoted from watchdog data.
                InterlockedIncrement(&g_stutterRateLimited);
                continue;
            }
            capturesThisSec++;
        }

        StutterRec rec;
        memset(&rec, 0, sizeof(rec));
        rec.elapsedUsec = elapsed;

        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;

        if (SuspendThread(g_mainThreadHandle) == (DWORD)-1) continue;
        // Re-check the sequence: if the frame ended between the elapsed test
        // and the suspend, this sample belongs to a frame that already
        // finished and would be misleading.
        int stale = (g_frameSeq != seq);
        if (!stale && GetThreadContext(g_mainThreadHandle, &ctx)) {
            rec.eip = ctx.Eip;
            rec.esp = ctx.Esp;
            __try {
                DWORD *ebp = (DWORD *)ctx.Ebp;
                for (int i = 0; i < STUTTER_STACK_DEPTH; i++) {
                    if (!ebp) break;
                    DWORD ret = ebp[1];
                    if (!ret) break;
                    rec.stack[rec.stackCount++] = ret;
                    DWORD *next = (DWORD *)ebp[0];
                    // Frames must walk monotonically toward higher addresses;
                    // anything else means FPO or a corrupt chain - stop.
                    if (next <= ebp) break;
                    ebp = next;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // partial stack is still useful
            }

            // Raw stack scan. The EBP chain dies immediately inside FPO
            // system/driver code, which is exactly where the main thread was
            // found 96% of the time - so also sweep the raw stack for values
            // that point into the game module. Those are the return addresses
            // that say which game code entered the blocking call.
            __try {
                DWORD *sp = (DWORD *)ctx.Esp;
                for (int i = 0; i < 1024 && rec.scanCount < STUTTER_SCAN_DEPTH; i++) {
                    DWORD v = sp[i];
                    if (v >= g_mainModBase && v < g_mainModBase + g_mainModSize) {
                        rec.scan[rec.scanCount++] = v;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ResumeThread(g_mainThreadHandle);
        if (stale || rec.eip == 0) continue;

        samplesThisFrame++;

        rec.readCountInFrame = g_readFileCount - g_frameBaseReadCount;
        rec.readKbInFrame = (LONG)((g_readFileBytes - g_frameBaseReadBytes) / 1024);
        rec.paceUsecInFrame = g_readPaceDelayTotalUsec - g_frameBasePaceUsec;
        rec.allocsInFrame = g_namedHeapCount - g_frameBaseAllocs;

        LONG csStart = g_mainCsWaitStartUsec;
        if (csStart) {
            rec.csWaitUsec = NowUsec() - csStart;
            rec.csPtr = (DWORD)g_mainCsPtr;
            rec.csOwnerTid = (DWORD)g_mainCsOwner;
        }
        LONG wStart = g_mainWfsoStartUsec;
        if (wStart) {
            rec.wfsoWaitUsec = NowUsec() - wStart;
            rec.wfsoHandle = (DWORD)g_mainWfsoHandle;
        }

        LONG slot = InterlockedIncrement(&g_stutterWrite) - 1;
        StutterRec *dst = &g_stutterRecs[slot % MAX_STUTTER_RECS];
        if (dst->ready) {
            InterlockedIncrement(&g_stutterMissed);
            continue;
        }
        rec.ready = 0;
        memcpy(dst, &rec, sizeof(rec));
        dst->ready = 1;
    }
}
__declspec(noinline) void *__cdecl OnReturn_ac3040_C(void) { return OnReturnBookkeeping(FN_AC3040); }
__declspec(naked) void OnReturn_ac3040(void)
{
    __asm {
        pushfd
        call OnReturn_ac3040_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_a01a00_C(void *r) { return OnEnterBookkeeping(r, FN_A01A00); }
__declspec(noinline) void *__cdecl OnReturn_a01a00_C(void) { return OnReturnBookkeeping(FN_A01A00); }
__declspec(naked) void OnReturn_a01a00(void)
{
    __asm {
        pushfd
        call OnReturn_a01a00_C
        popfd
        jmp eax
    }
}

__declspec(noinline) int __cdecl OnEnter_a015b0_C(void *r) { return OnEnterBookkeeping(r, FN_A015B0); }
__declspec(noinline) void *__cdecl OnReturn_a015b0_C(void) { return OnReturnBookkeeping(FN_A015B0); }
__declspec(naked) void OnReturn_a015b0(void)
{
    __asm {
        pushfd
        call OnReturn_a015b0_C
        popfd
        jmp eax
    }
}

// Its only static reference is a data slot (an indirect call target), so the
// caller can't be traced statically - trueRetAddr, already captured by the
// existing hijack machinery for every hooked function, IS the answer;
// logging it (deduplicated by module-relative address, first few distinct
// callers only) turns this into an empirical answer instead of a guess.
#define MAX_A41570_CALLERS 8
static DWORD g_a41570Callers[MAX_A41570_CALLERS];
static volatile LONG g_a41570CallerCount = 0;

__declspec(noinline) int __cdecl OnEnter_a41570_C(void *r)
{
    DWORD rva = (DWORD)r - (DWORD)GetModuleHandleA(NULL) + 0x00400000;
    LONG n = g_a41570CallerCount;
    int known = 0;
    for (LONG i = 0; i < n; i++) {
        if (g_a41570Callers[i] == rva) { known = 1; break; }
    }
    if (!known && n < MAX_A41570_CALLERS) {
        LONG slot = InterlockedIncrement(&g_a41570CallerCount) - 1;
        if (slot < MAX_A41570_CALLERS) {
            g_a41570Callers[slot] = rva;
            char line[96];
            sprintf(line, "[a41570] new caller: %08lX", (unsigned long)rva);
            LogLine(line);
        }
    }
    return OnEnterBookkeeping(r, FN_A41570);
}
__declspec(noinline) void *__cdecl OnReturn_a41570_C(void) { return OnReturnBookkeeping(FN_A41570); }
__declspec(naked) void OnReturn_a41570(void)
{
    __asm {
        pushfd
        call OnReturn_a41570_C
        popfd
        jmp eax
    }
}

// Fix attempt 21: FUN_00aa7850 (shader-compile queue drain) already has a
// time-budget check between items (FUN_00aa7420: accumulated <= 80% of
// *(int*)(this+0x5c), both in microseconds - confirmed via FUN_0049a310,
// which is QueryPerformanceCounter converted to microseconds). Content-
// verified live captures (v20) confirmed genuine shader recompiles bursting
// to hundreds of items within a ~5-10s window after leaving and returning to
// an area, costing up to 22ms in a single call. The check never interrupts
// an item already in progress and always lets at least one item through per
// queue per call (the work happens BEFORE the budget check, not after), so
// temporarily lowering this value cannot cause starvation - only spreads the
// same backlog over more frames instead of bursting it into one, the same
// strategy already proven for the ReadFile limiter, this time aimed at the
// actual mechanism instead of a symptom.
//
// thisPtr (ECX) is read directly from the register, still valid at the
// point of the extra push since nothing between function entry and here
// writes to the ECX register itself (only pushes its value for
// preservation). The original value is restored immediately in
// OnReturn_aa7850_C so nothing else that reads this field between frames
// ever observes it altered - only this one drain call, this one frame.
//
// The budget modification is gated on OnEnterBookkeeping's hijack having
// succeeded (guaranteed return-hijack -> OnReturn will fire to restore it);
// if the hijack failed (per-thread stack full - not expected for a main-
// thread-only, non-reentrant, once-per-frame call, but the path exists),
// the budget is left untouched rather than risk a save with no matching
// restore.
// [STALE ATTRIBUTION - see PROGRESS.md "CORRECTION: ShaderThrottle is NOT
// harmful". The v23 black-geometry regression described below persisted
// with this throttle fully OFF; the real cause was the DISCARD fix's
// pointer-only key. The throttle was exonerated and its default restored
// to ON. This block already misled twice - once when a stale summary line
// was quoted as status, and again 2026-08-15 when it was read as the
// explanation for a transient blackout. The mechanism reasoning below
// (pending shader -> black object) is kept as the v23-era theory, not as
// established fact.]
// DISABLED (v23 result): capping this budget causes severe visual
// corruption - distant geometry and even the player character render
// black/transparent, worse at lower resolution, worse the longer the
// throttle stays engaged. The safety analysis behind this fix only checked
// "does the queue eventually finish" (yes - no starvation) but missed the
// actual constraint: an object with a not-yet-compiled shader apparently
// renders as black/nothing rather than waiting or falling back, so what
// matters is how LONG a shader stays pending, not just total CPU spent on
// compiling. The real 20ms/frame default was almost certainly the original
// developers' own deliberate tradeoff - accept a CPU spike to keep that
// window imperceptibly short - not an oversight to correct. Throttling it
// harder optimized the wrong variable (per-frame CPU cost) at the direct
// expense of the one that actually matters visually (time-to-ready). Left
// disabled by default (g_shaderThrottleEnabled starts at 0) pending a
// fundamentally different approach that doesn't extend how long a shader
// stays uncompiled - hot-toggleable via F10 for isolation testing without
// a rebuild, but default OFF until a real fix exists.
#define SHADER_QUEUE_BUDGET_OFFSET 0x5c
#define SHADER_QUEUE_REDUCED_BUDGET_USEC 500
static LONG g_shaderBudgetOriginal = 0;
static DWORD g_shaderBudgetThisPtr = 0;
static volatile LONG g_shaderBudgetLoggedCount = 0;
// v21 result: the first-20-ever log only ever caught quiet, early-session
// calls (all exactly 2000 - coincidentally already at the reduction target,
// so the reduction branch never fired for any of them), giving no real
// visibility into whether the throttle engages during an actual burst -
// despite that, the shader queue's own worst-case per-call cost still
// dropped 2-3x (22ms->11.5ms) session over session, implying the engagement
// IS happening on later, unlogged calls. Replacing the "first N ever" cap
// with running min/max (cheap, every call) plus individual logs specifically
// when a reduction actually engages, capped separately - directly answers
// whether/how often the throttle does real work instead of inferring it.
static volatile LONG g_shaderBudgetMinSeen = 0x7FFFFFFF;
static volatile LONG g_shaderBudgetMaxSeen = 0;
static volatile LONG g_shaderBudgetEngagedCount = 0;
static volatile LONG g_shaderBudgetEngagedLogged = 0;

__declspec(noinline) int __cdecl OnEnter_aa7850_C(void *r, DWORD thisPtr)
{
    int hijacked = OnEnterBookkeeping(r, FN_AA7850);
    if (hijacked) {
        __try {
            int *budgetPtr = (int *)(thisPtr + SHADER_QUEUE_BUDGET_OFFSET);
            int original = *budgetPtr;
            if (original < g_shaderBudgetMinSeen) g_shaderBudgetMinSeen = original;
            if (original > g_shaderBudgetMaxSeen) g_shaderBudgetMaxSeen = original;
            if (g_shaderThrottleEnabled && original > SHADER_QUEUE_REDUCED_BUDGET_USEC) {
                InterlockedIncrement(&g_shaderBudgetEngagedCount);
                if (g_shaderBudgetEngagedLogged < 50) {
                    InterlockedIncrement(&g_shaderBudgetEngagedLogged);
                    char line[128];
                    sprintf(line, "[shaderbudget] ENGAGED original_usec=%d -> capped to %d",
                            original, SHADER_QUEUE_REDUCED_BUDGET_USEC);
                    LogLine(line);
                }
                g_shaderBudgetOriginal = original;
                g_shaderBudgetThisPtr = thisPtr;
                *budgetPtr = SHADER_QUEUE_REDUCED_BUDGET_USEC;
            } else {
                g_shaderBudgetThisPtr = 0; // disabled, or already <= target - nothing to restore
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_shaderBudgetThisPtr = 0;
        }
    }
    return hijacked;
}
__declspec(noinline) void *__cdecl OnReturn_aa7850_C(void)
{
    if (g_shaderBudgetThisPtr) {
        __try {
            *(int *)(g_shaderBudgetThisPtr + SHADER_QUEUE_BUDGET_OFFSET) = g_shaderBudgetOriginal;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_shaderBudgetThisPtr = 0;
    }
    return OnReturnBookkeeping(FN_AA7850);
}
__declspec(naked) void OnReturn_aa7850(void)
{
    __asm {
        pushfd
        call OnReturn_aa7850_C
        popfd
        jmp eax
    }
}

// Heap compactor (FN_B46C20). Plain timing, no argument tampering: the
// parameter its wrapper pushes ([this+0x4c]) is a block-list node pointer,
// not a numeric budget, so there is nothing safe to cap here - and the
// shader-queue budget experiment (v23, block above) already demonstrated
// what "throttle first, understand the constraint later" costs. Measure
// per-call duration and per-window call count first; whether the right
// lever is slicing, deferral, or leaving it alone comes out of that data.
// Returns -1 to SKIP the pass entirely (detour pops and `ret 4`s without
// ever entering the compactor - callee-clean __thiscall with one stack arg,
// verified against wrapper A's call site: no add esp after the CALL), 1 to
// run hijacked (timed), 0 to run untimed (per-thread stack full).
__declspec(noinline) int __cdecl OnEnter_b46c20_C(void *r)
{
    if (g_compactorDeferEnabled) {
        LONG seq = g_frameSeq;
        if (seq != g_compactorSeq) {
            g_compactorSeq = seq;
            g_compactorFrameUs = 0;
            if (g_compactorCooldown > 0) InterlockedDecrement(&g_compactorCooldown);
        }
        if (g_compactorCooldown > 0 || g_compactorFrameUs >= g_compactorBudgetUs) {
            InterlockedIncrement(&g_compactorSkips);
            return -1;
        }
    }
    return OnEnterBookkeeping(r, FN_B46C20);
}
__declspec(noinline) void *__cdecl OnReturn_b46c20_C(void) { return OnReturnBookkeeping(FN_B46C20); }
__declspec(naked) void OnReturn_b46c20(void)
{
    __asm {
        pushfd
        call OnReturn_b46c20_C
        popfd
        jmp eax
    }
}

// Entry detours. Preserve ECX/EDX around our own C call regardless of each
// target's exact calling convention - costs nothing, removes any risk of
// corrupting a live fastcall/thiscall argument register. Return-address
// hijack is conditional on OnEnter's result (see file header/PROGRESS.md
// for the full safety reasoning carried over from v2).
__declspec(naked) void Detour_aacf10(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_aacf10_C
        add esp, 4
        test eax, eax
        jz skip_aacf10
        mov dword ptr [esp + 8], offset OnReturn_aacf10
    skip_aacf10:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_aacf10]
    }
}
__declspec(naked) void Detour_a2ada0(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a2ada0_C
        add esp, 4
        test eax, eax
        jz skip_a2ada0
        mov dword ptr [esp + 8], offset OnReturn_a2ada0
    skip_a2ada0:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a2ada0]
    }
}
__declspec(naked) void Detour_d19a00(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_d19a00_C
        add esp, 4
        test eax, eax
        jz skip_d19a00
        mov dword ptr [esp + 8], offset OnReturn_d19a00
    skip_d19a00:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_d19a00]
    }
}

// param_1[0xc]/[0xd] in the decompile are float-typed slots that actually
// hold the raw uint32 low/high halves of a 64-bit QueryPerformanceCounter
// deadline (confirmed: the decompile assigns them via
// `(float)local_1c.s.LowPart` - a bit-pattern reinterpret, not a real float
// conversion). In memory that is bytes +0x30/+0x34 on the pacer object
// FUN_00ac3040 receives in ECX. Read/write as raw DWORDs at those offsets.
//
// Applied BEFORE FUN_00ac3040 runs each frame (called from Detour_ac3040,
// which already has ECX saved on the stack for the existing return-address
// hijack), so by the time the engine's own logic reads the deadline it is
// already clamped and never takes its own catch-up branch.
static void __cdecl ClampAc3040Deadline(void *pacer)
{
    if (!g_clampDeadlineEnabled || !pacer || g_mainModBase == 0) return;

    volatile DWORD *lowPart  = (volatile DWORD *)((char *)pacer + 0x30);
    volatile DWORD *highPart = (volatile DWORD *)((char *)pacer + 0x34);

    LARGE_INTEGER now, deadline, interval;
    if (!QueryPerformanceCounter(&now)) return;
    deadline.LowPart = *lowPart;
    deadline.HighPart = (LONG)*highPart;

    volatile unsigned int *ticks =
        (volatile unsigned int *)(g_mainModBase + FRAME_TARGET_TICKS_RVA);
    interval.LowPart = ticks[0];
    interval.HighPart = (LONG)ticks[1];
    if (interval.QuadPart <= 0) return;   // not initialised yet

    // Only clamp a deadline that has fallen more than one interval BEHIND
    // now. On-schedule or slightly-ahead deadlines are left untouched -
    // this must never make a normal frame wait LONGER than it already would.
    if (deadline.QuadPart < now.QuadPart - interval.QuadPart) {
        LARGE_INTEGER clamped;
        clamped.QuadPart = now.QuadPart + interval.QuadPart;
        *lowPart = clamped.LowPart;
        *highPart = (DWORD)clamped.HighPart;
        InterlockedIncrement(&g_deadlineClampCount);
    }
}

__declspec(naked) void Detour_ac3040(void)
{
    __asm {
        push ecx
        push edx
        // ECX (the frame-pacer "this" pointer FUN_00ac3040 expects via
        // __fastcall) sits at [esp+4] right here - pass it to the clamp
        // fix before doing anything else. Stack is restored to identical
        // layout afterward (verified: cdecl callee, caller cleans up),
        // so the original code below is untouched by this insertion.
        mov eax, [esp + 4]
        push eax
        call ClampAc3040Deadline
        add esp, 4
        mov eax, [esp + 8]
        push eax
        call OnEnter_ac3040_C
        add esp, 4
        test eax, eax
        jz skip_ac3040
        mov dword ptr [esp + 8], offset OnReturn_ac3040
    skip_ac3040:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_ac3040]
    }
}
__declspec(naked) void Detour_a01a00(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a01a00_C
        add esp, 4
        test eax, eax
        jz skip_a01a00
        mov dword ptr [esp + 8], offset OnReturn_a01a00
    skip_a01a00:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a01a00]
    }
}
__declspec(naked) void Detour_a015b0(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a015b0_C
        add esp, 4
        test eax, eax
        jz skip_a015b0
        mov dword ptr [esp + 8], offset OnReturn_a015b0
    skip_a015b0:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a015b0]
    }
}
__declspec(naked) void Detour_a41570(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_a41570_C
        add esp, 4
        test eax, eax
        jz skip_a41570
        mov dword ptr [esp + 8], offset OnReturn_a41570
    skip_a41570:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a41570]
    }
}
// Passes ECX (thisPtr) as a second argument, unlike every other Detour_xxx
// in this file - ECX is still valid to read at the "push ecx" (3rd push)
// point since nothing between function entry and here writes the ECX
// REGISTER itself (only pushes/pops its value for preservation).
__declspec(naked) void Detour_aa7850(void)
{
    __asm {
        push ecx
        push edx
        push ecx                ; arg2: thisPtr (register still holds it)
        mov eax, [esp + 12]     ; original [esp+8] = retaddr, now at +12 after the extra push
        push eax                ; arg1: trueRetAddr
        call OnEnter_aa7850_C
        add esp, 8
        test eax, eax
        jz skip_aa7850
        mov dword ptr [esp + 8], offset OnReturn_aa7850
    skip_aa7850:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_aa7850]
    }
}

__declspec(naked) void Detour_b46c20(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]
        push eax
        call OnEnter_b46c20_C
        add esp, 4
        cmp eax, -1
        je defer_b46c20
        test eax, eax
        jz skip_b46c20
        mov dword ptr [esp + 8], offset OnReturn_b46c20
    skip_b46c20:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_b46c20]
    defer_b46c20:
        ; skip the pass entirely: __thiscall, one stack arg, callee cleans -
        ; behave exactly like an immediate RET 4 from FUN_00b46c20.
        pop edx
        pop ecx
        ret 4
    }
}

static int InstallJmpHookRaw(HookedFunc *hf, void *detour, void **trampolineOut);
static int InstallJmpHook(HookedFunc *hf, void *detour, void **trampolineOut)
{
    int r = InstallJmpHookRaw(hf, detour, trampolineOut);
    HookRegNote(hf->name, r);            // status panel: "N installed / M failed"
    return r;
}
static int InstallJmpHookRaw(HookedFunc *hf, void *detour, void **trampolineOut)
{
    // 16, not 6: hideWindow needs a 7-byte patch (its prologue's last
    // instruction straddles the 6-byte boundary). Every existing caller uses
    // 5 or 6, so this only removes a latent overflow rather than changing
    // any current behaviour.
    unsigned char saved[16];
    if (hf->patchLen < 5 || hf->patchLen > (int)sizeof(saved)) return 0;
    memcpy(saved, hf->target, hf->patchLen);

    unsigned char *tramp = (unsigned char *)CodeAllocRW(128);
    if (!tramp) return 0;

    memcpy(tramp, saved, hf->patchLen);
    tramp[hf->patchLen] = 0xE9;
    *(int *)(tramp + hf->patchLen + 1) =
        (int)((unsigned char *)hf->target + hf->patchLen) - (int)(tramp + hf->patchLen + 5);
    if (!CodeSeal(tramp, 128)) return 0;
    *trampolineOut = tramp;

    DWORD oldProtect;
    if (!VirtualProtect(hf->target, hf->patchLen, CODE_PAGE_WRITABLE, &oldProtect)) return 0;

    unsigned char *t = (unsigned char *)hf->target;
    t[0] = 0xE9;
    *(int *)(t + 1) = (int)detour - (int)(t + 5);
    for (int i = 5; i < hf->patchLen; i++) t[i] = 0x90;

    VirtualProtect(hf->target, hf->patchLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), hf->target, hf->patchLen);
    return 1;
}

// Windowed (per-interval), not cumulative-since-start.
static LONG g_prevDurCount[NUM_FNS];
static LONGLONG g_prevDurSum[NUM_FNS];
static LONG g_prevWfsoCount = 0;
static LONGLONG g_prevWfsoSum = 0;
static LONG g_prevWfsoThreadCount[MAX_WFSO_THREADS];
static LONGLONG g_prevWfsoThreadSum[MAX_WFSO_THREADS];
static LONG g_prevDispatchCount = 0;
static LONG g_prevPrefetchedAllocCount = 0;
static LONGLONG g_prevPrefetchedByteCount = 0;
static LONG g_prevAllocThreadCount[MAX_ALLOC_THREADS][NUM_ALLOC_SRC];
static LONGLONG g_prevAllocThreadBytes[MAX_ALLOC_THREADS][NUM_ALLOC_SRC];
static LONG g_prevWarmEnqueued = 0, g_prevWarmDone = 0, g_prevWarmDropped = 0;
static LONGLONG g_prevWarmBytes = 0;

// ---- Cause 3 measurement: script/class-loader timing -----------------------
// The class loader parses script classes synchronously on the main thread on
// first encounter (PROGRESS.md "Cause 3"; re-confirmed live 2026-08-13 by a
// 41.3ms capture whose EBP chain matched the predicted stack frame-for-frame).
// The two hot loops are known from decompile - FUN_009ddfe0 rescans the whole
// obfuscated native-method table per method, XOR-decoding two strings per
// entry; FUN_009fd110 runs 8 substitution rounds per byte of class data - but
// their SPLIT is not, and the split decides whether the memoised binder alone
// is enough or the decrypt fast-path is needed too. So: wall-clock the
// outermost load (FUN_009dff70), the binder, and the decrypt caller
// (FUN_009fcfd0, per buffer not per block), and let one walking session
// answer it.
//
// All three take the standard paired hook (prologues verified in
// ghidra_output/loader_hook_safety.txt: no SEH, no inbound refs, boundaries
// at +6/+9/+5). Loads recurse into nested class dependencies, so a single
// saved-return slot would be clobbered by an inner call - the enter handler
// declines anything that is not the outermost main-thread call. Declined
// inner entries are still counted (g_ldrNested: closure size per window) and
// their time is inside the outermost measurement anyway. Off-main-thread
// entries are declined and counted rather than assumed impossible.
#if ENABLE_LOADER_DIAG
#define LDR_LOAD_RVA    (0x009dff70 - 0x00400000)
#define LDR_BIND_RVA    (0x009ddfe0 - 0x00400000)
#define LDR_DECRYPT_RVA (0x009fcfd0 - 0x00400000)
static void *g_tramp_ldrLoad = NULL, *g_tramp_ldrBind = NULL, *g_tramp_ldrDec = NULL;

// Window accumulators, drained by the monitor thread.
static volatile LONG g_ldrLoads = 0;       // outermost loads completed
static volatile LONG g_ldrNested = 0;      // nested (declined) entries = closure size
static volatile LONG g_ldrUsec = 0;        // wall time of outermost loads
static volatile LONG g_ldrWorstUsec = 0;   // worst single load
static volatile LONG g_ldrBindUsec = 0, g_ldrBindCalls = 0;
static volatile LONG g_ldrDecUsec = 0, g_ldrDecCalls = 0;
static volatile LONG g_ldrOffThread = 0;   // entries seen on non-main threads
static LONG g_ldrCumLoads = 0;             // session total (monitor thread only)

// Per-hook single-slot state (main thread only by construction).
static DWORD g_ldrLoadRet, g_ldrBindRet, g_ldrDecRet;
static LONG g_ldrLoadDepth = 0, g_ldrBindDepth = 0, g_ldrDecDepth = 0;
static unsigned __int64 g_ldrLoadT0, g_ldrBindT0, g_ldrDecT0;

// If an engine SEH unwind ever skips a return stub the depth latches at 1 and
// measurement stops for the session - a lost diagnostic, never a crash, and
// visible as a [loader] line that goes silent while stutters continue.
#define LDR_ENTER(depth, retSlot, t0, retAddr)                              \
    do {                                                                    \
        if ((LONG)GetCurrentThreadId() != g_mainThreadId) {                 \
            InterlockedIncrement(&g_ldrOffThread); return 0;                \
        }                                                                   \
        if ((depth) != 0) { InterlockedIncrement(&g_ldrNested); return 0; } \
        (depth) = 1; (retSlot) = (DWORD)(retAddr); (t0) = __rdtsc();        \
        return 1;                                                           \
    } while (0)

__declspec(noinline) int __cdecl OnEnter_ldrLoad_C(void *retAddr)
{
    LDR_ENTER(g_ldrLoadDepth, g_ldrLoadRet, g_ldrLoadT0, retAddr);
}
__declspec(noinline) void *__cdecl OnReturn_ldrLoad_C(void)
{
    void *ret = (void *)g_ldrLoadRet;
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - g_ldrLoadT0) / g_cyclesPerUsec);
        g_ldrUsec += us;
        if (us > g_ldrWorstUsec) g_ldrWorstUsec = us;
        InterlockedIncrement(&g_ldrLoads);
    }
    g_ldrLoadDepth = 0;
    return ret;
}

__declspec(noinline) int __cdecl OnEnter_ldrBind_C(void *retAddr)
{
    LDR_ENTER(g_ldrBindDepth, g_ldrBindRet, g_ldrBindT0, retAddr);
}
__declspec(noinline) void *__cdecl OnReturn_ldrBind_C(void)
{
    void *ret = (void *)g_ldrBindRet;
    if (g_cyclesPerUsec > 0.0) {
        g_ldrBindUsec += (LONG)((double)(__rdtsc() - g_ldrBindT0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_ldrBindCalls);
    }
    g_ldrBindDepth = 0;
    return ret;
}

__declspec(noinline) int __cdecl OnEnter_ldrDec_C(void *retAddr)
{
    LDR_ENTER(g_ldrDecDepth, g_ldrDecRet, g_ldrDecT0, retAddr);
}
__declspec(noinline) void *__cdecl OnReturn_ldrDec_C(void)
{
    void *ret = (void *)g_ldrDecRet;
    if (g_cyclesPerUsec > 0.0) {
        g_ldrDecUsec += (LONG)((double)(__rdtsc() - g_ldrDecT0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_ldrDecCalls);
    }
    g_ldrDecDepth = 0;
    return ret;
}

// The naked stubs mirror Detour_drawShadow / OnReturn_drawShadow exactly.
#define LDR_STUBS(name)                                                 \
    __declspec(naked) void OnReturn_##name(void)                        \
    {                                                                   \
        __asm { push eax }                                              \
        __asm { pushfd }                                                \
        __asm { call OnReturn_##name##_C }                              \
        __asm { mov ecx, eax }                                          \
        __asm { popfd }                                                 \
        __asm { pop eax }                                               \
        __asm { jmp ecx }                                               \
    }
LDR_STUBS(ldrLoad)
LDR_STUBS(ldrBind)
LDR_STUBS(ldrDec)

#define LDR_DETOUR(name, tramp)                                         \
    __declspec(naked) void Detour_##name(void)                          \
    {                                                                   \
        __asm { push ecx }                                              \
        __asm { push edx }                                              \
        __asm { mov eax, [esp + 8] }                                    \
        __asm { push eax }                                              \
        __asm { call OnEnter_##name##_C }                               \
        __asm { add esp, 4 }                                            \
        __asm { test eax, eax }                                         \
        __asm { jz decline }                                            \
        __asm { mov eax, offset OnReturn_##name }                       \
        __asm { mov dword ptr [esp + 8], eax }                          \
        __asm { decline: }                                              \
        __asm { pop edx }                                               \
        __asm { pop ecx }                                               \
        __asm { jmp dword ptr [tramp] }                                 \
    }
LDR_DETOUR(ldrLoad, g_tramp_ldrLoad)
LDR_DETOUR(ldrBind, g_tramp_ldrBind)
LDR_DETOUR(ldrDec,  g_tramp_ldrDec)
#endif  // ENABLE_LOADER_DIAG
