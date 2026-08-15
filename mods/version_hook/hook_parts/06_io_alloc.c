// ---- Loader dispatch throttle (call-site patch, NOT entry/return hijack) --
// FUN_004b5cb0 (the "process one loader request" dispatch switch, found at
// the very start of this investigation) has the same SEH prologue pattern
// that caused the earlier crash when hooked via return-address hijacking
// (FUN_00a01a00/FUN_00a015b0). Its only caller is FUN_004b68e0 (the Loader
// thread's own main loop), which has no SEH scaffolding, and FUN_004b5cb0
// is __fastcall with a single register argument (no stack args at all) -
// so instead of touching either function's entry/return path, this patches
// the single `CALL FUN_004b5cb0` instruction at its one call site
// (Ghidra VA 0x004b6a35, confirmed via Ghidra to be the only reference to
// this function anywhere in the binary) to redirect through a semaphore-
// throttling wrapper. FUN_004b5cb0 itself is never modified and keeps its
// own SEH fully intact - our wrapper brackets a normal, real nested CALL
// to it (standard CALL/RET, no manual return-address manipulation, so an
// exception propagating out of FUN_004b5cb0 unwinds through our wrapper's
// stack frame exactly like any other nested call, no special handling
// needed). Verified the code immediately after the call site reloads
// everything it needs from memory rather than relying on any register
// surviving across the call, so it's safe for our wrapper to leave
// EAX/ECX/EDX in whatever state our own Acquire/Release calls happen to
// leave them (matches how a void function's return already offers no
// register guarantees anyway).

#define LOADER_DISPATCH_CALL_SITE_RVA (0x004b6a35 - 0x00400000)
#define LOADER_DISPATCH_FUNC_RVA (0x004b5cb0 - 0x00400000)
#define LOADER_THROTTLE_MAX 2

// ---- Bulk-load auto-bypass ----------------------------------------------
// User confirmed the bandwidth throttles genuinely help the chunk-load
// stutters (so the trigger really is the game loading too much at once), but
// they also make LOADING SCREENS much slower - which is pure downside, since
// nobody is looking at a frame counter there.
//
// The two cases are distinguishable without finding the loader functions at
// all: a loading screen is SUSTAINED bulk reading, while traversal streaming
// is intermittent bursts between quiet periods. So the throttles bypass
// themselves whenever read volume stays above a bulk threshold for two
// consecutive monitor windows, and re-engage once it drops for two.
//
// Hysteresis (2 windows each way) rather than an instant flip: a single burst
// during traversal must not disable the throttle for the very stutter it
// exists to prevent, and one quiet window mid-load must not re-throttle a
// load that is still running.
#define BULK_LOAD_BYTES_PER_WINDOW (6 * 1024 * 1024)   // ~12MB/s sustained
static volatile LONG g_bulkLoadActive = 0;

static HANDLE g_loaderThrottleSem = NULL;
static void *g_realLoaderDispatch = NULL;
static volatile LONG g_loaderDispatchCount = 0;
// Default ON (attempt 6) - never conclusively shown to help on its own, but
// never shown to cause any problem either. Runtime-toggleable for benchmark
// isolation, same as everything else.
// OFF: still unconcluded. One machine measured a null, but beta testers
// report a possible DX9 effect and dispatch/priority behaviour depends on
// core count and contention. Off is the honest default until a real A/B
// settles it - see the note at the top of this file.
static volatile LONG g_loaderThrottleEnabled = 0;
// Acquire/Release must agree on whether THIS dispatch actually took a
// semaphore slot, even if the flag changes in between - otherwise toggling
// mid-dispatch could Acquire (flag ON) then skip the matching Release (flag
// now OFF), permanently leaking a slot and eventually deadlocking the
// throttle once re-enabled. TLS records the decision made at Acquire time so
// Release always mirrors it, regardless of the flag's value by then.
static DWORD g_loaderThrottleAcquiredTls;

static void __cdecl AcquireLoaderThrottle(void)
{
    int acquiring = (g_loaderThrottleEnabled != 0) && !g_bulkLoadActive;
    TlsSetValue(g_loaderThrottleAcquiredTls, (LPVOID)(UINT_PTR)acquiring);
    if (acquiring) {
        WaitForSingleObject(g_loaderThrottleSem, INFINITE);
    }
}

static void __cdecl ReleaseLoaderThrottle(void)
{
    InterlockedIncrement(&g_loaderDispatchCount);
    if ((UINT_PTR)TlsGetValue(g_loaderThrottleAcquiredTls)) {
        ReleaseSemaphore(g_loaderThrottleSem, 1, NULL);
    }
}

// ---- Dynamic allocation tracking + prefetch --------------------------------
// Rather than reverse-engineering every loader request type's exact buffer
// layout (dozens of cases in FUN_004b5cb0's switch, each different), track
// whatever memory the game's own allocators (HeapAlloc/VirtualAlloc, both
// confirmed imported by name) actually hand out WHILE a dispatch is
// in-flight on this thread, then prefetch exactly those regions the moment
// the dispatch returns - before the job system gets around to touching them
// later. This generalizes automatically to whichever request type turns
// out to matter, without needing to know its structure.

#define MAX_TRACKED_ALLOCS 256
typedef struct {
    void *ptr;
    size_t size;
} TrackedAlloc;
typedef struct {
    int inDispatch;
    int count;
    TrackedAlloc allocs[MAX_TRACKED_ALLOCS];
} DispatchTrackState;

static DWORD g_dispatchTrackTls;
static volatile LONG g_prefetchedAllocCount = 0;
static volatile LONGLONG g_prefetchedByteCount = 0;

static DispatchTrackState *GetDispatchTrackState(void)
{
    DispatchTrackState *st = (DispatchTrackState *)TlsGetValue(g_dispatchTrackTls);
    if (!st) {
        st = (DispatchTrackState *)calloc(1, sizeof(DispatchTrackState));
        if (st) TlsSetValue(g_dispatchTrackTls, st);
    }
    return st;
}

static void __cdecl BeginDispatchTracking(void)
{
    DispatchTrackState *st = GetDispatchTrackState();
    if (st) {
        st->inDispatch = 1;
        st->count = 0;
    }
}

static void __cdecl EndDispatchTrackingAndPrefetch(void)
{
    DispatchTrackState *st = GetDispatchTrackState();
    if (!st) return;
    st->inDispatch = 0;
    for (int i = 0; i < st->count; i++) {
        unsigned char *p = (unsigned char *)st->allocs[i].ptr;
        size_t sz = st->allocs[i].size;
        if (!p || sz == 0 || sz > 16 * 1024 * 1024) continue; // sanity cap only
        // v6: no prefetch here any more. Every allocation this loop would
        // have covered is already enqueued to the warmer thread from
        // OnAllocatorReturn (a strictly wider net), and warming it a second
        // time inline on the loader thread would only duplicate the work.
        // The counters are kept purely as the "how much was allocated inside
        // a loader dispatch" signal, to compare against the per-thread census.
        InterlockedIncrement(&g_prefetchedAllocCount);
        g_prefetchedByteCount += (LONGLONG)sz;
    }
    st->count = 0;
}

static void RecordAllocIfTracking(void *ptr, size_t size)
{
    if (!ptr || size == 0) return;
    DispatchTrackState *st = (DispatchTrackState *)TlsGetValue(g_dispatchTrackTls);
    if (!st || !st->inDispatch) return;
    if (st->count < MAX_TRACKED_ALLOCS) {
        st->allocs[st->count].ptr = ptr;
        st->allocs[st->count].size = size;
        st->count++;
    }
}

typedef LPVOID(WINAPI *PFN_HeapAlloc)(HANDLE, DWORD, SIZE_T);
static PFN_HeapAlloc g_realHeapAlloc = NULL;

