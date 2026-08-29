# GAILDRV2 drive check: launch the game (Rio, car 2, takeadrive), wait for
# the mod to be live, then run random_agent.py in a VISIBLE console and
# refocus the game window so keyboard injection reaches it.
$ErrorActionPreference = "SilentlyContinue"
$dev = "C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
$py  = "C:\Users\Jaret\Documents\Projects\REDRIVER2\JERICHO\MODS\gaildrv2\python"

Get-Process REDRIVER2_dev, python -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$out = @()
$out += "=== GAILDRV2 drive check (rio, car 2, takeadrive) ==="
$out += "config: " + (Get-Content "$dev\JERICHO\CONFIG\gaildrv2.ini" -Raw)

Start-Process -FilePath "$dev\REDRIVER2_dev.exe" -ArgumentList '-level','rio','-car','2','-gamemode','takeadrive' -WorkingDirectory $dev
$out += "game launched: -level rio -car 2 -gamemode takeadrive"
$out += "probing for a live mod (up to 120s) ..."

$probe = & python -u "$py\probe.py" --timeout 120 2>&1
$out += $probe

if ($probe -match "probe OK") {
    $out += "starting random_agent.py in a visible console ..."
    $agent = Start-Process -FilePath "python" -ArgumentList "-u","$py\random_agent.py","--input","auto","--steps","4000" -WorkingDirectory $py -WindowStyle Normal -PassThru

    # refocus the game window so keyboard injection goes to the game
    Start-Sleep -Seconds 1
    Add-Type -Namespace W -Name F -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);'
    $game = Get-Process REDRIVER2_dev -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($game) { [W.F]::SetForegroundWindow($game.MainWindowHandle) | Out-Null }
    $out += "game window refocused after agent launch"

    # let the agent drive for a while
    Start-Sleep -Seconds 45
    $out += "agent alive: $(-not $agent.HasExited)"
    if (-not $agent.HasExited) {
        $out += "stopping agent (45s drive window done)"
        Stop-Process -Id $agent.Id -Force
    }
} else {
    $out += "RESULT: FAIL (mod never became live in gameplay)"
}

$alive = (Get-Process REDRIVER2_dev -ErrorAction SilentlyContinue) -ne $null
$out += "game alive after drive check: $alive"
$out += "--- game log (gaildrv2 tail) ---"
$out += (Get-Content "$dev\REDRIVER2.log" | Select-String -Pattern "gaildrv2" | Select-Object -Last 6)

$out | Out-File "$dev\drive_outcome.txt" -Encoding utf8
