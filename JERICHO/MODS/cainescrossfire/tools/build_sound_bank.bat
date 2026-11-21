@echo off
rem Review, build or check Caine's Crossfire's weapon sound bank.
rem   build_sound_bank.bat --review
rem   build_sound_bank.bat --build
rem   build_sound_bank.bat --check
rem   build_sound_bank.bat --all
setlocal
python "%~dp0build_sound_bank.py" %*
exit /b %errorlevel%
