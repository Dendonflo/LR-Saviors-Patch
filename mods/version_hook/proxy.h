#pragma once
#include <windows.h>

// The real dinput8.dll this proxy forwards to. Set by LoadRealDinput8 in
// dllmain.c, which prefers a chain-load target in the game directory before
// falling back to System32 - see the comment there.
extern HMODULE g_hRealDinput8;

void ResolveRealExports(void);
