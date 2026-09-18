@echo off
rem MULTIPLAYER-map CHICAGO (the small arena, not the full city) with the player
rem spawned as the SEMI.
rem
rem The semi is car model 11, i.e. frontend slot 9 (-car slot9) - the slot the
rem game reserves for a content-override truck (FEmain.c, "remove truck").
rem
rem Its car data must be present: LEVELS\CHICAGO\CARMODEL_11_clean.dmodel
rem This install ships only CARMODEL_10_clean.dmodel (the SCHOOL BUS), and
rem forcing a model with no data CRASHES the game during load. So this script
rem checks first and, if the data is missing, launches with the level's default
rem car instead of crashing.
rem
rem -mp 1 selects chicago's arena 1; use -mp 0 for the other arena.
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

echo Chicago multiplayer arena 1: %CARARG%
start "" "REDRIVER2_dev.exe" -nointro -mp 1 -level chicago %CARARG% -gamemode takeadrive
