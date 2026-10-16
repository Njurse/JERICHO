@echo off
rem build_dev.bat - build JERICHO in the Release_dev configuration (x64).
rem
rem This is the DEVELOPMENT build, not the shipping one: DEBUG_OPTIONS,
rem COLLISION_DEBUG, CUTSCENE_RECORDER, the console, the extra dev tooling, and
rem every module in the tree. Use it when you want that tooling - and prefer
rem build_jericho.bat (Release) otherwise, because a symptom that only appears
rem here is usually the tooling rather than the game.
rem
rem The exports.def handling lives in build_config.bat, which this calls.
call "%~dp0build_config.bat" Release_dev JERICHO_dev
exit /b %errorlevel%
