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
    // HDTexPush RETIRED as a setting 2026-08-15 - now permanently on
    // (g_hdTexPushEnabled = 1 in 02_interop_provider.c). There is no benefit
    // to switching it off and no cost to leaving it on: with the HD GUI mod
    // absent, g_hdTexNotify stays NULL and the push is one dead branch per
    // staged upload. A toggle whose "off" position helps nobody is just a
    // way for a config file to break interop silently. Old inis carrying
    // HDTexPush=0 are ignored, which is the intended outcome.
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
// Menu language for the mod's own labels. 0 = auto (Steam app manifest, then
// the OS UI language - see 08c_lang_detect.c); 1..9 force one, in LANG_*
// order: 1 en  2 fr  3 de  4 it  5 es  6 ja  7 zh-Hans  8 zh-Hant  9 ko.
static volatile LONG g_langCfg = 0;
static volatile LONG g_advancedMenu = 0;
// Config schema version of the FILE on disk. Compile-time 0 deliberately: an
// ini written before this key existed (1.0 BETA) has no line to parse, so it
// keeps this 0 and is recognised as needing migration. See CfgMigrate, which
// is where the version's meaning and every step live.
#define CONFIG_VERSION 2
static volatile LONG g_configVersion = 0;

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
    // CutsceneShadowRevert RETIRED as a setting 2026-08-15 - now permanently
    // on (g_cutsceneRevert = 1 in 03_render_state.c). It is a BUG FIX, not a
    // preference: cutscenes are authored against the engine's own cascade
    // splits, so a raised Shadow Distance breaks their shadows. Switching
    // this off restores the bug and improves nothing. It only ever needed a
    // key while the detection was being developed and could misfire; mode 2
    // (named cut slots) has been confirmed correct across many cutscenes.
    // Note it costs nothing when Shadow Distance is Standard - the neutraliser
    // only touches percentages that are actually active.
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
#if ENABLE_AO_RECON
    // Tint probe for the AO investigation (24_ao_recon.c): paints multiply
    // bands into the screen-shadow composite so its channel semantics can be
    // read off the screen. Diagnostic-build key; gone when the gate goes.
    { &g_aoTint, "AoTint", 0, 1 },
#endif
#if ENABLE_AO_SSAO
    // SSAO injection (25_ssao.c). Strength = how much of the engine's own
    // [0.5..1] shadow envelope AO may use; Radius in engine units x100,
    // tuned by eye in the debug view; Proj = cot(fovY/2) x100 - wrong values
    // show as AO stretching with screen position in the debug view.
    // AoEnable: 0=off 1=SSAO (Alchemy spiral) 2=HBAO (horizon march).
    { &g_aoEnable,      "AoEnable",      0, 2 },
    { &g_aoDebug,       "AoDebug",       0, 1 },
    // Strength past 100 pushes the AO term below the engine's own 0.5 shadow
    // floor - deeper-than-stock creases, by user request ("very aggressive").
    // Per-estimator slots: the unprefixed keys are SSAO's (so values tuned
    // before HBAO existed keep meaning what they meant), AoHbao* are HBAO's.
    { &g_aoStrengthPctE[0], "AoStrengthPct", 0, 400 },
    // Intensity is an exponent for both estimators. Ranges are deliberately
    // far wider than either reference ships (SAO 1.0, HBAO+ 1.5) - the point
    // of a tuning build is to find where a setting stops helping, which you
    // cannot do from inside the range. SAO's contrast term is clamped in the
    // shader so the curve cannot invert past 8.0.
    { &g_aoIntensityE[0], "AoIntensity100", 1, 2000 },
    { &g_aoRadiusE[0],  "AoRadius100",   1, 100000 },
    { &g_aoStrengthPctE[1], "AoHbaoStrengthPct", 0, 400 },
    { &g_aoIntensityE[1], "AoHbaoIntensity100", 1, 2000 },
    { &g_aoRadiusE[1],  "AoHbaoRadius100", 1, 100000 },
    // Bias x1000. See g_aoBiasE: a world-space distance for SSAO, an angle
    // cosine for HBAO+. Capped short of 1000 because HBAO+ derives
    // 1/(1 - bias) from it.
    { &g_aoBiasE[0], "AoBias1000",     0, 950 },
    { &g_aoBiasE[1], "AoHbaoBias1000", 0, 950 },
    { &g_aoProj100E[0], "AoProj100",     10, 1000 },
    { &g_aoProj100E[1], "AoHbaoProj100", 10, 1000 },
    // Screen-radius ceiling, % of screen width. Below ~4 the AO becomes
    // contact-only; above ~15 near geometry samples unrelated scenery and
    // the estimator's variance shows up as low-resolution banding.
    { &g_aoRadiusMaxPctE[0], "AoRadiusMaxPct",     1, 50 },
    { &g_aoRadiusMaxPctE[1], "AoHbaoRadiusMaxPct", 1, 50 },
    // Bilateral blur pass (25_ssao.c): Sharp is the depth edge-stop - how
    // hard the blur refuses to smooth across depth discontinuities. 0 turns
    // the edge-stop off (plain gaussian, expect haloes); high values keep
    // edges crisp at the cost of residual grain along them.
    // Measured invariant: with AO off the composite's RGB minimum is
    // EXACTLY 128 frame-wide - the engine never darkens past 50%, and
    // materials are written to assume it. 0 = allow AO past that floor
    // (deeper creases, but any material that decodes the range renders
    // those pixels pure black - the 2026-08-16 menu shield).
    { &g_aoRespectFloor, "AoRespectFloor", 0, 1 },
    // AO buffer resolution divisor, applied to DISPLAY resolution (SSAA is
    // divided out first unless AoSsaaIndep=0). 1=full 2=half 4=quarter
    // 8=eighth; the estimator and blur run there, the composite step
    // upsamples with a depth-aware filter.
    // 1/8 removed 2026-08-16 (user: "basically unusable anyway") - the
    // estimator has too few samples per screen area to hold together.
    { &g_aoResDiv,      "AoResDiv",      1, 4 },
    { &g_aoSsaaIndep,   "AoSsaaIndep",   0, 1 },
    { &g_aoUpsampleDepth, "AoUpsampleDepth", 0, 1 },
    // AoQuality RETIRED 2026-08-16 by user decision ("no more option, locked
    // to quality"). The estimator always compiles the High tier; the Low and
    // Medium tables stay in 25_ssao.c in case a performance tier is ever
    // wanted again. Old inis carrying AoQuality are ignored, as intended.
    { &g_aoBlur,        "AoBlur",        0, 1 },
    { &g_aoBlurSharpE[0], "AoBlurSharp",     0, 4000 },
    { &g_aoBlurSharpE[1], "AoHbaoBlurSharp", 0, 4000 },
    // A-trous: each extra pass reuses the same 9-tap kernel with DOUBLED
    // spacing, so reach grows 9/17/33/65px for a linear cost. Spread scales
    // the base spacing (100 = 1px between taps).
    { &g_aoBlurPassesE[0], "AoBlurPasses",     0, 8 },
    { &g_aoBlurPassesE[1], "AoHbaoBlurPasses", 0, 8 },
    { &g_aoBlurStep100E[0], "AoBlurStep100",     5, 1600 },
    { &g_aoBlurStep100E[1], "AoHbaoBlurStep100", 5, 1600 },
