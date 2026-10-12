@echo off
rem build_jericho.bat — build JERICHO (Release x64) with MSVC via premake-generated solution.
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild build\JERICHO.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
exit /b %errorlevel%
