// ---- ForceImmediatePresent: intercept the game's OWN Direct3DCreate9 ------
//
// Investigation (see PROGRESS.md, "CONFIRMED: the 60Hz clustering is vsync")
// found the frame-time distribution clustered sharply on multiples of
// 16.7ms rather than the smooth curve a CPU-bound hitch produces, and a
// one-shot diagnostic (via GetDevice() on a real game texture, since the
// device-vtable probe below does not reach the real device - see the
// "Methodological finding" section) confirmed the real swap chain requests
// PresentationInterval=D3DPRESENT_INTERVAL_DEFAULT (0x0), Windowed=1. The
// D3D9 spec defines DEFAULT as behaving identically to INTERVAL_ONE (vsync).
// AMD Adrenalin's vsync-off setting not helping is consistent with this:
// that control has historically applied to exclusive-fullscreen
// presentation, not windowed, where the DWM compositor paces frames instead.
//
// Confirmed via LRFF13.exe's own import table that it imports plain
// `Direct3DCreate9` from d3d9.dll (not Ex) - so, unlike the abandoned
// Direct3DCreate9 EXPORT-hook approach (v11, defeated by RTSS/Steam overlay
// re-patching the export - see the v12 comment on ProbeD3D9ForVtable), this
// uses PatchIat() on the GAME'S OWN IAT entry for Direct3DCreate9. That is a
// private copy in this process's own import table, not a shared resource
// overlays fight over, so it sidesteps that problem entirely. It also means
// this does NOT depend on the device-vtable-sharing question that broke the
// Present diagnostic: this intercepts device CREATION itself, before any
// vtable-sharing assumption could matter.
//
// Real risk, not just a performance knob: engines from this console-port era
// often tie internal timing (animation speed, physics tick, cutscene sync)
// to an assumed fixed-cadence Present. Forcibly removing that cadence can
// desync those systems in ways much harder to notice than a dropped frame.
// Default OFF, one config key (F5 / ForceImmediatePresent), instantly
// revertible - same discipline as every other risky fix in this file.
typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_IDirect3D9CreateDevice)(
    IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);

static PFN_Direct3DCreate9 g_realDirect3DCreate9 = NULL;
static PFN_IDirect3D9CreateDevice g_origIDirect3D9CreateDevice = NULL;
static volatile LONG g_createDeviceHookInstalled = 0;
static volatile LONG g_forceImmediateApplied = 0;
static volatile LONG g_msaaSurveyed = 0;   // capability survey runs once

static HRESULT STDMETHODCALLTYPE HookedIDirect3D9CreateDevice(
    IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    if (g_forceImmediatePresentEnabled && pPP &&
        pPP->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        char line[160];
        sprintf(line, "[d3d9] ForceImmediatePresent: rewriting PresentationInterval 0x%lX -> IMMEDIATE (Windowed=%d)",
                (unsigned long)pPP->PresentationInterval, pPP->Windowed);
        LogLine(line);
        pPP->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        InterlockedIncrement(&g_forceImmediateApplied);
    }
    // Does the game's OWN code ask for MSAA? XIII-1/XIII-2 reportedly force
    // it (their transparency effects depend on it), so whether this engine
    // still requests it here is direct evidence about whether an MSAA path
    // exists in the renderer at all.
    if (pPP) {
        // Presentation size captured HERE as well as at Reset. It used to come
        // only from Reset, which is wrong whenever the device is created at its
        // final size and never Reset - a real, observed case (one session
        // logged zero Resets). g_backbufW then stayed 0, which made the [ssaa]
        // status line report "not supersampling" regardless of the truth, and
        // left anything keyed off the presented size working from nothing.
        if (pPP->BackBufferWidth && pPP->BackBufferHeight) {
            g_backbufW = pPP->BackBufferWidth;
            g_backbufH = pPP->BackBufferHeight;
            g_presentWindowed = (LONG)(pPP->Windowed ? 1 : 0);
            g_presentFmt = (LONG)pPP->BackBufferFormat;
        }
        if (pPP->hDeviceWindow) g_gameHwnd = pPP->hDeviceWindow;
        char l[192];
        sprintf(l, "[rt] CreateDevice: %ux%u backbufFmt=%d MULTISAMPLE=%d qual=%lu autoDepth=%d depthFmt=%d windowed=%d",
                pPP->BackBufferWidth, pPP->BackBufferHeight, (int)pPP->BackBufferFormat,
                (int)pPP->MultiSampleType, (unsigned long)pPP->MultiSampleQuality,
                (int)pPP->EnableAutoDepthStencil, (int)pPP->AutoDepthStencilFormat,
                (int)pPP->Windowed);
        LogLine(l);
    }
    // ---- MSAA capability survey ------------------------------------------
    // Bounds the whole MSAA question before any work is spent on it. Runs once
    // at device creation, where we hold the IDirect3D9 the game is actually
    // using - so the answer covers whatever backs d3d9.dll (Microsoft, DXVK, a
    // wrapper), not a probe device that might differ.
    //
    // Why it is worth asking at all: the "deferred renderer, MSAA impossible"
    // verdict was a measurement bug - `max SetRenderTarget index = 3` counted
    // SetRenderTarget(n, NULL), i.e. slots being CLEARED. Corrected, real
    // multi-target binds are ZERO: this is a single-target FORWARD renderer,
    // which is the configuration where MSAA is normally viable.
    //
    // Surveys the formats the frame graph actually uses: A8R8G8B8 (the scene
    // colour target MULTI_SAMPLE renders into), D24S8 (its depth), and R32F
    // (the linear-depth companion). R32F matters because if the engine samples
    // it per-pixel, an MSAA scene target needs a resolve for it too.
    if (!g_msaaSurveyed) {
        g_msaaSurveyed = 1;
        struct { D3DFORMAT fmt; const char *name; } fmts[] = {
            { D3DFMT_A8R8G8B8, "A8R8G8B8 (scene colour)" },
            { D3DFMT_D24S8,    "D24S8 (depth)" },
            { D3DFMT_R32F,     "R32F (linear depth)" },
        };
        D3DMULTISAMPLE_TYPE lv[] = { D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES,
                                     D3DMULTISAMPLE_8_SAMPLES };
        for (int fi = 0; fi < 3; fi++) {
            char l[256];
            int o = sprintf(l, "[msaa] %-26s :", fmts[fi].name);
            for (int li = 0; li < 3; li++) {
                DWORD q = 0;
                HRESULT chr = IDirect3D9_CheckDeviceMultiSampleType(
                    This, Adapter, DeviceType, fmts[fi].fmt,
                    pPP ? pPP->Windowed : TRUE, lv[li], &q);
                o += sprintf(l + o, "  x%d=%s", 2 << li,
                             SUCCEEDED(chr) ? "YES" : "no");
                if (SUCCEEDED(chr)) o += sprintf(l + o, "(q=%lu)", (unsigned long)q);
            }
            LogLine(l);
        }
        LogLine("[msaa] NOTE: support here only means the DRIVER can create such a surface. "
                "D3D9 still cannot SAMPLE a multisampled surface - the engine renders the "
                "scene to a texture and samples it in post, so a StretchRect resolve would "
                "have to be injected regardless.");
    }

    HRESULT hr = g_origIDirect3D9CreateDevice(This, Adapter, DeviceType, hFocusWindow,
                                              BehaviorFlags, pPP, ppReturnedDeviceInterface);
    // Until ForceStdD3D9 this path was believed dead (two prior test runs
    // never saw it fire) and was left as ForceImmediatePresent-only. It is
    // no longer provably dead, so it must install everything the Ex path
    // does - HookRealDevicePresent is the single entry point for staging,
    // DISCARD, CreateTexture/ManagedPool, shader timing, all of it. Without
    // this call, ForceStdD3D9 would boot with only the presentation-interval
    // rewrite active and every other fix silently inert - the same "load
    // path fine, other path missing" bug class this project keeps finding,
    // caught here before a test run rather than after one.
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookRealDevicePresent(*ppReturnedDeviceInterface);
    }
    return hr;
}

