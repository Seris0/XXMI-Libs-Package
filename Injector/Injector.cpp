// Injector.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "Injector.h"
#include "util_min.h"

#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <set>

static void wait_keypress(const char* msg)
{
	puts(msg);
	getchar();
}

static void wait_exit(int code = 0, char* msg = "\nPress enter to close...\n")
{
	wait_keypress(msg);
	exit(code);
}

static void exit_usage(const char* msg)
{
	printf("The Loader is not configured correctly. Please copy the d3d11.dll\n"
		"and d3dx.ini into this directory, then edit the d3dx.ini's [Loader] section\n"
		"to set the target executable and module name.\n"
		"\n"
		"%s", msg);

	wait_exit(EXIT_FAILURE);
}

static bool check_file_description(const char* buf, const char* module_path)
{
	struct LANGANDCODEPAGE {
		WORD wLanguage;
		WORD wCodePage;
	} *translate_query;
	char id[50];
	char* file_description = "";
	unsigned int query_size, file_desc_size;
	HRESULT hr;
	unsigned i;

	if (!VerQueryValueA(buf, "\\VarFileInfo\\Translation", (void**)&translate_query, &query_size))
		wait_exit(EXIT_FAILURE, "3DMigoto file information query failed\n");

	for (i = 0; i < (query_size / sizeof(struct LANGANDCODEPAGE)); i++) {
		hr = _snprintf_s(id, 50, 50, "\\StringFileInfo\\%04x%04x\\FileDescription",
			translate_query[i].wLanguage,
			translate_query[i].wCodePage);
		if (FAILED(hr))
			wait_exit(EXIT_FAILURE, "3DMigoto file description query bugged\n");

		if (!VerQueryValueA(buf, id, (void**)&file_description, &file_desc_size))
			wait_exit(EXIT_FAILURE, "3DMigoto file description query failed\n");

		printf("%s description: \"%s\"\n", module_path, file_description);
		if (!strncmp(file_description, "3Dmigoto", 8))
			return true;
	}

	return false;
}

static void check_3dmigoto_version(const char* module_path, const char* ini_section)
{
	VS_FIXEDFILEINFO* query = NULL;
	DWORD pointless_handle = 0;
	unsigned int size;
	char* buf;

	size = GetFileVersionInfoSizeA(module_path, &pointless_handle);
	if (!size)
		wait_exit(EXIT_FAILURE, "3DMigoto version size check failed\n");

	buf = new char[size];

	if (!GetFileVersionInfoA(module_path, pointless_handle, size, buf))
		wait_exit(EXIT_FAILURE, "3DMigoto version info check failed\n");

	if (!check_file_description(buf, module_path)) {
		printf("ERROR: The requested module \"%s\" is not 3DMigoto\n"
			"Please ensure that [Loader] \"module\" is set correctly and the DLL is in place.", module_path);
		wait_exit(EXIT_FAILURE);
	}

	if (!VerQueryValueA(buf, "\\", (void**)&query, &size))
		wait_exit(EXIT_FAILURE, "3DMigoto version query check failed\n");

	printf("3DMigoto Version %d.%d.%d\n",
		query->dwProductVersionMS >> 16,
		query->dwProductVersionMS & 0xffff,
		query->dwProductVersionLS >> 16);

	if (query->dwProductVersionMS < 0x00010003 ||
		query->dwProductVersionMS == 0x00010003 && query->dwProductVersionLS < 0x000f0000) {
		wait_exit(EXIT_FAILURE, "This version of 3DMigoto is too old to be safely loaded - please use 1.3.15 or later\n");
	}

	delete[] buf;
}

