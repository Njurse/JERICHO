/*
 * jer_host.h — the small host-platform layer JERICHO needs.
 *
 * Everything in the mod system that depends on the operating system lives
 * behind this interface, so porting JERICHO to a new OS means implementing
 * these few functions — nothing else. jer_host.c ships two back-ends: Win32
 * and POSIX (dlfcn / dirent / fork). A new platform either reuses one of
 * those or adds its own guarded block.
 *
 * The contract a port must satisfy:
 *   - jer_host_lib_variant() lists the on-disk names a compiled module may
 *     have (variant 0, 1, ...); the loader walks them in order.
 *   - jer_host_lib_open / _sym / _close load a module binary and resolve a
 *     symbol (LoadLibrary/GetProcAddress, dlopen/dlsym, ...).
 *   - jer_host_dir_scan() enumerates JERICHO/MODS.
 *   - jer_host_spawn / _poll / _free run one build step and report its exit
 *     code without blocking the game loop.
 *   - jer_host_exe_path() reports the running executable's path.
 *
 * The loader/compile logic in jer_loader.c and jer_compile.c calls only
 * these; it never sees <windows.h>, <dlfcn.h>, <dirent.h> or a PID.
 */
#ifndef JERICHO_JER_HOST_H
#define JERICHO_JER_HOST_H

/* Which host back-end is compiled in: "win32", "posix" or "stub". */
const char* jer_host_name(void);

/* ---- module binaries (DLL / .so / .dylib) ----------------------------- */

/*
 * Walk the plausible on-disk names for the compiled addon `id`. Variant 0 is
 * the canonical name, higher variants are fall-backs (e.g. a `lib` prefix).
 * Returns 1 and writes the bare file name (no directory) into out, or 0 when
 * there are no more variants.
 */
int jer_host_lib_variant(const char* id, int variant, char* out, int max);

/* Open a compiled module at `path`. Returns an opaque handle, or NULL. */
void* jer_host_lib_open(const char* path);

/* Resolve `sym` in a handle from jer_host_lib_open. Returns NULL if missing. */
void* jer_host_lib_sym(void* handle, const char* sym);

/* Close a handle from jer_host_lib_open. */
void jer_host_lib_close(void* handle);

/* A human-readable description of the last jer_host_lib_open / _sym failure
 * (empty when the last call succeeded). Valid until the next such call. */
const char* jer_host_last_error(void);

/* ---- directory scan --------------------------------------------------- */

/* Called once per entry of a scanned directory; isDir is non-zero for a
 * sub-directory. */
typedef void (*JER_HOST_DIR_FN)(const char* name, int isDir, void* user);

/*
 * Call cb for every entry in `path`, skipping "." and "..". Returns the
 * number of entries seen, or -1 if the directory could not be opened.
 */
int jer_host_dir_scan(const char* path, JER_HOST_DIR_FN cb, void* user);

/* ---- child processes (the Compile Mods build steps) ------------------- */

/* Opaque running-process handle. */
typedef struct JER_HOST_PROC JER_HOST_PROC;

/*
 * Run `cmdline` through the platform shell (cmd.exe /c on Windows, sh -c on
 * POSIX). `cmdline` carries its own argument quoting. stdout and stderr are
 * appended to logPath (created if absent); stdin is empty, so a build tool
 * can never block on it. Returns a handle, or NULL if it could not start.
 */
JER_HOST_PROC* jer_host_spawn(const char* cmdline, const char* logPath);

/*
 * Non-blocking poll: 1 when the child has finished (and *exitCode is set),
 * 0 while it is still running, -1 on error. Never blocks the caller.
 */
int jer_host_proc_poll(JER_HOST_PROC* proc, int* exitCode);

/*
 * Release a handle. It does NOT kill the child: a still-running build is left
 * to finish on its own (only the bookkeeping is freed).
 */
void jer_host_proc_free(JER_HOST_PROC* proc);

/* Sleep for `ms` milliseconds. Used by the blocking "Compile Mods" wait so it
 * does not peg a core while it polls. */
void jer_host_sleep_ms(int ms);

/* ---- misc ------------------------------------------------------------- */

/* Path of the running executable (no trailing separator). Returns 1 on
 * success. */
int jer_host_exe_path(char* out, int max);

/* 1 when replacing the running executable's file requires moving it aside
 * first — Windows locks a running image, POSIX relinks in place. */
int jer_host_exe_locks_while_running(void);

/* The shell-script extension the build shims use: "bat" (Windows) or "sh". */
const char* jer_host_script_ext(void);

/* 1 when `path` exists and is a regular (readable) file. */
int jer_host_file_exists(const char* path);

/* Last-modified time of `path`, or 0 when it does not exist. Only the
 * ordering of two values is meaningful (fresh build vs stale). */
long long jer_host_file_mtime(const char* path);

#endif /* JERICHO_JER_HOST_H */
