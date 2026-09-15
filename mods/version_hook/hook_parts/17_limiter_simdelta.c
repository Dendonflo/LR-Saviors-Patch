// ---- 59.94fps frame limiter unlock ---------------------------------------
//
// Located by sampling main-thread Sleep call sites: the dominant one is
// FUN_00ac3040+0x188, the engine's per-frame tick. Decompiled, it is a
// textbook hybrid frame limiter:
//
//   ticksPerFrame = QPC_freq * 1001 / 60000;      // 60000/1001 = 59.94 fps
//   deadline      = prevDeadline + n * ticksPerFrame;
//   while (now < deadline) {
//       Sleep(remaining < granularity ? 0 : 1);   // spin close, sleep 1ms far
//       QueryPerformanceCounter(&now);
//   }
//
// 1001/60000 is the NTSC rate, hardcoded - this is the 60fps lock, and the
// Sleep(0) branch is the ~12,000 calls/frame the probe measured.
//
// ticksPerFrame lives in a global (Ghidra 0x05115570/74, with an init flag at
// 0x05115578), recomputed only once and thereafter read-then-written-back
// unchanged every frame - so simply overwriting it sticks, and no code
// patching is needed.
//
// NOT set to zero: the surrounding arithmetic DIVIDES by this value
// (__aulldiv(..., fVar10, uVar8)), so zero would be an immediate
// divide-by-zero crash. A 1ms target is used instead - effectively unlocked
// while staying a legal divisor.
//
// The standing risk, unchanged from when this was first flagged: engines of
// this era frequently tie animation, physics and cutscene timing to an
// assumed fixed cadence. Removing the cap can desync those in ways far
// subtler than a dropped frame. Default OFF, one config key.
static volatile LONG g_frameUnlockApplied = 0;

static void ApplyFramerateUnlock(void)
{
    if (!g_unlockFramerateEnabled || !g_mainModBase) return;
    volatile unsigned int *initFlag =
        (volatile unsigned int *)(g_mainModBase + FRAME_TARGET_INIT_RVA);
    // Only once the engine has computed its own value - writing earlier would
    // just be overwritten by its one-time init.
    if ((*initFlag & 1) == 0) return;

    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return;

    LONG fpsX100 = g_targetFpsX100;
    unsigned __int64 target;
    const char *desc;
    if (fpsX100 > 0) {
        // ticksPerFrame = freq * 100 / fpsX100 (fpsX100 is fps*100, so this
        // is freq / (fpsX100/100) done in integer-safe order).
        target = ((unsigned __int64)f.QuadPart * 100) / (unsigned __int64)fpsX100;
        desc = "locked";
    } else {
        target = (unsigned __int64)f.QuadPart / 1000;   // 1ms - effectively unlocked
        desc = "unlocked (1ms target)";
    }
    if (target == 0) target = 1;

    volatile unsigned int *ticks =
        (volatile unsigned int *)(g_mainModBase + FRAME_TARGET_TICKS_RVA);
    if (ticks[0] == (unsigned int)target && ticks[1] == (unsigned int)(target >> 32)) return;
    ticks[0] = (unsigned int)target;
    ticks[1] = (unsigned int)(target >> 32);
    if (InterlockedCompareExchange(&g_frameUnlockApplied, 1, 0) == 0 || fpsX100 > 0) {
        char line[192];
        sprintf(line, "[probe] framerate target applied: ticksPerFrame -> %llu (%s, was 59.94fps)",
                (unsigned long long)target, desc);
        LogLine(line);
    }
}

