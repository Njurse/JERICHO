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
rem everything the bake generates lands in build\ (bank.json: "build_dir")
set "OUT=%BANK%\build"

for %%I in ("%BANK%\..\..\..\..\..") do set "ROOT=%%~fI"
set "MIRROR=%ROOT%\src_rebuild\bin\Release_dev\JERICHO\MODS\betacars\banks\firebird"

if not exist "%BANK%\bank.json" goto no_bank
if not exist "%CAR%\carbank_bake.py" goto no_tools

rem the bake writes into build\ and the result is copied in afterwards: baking
rem with -o pointing AT the bank folder overwrites the LEV it reads from and
rem silently produces an empty bank (20480 bytes, 0 cars). build\ is kept
rem between bakes (pages/ and models/ are reused when nothing changed); delete
rem it any time for a clean bake. Only the old LEV/LCF go, so a failed bake
rem cannot install a stale one.
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\FIREBIRD.LEV" del /q "%OUT%\FIREBIRD.LEV"
if exist "%OUT%\FIREBIRD.LCF" del /q "%OUT%\FIREBIRD.LCF"

set "LOG=%OUT%\rebake.log"
echo.
echo  ==== Firebird rebake ===================================================
echo   mode : %MODE%   [%FLIP%]
echo   bank : %BANK%\bank.json
echo   log  : %LOG%
echo  ------------------------------------------------------------------------
echo   baking...
python "%CAR%\carbank_bake.py" "%BANK%\bank.json" -o "%OUT%" --scale 265 --default-page 1:0 %FLIP% > "%LOG%" 2>&1
set "BAKERC=%ERRORLEVEL%"

rem show only what matters: sound notes, other warnings, and the summary.
rem the per-material import chatter stays in rebake.log
echo.
echo   sounds:
findstr /i /c:"sound model" "%LOG%" >nul && (for /f "usebackq delims=" %%L in (`findstr /i /c:"sound model" "%LOG%"`) do echo     %%L) || echo     (no sound notes)
echo   other warnings:
findstr /i "warning" "%LOG%" | findstr /v /i /c:"sound model" >nul && (for /f "usebackq delims=" %%L in (`findstr /i "warning" "%LOG%" ^| findstr /v /i /c:"sound model"`) do echo     %%L) || echo     (none)
echo.
if not "%BAKERC%"=="0" goto bake_failed
if not exist "%OUT%\FIREBIRD.LEV" goto bake_failed
for /f "usebackq delims=" %%L in (`findstr /i /c:"bank ok" "%LOG%"`) do echo   %%L

echo   installing to repo + game mirror...
copy /Y "%OUT%\FIREBIRD.LEV" "%BANK%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LCF" "%BANK%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LEV" "%MIRROR%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LCF" "%MIRROR%" >nul || goto fail
echo.
echo  ==== BAKE OK - installed ===============================================
echo.
echo   Take a Ride in the Firebird:
echo     0  Chicago      1  Havana      2  Las Vegas      3  Rio
echo     4  exit         (exits by itself after 10 seconds)
echo.
choice /c 01234 /n /t 10 /d 4 /m "  select 0-4: "
set "PICK=%ERRORLEVEL%"
set "CITY="
if "%PICK%"=="1" set "CITY=chicago"
if "%PICK%"=="2" set "CITY=havana"
if "%PICK%"=="3" set "CITY=lasvegas"
if "%PICK%"=="4" set "CITY=rio"
if "%CITY%"=="" (
    echo   exit - not launching.
    exit /b 0
)
set "EXEDIR=%ROOT%\src_rebuild\bin\Release_dev"
if not exist "%EXEDIR%\JERICHO_dev.exe" (
    echo FAILED: no game at %EXEDIR%\JERICHO_dev.exe
    exit /b 1
)
echo   launching Take a Ride in %CITY%...
pushd "%EXEDIR%"
start "" "JERICHO_dev.exe" -nointro -level %CITY% -car 1 -gamemode takeadrive -weather none -time day
popd
exit /b 0

:bake_failed
echo  ==== BAKE FAILED - nothing was installed ===============================
echo   last lines of the log:
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' -Tail 8 | ForEach-Object { '    ' + $_ }"
exit /b 1

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