static LPVOID WINAPI HookedHeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
{
    LPVOID result = g_realHeapAlloc(hHeap, dwFlags, dwBytes);
    RecordAllocIfTracking(result, dwBytes);
    if (result) CensusRecord(ALLOC_SRC_HEAP, dwBytes);
    return result;
}

typedef LPVOID(WINAPI *PFN_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static PFN_VirtualAlloc g_realVirtualAlloc = NULL;

static LPVOID WINAPI HookedVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    LPVOID result = g_realVirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    // Only track actual commits, not pure reservations - a reserved-but-
    // uncommitted region has no real content yet, prefetching it would be
    // pointless busywork (though harmless - PREFETCH* never faults).
    if ((flAllocationType & MEM_COMMIT) != 0) {
        RecordAllocIfTracking(result, dwSize);
        if (result) {
            CensusRecord(ALLOC_SRC_VIRTUAL, dwSize);
            if ((LONG)GetCurrentThreadId() != g_mainThreadId) EnqueueWarm(result, dwSize);
        }
    }
    return result;
}

// MapViewOfFile / ReadFile: the two remaining bulk paths the exe imports.
// A memory-mapped view is the classic way a 2013-era streaming engine gets
// archive contents into the address space, and its pages fault in on first
// touch - a signature that would look exactly like "cold data sourced from
// far away" to the animation code touching it, while never appearing in any
// allocator at all. Both are plain WINAPI, so both are safe IAT patches.

typedef LPVOID(WINAPI *PFN_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
static PFN_MapViewOfFile g_realMapViewOfFile = NULL;

static LPVOID WINAPI HookedMapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                                         DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                                         SIZE_T dwNumberOfBytesToMap)
{
    LPVOID result = g_realMapViewOfFile(hFileMappingObject, dwDesiredAccess,
                                        dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
    if (result) {
        SIZE_T size = dwNumberOfBytesToMap;
        if (size == 0) {
            // 0 means "to end of mapping" - ask the memory manager how big
            // the view actually turned out to be.
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(result, &mbi, sizeof(mbi)) == sizeof(mbi)) size = mbi.RegionSize;
        }
        CensusRecord(ALLOC_SRC_MAPVIEW, size);
        if ((LONG)GetCurrentThreadId() != g_mainThreadId) EnqueueWarm(result, size);
    }
    return result;
}

typedef BOOL(WINAPI *PFN_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static PFN_ReadFile g_realReadFile = NULL;

// ---- ReadFile rate limiter (fix attempt 8) --------------------------------
// v7's census found the bulk path: ReadFile on the Loader threads, peaking at
// 19.1MB per 500ms (~38MB/s) in ~87KB calls, while HeapAlloc, VirtualAlloc
// and MapViewOfFile were flat zero for the entire session.
//
// This reframes the whole problem. Every fix so far assumed the NEW data is
// cold and tried to warm it. But a ~19MB burst of file data (plus whatever
// ZDecode expands it into) sweeping through the 96MB L3 does something worse:
// it EVICTS the working set of the actors already on screen. The next frames'
// animation and physics jobs then have to re-fetch their own, previously
// warm, data from DRAM. That is precisely the measured signature - same
// instruction count, IPC down 24%, DRAM-sourced fills up 554%, extra time
// attributed to the game's own compute functions rather than the kernel - and
// it explains why five attempts at prefetching the newcomer changed nothing:
// the newcomer being cold was never the problem.
//
// Total bytes cannot be reduced from outside the engine, but the RATE can.
// Spreading the same transfer over more time lowers the eviction pressure per
// unit time and lets L3 re-warm between bursts. Loader threads are already
// BELOW_NORMAL and stream ahead of the player, so trading load latency for
// frame time is the right direction - a slightly later asset is invisible, a
// 67ms frame is not.
//
// Token bucket, no read splitting: an 87KB read is trivial against a 96MB
// cache on its own, so only the aggregate rate needs shaping. Not splitting
// means every ReadFile call is passed through byte-for-byte unchanged, which
// avoids any question about partial reads, EOF handling, or two threads
// sharing a file handle.
#define READ_RATE_BYTES_PER_SEC (12 * 1024 * 1024)
#define READ_BURST_SECONDS 0.25

static double g_readTokens = 0.0;
static unsigned __int64 g_readLastTsc = 0;
static CRITICAL_SECTION g_readPaceLock;
static volatile LONG g_readPaceDelayUsec = 0;

// Confirmed via a direct A/B test: disabling this reproduced a 101ms
// stutter (reads=272/20575KB in a single frame, main thread parked in
// AMDXN32.DLL). Default ON; runtime-toggleable (checkbox/hotkey) for
// benchmarking, not because it's suspected of causing problems.
static volatile LONG g_readPaceEnabled = 1;

static void PaceRead(DWORD bytes)
{
    if (!g_readPaceEnabled) return;
    if (g_bulkLoadActive) return;   // loading screen - let it run at full speed
    if (g_cyclesPerUsec <= 0.0) return; // calibration not done yet - never stall
    const double cap = (double)READ_RATE_BYTES_PER_SEC * READ_BURST_SECONDS;
    unsigned __int64 waitStart = __rdtsc();
    for (;;) {
        int allowed;
        EnterCriticalSection(&g_readPaceLock);
        unsigned __int64 now = __rdtsc();
        if (g_readLastTsc == 0) g_readLastTsc = now;
        double elapsedSec = ((double)(now - g_readLastTsc) / g_cyclesPerUsec) / 1000000.0;
        g_readLastTsc = now;
        g_readTokens += elapsedSec * (double)READ_RATE_BYTES_PER_SEC;
        if (g_readTokens > cap) g_readTokens = cap;
        // A single read larger than the whole burst allowance can never be
        // satisfied by waiting - let it straight through rather than spin.
        allowed = (g_readTokens >= (double)bytes) || ((double)bytes > cap);
        if (allowed) g_readTokens -= (double)bytes;
        LeaveCriticalSection(&g_readPaceLock);
        if (allowed) break;
        Sleep(1);
    }
    LONG waited = (LONG)((double)(__rdtsc() - waitStart) / g_cyclesPerUsec);
    if (waited > 0) {
        InterlockedExchangeAdd(&g_readPaceDelayUsec, waited);
        InterlockedExchangeAdd(&g_readPaceDelayTotalUsec, waited);
    }
}

// ---- ReadFile deep diagnostics (digging into cause 2 properly) -----------
// The rate limiter above only ever trades load latency for frame time - it
// does not explain WHY these bursts cost what they cost, which is the actual
// question now that cause 1 (LockRect) is fixed and confirmed. Three
// distinct explanations would call for three different real fixes, and
// they're distinguishable from here:
//   (a) genuine raw disk throughput - each call's own duration scales with
//       its size at a roughly constant, plausible-for-the-drive rate. Fix
//       would have to reduce total bytes moved (better compression, less
//       redundant streaming) - nothing hookable from outside changes this.
//   (b) many small calls, each fast on its own, but so numerous that their
//       SUM stalls a frame - a batching opportunity (fewer, larger reads,
//       or async I/O overlapping with rendering) would be the real fix.
//   (c) some INDIVIDUAL calls are anomalously slow for their size (latency
//       not explained by volume) - points at contention/queuing (antivirus,
//       disk queue depth, or the file being read scattered/non-sequential)
//       rather than raw transfer cost.
// Per-call duration (not just aggregate bytes/sec) distinguishes all three
// directly. File path is resolved (rate-limited to the slow-call path only,
// so the extra syscall never touches the hot path) to see whether this is
// one archive being read via many small sequential calls, or many distinct
// files/assets.
#define MAX_READFILE_PATH_LOG 40
static volatile LONG g_readFilePathLogCount = 0;
static volatile LONG g_readFileOverlappedCount = 0;
static volatile LONG g_readFileSyncCount = 0;

static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                                  LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    // Never pace the main thread: if it ever reads synchronously, delaying it
    // is delaying the frame - the exact thing being fixed.
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    if (!isMain) {
        PaceRead(nNumberOfBytesToRead);
    }

    if (lpOverlapped) InterlockedIncrement(&g_readFileOverlappedCount);
    else InterlockedIncrement(&g_readFileSyncCount);

    unsigned __int64 t0 = __rdtsc();
    BOOL ok = g_realReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    unsigned __int64 elapsedCycles = __rdtsc() - t0;

    // Census only - the kernel just wrote this buffer, so warming it would be
    // pure waste. What matters is the volume and the owning thread.
    DWORD got = (ok && lpNumberOfBytesRead) ? *lpNumberOfBytesRead : nNumberOfBytesToRead;
    CensusRecord(ALLOC_SRC_READFILE, got);
    InterlockedIncrement(&g_readFileCount);
    g_readFileBytes += (LONGLONG)got;

    if (g_cyclesPerUsec > 0.0 && got > 0) {
        LONG usec = (LONG)((double)elapsedCycles / g_cyclesPerUsec);
        double mbPerSec = (usec > 0) ? ((double)got / 1048576.0) / ((double)usec / 1000000.0) : 0.0;
        // Threshold picked well below what even a slow HDD should take for a
        // typical ~87KB streaming read - anything crossing it either moved
        // an unusually large amount in one call, or was anomalously slow for
        // its size (case (c) above), both worth a closer look either way.
        if (usec > 3000) {
            char path[MAX_PATH] = "?";
            DWORD pathLen = GetFinalPathNameByHandleA(hFile, path, MAX_PATH, FILE_NAME_NORMALIZED);
            const char *shortPath = path;
            if (pathLen > 0 && pathLen < MAX_PATH) {
                // GetFinalPathNameByHandleA prefixes "\\?\" or "\\?\UNC\" -
                // strip it so the log reads as a normal path.
                if (strncmp(path, "\\\\?\\", 4) == 0) shortPath = path + 4;
            }
            DWORD tid = GetCurrentThreadId();
            const char *name = isMain ? "MAIN" : "?";
            if (!isMain) {
                EnterCriticalSection(&g_threadNameLock);
                for (LONG j = 0; j < g_threadNameCount; j++) {
                    if (g_threadNames[j].threadId == tid) { name = g_threadNames[j].name; break; }
                }
                LeaveCriticalSection(&g_threadNameLock);
            }
            LONG n = InterlockedIncrement(&g_readFilePathLogCount);
            if (n <= MAX_READFILE_PATH_LOG) {
                char line[512];
                sprintf(line, "[readfile] SLOW %lu us for %lu bytes (%.1f MB/s) thread=%lu(%s) overlapped=%d path=%s",
                        (unsigned long)usec, (unsigned long)got, mbPerSec, tid, name,
                        lpOverlapped != NULL, shortPath);
                LogLine(line);
            }
        }
    }
    return ok;
}

