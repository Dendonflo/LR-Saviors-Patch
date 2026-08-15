// ---- Config file persistence ----------------------------------------------
// Plain "Key=Value" text file next to the exe - no library needed, trivial
// to hand-edit if something ever needs fixing outside the game. Loaded once
// at startup (before hooks are installed, so the very first frame already
// reflects the saved state) and saved every time a flag changes, from
// whichever control path changed it (hotkey or GUI checkbox).
static void GetConfigPath(char *outPath, size_t outSize)
{
    GetModuleFileNameA(NULL, outPath, (DWORD)outSize);
    char *slash = strrchr(outPath, '\\');
    if (slash) strcpy(slash + 1, MOD_CONFIG_FILE);
}

// ---- Toggleable fixes (GUI checkbox only - hotkeys removed) ---------------
// Used to also carry a vkey and a GetAsyncKeyState poll for a hotkey per
// entry. Dropped: with nine toggles the F-key list had gotten long, and the
// checkboxes are the only control actually used - one interaction surface
// instead of two that had to be kept in sync.
// One entry per toggleable fix - the GUI panel below iterates this list, so
// adding a future fix's toggle is one line here rather than touching
// multiple places.
typedef struct {
    volatile LONG *flag;
    const char *label;      // shown in the GUI checkbox and log lines
    const char *key;        // config-file key; load and save iterate this too
} ToggleableFix;

