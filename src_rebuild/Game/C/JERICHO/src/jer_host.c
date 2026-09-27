/*
 * jer_host.c — Win32 / POSIX back-ends for the JERICHO host layer
 * (see jer_host.h).
 *
 * This is the ONLY file in the mod system that includes a platform SDK
 * header (<windows.h>, <dlfcn.h>, <dirent.h>, <unistd.h>). Everything else
 * — the loader, the manager, the compile driver — talks to the interface in
 * jer_host.h, so porting to a new OS is a matter of adding one back-end
 * block here.
 *
 * Back-ends, selected by the preprocessor:
 *   win32   LoadLibraryA / FindFirstFileA / CreateProcessA / GetModuleFileNameA
 *   mac     dlopen / opendir / fork+execl / _NSGetExecutablePath
 *   posix   dlopen / opendir / fork+execl / /proc/self/exe
 *   stub    no runtime loading (emscripten / android / unknown): every call
 *           fails cleanly, exactly as the old inline guards did.
 */

#include "jer_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define JER_HOST_WIN32 1
#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)
#  define JER_HOST_STUB 1
#elif defined(__APPLE__)
#  define JER_HOST_MAC 1
#elif defined(__unix__)
#  define JER_HOST_POSIX 1
#else
#  define JER_HOST_STUB 1
#endif

/* ================================================================== */
/* identity                                                           */
/* ================================================================== */

const char* jer_host_name(void)
{
#if defined(JER_HOST_WIN32)
	return "win32";
#elif defined(JER_HOST_MAC)
	return "mac";
#elif defined(JER_HOST_POSIX)
	return "posix";
#else
	return "stub";
#endif
}