static int InstallAllocTrackingHooks(void)
{
    g_dispatchTrackTls = TlsAlloc();
    g_censusTls = TlsAlloc();
    InitializeCriticalSection(&g_allocThreadLock);
    InitializeCriticalSection(&g_allocImplLock);
    InitializeCriticalSection(&g_warmLock);
    InitializeCriticalSection(&g_readPaceLock);
#if ENABLE_ALLOCATOR_WARM
    CreateThread(NULL, 0, WarmerThread, NULL, 0, NULL);
#endif

    HMODULE hMain = GetModuleHandleA(NULL);

    void *realHeapAlloc = PatchIat(hMain, "KERNEL32.dll", "HeapAlloc", (void *)HookedHeapAlloc);
    void *realVirtualAlloc = PatchIat(hMain, "KERNEL32.dll", "VirtualAlloc", (void *)HookedVirtualAlloc);
    void *realMapView = PatchIat(hMain, "KERNEL32.dll", "MapViewOfFile", (void *)HookedMapViewOfFile);
    void *realReadFile = PatchIat(hMain, "KERNEL32.dll", "ReadFile", (void *)HookedReadFile);

    if (realHeapAlloc) g_realHeapAlloc = (PFN_HeapAlloc)realHeapAlloc;
    if (realVirtualAlloc) g_realVirtualAlloc = (PFN_VirtualAlloc)realVirtualAlloc;
    if (realMapView) g_realMapViewOfFile = (PFN_MapViewOfFile)realMapView;
    if (realReadFile) g_realReadFile = (PFN_ReadFile)realReadFile;

    char line[192];
    sprintf(line, "Census IAT hooks: HeapAlloc=%d VirtualAlloc=%d MapViewOfFile=%d ReadFile=%d",
            realHeapAlloc != NULL, realVirtualAlloc != NULL, realMapView != NULL, realReadFile != NULL);
    LogLine(line);

    return (realHeapAlloc != NULL) && (realVirtualAlloc != NULL);
}

// ---- Allocation census + background memory warmer (v6) --------------------
// Fix attempt 5 recorded only allocations made *inside* a loader dispatch, on
// the dispatching thread (~105 allocs / ~2.3MB in a burst - far too little to
// be a whole town chunk's worth of actor/skeleton/animation data). Two
// independent problems with that, both addressed here:
//
//  1. COVERAGE. If the memory that goes cold is allocated on a job worker, or
//     on the main thread at actor-instantiation time - anywhere other than
//     inside FUN_004b5cb0 on the loader thread - attempt 5 never saw it at
//     all. The census below records EVERY call through the engine's named-
//     heap allocator, attributed per-thread (thread names already captured
//     via the RaiseException hook), so the log answers directly: which thread
//     allocates the bulk of the bytes, and in which 500ms window. It also
//     records the distinct concrete allocator implementations reached through
//     the heap object's vtable, which is what makes it possible to go find
//     *sibling* wrappers (other allocation paths) statically in Ghidra
//     afterwards - the callers of those implementations are the complete set.
//
//  2. WARMING METHOD. `_mm_prefetch` is a *hint*: the CPU drops prefetches
//     freely once the fill buffers / outstanding-miss slots are saturated,
//     and a tight back-to-back loop over megabytes saturates them almost
//     immediately - so most of attempt 5's "prefetch" very likely never
//     issued a memory request at all, which would explain a mechanism that
//     was confirmed to *run* yet changed nothing. Real (volatile) loads
//     cannot be dropped: each one either hits or stalls until the line
//     actually arrives. Warming now runs on a dedicated BELOW_NORMAL thread
//     draining a queue, so that stall cost lands off the critical path
//     instead of inside the loader dispatch.
//
// Reading memory the engine may have freed in the meantime is guarded by SEH
// on our own thread (the engine's pools are pre-reserved and pre-committed -
// HeapAlloc/VirtualAlloc were confirmed never to fire - so a fault here is
// not an expected path, but the guard costs nothing on a non-critical thread).

// v7: the census is per-thread AND per-source. v6 measured only the engine's
// named-heap wrapper and found it is a main-thread small-object allocator
// (~300 bytes average, ~1.4MB per 500ms, 99.7% of it on the main thread) -
// far too little, and on the wrong thread, to be the multi-megabyte cold
// chunk data. The bulk allocation path is therefore something else, and
// these are the candidates the exe actually imports: HeapAlloc, VirtualAlloc,
// CreateFileMappingA/MapViewOfFile, ReadFile.
static const char *g_allocSrcNames[NUM_ALLOC_SRC] = {
    "namedheap", "HeapAlloc", "VirtualAlloc", "MapViewOfFile", "ReadFile"
};