static ToggleableFix g_toggles[] = {
    { &g_discardFixEnabled,     "DISCARD fix (cause 1 - LockRect)", "DiscardFix" },
    { &g_shaderThrottleEnabled, "Shader-queue budget throttle", "ShaderThrottle" },
    { &g_readPaceEnabled,       "ReadFile rate limiter", "ReadPace" },
    { &g_loaderThrottleEnabled, "Loader dispatch throttle + priority", "LoaderThrottle" },
#if ENABLE_ALLOCATOR_WARM
    { &g_allocatorWarmEnabled,  "Allocator prefetch/warmer", "AllocatorWarm" },
#endif
    { &g_stagingUploadEnabled,  "SYSTEMMEM staging upload (EXPERIMENTAL)", "StagingUpload" },
    { &g_stagingSurfaceEnabled, "SYSTEMMEM staging for Surface::LockRect (EXPERIMENTAL)", "StagingSurface" },
    { &g_stagingCubeEnabled,    "SYSTEMMEM staging for CubeTexture::LockRect (EXPERIMENTAL)", "StagingCube" },
    { &g_hdTexPushEnabled,      "Push staged texture uploads to the HD GUI mod (interop)", "HDTexPush" },
#if ENABLE_CLAMP_DEADLINE
    { &g_clampDeadlineEnabled,  "Clamp frame-limiter deadline (no catch-up) (EXPERIMENTAL)", "ClampDeadline" },
#endif
#if ENABLE_DEFER_UPLOADS
    { &g_deferUploadsEnabled,   "Defer/rate-limit GPU uploads (EXPERIMENTAL)", "DeferUploads" },
#endif
#if ENABLE_TEXTURE_POOL
    { &g_texturePoolEnabled,   "Texture pool - reuse released textures (EXPERIMENTAL)", "TexturePool" },
#endif
    { &g_forceStdD3D9,         "Force plain D3D9 instead of D3D9Ex (EXPERIMENTAL, may not boot)", "ForceStdD3D9" },
#if ENABLE_MANAGED_POOL
    { &g_managedPoolEnabled,   "D3DPOOL_MANAGED textures (needs ForceStdD3D9)", "ManagedPool" },
#endif
#if ENABLE_TIMER_RES
    { &g_timerResEnabled,      "Force 1ms timer resolution (frame-limiter oversleep)", "TimerRes" },
#endif
#if ENABLE_CASCADE_HUNT
    { &g_logShaderConsts,      "Log shader constants once per register (cascade hunt)", "LogShaderConsts" },
    { &g_traceSplitCall,       "Trace shadowSplitRange upload call site (cascade hunt)", "TraceSplitCall" },
#endif
    { &g_overlayEnabled,        "Frametime graph overlay (engine tick)", "GraphOverlay" },
    { &g_statusEnabled,         "Status panel (settings vs applied state)", "StatusPanel" },
#if ENABLE_FRAMETIME_DUMP
    { &g_logFrameTimes,         "Dump raw frametimes to log (period diagnostic)", "LogFrameTimes" },
#endif
#if ENABLE_SHADOWS_OFF
    { &g_shadowsOff,            "Skip shadow RENDER (diagnostic - keeps the pass alive)", "ShadowsOff" },
#endif
    // Label corrected 2026-08-15: this ships ON and has since it was
    // confirmed (39% faster, the 9-frame square wave gone, user-verified,
    // no visual issues) - "EXPERIMENTAL" on a default-on setting was a
    // contradiction, and "may tear" was wrong outright: it removes a CPU/GPU
    // serialising fence, it does not change presentation. Tearing is
    // ForceImmediatePresent's business, not this one.
    //
    // SAFETY COUPLING, do not break: the fence guaranteed the GPU had
    // finished with a buffer before the CPU touched it again, and this
    // engine locks textures without DISCARD/NOOVERWRITE (the whole reason
    // DiscardFix and the Staging* family exist). Those redirect the unsafe
    // locks into SYSTEMMEM staging, which is the plausible reason skipping
    // the fence produces no artifacts. GpuSyncSkip=1 with the staging
    // toggles OFF is the untested combination where corruption is expected
    // first - LoadConfig logs a line if it ever loads that way.
    { &g_gpuSyncSkip,           "Skip per-frame GPU fence (needs Staging* on)", "GpuSyncSkip" },
#if ENABLE_PASS_PROBE
    { &g_logPassRts,            "Log render targets per draw pass (resolution probe)", "LogPassRts" },
#endif
    // Not in the GUI: it is a log-volume switch, not a graphics setting, and
    // a player has no reason to meet it. Ini-only, default off.
    { &g_logMonitor,            "Per-window monitor telemetry to the log", "LogMonitor" },
#if ENABLE_SHADER_DIAG
    { &g_dumpShaders,           "Dump pixel shaders to shaders\\ (FXAA hunt)", "DumpShaders" },
#endif
#if ENABLE_SURFACE_DIAG
    { &g_msaaDebugClear,        "MSAA diagnostic: paint MS surface magenta", "MsaaDebugClear" },
#endif
    // Ships ON as of 2026-08-15 - see the declaration in 05_script_diag.c.
    // Tears without VRR; Graphics > Vsync > On is the one-click revert.
    { &g_forceImmediatePresentEnabled, "Force IMMEDIATE present / no vsync (tears without FreeSync/G-Sync)", "ForceImmediatePresent" },
    { &g_unlockFramerateEnabled,  "Unlock 59.94fps frame limiter (EXPERIMENTAL)", "UnlockFramerate" },
    { &g_simDeltaFix,          "Unquantise sim delta (fixes 60fps interact + stamina)", "SimDeltaFix" },
    // Cutout AA: three attempts, all dead ends, all retired together.
    //   A2cEnable     - vendor alpha-to-coverage ignored by modern drivers
    //   SsaaFoliage   - works, but 8x cost for a negligible gain
    //   FringeFoliage - cannot ramp where alpha is sub-pixel, i.e. at distance
    // Full reasoning in FEATURES.md; the bytecode transforms behind them are
    // the reusable part if this is ever revisited.
#if ENABLE_CUTOUT_AA
    { &g_a2cEnable,            "Alpha-to-coverage on cutouts (needs MSAA on)", "A2cEnable" },
    { &g_ssaaFoliage,          "Supersample foliage cutouts (needs MSAA on)", "SsaaFoliage" },
    { &g_fringeFoliage,        "Blended foliage fringe (softens cutout edges)", "FringeFoliage" },
#endif
    // Ships: substitutes a centre-tap passthrough for ps_A082B248, the game's
    // FXAA. Independent of the shader-diag tooling that found it.
    { &g_fxaaOff,              "Disable built-in FXAA", "FxaaOff" },
    // MsaaDepth1x deliberately absent - see its declaration. The experiment
    // broke occlusion AND tested a theory the SGSSAA observation has since
    // disproved, so it must not be reachable from a config file.
#if ENABLE_GYSAHL_DIAG
    { &g_logFaSchedule,        "Log FA object schedule changes (Gysahl plot bug)", "LogFaSchedule" },
    { &g_faCmpFix,             "Repair empty plot-name string after planting (Gysahl FIX)", "FaCmpFix" },
    { &g_watchPlantBuf,        "Hardware watchpoint on the plot-name buffer (diagnostic)", "WatchPlantBuf" },
#endif
};
#define NUM_TOGGLES (sizeof(g_toggles) / sizeof(g_toggles[0]))