// IDirect3D9::CreateDevice is vtable slot 16 (QueryInterface/AddRef/Release
// = 0-2, then 13 IDirect3D9-specific methods before CreateDevice).
#define IDIRECT3D9_VTBL_SLOT_CREATEDEVICE 16

static void InstallCreateDeviceHookOn(IDirect3D9 *d3d)
{
    if (InterlockedCompareExchange(&g_createDeviceHookInstalled, 1, 0) != 0) return;
    void **vtbl = *(void ***)d3d;
    g_origIDirect3D9CreateDevice = (PFN_IDirect3D9CreateDevice)vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE];
    DWORD oldProtect;
    if (VirtualProtect(&vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE] = (void *)HookedIDirect3D9CreateDevice;
        VirtualProtect(&vtbl[IDIRECT3D9_VTBL_SLOT_CREATEDEVICE], sizeof(void *), oldProtect, &oldProtect);
        LogLine("[d3d9] IDirect3D9::CreateDevice vtable hook installed (ForceImmediatePresent path)");
    } else {
        g_createDeviceHookInstalled = 0;
        LogLine("[d3d9] IDirect3D9::CreateDevice vtable hook FAILED (VirtualProtect)");
    }
}

// This fires only for calls through the GAME's own compiled
// call-through-IAT instruction. ProbeD3D9ForVtable below resolves
// Direct3DCreate9 itself via GetProcAddress and calls that pointer directly,
// bypassing the exe's IAT entirely - so the two paths are independent, and
// the probe's own throwaway device creation never touches this hook at all.
static IDirect3D9 *WINAPI HookedDirect3DCreate9(UINT SDKVersion)
{
    IDirect3D9 *real = g_realDirect3DCreate9(SDKVersion);
    // This firing at all is the entire result of the ForceStdD3D9 experiment:
    // it means the game's decade-dormant non-Ex fallback path is alive. If
    // ForceStdD3D9 is on and this line never appears in the log, the fallback
    // does not exist and the approach is dead regardless of anything else.
    if (g_forceStdD3D9) {
        char l[128];
        sprintf(l, "[d3d9] ForceStdD3D9: plain Direct3DCreate9 CALLED - fallback path is alive (result=%s)",
                real ? "ok" : "NULL");
        LogLine(l);
    }
    if (real) InstallCreateDeviceHookOn(real);
    return real;
}

// ---- Direct3DCreate9Ex path ----------------------------------------------
//
// Two test runs proved the game does NOT reach d3d9 through the exe's static
// `Direct3DCreate9` import: the IAT hook installed (first from the deferred
// thread, then from DllMain, ruling out a startup race) and never fired once,
// while the device demonstrably existed. The exe imports `LoadLibraryA` and
// `GetProcAddress`, and does NOT statically import `Direct3DCreate9Ex` at all
// - so whichever entry point it uses is resolved dynamically at runtime.
//
// Hooking GetProcAddress in the exe's own IAT catches that resolution
// regardless of which name is requested, and does it without patching the
// bytes of a shared d3d9.dll export - deliberately avoiding the hook war that
// defeated the v11 export-patch approach (RTSS/Steam Overlay re-patching
// Direct3DCreate9 and replacing our JMP outright). An IAT entry is this
// process's own private table; nothing else contends for it.
//
// IDirect3D9Ex vtable layout: IDirect3D9 occupies slots 0-16 (CreateDevice is
// 16), and IDirect3D9Ex appends GetAdapterModeCountEx(17),
// EnumAdapterModesEx(18), GetAdapterDisplayModeEx(19), CreateDeviceEx(20).
#define IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX 20

typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_IDirect3D9CreateDeviceEx)(
    IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
    D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **);

static PFN_Direct3DCreate9Ex g_realDirect3DCreate9Ex = NULL;
static PFN_IDirect3D9CreateDeviceEx g_origCreateDeviceEx = NULL;
static volatile LONG g_createDeviceExHookInstalled = 0;

static HRESULT STDMETHODCALLTYPE HookedIDirect3D9CreateDeviceEx(
    IDirect3D9Ex *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
    D3DDISPLAYMODEEX *pFullscreenDisplayMode, IDirect3DDevice9Ex **ppReturnedDeviceInterface)
{
    if (g_forceImmediatePresentEnabled && pPP &&
        pPP->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        char line[176];
        sprintf(line, "[d3d9] ForceImmediatePresent: CreateDeviceEx rewriting PresentationInterval 0x%lX -> IMMEDIATE (Windowed=%d)",
                (unsigned long)pPP->PresentationInterval, pPP->Windowed);
        LogLine(line);
        pPP->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        InterlockedIncrement(&g_forceImmediateApplied);
    }
    HRESULT hr = g_origCreateDeviceEx(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                      pPP, pFullscreenDisplayMode, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookRealDevicePresent((IDirect3DDevice9 *)*ppReturnedDeviceInterface);
    }
    return hr;
}

