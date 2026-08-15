// ---- Texture-upload fast/slow path census (v20) ---------------------------
// D3DX pixel conversion is ~18% of watchdog captures. FUN_00aa3250 is the
// engine's texture-upload path, and it ALREADY has a memcpy fast path -
// the decompile (ghidra_output/d3dx_callers.txt) shows the gate:
//
//   slow = (width  & (width -1)) != 0     // width not a power of two
//       || (height & (height-1)) != 0     // height not a power of two
//       || this[8] != srcFormat;          // format mismatch
//
//   slow == 0 -> row-wise memcpy into the locked surface   (free)
//   slow == 1 -> D3DXLoadSurfaceFromMemory                 (the 18%)
//
// So the question is not "can we avoid conversion" but "which of these three
// conditions is firing, and on what". This answers it: an ENTRY-ONLY detour
// (arguments only, no return hijack - so the SEH question never arises)
// that reads the same four values the gate reads, classifies each upload,
// and logs distinct slow combinations once each.
//
// Read-only by construction: it inspects arguments and returns. Nothing is
// written back, no path is changed. This is deliberately NOT the IAT-wrapper
// approach that crashed the game at Load Game - it uses the same
// InstallJmpHook machinery as every other hook here.
//
// Field offsets from the decompile (param_1 is int *, so [n] = byte n*4):
//   this[4]  +0x10  width
//   this[5]  +0x14  height
//   this[8]  +0x20  destination format (engine index)
//   param_4  stack  source format (engine index)
// Engine index -> D3DFORMAT via the table at 0x021886d0 (24=DXT1, 25=DXT3,
// 26=DXT5, 21=A8R8G8B8, 22=X8R8G8B8, ... see ghidra_output/upload_gate.txt).

#if ENABLE_UPLOAD_GATE

#define UPLOAD_GATE_RVA (0x00aa3250 - 0x00400000)

static void *g_trampoline_uploadGate = NULL;

static volatile LONG g_ugTotal;
static volatile LONG g_ugFast;      // memcpy path
static volatile LONG g_ugNpot;      // slow: non-power-of-two only
static volatile LONG g_ugFmt;       // slow: format mismatch only
static volatile LONG g_ugBoth;      // slow: both

typedef struct { LONG w, h, dst, src; } UgCombo;
#define UG_COMBO_MAX 48
static UgCombo g_ugCombos[UG_COMBO_MAX];
static volatile LONG g_ugComboCount;

