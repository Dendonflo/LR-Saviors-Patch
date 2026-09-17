// ---- D3D9 method timing (v11) ---------------------------------------------
// v10 located the stall: 74% of watchdog captures had the main thread parked
// in the graphics stack, overwhelmingly inside AMDXN32.DLL (the AMD D3D9
// driver) calling a KERNELBASE wait, ~4.6 seconds of stall across the
// session. That finally explains DXVK: it replaces d3d9.dll wholesale and
// never enters AMDXN32's D3D9 path at all. The critical-section hook fired
// only twice, so lock contention - and with it the priority-inversion theory
// - is ruled out.
//
// What is NOT yet known is WHICH D3D9 call blocks, and the fix depends
// entirely on that:
//   - Present blocking  -> the GPU or the driver's queue is behind, and the
//                          long frame is a symptom rather than a cause.
//   - a Create*/Lock*   -> a CPU-side driver stall on resource creation or
//     blocking              on locking a resource the GPU still owns, which
//                          is directly pace-able the way ReadFile was.
//
// Mechanism: COM methods on x86 are __stdcall with `this` as the first stack
// argument and a callee-cleaned stack of unknown size, so the arg-count-
// agnostic return-address hijack used throughout this investigation is again
// the right tool. Each hooked slot gets a 10-byte thunk (`mov eax, <id>` /
// `jmp GenericD3DEntry`) so one shared entry/return pair covers every method.
//
// Deliberately NOT hooking the ultra-hot state setters (SetTexture,
// SetRenderState, SetVertexShaderConstantF, ...): they cannot block, and at
// hundreds of thousands of calls per second the hook overhead would itself
// perturb frame timing. Only methods that can plausibly block are hooked.
#include <d3d9.h>

#define MAX_D3D_SLOTS 64

typedef struct {
    void **entry;      // address of the vtable slot itself
    void *orig;        // original function pointer
    const char *name;
    volatile LONG count;
    volatile LONG sumUsec;
    volatile LONG maxUsec;
} D3DSlot;

static D3DSlot g_d3dSlots[MAX_D3D_SLOTS];
// Flat array of original function pointers, kept separate from D3DSlot purely
// so the dispatch thunk can index it with an encodable x86 scale (*4);
// sizeof(D3DSlot) is 24, which no scaled-index addressing mode can express.
static void *g_d3dOrig[MAX_D3D_SLOTS];
static volatile LONG g_d3dSlotCount = 0;
static unsigned char *g_d3dThunks = NULL;
static LONG g_prevD3dCount[MAX_D3D_SLOTS];
static LONG g_prevD3dSum[MAX_D3D_SLOTS];

// Two independent hook layers can target the same vtable slot: the generic
// timing thunks (installed from the throwaway probe device) and the real
// replacement functions (installed on the game's device). On Microsoft's
// d3d9.dll those are always different vtables, so they never meet. On a
// single-class wrapper they are the SAME vtable, and whichever layer runs
// second would capture the other's hook as "the original" - producing a hook
// that calls a hook, with a return-address hijack in the middle.
//
// This resolves a slot value that is really one of our thunks back to the
// function the thunk was built to call. Layout is fixed by HookVtableSlotEx:
// 16-byte stride, `B8 <id:32>` then `E9 <rel32>`, so the id is recoverable
// from the thunk body itself rather than by searching the slot table.
static void *ResolveOrigSlot(void *cur)
{
    if (!cur || !g_d3dThunks) return cur;
    unsigned char *p = (unsigned char *)cur;
    if (p < g_d3dThunks || p >= g_d3dThunks + MAX_D3D_SLOTS * 16) return cur;
    if ((size_t)(p - g_d3dThunks) % 16 != 0 || p[0] != 0xB8 || p[5] != 0xE9) return cur;
    LONG id = *(LONG *)(p + 1);
    if (id < 0 || id >= MAX_D3D_SLOTS || !g_d3dOrig[id]) return cur;
    return g_d3dOrig[id];
}

#define D3D_STACK_DEPTH 16
typedef struct {
    void *trueRetAddr;
    unsigned __int64 entryTsc;
    LONG slotId;
    // Copied out of the caller's stack AT ENTRY, not referenced by pointer:
    // by the time OnD3DReturn runs, the real callee has already executed its
    // own `ret N` (STDMETHODCALLTYPE cleans its own args), so the original
    // stack slots are stale/reused - only values copied into our own memory
    // early are safe to read later. Generic slot-agnostic capture (this +
    // first 4 explicit args); each hooked method interprets what it needs.
    DWORD argThis, arg1, arg2, arg3, arg4;
} D3DRetFrame;
static LONG g_lockRectSlotId = -1;
static LONG g_presentSlotId = -1;
static volatile LONG g_presentParamsLogged = 0;
static volatile LONG g_lockRectDiscardInjected = 0;
// Diagnostics so the next run can confirm the seen-table capacity fix
// empirically rather than leaving it to inference again. Declared here rather
// than beside the table itself because the injection site above uses them.
static volatile LONG g_seenTexClears = 0;
static volatile LONG g_lockRectEligible = 0;   // dynamic + fullrect + unflagged
static volatile LONG g_lockRectFirstSeen = 0;  // eligible but first sighting -> skipped
static volatile LONG g_lockRectMipSkipped = 0; // eligible but mipmapped -> never safe to discard
static LONG g_prevLockRectDiscardInjected = 0;
typedef struct {
    int top;
    D3DRetFrame frames[D3D_STACK_DEPTH];
} D3DThreadStack;
static DWORD g_d3dStackTls;

static D3DThreadStack *GetD3DStack(void)
{
    D3DThreadStack *ts = (D3DThreadStack *)TlsGetValue(g_d3dStackTls);
    if (!ts) {
        ts = (D3DThreadStack *)calloc(1, sizeof(D3DThreadStack));
        if (ts) TlsSetValue(g_d3dStackTls, ts);
    }
    return ts;
}

// rawStack points at the untouched original stack frame at the moment of
// entry: rawStack[0]=return address, [1]=this, [2..5]=first four explicit
// args. Valid only during this call (see D3DRetFrame comment), so everything
// needed later is copied out immediately.
static int __cdecl OnD3DEnter(void *trueRetAddr, LONG slotId, DWORD *rawStack)
{
    D3DThreadStack *ts = GetD3DStack();
    if (!ts || ts->top >= D3D_STACK_DEPTH) return 0;
    D3DRetFrame *f = &ts->frames[ts->top];
    f->trueRetAddr = trueRetAddr;
    f->entryTsc = __rdtsc();
    f->slotId = slotId;
    f->argThis = rawStack[1];
    f->arg1 = rawStack[2];
    f->arg2 = rawStack[3];
    f->arg3 = rawStack[4];
    f->arg4 = rawStack[5];
    ts->top++;

    // NOTE: a Present-based diagnostic used to live here. Confirmed dead:
    // slotId never equals g_presentSlotId in practice, because the device
    // vtable this thunk patches comes from a throwaway PROBE device and is
    // not shared with the game's real one (see the comment beside
    // g_stagingDevice's first assignment for why, and the replacement
    // diagnostic that actually works).

    // Fix attempt 13: v12 found the actual blocking call. All six slow
    // Texture::LockRect captures this session shared one exact signature -
    // D3DUSAGE_DYNAMIC, full-resource lock (pRect==NULL), Flags==0 - the
    // textbook D3D9 stall: without D3DLOCK_DISCARD or D3DLOCK_NOOVERWRITE,
    // the driver has no choice but to fully sync with the GPU before handing
    // back a write pointer, blocking the CPU until the GPU catches up.
    // DYNAMIC resources exist specifically to be used WITH one of those
    // flags; locking one without either is the anti-pattern this depends on.
    //
    // rawStack is a live pointer into the real call's stack, not a copy -
    // writing rawStack[5] here changes the Flags argument the real LockRect
    // is about to receive when GenericD3DEntry jumps into it, no additional
    // asm needed.
    //
    // Gated deliberately narrow, matching exactly what was observed and
    // nothing more:
    //   - only this slot (Texture::LockRect)
    //   - only D3DUSAGE_DYNAMIC resources (D3DLOCK_DISCARD is invalid/
    //     undefined on anything else)
    //   - only when pRect == NULL (a full-resource lock) - every slow
    //     capture was fullRect=1; a partial lock could be relying on
    //     previously-written regions surviving, which DISCARD would
    //     invalidate wholesale and cause visual corruption. Leaving partial
    //     locks completely untouched means this can only ever affect the
    //     exact pattern already confirmed to stall, never a code path that
    //     hasn't been observed.
    //   - only when neither flag is already set (nothing to fix otherwise)
    //
    // Default DISABLED (g_discardFixEnabled starts at 0): user confirmed via
    // reboot-with-config-saved that this fix IS the source of black/
    // transparent rendering (player face, distant geometry), and that
    // toggling it on mid-session (after boot) does NOT reproduce it - which
    // pins the corruption specifically to a texture's FIRST-time population,
    // concentrated at boot when nearly everything streams in at once.
    // DISCARD only affects future lock calls, so enabling it mid-session
    // with nothing new streaming in never exercises the vulnerable path.
    //
    // Refinement: track each distinct texture pointer seen through this
    // check and only inject DISCARD from its SECOND qualifying lock onward,
    // never its first. The first lock is presumably part of the vulnerable
    // initial-population sequence (however many steps that turns out to
    // be); by the second lock, that sequence has already completed at least
    // once without our interference. This also lines up with the original
    // evidence: the measured stalls were on a texture locked repeatedly
    // every frame (a lookup/curve table), not a one-time load - by the time
    // it reaches "steady state," it has been locked many times already, so
    // requiring just one prior sighting is a conservative floor, not a
    // tight fit to a specific count.
    // RETIRED: a D3DLOCK_NOOVERWRITE alternative to DISCARD was tried and
    // tested here (same injection site, same gating), on the reasoning that
    // NOOVERWRITE avoids the same GPU-sync wait without discarding the
    // buffer, so - unlike DISCARD - it should need no first-lock skip.
    // User testing found real graphical glitches in the main menu AND a
    // crash once level load completed. Confirms the flagged risk was real:
    // NOOVERWRITE is a promise the driver trusts rather than one it
    // enforces, and this resource's actual GPU usage evidently does
    // overlap with the CPU write closely enough to cause a genuine
    // race - a worse failure class than DISCARD's corruption (recoverable
    // by toggling off; this was not, since it could crash before a toggle
    // was even possible). Removed entirely rather than left behind a flag -
    // no hotkey, no checkbox, no config key can re-enable this. See
    // PROGRESS.md for the historical record if revisited.

    // NOTE: the DISCARD injection that used to live here has moved into
    // HookedTexLockRect. Texture::LockRect no longer routes through this
    // generic thunk at all - it has a dedicated replacement function so the
    // staging path can decline to call the original - so a slotId test for it
    // here would now never fire.
    return 1;
}

