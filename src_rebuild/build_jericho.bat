@echo off
rem build_jericho.bat - build JERICHO in the Release configuration (x64, shipping).
rem
rem Build THIS one by default. Release is the configuration that ships: it
rem pre-includes only the released modules (carhacks + crumple + d1cars + mp - see
rem JERICHO_RELEASE_MODS in premake5.lua) and carries no DEBUG_OPTIONS, console or
rem dev-only tooling. Use build_dev.bat only when you actually want that extra
rem tooling: a symptom that shows up in Release_dev but not in Release is nearly
rem always the dev tooling itself.
rem
rem The exports.def handling - and why a bare msbuild fails here with LNK2001 -
rem lives in build_config.bat, which this calls.
call "%~dp0build_config.bat" Release JERICHO
exit /b %errorlevel%
