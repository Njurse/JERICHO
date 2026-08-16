@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\Jaret\Documents\Projects\REDRIVER2
cl /nologo /EHsc /TP ^
  /I src_rebuild\Game\C\JERICHO\include /I src_rebuild\Game\C /I src_rebuild\Game ^
  /I src_rebuild\dependencies\SDL2-2.30.2\include ^
  /I src_rebuild\dependencies\openal-soft-1.23.1-bin\include ^
  /I src_rebuild\dependencies\jpeg-9d ^
  /I src_rebuild\PsyCross\include /I src_rebuild\PsyCross\include\psx ^
  src_rebuild\Game\C\JERICHO\test\jer_test.c ^
  src_rebuild\Game\C\JERICHO\src\jer_system.c ^
  src_rebuild\Game\C\JERICHO\src\jer_manager.c ^
  src_rebuild\Game\C\JERICHO\src\jer_config.c ^
  src_rebuild\Game\C\JERICHO\test\test_registry.c ^
  JERICHO\MODS\example\example.c ^
  /Fe:jer_test.exe
