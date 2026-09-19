@echo off
rem CC select launcher: boot straight into the CC select flow - pick an arena
rem (the four cities), then a vehicle (every registered profile), then the match
rem starts with that combination.
rem
rem It launches with -ccmenu and NO -level, so the game comes up in the frontend
rem and the menu raises on the first frame (select/select.c). The module is
rem turned ON in the bin copy of JERICHO\CONFIG\modlist.ini first - the repo's
rem modlist pins gameplay modules OFF and the bin copy is what the game reads
rem (see tools\_enable_module.bat).
rem
rem Pass "dry" to print what it would run without launching or changing anything.
setlocal

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "RUN=-nointro -ccmenu"

if /I "%~1"=="dry" (
	echo would run: REDRIVER2_dev.exe %RUN%
	exit /b 0
)

call "%~dp0_enable_module.bat" cainescrossfire "%EXEDIR%"

cd /d "%EXEDIR%" || (echo no %EXEDIR% & exit /b 1)

echo CC select: REDRIVER2_dev.exe %RUN%
start "" "REDRIVER2_dev.exe" %RUN%
