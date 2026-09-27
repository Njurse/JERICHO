@echo off
setlocal enabledelayedexpansion
title Caine's Crossfire - Arena tool

rem ============================================================================
rem arena_menu.bat - make, open and test Caine's Crossfire arenas.
rem
rem   Double-click it (or run it from tools\). Arenas live in the mod's own
rem   folder, and the game reads AND writes that same folder (a dev build) - so
rem   the Python editor and the game edit one file:
rem
rem     arenas   JERICHO\MODS\cainescrossfire\arenas   (the file both use)
rem     game     <bin>\...\MODS\...\arenas             (an installed copy's fallback)
rem
rem   Option 7 checks the setup (interpreter, tkinter/Pillow, folders, game exe)
rem   and is the thing to run when the editor will not open.
rem   See ..\ARENAS.md for the .cca file format.
rem ============================================================================

set "TOOLS=%~dp0"

rem --- find the repo ---------------------------------------------------------
rem The folder holding BOTH the mod and src_rebuild. Walk up for it rather than
rem counting ..\..\..\.. from %~dp0: the build copies this script into the bin
rem mirror, and a fixed depth from there lands INSIDE the build - arenas written
rem to that copy are invisible to the game, which reads the repo folder. (That is
rem exactly what happened to pracinhas.)
set "REPO="
set "CAND=%TOOLS%"
:findrepo
for %%I in ("%CAND%\.") do set "CAND=%%~fI"
for %%I in ("%CAND%\..") do set "UP=%%~fI"
if exist "%CAND%\JERICHO\MODS\cainescrossfire" if exist "%CAND%\src_rebuild" set "REPO=%CAND%"
if defined REPO goto :gotrepo
if /i "%UP%"=="%CAND%" goto :gotrepo
set "CAND=%UP%"
goto :findrepo
:gotrepo
if not defined REPO for %%I in ("%TOOLS%..\..\..\..") do set "REPO=%%~fI"

set "ARENAS=%REPO%\JERICHO\MODS\cainescrossfire\arenas"
set "BIN=%REPO%\src_rebuild\bin\Release_dev"
set "GAME_ARENAS=%BIN%\JERICHO\MODS\cainescrossfire\arenas"
set "EXE=%BIN%\REDRIVER2_dev.exe"
rem the repo's editor, not this copy's - a mirror copy is stale
set "EDITOR=%REPO%\JERICHO\MODS\cainescrossfire\tools\arenaedit.py"
if not exist "%EDITOR%" set "EDITOR=%TOOLS%arenaedit.py"

rem --- find a real Python 3 (prefer the py launcher; skip the Store stub) ----
set "PY="
set "PYW="
py -3 --version >nul 2>nul
if not errorlevel 1 set "PY=py -3"
if not defined PY (
  for /f "delims=" %%P in ('where python 2^>nul') do (
    set "CAND=%%P"
    echo !CAND!| findstr /i "WindowsApps" >nul || if not defined PYW set "PYW=!CAND!"
  )
)
if not defined PY if defined PYW set "PY=%PYW%"

if not exist "%ARENAS%" mkdir "%ARENAS%" >nul 2>nul

:menu
cls
echo ================================================================
echo   Caine's Crossfire - arena tool
echo ================================================================
echo   repo:    %REPO%
echo   arenas:  %ARENAS%
if defined PY ( echo   python:  %PY% ) else ( echo   python:  NOT FOUND - install Python 3 to use the editor )
echo.
echo    1) New arena
echo    2) Open an arena in the Python editor
echo    3) Edit an arena in-game
echo    4) Check all arenas
echo    5) Render an arena to a PNG
echo    6) Sync the two arena folders
echo    7) Check the setup
echo    8) Build the level map (a city's top-down rip)
echo    Q) Quit
echo.
set "C="
set /p "C=Choose: "
if /i "%C%"=="1" goto new
if /i "%C%"=="2" goto openpy
if /i "%C%"=="3" goto game
if /i "%C%"=="4" goto check
if /i "%C%"=="5" goto render
if /i "%C%"=="6" goto syncmenu
if /i "%C%"=="7" goto setup
if /i "%C%"=="8" goto levelmap
if /i "%C%"=="q" goto :eof
goto menu