static bool verify_injection(PROCESSENTRY32* pe, const wchar_t* module, bool log_name)
{
	HANDLE snapshot;
	MODULEENTRY32 me;
	const wchar_t* basename = wcsrchr(module, '\\');
	bool rc = false;
	static std::set<DWORD> pids;
	wchar_t exe_path[MAX_PATH], mod_path[MAX_PATH];

	if (basename)
		basename++;
	else
		basename = module;

	do {
		snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe->th32ProcessID);
	} while (snapshot == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("%S (%d): Unable to verify if module was successfully loaded: %d\n",
			pe->szExeFile, pe->th32ProcessID, GetLastError());
		return false;
	}

	me.dwSize = sizeof(MODULEENTRY32);
	if (!Module32First(snapshot, &me)) {
		printf("%S (%d): Unable to verify if module was successfully loaded: %d\n",
			pe->szExeFile, pe->th32ProcessID, GetLastError());
		goto out_close;
	}

	if (log_name)
		printf("Target process found (%i): %S\n", pe->th32ProcessID, me.szExePath);
	wcscpy_s(exe_path, MAX_PATH, me.szExePath);

	rc = false;
	while (Module32Next(snapshot, &me)) {
		if (_wcsicmp(me.szModule, basename))
			continue;

		if (!_wcsicmp(me.szExePath, module)) {
			if (!pids.count(pe->th32ProcessID)) {
				printf("%d: Module loaded :)\n", pe->th32ProcessID);
				pids.insert(pe->th32ProcessID);
			}
			rc = true;
		}
		else {
			wcscpy_s(mod_path, MAX_PATH, me.szExePath);
			wcsrchr(exe_path, L'\\')[1] = '\0';
			wcsrchr(mod_path, L'\\')[1] = '\0';
			if (!_wcsicmp(exe_path, mod_path)) {
				printf("\n\n\n"
					"WARNING: Found a second copy of the module loaded from the game directory:\n"
					"%S\n"
					"This may crash - please remove the copy in the game directory and try again\n\n\n",
					me.szExePath);
				wait_exit(EXIT_FAILURE);
			}
		}
	}

out_close:
	CloseHandle(snapshot);
	return rc;
}

static bool check_for_running_target(wchar_t* target, const wchar_t* module)
{
	HANDLE snapshot;
	PROCESSENTRY32 pe;
	bool rc = false;
	wchar_t* basename = wcsrchr(target, '\\');
	static std::set<DWORD> pids;

	if (basename)
		basename++;
	else
		basename = target;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("Unable to verify if module was successfully loaded: %d\n", GetLastError());
		return false;
	}

	pe.dwSize = sizeof(PROCESSENTRY32);
	if (!Process32First(snapshot, &pe)) {
		printf("Unable to verify if module was successfully loaded: %d\n", GetLastError());
		goto out_close;
	}

	do {
		if (_wcsicmp(pe.szExeFile, basename))
			continue;

		rc = verify_injection(&pe, module, !pids.count(pe.th32ProcessID)) || rc;
		pids.insert(pe.th32ProcessID);
	} while (Process32Next(snapshot, &pe));

out_close:
	CloseHandle(snapshot);
	return rc;
}

static void wait_for_target(const char* target_a, const wchar_t* module_path, bool wait, int delay, bool launched)
{
	wchar_t target_w[MAX_PATH];

	if (!MultiByteToWideChar(CP_UTF8, 0, target_a, -1, target_w, MAX_PATH))
		return;

	for (int seconds = 0; wait || delay == -1; seconds++) {
		if (check_for_running_target(target_w, module_path) && delay != -1)
			break;
		Sleep(1000);

		if (launched && seconds == 3) {
			printf("\nStill waiting for the game to start...\n"
				"If the game does not launch automatically, leave this window open and run it manually.\n"
				"You can also adjust/remove the [Loader] launch= option in the d3dx.ini as desired.\n\n");
		}
	}

	for (int i = delay; i > 0; i--) {
		printf("Shutting down loader in %i...\r", i);
		Sleep(1000);
		check_for_running_target(target_w, module_path);
	}
	printf("\n");
}

static void elevate_privileges()
{
	DWORD size = sizeof(TOKEN_ELEVATION);
	TOKEN_ELEVATION Elevation;
	wchar_t path[MAX_PATH];
	HANDLE token = NULL;
	int rc;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return;

	if (!GetTokenInformation(token, TokenElevation, &Elevation, sizeof(Elevation), &size)) {
		CloseHandle(token);
		return;
	}

	CloseHandle(token);

	if (Elevation.TokenIsElevated)
		return;

	if (!GetModuleFileName(NULL, path, MAX_PATH))
		return;

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	rc = (int)(uintptr_t)ShellExecute(NULL, L"runas", path, NULL, NULL, SW_SHOWNORMAL);
	if (rc > 32)
		exit(0);
	if (rc == SE_ERR_ACCESSDENIED)
		wait_exit(EXIT_FAILURE, "Unable to run as admin: Access Denied\n");
	printf("Unable to run as admin: %d\n", rc);
	wait_exit(EXIT_FAILURE);
}

