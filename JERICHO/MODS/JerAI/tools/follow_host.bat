@echo off
rem ============================================================
rem  follow_host.bat -- bring in ONE extra game that joins YOUR
rem  localhost match and, with JerAI, follows you around the map.
rem
rem  You host a match as usual (the game's Host menu, or
rem  mp_host.bat), THEN run this. It starts a joining instance on
rem  127.0.0.1 (port 1400 by default) with JerAI in follow mode, so
rem  that car drives to YOUR car, holds a clearance and idles.
rem
rem  The follower gets its OWN run directory, so its log and mp.ini
rem  never collide with yours. Ctrl+C stops it.
rem
rem  Usage:
rem    follow_host.bat                      (default: 127.0.0.1:1400)
rem    follow_host.bat --port 1400 --client-car slot4
rem    follow_host.bat --clearance 2500 --range 0
rem    follow_host.bat --dry                (print, do not launch)
rem
rem  Anything it does not know is an error -- pass --help to list
rem  everything follow_host.py takes.
rem ============================================================
setlocal

python "%~dp0follow_host.py" %*
exit /b %errorlevel%