// ---- Draw-pass render-target inventory ------------------------------------
//
// FrameworkDrawManager registers the frame graph by name; its constructor
// (FUN_00ac72d0) pairs each name with a handler:
//
//   DRAW_SHADOW                   -> FUN_00ac6040    (shadow MAP render)
//   DRAW_MULTI_SAMPLE_SCHEDULE    -> FUN_00ac5ee0
//   DRAW_MULTI_SAMPLE_PROPAGATION -> FUN_00ac5f50
//   DRAW_MULTI_SAMPLE_DEPTH       -> FUN_00ac6890
//   DRAW_MULTI_SAMPLE_SHADOW      -> FUN_00ac6b00    (screen-space shadows)
//   DRAW_MULTI_SAMPLE             -> FUN_00ac6e50
//   DRAW_FILTER                   -> FUN_00ac7030
//
// NOTE "MULTI_SAMPLE" here is NOT scene MSAA. SCHEDULE and PROPAGATION open
// with the same shadow-enable gate as DRAW_SHADOW
// ([DAT_0511558c + 0x2c] && +0x2d), and FUN_00ac6b00 reads the cascade split
// values at DAT_05107a00 +0x42c/+0x430. The whole stage is shadow work.
//
// Tagging the current pass and logging the render target bound during it
// answers two things at once:
//   - what RESOLUTION each stage runs at (the "shadows are half res" claim -
//     the inventory has 1920x1080 targets at 3840x2160 output, but half res is
//     equally standard for SSAO/volumetrics, so dimensions alone cannot say)
//   - how many simultaneous targets each stage binds, which is the actual
//     MRT/G-buffer evidence rather than one global max
//
// Limitation worth knowing when reading the output: the tag is set on pass
// ENTRY and never cleared, so render targets bound AFTER DRAW_FILTER (post
// effect, composite, back buffer) are attributed to DRAW_FILTER. The frame
// hook resets it to (none) each frame, so anything before DRAW_SHADOW is
// labelled (none) rather than mislabelled.
// (enum, name table, g_curPass, g_passMaxRtIndex and the seen table are
// declared near the top of this file - the frame hook and
// HookedSetRenderTarget both need them and sit earlier.)
static void *g_trampoline_msSchedule = NULL;
static void *g_trampoline_msPropagation = NULL;
static void *g_trampoline_msDepth = NULL;
static void *g_trampoline_msShadow = NULL;
static void *g_trampoline_ms = NULL;
static void *g_trampoline_filter = NULL;
static void *g_trampoline_menu = NULL;
static void *g_trampoline_backbuf = NULL;

// One tiny naked detour per pass: stamp the tag, jump to the trampoline. No
// return hijack needed - the tag only has to be correct while the pass runs,
// and the next pass overwrites it. Written out longhand because this file
// already records that MSVC's inline assembler does not survive macro
// expansion (C2400).
__declspec(naked) void Detour_msSchedule(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_SCHEDULE
          jmp dword ptr [g_trampoline_msSchedule] } }
__declspec(naked) void Detour_msPropagation(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_PROPAGATION
          jmp dword ptr [g_trampoline_msPropagation] } }
__declspec(naked) void Detour_msDepth(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_DEPTH
          jmp dword ptr [g_trampoline_msDepth] } }
__declspec(naked) void Detour_msShadow(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS_SHADOW
          jmp dword ptr [g_trampoline_msShadow] } }
__declspec(naked) void Detour_ms(void)
{ __asm { mov dword ptr [g_curPass], PASS_MS
          jmp dword ptr [g_trampoline_ms] } }
// The filter (post) pass entry doubles as the SSAA in-place resolve point:
// scene complete, post has not read it yet. PUSHAD around the C call out of
// caution; at a function entry the caller-saved registers are dead by ABI,
// but this costs nothing on a once-per-frame path.
__declspec(naked) void Detour_filter(void)
{ __asm { mov dword ptr [g_curPass], PASS_FILTER
          pushad
          call SsaaInPlaceResolve_C
          popad
          jmp dword ptr [g_trampoline_filter] } }
__declspec(naked) void Detour_menu(void)
{ __asm { mov dword ptr [g_curPass], PASS_MENU
          jmp dword ptr [g_trampoline_menu] } }
__declspec(naked) void Detour_backbuf(void)
{ __asm { mov dword ptr [g_curPass], PASS_BACKBUF
          jmp dword ptr [g_trampoline_backbuf] } }

