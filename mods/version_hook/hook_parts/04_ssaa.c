// ---- Native SSAA (supersampling), scene only ------------------------------
// Renders the SCENE above output resolution and lets the engine's own
// scene->backbuffer copy downsample it. Unlike MSAA this supersamples
// SHADING, so it is the only route here that antialiases alpha-tested
// cutouts (vegetation, fences) - the thing the entire alpha-to-coverage
// effort failed to achieve.
//
// WHY THIS WORKS AT ALL - measured, not assumed (2026-08-12 probe runs,
// FEATURES.md "Native SSAA"):
//   1. Internal render size and backbuffer size are ALREADY decoupled. The
//      game's own resolution menu moves the internal size while every device
//      Reset stays at the desktop resolution - which is exactly why the MSAA
//      identity latch had to match GetInternalRenderSize() rather than the
//      backbuffer. SSAA is that same decoupling driven UPWARD.
//   2. The UI is not in the scene target. Inside DRAW_MENU the engine blits
//      the finished scene (A8R8G8B8) into the BACKBUFFER (X8R8G8B8), then
//      binds the backbuffer and draws the UI onto it. So the downsample point
//      already exists, and the UI lands after it at native resolution - the
//      "scene only, not the UI" requirement is satisfied by the engine's own
//      frame structure, with no mid-frame resolve to engineer.
//   3. That blit is POINT. Downscaling with POINT DECIMATES - it would throw
//      away three of every four pixels and produce zero antialiasing at full
//      cost. Forcing LINEAR while scaling is therefore mandatory, not a
//      refinement; see HookedStretchRect.
//
// Written into the engine's own size field rather than by substituting larger
// surfaces underneath it. That is the approach that worked for ShadowMapRes
// and ShadowBufPct, and it avoids the ShadowScale failure mode, where the
// engine kept deriving viewports and texel maths from its own recorded
// dimensions so every consumer stayed at the old size. Here the engine
// recomputes the whole chain - scene target, depth prepass, post pyramid,
// viewports - from the number we write.
//
// Percent, applied to BOTH axes: 100 = off, 200 = 2x per axis = 4x the pixels
// and 4x the shading. Cost is the SQUARE of this number.
static volatile LONG g_ssaaScale = 100;
// 1 = descriptor mode, the only shipping implementation. The numeric entry is
// gated out with the retired mode, so nothing can set this back to 0.
static volatile LONG g_ssaaMode = 1;
static volatile LONG g_ssaaWrites = 0;
static volatile LONG g_ssaaActive = 0;      // read by the StretchRect filter force
static volatile LONG g_ssaaCurW = 0, g_ssaaCurH = 0;
static LONG g_ssaaBaseW = 0, g_ssaaBaseH = 0;    // the engine's own (unscaled) size
static LONG g_ssaaWroteW = 0, g_ssaaWroteH = 0;  // the last value WE wrote
static LONG g_ssaaLoggedW = 0, g_ssaaLoggedH = 0;
// Last values acted on, so a scale/mode change can be detected and turned into
// exactly one screen-set rebuild rather than a per-tick one.
// Initialised to MATCH the defaults, so a fresh launch never looks like a
// change. g_ssaaMode's default is 1; leaving this at 0 made every launch fire
// a spurious forced rebuild.
static LONG g_ssaaLastScale = 100, g_ssaaLastMode = 1;