wchar_t* deduce_working_directory(wchar_t* setting, wchar_t dir[MAX_PATH])
{
	DWORD ret;
	wchar_t* file_part = NULL;

	ret = GetFullPathName(setting, MAX_PATH, dir, &file_part);
	if (!ret || ret >= MAX_PATH)
		return NULL;

	ret = GetFileAttributes(dir);
	if (ret == INVALID_FILE_ATTRIBUTES)
		return NULL;

	if (!(ret & FILE_ATTRIBUTE_DIRECTORY) && file_part)
		*file_part = '\0';

	printf("Using working directory: \"%S\"\n", dir);

	return dir;
}

// ----------------------------------------------------------------------------
// Injects a DLL into a remote process using WriteProcessMemory + CreateRemoteThread.
//
// Error codes:
// 100 - Cannot open process
// 110 - Module path not found on disk
// 120 - Failed to resolve kernel32.dll
// 130 - Failed to resolve LoadLibraryW
// 200 - Failed to allocate remote memory
// 300 - Failed to write DLL path to remote memory
// 400 - Failed to create remote thread
// 500 - Remote thread timed out
// 510 - WaitForSingleObject failed
// 600 - LoadLibraryW returned NULL in remote process
// 700 - Unknown error getting thread exit code
static int inject_into_pid(DWORD pid, const wchar_t* module_path, int timeout = 15)
{
	HANDLE process, thread = NULL;
	LPVOID memory = NULL;
	DWORD thread_exit_code = 0;
	int exit_code = EXIT_SUCCESS;

	process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
		PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
		FALSE, pid);
	if (!process)
		return 100;

	if (!module_path || GetFileAttributesW(module_path) == INVALID_FILE_ATTRIBUTES) {
		CloseHandle(process);
		return 110;
	}

	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (!kernel32) {
		CloseHandle(process);
		return 120;
	}

	FARPROC load_library = GetProcAddress(kernel32, "LoadLibraryW");
	if (!load_library) {
		CloseHandle(process);
		return 130;
	}

	size_t module_path_length = (wcslen(module_path) + 1) * sizeof(wchar_t);

	memory = VirtualAllocEx(process, NULL, module_path_length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!memory) {
		CloseHandle(process);
		return 200;
	}

	if (!WriteProcessMemory(process, memory, module_path, module_path_length, NULL)) {
		VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		CloseHandle(process);
		return 300;
	}

	thread = CreateRemoteThreadEx(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library,
		memory, 0, NULL, NULL);
	if (!thread) {
		VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		CloseHandle(process);
		return 400;
	}

	DWORD wait_code = WaitForSingleObject(thread, timeout * 1000);
	if (wait_code == WAIT_TIMEOUT) {
		exit_code = 500;
	}
	else if (wait_code == WAIT_FAILED) {
		exit_code = 510;
	}
	else if (!GetExitCodeThread(thread, &thread_exit_code)) {
		exit_code = 700;
	}
	else if (thread_exit_code == 0) {
		exit_code = 600;
	}

	CloseHandle(thread);
	VirtualFreeEx(process, memory, 0, MEM_RELEASE);
	CloseHandle(process);

	return exit_code;
}

