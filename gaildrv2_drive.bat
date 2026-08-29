@echo off
rem GAILDRV2 one-click launcher: game (Rio, car 2, takeadrive) + visible agent.
rem The agent injects input via vgamepad (focus-independent virtual Xbox pad);
rem UNPLUG physical controllers so the virtual pad is player slot 0 (Pads[0]).
rem Close the agent console (Ctrl+C) when done.
echo.
echo   IMPORTANT: unplug your 8BitDo / physical controller so the virtual
echo   pad becomes player 1 -- otherwise the car will not respond.
echo.
cd /d "C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
start "REDRIVER2" "REDRIVER2_dev.exe" -level rio -car 2 -gamemode takeadrive
cd /d "C:\Users\Jaret\Documents\Projects\REDRIVER2\JERICHO\MODS\gaildrv2\python"
start "GAILDRV2 agent" cmd /k "python -u random_agent.py --input auto --steps 6000"
timeout /t 6 >nul
rem refocus the game window so keyboard injection reaches it
powershell -NoProfile -Command "Add-Type -Namespace W -Name F -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool SetForegroundWindow(IntPtr hWnd);'; $g = Get-Process REDRIVER2_dev -ErrorAction SilentlyContinue | Select-Object -First 1; if ($g) { [W.F]::SetForegroundWindow($g.MainWindowHandle) | Out-Null }"
