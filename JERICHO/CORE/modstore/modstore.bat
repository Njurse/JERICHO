@echo off
rem ============================================================================
rem  modstore.bat -- fetch, verify and install ONE mod package (Windows twin of
rem                  modstore.sh; same commands, same refusals, same records).
rem
rem  The frontend menu (jer_modstore.c) decides WHAT to do and never touches the
rem  disk itself. Everything that can destroy work happens here, once, under the
rem  same rules the updater already follows: verify first, overwrite only, and
rem  never delete something this store did not install.
rem
rem  COMMANDS
rem    version                           print "modstore <n>" (handshake with the exe)
rem    index <url> <out-file>            fetch a repo's mods.json
rem    install <package-url> <sha256> <mod-id>
rem    remove <mod-id> [--wait <pid>]    delete MODS\<id> ONLY if the store put it
rem                                      there; --wait defers until that process exits
rem    rebake                            run BAKE_D1_CARS.bat beside the game
rem
rem  Every run writes JERICHO\CONFIG\modstore\last.json (op/id/ok/msg/script),
rem  which is what the exe polls, and logs to ...\modstore.log.
rem  NEEDS: curl.exe (Windows 10+) or PowerShell; tar.exe (Windows 10+) or
rem         PowerShell to unpack; certutil to verify.
rem ============================================================================
setlocal EnableDelayedExpansion

set "MODSTORE_SCRIPT_VERSION=1"
set "RC=0"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
if not exist "%ROOT%\JERICHO" (
	echo modstore: "%ROOT%\JERICHO" is missing -- this script must sit beside the game
	exit /b 1
)

set "STATE=%ROOT%\JERICHO\CONFIG\modstore"
set "RECORDS=%STATE%\installed"
set "LOG=%STATE%\modstore.log"
set "LAST=%STATE%\last.json"
set "MODS=%ROOT%\JERICHO\MODS"
set "BAKE=%ROOT%\BAKE_D1_CARS.bat"

if not exist "%STATE%" mkdir "%STATE%" >nul 2>nul
if not exist "%RECORDS%" mkdir "%RECORDS%" >nul 2>nul
if not exist "%MODS%" mkdir "%MODS%" >nul 2>nul

set "TMPD=%TEMP%\modstore-%RANDOM%%RANDOM%"
set "OP=none"
set "OPID="

rem ---- arguments: the command, then up to three operands --------------------
set "CMD=%~1"
if "%CMD%"=="" goto usage
if /i "%CMD%"=="help" goto usage
if /i "%CMD%"=="--help" goto usage
if /i "%CMD%"=="-h" goto usage
shift

set "A1="
set "A2="
set "A3="
:collect
if "%~1"=="" goto dispatch
if not defined A1 goto _a1
if not defined A2 goto _a2
if not defined A3 goto _a3
shift
goto collect
:_a1
set "A1=%~1"
shift
goto collect
:_a2
set "A2=%~1"
shift
goto collect
:_a3
set "A3=%~1"
shift
goto collect

:dispatch
if /i "%CMD%"=="version" goto cmd_version
if /i "%CMD%"=="index" goto cmd_index
if /i "%CMD%"=="install" goto cmd_install
if /i "%CMD%"=="remove" goto cmd_remove
if /i "%CMD%"=="rebake" goto cmd_rebake
echo modstore: unknown command "%CMD%" (try: version, index, install, remove, rebake)
set "RC=2"
goto end

rem ---------------------------------------------------------------- version
:cmd_version
echo modstore %MODSTORE_SCRIPT_VERSION%
goto end

