// ---- Shadow projection mode ---------------------------------------------
// History (2026-09-14, POST_BETA_PLAN.md "whole shadow drops to a third of
// its resolution"): the cascade watch showed both cascades flipping between
// a perspective light matrix and a uniform one at a camera-dependent point.
// First suspect was the 0.99 |cos| compare in the LiSPSM builder
// (FUN_00a87150) - redirected, installed, changed nothing: this build runs
// builder #1, not LiSPSM. Builder #4 (LiSPSM) was then tried by writing the
// selector field [DAT_05107a00+0x390] = 3: flashing and shifting shadows -
// dormant, broken code in this build. Both retired; only the decision hook
// below remains. The old ShadowLispsmCos key is ignored if present.
// ---- Shadow projection mode: hook the perspective-vs-uniform decision --------
// The 0.99 redirect (retired, see the header) installed and changed nothing:
// that compare is inside the LiSPSM builder (mode 4), and this build runs
// mode 1 (`[DAT_05107a00+0x390]` = 0 -> FUN_00a8cfb0 -> FUN_00a8c190). The
// real switch is there:
//
//   useUniform = FUN_00a89170(camera, lightDir);      // cdecl, returns 0/1
//   if (!useUniform) FUN_00a898d0(...)  // perspective shadow map (8 KB)
//   else             FUN_00a8b970(...)  // uniform ortho fit
//
//   FUN_00a89170: project lightDir and -lightDir through the camera;
//     inside(d) = w <= eps || (|x| < w && |y| < w)     -- in the view frustum
//     return inside(light) && inside(-light)
//
// So the uniform fallback engages whenever the sun's direction point or the
// anti-sun point is inside the screen rectangle - the whole FOV cone, which
// is why "the camera isn't aligned with the sun" and yet it flips. The
// fallback is legitimate (a perspective warp with its centre inside the
// frustum inverts), and two different projections cannot be blended, so
// the only way to remove the flip is to pick one projection and keep it.
// Mode 1 forces uniform: the near cascade stays at its ~44-unit fit, which
// at ShadowMapRes 8192 is ~186 texels/unit - roughly what the perspective
// fit gives at 2048 - and never changes with the camera. SHIPS AS DEFAULT
// (user decision 2026-09-14: lower density beats the pop). Ini + the PCSS
// tuning window only; deliberately no game-menu group.
//
// Prologue 53 8B DC 83 EC 08 (PUSH EBX / MOV EBX,ESP / SUB ESP,8): 6 bytes,
// the same clean boundary as FUN_00a32a00 in 03. cdecl: the caller does
// `add esp`, so the detour's own `ret` is correct.
#define PROJ_DECIDE_RVA (0x00a89170 - 0x00400000)
static void *g_trampoline_projDecide = NULL;
// g_projDecideHooked lives in 03_render_state.c (status panel reads it).

__declspec(naked) void Detour_projDecide(void)
{
    __asm {
        cmp dword ptr [g_shadowProjMode], 1
        je  force_uniform
        cmp dword ptr [g_shadowProjMode], 2
        je  force_perspective
        jmp dword ptr [g_trampoline_projDecide]
    force_uniform:
        mov eax, 1
        ret
    force_perspective:
        xor eax, eax
        ret
    }
}

static void InstallProjModeHook(unsigned char *base)
{
    static const unsigned char expect[6] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 };
    HookedFunc hf;
    char l[160];
    memset(&hf, 0, sizeof(hf));
    hf.name = "FUN_00a89170";
    hf.rva = PROJ_DECIDE_RVA;
    hf.target = base + PROJ_DECIDE_RVA;
    hf.patchLen = 6;
    if (memcmp(hf.target, expect, 6) != 0) {
        LogLine("[lispsm] projection-decision prologue mismatch - ShadowProjMode link skipped");
        HookRegNote("FUN_00a89170 (shadow proj)", 0);
        return;
    }
    g_projDecideHooked = InstallJmpHook(&hf, (void *)Detour_projDecide, &g_trampoline_projDecide);
    sprintf(l, "[lispsm] projection-decision link %s (ShadowProjMode=%ld: %s)",
            g_projDecideHooked ? "installed" : "FAILED", g_shadowProjMode,
            g_shadowProjMode == 1 ? "always uniform" : g_shadowProjMode == 2 ? "always perspective" : "engine");
    LogLine(l);
}
