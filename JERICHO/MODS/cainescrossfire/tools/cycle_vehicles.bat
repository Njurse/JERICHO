@echo off
rem ============================================================================
rem cycle_vehicles.bat <city> [frames]
rem
rem Boots <city> once per car model (0 1 2 3 4 8 9 10 12, and 11 outside Chicago)
rem so each vehicle can be watched and identified. Each run SELF-TERMINATES
rem (-frames), so nothing is killed - and cross-city is turned OFF, so you see the
rem city's OWN cars with nothing imported in the way.
rem
rem The model number is printed before each boot. Watch the car that spawns and
rem write down what it is (e.g. "havana 4 = police"). That is the whole point.
rem
rem   cycle_vehicles.bat havana         180 frames each (~6s)
rem   cycle_vehicles.bat rio 300        longer per car
rem
rem Model 11 crashes in a Chicago level (no data ships), so it is skipped there.
rem All ten others exist in all four cities (see carhacks\VEHICLES.md).
rem ============================================================================
setlocal

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "CFG=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"

set "CITY=%~1"
if "%CITY%"=="" set "CITY=havana"
set "FRAMES=%~2"
if not defined FRAMES set "FRAMES=180"

rem cross-city off: the city's own models, no import in the way.
echo cross_city_vehicles = 0 > "%CFG%"

cd /d "%EXEDIR%"

for %%M in (0 1 2 3 4 8 9 10 12) do call :one %%M
if /i not "%CITY%"=="chicago" call :one 11
goto :done

:one
echo.
echo === %CITY%  model %1   launch: -level %CITY% -car %1 ===
".\REDRIVER2_dev.exe" -nointro -level %CITY% -car %1 -weather none -time day -frames %FRAMES%
exit /b 0

:done
echo.
echo done. models shown: 0 1 2 3 4 8 9 10 12 (plus 11 outside Chicago)
endlocal