static void InstallCreateDeviceExHookOn(IDirect3D9Ex *d3dEx)
{
    if (InterlockedCompareExchange(&g_createDeviceExHookInstalled, 1, 0) != 0) return;
    void **vtbl = *(void ***)d3dEx;
    g_origCreateDeviceEx = (PFN_IDirect3D9CreateDeviceEx)vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX];
    DWORD oldProtect;
    if (VirtualProtect(&vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX], sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX] = (void *)HookedIDirect3D9CreateDeviceEx;
        VirtualProtect(&vtbl[IDIRECT3D9EX_VTBL_SLOT_CREATEDEVICEEX], sizeof(void *), oldProtect, &oldProtect);
        LogLine("[d3d9] IDirect3D9Ex::CreateDeviceEx vtable hook installed (ForceImmediatePresent path)");
    } else {
        g_createDeviceExHookInstalled = 0;
        LogLine("[d3d9] IDirect3D9Ex::CreateDeviceEx vtable hook FAILED (VirtualProtect)");
    }
}

static HRESULT WINAPI HookedDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D)
{
    HRESULT hr = g_realDirect3DCreate9Ex(SDKVersion, ppD3D);
    if (SUCCEEDED(hr) && ppD3D && *ppD3D) InstallCreateDeviceExHookOn(*ppD3D);
    return hr;
}

static FARPROC (WINAPI *g_realGetProcAddress)(HMODULE, LPCSTR) = NULL;

static FARPROC WINAPI HookedGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC real = g_realGetProcAddress(hModule, lpProcName);
    // Ordinal lookups pass a small integer in the pointer, not a string -
    // dereferencing that would fault.
    if (real && lpProcName && (ULONG_PTR)lpProcName > 0xFFFF) {
        if (strcmp(lpProcName, "Direct3DCreate9Ex") == 0) {
            g_realDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)real;
            if (g_forceStdD3D9) {
                // Report the symbol as missing so the game takes the same
                // path it would on an OS without D3D9Ex. Its plain
                // Direct3DCreate9 import is already patched (and already
                // proven present), so if a fallback exists at all we will
                // see HookedDirect3DCreate9 fire right after this line.
                LogLine("[d3d9] ForceStdD3D9: hiding Direct3DCreate9Ex - expecting plain D3D9 fallback");
                return NULL;
            }
            LogLine("[d3d9] game resolved Direct3DCreate9Ex via GetProcAddress - returning wrapper");
            return (FARPROC)HookedDirect3DCreate9Ex;
        }
        if (strcmp(lpProcName, "Direct3DCreate9") == 0) {
            g_realDirect3DCreate9 = (PFN_Direct3DCreate9)real;
            LogLine("[d3d9] game resolved Direct3DCreate9 via GetProcAddress - returning wrapper");
            return (FARPROC)HookedDirect3DCreate9;
        }
    }
    return real;
}

// Idempotent: called from DllMain (where it must succeed, to beat the game's
// own Direct3DCreate9 call) and again from the deferred install thread as a
// fallback in case the early attempt ever fails.
//
// Deliberately does NOT require d3d9.dll to be loaded. All this needs is the
// exe's own import table entry, which the loader has already snapped before
// any DllMain runs, and PatchIat reads the current value straight out of it -
// so there is no dependency on d3d9.dll's module handle, and no
// GetProcAddress call against another module from inside the loader lock.

static void InstallForceImmediatePresentHook(void)
{
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) != 0) return;

    HMODULE hExe = GetModuleHandleA(NULL);
    if (!hExe) {
        installed = 0;
        LogLine("[d3d9] ForceImmediatePresent: exe module handle missing, skipped");
        return;
    }
    // Kept for completeness in case a static-import path is ever used, but
    // two test runs proved the game does not reach d3d9 this way.
    void *realFn = PatchIat(hExe, "d3d9.dll", "Direct3DCreate9", (void *)HookedDirect3DCreate9);
    if (realFn) {
        g_realDirect3DCreate9 = (PFN_Direct3DCreate9)realFn;
        LogLine("[d3d9] Direct3DCreate9 IAT hook installed (game exe's own import table)");
    } else {
        LogLine("[d3d9] Direct3DCreate9 IAT hook not applied (import not found in exe)");
    }

    void *realCf = PatchIat(hExe, "kernel32.dll", "CreateFileA", (void *)HookedCreateFileA);
    if (realCf) {
        g_realCreateFileA = (PFN_CreateFileA)realCf;
        LogLine("[probe] CreateFileA IAT hook installed (main-thread file-open probe)");
    }
    void *realCh = PatchIat(hExe, "kernel32.dll", "CloseHandle", (void *)HookedCloseHandle);
    if (realCh) g_realCloseHandle = (PFN_CloseHandle)realCh;

    void *realSleep = PatchIat(hExe, "kernel32.dll", "Sleep", (void *)HookedSleep);
    if (realSleep) {
        g_realSleep = (PFN_Sleep)realSleep;
        LogLine("[probe] Sleep IAT hook installed (main-thread frame-limiter probe)");
    }

    // The one that actually matters: the game resolves its d3d9 entry point
    // dynamically, so intercept the resolution itself.
    void *realGpa = PatchIat(hExe, "kernel32.dll", "GetProcAddress", (void *)HookedGetProcAddress);
    if (realGpa) {
        g_realGetProcAddress = (FARPROC (WINAPI *)(HMODULE, LPCSTR))realGpa;
        LogLine("[d3d9] GetProcAddress IAT hook installed - will intercept dynamic d3d9 resolution");
    } else {
        installed = 0;   // allow the deferred-thread fallback to retry
        LogLine("[d3d9] GetProcAddress IAT hook FAILED (import not found in exe)");
    }
}

// ---- Crash reporter -------------------------------------------------------
// Replaces LogFlush=1 as the way to find out where a startup crash happens.
// Flushing every line DID make the last line truthful, but it also put ~40
// synchronous disk writes into the boot window, from several threads, inside
// a lock the main thread takes - and the intermittent startup failure got
// dramatically WORSE with it on. That is the observer effect this file already
// documents twice; an instrument that changes the failure rate is not
// measuring the failure.
//
// This costs nothing until the process actually dies: no I/O, no locks, no
// per-line work. When it does die it records the exception code, the faulting
// address, and - the part that actually narrows the search - WHICH MODULE the
// address belongs to. "Crashed in version.dll" and "crashed in LRFF13.exe" are
// completely different investigations, and right now we cannot tell them
// apart.
//
// Chains to whatever filter was already installed (the HD GUI mod may have its
// own) rather than replacing it, and only reports once.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter = NULL;
static volatile LONG g_crashReported = 0;

static void LogFlushNow(void)
{
    if (g_logFile && g_logLockState == 2) {
        EnterCriticalSection(&g_logLock);
        fflush(g_logFile);
        LeaveCriticalSection(&g_logLock);
    }
}

