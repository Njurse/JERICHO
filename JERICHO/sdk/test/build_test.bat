@echo off
rem Fake-host harness: build test_addon.dll and fake_host.exe, then run it.
setlocal
cd /d "%~dp0"

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
	for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
)
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
	echo fake-host: no Visual Studio vcvars64.bat found
	exit /b 2
)

call "%VCVARS%" >nul || exit /b 2

cl /nologo /LD /TP /DJERICHO_MODULE_BUILD /I..\include test_addon.c /Fe:test_addon.dll
if errorlevel 1 exit /b 1

rem The addon must resolve the table at runtime: no JERICHO import is allowed.
dumpbin /imports test_addon.dll | findstr /i "JERICHO" >nul
if not errorlevel 1 (
	echo FAIL: test_addon.dll imports a JERICHO symbol
	exit /b 1
)

cl /nologo /TP /I..\include fake_host.c /Fe:fake_host.exe
if errorlevel 1 exit /b 1

fake_host.exe test_addon.dll
exit /b %errorlevel%
