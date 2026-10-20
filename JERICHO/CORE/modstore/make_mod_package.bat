@echo off
rem ---------------------------------------------------------------------------
rem make_mod_package.bat — turn a mod folder into a publishable package.
rem
rem   make_mod_package.bat <mod-id|path> [--out <dir>] [--url <prefix>]
rem
rem Writes <out>\<id>-<version>.zip and prints the ready-to-paste mods.json entry,
rem with the sha256 and the size already computed. Nothing about publishing a mod
rem should involve retyping a hash: that is the mistake that silently refuses
rem every install on every player's machine, because the store is right to trust
rem the hash.
rem
rem The package is rooted at JERICHO\MODS\<id>\ the way a release lays its mods
rem out; the installer accepts that shape. The entry's "file" is RELATIVE (the
rem package name alone) because the store resolves it against the index URL, so a
rem repo can be one folder of zips next to its mods.json. --url emits an absolute
rem one instead.
rem
rem Exit: 0 ok, 1 error, 2 usage.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion
set "INFO=make_mod_package:"

set "ROOT=%~dp0..\..\.."
for %%i in ("%ROOT%") do set "ROOT=%%~fi"
set "MODS=%ROOT%\JERICHO\MODS"
set "OUT=%CD%"
set "URLPREFIX="
set "KEEP_BUILD=0"
set "TARGET="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--out" (
	if "%~2"=="" goto usage
	set "OUT=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--url" (
	if "%~2"=="" goto usage
	set "URLPREFIX=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--keep-build" (
	set "KEEP_BUILD=1"
	shift
	goto parse
)
if not "%TARGET%"=="" goto usage
set "TARGET=%~1"
shift
goto parse

:parsed
if "%TARGET%"=="" goto usage

set "SRC=%MODS%\%TARGET%"
if not "%TARGET%"=="" if exist "%TARGET%\mod.toml" set "SRC=%TARGET%"
if not exist "%SRC%\mod.toml" (
	echo %INFO% no mod.toml in "%SRC%" - that folder is not a mod>&2
	exit /b 1
)

rem --- read the manifest -----------------------------------------------------
set "VALUE="
call :toml "%SRC%\mod.toml" id
if not defined VALUE (
	echo %INFO% "%SRC%\mod.toml" has no id>&2
	exit /b 1
)
set "MID=!VALUE!"

set "VALUE="
call :toml "%SRC%\mod.toml" version
if not defined VALUE (
	echo %INFO% "%SRC%\mod.toml" has no version - a package with no version cannot be updated against>&2
	exit /b 1
)
set "MVER=!VALUE!"

set "VALUE="
call :toml "%SRC%\mod.toml" name
set "MNAME=!VALUE!"
if not defined MNAME set "MNAME=!MID!"

set "VALUE="
call :toml "%SRC%\mod.toml" author
set "MAUTHOR=!VALUE!"

set "VALUE="
call :toml "%SRC%\mod.toml" description
set "MDESC=!VALUE!"

set "VALUE="
call :toml "%SRC%\mod.toml" min_jericho
set "MMIN=!VALUE!"

set "VALUE="
call :toml "%SRC%\mod.toml" interface
set "MIFACE=!VALUE!"

for %%i in ("%SRC%") do set "BASENAME=%%~nxi"
if /i not "!BASENAME!"=="!MID!" (
	echo %INFO% the folder is "!BASENAME!" but its mod.toml says id = "!MID!" - they must match, because the store installs to MODS\%MID%>&2
	exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%" >nul 2>nul
for %%i in ("%OUT%") do set "OUT=%%~fi"

set "PKG=!MID!-!MVER!.zip"
set "ENTRY=%OUT%\!MID!-!MVER!.entry.json"
set "STAGE=%TEMP%\mmp-!MID!"

rem --- stage the release layout and zip it ------------------------------------
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\JERICHO\MODS" >nul 2>nul || (
	echo %INFO% cannot stage in "%STAGE%">&2
	exit /b 1
)
xcopy "%SRC%" "%STAGE%\JERICHO\MODS\!MID!\" /E /I /Q /Y >nul || (
	echo %INFO% cannot copy "%SRC%">&2
	rmdir /s /q "%STAGE%" >nul 2>nul
	exit /b 1
)

rem Build output is not part of a mod: obj\ and lib\ are what the compiler leaves
rem behind, and packing them took testmode from 9 KB to 268 KB with 86 files
rem nobody needs. --keep-build ships the folder exactly as it is.
if "%KEEP_BUILD%"=="1" goto staged_ok
if exist "%STAGE%\JERICHO\MODS\!MID!\obj" rmdir /s /q "%STAGE%\JERICHO\MODS\!MID!\obj"
if exist "%STAGE%\JERICHO\MODS\!MID!\lib" rmdir /s /q "%STAGE%\JERICHO\MODS\!MID!\lib"
del /q /s "%STAGE%\JERICHO\MODS\!MID!\*.pdb" >nul 2>nul
del /q /s "%STAGE%\JERICHO\MODS\!MID!\*.ilk" >nul 2>nul
del /q /s "%STAGE%\JERICHO\MODS\!MID!\*.exp" >nul 2>nul
:staged_ok

if exist "%OUT%\%PKG%" del /q "%OUT%\%PKG%"

call :zip "%STAGE%" "%OUT%\!PKG!"
if not errorlevel 1 goto zipped
echo %INFO% no zip tool worked - tried tar.exe and powershell Compress-Archive>&2
rmdir /s /q "%STAGE%" >nul 2>nul
exit /b 1

