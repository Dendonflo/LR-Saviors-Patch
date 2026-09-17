# Savior's Patch — Performance & graphics

A D3D9 mod for the Steam release of *Lightning Returns: Final Fantasy XIII*,
built to remove the frame-time stutter the PC port suffers from while
streaming assets, and to add graphics options the original never exposed.

Ships as a `dinput8.dll` proxy. All options live in the game's own Win32 menu
bar — there is no separate window and nothing is loaded from disk at runtime.

Source: https://github.com/Dendonflo/LR-Saviors-Patch — releases on Nexus Mods.

## What it does

**Stutter fixes**

| fix | what it addresses |
|---|---|
| `DiscardFix` | the AMD driver's blocking `LOCK_DISCARD` path on dynamic buffers |
| `StagingUpload` / `StagingSurface` / `StagingCube` | routes texture writes through SYSTEMMEM staging so the upload never blocks the frame |
| `ReadPace` | bounds file-read bursts that were landing 20 MB inside a single frame |
| `ShaderThrottle` | caps the engine's per-frame shader-compile budget |
| `GpuSyncSkip` | removes a per-frame GPU fence that serialised CPU and GPU |
| `SimDeltaFix` | unquantises the simulation delta, which the engine truncated to whole 59.94 Hz periods |
| `ForceStdD3D9` | pushes the game off D3D9Ex so the runtime manages texture memory itself |

**Graphics additions**

- native SSAA (scene only, UI excluded) and MSAA
- shadow map resolution up to 8192, shadow cascade distance
- shadow softness: the engine's soft-shadow kernel, resolution-normalised, or
  a PCSS replacement of the shadow projection shader (contact-hardening,
  32-tap) with an in-game tuning panel
- a uniform shadow projection by default: the engine's perspective map
  dropped to a coarser fit whenever the sun was on screen, which read as the
  whole shadow changing resolution on a small pan
- SSAO / HBAO+ with an in-game tuning panel
- FXAA removal, frame-rate target (30 / 60 / unlimited)
- optional extended NPC spawning distance (placed NPCs pop in at 200 units
  instead of 80, the random-NPC window and cap raised to match; off by
  default)
- a frame-time overlay that graphs the *engine tick* rather than presents.
- a two-column status panel (version and build first) that shows every
  setting beside what is actually in force: shadows, AA, AO, NPC spawning,
  the streaming fixes, and which code hooks installed or failed.

## Building

The shipped `dinput8.dll` is built from this tree with nothing but the
Microsoft compiler; there is no third-party code, no package manager, and
no download step. Reproducing it takes about a minute.

**Prerequisites**

- Windows 10/11.
- Visual Studio 2026 Community (any edition works) with the
  *Desktop development with C++* workload, which provides the 32-bit
  (`x86`) MSVC toolset and the Windows SDK. Nothing else is needed: the
  DLL links only against Windows system libraries (`user32`, `gdi32`,
  `comctl32`) and resolves `d3d9.dll` / `d3dx9_43.dll` at runtime from the
  copies the game itself already loads.
- Python 3 (optional) for `tools/check_shaders.py`, the pre-release check
  that the runtime-compiled HLSL actually compiles.

**Steps**

1. Clone the repository.
2. Open `mods/version_hook/build.cmd` and check the `vcvars32.bat` path on
   its `call` line matches your installation (default:
   `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat`).
3. Run it from a normal command prompt:

   ```bat
   cd mods\version_hook
   build.cmd
   ```

   It does exactly one thing:

   ```bat
   cl /nologo /O2 /W3 /LD dllmain.c hook.c proxy.c /Fe:dinput8_new.dll ^
      /link /DEF:dinput8.def user32.lib gdi32.lib comctl32.lib /MAP:dinput8_new.map
   ```

   Expect a handful of warnings (`C4996 sprintf`, `LNK4222` ordinal notes);
   there are no errors. Output is `dinput8_new.dll` (~390 KB) and
   `dinput8_new.map`, which lists every function in the binary by name.
4. Copy `dinput8_new.dll` into the game folder as `dinput8.dll`.

The whole mod is one translation unit: `hook.c` `#include`s the files in
`hook_parts/` in order (see its header comment), so everything the DLL
contains is readable in this tree, top to bottom. The build embeds
`__DATE__`/`__TIME__` in the boot banner, so two builds differ by those
bytes and the linker timestamp; everything else is deterministic.

**Antivirus**