#define MAX_ALLOC_THREADS 64
typedef struct {
    volatile LONG threadId; // 0 = unclaimed
    volatile LONG count[NUM_ALLOC_SRC];
    volatile LONGLONG bytes[NUM_ALLOC_SRC];
} AllocThreadSlot;
static AllocThreadSlot g_allocThreads[MAX_ALLOC_THREADS];
static CRITICAL_SECTION g_allocThreadLock;
static DWORD g_censusTls;

static AllocThreadSlot *ClaimAllocSlot(DWORD tid)
{
    EnterCriticalSection(&g_allocThreadLock);
    AllocThreadSlot *slot = NULL;
    for (int i = 0; i < MAX_ALLOC_THREADS; i++) {
        if (g_allocThreads[i].threadId == (LONG)tid) { slot = &g_allocThreads[i]; break; }
        if (slot == NULL && g_allocThreads[i].threadId == 0) slot = &g_allocThreads[i];
    }
    if (slot && slot->threadId == 0) slot->threadId = (LONG)tid;
    LeaveCriticalSection(&g_allocThreadLock);
    return slot;
}

static AllocThreadSlot *GetCensusSlot(void)
{
    AllocThreadSlot *s = (AllocThreadSlot *)TlsGetValue(g_censusTls);
    if (!s) {
        s = ClaimAllocSlot(GetCurrentThreadId());
        if (s) TlsSetValue(g_censusTls, s);
    }
    return s;
}

// Per-thread single-writer counters; only the monitor thread reads them.
static void CensusRecord(int src, size_t bytes)
{
    AllocThreadSlot *s = GetCensusSlot();
    if (!s) return;
    s->count[src]++;
    s->bytes[src] += (LONGLONG)bytes;
}

// Distinct concrete allocator implementations behind FUN_00b454a0's virtual
// dispatch. Layout confirmed from its disassembly:
//   heap   = *(void**)this        (MOV ECX,[ESI])
//   vtable = *(void**)heap        (MOV EAX,[ECX])
//   allocFn= ((void**)vtable)[1]  (MOV EAX,[EAX+4])
#define MAX_ALLOC_IMPLS 16
typedef struct {
    void *vtable;
    void *allocFn;
    void *exampleHeap;
    volatile LONG count;
    volatile LONG logged;
} AllocImplEntry;
static AllocImplEntry g_allocImpls[MAX_ALLOC_IMPLS];
static volatile LONG g_allocImplCount = 0;
static CRITICAL_SECTION g_allocImplLock;

static void NoteAllocImpl(void *thisPtr)
{
    if (!thisPtr) return;
    void *heap = *(void **)thisPtr;
    if (!heap) return;
    void *vt = *(void **)heap;
    if (!vt) return;
    void *fn = ((void **)vt)[1];

    LONG n = g_allocImplCount;
    for (LONG i = 0; i < n; i++) {
        if (g_allocImpls[i].vtable == vt && g_allocImpls[i].allocFn == fn) {
            InterlockedIncrement(&g_allocImpls[i].count);
            return;
        }
    }
    EnterCriticalSection(&g_allocImplLock);
    n = g_allocImplCount;
    LONG found = -1;
    for (LONG i = 0; i < n; i++) {
        if (g_allocImpls[i].vtable == vt && g_allocImpls[i].allocFn == fn) { found = i; break; }
    }
    if (found < 0 && n < MAX_ALLOC_IMPLS) {
        g_allocImpls[n].vtable = vt;
        g_allocImpls[n].allocFn = fn;
        g_allocImpls[n].exampleHeap = heap;
        found = n;
        g_allocImplCount = n + 1;
    }
    LeaveCriticalSection(&g_allocImplLock);
    if (found >= 0) InterlockedIncrement(&g_allocImpls[found].count);
}

// Bounded queue, plain critical section. Only allocations >= WARM_MIN_SIZE
// are enqueued, so the rate here is orders of magnitude below the raw
// allocator call rate - a lock is entirely adequate and much easier to reason
// about than a lock-free ring with a wrap-vs-drop desync hazard.
#define WARM_QUEUE_SIZE 8192
#define WARM_MIN_SIZE 4096
#define WARM_MAX_SIZE (16 * 1024 * 1024)

typedef struct {
    void *ptr;
    size_t size;
} WarmEntry;
static WarmEntry g_warmQueue[WARM_QUEUE_SIZE];
static int g_warmHead = 0;
static int g_warmCount = 0;
static CRITICAL_SECTION g_warmLock;

static volatile LONG g_warmEnqueued = 0;
static volatile LONG g_warmDropped = 0;
static volatile LONG g_warmDone = 0;
static volatile LONGLONG g_warmBytes = 0;
static volatile LONG g_warmSink = 0;

static void EnqueueWarm(void *ptr, size_t size)
{
    if (!ptr || size < WARM_MIN_SIZE || size > WARM_MAX_SIZE) return;
    EnterCriticalSection(&g_warmLock);
    if (g_warmCount < WARM_QUEUE_SIZE) {
        int idx = (g_warmHead + g_warmCount) % WARM_QUEUE_SIZE;
        g_warmQueue[idx].ptr = ptr;
        g_warmQueue[idx].size = size;
        g_warmCount++;
        InterlockedIncrement(&g_warmEnqueued);
    } else {
        InterlockedIncrement(&g_warmDropped);
    }
    LeaveCriticalSection(&g_warmLock);
}

static int DequeueWarm(WarmEntry *out)
{
    int got = 0;
    EnterCriticalSection(&g_warmLock);
    if (g_warmCount > 0) {
        *out = g_warmQueue[g_warmHead];
        g_warmHead = (g_warmHead + 1) % WARM_QUEUE_SIZE;
        g_warmCount--;
        got = 1;
    }
    LeaveCriticalSection(&g_warmLock);
    return got;
}

static DWORD WINAPI WarmerThread(LPVOID param)
{
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WarmEntry e;
        if (!DequeueWarm(&e)) {
            Sleep(1);
            continue;
        }
        LONG acc = 0;
        __try {
            volatile const unsigned char *p = (volatile const unsigned char *)e.ptr;
            for (size_t off = 0; off < e.size; off += 64) {
                acc += p[off];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            acc = 0;
        }
        g_warmSink += acc; // keeps the loads from being optimized away
        InterlockedIncrement(&g_warmDone);
        g_warmBytes += (LONGLONG)e.size;
    }
}

// ---- Engine's own named-heap allocator hook (FUN_00b454a0) ----------------
// The HeapAlloc/VirtualAlloc tracking above caught nothing across an entire
// session - confirmed the engine's per-object allocations never reach the
// OS allocator directly. FUN_00b454a0 is the actual layer: a __thiscall
// wrapper (EnterCriticalSection -> virtual "Allocate" call on a named heap
// object -> LeaveCriticalSection) already found and decompiled earlier in
// this investigation. Its own prologue has no SEH scaffolding (verified,
// unlike FUN_004b5cb0/FUN_00a01a00/FUN_00a015b0), so return-address
// hijacking - the same technique used for FUN_00aacf10 etc. - is safe here.
//
// Unlike every other function hijacked this way in this investigation,
// THIS one's return value is real and load-bearing: verified via raw
// disassembly (`MOV EAX,EBX` immediately before `RET 0x8`) that it
// genuinely returns the allocated pointer via EAX, despite Ghidra
// inferring a void signature (the same class of decompiler gap caught
// twice earlier in this investigation - trust disassembly, not the
// decompiled C, when it matters). 923 call sites across the engine depend
// on that value. The hook must preserve EAX across its own bookkeeping
// call and hand back the REAL pointer, not discard it like the
// confirmed-void functions instrumented earlier.
//
// Confirmed via disassembly: __thiscall(this=ECX, param_2=requested size,
// param_3=tag/context), RET 0x8 (cleans the 2 stack args itself - hijacking
// doesn't care, we never touch how it returns, only where).