// Texture::LockRect logged 2.64s of total stall across the session, worst
// single call 343ms - the largest cost found anywhere in the D3D9 layer by a
// wide margin (everything else, including Present, never even crossed the
// reporting threshold). The mechanism that would explain everything else
// found so far (main thread parked inside AMDXN32.DLL calling a KERNELBASE
// wait, invisible to the EnterCriticalSection hook because it's a lock
// INSIDE the driver, not a kernel32 one the game calls) is
// D3DCREATE_MULTITHREADED: the reference HD-texture mod's own source
// confirms "FF13 creates its device with D3DCREATE_MULTITHREADED", which
// makes the AMD driver serialize ALL D3D9 calls from ALL threads behind one
// internal lock. If a Loader/decompression thread calls LockRect on newly-
// streamed texture data without D3DLOCK_DISCARD (forcing the driver to wait
// for the GPU to finish with that resource before handing back a pointer),
// every OTHER thread - including the main thread trying to Present or draw -
// would stall behind the SAME internal lock for the same duration. That is
// exactly the v10 signature.
// This is a single, cheap, targeted check to confirm or kill that
// mechanism directly: only on the rare slow path (>5ms) does this touch
// LogLine at all, so the 13,750 fast LockRect calls this session pay
// nothing beyond the branch itself.
// Tracks distinct texture pointers seen through the DISCARD-injection check
// below, so it can skip a texture's first qualifying lock and only inject
// DISCARD from its second onward (see the comment at the injection site for
// why).
// Keyed on (texture pointer, mip level) TOGETHER, not the pointer alone.
// Bug caught by the user's own observation: moving the camera closer made
// Lightning's textures/hair reappear - a distance/LOD signature, meaning
// different MIP LEVELS of the same texture are involved, not different
// textures. All mip levels of one texture share the SAME IDirect3DTexture9
// pointer, distinguished only by the Level argument to LockRect - keying on
// the pointer alone meant locking level 0 (used up close) marked the WHOLE
// texture "seen," so level 3's (used at a distance) true first-ever lock
// got incorrectly treated as already-safe and had DISCARD applied to it,
// corrupting exactly the far-distance mip while the near one stayed fine.
// Matches the original evidence too: the stalling captures showed BOTH a
// 64x64 and a 128x128 mip level of the same DXT5 texture.
//
// CAPACITY BUG (found from the town-run log, fixed here). The table above was
// a 512-entry linear array whose insert was guarded by
// `if (!found && n < MAX_TRACKED_TEXTURES)`. Once 512 distinct (tex,level)
// pairs had been seen, that guard stopped admitting new keys - and because
// "seen" is the ONLY thing that authorizes the injection, every texture first
// locked after the table filled was permanently classified as never-seen and
// could never be given DISCARD. The fix silently stopped working instead of
// failing loudly.
// The log confirms it exactly: all 3 DISCARD injections of a 258-second run
// happened in the first 7.5% of the session, then zero for the remaining
// 92.5% - and all 15 SLOW LockRect stalls (including one 386ms lock on the
// main thread) happened AFTER injections stopped. The fix died early and the
// stalls it exists to prevent all landed afterwards.
// Replaced with an open-addressed hash set, which also removes the O(n)
// linear scan that ran under a critical section on every single LockRect
// (up to 290 calls per window x 512 comparisons).
// On high load factor the table is CLEARED rather than allowed to fill.
// Clearing is the safe direction: a forgotten texture looks new again, so it
// merely skips one DISCARD before being re-learned. The opposite error -
// treating a genuinely-new texture as seen - is the one that corrupts, and
// clearing can never cause it.
#define SEEN_TEX_SLOTS 16384          // power of two
#define SEEN_TEX_MAX_LOAD 12288       // 75% - clear and relearn past this
typedef struct {
    DWORD texPtr;                     // 0 = empty slot
    UINT level;
    // Shape of the resource this entry was recorded against. A destroyed
    // texture can be replaced by a NEW one at the same address, and a stale
    // "seen" entry would then wrongly authorize DISCARD on that new
    // texture's genuine first lock - the exact corruption this whole design
    // exists to avoid. The old 512-entry table was accidentally shielded
    // from this because it filled up and stopped matching anything; now that
    // the table actually works, the exposure is real and lasts all session.
    // GetLevelDesc is already called at the injection site, so validating
    // the shape costs nothing extra. Same address + same level but different
    // dimensions or format means the pointer was recycled.
    UINT width, height;
    DWORD format;
} SeenTextureKey;
static SeenTextureKey *g_seenTextures;      // [SEEN_TEX_SLOTS] heap, see g_psMap in 03
static LONG g_seenTextureCount = 0;
static CRITICAL_SECTION g_seenTextureLock;
static volatile LONG g_seenTextureLockState = 0; // 0=not started, 1=initializing, 2=ready
static unsigned SeenTexHash(DWORD texPtr, UINT level)
{
    // Texture pointers are allocator-aligned, so the low bits carry almost no
    // entropy - mix before masking or everything piles into a few buckets.
    unsigned h = (unsigned)texPtr ^ ((unsigned)level * 0x9E3779B1u);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h & (SEEN_TEX_SLOTS - 1);
}

static int HasSeenTextureBefore(DWORD texPtr, UINT level, UINT width, UINT height, DWORD format)
{
    // Lazy one-time init rather than threading this through the various
    // install functions - cheap, and correctness doesn't depend on WHEN it
    // happens, only that it happens before first use. Three-state instead
    // of a plain bool: if a second thread hits this while the first is
    // still inside InitializeCriticalSection, a plain "already claimed"
    // flag would let it straight through to EnterCriticalSection on a
    // not-yet-initialized object - undefined behavior. Spinning until the
    // winner marks it READY closes that window.
    if (InterlockedCompareExchange(&g_seenTextureLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_seenTextureLock);
        g_seenTextureLockState = 2;
    } else {
        while (g_seenTextureLockState != 2) Sleep(0);
    }
    if (texPtr == 0) return 0;  // 0 is the empty-slot marker, never a real key

    EnterCriticalSection(&g_seenTextureLock);

    unsigned idx = SeenTexHash(texPtr, level);
    int found = 0;
    int present = 0;   // an entry for this key already exists - do not insert
    for (unsigned probe = 0; probe < SEEN_TEX_SLOTS; probe++) {
        SeenTextureKey *e = &g_seenTextures[idx];
        if (e->texPtr == 0) break;                 // empty slot: key is absent
        if (e->texPtr == texPtr && e->level == level) {
            present = 1;
            if (e->width == width && e->height == height && e->format == format) {
                found = 1;
            } else {
                // Pointer recycled onto a different resource. Re-point the
                // entry at the new shape and report NOT seen, so this
                // texture's real first lock is left alone.
                e->width = width; e->height = height; e->format = format;
                found = 0;
            }
            break;
        }
        idx = (idx + 1) & (SEEN_TEX_SLOTS - 1);
    }

    if (!present) {
        if (g_seenTextureCount >= SEEN_TEX_MAX_LOAD) {
            // Full enough that probe chains get long. Drop everything and
            // relearn - see the note above on why this direction is safe.
            memset(g_seenTextures, 0, SEEN_TEX_SLOTS * sizeof(SeenTextureKey));
            g_seenTextureCount = 0;
            InterlockedIncrement(&g_seenTexClears);
            idx = SeenTexHash(texPtr, level);
        }
        // idx is either the empty slot the search stopped on, or a fresh
        // hash after a clear; walk to the first empty slot from there.
        while (g_seenTextures[idx].texPtr != 0) idx = (idx + 1) & (SEEN_TEX_SLOTS - 1);
        g_seenTextures[idx].texPtr = texPtr;
        g_seenTextures[idx].level = level;
        g_seenTextures[idx].width = width;
        g_seenTextures[idx].height = height;
        g_seenTextures[idx].format = format;
        g_seenTextureCount++;
    }

    LeaveCriticalSection(&g_seenTextureLock);
    return found;
}

// Used by the texture pool (further down) when handing a released texture
// back out for reuse: from this table's own perspective that is exactly a
// "pointer recycled onto a different resource" event, the same case the
// shape-mismatch branch above already handles safely (forces the next
// lookup to report NOT seen, so DISCARD is correctly skipped again on the
// reused texture's first genuine lock in its new life). Forcing width to a
// value no real texture can have reuses that exact, already-proven-safe
// path instead of adding a second way to reach the same state.
static void InvalidateSeenTextureForReuse(DWORD texPtr, UINT levels)
{
    if (InterlockedCompareExchange(&g_seenTextureLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_seenTextureLock);
        g_seenTextureLockState = 2;
    } else {
        while (g_seenTextureLockState != 2) Sleep(0);
    }
    EnterCriticalSection(&g_seenTextureLock);
    for (UINT level = 0; level < levels; level++) {
        unsigned idx = SeenTexHash(texPtr, level);
        for (unsigned probe = 0; probe < SEEN_TEX_SLOTS; probe++) {
            SeenTextureKey *e = &g_seenTextures[idx];
            if (e->texPtr == 0) break;
            if (e->texPtr == texPtr && e->level == level) {
                e->width = 0xFFFFFFFFu;   // no real texture has this width
                break;
            }
            idx = (idx + 1) & (SEEN_TEX_SLOTS - 1);
        }
    }
    LeaveCriticalSection(&g_seenTextureLock);
}

static void *__cdecl OnD3DReturn(void)
{
    D3DThreadStack *ts = GetD3DStack();
    ts->top--;
    D3DRetFrame f = ts->frames[ts->top];
    if (g_cyclesPerUsec > 0.0) {
        LONG usec = (LONG)((double)(__rdtsc() - f.entryTsc) / g_cyclesPerUsec);
        D3DSlot *s = &g_d3dSlots[f.slotId];
        InterlockedIncrement(&s->count);
        InterlockedExchangeAdd(&s->sumUsec, usec);
        if (usec > s->maxUsec) s->maxUsec = usec;

        if (usec > 5000) {
            DWORD tid = GetCurrentThreadId();
            const char *name = (tid == (DWORD)g_mainThreadId) ? "MAIN" : "?";
            if (name[0] == '?') {
                EnterCriticalSection(&g_threadNameLock);
                for (LONG j = 0; j < g_threadNameCount; j++) {
                    if (g_threadNames[j].threadId == tid) { name = g_threadNames[j].name; break; }
                }
                LeaveCriticalSection(&g_threadNameLock);
            }
            char line[256];
            sprintf(line, "[d3d9] SLOW %s: %ld us on thread %lu (%s)",
                    s->name, usec, tid, name);
            LogLine(line);

            // LockRect specifically: decode which texture, and with what
            // flags. D3DLOCK_DISCARD/NOOVERWRITE tell the driver it never
            // needs to wait for the GPU to finish with this resource before
            // handing back a CPU pointer - locking WITHOUT either forces a
            // full sync if the GPU still has the resource in flight, which is
            // one of the most well-known D3D9 stall patterns and exactly
            // consistent with a duration in the hundreds of milliseconds.
            // LockRect slow-path detail logging also moved into
            // HookedTexLockRect along with the rest of that method.
        }
    }
    return f.trueRetAddr;
}

