# Post-1.0-BETA plan — Savior's Patch

Written 2026-08-18, after the first beta feedback round. This is the work
queue plus enough context to pick any item up cold in a fresh session.
Reception is good; few issues reported. Items ordered by priority as agreed.

**Next task after this file was written: MSAA issues (reports incoming,
details unknown yet). See the MSAA section at the bottom for orientation.**

---

## Release state (facts, verified)

- `MOD_VERSION "1.0"` in `hook.h` — bump per release. Boot banner is the
  first log line: `[boot] Savior's Patch 1.0 (Performance & graphics) -
  built <date> <time>`. The public release name is carried in the version
  string itself so a reporter's log names the exact build.
- **THE NUMBER GOES BACKWARDS AT THIS POINT AND THAT IS INTENTIONAL.**
  1.0/1.1/1.2 BETA were the pre-release line; this is the first PUBLIC
  release and it is 1.0. So a beta log says 1.2 and a release log says 1.0.
  When triaging a report, **read the build timestamp in the banner, not just
  the number** — a 1.0 dated after a 1.2 is the newer build. Every future
  release numbers upward from 1.0 normally.
- **1.0 (public release) adds, on top of everything in 1.2 BETA below:**
  - **Anisotropic filtering, `AnisoLevel=16`.** The vanilla menu offered 1x
    and 8x and nothing else (disassembled — see `08d_texfilter.c`); it is
    replaced with Off / 2x / 4x / 8x / 16x, enforced at `SetSamplerState`
    rather than through the engine's settings field. `ConfigVersion` 3
    migrates an explicit 0 forward.
  - **Mip LOD bias clamp, `MipBiasMode=1`.** The engine biases mip selection
    negative constantly (1,319,073 negative writes against 30,036 positive,
    reaching −5.00), which undersamples and makes distant terrain crawl. A
    floor on the negative side only fixes it; positive biases are deliberate
    blur effects and pass through untouched. Graphics → Mip LOD Bias →
    Off / On, real-time. Floors of −0.5/−1.0 remain as `MipBiasMode=2`/`3`.
  - **The `[texfilter]` census ships**, on the release keep list. Three lines
    per reporting slot on a geometric schedule (10s, 30s, 2min, 10min, then
    every 10min), plus one on any setting change. It makes a filtering or
    mipmapping report actionable without asking the reporter for a verbose
    run — which is exactly what it was built to do for us.
  - Status rows report **liveness, not totals** (`StatLive`, 10_overlay.c).
- **Shipping defaults worth knowing** (all in `01_config_gates.c`):
  `AnisoLevel=16`, `MipBiasMode=1`, `ForceTrilinear=0`, `AoRespectFloor=0`,
  `InGameUi=1`, `ForceDynamicFramerate=1`, `ConfigVersion=3`.
- **Known cosmetic gap:** the Graphics → **Mip LOD Bias** group NAME is
  English in every language. Its two entries are localised (`S_OFF`/`S_ON`
  were already in the table); the group name is a term of art and was left.
  If it should be translated, add it to `tools/gen_i18n.py` and regenerate.