static LONG WINAPI ModCrashFilter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord &&
        InterlockedCompareExchange(&g_crashReported, 1, 0) == 0) {
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        char modPath[MAX_PATH];
        HMODULE hm = NULL;
        DWORD off = 0;
        modPath[0] = '\0';
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &hm) && hm) {
            GetModuleFileNameA(hm, modPath, MAX_PATH);
            off = (DWORD)((unsigned char *)addr - (unsigned char *)hm);
        }
        {
            const char *name = modPath;
            const char *slash = strrchr(modPath, '\\');
            if (slash) name = slash + 1;
            char l[400];
            sprintf(l, "[crash] code=0x%08lX addr=%p in %s+0x%lX thread=%lu",
                    (unsigned long)ep->ExceptionRecord->ExceptionCode, addr,
                    modPath[0] ? name : "<unknown module>",
                    (unsigned long)off, (unsigned long)GetCurrentThreadId());
            LogLine(l);
        }
        LogFlushNow();
    }
    return g_prevUnhandledFilter ? g_prevUnhandledFilter(ep)
                                 : EXCEPTION_CONTINUE_SEARCH;
}

// The unhandled-exception filter above caught NOTHING on a reproduced crash -
// no [crash] line at all. That is itself the finding: the process is not dying
// from an ordinary unhandled exception. Things that bypass that filter:
//
//   - /GS stack-cookie failure (STATUS_STACK_BUFFER_OVERRUN 0xC0000409), which
//     fail-fasts. This project has already lost days to exactly that once, in
//     LoadConfig's char[300].
//   - CRT abort() / invalid-parameter handler.
//   - Someone else's handler catching it and calling ExitProcess.
//   - TerminateProcess from outside.
//
// A VECTORED handler runs before any of that, on the FIRST chance, so it sees
// the exception whatever happens to it afterwards. Registered "first" (1) so
// it also beats any handler the HD GUI mod installs.
//
// Filtered to fatal codes only, and reports once. First-chance handlers see
// every benign C++ throw in the process, and logging those would be both
// enormous and - given what per-line flushing already did to the failure rate -
// actively harmful.
static PVOID g_crashVeh = NULL;

static LONG CALLBACK ModCrashVeh(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        int fatal = (code == 0xC0000005u)   /* access violation        */
                 || (code == 0xC0000409u)   /* stack buffer overrun    */
                 || (code == 0xC00000FDu)   /* stack overflow          */
                 || (code == 0xC000001Du)   /* illegal instruction     */
                 || (code == 0xC0000094u)   /* integer divide by zero  */
                 || (code == 0xC0000096u);  /* privileged instruction  */
        if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
        // Our own guarded probes AV by DESIGN: the watchdog's raw stack scan
        // (StutterWatchdogThread) sweeps the suspended main thread's stack
        // under __try and faults whenever it walks off the mapped region.
        // Found 2026-08-15: those benign first-chance AVs were consuming the
        // one-shot g_crashReported below, so when the game genuinely died
        // (the Load Game crash) this logger had already spent its single
        // report on a probe and the log ended silently - the exact failure
        // this handler exists to prevent. EIP inside our module -> pass
        // through without spending the report. A real crash in our DLL is
        // not lost: it reaches ModCrashFilter at the unhandled stage.
        {
            HMODULE selfMod = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(ULONG_PTR)ModCrashVeh, &selfMod) && selfMod) {
                DWORD eip = (DWORD)ep->ContextRecord->Eip;
                unsigned char *mb = (unsigned char *)selfMod;
                // SizeOfImage straight from our own in-memory PE header -
                // avoids a psapi.lib dependency for one field.
                DWORD sz = ((IMAGE_NT_HEADERS *)(mb + ((IMAGE_DOS_HEADER *)mb)->e_lfanew))
                               ->OptionalHeader.SizeOfImage;
                if (eip >= (DWORD)(ULONG_PTR)mb && eip < (DWORD)(ULONG_PTR)mb + sz)
                    return EXCEPTION_CONTINUE_SEARCH;
            }
        }
        if (InterlockedCompareExchange(&g_crashReported, 1, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;

        void *addr = ep->ExceptionRecord->ExceptionAddress;
        char modPath[MAX_PATH];
        HMODULE hm = NULL;
        DWORD off = 0;
        modPath[0] = '\0';
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &hm) && hm) {
            GetModuleFileNameA(hm, modPath, MAX_PATH);
            off = (DWORD)((unsigned char *)addr - (unsigned char *)hm);
        }
        {
            const char *name = modPath;
            const char *slash = strrchr(modPath, '\\');
            if (slash) name = slash + 1;
            char l[400];
            sprintf(l, "[crash] FIRST-CHANCE code=0x%08lX addr=%p in %s+0x%lX thread=%lu accessing=0x%08lX",
                    (unsigned long)code, addr,
                    modPath[0] ? name : "<unknown module>",
                    (unsigned long)off, (unsigned long)GetCurrentThreadId(),
                    ep->ExceptionRecord->NumberParameters >= 2
                        ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0);
            LogLine(l);
        }
        // Raw-stack sweep for game-module return addresses - same technique
        // and same rationale as the watchdog's scan line: when EIP is already
        // garbage (the Load Game crash had it in unmapped heap memory), the
        // EBP chain is useless, but the stack still holds the trail of who
        // was executing.
        {
            char buf[512];
            int o = sprintf(buf, "[crash]   stack:");
            int n = 0;
            __try {
                DWORD *sp = (DWORD *)ep->ContextRecord->Esp;
                for (int i = 0; i < 512 && n < 14; i++) {
                    DWORD v = sp[i];
                    if (g_mainModBase && v > (DWORD)g_mainModBase &&
                        v < (DWORD)g_mainModBase + 0x2400000) {
                        o += sprintf(buf + o, " %08lX",
                                     v - (DWORD)g_mainModBase + 0x00400000);
                        n++;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            LogLine(buf);
        }
        LogFlushNow();
    }
    return EXCEPTION_CONTINUE_SEARCH;   // never swallow it
}

void InstallEarlyHooks(void)
{
    // Both installed first, before anything else can fault. Function calls
    // only - no allocation, no I/O - so they are safe on the DllMain path.
    g_prevUnhandledFilter = SetUnhandledExceptionFilter(ModCrashFilter);
    g_crashVeh = AddVectoredExceptionHandler(1, ModCrashVeh);
    // The config must be read here too, not just on the deferred thread: the
    // flag is consulted inside HookedIDirect3D9CreateDevice, which can fire
    // before that thread has had a chance to run. LoadConfig is idempotent
    // (it only re-reads the same file), so the deferred call is harmless.
    {
        // First line of every log, before anything can fail. A bug report is
        // only actionable if the log says which build produced it, and the
        // build stamp distinguishes two DLLs carrying the same version.
        char b[160];
        sprintf(b, "[boot] %s %s (%s) - built %s %s",
                MOD_NAME, MOD_VERSION, MOD_TAGLINE, __DATE__, __TIME__);
        LogLine(b);
    }
    LoadConfig();
    LogLine("[boot] early: config loaded, installing present hook");
    InstallForceImmediatePresentHook();
    LogLine("[boot] early: present hook done");
}

static int InstallD3D9Hook(void)
{
    g_d3dStackTls = TlsAlloc();
    g_d3dThunks = (unsigned char *)VirtualAlloc(NULL, MAX_D3D_SLOTS * 16,
                                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_d3dThunks) return 0;
    ProbeD3D9ForVtable();
    return g_d3dSlotCount > 0;
}

void InstallPrefetchHook(void)
{
    char line[256];
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base) {
        LogLine("InstallPrefetchHook: GetModuleHandleA(NULL) failed");
        return;
    }

    CalibrateTsc();
    LoadConfig();
    // The "write the config back so every key is visible" step deliberately
    // does NOT happen here. This runs moments after DllMain, while the game's
    // own startup and any other proxy DLL (the HD GUI mod loads from
    // version.dll at the same time) are still initialising. Doing file I/O
    // there put a write on the same ini that LogAppendRequested may be reading
    // from another thread, and coincided with intermittent startup failures -
    // one launch where the HD mod did not load, one crash before the window
    // appeared, one clean run. Not proven, but the startup write buys nothing
    // at that instant, so it is moved to the monitor thread's first tick,
    // well after the process has settled. See g_configRewritten.
    // Before any texture hook can fire: the push pointer must be resolved by
    // the time the first staged upload completes. Runs on this deferred
    // thread, not DllMain, so GetProcAddress is outside the loader lock.
    DetectHdTexInterop();
    // The overlay driver always runs (it owns the overlay window and its
    // message pump). The debug panel is deprecated and only starts when
    // ENABLE_GUI_PANEL is compiled back in.
    CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
#if ENABLE_GUI_PANEL
    LogLine("[boot] prefetch: starting GUI thread");
    CreateThread(NULL, 0, GuiThread, NULL, 0, NULL);
    LogLine("[boot] prefetch: GUI thread started");
#endif

    for (int i = 0; i < NUM_FNS; i++) {
        g_funcs[i].target = base + g_funcs[i].rva;
        g_stackTls[i] = TlsAlloc();
    }

    g_mainModBase = (unsigned int)base;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        g_mainModSize = nt->OptionalHeader.SizeOfImage;
    }
    sprintf(line, "Module base = 0x%08X size = 0x%08X", (unsigned int)base, g_mainModSize);
    LogLine(line);