rem ---------------------------------------------------------------------------
:new
cls
echo New arena
echo --------------------------------------------------------------
set "NAME="
set /p "NAME=  file name (letters/digits/underscore, e.g. chicago_docks): "
if "%NAME%"=="" goto menu
set "DISP="
set /p "DISP=  on-screen name [%NAME%]: "
if "%DISP%"=="" set "DISP=%NAME%"
set "CITY="
set /p "CITY=  city (CHICAGO/HAVANA/VEGAS/RIO) [CHICAGO]: "
if "%CITY%"=="" set "CITY=CHICAGO"
set "MP="
set /p "MP=  map (1 = the city's small mp map, 0 = the full city) [1]: "
if "%MP%"=="" set "MP=1"

set "NEWFILE=%ARENAS%\%NAME%.cca"
if exist "%NEWFILE%" (
  echo.
  echo   %NEWFILE% already exists - using it.
) else (
  >"%NEWFILE%"  echo # cainescrossfire arena -- see ARENAS.md
  >>"%NEWFILE%" echo arena: %NAME%
  >>"%NEWFILE%" echo name: %DISP%
  >>"%NEWFILE%" echo city: %CITY%
  >>"%NEWFILE%" echo mp: %MP% 0
  >>"%NEWFILE%" echo region: none
  >>"%NEWFILE%" echo # spawn: x z heading [y]  ^(first = player, rest = opponents; y = height^)
  >>"%NEWFILE%" echo # pickup: weapon ^<name^> x z [ammo]   ^|   pickup: health x z [amount]
  echo.
  echo   created %NEWFILE%
)
call :sync
echo.
echo   open it now?
echo     1) Python editor (top-down)
echo     2) In-game editor
echo     Enter) back to the menu
set "GO="
set /p "GO=  choose: "
if "%GO%"=="1" set "OPENFILE=%NEWFILE%" & if "%GO%"=="1" goto openpy1
if "%GO%"=="2" goto game
goto menu

rem ---------------------------------------------------------------------------
:openpy
call :pick
if errorlevel 1 goto menu
set "OPENFILE=%PICKED%"
:openpy1
if not defined PY goto nopy
call :sync
echo.
echo   opening %OPENFILE% ...
echo.
%PY% "%EDITOR%" "%OPENFILE%"
set "RC=!errorlevel!"
echo.
if not "!RC!"=="0" (
  echo   the editor exited with code !RC! - the message above says why.
  echo   run "7) Check the setup" for the whole picture.
) else (
  echo   saved - arenas live in %ARENAS%
)
pause
call :sync
goto menu

rem ---------------------------------------------------------------------------
:game
if not exist "%EXE%" (
  echo.
  echo   The game is not built: %EXE%
  echo   Build it (Release_dev) first.
  pause
  goto menu
)
call :sync
if not exist "%GAME_ARENAS%" mkdir "%GAME_ARENAS%" >nul 2>nul
echo.
echo   launching with the in-game editor. In the game: Deathmatch, then the
echo   arena you want. L1 place, R1 slot, L2 delete, R2 region corners,
echo   SELECT save, START reload. Close the game when done.
echo.
pushd "%BIN%"
start "" /wait "%EXE%" -nointro -ccmenu -cceditor
popd
call :sync
echo.
echo   done - your edits are copied back to %ARENAS%
pause
goto menu

rem ---------------------------------------------------------------------------
:check
if not defined PY goto nopy
call :sync
echo.
%PY% "%EDITOR%" "%ARENAS%\*.cca" --check
set "RC=!errorlevel!"
echo.
if not "!RC!"=="0" echo   (warnings above - exit code !RC!)
pause
goto menu

rem ---------------------------------------------------------------------------
:render
call :pick
if errorlevel 1 goto menu
if not defined PY goto nopy
call :sync
set "OUT=%ARENAS%\renders"
if not exist "%OUT%" mkdir "%OUT%" >nul 2>nul
for %%F in ("%PICKED%") do set "OUT=%ARENAS%\renders\%%~nF.png"
echo.
%PY% "%EDITOR%" "%PICKED%" --render "%OUT%"
set "RC=!errorlevel!"
if not "!RC!"=="0" echo   the render failed - exit code !RC!
echo.
if exist "%OUT%" start "" "%OUT%"
pause
goto menu