rem ------------------------------------------------------------------ index
:cmd_index
set "OP=index"
if not defined A2 goto usage_index
set "URL=%A1%"
set "OUT=%A2%"
set "IDX=%TEMP%\modstore-index-%RANDOM%.json"
call :log "modstore: fetching !URL!"
call :fetch "!URL!" "!IDX!"
if errorlevel 1 (
	call :failmsg "could not fetch the repo index (!URL!)"
	goto end_fail
)
findstr /i /c:mods "!IDX!" >nul 2>nul
if errorlevel 1 (
	call :failmsg "the repo's index is not a mods.json (no mods list in it)"
	goto end_fail
)
copy /Y "!IDX!" "!OUT!" >nul 2>nul
if errorlevel 1 (
	call :failmsg "could not write !OUT!"
	goto end_fail
)
del /Q "!IDX!" >nul 2>nul
call :finish 1 "index fetched"
goto end

rem ---------------------------------------------------------------- install
:cmd_install
set "OP=install"
if not defined A3 goto usage_install
set "URL=%A1%"
set "WANT=%A2%"
set "OPID=%A3%"

call :valid_id "!OPID!"
if errorlevel 1 (
	call :failmsg "!OPID! is not a valid mod id (letters, digits and underscore, not starting with a digit)"
	goto end_fail
)
if "!WANT!"=="" (
	call :failmsg "no checksum was given, and an unverified package is not installed"
	goto end_fail
)

set "ASSET=!URL!"
for %%p in ("!URL!") do set "ASSET=%%~nxp"
for /f "tokens=1 delims=?" %%q in ("!ASSET!") do set "ASSET=%%q"
if "!ASSET!"=="" set "ASSET=package.zip"

if exist "!TMPD!" rmdir /s /q "!TMPD!" >nul 2>nul
mkdir "!TMPD!" >nul 2>nul

call :log "modstore: downloading !ASSET!"
call :fetch "!URL!" "!TMPD!\!ASSET!"
if errorlevel 1 (
	call :failmsg "could not download !ASSET!"
	goto end_fail
)

call :sha256 "!TMPD!\!ASSET!"
if errorlevel 1 (
	call :failmsg "certutil could not hash the download, so it cannot be verified"
	goto end_fail
)
if /i not "!GOT!"=="!WANT!" (
	call :failmsg "checksum mismatch - refusing to install (published !WANT!, downloaded !GOT!)"
	goto end_fail
)
call :log "modstore: checksum ok (!GOT!)"

mkdir "!TMPD!\x" >nul 2>nul
call :extract "!TMPD!\!ASSET!" "!TMPD!\x"
if errorlevel 1 (
	call :failmsg "could not unpack !ASSET! (no tar.exe, PowerShell or python that reads it)"
	goto end_fail
)

set "SRC="
call :find_mod_root "!TMPD!\x"
if not defined SRC (
	call :failmsg "the package carries no mod.toml, so it is not a mod"
	goto end_fail
)
rem The package's own id must be the one that was asked for: a repo entry naming
rem the wrong file must not land in another mod's folder.
set "VALUE="
call :toml "!SRC!\mod.toml" id
if defined VALUE if /i not "!VALUE!"=="!OPID!" (
	call :failmsg "the package is !VALUE!, not !OPID! - refusing to install it as a different mod"
	goto end_fail
)

set "DST=%MODS%\!OPID!"
if not exist "!DST!" mkdir "!DST!" >nul 2>nul

rem robocopy /E copies folders and files and has NO /PURGE: nothing of the
rem player's is ever deleted, exactly as update_jericho.bat behaves.
robocopy "!SRC!" "!DST!" /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 >nul
if errorlevel 8 (
	call :failmsg "could not copy the package into MODS\!OPID!"
	goto end_fail
)

set "FILES=0"
rem Count with a plain loop: `find /c /v ""` would resolve to an MSYS find if one
rem is on PATH, and an MSYS find treats /c as a PATH to walk -- a whole-drive scan.
for /f "delims=" %%f in ('dir /b /s /a-d "!DST!" 2^>nul') do set /a FILES+=1

set "VALUE="
call :toml "!DST!\mod.toml" name
set "MNAME=!VALUE!"
set "VALUE="
call :toml "!DST!\mod.toml" version
set "MVER=!VALUE!"
set "VALUE="
call :toml "!DST!\mod.toml" runtime
set "MRUN=!VALUE!"

