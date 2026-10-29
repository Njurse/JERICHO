@echo off
rem ============================================================================
rem  rebake.bat [mode]        -  bake the Firebird bank and install it
rem
rem  Bakes bank.json into FIREBIRD.LEV/.LCF and copies the result into the repo
rem  folder AND the game's mirror (src_rebuild\bin\Release_dev\...). The game
rem  reads the mirror, so installing to only one of the two is why a bake can
rem  "not show up".
rem
rem  Modes (default: full):
rem    full      mirror Y, reverse the winding, negate the normal tables
rem    plain     no transform at all - the OBJ exactly as it stands
rem    flipy     mirror Y only
rem    winding   reverse the winding only (for an inside-out car)
rem    normals   negate the normal tables only
rem
rem  What the three switches do, because they are easy to confuse:
rem    --flip-y        negates every vertex's Y. Decides whether the car is
rem                    upright or upside down.
rem    --flip-winding  reverses each face's vertex order. Decides which side of
rem                    a polygon is culled, i.e. inside-out or not.
rem    --flip-normals  negates the stored normal tables. bake stores them
rem                    INWARD, so a mesh from another tool can shade from the
rem                    wrong side while the shape is correct.
rem
rem  After baking: play_chicago.bat takes a ride in CHICAGO in the Firebird.
rem  If the atlas changed, run the page step first:
rem    python ..\..\..\bake\tools\car\carbank_page.py textures\firebird.png --set 1 --bank .
rem ============================================================================
setlocal

set "MODE=%~1"
if "%MODE%"=="" set "MODE=full"

if /i "%MODE%"=="help"   goto usage
if /i "%MODE%"=="-h"     goto usage
if /i "%MODE%"=="--help" goto usage
if /i "%MODE%"=="/?"     goto usage

set "FLIP=--flip-y --flip-winding --flip-normals"
if /i "%MODE%"=="plain"   set "FLIP="
if /i "%MODE%"=="flipy"   set "FLIP=--flip-y"
if /i "%MODE%"=="winding" set "FLIP=--flip-winding"
if /i "%MODE%"=="normals" set "FLIP=--flip-normals"
if /i "%MODE%"=="full"    goto mode_ok

rem anything else is a typo: say so instead of silently baking the default
echo rebake: unknown mode "%MODE%"
echo.
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
echo   rebake.bat [mode]
echo.
echo   mode   what it bakes
echo     full      mirror Y, reverse the winding, negate the normals (default)
echo     plain     no transform at all
echo     flipy     mirror Y only              - car upside down? this is the one
echo     winding   reverse the winding only   - car inside out? this is the one
echo     normals   negate the normals only    - right shape, wrong shading?
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
