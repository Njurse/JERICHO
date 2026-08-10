@echo off
rem Headless Driver 1 validation harness.
rem Builds d1test.exe (no window, no SDL, no controller) and runs it.
rem Run from src_rebuild/bin/Release_dev so the DRIVER\ data folder resolves:
rem     ..\..\tests\d1\d1test.bat

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cd /d %~dp0

cl /nologo /c /TP /I..\..\Game /I..\..\Game\C /I..\..\Game\C\JERICHO\include /I..\..\PsyCross\include /I..\..\PsyCross\include\psx /I..\..\dependencies\jpeg-9d /I..\..\dependencies\openal-soft-1.23.1-bin\include /I..\..\dependencies\SDL2-2.30.2\include /DNDEBUG /DNTSC_VERSION /D_CRT_SECURE_NO_WARNINGS /W3 d1test.c /Fo:d1test.obj
if errorlevel 1 goto :fail

cl /nologo /c /TP /D_CRT_SECURE_NO_WARNINGS /W3 d1stubs.c /Fo:d1stubs.obj
if errorlevel 1 goto :fail

cl /nologo /Fe:d1test.exe d1test.obj d1stubs.obj
if errorlevel 1 goto :fail

rem run from the game data folder so DRIVER\ resolves
cd /d ..\..\bin\Release_dev
..\..\tests\d1\d1test.exe
goto :eof

:fail
echo D1TEST BUILD FAILED
exit /b 1