// Preserves EAX across the bookkeeping call - it carries the HRESULT that
// every one of these methods returns.
__declspec(naked) void OnD3DReturnStub(void)
{
    __asm {
        push eax
        call OnD3DReturn
        mov ecx, eax
        pop eax
        jmp ecx
    }
}

__declspec(naked) void GenericD3DEntry(void)
{
    __asm {
        ; On entry, esp points at the UNTOUCHED original stack frame:
        ; [esp]=return address, [esp+4]=this, [esp+8..]=explicit args. Save
        ; that address in edx before disturbing anything, so OnD3DEnter can
        ; read it directly (works for any method's argument layout, since it
        ; is just raw stack, not yet reinterpreted).
        lea edx, [esp]
        push eax                       ; save slot id
        push edx                       ; arg3: raw stack pointer (original esp)
        push eax                       ; arg2: slot id
        push dword ptr [esp + 12]      ; arg1: true return address (*rawStack)
        call OnD3DEnter
        add esp, 12
        test eax, eax
        jz skip_d3d_hijack
        mov dword ptr [esp + 4], offset OnD3DReturnStub
    skip_d3d_hijack:
        pop eax                        ; restore slot id
        jmp dword ptr g_d3dOrig[eax * 4]
    }
}

// `replacement` non-NULL installs a real C function in the vtable slot
// INSTEAD of the timing thunk, so the hook can decide not to call the
// original at all. The generic thunk always ends in `jmp orig`, which is
// fine for measuring but cannot substitute behaviour - and substituting
// behaviour is exactly what the staging-upload path below has to do.
static int HookVtableSlotEx(void **vtable, int slot, const char *name, void *replacement)
{
    LONG id = g_d3dSlotCount;
    if (id >= MAX_D3D_SLOTS || !vtable) return -1;

    void **entry = &vtable[slot];
    g_d3dSlots[id].entry = entry;
    g_d3dSlots[id].orig = *entry;
    g_d3dOrig[id] = *entry;
    g_d3dSlots[id].name = name;

    if (replacement) {
        DWORD oldProt;
        if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProt)) return -1;
        *entry = replacement;
        VirtualProtect(entry, sizeof(void *), oldProt, &oldProt);
        g_d3dSlotCount = id + 1;
        return (int)id;
    }

    unsigned char *thunk = g_d3dThunks + id * 16;
    if (!CodeUnseal(g_d3dThunks, MAX_D3D_SLOTS * 16)) return -1;   // W^X: RW only while writing
    thunk[0] = 0xB8;                                  // mov eax, imm32
    *(LONG *)(thunk + 1) = id;
    thunk[5] = 0xE9;                                  // jmp rel32
    *(int *)(thunk + 6) = (int)(void *)GenericD3DEntry - (int)(thunk + 10);
    if (!CodeSeal(g_d3dThunks, MAX_D3D_SLOTS * 16)) return -1;

    DWORD oldProtect;
    if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect)) return -1;
    *entry = thunk;
    VirtualProtect(entry, sizeof(void *), oldProtect, &oldProtect);

    g_d3dSlotCount = id + 1;
    return (int)id;
}

static int HookVtableSlot(void **vtable, int slot, const char *name)
{
    return HookVtableSlotEx(vtable, slot, name, NULL);
}

// ---- Staging-upload path (the real fix for the per-mip streaming stall) ---
//
// Everything measured so far says the dominant stutter is the main thread
// blocked inside AMDXN32.DLL waiting for the GPU to release a texture the
// engine wants to write. DISCARD is the usual answer, but it cannot work
// here: the engine streams INDIVIDUAL mip levels on demand, and DISCARD
// invalidates the whole resource, destroying the levels it is not writing.
// Measured directly - discarding every level cut per-call cost 168us -> 113us
// but corrupted distant LOD; restricting to level 0 was visually clean and
// bought nothing (166us) because only ~5.5% of mip locks are level 0.
//
// So instead of trying to make the stalling lock cheap, this removes the
// stalling lock entirely. On an eligible LockRect the engine is handed a
// D3DPOOL_SYSTEMMEM staging surface - plain CPU memory the GPU has never
// seen, so locking it can never wait on anything - and the real DEFAULT-pool
// surface is never locked at all. On UnlockRect the data is transferred with
// UpdateSurface, which the driver schedules in GPU command order instead of
// blocking the CPU until the GPU catches up.
//
// This is a genuine behaviour change rather than a flag tweak, so: default
// OFF behind its own toggle (F6 / StagingUpload), and every failure path
// falls back to the original LockRect rather than failing the call.
static volatile LONG g_stagingHits = 0, g_stagingFallback = 0, g_stagingUpdateFail = 0;
static volatile LONG g_stagingCreated = 0;
static volatile LONG g_stagingRecycled = 0;
static LONG g_stagingBytes = 0;

typedef struct {
    UINT w, h;
    D3DFORMAT fmt;
    IDirect3DTexture9 *tex;   // SYSTEMMEM
    UINT level;               // which mip level of tex is the (w,h) surface
    int inUse;
} StagingTex;

typedef struct {
    IDirect3DTexture9 *tex;   // the real DEFAULT-pool texture
    UINT level;
    LONG stagingIdx;
} InFlightLock;

#define MAX_STAGING 512
#define MAX_INFLIGHT 64
static StagingTex g_staging[MAX_STAGING];
static LONG g_stagingCount = 0;
static InFlightLock g_inflight[MAX_INFLIGHT];
static LONG g_inflightCount = 0;
static CRITICAL_SECTION g_stagingLock;
static volatile LONG g_stagingLockState = 0;
static IDirect3DDevice9 *g_stagingDevice = NULL;

static void EnsureStagingInit(void)
{
    if (InterlockedCompareExchange(&g_stagingLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_stagingLock);
        g_stagingLockState = 2;
    } else {
        while (g_stagingLockState != 2) Sleep(0);
    }
}

// Block-compressed formats cannot exist as a standalone texture smaller than
// one 4x4 block. Confirmed empirically: every surface-staging rejection was a
// tiny DXT1/DXT5 mip (1x1, 2x2, 2x4, 4x2...), CreateTexture failed on all of
// them, and the pool sat stuck at 248 entries because no new ones could be
// created. Those tiny mips were 84% of all surface locks, so this single
// limitation was defeating almost the whole surface fix.
//
// Fix: allocate a legal 4x4-aligned BASE and mip down to the requested size,
// then use the level whose dimensions match EXACTLY. Scaling both axes by the
// same 2^k preserves aspect, so level k is precisely (w,h) - which matters
// because UpdateSurface requires source and destination dimensions to agree.
//   1x1 -> base 4x4, 3 levels, use level 2
//   2x4 -> base 4x8, 2 levels, use level 1
static int IsBlockCompressed(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 ||
           f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

// Pitch is bytes per row of TEXELS for uncompressed formats, but bytes per
// row of 4x4 BLOCKS for compressed ones - so multiplying by Height
// overcounts DXT by 4x. The first measurement run did exactly that and
// reported ~3.1GB where the true figure is far lower; ratios were unaffected
// (same bias both sides) but absolute volume was not. Corrected here.
static LONG UploadKBFor(UINT pitch, UINT height, D3DFORMAT fmt)
{
    UINT rows = IsBlockCompressed(fmt) ? ((height + 3) / 4) : height;
    return (LONG)(((unsigned __int64)pitch * rows) / 1024);
}

// Returns which mip level of the created texture corresponds to (w,h).
//
// The constraint is DIVISIBLE by 4, not merely >= 4. First version used ">= 4"
// and still got rejections for 14x14, 7x7, 3x3, 14x28 - all mips of
// non-power-of-two DXT textures (a 28x28 chain chain mips 14 -> 7 -> 3), which
// clear ">= 4" but are not block-aligned. Doubling until both axes are
// multiples of 4 fixes those: 7x7 -> 28x28 (28 = 4*7) at level 2, 3x3 ->
// 12x12 at level 2, 14x14 -> 28x28 at level 1.
static UINT StagingMipPlan(UINT w, UINT h, UINT *baseW, UINT *baseH, UINT *levels)
{
    UINT k = 0;
    while (k < 12) {
        UINT bw = w << k, bh = h << k;
        if (bw >= 4 && bh >= 4 && (bw & 3) == 0 && (bh & 3) == 0) break;
        k++;
    }
    *baseW = w << k; *baseH = h << k; *levels = k + 1;
    return k;
}

// Caller must hold g_stagingLock.
static LONG AcquireStaging(UINT w, UINT h, D3DFORMAT fmt)
{
    for (LONG i = 0; i < g_stagingCount; i++) {
        StagingTex *s = &g_staging[i];
        if (!s->inUse && s->w == w && s->h == h && s->fmt == fmt) { s->inUse = 1; return i; }
    }

    // Pool full: RECYCLE a free entry instead of giving up.
    //
    // This pool used to only ever grow to MAX_STAGING and then return -1
    // forever, which is fine while one caller uses a handful of sizes - but
    // adding Surface::LockRect staging pushed it to 192/192 and the failure
    // was silent and severe: the texture path's fallback tripled (7,264 ->
    // 19,263) because surfaces had consumed every slot, and a 423ms stall
    // reappeared. Falling back is "safe" but it silently un-fixes whichever
    // caller loses the race for slots.
    //
    // Recycling always succeeds in practice: at most MAX_INFLIGHT (64)
    // entries can be in use simultaneously, well under the pool size, so a
    // free entry always exists. Releasing and recreating costs one
    // allocation, paid only when a genuinely new (w,h,fmt) shows up.
    if (g_stagingCount >= MAX_STAGING && g_stagingDevice) {
        for (LONG i = 0; i < g_stagingCount; i++) {
            StagingTex *s = &g_staging[i];
            if (s->inUse) continue;
            IDirect3DTexture9 *repl = NULL;
            UINT rlevel = 0;
            if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, w, h, 1, 0, fmt,
                                                      D3DPOOL_SYSTEMMEM, &repl, NULL)) || !repl) {
                if (IsBlockCompressed(fmt)) {
                    UINT bw, bh, lv;
                    rlevel = StagingMipPlan(w, h, &bw, &bh, &lv);
                    if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, bw, bh, lv, 0, fmt,
                                                              D3DPOOL_SYSTEMMEM, &repl, NULL)) || !repl) {
                        return -1;
                    }
                } else {
                    return -1;   // fall back rather than lose the old entry
                }
            }
            if (s->tex) IDirect3DTexture9_Release(s->tex);
            s->tex = repl; s->w = w; s->h = h; s->fmt = fmt; s->level = rlevel; s->inUse = 1;
            InterlockedIncrement(&g_stagingRecycled);
            return i;
        }
    }

    if (g_stagingCount >= MAX_STAGING || !g_stagingDevice) return -1;
    IDirect3DTexture9 *t = NULL;
    UINT useLevel = 0;
    // SYSTEMMEM + no usage flags is the only combination UpdateSurface will
    // accept as a source. Single level normally: each mip level of the real
    // texture is staged independently, which is how the engine streams them.
    if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, w, h, 1, 0, fmt,
                                              D3DPOOL_SYSTEMMEM, &t, NULL)) || !t) {
        // Block-compressed formats below one 4x4 block cannot be created
        // standalone - allocate a legal base and mip down to the exact size.
        if (!IsBlockCompressed(fmt)) return -1;
        UINT bw, bh, lv;
        useLevel = StagingMipPlan(w, h, &bw, &bh, &lv);
        if (FAILED(IDirect3DDevice9_CreateTexture(g_stagingDevice, bw, bh, lv, 0, fmt,
                                                  D3DPOOL_SYSTEMMEM, &t, NULL)) || !t) {
            return -1;
        }
    }
    LONG i = g_stagingCount;
    g_staging[i].w = w; g_staging[i].h = h; g_staging[i].fmt = fmt;
    g_staging[i].tex = t; g_staging[i].level = useLevel; g_staging[i].inUse = 1;
    g_stagingCount = i + 1;
    InterlockedIncrement(&g_stagingCreated);
    return i;
}