#endif
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
    //
    // Renamed again 2026-08-15, to ScreenShadowResPct. "ShadowBufResPct" told
    // a reader nothing: "buffer" is an implementation detail, and the two
    // shadow settings people actually meet are resolution and distance, so a
    // third one named after a buffer reads as internal plumbing. The new name
    // says which shadows it affects - the SCREEN-SPACE pass
    // (DRAW_MULTI_SAMPLE_SHADOW, half-res by default), the contact/detail
    // layer, not the cascade shadow maps ShadowMapRes drives. Still no GUI
    // control: the effect is subtle and only applies on area change, which is
    // a trap in a menu but fine for someone reading the ini.
    { &g_shadowBufResPct, "ScreenShadowResPct", 0, 200 },
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
    { &g_logVerbose, "LogVerbose", 0, 1 },
    { &g_langCfg, "Language", 0, 8 },
    { &g_advancedMenu, "AdvancedMenu", 0, 1 },
    // Written by the mod, not a setting. Drives the one-time migrations in
    // CfgMigrate; appended at the END of the table on purpose, so adding it
    // shifts no existing index (the GUI reaches a couple of entries by
    // position).
    { &g_configVersion, "ConfigVersion", 0, 1000 },
};
#define NUM_NUMERICS (sizeof(g_numerics) / sizeof(g_numerics[0]))

// Config load and save are driven by the SAME g_toggles table as the hotkeys
// and the GUI, so a new toggle really is one line. This is not cosmetic
// tidying: DiscardMip0 shipped once with its hotkey, its checkbox and its
// load line all wired but its SaveConfig line missing, so every toggle was
// discarded at exit and it read back 0 on the next boot. Hand-maintained
// parallel lists of the same flags in three places will keep producing that
// bug; one list cannot.
// ---- Defaults snapshot and reset ------------------------------------------
// Players are told not to hand-edit the ini, so there has to be a way back
// from a bad set of values that does not involve them opening it. Both reset
// actions restore from a snapshot taken before the ini is first read, so the
// values restored are exactly the compile-time defaults - there is no second
// table to keep in sync, and a default changed in 01_config_gates.c is
// automatically what "reset" means from then on.
static LONG g_defTog[NUM_TOGGLES];
static LONG g_defNum[NUM_NUMERICS];
static volatile LONG g_defCaptured = 0;

static void CfgCaptureDefaults(void)
{
    size_t i;
    // Before the FIRST read, and only then: LoadConfig runs twice (early and
    // deferred) and the second pass would otherwise snapshot the ini's values
    // as if they were defaults.
    if (InterlockedCompareExchange(&g_defCaptured, 1, 0) != 0) return;
    for (i = 0; i < NUM_TOGGLES; i++)  g_defTog[i] = *g_toggles[i].flag;
    for (i = 0; i < NUM_NUMERICS; i++) g_defNum[i] = *g_numerics[i].val;
}