#define ALLOCATOR_RVA (0x00b454a0 - 0x00400000)
#define ALLOCATOR_PATCH_LEN 9 // PUSH EBP; MOV EBP,ESP; SUB ESP,0x40c

static void *g_allocatorTarget = NULL;
static void *g_allocatorTrampoline = NULL;

// Digging into cause 2, continued: per-call ReadFile duration ruled out raw
// disk speed and per-call I/O contention (only 2 calls all session exceeded
// 3ms, both plain large sequential reads at 4000+ MB/s). The 101ms stutter
// captured earlier can't be ReadFile's own time - 272 calls at that drive's
// real speed total ~5ms, not 100ms - yet its own watchdog capture landed
// mid-way through THIS allocator (EIP inside FUN_00b454a0) with 6070
// allocations in that one frame. This allocator takes a critical section on
// every call (confirmed via disassembly when this hook was first built:
// EnterCriticalSection -> virtual Allocate -> LeaveCriticalSection). 6070
// calls in one frame is a plausible lock-contention cost in its own right -
// timing each call directly tests it, especially whether the MAIN thread's
// own calls specifically slow down while a Loader/decode thread is flooding
// the same lock during a burst.
static volatile LONGLONG g_allocDurSumUsec[2]; // [0]=main thread, [1]=all other threads
static volatile LONG g_allocDurCount[2];
static volatile LONG g_allocDurMaxUsec[2];

#define ALLOC_STACK_DEPTH 16
typedef struct {
    void *trueRetAddr;
    size_t requestedSize;
    unsigned __int64 entryTsc;
} AllocRetFrame;
typedef struct {
    int top;
    DWORD tid;              // cached, so the hot path never re-queries it
    AllocThreadSlot *slot;  // this thread's census slot, claimed once
    AllocRetFrame frames[ALLOC_STACK_DEPTH];
} AllocThreadStack;

static DWORD g_allocStackTls;

static AllocThreadStack *GetAllocThreadStack(void)
{
    AllocThreadStack *ts = (AllocThreadStack *)TlsGetValue(g_allocStackTls);
    if (!ts) {
        ts = (AllocThreadStack *)calloc(1, sizeof(AllocThreadStack));
        if (ts) TlsSetValue(g_allocStackTls, ts);
    }
    return ts;
}

static int __cdecl OnAllocatorEnter(void *trueRetAddr, size_t requestedSize, void *thisPtr)
{
    NoteAllocImpl(thisPtr);
    AllocThreadStack *ts = GetAllocThreadStack();
    if (!ts || ts->top >= ALLOC_STACK_DEPTH) return 0;
    ts->frames[ts->top].trueRetAddr = trueRetAddr;
    ts->frames[ts->top].requestedSize = requestedSize;
    ts->frames[ts->top].entryTsc = __rdtsc();
    ts->top++;
    return 1;
}

// Structurally only ever called for a call OnAllocatorEnter successfully
// recorded (entry hijack is conditional on its return value), so
// ts->top > 0 is guaranteed - same invariant as every other return-hijack
// hook in this investigation.
static void *__cdecl OnAllocatorReturn(void *resultPtr)
{
    AllocThreadStack *ts = GetAllocThreadStack();
    ts->top--;
    AllocRetFrame f = ts->frames[ts->top];
    RecordAllocIfTracking(resultPtr, f.requestedSize);

    if (resultPtr && f.requestedSize) {
        if (!ts->tid) ts->tid = GetCurrentThreadId();
        CensusRecord(ALLOC_SRC_NAMEDHEAP, f.requestedSize);
        InterlockedIncrement(&g_namedHeapCount);

        if (g_cyclesPerUsec > 0.0) {
            LONG usec = (LONG)((double)(__rdtsc() - f.entryTsc) / g_cyclesPerUsec);
            int bucket = ((LONG)ts->tid == g_mainThreadId) ? 0 : 1;
            InterlockedIncrement(&g_allocDurCount[bucket]);
            InterlockedExchangeAdd64(&g_allocDurSumUsec[bucket], usec);
            if (usec > g_allocDurMaxUsec[bucket]) g_allocDurMaxUsec[bucket] = usec;
            // Lock contention shows up as an INDIVIDUAL call taking far
            // longer than the allocation work itself could justify - a plain
            // heap allocation is normally low-single-digit microseconds even
            // under load, so anything crossing 1ms is the lock, not the work.
            if (usec > 1000) {
                char line[128];
                sprintf(line, "[allocator] SLOW %ld us on thread %lu (%s) size=%zu",
                        usec, (unsigned long)ts->tid,
                        bucket == 0 ? "MAIN" : "other", f.requestedSize);
                LogLine(line);
            }
        }
        // Never warm the main thread's own allocations: it is about to touch
        // that memory itself anyway, so warming it would only add bandwidth
        // contention on the exact thread whose frame time we're trying to
        // protect. (Main thread identified as the one calling the frame
        // pacer FUN_00ac3040, which is confirmed once-per-frame/main-only.)
#if ENABLE_ALLOCATOR_WARM
        if (g_allocatorWarmEnabled && (LONG)ts->tid != g_mainThreadId) {
            EnqueueWarm(resultPtr, f.requestedSize);
        }
#endif
    }
    return f.trueRetAddr;
}

// Preserves EAX (the real allocated pointer) across the OnAllocatorReturn
// call - unlike the void-returning hooks elsewhere in this file, this
// value is load-bearing for the 923 real callers of FUN_00b454a0.
__declspec(naked) void OnAllocatorReturnStub(void)
{
    __asm {
        push eax
        push eax
        call OnAllocatorReturn
        mov ecx, eax          ; true return address, stashed in scratch reg
        add esp, 4
        pop eax                ; restore the REAL allocated pointer
        jmp ecx
    }
}

__declspec(naked) void Detour_Allocator(void)
{
    __asm {
        push ecx                       ; save 'this'  -> [esp]=this [esp+4]=ret [esp+8]=size
        push dword ptr [esp]           ; arg3: thisPtr
        push dword ptr [esp + 12]      ; arg2: requestedSize
        push dword ptr [esp + 12]      ; arg1: trueRetAddr
        call OnAllocatorEnter
        add esp, 12
        test eax, eax
        jz skip_allocator_hijack
        mov dword ptr [esp + 4], offset OnAllocatorReturnStub
    skip_allocator_hijack:
        pop ecx                        ; restore 'this'
        jmp dword ptr [g_allocatorTrampoline]
    }
}

