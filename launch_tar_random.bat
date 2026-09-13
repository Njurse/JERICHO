@echo off
rem Take-a-Ride launcher with RANDOM city / car / weather / time.
rem
rem Car uses the VALIDATED `-car slot1..slot10` form, never a raw model index:
rem the frontend maps slots through carNumLookup and bounds-checks them, while a
rem raw index past a level's car pool is out of bounds and crashes the game
rem during load (after LUMP_CAR_MODELS, no dump).
setlocal

set /a CITY=%RANDOM% %% 4
set /a SLOT=%RANDOM% %% 10 + 1
set /a W=%RANDOM% %% 3
set /a T=%RANDOM% %% 4

if %CITY%==0 set CITYNAME=chicago
if %CITY%==1 set CITYNAME=havana
if %CITY%==2 set CITYNAME=vegas
if %CITY%==3 set CITYNAME=rio

if %W%==0 set WEATHER=none
if %W%==1 set WEATHER=rain
if %W%==2 set WEATHER=wet

if %T%==0 set TIME=dawn
if %T%==1 set TIME=day
if %T%==2 set TIME=dusk
if %T%==3 set TIME=night

echo Take-a-Ride: city=%CITYNAME% car=slot%SLOT% weather=%WEATHER% time=%TIME%
cd /d "C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
start "" "REDRIVER2_dev.exe" -level %CITYNAME% -car slot%SLOT% -weather %WEATHER% -time %TIME% -gamemode takeadrive