// ---- Per-frame GPU fence skip (the frametime square wave) -----------------
//
// FUN_00a96090, called every frame from FUN_00ac3040, is an IDirect3DQuery9
// fence that busy-waits for the GPU to drain:
//
//     query->Issue(D3DISSUE_END);                      // vtable +0x18
//     hr = query->GetData(NULL, 0, D3DGETDATA_FLUSH);  // vtable +0x1c
//     while (hr != S_OK && hr != D3DERR_DEVICELOST && hr != D3DERR_INVALIDCALL) {
//         Sleep(0);
//         hr = query->GetData(NULL, 0, D3DGETDATA_FLUSH);
//     }
//
// (0x88760868 = D3DERR_DEVICELOST, 0x8876086C = D3DERR_INVALIDCALL; GetData
// returns S_FALSE while the GPU has not reached the query.)
//
// So the CPU is never allowed to run ahead of the GPU - a full pipeline flush
// every frame, where D3D9 would normally allow 1-3 frames of buffering. Per-
// frame capture pinned the entire frametime square wave to the enclosing call:
// r=+0.993 against frame time, 1148us on fast frames vs 7480us on slow, and
// that 6332us swing accounts for the whole 6235us frametime gap. Every other
// counter measured (allocations, file reads, WaitForSingleObject calls, shadow
// pass time) was flat - because a spin loop is not work, and Sleep(0) is not a
// wait object.
//
// Skipping it lets the CPU pipeline ahead. RISKS, and why this is default OFF:
//   - the fence may exist so the engine can safely touch resources the GPU is
//     still reading; removing it could corrupt or tear rendering
//   - it may be load-bearing for frame-limiter timing accuracy
//   - Issue() is skipped too, so anything else expecting that query to have
//     been issued sees it un-issued
// Expect more input latency if it works, since latency is exactly what a
// per-frame flush trades away.
#define GPU_FENCE_RVA (0x00a96090 - 0x00400000)
static void *g_trampoline_gpuFence = NULL;
// g_gpuFenceSkipped lives in 03_render_state.c (status panel reads it).

__declspec(naked) void Detour_gpuFence(void)
{
    __asm {
        cmp dword ptr [g_gpuSyncSkip], 0
        jz  run_gpu_fence
        lock inc dword ptr [g_gpuFenceSkipped]
        ret                                    // skip the fence entirely
    run_gpu_fence:
        jmp dword ptr [g_trampoline_gpuFence]
    }
}

// ---- DRAW_SHADOW pass timer (frametime square-wave diagnostic) ------------
// FUN_00ac6040 is the DRAW_SHADOW pass handler registered by
// FrameworkDrawManager; it wraps FUN_00a32a00, the shadow render proper (see
// FEATURES.md, "Decomp round 2"). Hooked here rather than FUN_00a32a00 itself
// because that one opens with 53 8B DC 83 EC 08 83 E4 F0 - it realigns the
// stack with AND ESP,-0x10 and addresses through EBX, so a return-address
// hijack there would have to find the return slot after a realignment. The
// handler has an ordinary 55 8B EC 51 56 prologue: a clean 5-byte patch (JMP
// rel32 is exactly 5, so no NOP padding) and the return address sits at [esp]
// on entry, the same shape every other paired hook here relies on.
#define DRAW_SHADOW_RVA (0x00ac6040 - 0x00400000)
static void *g_trampoline_drawShadow = NULL;
static DWORD g_dsTid = 0;
static DWORD g_dsRet = 0;
static unsigned __int64 g_dsEnter = 0;

__declspec(noinline) int __cdecl OnEnter_drawShadow_C(void *retAddr)
{
    g_curPass = PASS_SHADOW;             // pass tag for the RT inventory
    if (!g_logFrameTimes) return 0;      // only costs anything while capturing
    g_dsTid = GetCurrentThreadId();
    g_dsRet = (DWORD)retAddr;
    g_dsEnter = __rdtsc();
    return 1;
}

__declspec(noinline) void *__cdecl OnReturn_drawShadow_C(void)
{
    void *ret = (void *)g_dsRet;
    if (g_dsTid == GetCurrentThreadId() && g_dsEnter && g_cyclesPerUsec > 0.0)
        g_shadowPassUsec = (LONG)((double)(__rdtsc() - g_dsEnter) / g_cyclesPerUsec);
    return ret;
}

__declspec(naked) void OnReturn_drawShadow(void)
{
    __asm {
        push eax
        pushfd
        call OnReturn_drawShadow_C
        mov  ecx, eax
        popfd
        pop  eax
        jmp  ecx
    }
}

__declspec(naked) void Detour_drawShadow(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 8]          // return address -> only param
        push eax
        call OnEnter_drawShadow_C
        add esp, 4
        test eax, eax
        jz skip_drawShadow
        mov dword ptr [esp + 8], offset OnReturn_drawShadow
    skip_drawShadow:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_drawShadow]
    }
}

