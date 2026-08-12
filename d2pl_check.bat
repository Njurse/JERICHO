@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\Users\Jaret\Documents\Projects\REDRIVER2
cl /nologo /EHsc /TP /c /Zs /W3 ^
  /I src_rebuild\Game /I src_rebuild\Game\C /I src_rebuild\Game\C\JERICHO\include ^
  /I src_rebuild\dependencies\SDL2-2.30.2\include /I src_rebuild\dependencies\openal-soft-1.23.1-bin\include ^
  /I src_rebuild\dependencies\jpeg-9d /I mods\d2pl ^
  /I src_rebuild\PsyCross\include /I src_rebuild\PsyCross\include\psx ^
  mods\d2pl\d2pl.c mods\d2pl\d2pl_car.c mods\d2pl\d2pl_ped.c mods\d2pl\d2pl_weapon.c