rem ---------------------------------------------------------------------------
:syncmenu
call :sync
echo.
echo   synced - the newer file in each pair wins.
pause
goto menu

rem ---------------------------------------------------------------------------
:levelmap
if not defined PY goto nopy
cls
echo Build the level map
echo --------------------------------------------------------------
echo   A city's DriverLevelTool rip, RENDERED top-down ^(its textured faces,
echo   not a vertex cloud^) and aligned to the game's world coordinates. Both
echo   are cached next to the .obj, so doing this once makes opening an arena
echo   with that map instant afterwards. The preview it opens IS that render.
echo   Rips are gitignored and only RIO ships one, so another city is
echo   exported here first - that is the slow part ^(a few minutes^).
echo.
set "LCITY="
set /p "LCITY=  city (RIO/CHICAGO/HAVANA/VEGAS) [RIO]: "
if "%LCITY%"=="" set "LCITY=RIO"
set "PREVIEW=%REPO%\DriverLevelTool\%LCITY%_LEVELMODEL.preview.png"
echo.
if not exist "%REPO%\DriverLevelTool\%LCITY%_LEVELMODEL.obj" (
  echo   %LCITY% has no rip yet - exporting it with DriverLevelTool.
  echo   ~185 MB, a few minutes; DriverLevelTool opens its own window.
  echo.
  %PY% "%EDITOR%" --rip %LCITY%
  if not "!errorlevel!"=="0" (
    echo.
    echo   the rip failed - is DriverLevelTool.exe in %REPO%\DriverLevelTool\ ?
    pause
    goto menu
  )
  echo.
)
%PY% "%EDITOR%" --level %LCITY% --rebuild-map --dir "%ARENAS%" --render "%PREVIEW%"
set "RC=!errorlevel!"
echo.
if not "!RC!"=="0" echo   the build failed - exit code !RC!
if exist "%PREVIEW%" start "" "%PREVIEW%"
pause
goto menu

rem ---------------------------------------------------------------------------
:setup
if not defined PY goto nopy
call :sync
echo.
%PY% "%EDITOR%" --selftest
set "RC=!errorlevel!"
echo.
if not "!RC!"=="0" echo   SOMETHING IS MISSING - see above
pause
goto menu

rem ---------------------------------------------------------------------------
:nopy
echo.
echo   Python 3 was not found on PATH.
echo   Install it from python.org (tick "Add python.exe to PATH"), then re-run.
pause
goto menu

rem ---------------------------------------------------------------------------
rem copy arena files both ways, newest wins
:sync
if not exist "%GAME_ARENAS%" mkdir "%GAME_ARENAS%" >nul 2>nul
if exist "%ARENAS%\" xcopy "%ARENAS%\*.cca" "%GAME_ARENAS%\" /D /Y >nul 2>nul
if exist "%GAME_ARENAS%\" xcopy "%GAME_ARENAS%\*.cca" "%ARENAS%\" /D /Y >nul 2>nul
exit /b 0

rem ---------------------------------------------------------------------------
rem list the arena files and set PICKED (errorlevel 1 = back)
:pick
set "cnt=0"
for %%F in ("%ARENAS%\*.cca") do (
  if exist "%%~fF" (
    set /a cnt+=1
    set "f!cnt!=%%~fF"
  )
)
echo.
if !cnt!==0 (
  echo   No arena files yet - use "New arena" first.
  pause
  exit /b 1
)
for /l %%I in (1,1,!cnt!) do (
  for %%F in ("!f%%I!") do echo     %%I^)  %%~nxF
)
echo     B^)  back
set "sel="
set /p "sel=  number: "
if /i "!sel!"=="b" exit /b 1
set "PICKED="
call set "PICKED=%%f!sel!%%"
if not defined PICKED (
  echo   bad choice
  pause
  exit /b 1
)
exit /b 0
