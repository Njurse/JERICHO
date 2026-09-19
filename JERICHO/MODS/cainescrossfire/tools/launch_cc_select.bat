@echo off
rem CC select launcher: boot straight into the CC select flow - pick an arena
rem (the four cities), then one of that arena's vehicles, then the match starts.
rem
rem It launches with -ccmenu and NO -level, so the game comes up in the frontend
rem and the menu opens (select/select.c). The module is turned ON in the bin copy
rem of JERICHO\CONFIG\modlist.ini first (the repo's modlist pins gameplay modules
rem OFF and the bin copy is what the game reads - see tools\_enable_module.bat).
rem
rem It ALSO neutralises JERICHO\CONFIG\carhacks.ini. That file is shared runtime
rem state: the other launchers write a cross-city "import = slot:city:model" list
rem into it, and a stale list forces resident slots to another city's models -
rem which fights the CC select (the arena's own cars must load from the arena).
rem So the flow runs with carhacks' cross-city import off.
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

rem a clean carhacks.ini: no cross-city import, so the arena's own cars load
> "JERICHO\CONFIG\carhacks.ini" (
	echo # written by launch_cc_select.bat - carhacks neutralised for the CC select
	echo cross_city_vehicles = 0
	echo source_city = -1
	echo traffic_model = -1
	echo traffic_slot = 2
	echo car_list = 8,9,10
)

echo CC select: REDRIVER2_dev.exe %RUN%
start "" "REDRIVER2_dev.exe" %RUN%