set "TOMLSHA="
call :sha256 "!DST!\mod.toml"
if not errorlevel 1 set "TOMLSHA=!GOT!"
set "DLLSHA="
if exist "!DST!\!OPID!.dll" (
	call :sha256 "!DST!\!OPID!.dll"
	if not errorlevel 1 set "DLLSHA=!GOT!"
)

call :jsonesc "!MNAME!"
set "JNAME=!ESC!"
call :jsonesc "!MVER!"
set "JVER=!ESC!"
call :jsonesc "!MRUN!"
set "JRUN=!ESC!"
call :jsonesc "!URL!"
set "JURL=!ESC!"

echo {> "%RECORDS%\!OPID!.json"
echo   "id": "!OPID!",>> "%RECORDS%\!OPID!.json"
echo   "name": "!JNAME!",>> "%RECORDS%\!OPID!.json"
echo   "version": "!JVER!",>> "%RECORDS%\!OPID!.json"
echo   "runtime": "!JRUN!",>> "%RECORDS%\!OPID!.json"
echo   "repo": "!JURL!",>> "%RECORDS%\!OPID!.json"
echo   "sha256": "!WANT!",>> "%RECORDS%\!OPID!.json"
echo   "files": !FILES!,>> "%RECORDS%\!OPID!.json"
echo   "modtoml_sha256": "!TOMLSHA!",>> "%RECORDS%\!OPID!.json"
echo   "dll_sha256": "!DLLSHA!",>> "%RECORDS%\!OPID!.json"
echo   "when": "%DATE% %TIME%">> "%RECORDS%\!OPID!.json"
echo }>> "%RECORDS%\!OPID!.json"

if "!MRUN!"=="" (
	call :finish 1 "installed !MNAME! !MVER! into MODS\!OPID! (a compiled-in mod: JERICHO offers to recompile when you leave the repo)"
	goto end
)
call :finish 1 "installed !MNAME! !MVER! into MODS\!OPID!"
goto end

rem ----------------------------------------------------------------- remove
:cmd_remove
set "OP=remove"
if not defined A1 goto usage_remove
set "OPID=%A1%"
set "WAITPID="
if /i "%A2%"=="--wait" set "WAITPID=%A3%"

call :valid_id "!OPID!"
if errorlevel 1 (
	call :failmsg "!OPID! is not a valid mod id"
	goto end_fail
)
if not exist "%RECORDS%\!OPID!.json" (
	call :failmsg "the store did not install !OPID!, so it will not delete it"
	goto end_fail
)

set "DST=%MODS%\!OPID!"
if not exist "!DST!" (
	del /Q "%RECORDS%\!OPID!.json" >nul 2>nul
	call :finish 1 "MODS\!OPID! was already gone; the store's record is cleared"
	goto end
)

if not "!WAITPID!"=="" (
	call :pid_alive "!WAITPID!"
	if errorlevel 1 goto do_remove
	call :log "modstore: waiting for process !WAITPID! to exit before removing MODS\!OPID!"
)
:wait_loop
if "!WAITPID!"=="" goto do_remove
call :pid_alive "!WAITPID!"
if errorlevel 1 goto waited
ping -n 3 127.0.0.1 >nul 2>nul
goto wait_loop
:waited
call :log "modstore: process !WAITPID! has exited; removing MODS\!OPID!"

:do_remove
rmdir /s /q "!DST!" >nul 2>nul
if exist "!DST!" (
	call :failmsg "could not delete MODS\!OPID! - the game may still have it loaded; close the game and try again"
	goto end_fail
)
del /Q "%RECORDS%\!OPID!.json" >nul 2>nul
call :finish 1 "removed MODS\!OPID!"
goto end