int main()
{
	char* buf, target[MAX_PATH], setting[MAX_PATH], module_path[MAX_PATH];
	wchar_t setting_w[MAX_PATH], working_dir[MAX_PATH], * working_dir_p = NULL;
	wchar_t module_full_path[MAX_PATH], target_w[MAX_PATH];
	DWORD filesize, readsize;
	const char* ini_section;
	int rc = EXIT_FAILURE;
	HANDLE ini_file;
	bool launch;

	CreateMutexA(0, FALSE, "Local\\3DMigotoLoader");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
		wait_exit(EXIT_FAILURE, "ERROR: Another instance of the Loader is already running. Please close it and try again\n");

	printf("\n--------------------------------- Loader -------------------------------------\n\n");

	ini_file = CreateFile(L"d3dx.ini", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (ini_file == INVALID_HANDLE_VALUE)
		exit_usage("Unable to open d3dx.ini\n");

	filesize = GetFileSize(ini_file, NULL);
	buf = new char[filesize + 1];
	if (!buf)
		wait_exit(EXIT_FAILURE, "Unable to allocate d3dx.ini buffer\n");

	if (!ReadFile(ini_file, buf, filesize, &readsize, 0) || filesize != readsize)
		wait_exit(EXIT_FAILURE, "Error reading d3dx.ini\n");

	CloseHandle(ini_file);

	ini_section = find_ini_section_lite(buf, "loader");
	if (!ini_section)
		exit_usage("d3dx.ini missing required [Loader] section\n");

	if (!find_ini_setting_lite(ini_section, "target", target, MAX_PATH))
		exit_usage("d3dx.ini [Loader] section missing required \"target\" setting\n");

	if (!find_ini_setting_lite(ini_section, "module", module_path, MAX_PATH))
		exit_usage("d3dx.ini [Loader] section missing required \"module\" setting\n");

	// check_version defaults to false so generic DLLs (e.g. ReShade64.dll)
	// are accepted without extra config. Add check_version=true to [Loader]
	// in d3dx.ini to restore the strict 3DMigoto version check.
	if (find_ini_bool_lite(ini_section, "check_version", false))
		check_3dmigoto_version(module_path, ini_section);

	if (find_ini_bool_lite(ini_section, "require_admin", false))
		elevate_privileges();

	// Resolve the full absolute path of the module so the remote process
	// can find it regardless of its own working directory.
	if (!MultiByteToWideChar(CP_UTF8, 0, module_path, -1, module_full_path, MAX_PATH) ||
		!GetFullPathNameW(module_full_path, MAX_PATH, module_full_path, NULL))
		wait_exit(EXIT_FAILURE, "Invalid module path\n");

	if (GetFileAttributesW(module_full_path) == INVALID_FILE_ATTRIBUTES) {
		printf("Module not found: %S\n", module_full_path);
		wait_exit(EXIT_FAILURE);
	}

	printf("Module path: %S\n\n", module_full_path);

	// Convert target process name to wide for comparisons
	if (!MultiByteToWideChar(CP_UTF8, 0, target, -1, target_w, MAX_PATH))
		wait_exit(EXIT_FAILURE, "Invalid target setting\n");

	// Launch the game if configured
	launch = find_ini_setting_lite(ini_section, "launch", setting, MAX_PATH);
	if (launch) {
		printf("Launching \"%s\"...\n", setting);
		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

		if (!MultiByteToWideChar(CP_UTF8, 0, setting, -1, setting_w, MAX_PATH))
			wait_exit(EXIT_FAILURE, "Invalid launch setting\n");

		working_dir_p = deduce_working_directory(setting_w, working_dir);
		ShellExecute(NULL, NULL, setting_w, NULL, working_dir_p, SW_SHOWNORMAL);
	}

	printf("Waiting for target process \"%s\"...\n", target);

	// Basename of target exe for process list matching
	const wchar_t* target_basename = wcsrchr(target_w, '\\');
	target_basename = target_basename ? target_basename + 1 : target_w;

	// Wait up to wait_for_target_timeout seconds (default 60) for the
	// target process to appear, then inject. Already-attempted PIDs are
	// skipped so we don't retry failed injections in the same session.
	bool injected = false;
	std::set<DWORD> attempted_pids;
	int wait_limit = find_ini_int_lite(ini_section, "wait_for_target_timeout", 60);

	for (int i = 0; i < wait_limit && !injected; i++) {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			PROCESSENTRY32 pe;
			pe.dwSize = sizeof(PROCESSENTRY32);

			if (Process32First(snap, &pe)) {
				do {
					if (_wcsicmp(pe.szExeFile, target_basename))
						continue;
					if (attempted_pids.count(pe.th32ProcessID))
						continue;

					printf("Target found (PID %lu), injecting...\n", pe.th32ProcessID);
					int inject_rc = inject_into_pid(pe.th32ProcessID, module_full_path, 15);
					attempted_pids.insert(pe.th32ProcessID);

					if (inject_rc == EXIT_SUCCESS) {
						printf("Injection successful!\n");
						injected = true;
					}
					else {
						printf("Injection failed with code: %d\n", inject_rc);
					}
				} while (Process32Next(snap, &pe));
			}
			CloseHandle(snap);
		}

		if (!injected) Sleep(1000);
	}

	if (!injected) {
		printf("Timed out waiting for target process.\n");
		wait_exit(EXIT_FAILURE);
	}

	rc = EXIT_SUCCESS;

	// Keep loader alive while the game runs so we can verify injection
	if (find_ini_bool_lite(ini_section, "wait_for_target", true)) {
		printf("Monitoring... (close this window to stop)\n");
		wait_for_target(target, module_full_path,
			true,
			find_ini_int_lite(ini_section, "delay", 0),
			launch);
	}

	delete[] buf;
	return rc;
}