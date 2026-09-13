@echo off
rem Single-player CHICAGO with the player spawned as the SEMI.
rem
rem The semi is car model 11, i.e. frontend slot 9 (-car slot9) - the slot the
rem game reserves for a content-override truck (FEmain.c, "remove truck").
rem
rem Its car data must be present: LEVELS\CHICAGO\CARMODEL_11_clean.dmodel
rem This install ships only CARMODEL_10_clean.dmodel (the SCHOOL BUS), and
rem forcing a model with no data CRASHES the game during load (right after
rem LUMP_CAR_MODELS). So this script checks first and, if the data is missing,
rem launches with the level's default car instead of crashing - add the semi's
rem file and the same script will spawn you as the semi.
setlocal
cd /d "C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"

set "SEMI=DRIVER2\LEVELS\CHICAGO\CARMODEL_11_clean.dmodel"
set "CARARG=-car slot9"

if not exist "%SEMI%" (
	echo WARNING: no semi data at "%SEMI%".
	echo          Launching as the school bus instead - add the semi's
	echo          CARMODEL_11_clean.dmodel to spawn as the semi.
	set "CARARG=-car slot8"
)

echo Chicago single-player: %CARARG%
start "" "REDRIVER2_dev.exe" -nointro -level chicago %CARARG% -gamemode takeadrive
