@echo off
REM Build the dinput8.dll proxy + hook. 32-bit: the game is a 32-bit exe.
REM Output is dinput8_new.dll so an existing deployed dinput8.dll is never
REM overwritten in place - copy it over manually once it has been tested.
REM
REM Was version.dll until 2026-08-11; the HD GUI mod occupies that name.
REM version.def and proxy_version.c are kept unbuilt so the old target can be
REM restored by swapping proxy.c/proxy_version.c and the /DEF below.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (echo vcvars32 failed & exit /b 1)
cd /d "%~dp0"
REM /MAP: resolving dinput8.dll+NNNN offsets out of stutter stack walks was
REM being done by guesswork; the map turns that into a lookup.
cl /nologo /O2 /W3 /LD dllmain.c hook.c proxy.c ^
   /Fe:dinput8_new.dll ^
   /link /DEF:dinput8.def user32.lib gdi32.lib comctl32.lib ^
   /MAP:dinput8_new.map /MAPINFO:EXPORTS
exit /b %errorlevel%
