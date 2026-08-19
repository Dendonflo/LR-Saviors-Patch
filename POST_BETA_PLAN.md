# Post-1.0-BETA plan — Savior's Patch

Written 2026-08-18, after the first beta feedback round. This is the work
queue plus enough context to pick any item up cold in a fresh session.
Reception is good; few issues reported. Items ordered by priority as agreed.

**Next task after this file was written: MSAA issues (reports incoming,
details unknown yet). See the MSAA section at the bottom for orientation.**

---

## Release state (facts, verified)

- `MOD_VERSION "1.2"` in `hook.h` — bump per release. Boot banner is the
  first log line: `[boot] Savior's Patch 1.2 (Performance & graphics) -
  built <date> <time>`. The public release name is carried in the version
  string itself so a reporter's log names the exact build.
- **1.2 contents** (1.1 BETA was an internal build; its contents ship here):
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