#if ENABLE_GAME_MENU
    // Patch the menu-tree build call site now, well before the game's
    // framework init (FUN_00ab8210) constructs the window and runs the first
    // FUN_00abc310 rebuild - the menu exists with our entries from frame one.
    InstallGameMenuHook();
#endif

    // FA-object schedule diagnostic. Installed unconditionally (the detour
    // itself checks g_logFaSchedule and returns immediately when off, so it
    // can be toggled live from the GUI without a relaunch). Prologue verified
    // against the shipped exe: 55 8B EC 83 EC 14 - a standard PUSH EBP /
    // MOV EBP,ESP / SUB ESP, i.e. a clean 6-byte patch boundary with no
    // relative operands, the same shape as every other hook here.
    //
    // RETIRED (see ENABLE_GYSAHL_DIAG at the top of this file). "Unconditional"
    // is exactly why this had to be gated rather than just switched off: with
    // every toggle at 0 the block still patched seven game functions and put a
    // handler at the FRONT of the process exception chain. The bug it served is
    // fixed in scr104.clb.
#if ENABLE_GYSAHL_DIAG
    {
        HookedFunc faHf;
        faHf.name = "FUN_009c0490";
        faHf.target = base + FA_SCHED_RVA;
        faHf.rva = FA_SCHED_RVA;
        faHf.patchLen = 6;
        g_faSchedTarget = faHf.target;
        int faOk = InstallJmpHook(&faHf, (void *)Detour_faSched, &g_trampoline_faSched);
        sprintf(line, "[fa] changeFaObjectSchedule hook @ 0x%08X: %s",
                (unsigned int)faHf.target, faOk ? "installed" : "FAILED");
        LogLine(line);

        HookedFunc tcHf;
        tcHf.name = "FUN_009c22a0";
        tcHf.target = base + TIMER_CB_RVA;
        tcHf.rva = TIMER_CB_RVA;
        tcHf.patchLen = 6;
        int tcOk = InstallJmpHook(&tcHf, (void *)Detour_timerCb, &g_trampoline_timerCb);
        sprintf(line, "[fa] setRelativeTimerCallback hook @ 0x%08X: %s",
                (unsigned int)tcHf.target, tcOk ? "installed" : "FAILED");
        LogLine(line);

        // Window bisection. hideWindow needs a 7-byte patch: its prologue is
        // 55 8B EC 56 8B 75 08, so a 6-byte cut would split the final
        // MOV ESI,[EBP+8]. The other two are clean 6-byte PUSH EBP/MOV/SUB.
        HookedFunc wHf;
        int wOk;
        wHf.name = "showMessageWindow"; wHf.rva = WIN_SHOW_RVA;
        wHf.target = base + WIN_SHOW_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winShow, &g_trampoline_winShow);
        sprintf(line, "[fa] showMessageWindow hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "hideWindow"; wHf.rva = WIN_HIDE_RVA;
        wHf.target = base + WIN_HIDE_RVA; wHf.patchLen = 7;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winHide, &g_trampoline_winHide);
        sprintf(line, "[fa] hideWindow hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "isWaitingDecideOrCancel"; wHf.rva = WIN_WAIT_RVA;
        wHf.target = base + WIN_WAIT_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winWait, &g_trampoline_winWait);
        sprintf(line, "[fa] isWaitingDecideOrCancel hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "isWindowClosing"; wHf.rva = WIN_CLOSING_RVA;
        wHf.target = base + WIN_CLOSING_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_winClosing, &g_trampoline_winClosing);
        sprintf(line, "[fa] isWindowClosing hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);

        // Vectored handler for the hardware watchpoint. Installed first so it
        // is ahead of anything else that might claim the exception.
        g_vehHandle = AddVectoredExceptionHandler(1, PlantWatchVeh);
        sprintf(line, "[fa] watchpoint VEH: %s", g_vehHandle ? "installed" : "FAILED");
        LogLine(line);

        wHf.name = "stringComp"; wHf.rva = STRCMP_RVA;
        wHf.target = base + STRCMP_RVA; wHf.patchLen = 6;
        wOk = InstallJmpHook(&wHf, (void *)Detour_strCmp, &g_trampoline_strCmp);
        sprintf(line, "[fa] stringComp hook: %s", wOk ? "installed" : "FAILED");
        LogLine(line);
    }