:zipped

rmdir /s /q "%STAGE%" >nul 2>nul

if not exist "%OUT%\%PKG%" (
	echo %INFO% the zip was not written>&2
	exit /b 1
)

rem --- hash and size ----------------------------------------------------------
set "SHA="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%OUT%\%PKG%" SHA256') do (
	if not defined SHA (
		set "LINE=%%h"
		set "LINE=!LINE: =!"
		set "SHA=!LINE!"
	)
)
if not defined SHA (
	echo %INFO% could not hash "%PKG%">&2
	exit /b 1
)

for %%f in ("%OUT%\%PKG%") do set "SIZE=%%~zf"

set "FILEURL=!PKG!"
if not "%URLPREFIX%"=="" (
	set "PFX=!URLPREFIX!"
	if "!PFX:~-1!"=="/" (
		set "FILEURL=!PFX!!PKG!"
	) else (
		set "FILEURL=!PFX!/!PKG!"
	)
)

rem --- the entry --------------------------------------------------------------
rem The quote character goes in through a variable: a literal " inside a `call`
rem argument toggles quoting, which silently cuts the line at the next space and
rem is how the first version of this wrote a broken JSON file.
set "Q=""

call :jsonesc "!MID!"
set "JID=!JESC!"
call :jsonesc "!MNAME!"
set "JNAME=!JESC!"
call :jsonesc "!MVER!"
set "JVER=!JESC!"
call :jsonesc "!FILEURL!"
set "JFILE=!JESC!"

> "%ENTRY%" echo {

set "L=  !Q!id!Q!: !Q!!JID!!Q!,"
>> "%ENTRY%" echo(!L!
set "L=  !Q!name!Q!: !Q!!JNAME!!Q!,"
>> "%ENTRY%" echo(!L!
set "L=  !Q!version!Q!: !Q!!JVER!!Q!,"
>> "%ENTRY%" echo(!L!

if not defined MAUTHOR goto entry_nodesc
call :jsonesc "!MAUTHOR!"
set "L=  !Q!author!Q!: !Q!!JESC!!Q!,"
>> "%ENTRY%" echo(!L!

:entry_nodesc
if not defined MDESC goto entry_nomin
call :jsonesc "!MDESC!"
set "L=  !Q!description!Q!: !Q!!JESC!!Q!,"
>> "%ENTRY%" echo(!L!

:entry_nomin
set "L=  !Q!file!Q!: !Q!!JFILE!!Q!,"
>> "%ENTRY%" echo(!L!
set "L=  !Q!sha256!Q!: !Q!!SHA!!Q!,"
>> "%ENTRY%" echo(!L!

if not defined MIFACE goto entry_mincore
set "L=  !Q!interface!Q!: !MIFACE!,"
>> "%ENTRY%" echo(!L!

:entry_mincore
if not defined MMIN goto entry_size
call :jsonesc "!MMIN!"
set "L=  !Q!min_jericho!Q!: !Q!!JESC!!Q!,"
>> "%ENTRY%" echo(!L!

:entry_size
set "L=  !Q!size!Q!: !SIZE!"
>> "%ENTRY%" echo(!L!
>> "%ENTRY%" echo }

echo %INFO% packaged !MNAME! !MVER! -^> "%OUT%\%PKG%" (!SIZE! bytes^)
echo %INFO% entry written to "%ENTRY%"
echo.
type "%ENTRY%"
echo.

exit /b 0

rem ==== helpers ==============================================================

:usage
echo usage: make_mod_package.bat ^<mod-id^|path^> [--out ^<dir^>] [--url ^<prefix^>]>&2
exit /b 2

rem %1 = file, %2 = key -> VALUE
:toml
set "VALUE="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /r /c:"^ *%~2 *=" "%~1" 2^>nul`) do (
	set "S=%%b"
	goto toml_have
)
goto :eof
:toml_have
if not defined S goto :eof
set "S=!S:"=!"
if not defined S goto :eof
:toml_ltrim
if not defined S goto :eof
if not "!S:~0,1!"==" " goto toml_ltrim_done
set "S=!S:~1!"
goto toml_ltrim
:toml_ltrim_done
set "VALUE=!S!"
goto :eof

rem %1 = text -> JESC (JSON-safe: backslash-escaped quotes, backslashes doubled)
:jsonesc
set "T=%~1"
set "T=!T:\=\\!"
set "T=!T:"=\"!"
set "JESC=!T!"
goto :eof

rem %1 = folder holding JERICHO, %2 = output zip
:zip
rem Each attempt stands on its own line: reading errorlevel inside a parenthesised
rem block, and jumping out of one, are both easy to get wrong in cmd - and an
rem unescaped ')' in a block's message ends the block and swallows the output.
if exist "%SystemRoot%\System32\tar.exe" goto zip_tar
goto zip_ps

:zip_tar
pushd "%~1"
"%SystemRoot%\System32\tar.exe" -a -cf "%~2" JERICHO >nul 2>nul
set "ZIPRC=%errorlevel%"
popd
if "%ZIPRC%"=="0" goto :eof

:zip_ps
if not exist "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" exit /b 1
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "Compress-Archive -Path '%~1\JERICHO' -DestinationPath '%~2' -Force" >nul 2>nul
if not errorlevel 1 goto :eof
exit /b 1