// The AO tuning window's own contents, and nothing else. Deliberately NOT
// every key beginning with "Ao": resetting AoEnable from a button inside the
// AO panel would switch the effect off, which is not what "reset the tuning
// values" means to anyone pressing it. AoProj100 is excluded too - it is
// measured from the engine every frame, not chosen.
static const char *const g_aoTweakKeys[] = {
    "AoStrengthPct",  "AoHbaoStrengthPct",
    "AoIntensity100", "AoHbaoIntensity100",
    "AoRadius100",    "AoHbaoRadius100",
    "AoBias1000",     "AoHbaoBias1000",
    "AoRadiusMaxPct", "AoHbaoRadiusMaxPct",
    "AoBlurSharp",    "AoHbaoBlurSharp",
    "AoBlurPasses",   "AoHbaoBlurPasses",
    "AoBlurStep100",  "AoHbaoBlurStep100",
    "AoResDiv",
};

static int CfgIsAoTweakKey(const char *key)
{
    size_t i;
    for (i = 0; i < sizeof(g_aoTweakKeys) / sizeof(g_aoTweakKeys[0]); i++)
        if (strcmp(key, g_aoTweakKeys[i]) == 0) return 1;
    return 0;
}

static void SaveConfig(void);

// aoOnly = 0 resets everything persisted, = 1 only the AO tuning values.
static void CfgResetDefaults(int aoOnly)
{
    size_t i;
    if (!g_defCaptured) return;          // nothing to restore to yet
    if (!aoOnly)
        for (i = 0; i < NUM_TOGGLES; i++)
            InterlockedExchange(g_toggles[i].flag, g_defTog[i]);
    for (i = 0; i < NUM_NUMERICS; i++) {
        if (aoOnly && !CfgIsAoTweakKey(g_numerics[i].key)) continue;
        InterlockedExchange(g_numerics[i].val, g_defNum[i]);
    }
    // A reset produces a CURRENT config by definition - it just wrote today's
    // compile-time defaults. Without this the snapshot's 0 would be restored
    // (the snapshot is taken before the ini is read, when the version is still
    // "unknown") and every migration would run again on the next launch.
    InterlockedExchange(&g_configVersion, CONFIG_VERSION);
    SaveConfig();
    {
        char l[96];
        sprintf(l, "[config] reset to defaults (%s)", aoOnly ? "AO tuning only" : "everything");
        LogLine(l);
    }
}

// ---- Config version migrations --------------------------------------------
// SaveConfig writes EVERY key, so every existing ini has every value pinned to
// whatever the defaults were when it was written: changing a compile-time
// default reaches FRESH INSTALLS ONLY. This is the machinery for moving an
// existing config forward - bump CONFIG_VERSION, add a step, and files written
// by older builds are corrected once, at the next load.
//
// An ini with no ConfigVersion line is a 1.0 BETA file and parses as 0. The
// file is stamped and rewritten immediately after migrating, so a step runs
// exactly once: a step that re-ran every launch would keep overwriting a value
// the user had since set deliberately, which is worse than never migrating.
// (CONFIG_VERSION itself is defined with g_configVersion, above the table -
// CfgResetDefaults needs it and sits earlier in the file.)
static void CfgMigrate(LONG from)
{
    char l[192];
#if ENABLE_AO_SSAO
    // v2 (1.1): AoRespectFloor now ships as 0 - see its declaration in
    // 01_config_gates.c for why 1 caps the sliders and cancels AO in shade.
    // Conditional so the line only appears when something actually changed.
    if (from < 2 && g_aoRespectFloor != 0) {
        g_aoRespectFloor = 0;
        LogLine("[config] migration v2: AoRespectFloor 1 -> 0"
                " (at 1 the engine's 0.5 shadow floor clamps AO;"
                " the shipped AO defaults were tuned at 0)");
    }
#endif
    sprintf(l, "[config] config file version %ld -> %d", from, CONFIG_VERSION);
    LogLine(l);
    g_configVersion = CONFIG_VERSION;
    SaveConfig();                        // stamp now - migrations run once
}

static void LoadConfig(void)
{
    char path[MAX_PATH];
    CfgCaptureDefaults();                // must precede the early return below
    GetConfigPath(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        // No ini at all: a fresh install already HAS the current defaults, so
        // it is current by definition. Stamping here rather than leaving the
        // compile-time 0 stops the first SaveConfig from writing a file that
        // claims to predate its own defaults - which would re-run every
        // migration on the next launch, forever.
        g_configVersion = CONFIG_VERSION;
        return;
    }
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
    // Before the [config] loaded line below, so the log shows POST-migration
    // values - the alternative reports numbers that were already stale by the
    // time they were printed.
    if (g_configVersion < CONFIG_VERSION) CfgMigrate(g_configVersion);
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

