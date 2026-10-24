#!/usr/bin/env bash
# ============================================================
#  JERICHO - build_restart.sh
#
#  The POSIX follow-up to an in-game rebuild (the twin of
#  build_restart.bat). The game cannot run the new image until the
#  process that started the rebuild has exited, so Compile Mods /
#  the mod store's prompt starts this script detached and then quits.
#
#  It waits for that PID to disappear, builds the runtime addons and
#  the deep modules, relinks the exe, and starts the game again. On a
#  failed build the previous exe is restored and started, and
#  CONFIG/build-failed is left for the next boot to report.
#
#  Usage (the game passes these):
#    build_restart.sh <pid> <src_rebuild> <configuration> <exe> [deep ids...]
#
#  The game starts it with CONFIG/build.log as its log; run it by hand
#  to see the output here. It lives beside build_game.sh.
# ============================================================
set -u

HERE=$(cd -- "$(dirname -- "$0")" && pwd)

WAITPID=${1:-}
SRC=${2:-}
CONF=${3:-}
EXE=${4:-}

if [ -z "$WAITPID" ] || [ -z "$SRC" ] || [ -z "$CONF" ] || [ -z "$EXE" ]; then
    echo "usage: build_restart.sh <pid> <src_rebuild> <configuration> <exe> [deep-mod ids...]"
    exit 2
fi

shift 4

EXEDIR=$(cd -- "$(dirname -- "$EXE")" && pwd)
ROOT="$EXEDIR/JERICHO"
LOG="$ROOT/CONFIG/build.log"
FAILED="$ROOT/CONFIG/build-failed"
EXEOLD="$EXE.old"

mkdir -p "$ROOT/CONFIG" 2>/dev/null || true

log() { printf '%s\n' "$*" >> "$LOG" 2>/dev/null || printf '%s\n' "$*"; }

log "JERICHO: waiting for the game (pid $WAITPID) to exit..."
while kill -0 "$WAITPID" 2>/dev/null; do
    sleep 1
done

{
    printf '\n'
    printf '[build] ---- in-game rebuild ----\n'
} >> "$LOG" 2>/dev/null || true

log "JERICHO: building runtime addons and deep modules..."
log "JERICHO: the full compiler output goes to JERICHO/CONFIG/build.log"

# Back the exe up before anything relinks it. POSIX lets the linker replace a
# running image, but a failed link can still leave a truncated file, so the
# last good image is kept.
rm -f "$EXEOLD" >/dev/null 2>&1 || true
if ! cp -f "$EXE" "$EXEOLD" >/dev/null 2>&1; then
    log "JERICHO: could not back up the exe - not building."
    printf 'Rebuild failed: could not back up the exe. See JERICHO/CONFIG/build.log.\n' > "$FAILED" 2>/dev/null || true
    cd "$EXEDIR" 2>/dev/null || true
    exec "$EXE"
fi

rm -f "$FAILED" >/dev/null 2>&1 || true

failed() {
    log "JERICHO: build FAILED - restoring the previous exe."
    if [ -f "$EXEOLD" ]; then
        cp -f "$EXEOLD" "$EXE" >/dev/null 2>&1 || true
        rm -f "$EXEOLD" >/dev/null 2>&1 || true
    fi
    printf 'Rebuild failed. See JERICHO/CONFIG/build.log.\n' > "$FAILED" 2>/dev/null || true
    cd "$EXEDIR" 2>/dev/null || true
    exec "$EXE"
}

log "JERICHO: runtime addons..."
if ! "$HERE/build_mods.sh" >> "$LOG" 2>&1; then
    failed
fi

if [ "$#" -eq 0 ]; then
    # no ids (a tree that could not list them): the driver's "all" step does
    # premake + every deep module + the exe in one go.
    log "JERICHO: building every deep module and linking the game..."
    if ! "$HERE/build_game.sh" "$SRC" "$CONF" all >> "$LOG" 2>&1; then
        failed
    fi
else
    log "JERICHO: generating the build..."
    if ! "$HERE/build_game.sh" "$SRC" "$CONF" premake >> "$LOG" 2>&1; then
        failed
    fi

    for modid in "$@"; do
        log "JERICHO: compiling $modid..."
        if ! "$HERE/build_game.sh" "$SRC" "$CONF" mod "$modid" >> "$LOG" 2>&1; then
            failed
        fi
    done

    log "JERICHO: linking the game..."
    if ! "$HERE/build_game.sh" "$SRC" "$CONF" exe >> "$LOG" 2>&1; then
        failed
    fi
fi

rm -f "$EXEOLD" >/dev/null 2>&1 || true
log "JERICHO: build OK - starting the new exe."
cd "$EXEDIR" 2>/dev/null || true
exec "$EXE"