// Numeric settings. Separate from g_toggles because that table coerces every
// value to 0/1; these need their real magnitude preserved.
typedef struct {
    volatile LONG *val;
    const char *key;
    LONG lo, hi;
} NumericSetting;

// See the AdvancedMenu table entry below. Lives here (not in g_toggles) so
// the GUI panel never grows a checkbox for it.
static volatile LONG g_advancedMenu = 0;

static NumericSetting g_numerics[] = {
    // Upper bound raised to 1,000,000 so the default (1s = watchdog off in
    // practice) is actually representable; it used to clamp at 200ms.
    { &g_stutterThresholdUsec, "ThresholdUs", 4000, 1000000 },
    // 0 = unlocked, otherwise fps*100 (6000 = 60.00fps). Only takes effect
    // while UnlockFramerate is also on - see ApplyFramerateUnlock.
    { &g_targetFpsX100, "TargetFpsX100", 0, 24000 },
#if ENABLE_DEFER_UPLOADS
    // NOTE: the GUI's defer edit box reaches this entry as g_numerics[2], so it
    // must stay third while it exists, and its handler is gated in step with it.
    { &g_deferPerFrame, "DeferPerFrame", 1, 512 },
#endif
#if ENABLE_SHADOW_SCALE
    // Shadow map resolution multiplier. 1 = untouched (default). Only takes
    // effect at texture-creation time, i.e. on load - there is deliberately
    // no GUI control because hot-toggling it could not do anything, so the
    // config file IS the interface for this one.
    { &g_shadowScale, "ShadowScale", 1, 4 },
#endif
    // 0 = leave alone. Otherwise the shadow map edge length the ENGINE will
    // use (1024 = its "Standard", 2048 = its "Advanced"). This is the shadow
    // feature that WORKS - it writes the engine's own resolution field so
    // texture size, light projection and PCF taps all move together.
    { &g_shadowMapRes, "ShadowMapRes", 0, 8192 },
    // Percentages applied to the engine's own cascade split distances, at the
    // SOURCE fields (see ApplyCascadeSplitSource). 0 = leave alone,
    // 100 = unchanged, 300 = push the boundary 3x further.
    // Observed stock values: near 10.0, far 79.2.
    { &g_shadowSplitNearPct, "ShadowSplitNearPct", 0, 2000 },
    { &g_shadowSplitFarPct,  "ShadowSplitFarPct",  0, 2000 },
    // Cutscene-aware revert (21_cutscene_shadow.c). Cutscenes are authored
    // against the engine's own splits, so the option above breaks shadows in
    // some of them; while a cutscene plays the split is held at the engine
    // default and the player's setting resumes afterwards.
    // CutsceneFlagMask 0 = the "playing" bit is not identified yet, which
    // leaves the whole feature inert. Run once with ENABLE_CUTSCENE_DIAG and
    // read the [cutdiag] lines to find it, then set it here - no rebuild.
    { &g_cutsceneRevert,   "CutsceneShadowRevert", 0, 1 },
    // 2 = named cut slots (cinematics only); 1 = raw CinemaController flag,
    // which also fires on dialogue and UI prompts. See g_cutsceneMode.
    { &g_cutsceneMode,     "CutsceneDetectMode",   1, 2 },
    { &g_cutsceneFlagOff,  "CutsceneFlagOffset",   0, 0x4000 },
    { &g_cutsceneFlagMask, "CutsceneFlagMask",     0, 0x7fffffff },
    // Heap-compactor deferral (v18b, see 03_render_state.c). Defer=1 arms
    // both the per-frame budget and the post-storm cooldown; BudgetUs is
    // the per-frame compaction allowance AND the single-pass size that
    // counts as a storm. EXPERIMENTAL - ini-only until a Ruffian A/B run
    // says the fragmentation risk doesn't bite.
    { &g_compactorDeferEnabled, "CompactorDefer",    0, 1 },
    { &g_compactorBudgetUs,     "CompactorBudgetUs", 100, 20000 },
    { &g_compactorCooldownFrames, "CompactorCooldown", 1, 60 },
#if ENABLE_CASCADE_HUNT
    // Near-cascade extent multiplier, percent. 0/100 = untouched,
    // 200 = twice the ground covered by the sharp cascade.
    { &g_nearCascadePct, "NearCascadePct", 0, 800 },
#endif
#if ENABLE_TALK_TIMER
    // Percentage applied to FieldTalkManager's per-frame talk-entry countdown
    // step (stock 0.05/frame, unscaled by delta time). 0 = untouched,
    // 100 = unchanged, 50 = the 30fps rate while running at 60. See
    // ApplyTalkTimerScale.
    { &g_talkTimerPct, "TalkTimerPct", 0, 400 },
#endif
    // -1 = auto: install the generic device-vtable timing thunks only when
    // d3d9.dll is Microsoft's, where they are provably inert (see the
    // g_d3d9IsThirdParty comment). 1 forces them on under a wrapper too,
    // which is the A/B that tests the DXVK-crash hypothesis directly.
    // Cap, in microseconds, on the frame limiter's monotonic Sleep-granularity
    // high-water mark. 0 = observe only (default). ~2000 is the value to try:
    // the timer probe measured real resolution at 1.0000ms, so 2ms is a
    // realistic ceiling for what Sleep(1) actually costs. Too low and the
    // limiter sleeps where it should spin and overshoots the deadline.
#if ENABLE_SPIN_GUARD
    { &g_spinGuardUs, "SpinGuardUs", 0, 20000 },
#endif
    // Screen-space shadow buffer scale, percent of the engine's own half-res
    // size. 0/100 = untouched, 200 = full presentation res, 50 = quarter res.
    // Applied at texture creation, so it takes effect on area change or
    // restart - there is no GUI control because hot-toggling could not do
    // anything, exactly like ShadowMapRes. Use 50 first: a change that makes
    // shadows obviously WORSE proves the buffer matters and that this
    // interception reaches it, which is what makes 200 believable.
    // Screen-space shadow buffer resolution, PERCENT OF SCREEN. 0 = off,
    // 50 = the engine's own default, 100 = full screen, 200 = 2x supersampled.
    // Key renamed from ShadowBufPct, which was a percentage of the engine's
    // half-res value - the same numbers meant half as much and read as though
    // 100 were "unchanged" when it was actually a 2x. A stale ShadowBufPct in
    // an old config is simply ignored rather than silently reinterpreted.
    { &g_shadowBufResPct, "ShadowBufResPct", 0, 200 },
    // MSAA sample count on the scene colour pass. 0 = off, 2/4/8 = sample
    // count (this device reports all three supported for A8R8G8B8 + D24S8).
    // EXPERIMENTAL: this substitutes a render target under the engine and
    // resolves it back, the most invasive intervention in this file. It also
    // does nothing for shader/specular shimmer - MSAA is geometry edges only.
    { &g_msaaSamples, "MsaaSamples", 0, 8 },
    // Alpha-to-coverage on top of MSAA: the alpha value becomes a per-SAMPLE
    // coverage mask, so alpha-tested planes (vegetation, fences, barriers -
    // material the game uses everywhere) get real gradient edges out of the
    // resolve instead of the alpha test's binary cutout. Vendor backdoors,
    // auto-detected: AMD 'A2M1'/'A2M0' on POINTSIZE, NVIDIA 'ATOC' on
    // ADAPTIVETESS_Y. 1 = on (mirrors the engine's own ALPHATESTENABLE while
    // the MS pair is bound), 0 = off. Does nothing when MsaaSamples is 0.
    // RETIRED with the cutout-AA work: this was the render-state-only A2C
    // attempt, superseded by the shader rewrite and dead for the same reason
    // (the vendor hack is ignored).
#if ENABLE_CUTOUT_AA
    { &g_alphaToCoverage, "AlphaToCoverage", 0, 1 },
#endif
    // Which post-filter candidate shader to kill: 0 = none, 1..N = index into
    // g_psKillCandidates (top tap-count shaders from the dump scan). The GUI
    // dropdown drives this; substitution happens at BIND time so it is fully
    // hot. With MsaaDebugClear on, the substitute is solid magenta instead of
    // a passthrough - the footprint instrument that finds the real AA pass.
#if ENABLE_SHADER_DIAG
    { &g_fxaaPick, "FxaaPick", 0, 12 },
#endif
    // Graphics_Scaling (see ApplyScalingMode): 0 = don't touch, 1 = None,
    // 2 = Standard, 3 = Advanced. The engine's own image-scaling mode, and
    // the last remaining candidate for the always-on anti-aliasing.
#if ENABLE_SCALING_MODE
    { &g_scalingMode, "ScalingMode", 0, 3 },
#endif
    // Alpha-to-coverage ramp steepness. The coverage value is
    // saturate((alpha - threshold) * sharpness + 0.5), so lower = softer,
    // wider edges; higher = closer to the original hard cutout. 8 is a
    // reasonable starting point; 1 will look obviously blurry, which is
    // useful for confirming the rewrite is actually live.
#if ENABLE_CUTOUT_AA
    { &g_a2cSharpness, "A2cSharpness", 1, 64 },
    { &g_a2cDebugVis, "A2cDebugVis", 0, 1 },
    { &g_a2cMaskTest, "A2cMaskTest", 0, 1 },
#endif
    // Identify walk: paints the selected shader magenta so it can be named on
    // screen. This is the tool that found the FXAA pass and the glyph shader
    // after four offline analyses failed - kept one #define away.
#if ENABLE_SHADER_DIAG
    { &g_psIdentify, "ShaderIdentify", 0, 16000 },
#endif
    // Supersampling, percent per axis. 100 = off. Cost is the SQUARE: 150 is
    // 2.25x the pixels, 200 is 4x. See ApplySsaaScale.
    { &g_ssaaScale, "SsaaScale", 100, 200 },
#if ENABLE_SSAA_RESWRITE
    // Only meaningful while the retired resolution-write mode exists.
    { &g_ssaaMode, "SsaaMode", 0, 1 },
#endif
    // 1 = honour the chosen resolution in borderless fullscreen: downsample to
    // it, then upscale to the display, so picking 1080p really does give a
    // (supersampled) 1080p image. 0 = present the supersampled render directly
    // at display size. See HookedStretchRect.
    { &g_ssaaOutputRes, "SsaaOutputRes", 0, 1 },
    // Frametime overlay corner: 0 TL, 1 TR, 2 BL, 3 BR. Default bottom-right.
    // 0 is still accepted here but is deliberately not offered in the menu -
    // it is where the game's menu bar starts, so it always collides.
    // Retired: the overlay is dragged now, not corner-picked. Left in the
    // table so an existing ini carrying the key still loads cleanly.
    { &g_overlayPos, "OverlayPos", 0, 3 },
    // Window positions in screen pixels; -1 = auto-place on first show.
    { &g_overlayX, "OverlayX", -1, 16384 },
    { &g_overlayY, "OverlayY", -1, 16384 },
    { &g_statusX, "StatusPanelX", -1, 16384 },
    { &g_statusY, "StatusPanelY", -1, 16384 },
    // Log file control. LogAppend=1 keeps one file across runs (developer
    // behaviour); 0 recreates it each launch (shipping). LogMaxMB bounds a
    // single run, 0 = unlimited. Both ini-only - they are not user options.
    { &g_logAppend, "LogAppend", 0, 1 },
    { &g_logMaxMB, "LogMaxMB", 0, 4096 },
    { &g_logFlushAlways, "LogFlush", 0, 1 },
    { &g_probeDeviceThunks, "ProbeDeviceThunks", -1, 1 },
    // Shows the "Optimization" popup (the raw stutter-fix toggles) in the
    // game's menu bar. Deliberately ini-only - no UI control anywhere sets
    // it: normal users get the fixes silently, and there is no visible
    // switch inviting them to turn fixes off. Testers set AdvancedMenu=1
    // by hand.
    { &g_advancedMenu, "AdvancedMenu", 0, 1 },
};
#define NUM_NUMERICS (sizeof(g_numerics) / sizeof(g_numerics[0]))

