@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild build\JERICHO.vcxproj /p:Configuration=Release_dev /p:Platform=x64 /m /v:m /nologo
exit /b %errorlevel%