/* shared: "." and ".." are never worth reporting in a scan */
static int jerHostIsDot(const char* name)
{
	return name[0] == '.' &&
		(name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

/* shared: the last load/symbol failure, for jer_host_last_error() */
static char gJerHostErr[256];

const char* jer_host_last_error(void)
{
	return gJerHostErr;
}

/* shared: `path` names an existing, readable regular file */
int jer_host_file_exists(const char* path)
{
	FILE* f;

	if (path == NULL || path[0] == 0)
		return 0;

	f = fopen(path, "rb");

	if (f == NULL)
		return 0;

	fclose(f);
	return 1;
}

/* ================================================================== */
/* Win32                                                              */
/* ================================================================== */

#if defined(JER_HOST_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int jer_host_lib_variant(const char* id, int variant, char* out, int max)
{
	if (variant != 0)
		return 0;	/* Windows only ever has <id>.dll */

	snprintf(out, (size_t)max, "%s.dll", id);
	return 1;
}

void* jer_host_lib_open(const char* path)
{
	void* h = (void*)LoadLibraryA(path);

	if (h == NULL)
		snprintf(gJerHostErr, sizeof(gJerHostErr),
			"LoadLibrary(%s) failed (error %lu)", path, (unsigned long)GetLastError());
	else
		gJerHostErr[0] = 0;

	return h;
}

void* jer_host_lib_sym(void* handle, const char* sym)
{
	void* p;

	if (handle == NULL)
		return NULL;

	p = (void*)GetProcAddress((HMODULE)handle, sym);

	if (p == NULL)
		snprintf(gJerHostErr, sizeof(gJerHostErr),
			"GetProcAddress(%s) failed (error %lu)", sym, (unsigned long)GetLastError());
	else
		gJerHostErr[0] = 0;

	return p;
}

void jer_host_lib_close(void* handle)
{
	if (handle != NULL)
		FreeLibrary((HMODULE)handle);
}

int jer_host_dir_scan(const char* path, JER_HOST_DIR_FN cb, void* user)
{
	char pattern[640];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int count = 0;

	if (cb == NULL)
		return -1;

	snprintf(pattern, sizeof(pattern), "%s/*", path);
	h = FindFirstFileA(pattern, &fd);

	if (h == INVALID_HANDLE_VALUE)
		return -1;

	do
	{
		if (jerHostIsDot(fd.cFileName))
			continue;

		cb(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0, user);
		count++;
	} while (FindNextFileA(h, &fd) != 0);

	FindClose(h);
	return count;
}

struct JER_HOST_PROC
{
	PROCESS_INFORMATION pi;
	int closed;
};

JER_HOST_PROC* jer_host_spawn(const char* cmdline, const char* logPath)
{
	char full[2048];
	STARTUPINFOA si;
	SECURITY_ATTRIBUTES sa;
	HANDLE hLog = INVALID_HANDLE_VALUE;
	HANDLE hNul = INVALID_HANDLE_VALUE;
	int inherit;
	JER_HOST_PROC* p;

	/* Go through cmd.exe. The command is wrapped in one pair of quotes so a
	 * first token that is itself quoted (a path with spaces) survives — the
	 * cmd.exe /c ""..." "..." ..." form. */
	snprintf(full, sizeof(full), "cmd.exe /c \"%s\"", cmdline);

	p = (JER_HOST_PROC*)calloc(1, sizeof(*p));

	if (p == NULL)
		return NULL;

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	if (logPath != NULL && logPath[0])
		hLog = CreateFileA(logPath, FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	if (hLog != INVALID_HANDLE_VALUE)
	{
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = hLog;
		si.hStdError = hLog;

		/* a build tool must not inherit the game's stdin (it could block on
		 * it and the step would then never finish): give it NUL. */
		hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		si.hStdInput = hNul;
	}

	inherit = (hLog != INVALID_HANDLE_VALUE);

	if (!CreateProcessA(NULL, full, NULL, NULL, inherit, CREATE_NO_WINDOW,
		NULL, NULL, &si, &p->pi))
	{
		if (hLog != INVALID_HANDLE_VALUE)
			CloseHandle(hLog);
		if (hNul != INVALID_HANDLE_VALUE)
			CloseHandle(hNul);

		free(p);
		return NULL;
	}

	if (hLog != INVALID_HANDLE_VALUE)
		CloseHandle(hLog);
	if (hNul != INVALID_HANDLE_VALUE)
		CloseHandle(hNul);

	return p;
}

int jer_host_proc_poll(JER_HOST_PROC* p, int* exitCode)
{
	DWORD rc = 1;

	if (p == NULL || p->closed)
		return -1;

	if (WaitForSingleObject(p->pi.hProcess, 0) != WAIT_OBJECT_0)
		return 0;	/* still running */

	if (!GetExitCodeProcess(p->pi.hProcess, &rc))
		rc = 1;

	if (exitCode != NULL)
		*exitCode = (int)rc;

	return 1;
}

void jer_host_proc_free(JER_HOST_PROC* p)
{
	if (p == NULL)
		return;

	if (!p->closed)
	{
		CloseHandle(p->pi.hThread);
		CloseHandle(p->pi.hProcess);
		p->closed = 1;
	}

	free(p);
}

void jer_host_sleep_ms(int ms)
{
	if (ms > 0)
		Sleep((DWORD)ms);
}

int jer_host_exe_path(char* out, int max)
{
	if (out == NULL || max <= 0)
		return 0;

	if (GetModuleFileNameA(NULL, out, (DWORD)max) == 0)
		return 0;

	out[max - 1] = 0;
	return 1;
}

int jer_host_exe_locks_while_running(void)
{
	return 1;	/* Windows will not let the linker overwrite a running image */
}

const char* jer_host_script_ext(void)
{
	return "bat";
}

long long jer_host_file_mtime(const char* path)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	ULARGE_INTEGER u;

	if (path == NULL || !GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
		return 0;

	u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
	return (long long)u.QuadPart;
}

/* ================================================================== */
/* POSIX / macOS                                                      */
/* ================================================================== */

#elif defined(JER_HOST_POSIX) || defined(JER_HOST_MAC)

#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#if defined(JER_HOST_MAC)
#  include <mach-o/dyld.h>
#  define JER_HOST_EXT "dylib"
#else
#  define JER_HOST_EXT "so"
#endif

int jer_host_lib_variant(const char* id, int variant, char* out, int max)
{
	if (variant == 0)
	{
		snprintf(out, (size_t)max, "%s." JER_HOST_EXT, id);
		return 1;
	}

	if (variant == 1)
	{
		/* some toolchains insist on the lib prefix; accept it too */
		snprintf(out, (size_t)max, "lib%s." JER_HOST_EXT, id);
		return 1;
	}

	return 0;
}

void* jer_host_lib_open(const char* path)
{
	void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);

	if (h == NULL)
	{
		const char* e = dlerror();
		snprintf(gJerHostErr, sizeof(gJerHostErr), "%s", e ? e : "dlopen failed");
	}
	else
	{
		gJerHostErr[0] = 0;
	}

	return h;
}

void* jer_host_lib_sym(void* handle, const char* sym)
{
	void* p;

	if (handle == NULL)
		return NULL;

	p = dlsym(handle, sym);

	if (p == NULL)
	{
		const char* e = dlerror();
		snprintf(gJerHostErr, sizeof(gJerHostErr), "%s", e ? e : "dlsym failed");
	}
	else
	{
		gJerHostErr[0] = 0;
	}

	return p;
}

void jer_host_lib_close(void* handle)
{
	if (handle != NULL)
		dlclose(handle);
}

int jer_host_dir_scan(const char* path, JER_HOST_DIR_FN cb, void* user)
{
	DIR* d;
	struct dirent* de;
	int count = 0;

	if (cb == NULL)
		return -1;

	d = opendir(path);

	if (d == NULL)
		return -1;

	while ((de = readdir(d)) != NULL)
	{
		char full[640];
		struct stat st;
		int isDir = 0;

		if (jerHostIsDot(de->d_name))
			continue;

		snprintf(full, sizeof(full), "%s/%s", path, de->d_name);

		if (stat(full, &st) == 0)
			isDir = S_ISDIR(st.st_mode) ? 1 : 0;

		cb(de->d_name, isDir, user);
		count++;
	}

	closedir(d);
	return count;
}

struct JER_HOST_PROC
{
	pid_t pid;
	int reaped;
	int exitCode;
};

JER_HOST_PROC* jer_host_spawn(const char* cmdline, const char* logPath)
{
	JER_HOST_PROC* p;
	pid_t pid;

	p = (JER_HOST_PROC*)calloc(1, sizeof(*p));

	if (p == NULL)
		return NULL;

	pid = fork();

	if (pid < 0)
	{
		free(p);
		return NULL;
	}

	if (pid == 0)
	{
		/* child: append stdout+stderr to the log, read stdin from /dev/null */
		int fd;

		if (logPath != NULL && logPath[0])
		{
			fd = open(logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);

			if (fd >= 0)
			{
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);

				if (fd > STDERR_FILENO)
					close(fd);
			}
		}

		fd = open("/dev/null", O_RDONLY);

		if (fd >= 0)
		{
			dup2(fd, STDIN_FILENO);

			if (fd > STDERR_FILENO)
				close(fd);
		}

		execl("/bin/sh", "sh", "-c", cmdline, (char*)NULL);
		_exit(127);		/* exec failed */
	}

	p->pid = pid;
	return p;
}

int jer_host_proc_poll(JER_HOST_PROC* p, int* exitCode)
{
	int status = 0;
	pid_t r;

	if (p == NULL)
		return -1;

	if (p->reaped)
	{
		if (exitCode != NULL)
			*exitCode = p->exitCode;
		return 1;
	}

	r = waitpid(p->pid, &status, WNOHANG);

	if (r == 0)
		return 0;	/* still running */
	if (r < 0)
		return -1;

	p->reaped = 1;
	p->exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
		: (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);

	if (exitCode != NULL)
		*exitCode = p->exitCode;

	return 1;
}

void jer_host_proc_free(JER_HOST_PROC* p)
{
	if (p == NULL)
		return;

	if (!p->reaped)
	{
		int status;
		waitpid(p->pid, &status, WNOHANG);	/* reap if it already ended */
	}

	free(p);
}

void jer_host_sleep_ms(int ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int jer_host_exe_path(char* out, int max)
{
	if (out == NULL || max <= 0)
		return 0;

#if defined(JER_HOST_MAC)
	{
		uint32_t size = (uint32_t)max;

		if (_NSGetExecutablePath(out, &size) != 0)
			return 0;
	}
	return 1;
#else
	{
		ssize_t n = readlink("/proc/self/exe", out, (size_t)(max - 1));

		if (n <= 0)
			return 0;

		out[n] = 0;
	}
	return 1;
#endif
}

int jer_host_exe_locks_while_running(void)
{
	return 0;	/* POSIX relinks the running image in place */
}

const char* jer_host_script_ext(void)
{
	return "sh";
}

long long jer_host_file_mtime(const char* path)
{
	struct stat st;

	if (path == NULL || stat(path, &st) != 0)
		return 0;

	return (long long)st.st_mtime;
}

/* ================================================================== */
/* stub (emscripten / android / unknown)                              */
/* ================================================================== */

#else

int jer_host_lib_variant(const char* id, int variant, char* out, int max)
{
	(void)id;
	(void)variant;
	(void)out;
	(void)max;
	return 0;
}

void* jer_host_lib_open(const char* path)
{
	(void)path;
	snprintf(gJerHostErr, sizeof(gJerHostErr), "no runtime module loading on this platform");
	return NULL;
}

void* jer_host_lib_sym(void* handle, const char* sym)
{
	(void)handle;
	(void)sym;
	return NULL;
}

void jer_host_lib_close(void* handle)
{
	(void)handle;
}

int jer_host_dir_scan(const char* path, JER_HOST_DIR_FN cb, void* user)
{
	(void)path;
	(void)cb;
	(void)user;
	return -1;
}

struct JER_HOST_PROC
{
	int unused;
};

JER_HOST_PROC* jer_host_spawn(const char* cmdline, const char* logPath)
{
	(void)cmdline;
	(void)logPath;
	return NULL;
}

int jer_host_proc_poll(JER_HOST_PROC* p, int* exitCode)
{
	(void)p;
	(void)exitCode;
	return -1;
}

void jer_host_proc_free(JER_HOST_PROC* p)
{
	(void)p;
}

void jer_host_sleep_ms(int ms)
{
	(void)ms;
}

int jer_host_exe_path(char* out, int max)
{
	(void)out;
	(void)max;
	return 0;
}

int jer_host_exe_locks_while_running(void)
{
	return 0;
}

const char* jer_host_script_ext(void)
{
	return "sh";
}

long long jer_host_file_mtime(const char* path)
{
	(void)path;
	return 0;
}

#endif