rem ----------------------------------------------------------------- rebake
:cmd_rebake
set "OP=rebake"
set "OPID=bake"
if not exist "!BAKE!" set "BAKE=%ROOT%\JERICHO\MODS\mp\tools\pack_lan\BAKE_D1_CARS.bat"
if not exist "!BAKE!" (
	call :failmsg "BAKE_D1_CARS.bat is not beside the game, so the cars cannot be baked"
	goto end_fail
)
call :log "modstore: re-baking Driver 1 cars with !BAKE!"
call "!BAKE!" >> "%LOG%" 2>&1
if errorlevel 1 (
	call :failmsg "the bake failed - see JERICHO\CONFIG\modstore\modstore.log"
	goto end_fail
)
call :finish 1 "Driver 1 cars re-baked"
goto end

rem ============================== helpers ====================================
:log
echo %~1
>> "%LOG%" echo %~1
goto :eof

:jsonesc
rem %1 -> ESC, with no quotes (safe as a JSON string body). A backslash is
rem turned into a forward slash rather than dropped, so a path in a message
rem (MODS\testmod) still reads as a path in the frontend.
rem Undefined stays undefined, so an empty field prints as "".
set "ESC=%~1"
if not defined ESC goto :eof
set "ESC=!ESC:"=!"
if not defined ESC goto :eof
set "ESC=!ESC:\=/!"
goto :eof

:finish
rem %1 = ok (1/0), %2 = message. Always leaves last.json behind.
call :jsonesc "%~2"
set "JMSG=!ESC!"
call :jsonesc "!OP!"
set "JOP=!ESC!"
call :jsonesc "!OPID!"
set "JID=!ESC!"
echo {> "%LAST%.tmp"
echo   "op": "!JOP!",>> "%LAST%.tmp"
echo   "id": "!JID!",>> "%LAST%.tmp"
echo   "ok": %~1,>> "%LAST%.tmp"
echo   "msg": "!JMSG!",>> "%LAST%.tmp"
echo   "script": %MODSTORE_SCRIPT_VERSION%,>> "%LAST%.tmp"
echo   "when": "%DATE% %TIME%">> "%LAST%.tmp"
echo }>> "%LAST%.tmp"
move /Y "%LAST%.tmp" "%LAST%" >nul 2>nul
call :log "modstore: [%OP% %OPID%] %~2"
set "JMSG="
set "JOP="
set "JID="
goto :eof

:failmsg
rem Writes the failure into last.json. NO control flow: the caller does
rem `goto end_fail`, so there is exactly one way out of this script.
call :finish 0 "%~1"
if not "!TMPD!"=="" rmdir /s /q "!TMPD!" >nul 2>nul
goto :eof

:valid_id
rem %1 = id; errorlevel 0 when it is [A-Za-z_][A-Za-z0-9_]*
rem Membership is tested by REMOVING every allowed character from a copy and
rem requiring nothing to be left: no regex, no pipe (a pipe would run in a
rem child without delayed expansion) and no user text inside a pattern.
set "VID=%~1"
if not defined VID exit /b 1
set "FIRST=!VID:~0,1!"
for %%d in (0 1 2 3 4 5 6 7 8 9) do if "!FIRST!"=="%%d" exit /b 1
set "REST=!VID!"
rem Note the `if defined REST` guard: applying a `!VAR:x=!` replacement to an
rem UNDEFINED variable in cmd yields the modifier text itself ("x="), which
rem would leave junk behind the moment REST empties. Once it is undefined the
rem id has passed.
for %%c in (a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z 0 1 2 3 4 5 6 7 8 9 _) do if defined REST set "REST=!REST:%%c=!"
if defined REST exit /b 1
exit /b 0

:fetch
rem %1 = url, %2 = destination
where curl.exe >nul 2>nul
if errorlevel 1 goto fetch_ps
curl.exe -fsSL --connect-timeout 10 --max-time 300 -o "%~2" "%~1"
exit /b %errorlevel%
:fetch_ps
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Invoke-WebRequest -UseBasicParsing -Uri '%~1' -OutFile '%~2' -ErrorAction Stop } catch { exit 1 }"
exit /b %errorlevel%