// ---- Deferred GPU upload queue -------------------------------------------
// Holds ownership of BOTH surface refs and the staging pool slot until the
// copy is actually issued - releasing any of them early would either free a
// surface the GPU still needs or hand the staging texture to another upload
// while its data is still pending.
//
// Capacity is deliberately well under MAX_STAGING: every queued entry keeps
// a staging slot reserved, so a queue as large as the pool would starve
// AcquireStaging and force the (correct but slower) fallback path. 192 of
// 512 leaves ample headroom.
#define MAX_PENDING_UPLOADS 192
typedef struct {
    IDirect3DSurface9 *src;
    IDirect3DSurface9 *dst;
    LONG stagingIdx;
} PendingUpload;
static PendingUpload g_uploadQueue[MAX_PENDING_UPLOADS];
static LONG g_uploadHead = 0;    // ring: next to drain
static LONG g_uploadCount = 0;
static CRITICAL_SECTION g_uploadQueueLock;
static volatile LONG g_uploadQueueLockState = 0;
static volatile LONG g_deferQueued = 0, g_deferIssued = 0, g_deferOverflow = 0;
static volatile LONG g_uploadQueueDepth = 0;

static void EnsureUploadQueueInit(void)
{
    if (InterlockedCompareExchange(&g_uploadQueueLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_uploadQueueLock);
        g_uploadQueueLockState = 2;
    } else {
        while (g_uploadQueueLockState != 2) Sleep(0);
    }
}

// Issues the copy and releases everything. Used both for the immediate path
// and when draining the queue, so ownership rules live in exactly one place.
static void DoUploadNow(IDirect3DSurface9 *src, IDirect3DSurface9 *dst, LONG stagingIdx)
{
    HRESULT hr = E_FAIL;
    if (g_stagingDevice && src && dst) {
        hr = IDirect3DDevice9_UpdateSurface(g_stagingDevice, src, NULL, dst, NULL);
    }
    if (FAILED(hr)) InterlockedIncrement(&g_stagingUpdateFail);
    if (src) IDirect3DSurface9_Release(src);
    if (dst) IDirect3DSurface9_Release(dst);
    if (stagingIdx >= 0) {
        EnterCriticalSection(&g_stagingLock);
        g_staging[stagingIdx].inUse = 0;
        LeaveCriticalSection(&g_stagingLock);
    }
}

// Takes ownership of src/dst refs and the staging slot.
static void SubmitStagedUpload(IDirect3DSurface9 *src, IDirect3DSurface9 *dst, LONG stagingIdx)
{
#if ENABLE_DEFER_UPLOADS
    if (g_deferUploadsEnabled) {
        EnsureUploadQueueInit();
        EnterCriticalSection(&g_uploadQueueLock);
        if (g_uploadCount < MAX_PENDING_UPLOADS) {
            LONG slot = (g_uploadHead + g_uploadCount) % MAX_PENDING_UPLOADS;
            g_uploadQueue[slot].src = src;
            g_uploadQueue[slot].dst = dst;
            g_uploadQueue[slot].stagingIdx = stagingIdx;
            g_uploadCount++;
            g_uploadQueueDepth = g_uploadCount;
            LeaveCriticalSection(&g_uploadQueueLock);
            InterlockedIncrement(&g_deferQueued);
            return;
        }
        LeaveCriticalSection(&g_uploadQueueLock);
        // Queue full - issue immediately rather than drop. Falling back
        // costs a stall but never loses texture data.
        InterlockedIncrement(&g_deferOverflow);
    }
#endif  // ENABLE_DEFER_UPLOADS
    DoUploadNow(src, dst, stagingIdx);
}

// Called once per frame from the main thread (the ac3040 frame hook), which
// is also where the engine itself issues D3D work - so the copies land in
// the same command stream, just spread across frames instead of bursting.
static void DrainStagedUploads(void)
{
    if (g_uploadQueueLockState != 2) return;
    LONG budget = g_deferPerFrame;
    if (budget < 1) budget = 1;
    for (LONG i = 0; i < budget; i++) {
        PendingUpload p;
        EnterCriticalSection(&g_uploadQueueLock);
        if (g_uploadCount == 0) { LeaveCriticalSection(&g_uploadQueueLock); return; }
        p = g_uploadQueue[g_uploadHead];
        g_uploadHead = (g_uploadHead + 1) % MAX_PENDING_UPLOADS;
        g_uploadCount--;
        g_uploadQueueDepth = g_uploadCount;
        LeaveCriticalSection(&g_uploadQueueLock);
        DoUploadNow(p.src, p.dst, p.stagingIdx);
        InterlockedIncrement(&g_deferIssued);
    }
}

typedef HRESULT (STDMETHODCALLTYPE *LockRectFn)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *UnlockRectFn)(IDirect3DTexture9 *, UINT);
static LockRectFn g_origLockRect = NULL;
static UnlockRectFn g_origUnlockRect = NULL;
static LONG g_unlockRectSlotId = -1;

// Replicates the bookkeeping the generic thunk would have done, so the
// existing [d3d9] per-window stats keep working for these two methods.
static LONG D3DSlotRecord(LONG slotId, unsigned __int64 t0)
{
    if (slotId < 0 || g_cyclesPerUsec <= 0.0) return 0;
    LONG usec = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
    D3DSlot *s = &g_d3dSlots[slotId];
    InterlockedIncrement(&s->count);
    InterlockedExchangeAdd(&s->sumUsec, usec);
    if (usec > s->maxUsec) s->maxUsec = usec;
    return usec;
}

