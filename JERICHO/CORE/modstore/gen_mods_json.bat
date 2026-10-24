@echo off
rem ---------------------------------------------------------------------------
rem gen_mods_json.bat — build a complete, publishable mod repo from MODS\ in one go.
rem
rem   gen_mods_json.bat [options] [mod-id ...]
rem
rem For every mod folder (or just the ids given) it packages the mod, then writes
rem one mods.json listing them all. The versions come from each mod's own mod.toml,
rem so the index cannot drift from the manifests, and the hashes come from the
rem packages that were just built, so they cannot drift from the files either --
rem which is the point: a hash that drifts turns into "the store refuses to install
rem anything" on every player's machine, and it stays silent until then.
rem
rem Options:
rem   --mods <dir>       the folder holding the mods (default JERICHO\MODS, i.e. the
rem                      game's own; a standalone repo points it at its MODS\)
rem   --out <dir>        where the packages and mods.json go (default JERICHO\CORE\modstore\out)
rem   --mods <dir>       the folder holding the mods           (default <game>\JERICHO\MODS)
rem                      Point this at a repo's own MODS\ to publish a repo that
rem                      carries its sources, with no game needed.
rem   --repo-id <id>     the repo's id                        (default "local")
rem   --repo-name <name> the repo's display name              (default "Local mod repo")
rem   --homepage <url>   optional homepage for the index
rem   --url <prefix>     absolute package URL prefix (release hosting) instead of
rem                      the default relative file names
rem   --list             print the mods that would be included, then stop
rem
rem Exit: 0 ok, 1 error, 2 usage.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion
set "INFO=gen_mods_json:"

set "SELF=%~dp0"
if "%SELF:~-1%"=="\" set "SELF=%SELF:~0,-1%"
set "ROOT=%SELF%\..\..\.."
for %%i in ("%ROOT%") do set "ROOT=%%~fi"
set "MODS=%ROOT%\JERICHO\MODS"

set "OUT=%SELF%\out"
set "REPO_ID=local"
set "REPO_NAME=Local mod repo"
set "HOMEPAGE="
set "URLPREFIX="
set "LISTONLY=0"
set "IDS="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--mods" (
	if "%~2"=="" goto usage
	set "MODS=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--out" (
	if "%~2"=="" goto usage
	set "OUT=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--mods" (
	if "%~2"=="" goto usage
	set "MODS=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--repo-id" (
	if "%~2"=="" goto usage
	set "REPO_ID=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--repo-name" (
	if "%~2"=="" goto usage
	set "REPO_NAME=%~2"
	shift
	shift
	goto parse
)
if /i "%~1"=="--homepage" (
	if "%~2"=="" goto usage
	set "HOMEPAGE=%~2"
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
if /i "%~1"=="--list" (
	set "LISTONLY=1"
	shift
	goto parse
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
if "%~1"=="" goto parsed
set "IDS=%IDS% %~1"
shift
goto parse

:parsed

if not exist "%OUT%" mkdir "%OUT%" >nul 2>nul
for %%i in ("%OUT%") do set "OUT=%%~fi"

if not exist "%MODS%" (
	echo %INFO% no mods folder at "%MODS%">&2
	exit /b 1
)

rem --- which mods -------------------------------------------------------------
rem A mod is a folder with a mod.toml. With no ids given, every one of them is
rem included; `for /d` walks them in name order, so two runs agree.
set "COUNT=0"
set "IDLIST="
if not "%IDS%"=="" goto by_id

for /d %%d in ("%MODS%\*") do (
	if exist "%%d\mod.toml" set "IDLIST=!IDLIST! %%~nxd"
)
goto have_ids

:by_id
for %%i in (%IDS%) do (
	if not exist "%MODS%\%%i\mod.toml" (
		echo %INFO% no such mod: %%i>&2
		exit /b 1
	)
	set "IDLIST=!IDLIST! %%i"
)

:have_ids
if "%IDLIST%"=="" (
	echo %INFO% no mods with a mod.toml under "%MODS%">&2
	exit /b 1
)

if "%LISTONLY%"=="1" (
	for %%i in (%IDLIST%) do echo %%i
	exit /b 0
)

rem --- the published-versions ledger ------------------------------------------
rem What was published last time, per mod: "<id> <version> <content-hash> <zip
rem size> <zip sha256>". It is what makes the two publishing mistakes fail HERE
rem instead of on a player: a version that goes backwards, and changed content
rem published under an unchanged version (the store compares versions, so nobody
rem who already has it would ever see the change).
rem
rem The hash is over the mod's FILES, not the zip: a rebuild of unchanged sources
rem produces a new zip (archive timestamps) and refusing that would be useless.
set "LEDGER=%OUT%\versions.txt"
set "NEWLEDGER=%OUT%\versions.new"
if exist "%NEWLEDGER%" del /q "%NEWLEDGER%"

rem --- package each one -------------------------------------------------------
set "ENTRIES="

for %%i in (%IDLIST%) do (
	call :pack %%i
	if errorlevel 1 exit /b 1
	set "ID=%%i"
	set "ENTRY="
	for /f "delims=" %%e in ('dir /b /o-d "%OUT%\%%i-*.entry.json" 2^>nul') do (
		if not defined ENTRY set "ENTRY=%OUT%\%%e"
	)
	if not defined ENTRY (
		echo %INFO% %%i produced no entry file>&2
		exit /b 1
	)

	rem the version this package declares, straight from the entry the packer wrote
	set "MVER="
	for /f "tokens=2 delims=:," %%v in ('findstr /c:"\"version\":" "!ENTRY!"') do (
		if not defined MVER (
			set "V=%%v"
			set "V=!V: =!"
			set "V=!V:"=!"
			set "MVER=!V!"
		)
	)
	if not defined MVER (
		echo %INFO% %%i has no version in its entry>&2
		exit /b 1
	)

	call :content_hash "%MODS%\%%i"
	set "MHASH=!CHASH!"

	set "PREV="
	if exist "%LEDGER%" for /f "tokens=1,2,3" %%a in ('type "%LEDGER%"') do (
		if /i "%%a"=="%%i" if not defined PREV set "PREV=%%a/%%b/%%c"
	)

	rem A label cannot live inside a parenthesised block: cmd finds it, but the
	rem loop variable stops expanding there (the message came out as "%i"), so the
	rem checks below are nested ifs rather than gotos.
	if not defined PREV goto ledger_write_unused_guard

	if defined PREV (
		for /f "tokens=1,2,3 delims=/" %%a in ("!PREV!") do (
			set "PVER=%%b"
			set "PHASH=%%c"
		)

		call :ver_gt "!PVER!" "!MVER!"
		if "!VERGT!"=="1" (
			echo %INFO% %%i: !MVER! is OLDER than the published !PVER!>&2
			echo %INFO%   the store will never offer it, and anyone who already has !PVER! keeps it.>&2
			echo %INFO%   bump the version in !MODS!\%%i\mod.toml, or delete !LEDGER!>&2
			echo %INFO%   if you really mean to withdraw the newer release from this repo.>&2
			exit /b 1
		)

		if "!MVER!"=="!PVER!" if defined PHASH if /i not "!MHASH!"=="!PHASH!" (
			echo %INFO% %%i: the mod changed but the version did not ^(!MVER!^)>&2
			echo %INFO%   players on !PVER! would never see the change: the store compares versions.>&2
			echo %INFO%   bump the version in !MODS!\%%i\mod.toml.>&2
			exit /b 1
		)
	)

	:ledger_write_unused_guard
	for %%z in ("%OUT%\%%i-!MVER!.zip") do set "MSIZE=%%~zz"
	call :sha_of "%OUT%\%%i-!MVER!.zip"
	set "ZSHA=!SHAHEX!"
	>> "%NEWLEDGER%" echo(!ID! !MVER! !MHASH! !MSIZE! !ZSHA!

	set "ENTRIES=!ENTRIES! !ENTRY!"
	set /a COUNT+=1
)

if exist "%NEWLEDGER%" move /y "%NEWLEDGER%" "%LEDGER%" >nul

rem --- assemble ---------------------------------------------------------------
set "INDEX=%OUT%\mods.json"
if exist "%INDEX%" del /q "%INDEX%"

> "%INDEX%" echo {
>> "%INDEX%" echo   "schema": 1,
>> "%INDEX%" echo   "id": "!REPO_ID!",
>> "%INDEX%" echo   "name": "!REPO_NAME!",
if defined HOMEPAGE >> "%INDEX%" echo   "homepage": "!HOMEPAGE!",
>> "%INDEX%" echo   "mods": [

set "FIRST=1"
for %%e in (%ENTRIES%) do (
	if "!FIRST!"=="1" (
		set "FIRST=0"
	) else (
		>> "%INDEX%" echo ,
	)
	type "%%e" >> "%INDEX%"
)

rem `echo.` writes a blank line; a bare `echo` would write "ECHO is off." into the
rem index and break it
>> "%INDEX%" echo.
>> "%INDEX%" echo   ]
>> "%INDEX%" echo }

echo %INFO% !COUNT! mod(s) -^> "%INDEX%"
echo %INFO% packages in "%OUT%"
echo.
echo %INFO% serve it locally with:
echo          cd /d "%OUT%" ^&^& python -m http.server 8099
echo        then set this in JERICHO\CONFIG\modstore.ini:
echo          repo = http://127.0.0.1:8099/mods.json
echo.

exit /b 0

rem ==== helpers ==============================================================

:usage
echo usage: gen_mods_json.bat [--out ^<dir^>] [--repo-id ^<id^>] [--repo-name ^<name^>] [--homepage ^<url^>] [--url ^<prefix^>] [--list] [mod-id ...]>&2
exit /b 2

rem %1 = mod id, packaged into %OUT%
:pack
if not defined URLPREFIX goto pack_plain
call "%SELF%\make_mod_package.bat" "%MODS%\%~1" --out "%OUT%" --url "!URLPREFIX!" >nul
if errorlevel 1 (
	echo %INFO% could not package %~1>&2
	exit /b 1
)
goto :eof

:pack_plain
call "%SELF%\make_mod_package.bat" "%MODS%\%~1" --out "%OUT%" >nul
if errorlevel 1 (
	echo %INFO% could not package %~1>&2
	exit /b 1
)
goto :eof

rem %1 = file -> SHAHEX (certutil; always present on Windows)
:sha_of
set "SHAHEX="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%~1" SHA256 2^>nul') do (
	if not defined SHAHEX (
		set "H=%%h"
		set "H=!H: =!"
		set "SHAHEX=!H!"
	)
)
goto :eof

rem %1 = mod folder -> CHASH: one hash over its file names and their hashes
:content_hash
set "CHASH="
set "MAN=%TEMP%\modstore-content.txt"
if exist "%MAN%" del /q "%MAN%"
set "BASE=%~1"
for /f "delims=" %%f in ('dir /b /s /a-d /o:n "%~1" 2^>nul') do (
	set "REL=%%f"
	set "REL=!REL:%BASE%\=!"
	call :sha_of "%%f"
	>> "!MAN!" echo(!REL! !SHAHEX!
)
if not exist "%MAN%" goto :eof
call :sha_of "%MAN%"
set "CHASH=!SHAHEX!"
del /q "%MAN%" >nul 2>nul
goto :eof

rem %1, %2 = versions -> VERGT=1 when %1 is newer than %2
:ver_gt
set "VERGT=0"
for /f "tokens=1,2,3 delims=." %%a in ("%~1") do set "A1=%%a" & set "A2=%%b" & set "A3=%%c"
for /f "tokens=1,2,3 delims=." %%a in ("%~2") do set "B1=%%a" & set "B2=%%b" & set "B3=%%c"
set "A1=!A1!0" & set "A2=!A2!0" & set "A3=!A3!0"
set "B1=!B1!0" & set "B2=!B2!0" & set "B3=!B3!0"
set /a "A1=!A1:~0,-1!" 2>nul
set /a "A2=!A2:~0,-1!" 2>nul
set /a "A3=!A3:~0,-1!" 2>nul
set /a "B1=!B1:~0,-1!" 2>nul
set /a "B2=!B2:~0,-1!" 2>nul
set /a "B3=!B3:~0,-1!" 2>nul
if !A1! GTR !B1! set "VERGT=1" & goto :eof
if !A1! LSS !B1! goto :eof
if !A2! GTR !B2! set "VERGT=1" & goto :eof
if !A2! LSS !B2! goto :eof
if !A3! GTR !B3! set "VERGT=1"
goto :eof