1.1 was held by Nexus on a generic machine-learning verdict (BitDefender
`Gen:Variant.Draftor`, 8 of 11 engines being that one engine). 1.1.2 cleared
on upload with no behavioural change. What moved the score, in order of
weight: no page is ever writable *and* executable (stubs are written, then
sealed read-execute; game code goes `EXECUTE_WRITECOPY` while patched);
`GetKeyState` instead of `GetAsyncKeyState`; a `VERSIONINFO` resource; the
large lookup tables allocated at start-up instead of sitting in `.data`
(1.5 MB virtual over 5 KB on disk looks packed); and no "hook"/"vtable"/
"IAT" in string literals. `tools/check_strings.py` gates the last one;
[capa](https://github.com/mandiant/capa) on the built DLL shows the rest.
Keep these when adding code.

**Why antivirus heuristics dislike it anyway**

Every one of these is what a game hook has to do, and each is readable in
the source at the file named:

- It is a `dinput8.dll` *proxy*: the game loads it by name and it forwards
  the real exports to `System32\dinput8.dll` (`proxy.c`). Proxy DLLs are a
  classic malware pattern *and* the standard way PC game mods load.
- It patches game code in memory at start-up — 5/6-byte `jmp` hooks on a
  few dozen engine functions and a handful of immediate rewrites — which
  needs `VirtualProtect` and `VirtualAlloc(PAGE_EXECUTE_READWRITE)` for the
  trampolines (`07_timing_watchdog.c` `InstallJmpHook`, `19_boot_install.c`).
  Every patch site is verified against the expected original bytes first
  and skipped otherwise.
- It installs a vectored exception handler to write a crash report into
  `SaviorsPatch.log` (`19_boot_install.c` `ModCrashVeh`).
- It patches the D3D9 device vtable to substitute shaders and render
  targets (`16_output_res_cascade.c`), and compiles its own HLSL at runtime
  with the game's `d3dx9_43.dll` (`25_ssao.c`, `27_shadow_pcss.c`).
- It draws its overlay through GDI into a texture and polls input
  (`26_ingame_ui.c`); it reads and writes one file next to itself,
  `SaviorsPatch.ini`, and appends to `SaviorsPatch.log`.

It makes no network connections, starts no processes, touches nothing
outside the game folder, and contains no packed or encrypted code — the
`.map` file from the build accounts for every byte.

## Compatibility

Works alongside the FF13 HD GUI texture mod, which occupies `version.dll` —
hence this one using `dinput8.dll`. When that mod exposes the `HDTex_*`
interop exports, staged texture uploads are pushed to it so its content
hashing still sees real pixel data; without them the push disables itself and
logs why.

Any DLL proxying a system library from the game folder is detected by load
path, and the startup device probe is skipped when one is present — a second
D3D9 device racing the game's own through a wrapper is what crashed startup.

## Repository layout

```
mods/version_hook/             the mod itself
mods/version_hook/hook.c       ordered #include manifest - the mod is ONE
                               translation unit, assembled from:
mods/version_hook/hook_parts/  35 subsystem files (config, SSAA, MSAA, AO,
                               shadows/PCSS, menu, panels, watchdog,
                               staging, NPC spawning, boot/install, ...)
tools/ghidra_scripts/          headless Ghidra scripts used for the analysis
tools/clb/                     tools for reading the engine's script resources
tools/*.ps1                    log monitoring helpers
```

The single-TU structure is deliberate, not an accident to fix: the code
relies on TU-wide tentative definitions and statics shared across
subsystems. `hook.c`'s header comment explains; do not compile the parts
individually or reorder the includes.

The code carries its own reasoning inline. Nearly every fix sits under a
comment explaining what it addresses, what was measured, and — where it
applies — which earlier theories were wrong and why. Several conclusions here
were only reached after two or three wrong ones, and those are recorded
alongside the fixes rather than tidied away.

## What is not here

No game content: no executables (original or patched), no extracted archives,
no decompiler output, and no Ghidra project. The Ghidra scripts are committed;
run them against your own copy of the game to regenerate the analysis.

The detailed reverse-engineering write-ups are also kept out of this
repository. They document Square Enix's engine internals in bulk, which is a
different thing from shipping a mod.

Requires a legally owned copy of the game. This mod is unaffiliated with
Square Enix.

## Licence

GPL-3.0-or-later. Copyright (C) 2026 Dendonflo. Full text in [LICENSE](LICENSE).

Same licence as the FF13 HD GUI & Fonts mod, deliberately — the two are
designed to work together and share an interop path, and matching licences
means code can move between them in either direction.