// Config load and save are driven by the SAME g_toggles table as the hotkeys
// and the GUI, so a new toggle really is one line. This is not cosmetic
// tidying: DiscardMip0 shipped once with its hotkey, its checkbox and its
// load line all wired but its SaveConfig line missing, so every toggle was
// discarded at exit and it read back 0 on the next boot. Hand-maintained
// parallel lists of the same flags in three places will keep producing that
// bug; one list cannot.
static void LoadConfig(void)
{
    char path[MAX_PATH];
    GetConfigPath(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int matched = 0;
        for (size_t i = 0; i < NUM_TOGGLES; i++) {
            char pat[64];
            int val;
            sprintf(pat, "%s=%%d", g_toggles[i].key);
            if (sscanf(line, pat, &val) == 1) { *g_toggles[i].flag = val ? 1 : 0; matched = 1; break; }
        }
        if (!matched) {
            for (size_t i = 0; i < NUM_NUMERICS; i++) {
                char pat[64];
                int val;
                sprintf(pat, "%s=%%d", g_numerics[i].key);
                if (sscanf(line, pat, &val) == 1) {
                    if (val < g_numerics[i].lo) val = g_numerics[i].lo;
                    if (val > g_numerics[i].hi) val = g_numerics[i].hi;
                    *g_numerics[i].val = val;
                    break;
                }
            }
        }
        // NoOverwriteFix is deliberately absent from the table, so a stale
        // config still carrying that key is silently ignored rather than
        // reviving a confirmed-crashing toggle.
    }
    fclose(f);
    // LogFrameTimes is a momentary "capture from here" button, not a setting,
    // but it rides the same table as the real toggles so it was being saved
    // and restored like one. Restored as 1, the monitor thread saw a rising
    // edge on its first tick and armed at STARTUP - filling all 2048 frames
    // during the title and loading screens (~1000us each, shadow pass not
    // even running) before the player reached gameplay. The capture looked
    // valid and was completely worthless. Always start disarmed.
    g_logFrameTimes = 0;
    // The GpuSyncSkip / Staging* coupling, stated once at load rather than
    // left implicit in a comment. Not a warning about a known bug - no
    // corruption has ever been observed - but this is the combination
    // FEATURES.md identifies as where it would appear first, and a user who
    // reaches it by editing the ini deserves to see that in their log
    // instead of us reconstructing it from a bug report later.
    if (g_gpuSyncSkip && !(g_stagingUploadEnabled && g_stagingSurfaceEnabled
                           && g_stagingCubeEnabled))
        LogLine("[config] NOTE: GpuSyncSkip=1 with one or more Staging* toggles OFF."
                " The per-frame GPU fence is what made this engine's un-DISCARDed"
                " locks safe; the staging redirects replace that protection."
                " This combination is untested - if you see texture corruption,"
                " turn Staging* back on or GpuSyncSkip off.");
    // 1024 and BOUNDS-CHECKED. This was char[300] and simply ran off the end
    // as options accumulated: the line is now ~700 chars, so every launch
    // smashed the stack here and the process died on return from this
    // function. The config line still reached the log first, which made the
    // crash look like it happened in whatever ran next - it cost several
    // wrong diagnoses before markers showed no code after this point ran at
    // all. The guard means adding options can never reintroduce it.
    char logLine[1024];
    int n = sprintf(logLine, "[config] loaded:");
    for (size_t i = 0; i < NUM_TOGGLES && n < (int)sizeof(logLine) - 64; i++)
        n += sprintf(logLine + n, " %s=%ld", g_toggles[i].key, *g_toggles[i].flag);
    for (size_t i = 0; i < NUM_NUMERICS && n < (int)sizeof(logLine) - 64; i++)
        n += sprintf(logLine + n, " %s=%ld", g_numerics[i].key, *g_numerics[i].val);
    LogLine(logLine);
}

