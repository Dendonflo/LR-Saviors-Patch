# Crash log

Crashes captured by the shipped VEH (`ModCrashVeh`, `19_boot_install.c:431`),
with the analysis done at the time. Kept because this game is not especially
stable on its own and a crash we have already reasoned about is worth far more
later than a fresh one: the useful half of a crash report is the context around
it, and that is exactly what evaporates.

**How to read one of these.** `[crash]` lines carry module + offset and survive
the release log filter, so every user report with a log is actionable. Our own
DLL's frames are deliberately absent from the `stack:` line - the sweep prints
only values inside the GAME exe, rebased to Ghidra addresses (`v -
g_mainModBase + 0x400000`), and it is a RAW SCAN of 512 stack words rather than
a real unwind, so entries can be stale leftovers. Treat it as a hint, not a
call chain. Offsets in our DLL resolve against `dinput8_new.map`.

---

## 2026-08-18 — access violation switching fullscreen -> windowed

Build `1.1 BETA` (Aug 18 2026 16:46:21). Not reproduced deliberately; the user
was mode-switching during in-game UI testing. **Not investigated further by
decision** - the game is unstable generally, mode switching is rare in normal
play, and nothing depended on it. Recorded for if it recurs.

```
[crash] FIRST-CHANCE code=0xC0000005 addr=3CF24393 in <unknown module>+0x0
        thread=17116 accessing=0x3CF24393
[crash]   stack: 00DE7E01 00CF1FF5 00CF2138 00CF2138 00CF2628 00CF1E22 00CF2138
[stutter] elapsed_usec=5045913 EIP=ntdll.dll+7A08C reads=0/0KB pace_usec=0 allocs=0
[stutter]   ebp: ntdll.dll+D2086 ntdll.dll+D1B18 ntdll.dll+FC9DE ntdll.dll+C5CF9
```

### What is certain

`addr` and `accessing` are **the same value**, and it is in no loaded module.
That makes this an instruction-fetch fault, not a bad data access: the CPU was
told to EXECUTE at `0x3CF24393` and that page is not mapped. So this is a call
or jump through a garbage / freed function pointer. It rules out the usual
suspects - null derefs, released D3D surfaces, use-after-free of a COM object -
because all of those fault with EIP still inside real code.

The mode change itself COMPLETED before the fault: the log shows targets
rebuilt at 1920x1080, then `SCENE#4` appearing at 3840x2160, so the device
reset and the AO/MSAA latches re-established normally.

### What the stack says (weakly)

Disassembled against the shipped exe (`tools`-style capstone script, scratchpad):

| Ghidra VA | what is there |
|---|---|
| `00CF2628` | code containing `0xBB40E64F` - the MSVC `__security_cookie` sentinel, i.e. `__security_init_cookie` |
| `00CF1FF5` | `push 0x400000` (image base) then `call`, followed by `call dword ptr [0xde93a4]` - an IAT thunk |
| `00CF1E22` | inside a run of `jmp dword ptr [0xde____]` import thunks |

That is the CRT startup/teardown region, not the render path. But the sweep
prints ANY exe-range value it finds on the stack, so on a deep stack these are
very plausibly stale leftovers rather than the live chain. Weak evidence.

The more suggestive thing is what is **missing**: no render-path frames at all.
A fault reached by OUR code calling a bad pointer would look exactly like this,
because the handler skips reporting when EIP is inside our module and the sweep
discards non-exe addresses - so our return addresses would be invisible.

### Leading hypothesis, untested

`ResolveOrigSlot` (`12_locks_staging.c:63`) does not always return a d3d9.dll
address: when a vtable slot is already one of ours it decodes a thunk out of
`g_d3dThunks` (a separately allocated arena) and returns `g_d3dOrig[id]`. So
some `g_orig*` pointers derive from allocated memory rather than a DLL's
`.rdata`, and a pointer into a freed/rebuilt arena, called after a device
rebuild, produces precisely this signature.

Three things make it the first thing to test: the timing (a mode change is when
device objects get rebuilt), the fact that the EndScene hook was hours old and
runs ~4x per frame (`EndScene calls=2209 over 601 frames`), and that a stale
`g_origEndScene` would fault with EIP outside every module while pushing a
return address inside our DLL that the sweep would hide.

**The cheap discriminator, if this ever matters:** reproduce the mode switch
with `InGameUi=0`. Still crashes -> everything from that day is exonerated and
it is a pre-existing device-rebuild bug. Stops -> the EndScene path owns it, and
the fix is a device-identity check before calling through cached pointers.

### Unrelated, from the same log

- `[config] reset to defaults` lines appear in exact PAIRS - `(AO tuning only)`
  twice in a row, `(everything)` twice in a row, repeatedly. Each line is one
  `CfgResetDefaults` call, so something invokes it twice per action. May just be
  the arm-then-fire flow being clicked through; worth confirming rather than
  assuming.
- The frametime watchdog caught the aftermath, not a second fault:
  `elapsed_usec=5045913` with the main thread in ntdll is exception dispatch.
