// ---- D3DX call attribution (v19 diagnostic) ------------------------------
// The 2026-08-15 census (post-compactor-mitigation Ruffian runs) left d3dx9
// texture work as the largest untraced family: ~15% of watchdog captures,
// EIPs clustered around d3dx9_43.dll+DFF14..+E0457, small reads (~128KB),
// single-alloc frames - shaped like per-texture processing during streaming.
// The exe imports exactly nine D3DX functions. Two are the likely payload:
//   D3DXLoadSurfaceFromMemory        CPU format conversion at upload
//   D3DXCreateTextureFromFileInMemoryEx  full decode + mip chain
// and one would be a headline if it ever shows up hot on the main thread:
//   D3DXCompileShader                runtime HLSL -> bytecode
// This part wraps all nine via the same IAT patch used for the kernel32
// hooks: per-call timing, per-function counters for the monitor, and an
// individual log line (capped) for any call over 1ms, with the CALLER's
// return address as a Ghidra VA so the engine-side call site falls out of
// the log directly - no guessing which system owns it.
//
// Wrappers forward untyped DWORD args with the exact stdcall arity of each
// import; nothing is interpreted, so behaviour is bit-identical. All state
// is additive counters - there is no mode where this changes what D3DX does.

#if ENABLE_D3DX_DIAG

// D3dxFn / DX_* / g_d3dxFns are declared in 03_render_state.c - the monitor
// (part 11) prints the per-window line and compiles before this part.

static volatile LONG g_d3dxSlowLogged = 0;
#define D3DX_SLOW_USEC     1000
#define D3DX_SLOW_LOG_CAP  100

static void D3dxAccount(int idx, LONG usec, void *retAddr)
{
    D3dxFn *f = &g_d3dxFns[idx];
    InterlockedIncrement(&f->calls);
    InterlockedExchangeAdd(&f->sumUsec, usec);
    if (usec > f->maxUsec) f->maxUsec = usec;
    if (usec >= D3DX_SLOW_USEC) {
        InterlockedIncrement(&f->slowCalls);
        if (g_d3dxSlowLogged < D3DX_SLOW_LOG_CAP) {
            InterlockedIncrement(&g_d3dxSlowLogged);
            char line[192];
            DWORD rva = (DWORD)(ULONG_PTR)retAddr;
            if (g_mainModBase && rva > (DWORD)g_mainModBase)
                rva = rva - (DWORD)g_mainModBase + 0x00400000;
            sprintf(line, "[d3dx] %s took %ldus  caller=0x%08lX  main_thread=%d",
                    f->name, usec, rva,
                    (LONG)GetCurrentThreadId() == g_mainThreadId);
            LogLine(line);
        }
    }
}

// One macro per arity. _ReturnAddress() gives the game's call site.
#define D3DX_WRAP(idx, name, arity, arglist, passlist)                        \
    typedef DWORD (WINAPI *PFN_##name) arglist;                               \
    static DWORD WINAPI Hooked_##name arglist                                 \
    {                                                                         \
        LONG t0 = NowUsec();                                                  \
        DWORD r = ((PFN_##name)g_d3dxFns[idx].real) passlist;                 \
        D3dxAccount(idx, NowUsec() - t0, _ReturnAddress());                   \
        return r;                                                             \
    }

D3DX_WRAP(DX_CompileShaderFromFileA, D3DXCompileShaderFromFileA, 9,
          (DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9),
          (a1, a2, a3, a4, a5, a6, a7, a8, a9))
D3DX_WRAP(DX_GetShaderConstantTable, D3DXGetShaderConstantTable, 2,
          (DWORD a1, DWORD a2), (a1, a2))
D3DX_WRAP(DX_DebugMute, D3DXDebugMute, 1,
          (DWORD a1), (a1))
D3DX_WRAP(DX_CompileShader, D3DXCompileShader, 10,
          (DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9, DWORD a10),
          (a1, a2, a3, a4, a5, a6, a7, a8, a9, a10))
D3DX_WRAP(DX_GetPixelShaderProfile, D3DXGetPixelShaderProfile, 1,
          (DWORD a1), (a1))
D3DX_WRAP(DX_GetVertexShaderProfile, D3DXGetVertexShaderProfile, 1,
          (DWORD a1), (a1))
D3DX_WRAP(DX_LoadSurfaceFromMemory, D3DXLoadSurfaceFromMemory, 10,
          (DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9, DWORD a10),
          (a1, a2, a3, a4, a5, a6, a7, a8, a9, a10))
D3DX_WRAP(DX_LoadVolumeFromMemory, D3DXLoadVolumeFromMemory, 11,
          (DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9, DWORD a10, DWORD a11),
          (a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11))
D3DX_WRAP(DX_CreateTextureFromFileInMemoryEx, D3DXCreateTextureFromFileInMemoryEx, 14,
          (DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9, DWORD a10, DWORD a11, DWORD a12, DWORD a13, DWORD a14),
          (a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14))

static int InstallD3dxDiagHooks(void)
{
    static const char *importNames[DX_COUNT] = {
        "D3DXCompileShaderFromFileA", "D3DXGetShaderConstantTable",
        "D3DXDebugMute", "D3DXCompileShader", "D3DXGetPixelShaderProfile",
        "D3DXGetVertexShaderProfile", "D3DXLoadSurfaceFromMemory",
        "D3DXLoadVolumeFromMemory", "D3DXCreateTextureFromFileInMemoryEx",
    };
    static void *hooks[DX_COUNT] = {
        (void *)Hooked_D3DXCompileShaderFromFileA,
        (void *)Hooked_D3DXGetShaderConstantTable,
        (void *)Hooked_D3DXDebugMute,
        (void *)Hooked_D3DXCompileShader,
        (void *)Hooked_D3DXGetPixelShaderProfile,
        (void *)Hooked_D3DXGetVertexShaderProfile,
        (void *)Hooked_D3DXLoadSurfaceFromMemory,
        (void *)Hooked_D3DXLoadVolumeFromMemory,
        (void *)Hooked_D3DXCreateTextureFromFileInMemoryEx,
    };
    HMODULE hMain = GetModuleHandleA(NULL);
    int installed = 0;
    for (int i = 0; i < DX_COUNT; i++) {
        void *orig = PatchIat(hMain, "d3dx9_43.dll", importNames[i], hooks[i]);
        if (orig) { g_d3dxFns[i].real = orig; installed++; }
    }
    return installed;
}

#endif // ENABLE_D3DX_DIAG