// Returns the gate classification (0 = memcpy path, 1 = D3DX path) so the
// caller can tag the timing frame with it.
__declspec(noinline) LONG __cdecl ClassifyUpload_C(DWORD thisPtr, DWORD srcFmt)
{
    LONG slow = 0;
    __try {
        LONG w   = *(LONG *)(thisPtr + 0x10);
        LONG h   = *(LONG *)(thisPtr + 0x14);
        LONG dst = *(LONG *)(thisPtr + 0x20);
        LONG src = (LONG)srcFmt;

        // Sanity: a garbage `this` would otherwise poison the census.
        if (w <= 0 || w > 32768 || h <= 0 || h > 32768) return 0;

        int npot = ((w & (w - 1)) != 0) || ((h & (h - 1)) != 0);
        int fmt  = (dst != src);
        slow = (npot || fmt) ? 1 : 0;

        InterlockedIncrement(&g_ugTotal);
        if (!npot && !fmt)     InterlockedIncrement(&g_ugFast);
        else if (npot && fmt)  InterlockedIncrement(&g_ugBoth);
        else if (npot)         InterlockedIncrement(&g_ugNpot);
        else                   InterlockedIncrement(&g_ugFmt);

        if (!npot && !fmt) return 0;

        // Distinct slow shapes, logged once each - a handful of lines that
        // say exactly which textures pay and why, instead of a flood.
        LONG n = g_ugComboCount;
        if (n > UG_COMBO_MAX) n = UG_COMBO_MAX;
        for (LONG i = 0; i < n; i++)
            if (g_ugCombos[i].w == w && g_ugCombos[i].h == h &&
                g_ugCombos[i].dst == dst && g_ugCombos[i].src == src) return slow;
        if (n >= UG_COMBO_MAX) return slow;
        g_ugCombos[n].w = w; g_ugCombos[n].h = h;
        g_ugCombos[n].dst = dst; g_ugCombos[n].src = src;
        g_ugComboCount = n + 1;

        // Translate both format indices through the engine's own table so
        // the line reads in D3DFORMAT terms without a lookup by hand.
        DWORD dfmt = 0, sfmt = 0;
        if (g_mainModBase) {
            DWORD tbl = (DWORD)g_mainModBase + (0x021886d0 - 0x00400000);
            if (dst >= 0 && dst < 64) dfmt = *(DWORD *)(tbl + dst * 4);
            if (src >= 0 && src < 64) sfmt = *(DWORD *)(tbl + src * 4);
        }
        {
            char line[224];
            char dtag[8], stag[8];
            // FourCC formats (DXT1/3/5) print as text, numeric ones as D3DFORMAT.
            if (dfmt > 0x20202020) { *(DWORD *)dtag = dfmt; dtag[4] = 0; }
            else sprintf(dtag, "%lu", dfmt);
            if (sfmt > 0x20202020) { *(DWORD *)stag = sfmt; stag[4] = 0; }
            else sprintf(stag, "%lu", sfmt);
            sprintf(line, "[upload] SLOW %ldx%ld dst=%ld(%s) src=%ld(%s) reason=%s",
                    w, h, dst, dtag, src, stag,
                    (npot && fmt) ? "npot+format" : (npot ? "non-power-of-2" : "format"));
            LogLine(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return slow;
}

// Entry: classify, then record a timing frame tagged with that
// classification so the return can charge the duration to the right path.
__declspec(noinline) int __cdecl OnEnter_uploadGate_C(void *retAddr, DWORD thisPtr, DWORD srcFmt)
{
    LONG slow = ClassifyUpload_C(thisPtr, srcFmt);
    return OnEnterBookkeepingTagged(retAddr, FN_AA3250, slow);
}
__declspec(noinline) void *__cdecl OnReturn_uploadGate_C(void)
{
    return OnReturnBookkeeping(FN_AA3250);
}
__declspec(naked) void OnReturn_uploadGate(void)
{
    __asm {
        pushfd
        call OnReturn_uploadGate_C
        popfd
        jmp eax
    }
}

// ECX (this, __thiscall) and EDX are preserved around our own call. The
// return hijack is safe here on the same criteria as every other timed hook:
// no SEH prologue (verified in ghidra_output/upload_gate.txt), and it is
// conditional on OnEnter's result so a full per-thread stack leaves the true
// return address alone.
//   after push ecx / push edx:
//     [esp+0x08] = return address
//     [esp+0x0C] = param_2   [esp+0x10] = param_3
//     [esp+0x14] = param_4   <- source format
__declspec(naked) void Detour_uploadGate(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 0x14]   ; param_4 = source format
        push eax                ; arg3: srcFmt
        push ecx                ; arg2: thisPtr
        mov eax, [esp + 0x10]   ; return address (0x08 + the two pushes above)
        push eax                ; arg1: trueRetAddr
        call OnEnter_uploadGate_C
        add esp, 12
        test eax, eax
        jz skip_uploadGate
        mov dword ptr [esp + 8], offset OnReturn_uploadGate
    skip_uploadGate:
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_uploadGate]
    }
}

// ---- DDS texture-create census (v21) --------------------------------------
// CORRECTION to the v20 attribution. The upload gate above turned out to be
// cheap - 1458 slow uploads cost 8ms across an entire run - and the watchdog
// scans settle where the d3dx family actually lives: every d3dx capture
// carries 00AA2776 (the return address of the D3DXCreateTextureFromFileInMemoryEx
// call in FUN_00aa2710) and NOT ONE carries 00AA346C (the
// D3DXLoadSurfaceFromMemory site). The cost is whole-texture DDS decode, not
// per-surface upload.
//
// FUN_00aa2710's call, from the decompile:
//   D3DXCreateTextureFromFileInMemoryEx(device, pSrcData,
//       D3DX_DEFAULT /*SrcDataSize*/, D3DX_DEFAULT /*Width*/,
//       D3DX_DEFAULT /*Height*/, 1 /*MipLevels*/, 0 /*Usage*/,
//       D3DFMT_UNKNOWN /*Format*/, 2 /*Pool=SYSTEMMEM*/,
//       D3DX_DEFAULT /*Filter*/, D3DX_DEFAULT /*MipFilter*/, ...)
//
// D3DX_DEFAULT for Width/Height is documented to round the image UP TO A
// POWER OF TWO, and D3DX_DEFAULT for Filter is TRIANGLE|DITHER - so a
// non-power-of-two DDS would be fully resampled with an expensive filter on
// load. That is a theory, not a finding: this census records what the DDS
// header actually declares so it can be checked rather than assumed.
//
// Entry-only (FUN_00aa2710 has an SEH prologue - PUSH -1/PUSH handler/
// MOV EAX,FS:[0] - so return hijacking is out, same rule that keeps
// FN_A01A00 disabled). 5-byte patch, boundary and inbound refs verified in
// ghidra_output/texcreate_safety.txt.
//
// DDS header: +12 dwHeight, +16 dwWidth, +28 dwMipMapCount, +84 ddspf.dwFourCC
static void *g_trampoline_texCreate = NULL;
static volatile LONG g_tcTotal, g_tcNpot;
typedef struct { LONG w, h, mips, fourcc; } TcCombo;
#define TC_COMBO_MAX 48
static TcCombo g_tcCombos[TC_COMBO_MAX];
static volatile LONG g_tcComboCount;