static HRESULT STDMETHODCALLTYPE HookedTexLockRect(IDirect3DTexture9 *This, UINT Level,
                                                   D3DLOCKED_RECT *pLockedRect,
                                                   const RECT *pRect, DWORD Flags)
{
    D3DSURFACE_DESC desc;
    int haveDesc = 0;
    memset(&desc, 0, sizeof(desc));
    if (This && SUCCEEDED(IDirect3DTexture9_GetLevelDesc(This, Level, &desc))) haveDesc = 1;

    // Traversal-stutter investigation: 27/28 ZwWaitForAlertByThreadId
    // captures spread across a whole run (not just startup) share this exact
    // return address, which decompiles to FUN_00a6e360 doing
    // LockRect(level0,flags=0)+memcpy(16KB)+UnlockRect with no DISCARD. No
    // individual SLOW LockRect fired for it (checked: none this run), so the
    // cost is not one call crossing 5ms - something about repetition, the
    // memcpy itself, or whatever this stalls on inside AMDXN32 is the real
    // question. Logs full detail the first few times this exact call site is
    // seen, staged or not, to find out WHY staging isn't already absorbing it
    // (the outer eligibility gate below has no rejection diagnostic at all -
    // silent rejection here is the same blind spot as the DiscardMip0/
    // ThresholdUs missing-save-line bugs: works or doesn't with no visible
    // difference until specifically instrumented).
    if (haveDesc) {
        DWORD retAddr = (DWORD)(UINT_PTR)_ReturnAddress();
        DWORD ghidraVA = retAddr - g_mainModBase + 0x00400000;
        if (ghidraVA >= 0x00a6e360 && ghidraVA < 0x00a6e360 + 190) {
            static volatile LONG seen = 0;
            LONG n = InterlockedIncrement(&seen);
            if (n <= 6) {
                char l[256];
                sprintf(l, "[d3d9] FUN_00a6e360 LockRect call #%ld: %ux%u fmt=%d pool=%d usage=0x%lX "
                          "flags=0x%lX fullRect=%d stagingEligible=%d",
                        n, desc.Width, desc.Height, (int)desc.Format, (int)desc.Pool,
                        (unsigned long)desc.Usage, (unsigned long)Flags, pRect == NULL,
                        (g_stagingUploadEnabled && pLockedRect && pRect == NULL &&
                         !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) &&
                         desc.Pool == D3DPOOL_DEFAULT &&
                         !(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))));
                LogLine(l);
            }
        }
    }

    // --- staging path -----------------------------------------------------
    // Only full-surface writes to a DEFAULT-pool, non-rendertarget surface.
    // READONLY is excluded outright: the engine would be reading, and a fresh
    // staging surface holds no meaningful contents to read back.
    if (g_stagingUploadEnabled && haveDesc && pLockedRect && pRect == NULL &&
        !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) &&
        desc.Pool == D3DPOOL_DEFAULT &&
        !(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {

        EnsureStagingInit();
        EnterCriticalSection(&g_stagingLock);
        if (!g_stagingDevice) {
            // Take the device from the texture itself - the vtable was
            // obtained from a throwaway probe device, so that one is NOT the
            // device these textures belong to. The extra reference is kept
            // deliberately for the process lifetime.
            IDirect3DTexture9_GetDevice(This, &g_stagingDevice);

            // The device-vtable Present hook has NEVER fired in this
            // project's entire history (confirmed: zero Present/BeginScene/
            // EndScene/Clear/Draw* stat lines across the whole log, versus
            // thousands for Texture/VB/IB Lock) - meaning the probe device's
            // vtable is not shared with the real one, almost certainly
            // because this game creates its device via Direct3DCreate9Ex /
            // CreateDeviceEx, a genuinely different COM object from the
            // IDirect3DDevice9 our probe creates. Resource vtables (Texture,
            // VB, IB) have no Ex variant, so THOSE stayed shared - which is
            // exactly why LockRect/staging have real data throughout. This
            // g_stagingDevice pointer, by contrast, came from GetDevice() on
            // a texture the REAL game created, so it unambiguously IS the
            // real device - used here to answer the presentation-interval
            // question the broken Present hook could not.
            if (g_stagingDevice && InterlockedCompareExchange(&g_presentParamsLogged, 1, 0) == 0) {
                IDirect3DSwapChain9 *sc = NULL;
                if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(g_stagingDevice, 0, &sc)) && sc) {
                    D3DPRESENT_PARAMETERS pp;
                    memset(&pp, 0, sizeof(pp));
                    if (SUCCEEDED(IDirect3DSwapChain9_GetPresentParameters(sc, &pp))) {
                        char line[256];
                        const char *interval =
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) ? "IMMEDIATE(no cap)" :
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE)       ? "ONE(vsync, requested by GAME)" :
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_TWO)       ? "TWO(half refresh)" :
                            (pp.PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT)   ? "DEFAULT(driver decides)" : "OTHER";
                        sprintf(line, "[d3d9] REAL device swapchain (via GetDevice, not the broken Present link): "
                                      "Windowed=%d SwapEffect=%d BackBufferCount=%d RefreshRate=%luHz "
                                      "PresentationInterval=0x%lX %s",
                                pp.Windowed, (int)pp.SwapEffect, pp.BackBufferCount,
                                (unsigned long)pp.FullScreen_RefreshRateInHz,
                                (unsigned long)pp.PresentationInterval, interval);
                        LogLine(line);
                    }
                    IDirect3DSwapChain9_Release(sc);
                }
            }
        }
        LONG idx = AcquireStaging(desc.Width, desc.Height, desc.Format);
        IDirect3DTexture9 *stage = (idx >= 0) ? g_staging[idx].tex : NULL;
        UINT stageLevel = (idx >= 0) ? g_staging[idx].level : 0;
        LeaveCriticalSection(&g_stagingLock);
        // Lock released before touching D3D: the driver takes its own locks
        // and there is no reason to hold ours across that.

        if (stage) {
            D3DLOCKED_RECT lr;
            memset(&lr, 0, sizeof(lr));
            // Deliberately the ORIGINAL, not the IDirect3DTexture9_LockRect
            // macro. The staging texture shares the very vtable this function
            // is installed into, so the macro would re-enter this hook. The
            // SYSTEMMEM pool test above would bounce it straight back out, so
            // it would not actually recurse forever - but resting recursion
            // safety on that coincidence is far too subtle to leave standing.
            int ok = 0;
            if (SUCCEEDED(g_origLockRect(stage, stageLevel, &lr, NULL, 0))) {
                EnterCriticalSection(&g_stagingLock);
                if (g_inflightCount < MAX_INFLIGHT) {
                    g_inflight[g_inflightCount].tex = This;
                    g_inflight[g_inflightCount].level = Level;
                    g_inflight[g_inflightCount].stagingIdx = idx;
                    g_inflightCount++;
                    ok = 1;
                }
                LeaveCriticalSection(&g_stagingLock);
                if (ok) {
                    *pLockedRect = lr;           // engine writes into CPU memory
                    InterlockedIncrement(&g_stagingHits);
                    { LONG _kb = UploadKBFor(lr.Pitch, desc.Height, desc.Format);
                          InterlockedExchangeAdd(&g_uploadKB, _kb);
                          if ((LONG)GetCurrentThreadId() == g_mainThreadId)
                              InterlockedExchangeAdd(&g_uploadKBMain, _kb); }
                    D3DSlotRecord(g_lockRectSlotId, __rdtsc());  // ~0us by construction
                    return S_OK;                 // real surface never locked
                }
                g_origUnlockRect(stage, stageLevel);   // no slot to track it, undo
            }
            EnterCriticalSection(&g_stagingLock);
            g_staging[idx].inUse = 0;            // give the staging texture back
            LeaveCriticalSection(&g_stagingLock);
        }
        InterlockedIncrement(&g_stagingFallback);
        // falls through to the normal path below
    }

    // --- original path, with the existing DISCARD injection ----------------
    DWORD flags = Flags;
    if (g_discardFixEnabled && This && pRect == NULL &&
        !(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) &&
        haveDesc && (desc.Usage & D3DUSAGE_DYNAMIC)) {
        InterlockedIncrement(&g_lockRectEligible);
        UINT levels = IDirect3DTexture9_GetLevelCount(This);
        if (levels != 1) {
            InterlockedIncrement(&g_lockRectMipSkipped);
        } else if (HasSeenTextureBefore((DWORD)(UINT_PTR)This, Level,
                                        desc.Width, desc.Height, (DWORD)desc.Format)) {
            flags |= D3DLOCK_DISCARD;
            InterlockedIncrement(&g_lockRectDiscardInjected);
        } else {
            InterlockedIncrement(&g_lockRectFirstSeen);
        }
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origLockRect(This, Level, pLockedRect, pRect, flags);
    LONG usec = D3DSlotRecord(g_lockRectSlotId, t0);

    if (usec > 5000 && haveDesc) {
        char line[256];
        DWORD tid = GetCurrentThreadId();
        sprintf(line, "[d3d9] SLOW Texture::LockRect: %ld us on thread %lu (%s)",
                usec, tid, (tid == (DWORD)g_mainThreadId) ? "MAIN" : "?");
        LogLine(line);
        sprintf(line, "[d3d9]   LockRect detail: tex=%p level=%u/%lu %ux%u fmt=%d pool=%d "
                      "usage=0x%lX flags=0x%lX DISCARD=%d fullRect=%d",
                (void *)This, Level, (unsigned long)IDirect3DTexture9_GetLevelCount(This),
                desc.Width, desc.Height, (int)desc.Format, (int)desc.Pool,
                (unsigned long)desc.Usage, (unsigned long)flags,
                (flags & D3DLOCK_DISCARD) != 0, pRect == NULL);
        LogLine(line);
    }
    return hr;
}

// ---- IDirect3DSurface9::LockRect - the last unmeasured lock path ---------
//
// After staging removed every SLOW Texture::LockRect (confirmed: zero such
// events in the last two runs) and UpdateSurface measured at ~0us/call,
// 68.2% of remaining stutter STILL shows AMDXN32.DLL blocking with the same
// texture-upload chain on the stack (FUN_00aa28d0 / FUN_00aa3250). The
// return address 0x00aa335e lands immediately after `00aa335c CALL EDX` - an
// indirect COM vtable call - so something in that chain is calling an
// interface method we have never hooked.
//
// IDirect3DSurface9::LockRect is the obvious candidate and a genuine blind
// spot: it lives on a COMPLETELY SEPARATE vtable from
// IDirect3DTexture9::LockRect, so neither our instrumentation nor the
// staging interception has ever seen it. An engine that calls
// GetSurfaceLevel() and then locks the surface directly would bypass every
// texture-level fix in this file while producing exactly the observed
// signature.
//
// Taken from a REAL game surface (the destination in the staging transfer)
// rather than a throwaway probe object, because the probe-device approach is
// confirmed not to share vtables with the real objects - that mistake cost
// this project every device-level measurement it thought it had (see the
// "Methodological finding" section in PROGRESS.md). Diagnostic only for now:
// it times the call and never alters behaviour.
// MEASURED, and it is the remaining stutter: 9,519 calls, 2.50s total,
// **262.7us per call**, worst 13,136us - against 64us/call for the
// staging-fixed texture path, in a session whose entire stutter stall was
// 6.78s. The engine locks surfaces directly (GetSurfaceLevel then LockRect),
// bypassing every texture-level fix in this file.
//
// Same bug, same fix: hand the engine a SYSTEMMEM staging surface instead of
// letting it lock the DEFAULT-pool one, then transfer with UpdateSurface on
// unlock. The existing staging TEXTURE pool is reused rather than adding a
// second pool - its level-0 surface is exactly the SYSTEMMEM source
// UpdateSurface wants, and the pooling/eviction logic is already proven.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfaceLockRect)(
    IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfaceUnlockRect)(IDirect3DSurface9 *);
static PFN_SurfaceLockRect g_origSurfaceLockRect = NULL;
static PFN_SurfaceUnlockRect g_origSurfaceUnlockRect = NULL;
static volatile LONG g_surfLockHooked = 0;
static volatile LONG g_surfLockCount = 0, g_surfLockSumUsec = 0, g_surfLockMaxUsec = 0;
static volatile LONG g_surfStagingHits = 0, g_surfStagingFallback = 0, g_surfStagingUpdateFail = 0;

typedef struct {
    IDirect3DSurface9 *surf;
    LONG stagingIdx;
} InFlightSurf;
static InFlightSurf g_inflightSurf[MAX_INFLIGHT];
static LONG g_inflightSurfCount = 0;

static HRESULT STDMETHODCALLTYPE HookedSurfaceLockRect(
    IDirect3DSurface9 *This, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    if (g_stagingSurfaceEnabled && This && pLockedRect && pRect == NULL &&
        !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) {
        D3DSURFACE_DESC d;
        memset(&d, 0, sizeof(d));
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(This, &d)) &&
            d.Pool == D3DPOOL_DEFAULT &&
            !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {

            EnsureStagingInit();
            EnterCriticalSection(&g_stagingLock);
            if (!g_stagingDevice) IDirect3DSurface9_GetDevice(This, &g_stagingDevice);
            LONG idx = AcquireStaging(d.Width, d.Height, d.Format);
            IDirect3DTexture9 *stage = (idx >= 0) ? g_staging[idx].tex : NULL;
            UINT stageLevel = (idx >= 0) ? g_staging[idx].level : 0;
            LeaveCriticalSection(&g_stagingLock);

            if (stage) {
                D3DLOCKED_RECT lr;
                memset(&lr, 0, sizeof(lr));
                // Original, not the macro: the staging texture shares the
                // vtable this file already patched (same reasoning as the
                // texture staging path).
                if (SUCCEEDED(g_origLockRect(stage, stageLevel, &lr, NULL, 0))) {
                    int ok = 0;
                    EnterCriticalSection(&g_stagingLock);
                    if (g_inflightSurfCount < MAX_INFLIGHT) {
                        g_inflightSurf[g_inflightSurfCount].surf = This;
                        g_inflightSurf[g_inflightSurfCount].stagingIdx = idx;
                        g_inflightSurfCount++;
                        ok = 1;
                    }
                    LeaveCriticalSection(&g_stagingLock);
                    if (ok) {
                        *pLockedRect = lr;
                        InterlockedIncrement(&g_surfStagingHits);
                        { LONG _kb = UploadKBFor(lr.Pitch, d.Height, d.Format);
                          InterlockedExchangeAdd(&g_uploadKB, _kb);
                          if ((LONG)GetCurrentThreadId() == g_mainThreadId)
                              InterlockedExchangeAdd(&g_uploadKBMain, _kb); }
                        return S_OK;    // real surface never locked
                    }
                    g_origUnlockRect(stage, stageLevel);
                }
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            InterlockedIncrement(&g_surfStagingFallback);
            // Why did this fail? The pool now has spare capacity
            // (248/384, recycled=0) so exhaustion is ruled out - meaning
            // AcquireStaging's CreateTexture is rejecting these specific
            // shapes, or the in-flight table is full. Log the first few
            // DISTINCT shapes rather than guessing again. MultiSampleType is
            // included because a multisampled surface can be neither created
            // as a SYSTEMMEM texture nor used with UpdateSurface, which would
            // explain a large, consistent rejection count.
            {
                static DWORD seen[8]; static LONG seenN = 0;
                DWORD sig = (DWORD)d.Format ^ (d.Width << 4) ^ (d.Height << 16);
                int known = 0;
                for (LONG i = 0; i < seenN; i++) if (seen[i] == sig) { known = 1; break; }
                if (!known && seenN < 8) {
                    seen[seenN++] = sig;
                    char l2[224];
                    sprintf(l2, "[d3d9] surface staging REJECT: %ux%u fmt=%d pool=%d usage=0x%lX msaa=%d inflight=%ld",
                            d.Width, d.Height, (int)d.Format, (int)d.Pool,
                            (unsigned long)d.Usage, (int)d.MultiSampleType, g_inflightSurfCount);
                    LogLine(l2);
                }
            }
        }
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origSurfaceLockRect(This, pLockedRect, pRect, Flags);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_surfLockCount);
        InterlockedExchangeAdd(&g_surfLockSumUsec, us);
        if (us > g_surfLockMaxUsec) g_surfLockMaxUsec = us;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedSurfaceUnlockRect(IDirect3DSurface9 *This)
{
    if (g_stagingLockState == 2) {
        EnterCriticalSection(&g_stagingLock);
        LONG found = -1;
        for (LONG i = 0; i < g_inflightSurfCount; i++) {
            if (g_inflightSurf[i].surf == This) { found = i; break; }
        }
        if (found >= 0) {
            LONG idx = g_inflightSurf[found].stagingIdx;
            g_inflightSurf[found] = g_inflightSurf[g_inflightSurfCount - 1];
            g_inflightSurfCount--;
            IDirect3DTexture9 *stage = g_staging[idx].tex;
            UINT stageLevel = g_staging[idx].level;
            LeaveCriticalSection(&g_stagingLock);

            g_origUnlockRect(stage, stageLevel);

            IDirect3DSurface9 *src = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(stage, stageLevel, &src)) && src) {
                // AddRef the destination: previously it was used immediately
                // and needed no reference, but a deferred copy must keep it
                // alive until the copy actually issues.
                IDirect3DSurface9_AddRef(This);
                SubmitStagedUpload(src, This, idx);   // takes both refs + slot
            } else {
                InterlockedIncrement(&g_surfStagingUpdateFail);
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            return S_OK;
        }
        LeaveCriticalSection(&g_stagingLock);
    }
    return g_origSurfaceUnlockRect(This);
}

