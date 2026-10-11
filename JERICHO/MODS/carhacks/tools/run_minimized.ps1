# run_minimized.ps1 - start the game MINIMIZED, capture its stdout, wait, return its exit code.
#
#   powershell -ExecutionPolicy Bypass -File run_minimized.ps1 <exe> <stdout-log> [args...]
#
# Why: the game opens an SDL window, and a rig runs it a dozen times in a row - a window
# stealing focus on every row is unbearable. Start-Process (not cmd's "start") gives a real
# PID AND can minimize the window, while -RedirectStandardOutput still captures the run's
# text, which is what the rigs grep. The process self-terminates via -frames, so nothing is
# ever killed by image name. The child inherits the caller's environment, so a lever exported
# by the rig (export JERICHO_DUMPVRAM=1) still reaches the game.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)

$dir = Split-Path -Parent (Resolve-Path $Exe)
$err = "$Out.err"

$p = Start-Process -FilePath $Exe -ArgumentList $Rest -WorkingDirectory $dir `
    -WindowStyle Minimized -RedirectStandardOutput $Out -RedirectStandardError $err -PassThru

$p.WaitForExit()
exit $p.ExitCode