#endif  // ENABLE_GYSAHL_DIAG

    int ok[NUM_FNS];
    ok[FN_AACF10] = InstallJmpHook(&g_funcs[FN_AACF10], (void *)Detour_aacf10, &g_trampoline_aacf10);
    ok[FN_A2ADA0] = InstallJmpHook(&g_funcs[FN_A2ADA0], (void *)Detour_a2ada0, &g_trampoline_a2ada0);
    ok[FN_D19A00] = InstallJmpHook(&g_funcs[FN_D19A00], (void *)Detour_d19a00, &g_trampoline_d19a00);
    // FUN_00ac3040 (frame pacer) re-added: no SEH prologue (unlike
    // FUN_00a01a00/FUN_00a015b0, the two suspected of causing the earlier
    // crash), called once/frame from the main thread only - not a
    // multi-thread-contended hot spinlock like FUN_00a015b0, where adding
    // hook overhead could itself perturb contention timing. Lowest-risk of
    // the three new targets and the highest-value one (consistently the
    // single hottest function by raw sample count in every capture across
    // this entire investigation).
    ok[FN_AC3040] = InstallJmpHook(&g_funcs[FN_AC3040], (void *)Detour_ac3040, &g_trampoline_ac3040);

    // Sim-delta unquantiser. Same frame, 0x370 bytes further on than the
    // limiter above. Installed unconditionally because the detour's first act
    // is to check g_simDeltaFix and bail, so it can be A/B'd live from the GUI
    // without a relaunch - which is the whole point for a change whose effect
    // has to be judged by playing. Prologue verified against the shipped exe:
    // 55 8B EC 83 EC 08 (PUSH EBP / MOV EBP,ESP / SUB ESP,8), a clean 6-byte
    // boundary with no relative operands.
    {
        HookedFunc sdHf;
        sdHf.name = "FUN_00ac33b0";
        sdHf.rva = SIM_DELTA_RVA;
        sdHf.target = base + SIM_DELTA_RVA;
        sdHf.patchLen = 6;
        int sdOk = InstallJmpHook(&sdHf, (void *)Detour_simDelta, &g_trampoline_simDelta);
        sprintf(line, "[simdelta] frame-delta hook @ 0x%08X: %s",
                (unsigned int)sdHf.target, sdOk ? "installed" : "FAILED");
        LogLine(line);

        // DRAW_SHADOW pass timer. patchLen 5, not 6: the prologue is
        // 55 8B EC 51 56 8B F1, so a 6-byte cut would land inside MOV ESI,ECX.
        HookedFunc dsHf;
        dsHf.name = "FUN_00ac6040";
        dsHf.rva = DRAW_SHADOW_RVA;
        dsHf.target = base + DRAW_SHADOW_RVA;
        dsHf.patchLen = 5;
        int dsOk = InstallJmpHook(&dsHf, (void *)Detour_drawShadow, &g_trampoline_drawShadow);
        sprintf(line, "[ft] DRAW_SHADOW pass timer @ 0x%08X: %s",
                (unsigned int)dsHf.target, dsOk ? "installed" : "FAILED");
        LogLine(line);

#if ENABLE_LOADER_DIAG
        // Cause 3 measurement timers. Patch lengths from the verified
        // prologues (ghidra_output/loader_hook_safety.txt): 009dff70 is
        // 55 8B EC / 8B 45 1C (boundary +6), 009ddfe0 is 55 8B EC /
        // 81 EC 0C 03 00 00 (boundary +9), 009fcfd0 is 8B 44 24 10 / 53
        // (boundary +5). All position-independent, so they relocate into
        // the trampolines unchanged.
        {
            struct { const char *name; DWORD rva; int len; void *detour; void **tramp; } ldr[] = {
                { "FUN_009dff70(load)",    LDR_LOAD_RVA,    6, (void *)Detour_ldrLoad, &g_tramp_ldrLoad },
                { "FUN_009ddfe0(bind)",    LDR_BIND_RVA,    9, (void *)Detour_ldrBind, &g_tramp_ldrBind },
                { "FUN_009fcfd0(decrypt)", LDR_DECRYPT_RVA, 5, (void *)Detour_ldrDec,  &g_tramp_ldrDec },
            };
            for (int li = 0; li < (int)(sizeof(ldr)/sizeof(ldr[0])); li++) {
                HookedFunc lh;
                lh.name = ldr[li].name;
                lh.rva = ldr[li].rva;
                lh.target = base + ldr[li].rva;
                lh.patchLen = ldr[li].len;
                int lok = InstallJmpHook(&lh, ldr[li].detour, ldr[li].tramp);
                sprintf(line, "[loader] timer %s @ 0x%08X (len %d): %s",
                        ldr[li].name, (unsigned int)lh.target, ldr[li].len,
                        lok ? "installed" : "FAILED");
                LogLine(line);
            }
        }
#endif

        // Shadow-render skip (ShadowsOff diagnostic). Separate from the timer
        // above: that one wraps the pass HANDLER, this one skips the RENDER
        // inside it, so the handler's completion work still runs.
        HookedFunc srHf;
        srHf.name = "FUN_00a32a00";
        srHf.rva = SHADOW_RENDER_RVA;
        srHf.target = base + SHADOW_RENDER_RVA;
        srHf.patchLen = 6;
        int srOk = InstallJmpHook(&srHf, (void *)Detour_shadowRender, &g_trampoline_shadowRender);
        sprintf(line, "[shadow] shadow-render skip hook @ 0x%08X: %s",
                (unsigned int)srHf.target, srOk ? "installed" : "FAILED");
        LogLine(line);

        // Per-frame GPU fence. patchLen 5: the prologue is A1 60 E9 10 05
        // (MOV EAX,[0x0510E960]) - one 5-byte instruction, and JMP rel32 is
        // exactly 5, so nothing is split and no NOP padding is needed. The
        // operand is an absolute address, so it relocates into the trampoline
        // unchanged.
        HookedFunc gfHf;
        gfHf.name = "FUN_00a96090";
        gfHf.rva = GPU_FENCE_RVA;
        gfHf.target = base + GPU_FENCE_RVA;
        gfHf.patchLen = 5;
        int gfOk = InstallJmpHook(&gfHf, (void *)Detour_gpuFence, &g_trampoline_gpuFence);
        sprintf(line, "[gpufence] per-frame GPU fence hook @ 0x%08X: %s",
                (unsigned int)gfHf.target, gfOk ? "installed" : "FAILED");
        LogLine(line);

        // Draw-pass tags. patchLen differs per handler because their prologues
        // do, and cutting mid-instruction would corrupt the trampoline:
        //   ac5ee0/ac5f50/ac7030 : 55 8B EC 51 A1 xx xx xx xx  -> 9
        //   ac6890/ac6b00        : 55 8B EC 83 EC nn           -> 6
        //   ac6e50               : 55 8B EC 51 53              -> 5
        //   ac6150 (DRAW_MENU)   : 55 8B EC 51 A1 xx xx xx xx  -> 9
        //   ab7810 (BACK_BUFFER) : 55 8B EC 8B 45 10           -> 6
        // The last two were verified against the shipped exe on 2026-08-12
        // (ghidra_output/menu_pass_prologue.txt lists each instruction with
        // its cumulative offset, so the boundary is read rather than assumed).
        // ab7810 is only 22 bytes long in total, which is fine - the patch
        // needs 6 relocatable leading bytes, not a large function.
        struct { const char *name; unsigned int rva; int len; void *detour; void **tramp; }
        passHooks[] = {
            { "FUN_00ac5ee0", 0x00ac5ee0 - 0x00400000, 9, (void *)Detour_msSchedule,    &g_trampoline_msSchedule },
            { "FUN_00ac5f50", 0x00ac5f50 - 0x00400000, 9, (void *)Detour_msPropagation, &g_trampoline_msPropagation },
            { "FUN_00ac6890", 0x00ac6890 - 0x00400000, 6, (void *)Detour_msDepth,       &g_trampoline_msDepth },
            { "FUN_00ac6b00", 0x00ac6b00 - 0x00400000, 6, (void *)Detour_msShadow,      &g_trampoline_msShadow },
            { "FUN_00ac6e50", 0x00ac6e50 - 0x00400000, 5, (void *)Detour_ms,            &g_trampoline_ms },
            { "FUN_00ac7030", 0x00ac7030 - 0x00400000, 9, (void *)Detour_filter,        &g_trampoline_filter },
            { "FUN_00ac6150", 0x00ac6150 - 0x00400000, 9, (void *)Detour_menu,          &g_trampoline_menu },
            { "FUN_00ab7810", 0x00ab7810 - 0x00400000, 6, (void *)Detour_backbuf,       &g_trampoline_backbuf },
        };
        // Screen-space buffer allocator - the provenance gate for ShadowBufPct.
        // Prologue 55 8B EC 83 EC 10, clean 6-byte boundary.
        HookedFunc sbHf;
        sbHf.name = "FUN_00b00f10";
        sbHf.rva = 0x00b00f10 - 0x00400000;
        sbHf.target = base + sbHf.rva;
        sbHf.patchLen = 6;
        int sbOk = InstallJmpHook(&sbHf, (void *)Detour_sbufAlloc, &g_trampoline_sbufAlloc);
        sprintf(line, "[shadowbuf] screen-buffer allocator hook @ 0x%08X: %s",
                (unsigned int)sbHf.target, sbOk ? "installed" : "FAILED");
        LogLine(line);

        // Screen-set rebuild gate - kills the per-frame rebuild loop that
        // descriptor-mode SSAA otherwise provokes. Prologue 56 8B F1 8B 4E 6C
        // (PUSH ESI / MOV ESI,ECX / MOV ECX,[ESI+0x6c]) - 6 bytes, verified
        // against the shipped exe, no relative operands.
        HookedFunc rbHf;
        rbHf.name = "FUN_00b00810";
        rbHf.rva = 0x00b00810 - 0x00400000;
        rbHf.target = base + rbHf.rva;
        rbHf.patchLen = 6;
        int rbOk = InstallJmpHook(&rbHf, (void *)Detour_screenSetRebuild,
                                  &g_trampoline_screenSetRebuild);
        sprintf(line, "[ssaa] screen-set rebuild gate @ 0x%08X: %s",
                (unsigned int)rbHf.target, rbOk ? "installed" : "FAILED");
        LogLine(line);

        // Texture factory entry probe. patchLen 5: the prologue is
        // 55 8B EC 6A FF (PUSH EBP / MOV EBP,ESP / PUSH -1) - verified against
        // the shipped exe, NOT the bare SEH form assumed first. Exactly 5
        // bytes, no relative operands.
        HookedFunc tfHf;
        tfHf.name = "FUN_00a94770";
        tfHf.rva = 0x00a94770 - 0x00400000;
        tfHf.target = base + tfHf.rva;
        tfHf.patchLen = 5;
        int tfOk = InstallJmpHook(&tfHf, (void *)Detour_texFactory, &g_trampoline_texFactory);
        sprintf(line, "[halfres] texture-factory probe @ 0x%08X: %s",
                (unsigned int)tfHf.target, tfOk ? "installed" : "FAILED");
        LogLine(line);

        // TextureImp constructor - names the code that decided each texture's
        // dimensions, one level above the engine's generic creation path.
        HookedFunc tiHf;
        tiHf.name = "FUN_00aa3ce0";
        tiHf.rva = TEXIMP_CTOR_RVA;
        tiHf.target = base + TEXIMP_CTOR_RVA;
        tiHf.patchLen = 6;
        int tiOk = InstallJmpHook(&tiHf, (void *)Detour_texImpCtor, &g_trampoline_texImpCtor);
        sprintf(line, "[halfres] TextureImp ctor probe @ 0x%08X: %s",
                (unsigned int)tiHf.target, tiOk ? "installed" : "FAILED");
        LogLine(line);

        // sizeof-derived, NOT a literal: this loop was hardcoded to 6 and the
        // two handlers added for the SSAA probe (DRAW_MENU, DRAW_BACK_BUFFER)
        // compiled into the table above but were silently never installed -
        // the log showed six "tag hook ... installed" lines and no failure,
        // so it read as success. A count that cannot drift from the table is
        // the only version of this that stays correct when the table grows.
        for (int ph = 0; ph < (int)(sizeof(passHooks) / sizeof(passHooks[0])); ph++) {
            HookedFunc phf;
            phf.name = passHooks[ph].name;
            phf.rva = passHooks[ph].rva;
            phf.target = base + passHooks[ph].rva;
            phf.patchLen = passHooks[ph].len;
            int ok2 = InstallJmpHook(&phf, passHooks[ph].detour, passHooks[ph].tramp);
            sprintf(line, "[pass] tag hook %s @ 0x%08X (len %d): %s",
                    passHooks[ph].name, (unsigned int)phf.target,
                    passHooks[ph].len, ok2 ? "installed" : "FAILED");
            LogLine(line);
        }
    }
    // FN_A01A00/FN_A015B0 remain DISABLED pending a safer approach (either
    // a verified SEH-safety check, or the non-return-hijacking entry-only
    // technique instead) - not worth re-risking a crash on these two until
    // that exists.
    ok[FN_A01A00] = 0;
    ok[FN_A015B0] = 0;
    // Shadow projection: LiSPSM fallback threshold redirect (28_shadow_proj.c).
    InstallProjModeHook(base);
    InstallNpcPoolPatch(base);
    // FUN_00a41570 (v17): confirmed safe via the same "no inbound refs into
    // the prologue, standard PUSH EBP/MOV EBP,ESP/SUB ESP prologue" check
    // that has been correct for every hook except FN_A01A00/FN_A015B0 (which
    // both have SEH), so this one is enabled.
    ok[FN_A41570] = InstallJmpHook(&g_funcs[FN_A41570], (void *)Detour_a41570, &g_trampoline_a41570);
    // FUN_00aa7850 (v17b): shader-compile queue drain, safety-checked the
    // same way as FN_A41570 (standard prologue, no SEH, no external inbound
    // refs into the patched bytes).
    ok[FN_AA7850] = InstallJmpHook(&g_funcs[FN_AA7850], (void *)Detour_aa7850, &g_trampoline_aa7850);
    // FUN_00b46c20 (v18): heap compactor, promoted to the dominant remaining
    // stutter source by the 2026-08-15 Wild Lands / Dead Dunes run (108/222
    // watchdog records, the whole Ruffian-entry storm). Safety survey in
    // ghidra_output/defrag_hook.txt: standard prologue, clean 6-byte cut,
    // no SEH, no inbound refs into the stolen bytes.
    ok[FN_B46C20] = InstallJmpHook(&g_funcs[FN_B46C20], (void *)Detour_b46c20, &g_trampoline_b46c20);

    // FN_AA3250 is installed separately by InstallUploadGateHook (below), so
    // it reports its own line rather than joining this one.
    ok[FN_AA3250] = 0;
    // [boot]-tagged so it survives the release log filter. Whether the engine
    // hooks attached is the first thing worth knowing about any report, and
    // ac3040 in particular is the frame tick that the per-frame work rides on
    // - an untagged line meant a silent install failure was invisible in every
    // shipped log.
    sprintf(line, "[boot] hooks installed: aacf10=%d a2ada0=%d d19a00=%d ac3040=%d a01a00=%d a015b0=%d a41570=%d aa7850=%d b46c20=%d",
            ok[0], ok[1], ok[2], ok[3], ok[4], ok[5], ok[6], ok[7], ok[8]);
    LogLine(line);

    int csOk = InstallCriticalSectionHook();
    HookRegNote("EnterCriticalSection IAT", csOk);
    sprintf(line, "EnterCriticalSection IAT hook installed: %d", csOk);
    LogLine(line);

    int wfsoOk = InstallWfsoHook();
    HookRegNote("WaitForSingleObject IAT", wfsoOk);
    sprintf(line, "WaitForSingleObject IAT hook installed: %d", wfsoOk);
    LogLine(line);