static void HookRealSurfaceLockRect(IDirect3DSurface9 *surf)
{
    if (!surf) return;
    if (InterlockedCompareExchange(&g_surfLockHooked, 1, 0) != 0) return;
    void **vtbl = *(void ***)surf;
    DWORD oldProtect;

    int slot = offsetof(IDirect3DSurface9Vtbl, LockRect) / sizeof(void *);
    g_origSurfaceLockRect = (PFN_SurfaceLockRect)ResolveOrigSlot(vtbl[slot]);
    if (VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slot] = (void *)HookedSurfaceLockRect;
        VirtualProtect(&vtbl[slot], sizeof(void *), oldProtect, &oldProtect);
    }

    int slotU = offsetof(IDirect3DSurface9Vtbl, UnlockRect) / sizeof(void *);
    g_origSurfaceUnlockRect = (PFN_SurfaceUnlockRect)ResolveOrigSlot(vtbl[slotU]);
    if (VirtualProtect(&vtbl[slotU], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[slotU] = (void *)HookedSurfaceUnlockRect;
        VirtualProtect(&vtbl[slotU], sizeof(void *), oldProtect, &oldProtect);
    }

    LogLine("[d3d9] REAL Surface::LockRect/UnlockRect links installed (from a live game surface)");
}

static HRESULT STDMETHODCALLTYPE HookedTexUnlockRect(IDirect3DTexture9 *This, UINT Level)
{
    if (g_stagingLockState == 2) {
        EnterCriticalSection(&g_stagingLock);
        LONG found = -1;
        for (LONG i = 0; i < g_inflightCount; i++) {
            if (g_inflight[i].tex == This && g_inflight[i].level == Level) { found = i; break; }
        }
        if (found >= 0) {
            LONG idx = g_inflight[found].stagingIdx;
            g_inflight[found] = g_inflight[g_inflightCount - 1];
            g_inflightCount--;
            IDirect3DTexture9 *stage = g_staging[idx].tex;
            UINT stageLevel = g_staging[idx].level;
            LeaveCriticalSection(&g_stagingLock);

            g_origUnlockRect(stage, stageLevel);   // original, not the macro - see LockRect

            // HD GUI mod interop: this is the only moment the engine's pixels
            // exist CPU-side (the real texture only ever receives a GPU-side
            // copy), so push them across BEFORE SubmitStagedUpload can recycle
            // the staging slot. The re-lock is SYSTEMMEM: no GPU sync by
            // construction. Level 0 only - that is all its hashing reads.
            if (g_hdTexNotify && g_hdTexPushEnabled && Level == 0) {
                D3DSURFACE_DESC hdDesc;
                D3DLOCKED_RECT hdLr;
                if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(This, Level, &hdDesc)) &&
                    SUCCEEDED(g_origLockRect(stage, stageLevel, &hdLr, NULL, D3DLOCK_READONLY))) {
                    g_hdTexNotify(This, Level, hdLr.pBits, (unsigned)hdLr.Pitch,
                                  hdDesc.Width, hdDesc.Height, (unsigned)hdDesc.Format);
                    g_origUnlockRect(stage, stageLevel);
                    InterlockedIncrement(&g_hdTexPushCount);
                }
            }

            // The transfer itself. UpdateSurface is queued into the command
            // stream, so it orders against GPU work instead of waiting for it
            // - that is the entire point of this path.
            IDirect3DSurface9 *src = NULL, *dst = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(stage, stageLevel, &src)) &&
                SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(This, Level, &dst))) {
                // Opportunistically hook IDirect3DSurface9::LockRect off a
                // REAL game surface. Must happen BEFORE handing the ref to
                // SubmitStagedUpload, which takes ownership of it.
                HookRealSurfaceLockRect(dst);
                SubmitStagedUpload(src, dst, idx);   // takes both refs + slot
            } else {
                if (src) IDirect3DSurface9_Release(src);
                if (dst) IDirect3DSurface9_Release(dst);
                InterlockedIncrement(&g_stagingUpdateFail);
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }

            D3DSlotRecord(g_unlockRectSlotId, __rdtsc());
            return S_OK;   // the real surface was never locked, nothing to unlock
        }
        LeaveCriticalSection(&g_stagingLock);
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origUnlockRect(This, Level);
    D3DSlotRecord(g_unlockRectSlotId, t0);
    return hr;
}

// ---- IDirect3DCubeTexture9::LockRect - another unhooked vtable ------------
//
// The FUN_00a6e360 call-site diagnostic recorded ZERO hits, despite 27/28
// traversal-stutter captures naming that function. The decompile reads
// vtable +0x4C / +0x50, which IS LockRect/UnlockRect on IDirect3DTexture9 -
// but it is ALSO LockRect/UnlockRect at the identical offsets on
// IDirect3DCubeTexture9, which is a completely separate vtable this project
// has never touched (confirmed: zero references to CreateCubeTexture or
// IDirect3DCubeTexture9 anywhere in this file before now).
//
// Corroborating: the very first decompile of this investigation found
// FUN_00aa28d0 looping over SIX FACES, and this function copies 16KB
// (64x64x4) - a typical cubemap face. Cubemaps are also exactly what a
// traversal trigger would touch (environment/reflection probes updating as
// the camera moves).
//
// Measurement ONLY for now - no staging redirect. Same discipline as the
// Surface::LockRect work: confirm it is expensive and actually called before
// changing behaviour. Cube LockRect takes an extra FaceType argument, so it
// needs its own signature; it cannot reuse the 2D texture hook.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CubeLockRect)(
    IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CubeUnlockRect)(
    IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT);
static PFN_CubeLockRect g_origCubeLockRect = NULL;
static PFN_CubeUnlockRect g_origCubeUnlockRect = NULL;
static volatile LONG g_cubeLockCount=0, g_cubeLockSumUsec=0, g_cubeLockMaxUsec=0;
static volatile LONG g_cubeStagingHits=0, g_cubeStagingFallback=0, g_cubeStagingUpdateFail=0;

// A cubemap upload locks all six faces in sequence, so the in-flight key must
// include the face - (texture, level) alone would collide across faces.
typedef struct {
    IDirect3DCubeTexture9 *tex;
    D3DCUBEMAP_FACES face;
    UINT level;
    LONG stagingIdx;
} InFlightCube;
static InFlightCube g_inflightCube[MAX_INFLIGHT];
static LONG g_inflightCubeCount = 0;
static volatile LONG g_cubeLockLogged = 0;

