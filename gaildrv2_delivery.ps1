# GAILDRV2 delivery check v2:
#  1. launch the game, 2. wait until the mod answers (probe), 3. run the full
#  test_bridge.py suite in a VISIBLE console (tee'd to a log for review).
$ErrorActionPreference = "SilentlyContinue"
$dev = "C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
$py  = "C:\Users\Jaret\Documents\Projects\REDRIVER2\JERICHO\MODS\gaildrv2\python"

Get-Process REDRIVER2_dev, python -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$out = @()
$out += "=== delivery check v2 ==="
$out += "config: " + (Get-Content "$dev\JERICHO\CONFIG\gaildrv2.ini" -Raw)

Start-Process -FilePath "$dev\REDRIVER2_dev.exe" -ArgumentList '-level','havana','-gamemode','takeadrive' -WorkingDirectory $dev
$out += "game launched, probing for a live mod (up to 120s) ..."

$probe = & python -u "$py\probe.py" --timeout 120 2>&1
$out += $probe

if ($probe -match "probe OK") {
    $out += "starting test_bridge.py in a visible console ..."
    Start-Process -FilePath "python" -ArgumentList "-u","$py\test_bridge.py","--log","$dev\delivery_test.log" -WorkingDirectory $py -WindowStyle Normal
    # wait for the test to finish (python process exits when done)
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (-not (Get-Process python -ErrorAction SilentlyContinue)) { break }
    }
    $out += "--- test output (delivery_test.log) ---"
    $out += (Get-Content "$dev\delivery_test.log" -ErrorAction SilentlyContinue)
} else {
    $out += "RESULT: FAIL (mod never became live in gameplay)"
}

$alive = (Get-Process REDRIVER2_dev -ErrorAction SilentlyContinue) -ne $null
$out += "game alive after tests: $alive"
$out += "--- game log (gaildrv2 tail) ---"
$out += (Get-Content "$dev\REDRIVER2.log" | Select-String -Pattern "gaildrv2" | Select-Object -Last 8)

$out | Out-File "$dev\delivery_outcome.txt" -Encoding utf8