static void SaveConfig(void)
{
    char path[MAX_PATH];
    GetConfigPath(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    // Header. LoadConfig matches keys by name and ignores anything that does
    // not match, so comment lines are safe - and they are worth the three
    // lines: several settings here have no menu entry at all, and an ini is
    // the only place a user can discover them.
    fprintf(f, "# " MOD_NAME " - " MOD_TAGLINE "\n");
    fprintf(f, "# Lightning Returns: Final Fantasy XIII\n");
    fprintf(f, "#\n");
    fprintf(f, "# Every setting the mod knows, with its current value.\n");
    fprintf(f, "# Most are set from the in-game menu (Graphics / Other); the rest\n");
    fprintf(f, "# are safe to edit here, with the game CLOSED - a running game\n");
    fprintf(f, "# rewrites this file from memory whenever a setting changes.\n");
    fprintf(f, "# Delete this file to restore defaults.\n\n");
    for (size_t i = 0; i < NUM_TOGGLES; i++)
        fprintf(f, "%s=%ld\n", g_toggles[i].key, *g_toggles[i].flag);
    // Numeric settings persist too. This was missing on the first attempt:
    // LoadConfig parsed ThresholdUs correctly and the GUI applied it live, but
    // nothing ever wrote it, so it silently reverted to the default every
    // launch. Exactly the same failure mode as the DiscardMip0 save line -
    // read path present, write path absent, no error anywhere.
    for (size_t i = 0; i < NUM_NUMERICS; i++)
        fprintf(f, "%s=%ld\n", g_numerics[i].key, *g_numerics[i].val);
    fclose(f);
}