#if ENABLE_D3DX_DIAG
    int d3dxOk = InstallD3dxDiagHooks();
    sprintf(line, "[d3dx] IAT timing hooks installed: %d of 9", d3dxOk);
    LogLine(line);
#endif
#if ENABLE_CRASH_LOG
    InstallCrashLogVeh();
#endif
#if ENABLE_UPLOAD_GATE
    {
        int ugOk = InstallUploadGateHook();
        sprintf(line, "[upload] texture-upload gate census hook: %s",
                ugOk ? "installed" : "FAILED");
        LogLine(line);
        int tcOk = InstallTexCreateHook();
        sprintf(line, "[texcreate] DDS create census hook: %s",
                tcOk ? "installed" : "FAILED");
        LogLine(line);
    }
#endif

    int raiseOk = InstallRaiseExceptionHook();
    HookRegNote("RaiseException IAT", raiseOk);
    sprintf(line, "RaiseException IAT hook installed: %d", raiseOk);
    LogLine(line);

    int idealOk = InstallIdealProcessorHook();
    HookRegNote("SetThreadIdealProcessor IAT", idealOk);
    sprintf(line, "SetThreadIdealProcessor IAT hook installed: %d", idealOk);
    LogLine(line);

    // Install alloc tracking BEFORE the throttle call-site patch, so there's
    // no window where dispatches are already routed through our wrapper
    // (which calls Begin/EndDispatchTracking) while HeapAlloc/VirtualAlloc
    // aren't hooked yet - a miss there would just mean an untracked
    // allocation, not a crash, but there's no reason to accept the gap.
    int allocTrackOk = InstallAllocTrackingHooks();
    HookRegNote("OS allocator IAT", allocTrackOk);
    sprintf(line, "HeapAlloc/VirtualAlloc tracking hooks installed: %d", allocTrackOk);
    LogLine(line);

    // The engine's own named-heap allocator - installed before the throttle
    // call-site patch for the same reason as the OS-allocator hooks above
    // (no window where dispatches route through our wrapper before this is
    // ready to catch what it calls).
    int allocatorHookOk = InstallAllocatorHook();
    HookRegNote("FUN_00b454a0 (named heap)", allocatorHookOk);
    sprintf(line, "FUN_00b454a0 (named-heap allocator) hook installed: %d", allocatorHookOk);
    LogLine(line);

    // v12: runs here on the deferred thread, not from DllMain - the
    // throwaway-device probe actually calls into d3d9.dll, which carries a
    // real deadlock risk from DllMain (see ProbeD3D9ForVtable's comment).
    int d3dOk = InstallD3D9Hook();
    HookRegNote("D3D9 device vtable", d3dOk);
    sprintf(line, "D3D9 instrumentation installed (throwaway-device vtable probe): %d", d3dOk);
    LogLine(line);

    InstallForceImmediatePresentHook();

    int throttleOk = InstallLoaderThrottle();
    HookRegNote("loader dispatch throttle", throttleOk);
    sprintf(line, "Loader dispatch throttle installed (max=%d concurrent): %d", LOADER_THROTTLE_MAX, throttleOk);
    LogLine(line);

    int shaderIdOk = InstallShaderIdentityHook();
    HookRegNote("shader identity capture", shaderIdOk);
    sprintf(line, "Shader identity capture installed (entry-only, SEH-safe): %d", shaderIdOk);
    LogLine(line);

    int anyOk = d3dOk | csOk | wfsoOk | raiseOk | idealOk | throttleOk | allocTrackOk | allocatorHookOk | shaderIdOk;
    for (int i = 0; i < NUM_FNS; i++) anyOk |= ok[i];
    if (anyOk) {
        CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
        CreateThread(NULL, 0, StutterWatchdogThread, NULL, 0, NULL);
    }
}