// STATE 2026-08-12, after four test runs: this route WORKS where the
// presentation is clamped by the display - i.e. fullscreen at the desktop
// resolution, which is the normal way this game is played - and does nothing
// useful elsewhere. It ships in exactly that form.
//
// The root fact: `[settings]+0x10/+0x14` is the game's RESOLUTION, not an
// internal-only render size. It drives the window and swap chain as well as
// the render targets. So raising it renders bigger AND presents bigger,
// EXCEPT when the presentation physically cannot follow - fullscreen at the
// desktop resolution - where the clamp leaves render > presented, which is
// supersampling. That clamp is doing the real work, and it is dependable
// (a display cannot present more pixels than it has).
//
// TWO THINGS TRIED AND REVERTED, both kept below one #define each:
//
//  1. Anchoring the scale to the PRESENTED size instead of the resolution
//     setting (ENABLE_SSAA_PRESENT_ANCHOR). Correct in principle, disastrous
//     in practice: at a 1080p setting the engine already presents into a 4K
//     backbuffer, so 2x asked for 8K. Worse, it closed a feedback loop - our
//     write resizes the window, the window's DECORATED client area becomes
//     the new backbuffer (observed: 3764x2131, a size nobody requested), that
//     re-anchors the scale, which produces another write. 21 Resets in one
//     run, writes=17, and a window blinking about once a second.
//
//  2. Pinning the swap chain at Reset (ENABLE_SSAA_PRESENT_PIN). Never fired
//     once - pins=0 across a whole run - because it tested for our exact
//     target while the engine kept arriving at decoration-adjusted sizes. In
//     the one case it did engage it produced a 1080p swap chain inside a 4K
//     window: unfiltered pixel-doubling, and a 1:1 blit into the top-left
//     quarter with a menu open.
//
// Anchoring to the resolution setting (below) has no such loop: the base only
// changes when someone OTHER than us writes the field, so writes settle at 1.
//
// The route that would lift the fullscreen-only restriction is descriptor
// scaling - never touch this field, enlarge the render TARGETS at creation
// provenance-gated on FUN_00b00f10, as ShadowBufPct already does. See
// FEATURES.md; that is the next piece of work, not a replacement for this one.
// RETIRED 2026-08-12. Descriptor mode supersedes it completely: it works in
// windowed AND fullscreen, at any resolution setting, and honours the chosen
// resolution instead of silently overriding it. This one only ever worked
// where the display clamped the presentation, and its two attempted fixes are
// documented above. Kept compiled-out rather than deleted.
#define ENABLE_SSAA_RESWRITE       0
#define ENABLE_SSAA_PRESENT_ANCHOR 0
#define ENABLE_SSAA_PRESENT_PIN    0