static HRESULT STDMETHODCALLTYPE HookedCubeLockRect(
    IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES Face, UINT Level,
    D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    // Staging redirect - same shape as the 2D texture and surface paths.
    if (g_stagingCubeEnabled && This && pLockedRect && pRect == NULL &&
        !(Flags & (D3DLOCK_READONLY | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) {
        D3DSURFACE_DESC cd;
        memset(&cd, 0, sizeof(cd));
        if (SUCCEEDED(IDirect3DCubeTexture9_GetLevelDesc(This, Level, &cd)) &&
            cd.Pool == D3DPOOL_DEFAULT &&
            !(cd.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {

            EnsureStagingInit();
            EnterCriticalSection(&g_stagingLock);
            if (!g_stagingDevice) IDirect3DCubeTexture9_GetDevice(This, &g_stagingDevice);
            LONG idx = AcquireStaging(cd.Width, cd.Height, cd.Format);
            IDirect3DTexture9 *stage = (idx >= 0) ? g_staging[idx].tex : NULL;
            UINT stageLevel = (idx >= 0) ? g_staging[idx].level : 0;
            LeaveCriticalSection(&g_stagingLock);

            if (stage) {
                D3DLOCKED_RECT lr;
                memset(&lr, 0, sizeof(lr));
                if (SUCCEEDED(g_origLockRect(stage, stageLevel, &lr, NULL, 0))) {
                    int ok = 0;
                    EnterCriticalSection(&g_stagingLock);
                    if (g_inflightCubeCount < MAX_INFLIGHT) {
                        g_inflightCube[g_inflightCubeCount].tex = This;
                        g_inflightCube[g_inflightCubeCount].face = Face;
                        g_inflightCube[g_inflightCubeCount].level = Level;
                        g_inflightCube[g_inflightCubeCount].stagingIdx = idx;
                        g_inflightCubeCount++;
                        ok = 1;
                    }
                    LeaveCriticalSection(&g_stagingLock);
                    if (ok) {
                        *pLockedRect = lr;
                        InterlockedIncrement(&g_cubeStagingHits);
                        { LONG _kb = UploadKBFor(lr.Pitch, cd.Height, cd.Format);
                          InterlockedExchangeAdd(&g_uploadKB, _kb);
                          if ((LONG)GetCurrentThreadId() == g_mainThreadId)
                              InterlockedExchangeAdd(&g_uploadKBMain, _kb); }
                        return S_OK;   // real cube face never locked
                    }
                    g_origUnlockRect(stage, stageLevel);
                }
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            InterlockedIncrement(&g_cubeStagingFallback);
        }
    }

    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCubeLockRect(This, Face, Level, pLockedRect, pRect, Flags);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_cubeLockCount);
        InterlockedExchangeAdd(&g_cubeLockSumUsec, us);
        if (us > g_cubeLockMaxUsec) g_cubeLockMaxUsec = us;
        // Log the first few with full detail, so if this IS the trigger we
        // already know the shape needed to stage it (pool/usage/format
        // decide eligibility) without another run.
        if (InterlockedIncrement(&g_cubeLockLogged) <= 6) {
            D3DSURFACE_DESC d;
            memset(&d, 0, sizeof(d));
            char l[224];
            if (SUCCEEDED(IDirect3DCubeTexture9_GetLevelDesc(This, Level, &d))) {
                sprintf(l, "[d3d9] CubeTexture::LockRect #%ld: face=%d level=%u %ux%u fmt=%d pool=%d usage=0x%lX flags=0x%lX fullRect=%d took=%ldus",
                        g_cubeLockLogged, (int)Face, Level, d.Width, d.Height, (int)d.Format,
                        (int)d.Pool, (unsigned long)d.Usage, (unsigned long)Flags, pRect == NULL, us);
            } else {
                sprintf(l, "[d3d9] CubeTexture::LockRect #%ld: face=%d level=%u (GetLevelDesc failed) flags=0x%lX took=%ldus",
                        g_cubeLockLogged, (int)Face, Level, (unsigned long)Flags, us);
            }
            LogLine(l);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCubeUnlockRect(
    IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES Face, UINT Level)
{
    if (g_stagingLockState == 2) {
        EnterCriticalSection(&g_stagingLock);
        LONG found = -1;
        for (LONG i = 0; i < g_inflightCubeCount; i++) {
            if (g_inflightCube[i].tex == This && g_inflightCube[i].face == Face &&
                g_inflightCube[i].level == Level) { found = i; break; }
        }
        if (found >= 0) {
            LONG idx = g_inflightCube[found].stagingIdx;
            g_inflightCube[found] = g_inflightCube[g_inflightCubeCount - 1];
            g_inflightCubeCount--;
            IDirect3DTexture9 *stage = g_staging[idx].tex;
            UINT stageLevel = g_staging[idx].level;
            LeaveCriticalSection(&g_stagingLock);

            g_origUnlockRect(stage, stageLevel);

            IDirect3DSurface9 *src = NULL, *dst = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(stage, stageLevel, &src)) &&
                SUCCEEDED(IDirect3DCubeTexture9_GetCubeMapSurface(This, Face, Level, &dst))) {
                SubmitStagedUpload(src, dst, idx);   // takes both refs + slot
            } else {
                if (src) IDirect3DSurface9_Release(src);
                if (dst) IDirect3DSurface9_Release(dst);
                InterlockedIncrement(&g_cubeStagingUpdateFail);
                EnterCriticalSection(&g_stagingLock);
                g_staging[idx].inUse = 0;
                LeaveCriticalSection(&g_stagingLock);
            }
            return S_OK;
        }
        LeaveCriticalSection(&g_stagingLock);
    }
    return g_origCubeUnlockRect(This, Face, Level);
}

// ---- Texture lifetime + upload-volume probes ------------------------------
// Deciding whether a TEXTURE POOL is worth building. A pool prevents the
// driver from tearing down and re-establishing GPU heaps (the plausible
// cause of the measured 8.8ms CreateTexture for a 64-byte texture), but it
// only ever gets used if the game actually RELEASES textures and later
// creates matching ones. If the game simply caches everything (which is what
// "second visit through an area is smoother" hints at), a pool would sit
// empty and buy nothing.
//
// So: count real destructions (Release returning 0), not just creations.
//   creates >> destroys  -> textures are cached, pooling is pointless.
//   creates ~= destroys  -> genuine churn, pooling should help.
//
// Also accumulate UPLOAD BYTES through the staging paths. CreateTexture is
// only ~0.12s/run against 1.2-2.8s of stall, so it cannot be the dominant
// first-visit cost; the engine's own memcpy of pixel data into the surfaces
// we hand it has never been measured and is the larger suspect. If a chunk
// load pushes tens of MB through a handful of frames, no pool addresses that
// and the effort belongs elsewhere.
typedef ULONG (STDMETHODCALLTYPE *PFN_TexRelease)(IDirect3DTexture9 *);
static PFN_TexRelease g_origTexRelease = NULL;
// (declared as tentative definitions near the top - see there)

// ---- Texture pool ----------------------------------------------------------
// Reuse a texture whose game-visible refcount just reached zero instead of
// letting the driver tear it down, and hand it back out on a later
// CreateTexture call that asks for the exact same shape. Two confirmed
// findings point here: creation-count correlates with slow frames
// (1.68-2.62x, fading but real once the cache is warm) and a single
// CreateTexture for a 64-byte texture measured 8.8ms - allocation cost
// tracks the driver's GPU heap bookkeeping, not the data size, so a repeat
// of a shape it has already allocated should be far cheaper. See
// PROGRESS.md "Texture-pool question ANSWERED" for the full measurement and
// the ~1/3-of-remaining-stutter ceiling estimate.
//
// Correctness rests on a property this project already relies on
// everywhere else: a texture is useless to the engine until it has been
// populated via LockRect/UnlockRect (or the staging UpdateSurface path), so
// stale pixel data left over from a reused texture's previous life is never
// visible - draw calls only ever happen after repopulation. RENDERTARGET,
// DEPTHSTENCIL and AUTOGENMIPMAP textures are excluded below precisely
// because that guarantee does NOT hold for them (the pipeline draws INTO a
// render target rather than populating it via Lock), and only
// MANAGED/DEFAULT pool textures with an explicit (non-zero) Levels count are
// considered, to avoid needing to replicate the driver's own mip-chain-size
// computation for the auto-chain (Levels=0) case.
//
// Interaction with the DISCARD fix (cause 1): that fix's HasSeenTextureBefore
// table already treats "same pointer, different shape" as a recycled
// address and correctly resets to "not seen" for it - the exact mechanism
// this pool's reuse needs, since a pool-hit reuse has the SAME shape by
// construction and would otherwise be wrongly treated as an already-seen
// texture, letting DISCARD apply to its first lock in its new life (the
// same corruption class already found and fixed once for cause 1).
// InvalidateSeenTextureForReuse (defined next to HasSeenTextureBefore)
// forces that same shape-mismatch path on every reuse.
// RETIRED (ENABLE_TEXTURE_POOL) - see the gate at the top of this file.
#if ENABLE_TEXTURE_POOL
#define TEXPOOL_MAX_ENTRIES 256
#define TEXPOOL_MAX_PER_KEY 8

typedef struct {
    IDirect3DTexture9 *tex;
    UINT width, height, levels;
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD usage;
    int inUse;
} TexPoolEntry;

static TexPoolEntry g_texPool[TEXPOOL_MAX_ENTRIES];
static LONG g_texPoolCount = 0;
static CRITICAL_SECTION g_texPoolLock;
static volatile LONG g_texPoolLockState = 0;
static volatile LONG g_texPoolHits = 0, g_texPoolMisses = 0;
static volatile LONG g_texPoolReturned = 0, g_texPoolEvicted = 0;

static void EnsureTexPoolInit(void)
{
    if (InterlockedCompareExchange(&g_texPoolLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_texPoolLock);
        g_texPoolLockState = 2;
    } else {
        while (g_texPoolLockState != 2) Sleep(0);
    }
}

static int IsPoolableTexture(DWORD usage, D3DPOOL pool)
{
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))
        return 0;
    if (pool != D3DPOOL_MANAGED && pool != D3DPOOL_DEFAULT)
        return 0;
    return 1;
}

static BOOL TryAcquireFromPool(UINT w, UINT h, UINT levels, D3DFORMAT fmt, D3DPOOL pool, DWORD usage,
                                IDirect3DTexture9 **outTex)
{
    EnsureTexPoolInit();
    EnterCriticalSection(&g_texPoolLock);
    for (LONG i = 0; i < g_texPoolCount; i++) {
        TexPoolEntry *e = &g_texPool[i];
        if (!e->inUse && e->width == w && e->height == h && e->levels == levels &&
            e->format == fmt && e->pool == pool && e->usage == usage) {
            e->inUse = 1;
            IDirect3DTexture9_AddRef(e->tex);
            *outTex = e->tex;
            LeaveCriticalSection(&g_texPoolLock);
            InvalidateSeenTextureForReuse((DWORD)(UINT_PTR)e->tex, levels);
            InterlockedIncrement(&g_texPoolHits);
            return TRUE;
        }
    }
    LeaveCriticalSection(&g_texPoolLock);
    InterlockedIncrement(&g_texPoolMisses);
    return FALSE;
}

// Called with a texture that is still safely alive (see HookedTexRelease's
// temp-AddRef pattern) whose game-visible refcount just reached zero.
// Returns TRUE if the pool took ownership (caller must NOT release it any
// further - the pool's ref stands in for the game's departed one), FALSE if
// the caller should let it die exactly like the unmodified path would.
static BOOL ReturnToPoolOrEvict(IDirect3DTexture9 *tex)
{
    EnsureTexPoolInit();
    EnterCriticalSection(&g_texPoolLock);
    for (LONG i = 0; i < g_texPoolCount; i++) {
        if (g_texPool[i].tex == tex) {
            g_texPool[i].inUse = 0;
            LeaveCriticalSection(&g_texPoolLock);
            InterlockedIncrement(&g_texPoolReturned);
            return TRUE;
        }
    }
    LeaveCriticalSection(&g_texPoolLock);

    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &desc))) return FALSE;
    if (!IsPoolableTexture(desc.Usage, desc.Pool)) return FALSE;
    UINT levels = IDirect3DTexture9_GetLevelCount(tex);
    if (levels == 0) return FALSE;

    EnterCriticalSection(&g_texPoolLock);
    if (g_texPoolCount >= TEXPOOL_MAX_ENTRIES) {
        LeaveCriticalSection(&g_texPoolLock);
        InterlockedIncrement(&g_texPoolEvicted);
        return FALSE;
    }
    LONG sameKey = 0;
    for (LONG i = 0; i < g_texPoolCount; i++) {
        TexPoolEntry *e = &g_texPool[i];
        if (e->width == desc.Width && e->height == desc.Height && e->levels == levels &&
            e->format == desc.Format && e->pool == desc.Pool && e->usage == desc.Usage)
            sameKey++;
    }
    if (sameKey >= TEXPOOL_MAX_PER_KEY) {
        LeaveCriticalSection(&g_texPoolLock);
        InterlockedIncrement(&g_texPoolEvicted);
        return FALSE;
    }
    TexPoolEntry *e = &g_texPool[g_texPoolCount++];
    e->tex = tex;
    e->width = desc.Width; e->height = desc.Height; e->levels = levels;
    e->format = desc.Format; e->pool = desc.Pool; e->usage = desc.Usage;
    e->inUse = 0;
    LeaveCriticalSection(&g_texPoolLock);
    return TRUE;
}

