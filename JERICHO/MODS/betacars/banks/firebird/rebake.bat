@echo off
rem ============================================================================
rem  rebake.bat [mode]        -  bake the Firebird bank and install it
rem
rem  Bakes bank.json into FIREBIRD.LEV/.LCF and copies it into the repo folder
rem  AND the game's mirror (src_rebuild\bin\Release_dev\...). The game reads the
rem  mirror, so installing to only one of the two is why a bake can appear to
rem  do nothing.
rem
rem  Modes (default: flipy):
rem    flipy     negate Y only            - upright, faces as authored
rem    full      negate Y and Z           - a 180-degree rotation about X
rem    winding   reverse the winding only
rem    normals   negate the normal tables only
rem    flipfull  negate Y, reverse winding, negate normals (the old "full")
rem    plain     no transform at all
rem
rem  The switches, because they are easy to confuse:
rem    --flip-y        negates every vertex's Y. Decides upright vs upside down.
rem    --flip-z        negates Z as well; Y+Z is a ROTATION about X (handedness
rem                    kept), while either alone is a MIRROR and mirrors turn a
rem                    model inside out.
rem    --flip-winding  reverses each face's vertex order. Decides which side is
rem                    culled.
rem    --flip-normals  negates the stored normal tables; bake stores them INWARD.
rem
rem  After baking:  play_chicago.bat        (or: play_chicago.bat test 250)
rem  The pages and the palette are re-derived by the bake itself: a page is
rem  re-baked whenever anything it was baked from changes - its image, its
rem  transparent key/tolerance, its palette, or one of the wheel sprites that get
rem  composited into it. The backdrop is keyed SH there (the engine's marker is a
rem  CLUT entry of 0x0000, so a pink sky left alone draws as an opaque square
rem  round the wheel), and the wheel island is carved from the sprite declared in
rem  bank.json - not inherited from the page art.
rem
rem    python ..\..\..\bake\tools\car\carbank_bake.py bank.json --check
rem        is anything stale? (pages, palette, declared sources)
rem    python ..\..\..\bake\tools\car\carbank_bake.py bank.json --audit f800f8
rem        where does a colour still live? (page texels, palette entries, and
rem        whether a car poly samples it)
rem    python ..\..\..\bake\tools\car\carbank_bake.py bank.json --force-pages
rem        re-bake every page even when nothing it came from changed (use after
rem        changing the bake itself)
rem    python ..\..\..\bake\tools\car\carbank_bake.py bank.json --complete --dry-run
rem        which sound knobs is this bank leaving to the bake? (add them
rem        with --complete - the numbers written are the ones used, so
rem        the bank still bakes byte for byte the same)
rem    python ..\..\..\bake\tools\car\carbank_palette.py bank.json --from-pages
rem        rebuild the palette rows from the pages' own colours
rem    python ..\..\..\bake\tools\car\carbank_page.py <image> --set N --bank .
rem        bake ONE page by hand (--transparent/--protect/--palette/--report)
setlocal

set "MODE=%~1"
if "%MODE%"=="" set "MODE=flipy"

if /i "%MODE%"=="help"    goto usage
if /i "%MODE%"=="-h"      goto usage
if /i "%MODE%"=="--help"  goto usage
if /i "%MODE%"=="/?"      goto usage

set "FLIP=--flip-y"
if /i "%MODE%"=="plain"    set "FLIP="
if /i "%MODE%"=="full"     set "FLIP=--flip-y --flip-z"
if /i "%MODE%"=="winding"  set "FLIP=--flip-winding"
if /i "%MODE%"=="normals"  set "FLIP=--flip-normals"
if /i "%MODE%"=="flipfull" set "FLIP=--flip-y --flip-winding --flip-normals"

if /i "%MODE%"=="flipy"    goto mode_ok
if /i "%MODE%"=="plain"    goto mode_ok
if /i "%MODE%"=="full"     goto mode_ok
if /i "%MODE%"=="winding"  goto mode_ok
if /i "%MODE%"=="normals"  goto mode_ok
if /i "%MODE%"=="flipfull" goto mode_ok

echo rebake: unknown mode "%MODE%"
goto usage

:mode_ok
set "BANK=%~dp0"
set "BANK=%BANK:~0,-1%"
set "CAR=%BANK%\..\..\..\bake\tools\car"
set "OUT=%BANK%\.out"

for %%I in ("%BANK%\..\..\..\..\..") do set "ROOT=%%~fI"
set "MIRROR=%ROOT%\src_rebuild\bin\Release_dev\JERICHO\MODS\betacars\banks\firebird"

if not exist "%BANK%\bank.json" goto no_bank
if not exist "%CAR%\carbank_bake.py" goto no_tools

rem the bake writes to a scratch folder and is copied in afterwards: baking with
rem -o pointing AT the bank folder rewrites the manifest mid-read and silently
rem produces an empty bank (20480 bytes, 0 cars)
if exist "%OUT%" rmdir /s /q "%OUT%"

echo mode  : %MODE%   [%FLIP%]
echo baking: %BANK%\bank.json
python "%CAR%\carbank_bake.py" "%BANK%\bank.json" -o "%OUT%" --scale 265 --default-page 1:0 %FLIP%
if errorlevel 1 goto fail
if not exist "%OUT%\FIREBIRD.LEV" goto fail

echo installing
copy /Y "%OUT%\FIREBIRD.LEV" "%BANK%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LCF" "%BANK%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LEV" "%MIRROR%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LCF" "%MIRROR%" >nul || goto fail
rmdir /s /q "%OUT%"

echo done. next:  play_chicago.bat        (or: play_chicago.bat test 250)
exit /b 0

:usage
echo.
echo   rebake.bat [mode]        (default: flipy)
echo.
echo     flipy     negate Y only            upright, faces as authored
echo     full      negate Y and Z           180-degree rotation about X
echo     winding   reverse the winding only
echo     normals   negate the normal tables only
echo     flipfull  Y + winding + normals    the old "full"
echo     plain     no transform at all
echo.
echo   Installs into the repo folder and the game mirror both.
exit /b 2

:no_bank
echo FAILED: no bank.json in %BANK%
exit /b 1

:no_tools
echo FAILED: no bake tools at %CAR%
echo (JERICHO\MODS\bake is a submodule: git submodule update --init JERICHO/MODS/bake)
exit /b 1

:fail
echo FAILED - nothing was installed
exit /b 1