// ---- Frame-limiter sleep granularity: a monotonic high-water mark ---------
//
// Explains an observation that looks impossible at first: locking to 100fps
// yields ~70fps with erratic frametimes, while leaving it unlocked yields
// ~110fps. A cap that makes the game SLOWER than no cap.
//
// The limiter's inner loop (FUN_00ac3040, decompiled in
// ghidra_output/frame_delta.txt) decides how to wait like this:
//
//     remaining = deadline - now;
//     if (remaining < [0x05115568])  Sleep(0);     // busy-spin
//     else                           Sleep(1);     // real sleep
//     ...
//     elapsed = now_after - now_before;
//     [0x05115568] = max([0x05115568], elapsed);   // <-- RUNNING MAXIMUM
//
// [0x05115568/6c] is the engine's estimate of how coarse Sleep(1) is, and it
// is a HIGH-WATER MARK: only ever raised, never reset and never decayed. A
// single bad Sleep - a loading screen, an alt-tab, any scheduler hiccup -
// raises it permanently for the rest of the session.
//
// Against the frame interval:
//     59.94fps -> 16.68ms/frame; a ~2ms mark leaves most of the frame asleep
//     100fps   -> 10.00ms/frame; once the mark passes 10ms, `remaining` is
//                 ALWAYS below it, so every frame becomes a pure busy-spin
//     unlocked -> ticksPerFrame is 1ms, `now` is already past the deadline,
//                 so the loop body never executes at all - the limiter is free
//
// The spin was measured at ~31% of the MAIN thread earlier in this project
// (see PROGRESS.md and the UnlockFramerate notes) - and the main thread is
// what dispatches all other work. Burning a third of it is enough to turn
// ~110fps into ~70.
//
// Diagnostic first: log the mark every monitor window before changing
// anything. If it reads well under the frame interval, this theory is wrong
// and the slowdown is something else.
//
// SpinGuardUs then caps it. The cap must be close to the REAL Sleep(1)
// resolution - the timer probe measured this system at 1.0000ms - because
// setting it too low makes the limiter sleep when it should have spun and
// overshoot the deadline instead, which trades a throughput problem for a
// pacing one. 0 = off (default: observe before intervening).
#define SLEEP_GRAN_RVA (0x05115568 - 0x00400000)
static volatile LONG g_spinGuardUs = 0;
static volatile LONG g_spinGuardWrites = 0;
static volatile LONG g_lastGranUs = 0;