:sha256
rem %1 = file -> GOT (hex, spaces stripped)
set "GOT="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%~1" SHA256 2^>nul') do (
	if not defined GOT (
		set "H=%%h"
		set "H=!H: =!"
		set "GOT=!H!"
	)
)
if not defined GOT exit /b 1
exit /b 0

:extract
rem %1 = archive, %2 = destination
set "TARX=%SystemRoot%\System32\tar.exe"
if exist "%TARX%" (
	"%TARX%" -xf "%~1" -C "%~2"
	if not errorlevel 1 exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Expand-Archive -LiteralPath '%~1' -DestinationPath '%~2' -Force -ErrorAction Stop } catch { exit 1 }"
if not errorlevel 1 exit /b 0
where python >nul 2>nul
if errorlevel 1 exit /b 1
python -c "import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "%~1" "%~2"
exit /b %errorlevel%

:find_mod_root
rem %1 = unpacked folder -> SRC, the folder holding mod.toml. Both package shapes
rem are accepted: a bare MODS/<id>/ tree, and one rooted at JERICHO/MODS/<id>/
rem (what a release-style layout produces). WHICH id it is is the caller's check.
set "SRC="
if exist "%~1\mod.toml" set "SRC=%~1" & goto :eof
for /f "delims=" %%f in ('dir /b /s "%~1\mod.toml" 2^>nul') do (
	set "SRC=%%~dpf"
	set "SRC=!SRC:~0,-1!"
	goto :eof
)
goto :eof

:toml
rem %1 = file, %2 = key -> VALUE
rem The value is assigned straight from %%b: routing it through `call :sub "%%b"`
rem cuts it at the first space inside the quotes ("Test Mod" -> Test).
set "VALUE="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /r /c:"^ *%~2 *=" "%~1" 2^>nul`) do (
	set "S=%%b"
	goto :toml_have
)
goto :eof
:toml_have
if not defined S goto :eof
set "S=!S:"=!"
if not defined S goto :eof
:toml_ltrim
if not defined S goto :eof
if not "!S:~0,1!"==" " goto :toml_ltrim_done
set "S=!S:~1!"
goto :toml_ltrim
:toml_ltrim_done
set "VALUE=!S!"
goto :eof

:pid_alive
rem %1 = pid -> errorlevel 0 when ALIVE
rem No pipe: a pipe runs in a child cmd where delayed expansion is OFF, so an
rem !PA! there never becomes the number and every pid looks dead. %PA% is
rem expanded by this shell before the child sees the line. CSV output puts the
rem pid in field 2, and tasklist's "no tasks" INFO line has no commas at all.
set "PA=%~1"
if not defined PA exit /b 1
set "ALIVE="
for /f "tokens=2 delims=," %%p in ('tasklist /FI "PID eq %PA%" /NH /FO CSV 2^>nul') do if "%%~p"=="%PA%" set "ALIVE=1"
if defined ALIVE exit /b 0
exit /b 1

rem ============================== exits ======================================
:usage_index
echo modstore: usage: modstore.bat index ^<url^> ^<out-file^>
set "RC=2"
goto end

:usage_install
echo modstore: usage: modstore.bat install ^<package-url^> ^<sha256^> ^<mod-id^>
set "RC=2"
goto end

:usage_remove
echo modstore: usage: modstore.bat remove ^<mod-id^> [--wait ^<pid^>]
set "RC=2"
goto end

:usage
echo modstore: fetch, verify and install ONE mod package.
echo.
echo   modstore.bat version
echo   modstore.bat index ^<url^> ^<out-file^>
echo   modstore.bat install ^<package-url^> ^<sha256^> ^<mod-id^>
echo   modstore.bat remove ^<mod-id^> [--wait ^<pid^>]
echo   modstore.bat rebake
set "RC=2"
goto end

:end_fail
set "RC=1"

:end
if not "!TMPD!"=="" rmdir /s /q "!TMPD!" >nul 2>nul
endlocal & exit /b %RC%