static int InstallAllocatorHook(void)
{
    // Census/warmer state is initialised in InstallAllocTrackingHooks, which
    // runs first - it must, since the OS-level hooks it installs can start
    // feeding the census immediately.
    g_allocStackTls = TlsAlloc();

    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    g_allocatorTarget = base + ALLOCATOR_RVA;

    unsigned char saved[ALLOCATOR_PATCH_LEN];
    memcpy(saved, g_allocatorTarget, ALLOCATOR_PATCH_LEN);

    unsigned char *tramp = (unsigned char *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;

    memcpy(tramp, saved, ALLOCATOR_PATCH_LEN);
    tramp[ALLOCATOR_PATCH_LEN] = 0xE9;
    *(int *)(tramp + ALLOCATOR_PATCH_LEN + 1) =
        (int)((unsigned char *)g_allocatorTarget + ALLOCATOR_PATCH_LEN) -
        (int)(tramp + ALLOCATOR_PATCH_LEN + 5);
    g_allocatorTrampoline = tramp;

    DWORD oldProtect;
    if (!VirtualProtect(g_allocatorTarget, ALLOCATOR_PATCH_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;

    unsigned char *t = (unsigned char *)g_allocatorTarget;
    t[0] = 0xE9;
    *(int *)(t + 1) = (int)(void *)Detour_Allocator - (int)(t + 5);
    for (int i = 5; i < ALLOCATOR_PATCH_LEN; i++) t[i] = 0x90;

    VirtualProtect(g_allocatorTarget, ALLOCATOR_PATCH_LEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), g_allocatorTarget, ALLOCATOR_PATCH_LEN);
    return 1;
}

__declspec(naked) void LoaderDispatchWrapper(void)
{
    __asm {
        push ecx
        call AcquireLoaderThrottle
        pop ecx
        push ecx
        call BeginDispatchTracking
        pop ecx
        call dword ptr [g_realLoaderDispatch]
        call EndDispatchTrackingAndPrefetch
        call ReleaseLoaderThrottle
        ret
    }
}

static int InstallLoaderThrottle(void)
{
    g_loaderThrottleAcquiredTls = TlsAlloc();
    g_loaderThrottleSem = CreateSemaphoreA(NULL, LOADER_THROTTLE_MAX, LOADER_THROTTLE_MAX, NULL);
    if (!g_loaderThrottleSem) return 0;

    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    g_realLoaderDispatch = base + LOADER_DISPATCH_FUNC_RVA;

    unsigned char *callSite = base + LOADER_DISPATCH_CALL_SITE_RVA;
    DWORD oldProtect;
    if (!VirtualProtect(callSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;
    callSite[0] = 0xE8; // CALL rel32 - same opcode as the original instruction, only the target changes
    *(int *)(callSite + 1) = (int)(void *)LoaderDispatchWrapper - (int)(callSite + 5);
    VirtualProtect(callSite, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), callSite, 5);
    return 1;
}

// ---- WaitForSingleObject IAT hook -----------------------------------------
// Deliberately a completely different, much safer technique than the
// inline hooks below: patches the game's Import Address Table entry for
// WaitForSingleObject (kernel32.dll) to point at our own normal C wrapper,
// which calls straight through to the real function and returns its
// result normally. No naked asm, no return-address manipulation, no SEH
// risk - this is the standard, decades-proven-safe way to instrument a
// well-defined WINAPI call. Tests the job-system-synchronization
// hypothesis directly (the earlier CSwitch capture found WrDispatchInt/
// WrPreempted wait reasons appearing exclusively during the bad-frame
// cluster) without touching any internal game code at all.

typedef DWORD(WINAPI *PFN_WaitForSingleObject)(HANDLE, DWORD);
static PFN_WaitForSingleObject g_realWaitForSingleObject = NULL;

static volatile LONG g_wfsoCallCount = 0;
static volatile LONGLONG g_wfsoSumCycles = 0;

// Per-thread breakdown: fixed slot table, linear scan to find-or-claim a
// slot for the calling thread ID. Small, bounded number of distinct
// threads actually call this (job system + a handful of others), so a
// simple table is fine - protected by a critical section only for the
// rare "claim a new slot" path; the common "increment existing slot"
// path uses interlocked ops on that slot's own counters, no locking.
#define MAX_WFSO_THREADS 64
typedef struct {
    volatile LONG threadId; // 0 = unclaimed
    volatile LONG callCount;
    volatile LONGLONG sumCycles;
} WfsoThreadSlot;
static WfsoThreadSlot g_wfsoThreads[MAX_WFSO_THREADS];
static CRITICAL_SECTION g_wfsoThreadTableLock;

static WfsoThreadSlot *GetOrClaimWfsoSlot(DWORD tid)
{
    for (int i = 0; i < MAX_WFSO_THREADS; i++) {
        if (g_wfsoThreads[i].threadId == (LONG)tid) return &g_wfsoThreads[i];
    }
    EnterCriticalSection(&g_wfsoThreadTableLock);
    // re-check under lock in case another thread claimed a slot for us
    // (can't happen for our own tid, but a fresh empty slot search must
    // be re-done safely)
    WfsoThreadSlot *slot = NULL;
    for (int i = 0; i < MAX_WFSO_THREADS; i++) {
        if (g_wfsoThreads[i].threadId == (LONG)tid) { slot = &g_wfsoThreads[i]; break; }
        if (slot == NULL && g_wfsoThreads[i].threadId == 0) slot = &g_wfsoThreads[i];
    }
    if (slot && slot->threadId == 0) {
        slot->threadId = (LONG)tid;
    }
    LeaveCriticalSection(&g_wfsoThreadTableLock);
    return slot;
}

static DWORD WINAPI HookedWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    LONG mainStart = 0;
    if (isMain) {
        mainStart = NowUsec();
        g_mainWfsoHandle = (LONG)(ULONG_PTR)hHandle;
        g_mainWfsoStartUsec = mainStart ? mainStart : 1;
    }
    unsigned __int64 t0 = __rdtsc();
    DWORD result = g_realWaitForSingleObject(hHandle, dwMilliseconds);
    unsigned __int64 elapsed = __rdtsc() - t0;
    if (isMain) {
        LONG waited = NowUsec() - mainStart;
        g_mainWfsoStartUsec = 0;
        if (waited > 0) InterlockedExchangeAdd(&g_mainWfsoWaitTotalUsec, waited);
    }
    InterlockedIncrement(&g_wfsoCallCount);
    g_wfsoSumCycles += (LONGLONG)elapsed;

    WfsoThreadSlot *slot = GetOrClaimWfsoSlot(GetCurrentThreadId());
    if (slot) {
        InterlockedIncrement(&slot->callCount);
        slot->sumCycles += (LONGLONG)elapsed;
    }
    return result;
}

// Finds the IAT slot for {moduleName}!{funcName} in the given module's
// import table and overwrites it with newFunc. Returns the original
// function pointer (needed so the wrapper can call through), or NULL on
// failure (leaves everything untouched).
static void *PatchIat(HMODULE hostModule, const char *moduleName, const char *funcName, void *newFunc)
{
    unsigned char *base = (unsigned char *)hostModule;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0) return NULL;

    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + importDir.VirtualAddress);
    for (; imp->Name != 0; imp++) {
        const char *thisModuleName = (const char *)(base + imp->Name);
        if (_stricmp(thisModuleName, moduleName) != 0) continue;

        IMAGE_THUNK_DATA *origThunk = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *iatThunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; origThunk++, iatThunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *byName = (IMAGE_IMPORT_BY_NAME *)(base + origThunk->u1.AddressOfData);
            if (strcmp((const char *)byName->Name, funcName) != 0) continue;

            void *original = (void *)iatThunk->u1.Function;
            DWORD oldProtect;
            if (!VirtualProtect(&iatThunk->u1.Function, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
                return NULL;
            }
            iatThunk->u1.Function = (ULONG_PTR)newFunc;
            VirtualProtect(&iatThunk->u1.Function, sizeof(void *), oldProtect, &oldProtect);
            return original;
        }
    }
    return NULL;
}

// ---- RaiseException IAT hook (thread-name capture) ------------------------
// Same safe technique as WaitForSingleObject: a plain C wrapper, no asm.
// Captures the classic MSVC thread-naming convention (RaiseException with
// code 0x406D1388, already confirmed via static analysis that this engine
// uses named worker threads: "Loader", "GameInitializer", "Trophy",
// "GameUpdate", "CDevCommon Task%d") so the mystery thread IDs showing up
// in the WaitForSingleObject breakdown can be identified. Safe to insert a
// plain function call between the caller's __try and the real
// RaiseException: SEH handler lookup walks the exception-registration
// chain via FS:[0], not literal call-stack depth, so an extra ordinary
// (non-SEH-establishing) frame in between changes nothing.