static void ApplySsaaScale(void)
{
    // Descriptor mode does its work in OnTexImpCtor_C and must NOT touch the
    // resolution field - that field moving is the entire reason the retired
    // mode was fullscreen-only. g_ssaaActive tracks "a scale is applied", which
    // the downsample/output-res logic in HookedStretchRect keys off.
    //
    // This path is NOT inside the ENABLE_SSAA_RESWRITE gate: it is the shipping
    // implementation, and gating it with the retired one would silently disable
    // supersampling entirely.
    if (g_ssaaMode == 1) {
        g_ssaaActive = (g_ssaaScale != 100) ? 1 : 0;
        // A scale change only takes effect when the screen set is rebuilt, and
        // going from unscaled to scaled leaves the detector's sentinel still
        // agreeing with the settings - so it would never ask for one. Request
        // it, and make the detector's comparison fail so it actually happens.
        // Only a SCALE change matters, and only when a scale is actually being
        // applied or removed.
        //
        // This previously also compared the mode, with g_ssaaLastMode
        // initialised to 0 while g_ssaaMode defaults to 1 - so on the FIRST
        // tick of every launch it read as "the mode just changed" and forced a
        // rebuild even with SSAA switched off. That poke zeroes slot 0x21's
        // recorded dimensions, which is exactly what the MSAA identity latch
        // compares candidate render targets against: the scene target was
        // never recognised and MSAA silently did nothing
        // (subs=0 with 2.5M draws counted, i.e. hooks live, latch starved).
        // Never poke the engine's bookkeeping when there is nothing to change.
        if (g_ssaaScale != g_ssaaLastScale) {
            int wasScaled = (g_ssaaLastScale > 100);
            int isScaled  = (g_ssaaScale > 100);
            g_ssaaLastScale = g_ssaaScale;
            g_ssaaLastMode = g_ssaaMode;
            if (wasScaled || isScaled) {
                InterlockedExchange(&g_ssaaRebuildPending, 1);
                SsaaForceScreenSetRebuild();
                char cl[128];
                sprintf(cl, "[ssaa] scale changed -> forcing one screen-set rebuild (scale=%ld%%)",
                        g_ssaaScale);
                LogLine(cl);
            }
        }
        return;
    }
#if !ENABLE_SSAA_RESWRITE
    // The retired mode is compiled out; nothing else can apply a scale.
    g_ssaaActive = 0;
    return;
#else
    // Leaving descriptor mode with scaled buffers live needs the same one-shot
    // rebuild, or they stay enlarged after the feature is switched off.
    if (g_ssaaMode != g_ssaaLastMode) {
        g_ssaaLastMode = g_ssaaMode;
        g_ssaaLastScale = g_ssaaScale;
        InterlockedExchange(&g_ssaaRebuildPending, 1);
        SsaaForceScreenSetRebuild();
    }
    if (g_mainModBase == 0) return;
    __try {
        DWORD objPtr = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
        if (!objPtr) return;                    // settings object not built yet
        volatile DWORD *pw = (volatile DWORD *)(objPtr + 0x10);
        volatile DWORD *ph = (volatile DWORD *)(objPtr + 0x14);
        DWORD curW = *pw, curH = *ph;
        // Same sanity window GetInternalRenderSize uses: anything outside it
        // means this pointer is not the object expected, and writing would be
        // a blind poke into unknown memory.
        if (curW < 320 || curW > 16384 || curH < 200 || curH > 16384) return;

        // Whose value is this? Anything we did not write ourselves came from
        // the engine - including the player using the game's own resolution
        // menu - and is remembered so turning SSAA off can put it back.
        if ((LONG)curW != g_ssaaWroteW || (LONG)curH != g_ssaaWroteH) {
            g_ssaaBaseW = (LONG)curW;
            g_ssaaBaseH = (LONG)curH;
        }
        if (g_ssaaBaseW <= 0 || g_ssaaBaseH <= 0) return;

        LONG scale = g_ssaaScale;
        if (scale < 100) scale = 100;
        if (scale > 200) scale = 200;

        // Anchored to the RESOLUTION SETTING, deliberately - see the
        // ENABLE_SSAA_PRESENT_ANCHOR note above for what anchoring to the
        // presented size cost. The base only moves when someone other than us
        // writes the field, so there is no feedback path back into our own
        // input and `writes` settles at 1 instead of climbing.
        LONG wantW = g_ssaaBaseW, wantH = g_ssaaBaseH;
#if ENABLE_SSAA_PRESENT_ANCHOR
        LONG presW = (LONG)g_backbufW, presH = (LONG)g_backbufH;
        if (scale != 100 && (presW <= 0 || presH <= 0)) return;
        if (scale != 100) { wantW = presW; wantH = presH; }
#endif
        if (scale != 100) {
            // Both axes take the SAME percentage and the SAME rounding, so the
            // aspect ratio survives. That matters: the final copy stretches
            // full-surface to full-surface, so a mismatched aspect would
            // DISTORT the image rather than merely soften it.
            wantW = ((wantW * scale / 100) + 2) & ~3L;
            wantH = ((wantH * scale / 100) + 2) & ~3L;
        }

        if ((LONG)curW != wantW || (LONG)curH != wantH) {
            *pw = (DWORD)wantW;
            *ph = (DWORD)wantH;
            g_ssaaWroteW = wantW;
            g_ssaaWroteH = wantH;
            InterlockedIncrement(&g_ssaaWrites);
            // Logged only when the TARGET changes, not per write: if the
            // engine ever fights us for this field the write counter climbs
            // while the log stays quiet, which is the signal worth having.
            if (wantW != g_ssaaLoggedW || wantH != g_ssaaLoggedH) {
                g_ssaaLoggedW = wantW; g_ssaaLoggedH = wantH;
                char l[192];
                sprintf(l, "[ssaa] internal render size %lux%lu -> %ldx%ld"
                           " (scale %ld%%, engine base %ldx%ld)",
                        (unsigned long)curW, (unsigned long)curH,
                        wantW, wantH, scale, g_ssaaBaseW, g_ssaaBaseH);
                LogLine(l);
            }
        }
        g_ssaaCurW = wantW;
        g_ssaaCurH = wantH;
        g_ssaaActive = (scale != 100) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif  // ENABLE_SSAA_RESWRITE
}
