/*
 * Lightning Returns: Final Fantasy XIII - asset streaming & stutter mod
 * Copyright (C) 2026  Dendonflo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// Standalone dinput8.dll proxy. Loads the REAL dinput8 (never this file's own
// DLL - see LoadRealDinput8) and forwards every export unchanged (proxy.c),
// so nothing depending on dinput8 can tell this isn't the genuine DLL.
// Separately, on its own thread (never inside DllMain itself, to stay clear
// of the loader lock), installs the animation-prefetch hook from hook.c.
//
// Was version.dll until 2026-08-11; moved because the HD GUI mod occupies
// that name and only one DLL can. Reasoning for dinput8 specifically is in
// proxy.c's header.

#include <windows.h>
#include <stdio.h>
#include "proxy.h"
#include "hook.h"

static void LogStartup(const char *msg)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) {
        strcpy(slash + 1, "version_hook.log");
    }
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

// Resolve the DLL we forward to, in priority order:
//
//   1. dinput8_chain.dll in the game directory - the CHAIN slot. dinput8 is
//      also LayeredFS's name, and only one file can be called dinput8.dll.
//      Renaming that mod to dinput8_chain.dll makes this proxy load it, and
//      since LayeredFS is itself a dinput8 proxy it goes on to load the system
//      one - so both mods run with no ASI loader and no other moving parts.
//
//      The name is deliberately NOT dinput8_.dll: a trailing underscore is
//      how a parked/disabled mod is marked in practice (this install has both
//      d3d9_.dll and dinput8_.dll sitting parked), so chaining to it would
//      silently re-enable something the user had switched off. Chaining must
//      be an explicit act, hence an explicit name.
//   2. System32\dinput8.dll - the normal case, nothing else in the chain.
//      Explicit full path so this can never resolve back to our own DLL.
//
// A chain target that is NOT a dinput8 proxy would break input, so the load
// is verified: the file must actually export DirectInput8Create before it is
// accepted, otherwise fall through to System32.
static void LoadRealDinput8(void)
{
    char path[MAX_PATH];
    char *slash;

    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) {
        strcpy(slash + 1, "dinput8_chain.dll");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            HMODULE h = LoadLibraryA(path);
            if (h && GetProcAddress(h, "DirectInput8Create")) {
                g_hRealDinput8 = h;
                LogStartup("Chain-loaded dinput8_chain.dll from the game directory "
                           "(another dinput8 proxy, e.g. LayeredFS)");
                return;
            }
            if (h) {
                FreeLibrary(h);
                LogStartup("dinput8_chain.dll present but exports no DirectInput8Create "
                           "- ignoring it and using System32");
            }
        }
    }

    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\dinput8.dll");
    g_hRealDinput8 = LoadLibraryA(path);
    LogStartup(g_hRealDinput8
        ? "Real dinput8.dll loaded OK from System32"
        : "FAILED to load real dinput8.dll from System32 - passthrough exports will fail safe");
}

static DWORD WINAPI HookInstallThread(LPVOID param)
{
    (void)param;
    InstallPrefetchHook();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        LogStartup("=== dinput8.dll (prefetch hook) attached ===");
        LoadRealDinput8();
        ResolveRealExports();
        // Synchronous, before the game's entry point runs. Only the pieces
        // that MUST beat the game's own startup live here - see
        // InstallEarlyHooks' comment in hook.h for why deferring this
        // particular hook demonstrably does not work.
        InstallEarlyHooks();
        CreateThread(NULL, 0, HookInstallThread, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        if (g_hRealDinput8) {
            FreeLibrary(g_hRealDinput8);
        }
        break;
    }
    return TRUE;
}