typedef void(WINAPI *PFN_RaiseException)(DWORD, DWORD, DWORD, const ULONG_PTR *);
static PFN_RaiseException g_realRaiseException = NULL;

// g_threadNames / g_threadNameCount / g_threadNameLock forward-declared near
// the top of the file (needed by the ReadFile diagnostics, which run earlier
// in file order) - C merges these into the same objects.

// Fix attempt: lower the priority of threads specifically named "Loader"
// (confirmed via live thread-name capture to be the ones blocking for
// tens to hundreds of milliseconds - one for 1.66s - in the window right
// before the CPU-side stutter). This doesn't change what work happens,
// only the OS scheduler's preference when a Loader thread and a
// frame-critical job-system thread both want the CPU at the same time -
// standard, fully reversible Windows API, cannot itself cause a crash
// (worst case: loading takes a little longer, invisible to the player,
// vs. a visible frame hitch).
// Shares g_loaderThrottleEnabled with the dispatch semaphore (same original
// fix attempt, bundled). Note this only gates NEWLY-named threads going
// forward - a thread's priority isn't retroactively restored if toggled off
// after it was already lowered, same "takes effect going forward, not
// retroactively" caveat as the other fixes in this session.
static void MaybeLowerLoaderPriority(DWORD tid, const char *name, BOOL isCurrentThread)
{
    if (!g_loaderThrottleEnabled) return;
    if (strcmp(name, "Loader") != 0) return;
    HANDLE hThread = isCurrentThread ? GetCurrentThread() : OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
    if (!hThread) return;
    BOOL ok = SetThreadPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL);
    char line[128];
    sprintf(line, "[priority] Lowered '%s' (tid=%lu) to BELOW_NORMAL: %s", name, tid, ok ? "OK" : "FAILED");
    LogLine(line);
    if (!isCurrentThread) CloseHandle(hThread);
}

static void WINAPI HookedRaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                                         DWORD nNumberOfArguments, const ULONG_PTR *lpArguments)
{
    if (dwExceptionCode == 0x406D1388 && nNumberOfArguments >= 3 && lpArguments != NULL) {
        DWORD type = (DWORD)lpArguments[0];
        const char *name = (const char *)lpArguments[1];
        DWORD tid = (DWORD)lpArguments[2];
        if (type == 0x1000 && name != NULL) {
            BOOL isCurrentThread = (tid == (DWORD)-1);
            DWORD resolvedTid = isCurrentThread ? GetCurrentThreadId() : tid;
            EnterCriticalSection(&g_threadNameLock);
            if (g_threadNameCount < MAX_THREAD_NAMES) {
                ThreadNameEntry *e = &g_threadNames[g_threadNameCount++];
                e->threadId = resolvedTid;
                strncpy(e->name, name, sizeof(e->name) - 1);
                e->name[sizeof(e->name) - 1] = 0;
                char line[128];
                sprintf(line, "[threadname] tid=%lu name=%s", e->threadId, e->name);
                LogLine(line);
            }
            LeaveCriticalSection(&g_threadNameLock);
            MaybeLowerLoaderPriority(resolvedTid, name, isCurrentThread);
        }
    }
    g_realRaiseException(dwExceptionCode, dwExceptionFlags, nNumberOfArguments, lpArguments);
}

static int InstallRaiseExceptionHook(void)
{
    InitializeCriticalSection(&g_threadNameLock);
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "RaiseException", (void *)HookedRaiseException);
    if (!original) return 0;
    g_realRaiseException = (PFN_RaiseException)original;
    return 1;
}

// ---- SetThreadIdealProcessor IAT hook -------------------------------------
// Same safe plain-C IAT-patch technique. Confirmed imported by the exe
// (checked the actual import table directly). Tests whether Loader
// threads and job-worker threads are being assigned overlapping/adjacent
// ideal processors - if so, that's a concrete, directly fixable
// contention mechanism (redirect via the same hook, once confirmed).

typedef DWORD(WINAPI *PFN_SetThreadIdealProcessor)(HANDLE, DWORD);
static PFN_SetThreadIdealProcessor g_realSetThreadIdealProcessor = NULL;
typedef DWORD(WINAPI *PFN_GetThreadId)(HANDLE);
static PFN_GetThreadId g_pGetThreadId = NULL;

static DWORD WINAPI HookedSetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor)
{
    DWORD result = g_realSetThreadIdealProcessor(hThread, dwIdealProcessor);
    DWORD tid = g_pGetThreadId ? g_pGetThreadId(hThread) : 0;
    const char *name = "?";
    EnterCriticalSection(&g_threadNameLock);
    for (LONG i = 0; i < g_threadNameCount; i++) {
        if (g_threadNames[i].threadId == tid) { name = g_threadNames[i].name; break; }
    }
    char line[192];
    sprintf(line, "[idealproc] tid=%lu name=%s requested_ideal_cpu=%lu (prev=%lu)",
            tid, name, dwIdealProcessor, result);
    LeaveCriticalSection(&g_threadNameLock);
    LogLine(line);
    return result;
}

static int InstallIdealProcessorHook(void)
{
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        g_pGetThreadId = (PFN_GetThreadId)GetProcAddress(hKernel32, "GetThreadId");
    }
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "SetThreadIdealProcessor", (void *)HookedSetThreadIdealProcessor);
    if (!original) return 0;
    g_realSetThreadIdealProcessor = (PFN_SetThreadIdealProcessor)original;
    return 1;
}

// ---- EnterCriticalSection contention hook (v10) --------------------------
// 96% of watchdog captures found the main thread blocked inside a system DLL
// rather than executing game code, so the stutter is a WAIT. The exe imports
// EnterCriticalSection, and a lock held by a streaming/decompression thread
// while the main thread wants it produces exactly the observed shape: an
// isolated multi-tens-of-ms frame with normal CPU work either side.
//
// EnterCriticalSection is far too hot to instrument naively. This wrapper
// calls TryEnterCriticalSection FIRST: on the uncontended path (the
// overwhelming majority) that succeeds and returns immediately, costing one
// extra call and no bookkeeping. Only when it FAILS - i.e. an actual
// contended wait, which is the only case of interest - does it record the
// lock, its current owner, and the wait duration before blocking for real.
// Semantics are identical: a successful TryEnterCriticalSection leaves the
// caller owning the lock exactly once, same as EnterCriticalSection.
typedef void(WINAPI *PFN_EnterCriticalSection)(LPCRITICAL_SECTION);
static PFN_EnterCriticalSection g_realEnterCriticalSection = NULL;

static void WINAPI HookedEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (TryEnterCriticalSection(cs)) return;

    if ((LONG)GetCurrentThreadId() != g_mainThreadId) {
        g_realEnterCriticalSection(cs);
        return;
    }

    // Main thread, genuinely contended: this is the event being hunted.
    LONG start = NowUsec();
    g_mainCsPtr = (LONG)(ULONG_PTR)cs;
    g_mainCsOwner = (LONG)(ULONG_PTR)cs->OwningThread;
    g_mainCsWaitStartUsec = start ? start : 1;
    g_realEnterCriticalSection(cs);
    LONG waited = NowUsec() - start;
    g_mainCsWaitStartUsec = 0;
    if (waited > 0) {
        InterlockedExchangeAdd(&g_mainCsWaitTotalUsec, waited);
        InterlockedIncrement(&g_mainCsWaitCount);
    }
}

static int InstallCriticalSectionHook(void)
{
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "EnterCriticalSection", (void *)HookedEnterCriticalSection);
    if (!original) return 0;
    g_realEnterCriticalSection = (PFN_EnterCriticalSection)original;
    return 1;
}

