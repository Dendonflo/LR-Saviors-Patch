# Lightning Returns: Final Fantasy XIII — asset streaming & stutter mod

A D3D9 mod for the Steam release of *Lightning Returns: Final Fantasy XIII*,
built to remove the frame-time stutter the PC port suffers from while
streaming assets, and to add graphics options the original never exposed.

Ships as a `dinput8.dll` proxy. All options live in the game's own Win32 menu
bar — there is no separate window and nothing is loaded from disk at runtime.

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

**Graphics additions** — native SSAA (scene only, UI excluded), MSAA, shadow
map resolution up to 8192, shadow cascade distance, FXAA removal, frame-rate
target (30 / 60 / unlimited), and a frame-time overlay that graphs the *engine
tick* rather than presents.

## Building

32-bit MSVC. Run `mods/version_hook/build.cmd` — it calls `vcvars32.bat`,
compiles `dllmain.c` + `hook.c` + `proxy.c`, and links against
`dinput8.def`. Output is `dinput8_new.dll`; copy it into the game directory as
`dinput8.dll`.

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
mods/version_hook/     the mod itself (hook.c is the bulk of it)
tools/ghidra_scripts/  headless Ghidra scripts used for the analysis
tools/clb/             tools for reading the engine's script resources
tools/*.ps1            log monitoring helpers
```

`hook.c` carries its own reasoning inline. Nearly every fix sits under a
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
