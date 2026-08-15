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

__declspec(noinline) void __cdecl OnEnter_uploadGate_C(DWORD thisPtr, DWORD srcFmt)
{
    __try {
        LONG w   = *(LONG *)(thisPtr + 0x10);
        LONG h   = *(LONG *)(thisPtr + 0x14);
        LONG dst = *(LONG *)(thisPtr + 0x20);
        LONG src = (LONG)srcFmt;

        // Sanity: a garbage `this` would otherwise poison the census.
        if (w <= 0 || w > 32768 || h <= 0 || h > 32768) return;

        int npot = ((w & (w - 1)) != 0) || ((h & (h - 1)) != 0);
        int fmt  = (dst != src);

        InterlockedIncrement(&g_ugTotal);
        if (!npot && !fmt)     InterlockedIncrement(&g_ugFast);
        else if (npot && fmt)  InterlockedIncrement(&g_ugBoth);
        else if (npot)         InterlockedIncrement(&g_ugNpot);
        else                   InterlockedIncrement(&g_ugFmt);

        if (!npot && !fmt) return;

        // Distinct slow shapes, logged once each - a handful of lines that
        // say exactly which textures pay and why, instead of a flood.
        LONG n = g_ugComboCount;
        if (n > UG_COMBO_MAX) n = UG_COMBO_MAX;
        for (LONG i = 0; i < n; i++)
            if (g_ugCombos[i].w == w && g_ugCombos[i].h == h &&
                g_ugCombos[i].dst == dst && g_ugCombos[i].src == src) return;
        if (n >= UG_COMBO_MAX) return;
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
}

// Entry-only: reads arguments, never rewrites the return address. ECX (this,
// __thiscall) and EDX are preserved around our own call.
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
        push eax                ; arg2: srcFmt
        push ecx                ; arg1: thisPtr
        call OnEnter_uploadGate_C
        add esp, 8
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_uploadGate]
    }
}

static int InstallUploadGateHook(void)
{
    if (!g_mainModBase) return 0;
    HookedFunc hf;
    hf.name = "FUN_00aa3250";
    hf.rva = UPLOAD_GATE_RVA;
    hf.target = (void *)(g_mainModBase + UPLOAD_GATE_RVA);
    // 55 8B EC 83 EC 48 - clean 6-byte boundary, no SEH, no inbound refs
    // (verified in ghidra_output/upload_gate.txt).
    hf.patchLen = 6;
    return InstallJmpHook(&hf, (void *)Detour_uploadGate, &g_trampoline_uploadGate);
}

#endif // ENABLE_UPLOAD_GATE
