@echo off
rem Re-bake the Firebird and install it into the game's mirror.
rem
rem Run this after editing anything in this folder (models/, pages/, textures/).
rem If the ATLAS changed, run the page step first (it rebuilds pages/PAGE_1.bin):
rem   python "..\..\..\bake\tools\car\carbank_page.py" textures\firebird.png --set 1 --bank .
rem
rem The bake writes to a scratch folder and is then copied in: baking with -o
rem pointing AT the bank folder rewrites the manifest mid-read and silently
rem produces an empty bank (20480 bytes, 0 cars).
setlocal

rem pass "flip" to bake with the mesh mirrored in Y. The Firebird source OBJ is
rem Y-up like the engine, so it bakes as authored; the flag exists because a
rem foreign (Blender) mesh may need it. rebake.bat flip also reverses the
rem winding, which a Y mirror requires or the car is inside out.
rem  mode is optional:  flipy (default) mirrors Y only, flip does Y + winding,
rem  noflip bakes as the OBJ stands.
set "FLIP=--flip-y"
if /i "%~1"=="flip" set "FLIP=--flip-y --flip-winding"
if /i "%~1"=="noflip" set "FLIP="
set BANK=%~dp0
set BANK=%BANK:~0,-1%
set CAR=%BANK%\..\..\..\bake\tools\car
set OUT=%BANK%\.out

for %%I in ("%BANK%\..\..\..\..\..") do set ROOT=%%~fI
set MIRROR=%ROOT%\src_rebuild\bin\Release_dev\JERICHO\MODS\betacars\banks\firebird

if exist "%OUT%" rmdir /s /q "%OUT%"
echo baking %BANK%\bank.json
python "%CAR%\carbank_bake.py" "%BANK%\bank.json" -o "%OUT%" --scale 265 --default-page 1:0 %FLIP%
if errorlevel 1 goto fail
if not exist "%OUT%\FIREBIRD.LEV" goto fail

echo installing
copy /Y "%OUT%\FIREBIRD.LEV" "%BANK%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LCF" "%BANK%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LEV" "%MIRROR%" >nul || goto fail
copy /Y "%OUT%\FIREBIRD.LCF" "%MIRROR%" >nul || goto fail
rmdir /s /q "%OUT%"

echo done - start the game with:  JERICHO_dev.exe -level HAVANA -nointro -playercar 1
exit /b 0

:fail
echo FAILED - nothing was installed
exit /b 1