#endif  // ENABLE_TEXTURE_POOL

static ULONG STDMETHODCALLTYPE HookedTexRelease(IDirect3DTexture9 *This)
{
#if !ENABLE_TEXTURE_POOL
    // Pool retired: this is now just the destroy counter. Release is extremely
    // hot - every SetTexture can AddRef/Release - so with the pool gone the
    // branch it used to need goes with it.
    ULONG rc = g_origTexRelease(This);
    if (rc == 0) InterlockedIncrement(&g_texDestroyCount);
    return rc;
#else
    if (!g_texturePoolEnabled) {
        // Release is extremely hot (every SetTexture can AddRef/Release), so
        // this stays to a single call-through plus one compare on the common
        // path.
        ULONG rc = g_origTexRelease(This);
        if (rc == 0) InterlockedIncrement(&g_texDestroyCount);
        return rc;
    }
    // Pool path: hold a temp AddRef across the game's own Release so the
    // real refcount can never actually reach zero while we decide whether to
    // keep the object alive - touching it after a real zero-crossing would
    // be use-after-free, since a typical Release implementation frees the
    // object inside the same call that returns 0.
    IDirect3DTexture9_AddRef(This);
    ULONG rc = g_origTexRelease(This);   // performs the game's actual, intended decrement
    if (rc == 1) {
        // Only our temp ref is left: this WAS the game's last reference.
        // Safe to inspect (GetLevelDesc etc.) since we still hold a live ref.
        if (ReturnToPoolOrEvict(This)) {
            return 0;   // matches what a normal Release-to-zero returns
        }
        ULONG rc2 = g_origTexRelease(This);   // release our temp ref for real
        if (rc2 == 0) InterlockedIncrement(&g_texDestroyCount);
        return rc2;
    }
    // Other references remain - undo our temp ref and return the count a
    // normal, unmodified Release call would have reported.
    return g_origTexRelease(This);
#endif  // !ENABLE_TEXTURE_POOL
}

// Resource Lock methods live on their own per-type vtables, not the device's,
// and a Lock on a resource the GPU still owns is one of the classic D3D9
// stalls - so they must be covered. Creating one throwaway resource of each
// type is the cleanest way to obtain those vtables with correct types, rather
// than trying to intercept the first game-created resource.
//
// Called from two places (whichever runs first wins): the throwaway probe
// device on native (cheap there - see ProbeD3D9ForVtable), and the game's OWN
// real device via HookRealDevicePresent - the latter is now the ONLY path
// under a third-party d3d9.dll, since a second real device is what crashes
// DXVK (see the g_d3d9IsThirdParty comment and "First real WER crash dump").
// Idempotent so both call sites can be unconditional.
static void HookResourceVtables(IDirect3DDevice9 *dev)
{
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) != 0) return;

    IDirect3DTexture9 *tex = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL)) && tex) {
        void **vt = *(void ***)tex;
        // These two get real replacement functions rather than timing thunks:
        // the staging path has to be able to NOT call the original.
        {
            DWORD oldProtR;
            int slotR = offsetof(IDirect3DTexture9Vtbl, Release) / sizeof(void *);
            g_origTexRelease = (PFN_TexRelease)vt[slotR];
            if (VirtualProtect(&vt[slotR], sizeof(void *), PAGE_READWRITE, &oldProtR)) {
                vt[slotR] = (void *)HookedTexRelease;
                VirtualProtect(&vt[slotR], sizeof(void *), oldProtR, &oldProtR);
            }
        }
        g_lockRectSlotId = HookVtableSlotEx(vt, offsetof(IDirect3DTexture9Vtbl, LockRect) / sizeof(void *),
                                            "Texture::LockRect", (void *)HookedTexLockRect);
        if (g_lockRectSlotId >= 0) g_origLockRect = (LockRectFn)g_d3dSlots[g_lockRectSlotId].orig;
        g_unlockRectSlotId = HookVtableSlotEx(vt, offsetof(IDirect3DTexture9Vtbl, UnlockRect) / sizeof(void *),
                                              "Texture::UnlockRect", (void *)HookedTexUnlockRect);
        if (g_unlockRectSlotId >= 0) g_origUnlockRect = (UnlockRectFn)g_d3dSlots[g_unlockRectSlotId].orig;
        IDirect3DTexture9_Release(tex);
    }
    IDirect3DVertexBuffer9 *vb = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(dev, 256, 0, 0, D3DPOOL_MANAGED, &vb, NULL)) && vb) {
        void **vt = *(void ***)vb;
        HookVtableSlot(vt, offsetof(IDirect3DVertexBuffer9Vtbl, Lock) / sizeof(void *), "VB::Lock");
        IDirect3DVertexBuffer9_Release(vb);
    }
    // Cube textures: never instrumented before this point. LockRect sits at
    // the SAME vtable offset as IDirect3DTexture9's (+0x4C), which is why the
    // decompile of the traversal trigger was ambiguous between the two.
    IDirect3DCubeTexture9 *cube = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateCubeTexture(dev, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL)) && cube) {
        void **vt = *(void ***)cube;
        DWORD oldProt;
        int sl = offsetof(IDirect3DCubeTexture9Vtbl, LockRect) / sizeof(void *);
        g_origCubeLockRect = (PFN_CubeLockRect)vt[sl];
        if (VirtualProtect(&vt[sl], sizeof(void *), PAGE_READWRITE, &oldProt)) {
            vt[sl] = (void *)HookedCubeLockRect;
            VirtualProtect(&vt[sl], sizeof(void *), oldProt, &oldProt);
        }
        int su = offsetof(IDirect3DCubeTexture9Vtbl, UnlockRect) / sizeof(void *);
        g_origCubeUnlockRect = (PFN_CubeUnlockRect)vt[su];
        if (VirtualProtect(&vt[su], sizeof(void *), PAGE_READWRITE, &oldProt)) {
            vt[su] = (void *)HookedCubeUnlockRect;
            VirtualProtect(&vt[su], sizeof(void *), oldProt, &oldProt);
        }
        IDirect3DCubeTexture9_Release(cube);
        char l[128];
        sprintf(l, "[d3d9] CubeTexture LockRect/UnlockRect links installed (slots %d/%d)", sl, su);
        LogLine(l);
    }

    IDirect3DIndexBuffer9 *ib = NULL;
    if (SUCCEEDED(IDirect3DDevice9_CreateIndexBuffer(dev, 256, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL)) && ib) {
        void **vt = *(void ***)ib;
        HookVtableSlot(vt, offsetof(IDirect3DIndexBuffer9Vtbl, Lock) / sizeof(void *), "IB::Lock");
        IDirect3DIndexBuffer9_Release(ib);
    }
}

#define HOOK_DEV(m) HookVtableSlot(vt, offsetof(IDirect3DDevice9Vtbl, m) / sizeof(void *), #m)

static void HookDeviceVtable(IDirect3DDevice9 *dev)
{
    void **vt = *(void ***)dev;

    // The resource hooks are the ones that actually do work (Texture/Cube/
    // Surface LockRect - the staging fixes) and they are safe everywhere:
    // resource classes are shared across device classes even on native d3d9,
    // which is exactly why they fire on the game's textures today. They go in
    // unconditionally.
    //
    // The device-slot timing thunks below are the opposite: on native they
    // have never fired once (the probe device is SOFTWARE-vertex-processing,
    // the game's is HARDWARE, and Microsoft's runtime gives those different
    // classes), so they cost nothing and prove nothing. Under a single-class
    // wrapper like DXVK the same code would land on the game's live device -
    // untested return-address hijacks across the entire draw path, plus a
    // double-patch race with HookRealDevicePresent. Off by default there.
    //
    // d3d9.dll provenance alone cannot catch every single-class case: the HD
    // GUI mod (version.dll) wraps EVERY device CreateDevice returns in one
    // C++ proxy class, so probe and game devices share a vtable even though
    // d3d9.dll itself is genuinely System32's. The decisive test is where
    // this device's vtable actually LIVES: a native device vtable is inside
    // d3d9.dll; anywhere else means some wrapper owns this object, and the
    // DXVK reasoning applies unchanged. Resource hooks are unaffected either
    // way - they are read from real resource objects, which no wrapper here
    // proxies.
    HMODULE vtOwner = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)vt, &vtOwner);
    {
        HMODULE d3d9Mod = GetModuleHandleA("d3d9.dll");
        int vtIsWrapped = (vtOwner != d3d9Mod);   // unresolvable owner counts as wrapped
        if (vtIsWrapped) {
            char l[320]; char ownerName[MAX_PATH];
            ownerName[0] = '\0';
            if (vtOwner) GetModuleFileNameA(vtOwner, ownerName, MAX_PATH);
            sprintf(l, "[d3d9] device slot table lives in '%s', not d3d9.dll - wrapped device",
                    ownerName[0] ? ownerName : "<unknown>");
            LogLine(l);
        }
        int wantThunks = (g_probeDeviceThunks < 0)
                             ? (!g_d3d9IsThirdParty && !vtIsWrapped)
                             : (g_probeDeviceThunks != 0);
        if (!wantThunks) {
            HookResourceVtables(dev);
            LogLine("[d3d9] device-slot timing thunks SKIPPED (wrapped device or "
                    "third-party d3d9.dll); resource links installed as normal");
            return;
        }
    }

    g_presentSlotId = HOOK_DEV(Present);
    HOOK_DEV(Reset);
    HOOK_DEV(TestCooperativeLevel);
    HOOK_DEV(EvictManagedResources);
    HOOK_DEV(CreateTexture);
    HOOK_DEV(CreateVertexBuffer);
    HOOK_DEV(CreateIndexBuffer);
    HOOK_DEV(CreateRenderTarget);
    HOOK_DEV(CreateDepthStencilSurface);
    HOOK_DEV(CreateOffscreenPlainSurface);
    HOOK_DEV(CreateVertexShader);
    HOOK_DEV(CreatePixelShader);
    HOOK_DEV(CreateQuery);
    HOOK_DEV(UpdateSurface);
    HOOK_DEV(UpdateTexture);
    HOOK_DEV(GetRenderTargetData);
    HOOK_DEV(StretchRect);
    HOOK_DEV(ColorFill);
    HOOK_DEV(SetRenderTarget);
    HOOK_DEV(BeginScene);
    HOOK_DEV(EndScene);
    HOOK_DEV(Clear);
    HOOK_DEV(DrawPrimitive);
    HOOK_DEV(DrawIndexedPrimitive);
    HOOK_DEV(DrawPrimitiveUP);
    HOOK_DEV(DrawIndexedPrimitiveUP);

    HookResourceVtables(dev);

    char line[192];
    sprintf(line, "[d3d9] device slot table linked, %ld slots instrumented", g_d3dSlotCount);
    LogLine(line);
}

// Defined here rather than inline in MonitorThread because the D3D9 state it
// reads is declared below the monitor in this file.
