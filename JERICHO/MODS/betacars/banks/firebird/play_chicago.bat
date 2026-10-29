@echo off
rem ============================================================================
rem  play_chicago.bat [test [frames]]
rem
rem  Take a ride in CHICAGO with the Firebird as the player's car.
rem
rem  The car is chosen on the command line with -car <model>: the engine assigns
rem  the player's model from the mission header before resident models exist, so
rem  anything set from a hook is replaced by the level's own car. -car 1 names
rem  the model the Firebird bank provides (bank slot 1), and carhacks.ini's
rem  import = 5:25:1 fills spare resident slot 5 with city 25 = FIREBIRD.
rem
rem  test [frames]  run headless instead of interactive: -frames N plus a shot,
rem                 so the run exits by itself (frames defaults to 250).
rem ============================================================================
setlocal

rem the bank lives at <repo>\JERICHO\MODS\betacars\banks\firebird
set "BANK=%~dp0"
set "BANK=%BANK:~0,-1%"
for %%I in ("%BANK%\..\..\..\..\..") do set "ROOT=%%~fI"
set "EXEDIR=%ROOT%\src_rebuild\bin\Release_dev"
set "CFG=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"
set "BANKLEV=%EXEDIR%\JERICHO\MODS\betacars\banks\firebird\FIREBIRD.LEV"

if not exist "%EXEDIR%\JERICHO_dev.exe" goto no_exe
if not exist "%BANKLEV%" goto no_bank

echo Firebird bank : %BANKLEV%
findstr /c:"import = 5:25:1" "%CFG%" >nul 2>&1
if errorlevel 1 goto wrong_import
echo import        : 5:25:1 (spare slot 5, city 25 = FIREBIRD, model 1)

if /i "%~1"=="test" goto test_run
if not "%~1"=="" goto usage

echo mode          : take a ride in CHICAGO, playing now
cd /d "%EXEDIR%"
start "" "JERICHO_dev.exe" -nointro -level CHICAGO -car 1 -gamemode takeadrive -weather none -time day
exit /b 0

:test_run
set "FRAMES=%~2"
if "%FRAMES%"=="" set "FRAMES=250"
set /a SHOTAT=%FRAMES%-20
echo mode          : headless test, %FRAMES% frames, screenshot at %SHOTAT%
cd /d "%EXEDIR%"
"JERICHO_dev.exe" -nointro -level CHICAGO -car 1 -gamemode takeadrive -weather none -time day -frames %FRAMES% -shot %SHOTAT%
exit /b 0

:no_exe
echo FAILED: no game at %EXEDIR%\JERICHO_dev.exe
exit /b 1

:no_bank
echo FAILED: the game's mirror has no Firebird bank.
echo Run rebake.bat first - it bakes and copies into the mirror.
exit /b 1

:wrong_import
echo FAILED: %CFG%
echo has no "import = 5:25:1" line, so nothing would import the Firebird.
echo Expected: import = 5:25:1   (spare slot 5, city 25 = FIREBIRD, model 1)
exit /b 1

:usage
echo usage: play_chicago.bat [test [frames]]
echo   no arguments   take a ride in CHICAGO in the Firebird
echo   test [frames]  headless run that exits by itself
exit /b 2
