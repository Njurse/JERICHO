@echo off
rem ============================================================
rem  JERICHO - build_mods.bat
rem
rem  Compiles EVERY installed module (JERICHO/MODS/<id>) into a
rem  DLL using the game's own toolchain. The game exe is never
rem  rebuilt - modules are loaded at runtime.
rem
rem  Run by the in-game "Compile Mods" button (Options -> JERICHO)
rem  or by hand from the repo's JERICHO/ folder.
rem
rem  DLLs are built into the repo's JERICHO/MODS/<id>/ and copied
rem  into this folder's MODS/ (the runtime location next to the exe).
rem ============================================================
setlocal EnableExtensions

rem ---- locate the game dev tree (src_rebuild with the mods premake) ----
rem Case 1: this script lives in the runtime JERICHO next to the exe
rem         (src_rebuild\bin\<cfg>\JERICHO) -> premake is two levels up.
rem Case 2: this script lives in the repo (REDRIVER2\JERICHO) -> premake
rem         is in ..\src_rebuild. (Probe premake5_mods.lua, not premake5.exe:
rem         a stray premake5.exe elsewhere must not win.)
if exist "%~dp0..\..\premake5_mods.lua" (
    set "SRC_REBUILD=%~dp0..\.."
    set "REPO_JERICHO=%~dp0..\..\JERICHO"
) else if exist "%~dp0..\src_rebuild\premake5_mods.lua" (
    set "SRC_REBUILD=%~dp0..\src_rebuild"
    set "REPO_JERICHO=%~dp0"
) else (
    echo JERICHO: cannot find the mods premake - run this from the REDRIVER2
    echo          development tree ^(src_rebuild^).
    exit /b 4
)

set "RUNTIME_MODS=%~dp0MODS"

rem ---- locate MSBuild via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%v"
)
if not defined MSBUILD (
    where msbuild >nul 2>nul && set "MSBUILD=msbuild"
)
if not defined MSBUILD (
    echo JERICHO: MSBuild was not found - install "Desktop development with C++".
    exit /b 3
)

rem ---- generate the mods-only solution and build every module DLL ----
pushd "%SRC_REBUILD%"

premake5.exe --file=premake5_mods.lua vs2019
if errorlevel 1 (
    echo JERICHO: premake failed
    popd
    exit /b 1
)

"%MSBUILD%" build_mods\REDRIVER2_MODS.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
set "BUILD_ERR=%errorlevel%"

popd

if not "%BUILD_ERR%"=="0" (
    echo JERICHO: module build failed ^(see errors above^)
    exit /b 1
)

rem ---- copy fresh DLLs into the runtime MODS folder(s) ----
rem 1) this script's own MODS (runtime layout: script sits next to the exe)
if /I not "%RUNTIME_MODS%"=="%REPO_JERICHO%\MODS" (
    call :copy_mods "%RUNTIME_MODS%"
)
rem 2) the mirrored JERICHO next to the game exe in the dev tree
if exist "%SRC_REBUILD%\bin\Release\JERICHO\MODS" (
    if /I not "%SRC_REBUILD%\bin\Release\JERICHO\MODS"=="%REPO_JERICHO%\MODS" (
        call :copy_mods "%SRC_REBUILD%\bin\Release\JERICHO\MODS"
    )
)

echo JERICHO: modules compiled - reload the Mods screen to activate them.
exit /b 0

:copy_mods
for /d %%m in ("%~1\*") do (
    if exist "%REPO_JERICHO%\MODS\%%~nxm\%%~nxm.dll" (
        copy /y "%REPO_JERICHO%\MODS\%%~nxm\%%~nxm.dll" "%~1\%%~nxm\" >nul
    )
)
exit /b 0
