// Passthrough wrappers for every real version.dll export. Nothing else in
// the game (or any other mod/overlay that also depends on version.dll, e.g.
// via a chained proxy) should be able to tell this isn't the real DLL.
//
// Each wrapper resolves lazily-cached pointers into the REAL system
// version.dll (loaded from an explicit System32 path in dllmain.c, so this
// never recurses into itself) and forwards the call unchanged. If a given
// export somehow fails to resolve, we fail safe (return a benign
// error/zero) instead of crashing on a null call.

#include "proxy.h"

HMODULE g_hRealVersionDll = NULL;

typedef BOOL(WINAPI *PFN_GetFileVersionInfoA)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *PFN_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *PFN_GetFileVersionInfoExA)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *PFN_GetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI *PFN_GetFileVersionInfoSizeA)(LPCSTR, LPDWORD);
typedef DWORD(WINAPI *PFN_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
typedef DWORD(WINAPI *PFN_GetFileVersionInfoSizeExA)(DWORD, LPCSTR, LPDWORD);
typedef DWORD(WINAPI *PFN_GetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD);
typedef DWORD(WINAPI *PFN_VerFindFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
typedef DWORD(WINAPI *PFN_VerFindFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD(WINAPI *PFN_VerInstallFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
typedef DWORD(WINAPI *PFN_VerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD(WINAPI *PFN_VerLanguageNameA)(DWORD, LPSTR, DWORD);
typedef DWORD(WINAPI *PFN_VerLanguageNameW)(DWORD, LPWSTR, DWORD);
typedef BOOL(WINAPI *PFN_VerQueryValueA)(LPCVOID, LPCSTR, LPVOID *, PUINT);
typedef BOOL(WINAPI *PFN_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
// GetFileVersionInfoByHandle is undocumented/rarely used; included for
// completeness per "passthrough every version.dll function". Harmless if it
// fails to resolve or is never called - guarded like everything else below.
typedef BOOL(WINAPI *PFN_GetFileVersionInfoByHandle)(HANDLE, DWORD, DWORD, LPVOID);

static PFN_GetFileVersionInfoA pGetFileVersionInfoA;
static PFN_GetFileVersionInfoW pGetFileVersionInfoW;
static PFN_GetFileVersionInfoExA pGetFileVersionInfoExA;
static PFN_GetFileVersionInfoExW pGetFileVersionInfoExW;
static PFN_GetFileVersionInfoSizeA pGetFileVersionInfoSizeA;
static PFN_GetFileVersionInfoSizeW pGetFileVersionInfoSizeW;
static PFN_GetFileVersionInfoSizeExA pGetFileVersionInfoSizeExA;
static PFN_GetFileVersionInfoSizeExW pGetFileVersionInfoSizeExW;
static PFN_VerFindFileA pVerFindFileA;
static PFN_VerFindFileW pVerFindFileW;
static PFN_VerInstallFileA pVerInstallFileA;
static PFN_VerInstallFileW pVerInstallFileW;
static PFN_VerLanguageNameA pVerLanguageNameA;
static PFN_VerLanguageNameW pVerLanguageNameW;
static PFN_VerQueryValueA pVerQueryValueA;
static PFN_VerQueryValueW pVerQueryValueW;
static PFN_GetFileVersionInfoByHandle pGetFileVersionInfoByHandle;

#define RESOLVE(name) \
    p##name = (PFN_##name)(g_hRealVersionDll ? GetProcAddress(g_hRealVersionDll, #name) : NULL)

void ResolveRealExports(void)
{
    RESOLVE(GetFileVersionInfoA);
    RESOLVE(GetFileVersionInfoW);
    RESOLVE(GetFileVersionInfoExA);
    RESOLVE(GetFileVersionInfoExW);
    RESOLVE(GetFileVersionInfoSizeA);
    RESOLVE(GetFileVersionInfoSizeW);
    RESOLVE(GetFileVersionInfoSizeExA);
    RESOLVE(GetFileVersionInfoSizeExW);
    RESOLVE(VerFindFileA);
    RESOLVE(VerFindFileW);
    RESOLVE(VerInstallFileA);
    RESOLVE(VerInstallFileW);
    RESOLVE(VerLanguageNameA);
    RESOLVE(VerLanguageNameW);
    RESOLVE(VerQueryValueA);
    RESOLVE(VerQueryValueW);
    RESOLVE(GetFileVersionInfoByHandle);
}

BOOL WINAPI GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d)
{
    return pGetFileVersionInfoA ? pGetFileVersionInfoA(a, b, c, d) : FALSE;
}

BOOL WINAPI GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d)
{
    return pGetFileVersionInfoW ? pGetFileVersionInfoW(a, b, c, d) : FALSE;
}

BOOL WINAPI GetFileVersionInfoExA(DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e)
{
    return pGetFileVersionInfoExA ? pGetFileVersionInfoExA(a, b, c, d, e) : FALSE;
}

BOOL WINAPI GetFileVersionInfoExW(DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e)
{
    return pGetFileVersionInfoExW ? pGetFileVersionInfoExW(a, b, c, d, e) : FALSE;
}

DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b)
{
    return pGetFileVersionInfoSizeA ? pGetFileVersionInfoSizeA(a, b) : 0;
}

DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b)
{
    return pGetFileVersionInfoSizeW ? pGetFileVersionInfoSizeW(a, b) : 0;
}

DWORD WINAPI GetFileVersionInfoSizeExA(DWORD a, LPCSTR b, LPDWORD c)
{
    return pGetFileVersionInfoSizeExA ? pGetFileVersionInfoSizeExA(a, b, c) : 0;
}

DWORD WINAPI GetFileVersionInfoSizeExW(DWORD a, LPCWSTR b, LPDWORD c)
{
    return pGetFileVersionInfoSizeExW ? pGetFileVersionInfoSizeExW(a, b, c) : 0;
}

DWORD WINAPI VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h)
{
    return pVerFindFileA ? pVerFindFileA(a, b, c, d, e, f, g, h) : 0;
}

DWORD WINAPI VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h)
{
    return pVerFindFileW ? pVerFindFileW(a, b, c, d, e, f, g, h) : 0;
}

DWORD WINAPI VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h)
{
    return pVerInstallFileA ? pVerInstallFileA(a, b, c, d, e, f, g, h) : 0;
}

DWORD WINAPI VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h)
{
    return pVerInstallFileW ? pVerInstallFileW(a, b, c, d, e, f, g, h) : 0;
}

DWORD WINAPI VerLanguageNameA(DWORD a, LPSTR b, DWORD c)
{
    return pVerLanguageNameA ? pVerLanguageNameA(a, b, c) : 0;
}

DWORD WINAPI VerLanguageNameW(DWORD a, LPWSTR b, DWORD c)
{
    return pVerLanguageNameW ? pVerLanguageNameW(a, b, c) : 0;
}

BOOL WINAPI VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID *c, PUINT d)
{
    return pVerQueryValueA ? pVerQueryValueA(a, b, c, d) : FALSE;
}

BOOL WINAPI VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d)
{
    return pVerQueryValueW ? pVerQueryValueW(a, b, c, d) : FALSE;
}

BOOL WINAPI GetFileVersionInfoByHandle(HANDLE a, DWORD b, DWORD c, LPVOID d)
{
    return pGetFileVersionInfoByHandle ? pGetFileVersionInfoByHandle(a, b, c, d) : FALSE;
}
