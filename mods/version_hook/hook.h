#pragma once

// Installs the animation/skeleton prefetch hook. Safe to call once the
// process's own main module is loaded (i.e. from a thread, not directly
// inside DllMain's DLL_PROCESS_ATTACH).
void InstallPrefetchHook(void);

// The small subset of setup that MUST happen before the game's entry point
// runs, and therefore must be called synchronously from DllMain rather than
// from the deferred install thread.
//
// Everything else is deliberately deferred (see the loader-lock reasoning in
// dllmain.c), but device creation cannot be: the game calls
// Direct3DCreate9 within a couple of seconds of start, and the deferred
// thread demonstrably loses that race - a test run installed the IAT hook
// only AFTER the real device already existed, so the hook never fired.
// This does no loader-lock-unsafe work: it patches the exe's own already-
// snapped import table and reads a config file, with no LoadLibrary,
// no GetProcAddress against other modules, and no calls into d3d9.dll.
void InstallEarlyHooks(void);