static void ReportAndClampSleepGranularity(void)
{
    if (g_mainModBase == 0) return;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return;
    __try {
        volatile unsigned int *gran =
            (volatile unsigned int *)(g_mainModBase + SLEEP_GRAN_RVA);
        unsigned __int64 cur =
            ((unsigned __int64)gran[1] << 32) | (unsigned __int64)gran[0];
        // Sanity: before the limiter has run even once this is 0, and a wild
        // value would mean the address is not what we think it is.
        unsigned __int64 us = (cur * 1000000ULL) / (unsigned __int64)f.QuadPart;
        if (us > 10000000ULL) return;            // >10s - not a sleep duration
        g_lastGranUs = (LONG)us;

        LONG capUs = g_spinGuardUs;
        if (capUs > 0 && us > (unsigned __int64)capUs) {
            unsigned __int64 want =
                ((unsigned __int64)capUs * (unsigned __int64)f.QuadPart) / 1000000ULL;
            if (want == 0) want = 1;
            gran[0] = (unsigned int)want;
            gran[1] = (unsigned int)(want >> 32);
            InterlockedIncrement(&g_spinGuardWrites);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- SIM DELTA: unquantise the per-frame simulation delta -----------------
//
// Reported by another modder working on the same 60fps bug class, then
// verified here against the decompile (ghidra_output/frame_delta.txt).
//
// FUN_00ac33b0 is the frame-delta function. It is called once per frame from
// FUN_00ab93f0, immediately after the limiter FUN_00ac3040, and its output
// lands in the sim struct:
//
//     FUN_00ac3040();                            // limiter sleeps to deadline
//     FUN_00ac3a90();
//     piVar3 = (int *)FUN_00ac33b0(&local_1c);   // delta
//     param_1[0xc] = *piVar3;
//
// What it computes:
//
//     bucket = qpcFreq * 1001 / 60000;      // 0x3e9 = 1001; ONE 59.94Hz frame
//     qnow   = (now / bucket) * bucket;     // <-- TRUNCATES THE TIMESTAMP
//     delta  = (qnow - qprev) * 300000 / qpcFreq;
//     *out   = ((delta + 0x9c6) / 0x138d) * 0x138d;    // 2502 / 5005
//
// The final rounding is NOT the problem - it is integer-division cleanup.
// The defect is the line above it: `now` is truncated to a whole number of
// 59.94Hz frame periods BEFORE differencing, so consecutive deltas can only
// ever be 0, 5005, 10010... Nothing between is representable.
//
// TWO effects stack, and the second is the one that makes this a STOCK bug.
//
// 1. Systematic rate mismatch. Buckets advanced per frame = 59.94 / F:
//        F = 30     -> 1.998
//        F = 59.94  -> 1.000
//        F = 60.00  -> 0.999   one ZERO every ~1000 frames
//        F = 120    -> 0.4995  0, 5005, 0, 5005 ...
//    The 120 row is exactly the reported "0ms, 16.683ms, 0ms, 16.683ms".
//
// 2. Frame-time JITTER. The limiter's deadline advances by exactly one bucket,
//    but `now` is sampled after its Sleep-spin loop exits and overshoots by a
//    variable amount. With now_k = deadline_k + jitter_k, a long frame followed
//    by a short one can put both truncated timestamps in the SAME bucket - a
//    zero delta - at ANY framerate.
//
// An earlier version of this comment claimed zeros were impossible at or below
// 59.94. That was wrong: it assumed a perfectly regular frame period. These
// bugs do occur in the stock game at its base 59.94 rate ("60fps" is rounding).
// What changes with framerate is only how much jitter it takes:
//        F = 30     -> a ~33ms negative swing; very rare
//        F = 59.94  -> enough to cross the phase boundary; sub-millisecond
//        F > 59.94  -> none needed, it happens on its own
//
// This is the better model because it explains the one property nothing else
// did: these bugs are PROBABILISTIC. Each has a chance to fail rather than
// failing reliably, and jitter-triggered bucket collisions produce exactly
// that. It also explains why 30fps mode fixes all four without zeros needing
// to be impossible - just orders of magnitude rarer.
//
// Consequence: this fix helps at the STOCK 59.94 rate too, not only under
// UnlockFramerate.
//
// NOTE this is a SEPARATE cached bucket from the one ApplyFramerateUnlock
// writes. The limiter caches at 0x05115570/74 (flag 0x05115578); this function
// caches its own at 0x05115580/84 (flag 0x05115588). Both compute
// freq*1001/60000. So unlocking the framerate speeds up pacing while leaving
// the delta quantum pinned at 59.94 - i.e. UnlockFramerate makes an existing
// stock defect fire constantly instead of occasionally. It does not create it.
//
// DO NOT "fix" this by writing the delta's own bucket global to match the real
// framerate. The 5005 rounding downstream is hardcoded: at 120fps the true
// delta becomes 2500 ticks, and round-to-nearest(2500, 5005) is ZERO. That
// makes it permanently broken instead of intermittently.
//
// The only workable intervention is to replace the OUTPUT with the true
// unquantised delta. `this+0x30` holds the raw 64-bit QPC timestamp the
// function itself uses as `now`, so the detour keeps its own previous raw
// value and recomputes:  (now - prev) * 300000 / qpcFreq.
//
// CONFIRMED IN GAME (2026-08-10) and therefore default ON. It fixes two
// independent symptoms at once:
//   - the interact prompt failing above 59.94fps
//   - the stamina bar not draining at higher framerates
// The stamina one is the more informative of the two: it is a continuous
// per-frame accumulation rather than a discrete event, which is direct
// evidence that the zero-deltas are being consumed as real elapsed time by
// ordinary gameplay code, not just by some edge-case state machine.
//
// RESIDUAL RISK, unproven either way: the engine has only ever seen deltas
// that are exact multiples of 5005. Anything downstream doing `delta / 5005`
// to derive a frame count would get 0 at 60fps where it used to get 1. No
// regression has been observed, but absence of a report is not absence of a
// bug - the flag stays live and hot-toggleable so any suspected regression can
// be A/B'd on the spot rather than requiring a rebuild.
#define SIM_DELTA_RVA (0x00ac33b0 - 0x00400000)
static void *g_trampoline_simDelta = NULL;
static DWORD g_sdTid = 0;
static DWORD g_sdRet = 0;
static unsigned int g_sdThis = 0, g_sdOut = 0;
static unsigned __int64 g_sdPrevRaw = 0;
static int g_sdHavePrev = 0;
static LONGLONG g_sdFreq = 0;
static volatile LONG g_sdReplaced = 0;   // frames whose delta we rewrote
static volatile LONG g_sdZeroFixed = 0;  // of those, ones the engine reported as 0
static volatile LONG g_sdLastOrig = 0, g_sdLastNew = 0;
// Cap a replacement at 8 frames' worth. After a loading screen or a long
// stall the true elapsed time can be seconds, and handing the simulation a
// single multi-second delta is its own class of disaster (physics tunnelling,
// timers firing en masse). The stock code cannot produce that because its
// deadline logic clamps first; ours has to clamp explicitly.
#define SD_MAX_DELTA (5005 * 8)

__declspec(noinline) int __cdecl OnEnter_simDelta_C(void *thisPtr, void *outPtr, void *retAddr)
{
    if (!g_simDeltaFix || !thisPtr || !outPtr) return 0;
    g_sdTid  = GetCurrentThreadId();
    g_sdRet  = (DWORD)retAddr;
    g_sdThis = (unsigned int)thisPtr;
    g_sdOut  = (unsigned int)outPtr;
    return 1;
}

__declspec(noinline) void *__cdecl OnReturn_simDelta_C(void)
{
    void *ret = (void *)g_sdRet;
    __try {
        if (g_sdTid != GetCurrentThreadId() || !g_sdThis || !g_sdOut) return ret;
        if (g_sdFreq == 0) {
            LARGE_INTEGER f;
            if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) g_sdFreq = f.QuadPart;
            else return ret;
        }
        unsigned __int64 now = *(volatile unsigned __int64 *)(g_sdThis + 0x30);
        LONG *out = (LONG *)g_sdOut;
        LONG orig = *out;
        if (g_sdHavePrev && now > g_sdPrevRaw) {
            unsigned __int64 d = now - g_sdPrevRaw;
            unsigned __int64 t = (d * 300000ULL) / (unsigned __int64)g_sdFreq;
            if (t > SD_MAX_DELTA) t = SD_MAX_DELTA;
            *out = (LONG)t;
            g_sdLastOrig = orig;
            g_sdLastNew  = (LONG)t;
            InterlockedIncrement(&g_sdReplaced);
            if (orig == 0) InterlockedIncrement(&g_sdZeroFixed);
        }
        g_sdPrevRaw = now;
        g_sdHavePrev = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// EAX must survive: the caller is decompiled as reading a pointer back out of
// it (`piVar3 = FUN_00ac33b0(...); param_1[0xc] = *piVar3;`). Whether that is
// real or a decompiler artefact of the trailing store, clobbering it would be
// the kind of bug that only shows up as garbage frame timing.
__declspec(naked) void OnReturn_simDelta(void)
{
    __asm {
        push eax                      // preserve the callee's return value
        pushfd
        call OnReturn_simDelta_C      // -> eax = the real return address
        mov  ecx, eax                 // ECX is volatile here, safe scratch
        popfd
        pop  eax                      // restore the return value
        jmp  ecx
    }
}

__declspec(naked) void Detour_simDelta(void)
{
    __asm {
        push ecx
        push edx
        // After both pushes: [esp+4]=this(ECX), [esp+8]=return address,
        // [esp+12]=the out-pointer stack arg. __cdecl wants them pushed
        // right-to-left to arrive as (thisPtr, outPtr, retAddr).
        mov eax, [esp + 8]       // return address -> 3rd
        push eax
        mov eax, [esp + 16]      // out-pointer    -> 2nd
        push eax
        mov eax, [esp + 12]      // this           -> 1st
        push eax
        call OnEnter_simDelta_C
        add esp, 12
        test eax, eax
        jz skip_simDelta
        mov dword ptr [esp + 8], offset OnReturn_simDelta
    skip_simDelta:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_simDelta]
    }
}