static int InstallWfsoHook(void)
{
    InitializeCriticalSection(&g_wfsoThreadTableLock);
    HMODULE hMain = GetModuleHandleA(NULL);
    void *original = PatchIat(hMain, "KERNEL32.dll", "WaitForSingleObject", (void *)HookedWaitForSingleObject);
    if (!original) return 0;
    g_realWaitForSingleObject = (PFN_WaitForSingleObject)original;
    return 1;
}

// Turns a runtime address into something directly usable: game-module
// addresses become Ghidra VAs (base-relative + 0x00400000), everything else
// becomes "module.dll+RVA". Resolving system/driver DLLs by NAME is the whole
// point - the watchdog found the main thread inside one 96% of the time, and
// a bare relocated address says nothing about which one.
static void DescribeAddr(unsigned int addr, char *out)
{
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hm) && hm) {
        if ((unsigned int)hm == g_mainModBase) {
            sprintf(out, "%08X", addr - g_mainModBase + 0x00400000);
            return;
        }
        char path[MAX_PATH];
        path[0] = 0;
        GetModuleFileNameA(hm, path, MAX_PATH);
        const char *bn = strrchr(path, 92); // 92 = backslash
        bn = bn ? bn + 1 : path;
        sprintf(out, "%s+%X", bn, addr - (unsigned int)hm);
        return;
    }
    sprintf(out, "?%08X", addr);
}

// The log file is opened ONCE and held, not reopened per line.
//
// This was a genuine observer-effect bug, found by this project's own file-open
// probe. The previous version did fopen/fprintf/fclose on every line, and
// LogLine is called on the MAIN THREAD from the slow-lock path in
// HookedTexLockRect. So every slow lock we reported triggered a synchronous
// CreateFile + CloseHandle on the render thread - which is exactly the kind of
// stall being hunted. It showed up in the stutter attribution as
// `ZwCreateFile` (0.15s), `ZwClose` (0.09s) and `MSVCR100.dll+10A3B` (0.17s,
// the CRT's file I/O), with EBP chains consisting entirely of `VERSION.dll`
// frames - i.e. our own mod. Roughly 0.4s of measured "stutter" in that run
// was the instrument stalling the frame it was measuring.
//
// The same care was taken with the stutter watchdog (rate-limited, never
// suspends more than 25x/sec) and the frametime histogram (one array
// increment, no I/O) - the logger was simply overlooked.
//
// Holding the handle removes the open/close syscalls entirely. fflush is kept
// so a crash still leaves a complete log, which has mattered repeatedly here
// for diagnosing crashes that killed the process outright.
static FILE *g_logFile = NULL;
static CRITICAL_SECTION g_logLock;
static volatile LONG g_logLockState = 0;   // 0=none, 1=initialising, 2=ready
static volatile LONG g_bootFlush = 1;      // flush per line until boot is done

// Reads LogAppend out of the ini directly. LogLine runs before LoadConfig
// (the very first line is written during DllMain), so the normal config path
// is not available yet and the answer is needed before the file is opened.
// Absent key or absent file = 0 = truncate, which is the shipping behaviour.
// Raw Win32, NOT the CRT. The first LogLine happens inside DllMain, under the
// loader lock, while other DLLs (the HD GUI mod's version.dll) are still
// loading. CRT file I/O there is the classic way to turn a clean startup into
// an intermittent one, and doing it with fopen coincided with exactly that -
// one launch where the HD mod did not load, one crash before the window, one
// clean run. CreateFile/ReadFile take no CRT locks and are safe here.
//
// Reads only the first 4 KB: the key is one short line in a small file, and a
// bounded read cannot become a long stall on the startup path.
static int LogAppendRequested(void)
{
    char path[MAX_PATH];
    char buf[4096];
    HANDLE h;
    DWORD got = 0;
    char *p;

    GetModuleFileNameA(NULL, path, MAX_PATH);
    {
        char *slash = strrchr(path, '\\');
        if (!slash) return 0;
        strcpy(slash + 1, MOD_CONFIG_FILE);
    }
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;      // no ini = truncate = default
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!got) return 0;
    buf[got] = '\0';

    // Line-anchored so a key that merely CONTAINS the name cannot match.
    for (p = buf; (p = strstr(p, "LogAppend=")) != NULL; p += 10) {
        if (p != buf && p[-1] != '\n' && p[-1] != '\r') continue;
        return (p[10] != '0');
    }
    return 0;
}

static void LogLine(const char *msg)
{
    if (InterlockedCompareExchange(&g_logLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_logLock);
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, MOD_LOG_FILE);
        // "w" (truncate) is the SHIPPING default, not "a". Appending across
        // every launch is right for a developer bisecting two sessions and
        // wrong for a player: with any diagnostic armed this file grows
        // without bound, and one playthrough here reached ~2 GB. One run per
        // file keeps it to something a user can send and an editor can open.
        //
        // LogAppend=1 in the ini restores the old cross-run behaviour. It is
        // read straight from the file rather than from g_logAppend, because
        // logging starts long before LoadConfig runs - the boot lines are
        // exactly the ones worth keeping, so the setting has to be known
        // before the first of them is written.
        g_logFile = fopen(path, LogAppendRequested() ? "a" : "w");
        g_logLockState = 2;
    } else {
        while (g_logLockState != 2) Sleep(0);
    }
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
    // Size cap. Truncating per run bounds the file across a playthrough, but
    // NOT within a single long session with a diagnostic armed - LogPassRts
    // and the watchdog can both produce lines continuously. Past the cap the
    // log stops growing rather than being rotated: a partial log from the
    // start of a session is more useful than a tail of whatever happened to
    // be last, because the boot lines and the first occurrence of a problem
    // are what get read.
    if (g_logCapped) { LeaveCriticalSection(&g_logLock); return; }
    fprintf(g_logFile, "%s\n", msg);
    if (g_logMaxMB > 0) {
        g_logBytes += (LONGLONG)strlen(msg) + 1;
        if (g_logBytes > (LONGLONG)g_logMaxMB * 1048576) {
            fprintf(g_logFile, "[log] size cap of %ld MB reached - logging stops here."
                               " Raise LogMaxMB (0 = unlimited) in " MOD_CONFIG_FILE ".\n",
                    g_logMaxMB);
            fflush(g_logFile);
            g_logCapped = 1;
        }
    }
    // BOOT ONLY: flush every line until the first monitor window clears this.
    // Buffered logging is right for the steady state (see the note above),
    // but it made a boot crash undiagnosable - everything up to the fault was
    // lost and the log's last lines came from the PREVIOUS session, which
    // sent me guessing at the diff instead of reading the failure.
    if (g_bootFlush) fflush(g_logFile);
    // fflush per line REMOVED - it was a synchronous disk write inside a lock
    // the MAIN THREAD also takes (via the "[d3d9] SLOW ..." paths). With the
    // stutter watchdog running at a low threshold it wrote ~75 flushed lines a
    // second into a multi-megabyte file, and a stack-walk capture found the
    // main thread parked in KERNELBASE under VERSION.dll on 42% of slow frames
    // - i.e. the instrument was manufacturing the slow frames it was there to
    // diagnose. An instrument that perturbs what it measures is worse than
    // useless, which this file already says about the D3D9 overlay it refused
    // to build; the same rule applies here and was missed.
    //
    // The buffer is flushed once per monitor tick (500ms) instead, so at most
    // half a second of log is at risk on a hard crash. That tradeoff is
    // deliberate and worth stating: several crashes in this project were
    // diagnosed from the log tail. If a crash ever loses its last lines,
    // FlushLog() can be called from wherever the risky work happens rather
    // than restoring the per-line flush.
    LeaveCriticalSection(&g_logLock);
}

static void FlushLog(void)
{
    if (g_logLockState != 2 || !g_logFile) return;
    EnterCriticalSection(&g_logLock);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

