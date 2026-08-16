set SDL2_DIR=%cd%\dependencies\SDL2-2.30.2
set OPENAL_DIR=%cd%\dependencies\openal-soft-1.23.1-bin
set JPEG_DIR=%cd%\dependencies\jpeg-9d
rem JERICHO auto-scans JERICHO/MODS and compiles every installed mod, so no
rem --with-mods list is needed here. Use --with-mods="" for a zero-mods build.
premake5.exe vs2019
pause
