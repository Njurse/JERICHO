@echo off
rem ============================================================================
rem _enable_module.bat <module-id> [exe-dir]
rem
rem   Turn a module ON in the bin/ modlist - the copy the game actually reads.
rem
rem   Why this exists. A launcher writes carhacks.ini, but only the cainescrossfire
rem   module reads that file, and the repo's modlist pins gameplay modules OFF
rem   (they are opt-in). The bin/ copy of the modlist is runtime state - the
rem   frontend rewrites it from Options -> JERICHO - so switching it on belongs to
rem   the launcher, not to a comment saying it must be on. Those launchers used to
rem   work only because the bin mirror was stale; once the repo's modlist actually
rem   propagated, they silently did nothing.
rem
rem   The PostBuild only copies the repo's modlist when the repo's copy is NEWER
rem   (xcopy /D), so this change survives rebuilds until the repo file is edited.
rem
rem   The edit is done in PowerShell, on the file as text. Batch cannot do it
rem   cleanly: `for /f` SKIPS blank lines, so a rewrite quietly deletes the file's
rem   spacing, and echoing a line that contains & or parentheses executes part of
rem   it. Both were observed here before this was moved.
rem
rem   Prints one line either way, so a launch log always says what happened.
rem ============================================================================
setlocal

set "MOD=%~1"
if "%MOD%"=="" (
	echo   module         : _enable_module.bat needs a module id
	exit /b 1
)

set "EXEDIR=%~2"
if "%EXEDIR%"=="" set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "MODLIST=%EXEDIR%\JERICHO\CONFIG\modlist.ini"

if not exist "%MODLIST%" (
	echo   module %MOD% : NO MODLIST at "%MODLIST%" - build the game first
	exit /b 1
)

set "MODULE_ENABLE_ID=%MOD%"
set "MODULE_ENABLE_LIST=%MODLIST%"
set "RC="
for /f "usebackq delims=" %%R in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=$env:MODULE_ENABLE_LIST; $m=[regex]::Escape($env:MODULE_ENABLE_ID); $t=[IO.File]::ReadAllText($p); $on='(?m)^[ \t]*'+$m+'[ \t]*=[ \t]*1([ \t]*(#.*)?)$'; $off='(?m)^([ \t]*'+$m+'[ \t]*=[ \t]*)0([ \t]*(#.*)?)$'; if ($t -match $off) { [IO.File]::WriteAllText($p, ($t -replace $off, '${1}1${2}'), (New-Object Text.UTF8Encoding $false)); 'DONE' } elseif ($t -match $on) { 'ALREADY' } else { 'MISSING' }"`) do set "RC=%%R"

if "%RC%"=="DONE"    echo   module %MOD% : enabled in %MODLIST%
if "%RC%"=="ALREADY" echo   module %MOD% : already enabled
if "%RC%"=="MISSING" echo   module %MOD% : NOT LISTED in %MODLIST% - add a line for it
if "%RC%"==""        echo   module %MOD% : could not run PowerShell - enable it by hand
endlocal
exit /b 0