- **1.2 BETA contents** (1.1 BETA was an internal build; its contents ship
  here too):
  - queue item 0 — `AoRespectFloor` now defaults to 0, plus the
    `ConfigVersion` migration machinery that carries existing inis forward.
  - queue item 1 — the engine's own framerate mode is forced back to Dynamic
    (Fixed halves the mod's target). Re-checked once a second for the first
    minute, because a launcher can revert it after boot.
  - the MSAA screen-grab fix (below).
  - queue item 2 — NVIDIA's HBAO+ blur was built, tested and RETIRED; ours
    wins. Gated at `ENABLE_NV_BLUR`.
  - **the in-game UI** (`26_ingame_ui.c`): all three mod surfaces — AO tuning
    panel, frametime graph, status panel — render inside the frame at
    EndScene instead of as Win32 windows. `InGameUi=1`.
  - i18n: the language "failure" was a missing success line; detection always
    worked. Both sides are logged now.
- **The in-game UI, and why it exists.** The Win32 tuning window lost to
  fullscreen three fixes running: topmost is a band not a guarantee;
  ownership pins Z-order but couples fates (Windows hides owned windows when
  the owner minimises, and a fullscreen device minimises on any focus loss);
  and every activation near the game window costs it a focus round-trip the
  user reads as a stutter. Rendering inside the frame removes all three as
  categories. Hard-won details worth keeping: **neither device-level Present
  hook has ever fired in this game** (the swap-chain one is a confirmed
  first-frame crash, retired) so `EndScene` is the injection point — which
  also means the MSAA resolve backstop, which lived in Present, had never
  run either and now does; EndScene fires **~4x per frame** here, so
  per-frame work must latch on `g_msFrameSeq`; and the display surfaces
  repaint on a 250ms timer, which is the cadence their Win32 versions always
  had — repainting them per frame cost 140fps → 105.
- **MSAA screen-grab fix** (commits 0bf9834 diag, 7131e3d fix). Symptoms:
  with MSAA on, fullscreen "grab" effects vanished — the white flash layers
  on spells, and the battle-transition freeze-frame (so the transition cut
  straight to the still-loading arena). Root cause: the substitution only
  intercepted `SetRenderTarget`, and the engine also reads the scene surface
  mid-episode (stale grab) and WRITES into it by StretchRect (our resolve
  then overwrote the engine's paste). Both are handled in the `[grab]`
  wrapper around `HookedStretchRect` (16): a sync-resolve before a foreign
  read, and for a foreign write an episode handback plus `g_msSuppressFrame`
  (cleared where `g_msNeedDepthClear` is armed, 07). Hole B — mid-episode
  SAMPLING of the scene texture — was instrumented in 24 and never fired;
  the detectors stay. `[grab]` is on the release-filter keep list, so beta
  logs carry this for free. Rates are in the `[msaa]` report as
  `grab sync= foreignW= suppressed=`.
- **Open question on that fix:** the handback may fire every frame (the
  engine's scene→SCENE#3→scene post ping-pong looks per-frame), which is
  harmless only because it lands after the world pass. If `suppressed` ever
  approaches `subs` in the `[msaa]` line, MSAA is being defeated — that line
  is NOT on the keep list, so it needs a `LogVerbose=1` run to read.
- **Release log filter** is live: `LogLineWanted()` at
  `hook_parts/06_io_alloc.c:1302`, called from `LogLine` at :1346. Keeps
  `[boot] [config] [menu] [i18n] [fov] [swap] [crash] [stutter] [log]` plus
  any line containing FAILED / ERROR / cannot / not loaded / not found /
  unable (lines containing `NOTE:` excluded). `LogVerbose=1` in the ini
  restores the full firehose. Measured: 1801 lines -> 12 on a real session.
- **The crash handler ships enabled.** `ModCrashVeh` at
  `hook_parts/19_boot_install.c:431`, registered unconditionally at :528.
  (`ENABLE_CRASH_LOG 0` gates a *different*, retired logger — do not be
  misled.) `[crash]` lines carry module+offset, survive the filter, and
  resolve against `dinput8_new.map`. Every crash report with a log is
  actionable.
- **Reset machinery**: `CfgCaptureDefaults` (08_config_persist.c:438)
  snapshots compile-time defaults one-shot before the first ini read;
  `CfgResetDefaults(aoOnly)` at :477 restores + saves. UI: Other → Reset all
  settings; AO panel → Reset AO settings (excludes AoEnable and AoProj100 by
  design).
- **i18n**: 34 strings × 8 languages, generated by `tools/gen_i18n.py` →
  `hook_parts/08b_i18n.c` (pure-ASCII \uXXXX — MSVC charset guessing).
  Detection reads the game's own menu bar back on every rebuild
  (`LangDetectFromMenu`, 08c_lang_detect.c:129) and matches the localised
  "Graphics" label (anchors extracted from the exe's own localisation block;
  offsets recorded in gen_i18n.py). Labels are space-padded in the exe —
  trim + case-insensitive prefix match, NOT wcscmp (that bug shipped once
  and hid behind the OS-language fallback). Order: ini `Language=` (1..8) →
  menu label → OS UI language → English.

---

## Queue

### 0. AoRespectFloor should ship as 0 — DONE in 1.1 BETA

**Shipped.** Code default is now 0 (`01_config_gates.c`), and the
ConfigVersion migration machinery exists: `CONFIG_VERSION` + `g_configVersion`
declared above the numerics table in `08_config_persist.c`, `CfgMigrate(from)`
just above `LoadConfig`, called after parsing and before the `[config] loaded`
line. An ini with no `ConfigVersion` key parses as 0 (a 1.0 BETA file); steps
run in order, then the file is stamped and rewritten immediately so a step
runs exactly once — a re-running step would keep overwriting a value the user
had since set on purpose. Fresh installs stamp current in the no-file branch;
`CfgResetDefaults` re-stamps too (its snapshot predates the ini read, so it
would otherwise restore 0 and re-migrate forever).

**Adding a future migration:** bump `CONFIG_VERSION`, add an `if (from < N)`
block to `CfgMigrate`. That is the whole procedure.

The original analysis follows, since it is the reasoning behind the default.



**Problem.** Beta users report AO "can't be made strong enough". Root cause:
the shipped default `AoRespectFloor=1` clamps the composite at the engine's
0.5 floor. Two clamps, same flag:

- estimator: `if (cParam2.y > 0.5) ao = saturate(ao)` (25_ssao.c:438)
- combine: `outc = eng * ao; if (cK0.x > 0.5) outc = max(outc, 0.5)`

Consequences with floor=1: in lit areas (eng ~ 1) AO can darken at most to
0.5 — past a point the sliders do nothing; in engine-shadowed areas
(eng ~ 0.5) ANY ao lands below 0.5 and is clamped straight back — AO
contributes almost nothing indoors / in shade.

**The kicker:** the shipped tuned defaults (Strength 149, Intensity 522/400
etc., commit e62b0be) were all tuned with the dev ini at `AoRespectFloor=0`.
Fresh installs get the numbers but not the behaviour they were tuned under —
they cannot reproduce the advertised look.

**Risk, honestly:** the floor guard was added during the black-shield hunt;
that bug was root-caused to CreateStateBlock (state-block rule), NOT the
floor. Weeks of play at floor=0 on the dev machine with zero material
artifacts. Residual risk = a material that decodes the [0.5..1] envelope
rendering sub-floor pixels hard black — distinctive if it ever appears; the
flag stays as the escape hatch.

**Migration wrinkle:** `SaveConfig` writes EVERY key, so all existing beta
inis have `AoRespectFloor=1` pinned — changing the code default reaches new
installs only. Plan: add a `ConfigVersion` ini key; on load, if the file's
version < current, apply migrations (v2: force AoRespectFloor=0), then
stamp. Reusable machinery for every future default change. "Reset all
settings" already restores the new default (snapshot is compile-time).

### 1. Force the engine's framerate mode to Dynamic at boot — DONE (commit 60d79c9)

**Shipped.** `GameMenuForceDynamicFps` in `09_game_menu.c`, called at function
level from `GameMenuAppend` (outside the anchor-surgery block, so a stale popup
RVA cannot skip it). Queries `MENU_RVA_FRATE_STAB` with `handler(0)`; if Fixed
is active, calls `MENU_RVA_FRATE_VAR` with `handler(1)` (applies + persists
through the game's own path), then RE-QUERIES to verify and logs the outcome.
One-shot, conditional, `[menu]`-tagged so it appears in release logs.
`ForceDynamicFramerate` ini key, default 1, ini-only.

**Open:** whether `GameMenuAppend` runs at all in exclusive fullscreen (the
menu bar is detached with `SetMenu(NULL)`, but the builder may still run). If a
fullscreen log shows no `[menu] engine framerate mode ...` line, the fallback
is a one-shot on the main-thread frame tick (`OnEnter_ac3040_C`, 07) — NOT the
monitor thread.

The original analysis follows.



**Problem.** Vanilla "Fixed" (Stability) mode halves the mod's framerate
target. Our menu replaced the vanilla FrameRate popup, but the vanilla
SETTING persists in the game's own storage — and the game resets settings
to low after a crash (item 4), so users land in Fixed without knowing.

**Mechanism already in the codebase.** Vanilla handler convention:
`handler(0)` = query current state, `handler(1)` = apply + persist through
the game's own path. `MENU_RVA_FRATE_VAR 0x006CADB0` /
`MENU_RVA_FRATE_STAB 0x006CADE0` (09_game_menu.c:59-60); wrapped exemplar
`MenuH_FrVariable` at 09_game_menu.c:399 (kept behind a gate).

**Plan.** At first menu build (manager provably exists there — same context
as `LangDetectFromMenu` at :549): query Stability; if active, call the
Variable handler once, log `[boot] engine framerate mode was Fixed - forced
to Dynamic`. Idempotent. **Open question:** does the build hook fire early
enough in fullscreen (menu bar detached via SetMenu(NULL), but the builder
may still run)? If not, fall back to a one-shot check on the monitor thread.

### 2. NVIDIA's HBAO+ blur — BUILT, TESTED, RETIRED (commits 1053248, then gated)

**Answered.** Built faithfully (radius 3, one texel spacing, their gaussian
weight, separable X then Y), shipped as a per-estimator panel toggle, tested on
both estimators. User's verdict 2026-08-18: **keep ours.**

- **SSAO — clearly worse.** SAO's noise is white grain spread over the whole
  tap disk, and a fixed 3-tap radius at one texel cannot cover it. Ours reaches
  9/17/33/65px through a-trous levels, which is what makes that grain resolve.
- **HBAO+ — no visible difference.** Its noise is a structured 4x4 interleaved
  tile rather than grain, so it is already resolved by the single narrow pass
  (spread 5) ours runs there. Both kernels are doing the same small job.

Worse where the blur matters, equivalent where it does not — nothing left for
it to win. Gated behind `ENABLE_NV_BLUR 0` (01_config_gates.c) rather than
deleted, with the full reasoning at the gate.

**The reusable finding, and the reason the code is kept:** NVIDIA's blur
weights the ABSOLUTE depth difference, tuned for a depth range their default
sharpness of 40 was chosen against. This game's depth spans ~4..2000 world
units, so an absolute threshold that stops edges at arm's length ignores them
across a courtyard. Any future port of a depth-aware filter into this engine
has to normalise by centre depth first — ours already does.

`tools/check_shaders.py` PARSES the gate rather than hardcoding it, so flipping
`ENABLE_NV_BLUR` back on cannot leave the checker silently skipping a shader
that is again being compiled at runtime.

The original plan follows.



User wants it as a temporary toggle until they decide which blur ships.

**Reference** (fetched, verified): `gl_ssao/hbao_blur.frag.glsl` — separable
2-pass, fixed KERNEL_RADIUS, weight `exp2(-r^2*falloff - (dd*sharpness)^2)`
with `falloff = 1/(2*sigma^2)`, `sigma = KERNEL_RADIUS/2`; depth rides in
the source's second channel (we sample our depth texture instead — already
bound in the current blur).

**One required adaptation:** their sharpness weights ABSOLUTE depth
difference; this game's depth runs 4..2000 world units, so normalise the
depth delta by centre depth (as our current blur does) or one slider value
behaves differently near and far. Their default sharpness 40 is NOT
comparable to our Blur Sharp values (dev-tuned 681/915 on the
reciprocal-quadratic kernel).

**Plan.** New PS string beside `g_aoBlurHlsl` (~25 lines), compiled once;
`AoBlurMode` per-estimator slot (vals[2] pattern; 0 = a-trous ours,
1 = HBAO+); panel control; Passes/Spread apply only to mode 0 (theirs is
fixed-radius), Sharp maps to g_Sharpness. `tools/check_shaders.py` gains one
variant — its QUALITY table mirrors the C tables, keep in sync. Run it
before every deploy (runtime-compiled HLSL fails silently; it caught a dead
SSAO once).

### 3. SSAA crashes on enable / resolution / display-mode change

**Symptom (beta reports):** stable during play once configured; crashes when
enabling SSAA or changing resolution/display mode with it on. Dominoes into
item 4 (crash → vanilla settings reset).

**Suspicion:** device Reset / screen-set rebuild path — DEFAULT-pool
resources and the descriptor rewrite live there (04_ssaa.c; AO RTs released
via AoReconReset before Reset forwards). No root cause without data.

**Plan.** (a) Ask reporters for `SaviorsPatch.log` — `[crash]` module+offset
plus the build banner resolve against the .map. (b) Hardening worth doing
regardless: log the Reset HRESULT; on a failed rebuild DROP SSAA gracefully
(log + disable) instead of proceeding — converts a crash into a logged
fallback.

### 4. Vanilla graphics settings wiped after a crash — snapshot/restore

**Problem.** The game resets its own settings to low on unclean exit.
Windows users relaunch via Nova Launcher which rewrites them; Linux users
(Nova is not Linux-compatible) must redo every vanilla option by hand.

**Plan.** Generalise the handler mechanism: at boot, query each vanilla
handler (`handler(0)`), persist the answers in OUR ini (survives crashes);
on next boot re-apply (`handler(1)`) any that came back reset. Restores the
user's actual choices — deliberately NOT a blanket "force Advanced" (that
would override a deliberate Standard choice).

**FOUND 2026-08-18 — the settings are a plain ini, and this changes the plan.**
The engine's own graphics settings live in Steam userdata, NOT the registry
and not the game folder:

```
<Steam>\userdata\<steamid3>\345350\remote\SquareEnix\
    LightningReturnsFinalFantasyXIII\
        Configuration.ini   <- the settings
        Environment.ini     <- VoiceLanguage, DownloadContent
        Validation.ini      <- a single token: "Validated"
```

`Configuration.ini` is `[Configuration]` with aligned `Key = Value` lines:
`Graphics_Presentation` (FullScreen), `Graphics_Resolution` (3840x2160),
`Graphics_Scaling`, `Graphics_ColorCorrection`, `Graphics_Glare`,
`Graphics_DepthOfField`, `Graphics_Shadowing`, `Graphics_Lighting`,
`Graphics_TextureFiltering` (all Advanced), `Graphics_FrameRate`
(Variable/Stability), `Control_ConfirmButtonLayout`. The key names match the
`Graphics_*` localisation keys the handler RVAs were found by, so the missing
handlers (Lighting, DoF, Glare, ColorCorrection, TextureFiltering) can now be
identified by NAME instead of hunted.

**`Validation.ini` is the likely crash-reset mechanism** — a clean-exit marker
the game re-runs its auto-detect after when it is missing or not "Validated".
Worth confirming before designing anything: if so, item 4 may be less about
snapshot/restore and more about the marker.

**Two possible designs now.** (a) Read `Configuration.ini` ourselves at boot,
compare against a snapshot in our own ini, and re-apply through the vanilla
HANDLERS (still the safe path - the engine updates its own live state, not
just the file). (b) Write the file directly - simpler, but behind the engine's
back and possibly at odds with `Validation.ini` and Steam Cloud sync. (a) is
strongly preferred; the file is then a source of truth for READING, and the
handlers stay the only writers.

**Note it is Steam Cloud synced**, so a conflict is possible if it is written
while Steam is running.

**Gap:** we hold RVAs for Shadowing (ADV 0x006CAC90 / STD 0x006CACC0),
Scaling ADV 0x006CAEA0, Presentation FS 0x006CA830, and FrameRate (above).
Lighting, DoF, Glare, ColorCorrection, TextureFiltering handlers need
finding — mechanical: string-keyed registrations (`Graphics_Lighting_...`
keys visible in the exe's localisation block), follow the xrefs. FrameRate
itself is covered by item 1.

### 5. AO apply mode — second compositing target (prototype)

**Current limit.** AO writes into the engine's screen-shadow composite
([0.5..1] envelope, sampler 14, ~119 material draws). Consequence: AO
scales the game's shadowing, not its ambient light — reads much stronger in
shadow than in sun. The *right* target (the ambient term inside material
shaders) is unreachable: hundreds of runtime-compiled shaders.

**Second option we can actually build:** multiply AO onto the scene image
late in the frame. Known hook point: the FXAA pass —
`FXAA_SHADER_HASH_REAL 0xA082B248` (14_aa_shaders.c:20), already substituted
at bind time for FxaaOff; the draw before it has a known RT. Reuse the AO
combine machinery as a fullscreen multiply there.

**Plan.** `AO apply mode`: Shadow buffer (current) / Image / Both. Caveats
to design around: image mode = MXAO look (specular/emissive dimmed;
transparents/particles get AO from geometry behind them); Both must avoid
double-darkening where the two overlap. Bypasses the 0.5 floor and the
sunlight gating entirely. Largest item; needs visual iteration with the
user.

---

## Facts worth not re-deriving (cross-cutting)

- **SaveConfig writes every key** → changing a code default NEVER reaches an
  existing ini. Any future default change needs the ConfigVersion migration
  (item 0) or it affects fresh installs only.
- **State-block rule**: CreateStateBlock(D3DSBT_ALL)+Apply is destructive on
  this device (BehaviorFlags 0x44, MULTITHREADED; the asset-loader thread
  races the capture/restore window). AO uses explicit state restore from the
  shadows in 15_msaa.c (`g_esRs[256]`, `g_esVp`, `g_esVs`, `g_esDecl`,
  `g_esFvf`, `g_esStreamVb`, `g_esTex[16]`).
- **Self-interference rule**: injected device calls go through `g_orig*`
  pointers, never the patched vtable (hooks are state machines).
- **Proxy slots** (read from the exe's import table): taken — d3d9
  (ReShade), dinput8 (us), version (HD GUI mod). Free — winmm, xinput1_3,
  d3dx9_43, imagehlp. Nine imports are KnownDLLs and cannot proxy. Advice
  given to the ultrawide-mod user: rename theirs to winmm.dll (an ASI loader
  itself consumes a slot).
- **Build-lock**: 13 live engine addresses total, 12 in 09_game_menu.c
  (menu manager singleton + builder + six vanilla handlers); everything else
  is D3D9-level and portable. Porting to another exe build = re-find those
  13 (string-keyed, mechanical) + re-verify the engine pokes (sim delta,
  limiter, cutscene flag). No version gate exists yet — worth adding a
  SizeOfImage + known-byte check before hooks install, degrading to
  "D3D9 features only" on mismatch.
- **HBAO+ divergence from NVIDIA's** (2026-08-18 analysis, sources
  fetched): the estimator is IDENTICAL to gl_ssao's non-deinterleaved path;
  the differences are integration only — (1) jitter values generated
  procedurally (same 16 values, no cache architecture; our taps are FINER
  than their deinterleaved path, which snaps to a 4px grid), (3) we default
  AoResDiv=2 vs their full-res rule (trade taken knowingly; the 4x4
  interleaved tile resolves cleanly at half res), (4) no second depth layer
  (needs a geometry pass we don't control — this is the halos-behind-
  foreground gap), plus the compositing target above, plus the blur (item
  2).
- **AoProj100** is measured every frame from viewProjMatrix column norms
  (FOV probe; `ENABLE_FOV_PROBE=1` is load-bearing, not diagnostic —
  current value 317 = fovY 35 deg). Never expose it as a slider again.

---

## MSAA — orientation for the next task (reports pending, no details yet)

What exists today:

- `15_msaa.c` owns MSAA. `MsaaSamples` ini 0..8; menu offers Off/2x/4x/8x.
  The file's header records the forward-vs-deferred investigation (LR is
  FORWARD — the MRT probe settled it; that is what made MSAA plausible).
- Known mechanism constraint (from the log, still true): driver support for
  a multisampled surface does not make it sampleable in D3D9 — the engine
  renders the scene to a texture and samples it in post, so the mod injects
  a StretchRect resolve.
- `MsaaDebugClear` diagnostic (paint the MS surface magenta) exists behind
  `ENABLE_SURFACE_DIAG` (currently 0) — flip that gate for surface-level
  debugging.
- Retired neighbours: ENABLE_CUTOUT_AA (A2C, SSAA-foliage, fringe — all
  dead ends; reasoning in FEATURES.md, AA section).
- The engine-state shadows AO depends on live in this same file — MSAA
  changes can affect AO's state restore. Any new device-call work: the
  state-block rule and self-interference rule apply.
- First step when reports arrive: get `SaviorsPatch.log` (crash lines are
  actionable as-is); for visual issues ask for a `LogVerbose=1` run.
- Plausible interaction surfaces to check early: MSAA x SSAA (both touch
  the present/resolve path; SSAA is already crash-prone on mode change —
  item 3), MSAA x AO (AO reads the depth texture; a multisampled depth
  cannot be sampled — what does the AO depth path see with MSAA on?), and
  MSAA x ReShade (a d3d9.dll proxy sits in the chain on many installs,
  including the dev machine).

---

## Standing rules (so a fresh session doesn't relearn them)

- User runs the tests; we build. Granular commits with `-F messagefile`
  (inline multi-line messages break in this shell). Config edits only with
  the game closed. Deploy = copy `mods/version_hook/dinput8_new.dll` over
  the game's `dinput8.dll`; blocked while the game runs.
- Gate retired code, never delete. No disk-loaded shaders (runtime-compile
  is fine). `python tools/check_shaders.py` before every deploy that
  touches shader strings.
- Confirm every RE diagnostic empirically before concluding. Slice logs at
  the last `[mark]`; use `tools/analyze_run.py`. ShaderThrottle=1 is
  deliberate and exonerated.
- Do not make unrequested changes; when the user has decided an approach,
  new information goes back to them — it does not override the decision
  (learned twice: the SAO estimator, the Steam-manifest language
  detection).

---

## 1.0 release work (started 2026-08-19)

1.2 BETA is out, tested by several people, no issues reported. The road to
the public 1.0 + Nexus post adds two features.

### A. Anisotropic filtering — BUILT, awaiting the first test run

**The vanilla ceiling is confirmed 8x.** Graphics > Texture Filtering has two
entries and both are three-line handlers writing one DWORD:

| entry | RVA | writes |
|---|---|---|
| `Graphics_TextureFiltering_Advanced` | `0x006CAD50` | `settings+0x38 = 8` |
| `Graphics_TextureFiltering_Standard` | `0x006CAD80` | `settings+0x38 = 1` |

(settings base = `DAT_0511558c`, i.e. RVA `0x04D1558C`. Disassembled against
the shipped exe, not decompiler output.) So "Standard" is 1x — trilinear only
— and 8x is the whole range.

**Shipped design.** The engine field is NOT written. All 138 xrefs to that
global were scanned and only these two handlers touch +0x38, so its consumer
reads it through an unidentified copy — and it is the field serialised to
`Configuration.ini`, where an out-of-range enum is a plausible way to trip a
settings reset. Enforcement lives at `SetSamplerState` instead
(`12b_texfilter_hook.c`), with the state/counters/report in `08d_texfilter.c`
(split because d3d9.h does not enter the TU until part 12).

**The qualifying rule** is what keeps this safe, and it is derived from the
engine rather than hardcoded: *a stage qualifies once the engine has enabled
mipmapping on it.* Anisotropy refines mip selection, so a sampler with no mip
chain gains nothing — and the samplers that must not be touched (post passes,
LUTs, shadow comparisons, our own UI blit) are exactly the ones an engine
leaves at `MIPFILTER NONE`.

**Menu:** Off / 2x / 4x / 8x / 16x, replacing the vanilla pair. `AnisoLevel`
in the ini. Status panel has an `Anisotropic` row whose amber condition —
level set, zero MINFILTER upgrades — is the one real failure mode.

**Ships at `AnisoLevel=16`** (user-confirmed working in game, 2026-08-19).
Anisotropy costs bandwidth only on the samples it actually takes, and it takes
the extra ones only at grazing angles - so on any GPU that can run this game
it is close to free, against a stock game whose ceiling is 8x and whose
"Standard" is anisotropy OFF.

`ConfigVersion` 3 migrates `AnisoLevel=0 -> 16`. Only inis written by the
single census build carry an explicit 0, and 0 there meant "baseline for the
measurement run", not a preference; any other value is a real menu choice and
is left alone.

The `AnisoLevel=0` state (leave the engine alone) is still reachable from the
ini. Its menu entry is retired - with 16x as the default, an entry meaning
"defer to a menu whose entries we just deleted" is a trap. `MenuH_AnisoEngine`
stays compiled.

**Confirmed in gameplay** (2026-08-19): the engine asks for `ANISOTROPIC` +
`MAXANISOTROPY=8` on 1,327,208 writes, we raise those to 16 and additionally
upgrade the 448,074 `LINEAR` writes the engine leaves alone — which is what
makes the level mean anything for a player whose engine setting is Standard.
3.5M MINFILTER upgrades and 1.8M level rewrites across 16.4M sampler writes,
with no framerate cost reported. **Done for 1.0.**

`ForceTrilinear` resolved to NO — see section B.

### B. Mipmapping — ONE REAL FAULT FOUND AND FIXED (2026-08-19)

**The fault is the engine's negative mip LOD bias, and `MipBiasMode=1` ships
as the fix.** Four census runs. Of the four mechanisms that can produce a
mipmapping fault, three are healthy and one was not — and the one that was
not is the one this document, an hour earlier, recorded as "not a defect,
leave it alone". That reversal is the most useful thing on this page, so the
reasoning for it is preserved below rather than edited away.

**What the wrong call was, and why it was wrong.** The measurement said 97.8%
of LOD bias writes are negative, spanning −5.00 to +5.00, and I concluded
*per-material art direction, not a fault* — because no engine writes ±5.0 by
accident. That inference was sound and the conclusion still did not follow.
Deliberate does not mean harmless: the bias was authored against a 720p
console target, and it is resolution-independent, so it survives unchanged
into a 1440p/4K PC render where the pixel footprint is already far smaller.
Intent and correctness are separate questions, and I answered the first one
while believing I had answered the second.

**What actually settled it was the user's own eye, not the log**: distant
terrain in the Wildlands crawling like "a texture too detailed for the
current res", improving with both anisotropy and SSAA, explicitly NOT
sparkle. Pattern crawl that responds to sample count is undersampling, and
a negative bias is undersampling by construction — distance-weighted,
because near the camera the selection is already at level 0 and the bias
clamps out, while far away it sits mid-chain and applies in full. "Fine
nearby, shimmery in the distance" is the shape that produces.

**The general lesson, which is worth more than the fix:** a census can only
tell you what the engine DOES, never whether what it does looks wrong on
someone's monitor. Closing an investigation on "no confirmed symptom" was
correct process, but the correct next step was to ask what the symptom would
look like, not to file the question as settled. Related: the loading-screen
trap below, which is the same failure of window selection one level down.

**The fix**: a FLOOR on the negative side only, at `SetSamplerState`.
Positive biases pass through untouched — those really are deliberate blur
effects (30,036 of them, stages 0 and 1 only, reaching +5.0). Menu is
Graphics → Mip LOD Bias → Off / On, real-time in both directions because the
engine rewrites this state over a million times a session. Floors of −0.5 and
−1.0 remain reachable as `MipBiasMode=2`/`3` in the ini: a mild negative bias
is legitimate practice once anisotropy is paying for the extra samples, which
at 16x it now is.

The other three mechanisms were and remain healthy — evidence below, because
a negative result is what stops each being re-opened on the next vague
report.

**Read the loading-screen trap first.** The first census build only ever
captured its first 13 seconds, and two conclusions drawn from that window
were WRONG in the same direction:

| read from loading | actual gameplay |
|---|---|
| "never uses trilinear — `mip N/P/L=0/594/0`" | `10220/99087/1725469`, i.e. ~95% trilinear |
| "never asks for anisotropy — all 1750 writes are 1x" | `MAXANISOTROPY=8` on 1,327,208 writes |

A loading screen touches 5 sampler stages and no world material. Any future
census reading must come from a window where `stages used=0xFFFF` and
`mipped=0xFFFF`, which is the tell that world rendering is actually running.

**The four mechanisms, and the verdict on each:**

- **`MIPFILTER` POINT (mip banding).** 99,087 POINT against 1,725,469 LINEAR
  — 5.4%, in a game that is otherwise trilinear throughout. Not a defect;
  those are specific samplers choosing point deliberately. `ForceTrilinear`
  stays built, gated off, ini-only. **Not a 1.0 feature.**
- **`MIPMAPLODBIAS`. THE FAULT — see the section header.** Written 1,349,109
  times, range −5.00 to +5.00, **97.8% negative** (1,319,073 sharpening vs
  30,036 blurring), positives confined to `blurStages=0x0003`. Fixed by a
  negative-side floor, `MipBiasMode` shipping at 1.
- **`MAXMIPLEVEL`.** `nz=0` in every window. Clean.
- **Textures with no mip chain.** 302 of 1242 (24%), 260 compressed, 141 at
  1024+, and creator attribution kills the HD-GUI-mod hypothesis outright:
  **game=295, other=7**. These are the game's own. The eight worked examples
  were all UI-shaped (512x64, 1024x64, 660x76 — non-power-of-two), but they
  were sampled at the title screen, so they describe the menu and not the
  world; the sampler now skips the first 300 creations so a future log
  answers this without a special run.

**Why no fix for the missing chains even so.** Generating them means asking
`CreateTexture` for more levels than the game wants, detecting when its own
level-0 upload has finished, and filtering the rest — against a game whose
code assumes one level and whose upload path already goes through the
staging redirect. That is a substantial, risky feature. Against it: no
confirmed visual symptom exists. Nobody has produced a screenshot, and the
reports that started this were second-hand. **Not worth building blind.**
If a symptom is ever pinned to a specific surface, this is where to start
and the counters are already shipping.

### B-OLD. The original measuring plan (kept for the mechanism notes)

Reports of "mipmapping issues" exist but nobody has pinned a symptom, so
nothing is being fixed blind. The `[texfilter]` census measures every
mechanism that can produce one, in a single line:

- `mip N/P/L` — `MIPFILTER` histogram. **P (POINT) is the classic defect**: a
  visible arc on the ground where one mip ends and the next begins, sliding
  with the camera. `ForceTrilinear=1` rewrites it; ships OFF until the census
  says the game actually does it.
- `lodBias nz= last=` — `MIPMAPLODBIAS`. A positive bias is the engine
  deliberately blurring, which reads as "mushy textures" and which no
  filtering setting can fix.
- `maxMipLevel nz=` — the engine refusing its own sharpest mip levels, the
  other route to "blurry at distance" with a perfect texture.
- `textures>=64px: N single-level of M` — the CONTENT half, from
  `CreateTexture`. A large surface texture with exactly one level has no mip
  chain, so it shimmers at every distance and no sampler setting helps. By
  eye this is indistinguishable from a filtering fault, which is why it is
  counted separately.

Anything the census shows as healthy is off the list; whatever is left is the
thing to fix.

---

## Resolution options — ANALYSIS COMPLETE, nothing built (2026-08-20)

User request: the game's resolution menu is a fixed list that never asks the
display what it supports; add options. Analysis below is disassembly-verified
(scratchpad scripts res_ini/res_writer/res_ser.py against the shipped exe).

### The complete mechanism

**Selection.** Eleven hardcoded handlers, registered by the vanilla menu build
(`FUN_00acaf60`) exactly like every other Graphics setting:

    Graphics_Resolution_3840x2160  FUN_00aca8b0    ... down to
    Graphics_Resolution_1280x720   FUN_00acab30

Every handler is the same 59-byte shape as the TextureFiltering pair:
`if (apply) { settings+0x10 = W; settings+0x14 = H; } return (fields == W,H)`.
All eleven are EXACTLY 16:9 (2160x1215, 2400x1350, 1440x810 included). **No
display-mode enumeration exists anywhere in the chain** — the list is the
list, on every machine.

**Application.** Already mapped by the SSAA work and proven live daily:
`settings+0x10/+0x14` IS the resolution. The engine's screen-set rebuild
detector compares slot 0x21's recorded dimensions against these fields and
rebuilds the whole chain (scene target, prepass, post pyramid, viewports,
window size, swap chain) when they differ. Writing the fields at runtime
applies within a frame — this is exactly how the retired SSAA reswrite mode
worked and how ApplySsaaScale's rebuild poke still works. Presentation is
ALWAYS borderless at desktop size (every Reset logs windowed=1); the chosen
resolution is the INTERNAL render size, scaled to the desktop-size backbuffer
by the engine's own scaling draw.

**Persistence — the round-trip is a static table, and this is the crux.**
At `0x218B4B8` in .rdata: 32 records of `{key_ptr, value_ptr, handler_ptr}`,
12 bytes each, covering every Configuration.ini line (11 of them are the
resolutions; value strings are bare "2560x1440" etc.).

- Parser `FUN_00ac4dd0` (called from `FUN_004298c0` after the Validation.ini
  gate): for each record, read ini value for `key`, `strcmp` against the
  record's `value`, on match call `handler(1)`. Loop bound is a hardcoded
  `0x180` (32*12). **Unknown value -> no handler called -> default stands.**
- Serializer `FUN_00ac4e60`: for each record, call `handler(0)`; if checked,
  append `key = value` using the format string at `0x218b638`. **If no
  handler is checked (custom W/H in the fields), the line is simply OMITTED.**
  No corruption, no out-of-range value - the key just vanishes and the next
  boot auto-detects. This kills the settings-reset fear for this field.

**Bonus, confirms old queue item 4:** `FUN_004298c0` disassembled: it reads
Validation.ini, strcmps the content against "Validated"; on MISMATCH it
returns without ever reading Configuration.ini (-> auto-detect). A MISSING
Validation.ini still reads the config. So the crash-reset mechanism is
"Validation.ini present but not 'Validated'", written as "Validated" only on
clean save. Item 4's hypothesis was right in substance.

### Design options

**(a) RECOMMENDED - mod-owned entries, mod persistence.** Same pattern as
Shadowing/FrameRate: append/replace entries in the Resolution popup via
GameMenuInsertLeaf; handlers write `settings+0x10/+0x14` directly (the
rebuild detector applies it live); persist the choice in SaviorsPatch.ini;
re-apply at boot from the monitor thread (ApplyShadowResolution pattern) and
KEEP re-applying briefly (Nova lesson - the game's own parser runs at boot
and a vanilla-value ini would fight us). When the user picks a resolution
that IS one of the vanilla eleven, call the vanilla handler instead so it
serialises natively and the mod ini stays out of it. Custom values: the
game's ini omits the line, ours carries it. Zero engine-table modification.

**(b) REJECTED - extend the game's static table.** Records are contiguous
.rdata shared with all 32 settings; extending means relocating the table and
patching six hardcoded base addresses plus two 0x180 loop bounds across
parser and serializer. Instruction patching for cosmetic parity (the game's
own ini carrying the custom value) that design (a) gets for free in ours.

**(c) REJECTED - ini-only field write at boot.** Works (it is what the SSAA
reswrite era proved) but invisible: no menu, no discoverability.

### What the offered list should be

Enumerate rather than hardcode, or the mod repeats the game's mistake:
- `EnumDisplaySettings` for the display's real mode list (dedup, sort), plus
  the desktop resolution itself marked as native.
- Because presentation is always desktop-size borderless, ANY internal size
  works - "what the screen supports" is not actually a constraint for this
  engine. The list is about offering sensible choices, not legal ones.
  Above-desktop = supersampling (SSAA already productises that properly, with
  the chosen-res promise); the new list's job is native + below-native + the
  in-between steps the vanilla list skips (e.g. 1800p on 4K displays).

### The open risk: aspect ratio

Every vanilla resolution is exactly 16:9, the post pyramid has a fixed
1280x720 (16:9) base, and nothing is known about FOV or UI at other ratios.
16:10 (1920x1200, Steam Deck 1280x800) and 21:9 (3440x1440) are the obvious
candidates users will want. **Before designing any menu: one cheap
experiment** - write a non-16:9 pair (e.g. 2560x1080) into the fields at
runtime and look. Outcomes: correct wider FOV (great, ultrawide is real),
stretch (offer only same-ratio options), letterbox (same), or breakage
(16:9-only list, clamp enforced). Everything downstream branches on that
one observation.

---

## Next graphics round — recon (2026-09-14, analysis only, nothing built)

Three candidates were investigated statically. Scripts:
`tools/ghidra_scripts/GfxNextRecon.java`, `FieldGlobalTable.java`,
`FieldGlobalReaders.java`; output in `ghidra_output/gfx_next_recon.txt`,
`field_global_readers.txt`. Everything below is decompilation-derived and
needs the usual runtime confirmation before it is believed.

### A. DoF (and bloom) run at a fixed 720p base — the mechanism, fully mapped

- `FUN_00b00c00` (post pyramid allocator) sizes 25 surfaces from ONE value:
  `[0511558c]+0x1c` (=720), via `FUN_00b00b90(h, fmt)` (width derived).
  Slots 0-10 fmt 4 (A8R8G8B8) at full/half/quarter, slots 0xb-0x18 fmt 7
  (A16F) down to 1/64 — the luminance chain. Change detector `FUN_00b014b0`
  compares slot 0 against `+0x18/+0x1c` and frees the set (`FUN_00b00640`)
  on mismatch; the allocator then lazily rebuilds.
- **Do NOT write `+0x18/+0x1c`.** Zero code writes it after init (only the
  1280x720 constant), but it has ~50 readers: `FUN_007c0d60/70` getters have
  34/23 callers, all in the 0x0046..0x0088 range = the 2D/UI/camera layer
  (`FUN_0065e5a0` builds a projection from it, `FUN_007c0d80` returns h/w).
  It is the UI's DESIGN resolution. Scaling it = HUD layout breaks.
- **Viable route: ShadowBufPct's provenance trick, aimed at `FUN_00b00c00`.**
  Descriptor rewrite in the TextureImp ctor while the allocator executes
  (that is what made half-res scaling consistent end-to-end: the engine
  derives viewports from the wrapper dims). One extra piece the shadow case
  did not need: `FUN_00b014b0` will see slot 0 ≠ `+0x18/+0x1c` and free the
  set every frame. Either trampoline that 103-byte function with a copy
  whose third comparison uses the scaled pair, or scale slots 1..0x18 and
  leave slot 0 alone if slot 0 turns out not to be in the DoF chain.
- Still unknown, needs the runtime tool: which pyramid slots the DoF pass
  actually samples, the DoF shader hashes (Standard vs Advanced differ only
  by the `+0x3e` byte: handlers `FUN_00acac60`/`FUN_00acac30`; Glare is
  `+0x3f`, ColorCorrection `+0x3d`, Lighting `+0x3c`), and whether the blur
  shaders carry hardcoded 1/1280,1/720 texel constants. Re-enable
  `ENABLE_SHADER_DIAG` + the kill list, DoF-heavy scene, Standard then
  Advanced: the hashes that flip are the DoF shaders.
- Cost note for a `PostScale` knob: bokeh gather cost is linear in pixels;
  offer 1x / 1.5x / 2x / 3x (720p / 1080p / 1440p / 2160p base).

### B. Shadow filtering — the kernel is CPU-generated, 8 taps, tunable radius

`FUN_00ac6b00` (MS_SHADOW) calls `FUN_00a525c0` (14 KB, __thiscall, 20
stack args). Inside, the soft-shadow kernel is built on the CPU and uploaded
as PS constants:

```
for i in 0..7:                       // hardcoded 8 (loop bound AND the shader)
   r     = i * param_17               // param_17 = [DAT_05107a00+0x4a0], default 0.7
   angle = (i + c) * step             // spiral
   off   = (cos,sin) * r * (1/w, 1/h)  // w,h = dims of param_7 = the shadow
                                       //   atlas texture -> taps are in SHADOW
                                       //   MAP texels, i.e. real PCF
   wgt   = (pow(..) - pow(..)) * exp(..); normalised by the sum
```

Shadow object (`DAT_05107a00`) fields, set in ctor `FUN_00a30d80`:
`+0x4a0` radius step 0.7 (float), `+0x4a4` = 8 (tap count, but NOT passed to
the renderer — the loop is hardcoded), `+0x4a8` = 1 (byte), `+0x4ac` = 1,
`+0x4b0` = 20.0, `+0x4b4/+0x4b8` = 1000.0, `+0x4bc` = 0.5.

Consequences:
- **Why 8192 looked less soft / stair-stepped:** the kernel is 0..4.9 texels
  wide regardless of map size. At 4x the resolution the world-space
  footprint is 4x narrower. A `ShadowFilterRadius` write to `+0x4a0`
  (monitor-thread cadence, like ShadowMapRes) is a zero-shader-work knob;
  `0.7 * ShadowMapRes/2048` restores the stock softness at any resolution.
  With only 8 taps, radii much above ~2 will show banding — the honest
  ceiling of the CPU-side knob.
- **More taps / PCSS = shader replacement** of the PS bound inside
  `FUN_00a525c0` (hash from the runtime tool). The shader already has both
  inputs PCSS needs: the receiver's light-space depth (it computes it for
  the compare) and the atlas (the blocker depths ARE the map contents).
  Blocker search = extra taps in the same sampler; directional light so
  penumbra = (dReceiver - dBlocker) * k with k a "sun size" slider, not the
  perspective /dBlocker form. Extra constants (tap table, k) ride on our
  existing SetPixelShaderConstantF hook. Runs on the half-res buffer, so
  32 blocker + 32 PCF taps is cheap at 4K.
- `ShadowBufPct` (default 0) may finally earn its keep here: a sharper
  filter is exactly the case where the half-res buffer stops matching the
  content detail.

### C. NPC / enemy pop distance — a data-driven global, found

`sys/wdbpack.bin: r_field_global.wdb` (sheet `FieldGlobal`, 217 entries,
`fVal` + `uConvertType`) is mirrored into a static table of 0x20-byte
records, `name[16]` at `+0`, exe default at `+0x10`, **live float at
`+0x14`**, int(live) at `+0x18`. Base `0x024c8a30` (Ghidra base 0x400000),
111 records. Loader `FUN_005b2f60` runs on field load, looks each name up
in the WDB (`FUN_0073cfb0`) and writes `+0x14` (so a mod write must come
after it, i.e. the monitor cadence works). Getters `FUN_005b2f20`
(float, `[idx*0x20 + 0x024c8a44]`) / `FUN_005b2f40` (int) — the field code
never references the slots directly.

| idx | name | WDB value | live float (Ghidra VA) |
|---|---|---|---|
| 37 | `POPPopLength` | 80 | `0x024c8ee4` |
| 38 | `POPDepopLength` | 100 | `0x024c8f04` |
| 39 | `POPWNearLenMob` | 150 | `0x024c8f24` |
| 33 | `POPWNearLength` | 900 | `0x024c8e64` |
| 35 | `POPWLoadRes` | 100 | `0x024c8ea4` |
| 60 | `FEPopRange` | 28 | `0x024c91c4` |
| 84 | `FEPopRangeDeath` | 8 | `0x024c94c4` |
| 90 | `DZDepopLength` | 100 | `0x024c9584` |

`POP*` = the NPC population system, `FE*` = field enemies. Neighbours
`LDAdd/Sub{0,1,2}{Border,Speed,Timer}`, `LDModLevel1..3` (-5/-60/-300),
`POPSuggBordar` 1400, `POPMobBase` 1490 look like a load-budget controller
that throttles pops — the plausible reason NPCs appear "REALLY close": not
the distance constant but the budget denying the pop until late.
Camera-side character fade is a separate table (`f18CharacterChanging
AlphaDistanceMax/_PC` in the 0x020a17xx schema).

**First experiment (cheap, needs a run):** write `POPPopLength` 80 → 200
and `FEPopRange` 28 → 60 from the monitor thread in a busy zone (Luxerion
market) and watch. Three outcomes: pops move out (done, ship a knob), no
change (budget-gated: next is the LD controller), or stutter regression
(this IS the asset-streaming domain; more concurrent NPC loads is exactly
the load this mod was built to pace).

### B, step 1 — BUILT (2026-09-14): `ShadowFilterPct`, awaiting the first run

`ApplyShadowFilterRadius` (03_render_state.c) writes `scene+0x4a0` per
frame next to `ApplyCascadeSplitSource`, baseline/last-wrote pattern, sanity
window (0.01, 64). Ini key `ShadowFilterPct` 0..1600 (0 = untouched); menu
`Graphics ▸ Shadow Softness ▸ Standard / 150% / 200% / 300% / 400%`
directly under Shadow Distance (new string `S_SHADOW_SOFT`, 8 languages).
Status line `[shadow-filter] engine radius=… pct=… writes=…` — the
`radius` value is the confirmation: 0.700 means the field is the one the
decomp says it is. Deployed; previous DLL kept as
`mods/version_hook/dinput8_v_pre_shadowfilter_backup.dll`.

What the run answers: (1) is +0x4a0 the live radius at all (Standard vs
400% must differ visibly at the same ShadowMapRes); (2) how far 8 taps
stretch before banding — the number that sets whether the shader
replacement (more taps / PCSS) is worth doing.

**Run 1 result (2026-09-14):** the field is confirmed live — Standard vs
400% differ. Two findings, both predicted by the kernel maths and now
addressed: (1) the effect was inversely proportional to ShadowMapRes (texel
radius); `ShadowFilterPct` is now normalised by `res/2048`, so 100% = the
stock 2048 world footprint at any resolution, menu is Standard / 100 / 150 /
200 / 300. (2) "scatters rather than blurs, grainy at low res": 8 taps
spread past overlap = dither. Only a denser kernel (shader replacement)
fixes that. Also fixed: Reset All now selects the vanilla Advanced (2048)
handler explicitly, since clearing the force left the engine field wherever
the force had put it.

### B, step 2 — PCSS BUILT (2026-09-14), awaiting the first run

Shader identified by the user with the debug panel identify walk, then
disassembled (`tools/disasm_ps.py`, D3DDisassemble via d3dcompiler_47):
- **`ps_C7978054`** = the shadow projection: s0 linear depth, s1 atlas,
  s2 64x64 noise (per-pixel tap rotation — the "scatter"), c0.x split,
  c1 1/size, c2-c5 / c6-c9 cascade rows, c10+c15 weights, c11-c14+c16-c19
  offsets; visibility out in oC0.w. Cascade picked by `-v0.z*depth >= c0.x`.
- `ps_BE7317DB` = 4-tap depth-aware blur of the mask; `ps_7D468E35` = the
  half→full bilateral upsample; `ps_EB57E0DA` = point re-fetch;
  `ps_9FDD8F71` = depth compare/write; `ps_6113EE1F` = passthrough.
Replacement in `27_shadow_pcss.c`: same contract, 16-tap Poisson blocker
search + 32-tap PCF, penumbra in world units from |row_x|,|row_z|, rotated
by the engine's noise, clamped to the cascade's atlas band. Ini:
`ShadowPcss`, `PcssLightSize` (tan x1000, 30), `PcssMinRadiusX10` (10),
`PcssMaxRadius` (24), `PcssSearchRadius` (16), `PcssBias` (0); radii in
2048-map texels scaled to the live map. Menu: Shadow Softness ▸ PCSS.
Checker covers it (`PASS PCSS`). Status `[pcss] on= state= binds=`.

### The "whole shadow drops to a third of its resolution on a small pan" — SOLVED (2026-09-14)

Cascade watch (`ENABLE_CASCADE_WATCH`, `shaders\cwatch.csv`, constants c0-c9
captured while ps_C7978054 is bound, draw+unbind snapshots agree): both
cascades flip between a PERSPECTIVE light matrix (w row live; near cascade
~12 world units wide, far ~110) and plain ORTHO (w row 0,0,0,1; ~44 / ~230)
= the engine is LiSPSM with a hard fallback to uniform. Builder
`FUN_00a87150`: `if (0.99 <= |dot(view, light)|) uniform else LiSPSM` — the
literal at `DAT_0208f3c8` (shared with two unrelated compares). Fix in
`28_lispsm.c`: the one `MOVSD xmm3,[0x0208f3c8]` at 0x00a8737d is redirected
to a mod-owned double driven by ini `ShadowLispsmCos` (x10000, default 9990
= ~2.6 deg; 9900 = engine; 0 = engine). Also a row in the PCSS tuning
window. The LiSPSM branch already tends to uniform as sin -> 0 by its own
formula, so a near-1 threshold is safe in principle; watch for artefacts
when looking straight along the sun.

Note on c0: in this build c0 = (1/1280, 1/720, 0, 0) — so the shader's
`-v0.z*depth - c0.y >= 0` cascade test is effectively "always cascade A" and
the atlas's second half is the LiSPSM/uniform PAIR, not a distance split.
The PCSS shader copies the engine's test verbatim so it behaves identically;
the "split" naming in its comments is wrong and should be revisited.

**Correction (run 3):** the 0.99 redirect installed (relocation-aware
check fixed) and the flip was unchanged, so that compare is NOT the switch:
it sits in the LiSPSM builder (mode 4) and this build runs projection mode 1
(`[DAT_05107a00+0x390]` = 0 → `FUN_00a8cfb0` → `FUN_00a8c190`). The real
decision is `FUN_00a89170(camera, lightDir)`: project the light direction
and its opposite through the camera; uniform iff both are "inside"
(w ≤ ε, or |x|<w && |y|<w) — i.e. whenever the sun point or the anti-sun
point is inside the screen rectangle (whole FOV cone). Perspective builder
`FUN_00a898d0`, uniform `FUN_00a8b970`. No blend is possible between two
projections; built `ShadowProjMode` (28_lispsm.c, 6-byte prologue hook on
FUN_00a89170, cdecl): 0 engine, 1 always uniform ("Stable" in the new
Shadow Projection menu group), 2 always perspective (test only). At 8192
uniform ≈ perspective-at-2048 density, and never flips. `ShadowLispsmCos`
left in at 0 (engine) — harmless, retire later.

**Decision (2026-09-14, run 4):** LiSPSM (engine builder #4) tested — flashing,
shifting shadows; dormant broken code. Retired along with the 0.99 redirect
and the builder-field write; `ShadowLispsmCos` key gone. **`ShadowProjMode=1`
(uniform) ships as the default**: lower near-camera density than the
engine's perspective map, no whole-shadow resolution pop. Ini + PCSS
tuning-window checkbox only; no game-menu group (user's call). Shadow
Softness menu is now Off / On (= normalised 100%) / PCSS / Tuning Panel;
150-300% remain ini-only via `ShadowFilterPct`. `ENABLE_CASCADE_WATCH` 0.
Still to do before release: ENABLE_SHADER_DIAG / ENABLE_GUI_PANEL back to 0,
DumpShaders off, `15b_shadow_ps_probe.c` deletion or gate note, README.

### Cleanup pass (2026-09-14, evening) — shipping state
- PCSS defaults = user's tuned values: LightSize 5, MinRadiusX10 0,
  MaxRadius 10, SearchRadius 1 (floored to MaxRadius), Bias 0.
- `26b_shadow_panel.c`: in-frame Shadow Tuning panel (same machinery as the
  AO panel: DIB → texture → quad, polled input, painter's-order Z). Mode
  selector Off / On / PCSS mirrors the menu; On shows the Softness % slider
  (25..400, defaults to 100 when switched on), PCSS shows the five rows;
  uniform-projection checkbox; double-click reset = CfgResetDefaults(2)
  (shadow keys only). Opened from Shadow Softness ▸ Tuning Panel. The Win32
  window in 10b is now the InGameUi=0 fallback only.
- Gates: ENABLE_SHADER_DIAG 0, ENABLE_GUI_PANEL 0, ENABLE_CASCADE_WATCH 0
  (kept, useful), ENABLE_SHADOW_PS_PROBE removed with its file.
- New strings (8 languages): S_SHADOW_TUNING, S_SOFTNESS, S_LIGHT_SIZE,
  S_MIN_RADIUS, S_SEARCH_RADIUS, S_UNIFORM_PROJ, S_RESET_SHADOW.
- README updated. Log keep-tags: [cwatch] [pcss] [shadow-filter] [lispsm].

## NPC spawn distance — DONE (2026-09-15)

`29_npc_pop.c`. Mechanism (FUN_005a02f0, the field NPC manager update):
296 entity slots registered by the area; per frame, candidates within
`POPPopLength` are sorted and granted "active" while their CATEGORY's cap
lasts. Caps are hardcoded immediates in that function: 0x80 (cat 0, placed
NPCs), 0x14 (cat 1, mob NPCs — its acceptance threshold is POPSuggBordar
against a POPMobBase-derived score), 0x30 (cat 2, enemies presumably). No
array is sized by a cap; the 296 slots are the only hard limit. Mob NPCs
also have their own window radius, POPWNearLenMob (150).

Shipped: `NpcSpawnFix` (menu Graphics ▸ NPC Spawn Distance, default On)
writes POPPopLength 80→200, POPDepopLength 100→240, POPWNearLenMob →
pop+50 (250) into the live FieldGlobal table (name-checked, monitor
cadence, after the loader), and patches the cat-1 immediate 20→80. Hot:
on within a tick; off reverts the cap instantly and the distances on the
next area load. Ini: NpcPopLength / NpcDepopLength / NpcMobWindow /
NpcPoolA/B/C. Table layout proven at runtime by the [npc] dump (live values
= WDB values on the named rows). LD* rows turned out to be a movement-speed
controller, not load. Not addressed (by design or not a constant): pops on
area entry, scheduled appearances, model streaming latency.

Cleanup: shader dump moved to ghidra_output/shader_dump_2026-09-14/
(the identified ps_C7978054 / BE7317DB / 7D468E35 chain is in there);
game-folder shaders\ removed. Gates all 0 except ENABLE_SHADOW_PCSS.

## 1.1 release prep (2026-09-15)

`MOD_VERSION` 1.1; no CONFIG_VERSION bump (every new key is absent from a
1.0 ini and takes its default; the file catches up on the first SaveConfig -
there is NO boot-time rewrite, the `g_configRewritten` flag is declared and
never set). `release/` holds the 1.1 archive, README.txt and the Nexus
changelog (`NEXUS_1.1_changelog.txt`, BBCode).

Status panel rebuilt as two 470 px columns (10_overlay.c): left = graphics
(mod name + version as the first line, shadows incl. softness/PCSS params/
projection, AA, AO incl. live estimator numbers, NPC spawning), right =
build stamp, frame pacing, every streaming toggle with its live counter,
D3D9 provenance, and a hook registry (`HookRegNote` in 03, fed by
InstallJmpHook and the IAT/vtable installers in 19) that lists failed hooks
by name. Known reading: `D3D9 device vtable` FAILS under the HD GUI mod -
the throwaway-probe timing thunks are skipped on a wrapped device (12,
HookDeviceVtable); the real device hooks report on the "device hooked"
row. Left as a failure on purpose (user, 2026-09-15): it is one.

## STATE OF THE PROJECT (2026-09-15, after the shadow + NPC round)

Shipping build = `mods/version_hook/dinput8_new.dll` (deployed). Gates at
release state: only ENABLE_SHADOW_PCSS, ENABLE_AO_SSAO, ENABLE_AO_RECON,
ENABLE_FOV_PROBE are 1 (the last two are load-bearing for AO, not
diagnostics). `28_lispsm.c` renamed `28_shadow_proj.c` (it only holds the
projection-decision hook). Dev DLL copies from this round removed.

Features landed this round (all hot, all in the game menu unless noted):
- Shadow Softness: Off / On (remembered %, normalised so 100 = vanilla
  Advanced at any map size) / PCSS (defaults light 5, min 0.5 texel, max
  15, search = max, bias 0) / Tuning Panel (in-frame, mode selector,
  per-mode reset). Engine shader ps_C7978054 replaced on bind.
- Uniform shadow projection (ShadowProjMode=1, ini only): removes the
  whole-shadow resolution pop (engine LiSPSM-style perspective map falling
  back to uniform whenever the sun/anti-sun point is on screen).
- NPC Spawning Distance: Default / Extended (pop 80->200, depop 100->240,
  mob window 150->250, mob cap 20->80). Off by default.
- AO panel: Off / SSAO / HBAO+ selector, reset scoped to the live
  estimator; menu ticks refresh from the panels.
- Reset All selects vanilla Advanced (2048) shadows explicitly.

Tried and retired (reasoning in the shader / file comments): LiSPSM builder
#4 (flashing), the 0.99 threshold redirect (not the switch), PCSS nearest /
weighted / min-max blocker rules (halo or too soft), the draw-time Get*
shader probe (crashed), the Win32 PCSS window (fallback only).

Open / not done:
- DoF (and bloom) still at a fixed 720p base — mechanism fully mapped in
  "Next graphics round" A, nothing built. Needs the provenance-gated
  descriptor rewrite plus a FUN_00b014b0 trampoline.
- PCSS: character-vs-canopy intersection softening accepted.
- NPC: pops on area entry / scheduled appearances / streaming latency not
  addressed; a streaming-priority probe was offered and declined for now.
- Release housekeeping still owed before a public build: version bump,
  CONFIG_VERSION migration if any default changes for existing inis (none
  needed: new keys default correctly when absent), NEXUS text.

**DoF/bloom 720p — moved out of this mod (2026-09-15).** To be done in the
HD textures & shaders mod instead. Hand-off, all verified here: post
pyramid allocator `FUN_00b00c00` (prologue 55 8B EC 83 EC 18, 6 bytes
clean) sizes 25 surfaces from `[0511558c]+0x1c` via `FUN_00b00b90(h, fmt)`;
change detector `FUN_00b014b0` compares slot 0 of `[05115724]` against
`+0x18/+0x1c` and frees the set via `FUN_00b00640` (lazy realloc). Do NOT
write `+0x18/+0x1c` (UI design resolution, ~50 readers). Working recipe =
ShadowBufPct's: descriptor rewrite in the TextureImp ctor (`FUN_00aa3ce0`,
desc w @+0x0c h @+0x10) gated on the allocator being on the stack, plus a
detector gate that treats "slot 0 == scaled(+0x18/+0x1c)" as matching,
plus LINEAR on shrinking blits into the set (the scene->pyramid copy is
POINT). DoF shader constants: c0 = (1/1280, 1/720) in ps_C7978054's
neighbour chain — check whether the DoF/blur shaders take texel size from
the wrapper dims or from the settings pair before trusting the result.
Shader dump for the whole chain: ghidra_output/shader_dump_2026-09-14/.
