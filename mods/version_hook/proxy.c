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
// Passthrough wrappers for every real dinput8.dll export.
//
// Moved from version.dll to dinput8.dll (2026-08-11): version.dll is taken by
// the HD GUI mod, and both cannot occupy the same name. dinput8 is the best
// remaining slot on measurement, not on taste:
//   - it IS statically imported by LRFF13.exe (first entry in the import
//     table), so a DLL of this name next to the exe really does get loaded;
//   - it is NOT a Windows KnownDLL, so the loader takes ours rather than
//     silently resolving System32 (that rules out imagehlp/imm32/ws2_32/etc);
//   - the real DLL has SIX exports, all by name, ordinals 1-6 - the smallest
//     passthrough surface of any candidate (version.dll 17, winmm 193,
//     d3dx9_43 329), so there is very little here that can go wrong;
//   - the exe imports exactly one of them (DirectInput8Create).
// The one known occupant of this slot is LayeredFS, a modder-facing tool
// whose users can chain-load - and the chain below makes that automatic.
//
// Each wrapper resolves lazily-cached pointers into the REAL dinput8 (see
// LoadRealDinput8 in dllmain.c, which never resolves to this file's own DLL)
// and forwards the call unchanged. If an export fails to resolve we fail safe
// (benign error / null) rather than calling through a null pointer.
//
// Types are deliberately kept as void*/HRESULT rather than pulling in
// dinput.h: nothing here inspects an argument, it only forwards, so what
// matters is the calling convention and the argument count - both of which
// are pinned by the signatures below and by dinput8.def's ordinals.

#include "proxy.h"

HMODULE g_hRealDinput8 = NULL;

typedef HRESULT (WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, const void *, void **, void *);
typedef HRESULT (WINAPI *PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI *PFN_DllGetClassObject)(const void *, const void *, void **);
typedef HRESULT (WINAPI *PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllUnregisterServer)(void);
typedef const void * (WINAPI *PFN_GetdfDIJoystick)(void);

static PFN_DirectInput8Create   pDirectInput8Create;
static PFN_DllCanUnloadNow      pDllCanUnloadNow;
static PFN_DllGetClassObject    pDllGetClassObject;
static PFN_DllRegisterServer    pDllRegisterServer;
static PFN_DllUnregisterServer  pDllUnregisterServer;
static PFN_GetdfDIJoystick      pGetdfDIJoystick;

#define RESOLVE(name) \
    p##name = (PFN_##name)(g_hRealDinput8 ? GetProcAddress(g_hRealDinput8, #name) : NULL)

void ResolveRealExports(void)
{
    RESOLVE(DirectInput8Create);
    RESOLVE(DllCanUnloadNow);
    RESOLVE(DllGetClassObject);
    RESOLVE(DllRegisterServer);
    RESOLVE(DllUnregisterServer);
    RESOLVE(GetdfDIJoystick);
}

HRESULT WINAPI DirectInput8Create(HINSTANCE a, DWORD b, const void *c, void **d, void *e)
{
    // E_FAIL rather than a crash if the real DLL is missing: the game checks
    // this HRESULT, so it degrades to "no DirectInput" instead of a null call.
    return pDirectInput8Create ? pDirectInput8Create(a, b, c, d, e) : E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    // S_FALSE = "do not unload", the safe answer when we cannot ask.
    return pDllCanUnloadNow ? pDllCanUnloadNow() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(const void *a, const void *b, void **c)
{
    if (!pDllGetClassObject) {
        if (c) *c = NULL;
        return E_FAIL;   // CLASS_E_CLASSNOTAVAILABLE is the stricter answer,
                         // but E_FAIL keeps this file free of COM headers.
    }
    return pDllGetClassObject(a, b, c);
}

HRESULT WINAPI DllRegisterServer(void)
{
    return pDllRegisterServer ? pDllRegisterServer() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    return pDllUnregisterServer ? pDllUnregisterServer() : E_FAIL;
}

const void * WINAPI GetdfDIJoystick(void)
{
    return pGetdfDIJoystick ? pGetdfDIJoystick() : NULL;
}