__declspec(noinline) void __cdecl OnEnter_texCreate_C(DWORD ddsPtr)
{
    __try {
        if (!ddsPtr) return;
        if (*(DWORD *)ddsPtr != 0x20534444) return;   // not "DDS "
        LONG h      = *(LONG *)(ddsPtr + 12);
        LONG w      = *(LONG *)(ddsPtr + 16);
        LONG mips   = *(LONG *)(ddsPtr + 28);
        LONG fourcc = *(LONG *)(ddsPtr + 84);
        if (w <= 0 || w > 32768 || h <= 0 || h > 32768) return;

        int npot = ((w & (w - 1)) != 0) || ((h & (h - 1)) != 0);
        InterlockedIncrement(&g_tcTotal);
        if (npot) InterlockedIncrement(&g_tcNpot);

        LONG n = g_tcComboCount;
        if (n > TC_COMBO_MAX) n = TC_COMBO_MAX;
        for (LONG i = 0; i < n; i++)
            if (g_tcCombos[i].w == w && g_tcCombos[i].h == h &&
                g_tcCombos[i].mips == mips && g_tcCombos[i].fourcc == fourcc) return;
        if (n >= TC_COMBO_MAX) return;
        g_tcCombos[n].w = w; g_tcCombos[n].h = h;
        g_tcCombos[n].mips = mips; g_tcCombos[n].fourcc = fourcc;
        g_tcComboCount = n + 1;
        {
            char line[192], tag[8];
            if (fourcc > 0x20202020) { *(DWORD *)tag = (DWORD)fourcc; tag[4] = 0; }
            else sprintf(tag, "raw%ld", fourcc);
            // pow2_target is what D3DX_DEFAULT would round the image up to.
            LONG pw = 1, ph = 1;
            while (pw < w) pw <<= 1;
            while (ph < h) ph <<= 1;
            sprintf(line, "[texcreate] DDS %ldx%ld mips=%ld fmt=%s%s",
                    w, h, mips, tag,
                    npot ? "" : " (pow2)");
            LogLine(line);
            if (npot) {
                sprintf(line, "[texcreate]   ^ NPOT: D3DX_DEFAULT would resample to %ldx%ld"
                              " (%.1fx the pixels)", pw, ph,
                        (double)(pw * ph) / (double)(w * h));
                LogLine(line);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Entry-only, no return hijack (SEH prologue). At entry:
//   [esp] = retaddr, [esp+4] = param_1 (the DDS blob)
__declspec(naked) void Detour_texCreate(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 0x0C]   ; param_1 = pSrcData
        push eax
        call OnEnter_texCreate_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_texCreate]
    }
}

static int InstallTexCreateHook(void)
{
    if (!g_mainModBase) return 0;
    HookedFunc hf;
    hf.name = "FUN_00aa2710";
    hf.rva = 0x00aa2710 - 0x00400000;
    hf.target = (void *)(g_mainModBase + hf.rva);
    hf.patchLen = 5;   // 55 8B EC 6A FF - boundary verified
    return InstallJmpHook(&hf, (void *)Detour_texCreate, &g_trampoline_texCreate);
}

static int InstallUploadGateHook(void)
{
    if (!g_mainModBase) return 0;
    // Uses the shared g_funcs slot so the standard [monitor] line reports
    // call count and duration alongside every other timed function; the
    // fast/slow split is accumulated separately in OnReturnBookkeeping.
    g_funcs[FN_AA3250].target = (void *)(g_mainModBase + UPLOAD_GATE_RVA);
    // 55 8B EC 83 EC 48 - clean 6-byte boundary, no SEH, no inbound refs
    // (verified in ghidra_output/upload_gate.txt).
    return InstallJmpHook(&g_funcs[FN_AA3250], (void *)Detour_uploadGate,
                          &g_trampoline_uploadGate);
}

#endif // ENABLE_UPLOAD_GATE
