#include "Injector.h"

#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <set>

#define DLL_EXPORT extern "C" __declspec(dllexport)


static HMODULE g_module = NULL;

DLL_EXPORT int Inject(DWORD pid, LPCWSTR module_path, int timeout);

static bool resolve_module_path(LPCWSTR module_path, wchar_t module_full_path[MAX_PATH])
{
	DWORD length;

	if (!module_path || !*module_path)
		return false;

	length = GetFullPathNameW(module_path, MAX_PATH, module_full_path, NULL);
	if (!length || length >= MAX_PATH)
		return false;

	return GetFileAttributesW(module_full_path) != INVALID_FILE_ATTRIBUTES;
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
		snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe->th32ProcessID);
	} while (snapshot == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("%S (%d): Verification Failed: Invalid Handle: %d\n", pe->szExeFile, pe->th32ProcessID, GetLastError());
		return false;
	}

	me.dwSize = sizeof(MODULEENTRY32);
	if (!Module32First(snapshot, &me)) {
		printf("%S (%d): Verification Failed: No Modules: %d\n", pe->szExeFile, pe->th32ProcessID, GetLastError());
		goto out_close;
	}

	// First module is the executable, and this is how we get the full path:
	if (log_name)
		printf("Target process found (%i): %S\n", pe->th32ProcessID, me.szExePath);
	wcscpy_s(exe_path, MAX_PATH, me.szExePath);

	rc = false;
	while (Module32Next(snapshot, &me)) {
		if (_wcsicmp(me.szModule, basename))
			continue;

		if (!_wcsicmp(me.szExePath, module)) {
			if (!pids.count(pe->th32ProcessID)) {
				printf("%d: 3DMigoto loaded :)\n", pe->th32ProcessID);
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
					"WARNING: Found a second copy of 3DMigoto loaded from the game directory:\n"
					"%S\n"
					"This may crash - please remove the copy in the game directory and try again\n\n\n",
					me.szExePath);
			}
		}
	}

out_close:
	CloseHandle(snapshot);
	return rc;
}


static bool check_for_running_target(LPCWSTR target, LPCWSTR module)
{
	// https://docs.microsoft.com/en-us/windows/desktop/ToolHelp/taking-a-snapshot-and-viewing-processes
	HANDLE snapshot;
	PROCESSENTRY32 pe;
	bool rc = false;
	const wchar_t* basename = wcsrchr(target, '\\');
	static std::set<DWORD> pids;

	if (basename)
		basename++;
	else
		basename = target;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("Check Failed: Invalid Handle: %d\n", GetLastError());
		return false;
	}

	pe.dwSize = sizeof(PROCESSENTRY32);
	if (!Process32First(snapshot, &pe)) {
		printf("Check Failed: No Processes: %d\n", GetLastError());
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


// ----------------------------------------------------------------------------
// Sets up the optional global Windows hook for the target library.
// Note: Make sure to remove the hook with UnhookLibrary afterwards.
//
// Error codes:
// 100 - Another instance of 3DMigotoLoader is running
// 200 - Invalid module path
//
// If the DLL cannot be locally loaded, does not export CBTProc, or the hook
// cannot be installed, we fall back to direct remote-thread injection later in
// WaitForInjection, matching the EXE's behaviour more closely. In those cases
// *hook will be NULL and this function still returns success.

DLL_EXPORT int HookLibrary(LPCWSTR module_path, HHOOK* hook, HANDLE* mutex)
{
	FARPROC fn;
	wchar_t module_full_path[MAX_PATH];

	*hook = NULL;
	*mutex = CreateMutexA(0, FALSE, "Local\\3DMigotoLoader");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (*mutex) {
			CloseHandle(*mutex);
			*mutex = NULL;
		}
		return 100;
	}

	if (!resolve_module_path(module_path, module_full_path)) {
		if (*mutex) {
			CloseHandle(*mutex);
			*mutex = NULL;
		}
		return 200;
	}

	g_module = LoadLibraryExW(module_full_path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!g_module) {
		printf("Local load failed for %S (%lu). Falling back to direct injection.\n", module_full_path, GetLastError());
		return EXIT_SUCCESS;
	}

	// Check if dll has CBTProc callback.
	fn = GetProcAddress(g_module, "CBTProc");
	if (!fn) {
		FreeLibrary(g_module);
		g_module = NULL;
		return EXIT_SUCCESS;
	}

	// Setup hook for loaded dll.
	*hook = SetWindowsHookEx(WH_CBT, (HOOKPROC)fn, g_module, 0);
	if (!*hook) {
		printf("SetWindowsHookEx failed (%lu). Falling back to direct injection.\n", GetLastError());
		FreeLibrary(g_module);
		g_module = NULL;
	}

	return EXIT_SUCCESS;
}


static bool try_inject_running_target(LPCWSTR target, LPCWSTR module)
{
	HANDLE snapshot;
	PROCESSENTRY32 pe;
	bool rc = false;
	const wchar_t* basename = wcsrchr(target, '\\');
	static std::set<DWORD> attempted_pids;

	if (basename)
		basename++;
	else
		basename = target;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("Inject Check Failed: Invalid Handle: %d\n", GetLastError());
		return false;
	}

	pe.dwSize = sizeof(PROCESSENTRY32);
	if (!Process32First(snapshot, &pe)) {
		printf("Inject Check Failed: No Processes: %d\n", GetLastError());
		goto out_close;
	}

	do {
		int inject_rc;

		if (_wcsicmp(pe.szExeFile, basename))
			continue;

		if (verify_injection(&pe, module, !attempted_pids.count(pe.th32ProcessID))) {
			rc = true;
			continue;
		}

		if (attempted_pids.count(pe.th32ProcessID))
			continue;

		printf("%lu: Hook path unavailable, attempting direct injection...\n", pe.th32ProcessID);
		inject_rc = Inject(pe.th32ProcessID, module, 15);
		attempted_pids.insert(pe.th32ProcessID);

		if (inject_rc == EXIT_SUCCESS) {
			printf("%lu: Direct injection successful.\n", pe.th32ProcessID);
			rc = true;
		}
		else {
			printf("%lu: Direct injection failed: %d\n", pe.th32ProcessID, inject_rc);
		}
	} while (Process32Next(snapshot, &pe));

out_close:
	CloseHandle(snapshot);
	return rc;
}


// ----------------------------------------------------------------------------
// Waits for the given process to spawn (or until timeout) and ensures the
// module is injected either via hook or direct remote-thread injection.
DLL_EXPORT int WaitForInjection(LPCWSTR module_path, LPCWSTR target_process, int timeout = 10)
{
	wchar_t module_full_path[MAX_PATH];

	if (!resolve_module_path(module_path, module_full_path))
		return EXIT_FAILURE;

	for (int seconds = 0; seconds < +timeout; seconds++) {
		if (check_for_running_target(target_process, module_full_path))
			return EXIT_SUCCESS;
		if (try_inject_running_target(target_process, module_full_path))
			return EXIT_SUCCESS;
		Sleep(1000);
	}
	return EXIT_FAILURE;
}


// ----------------------------------------------------------------------------
// Removes installed hook for given handle and removes the Local\\3DMigotoLoader mutex
DLL_EXPORT int UnhookLibrary(HHOOK* hook, HANDLE* mutex)
{
	bool ok = true;

	if (hook && *hook) {
		if (!UnhookWindowsHookEx(*hook))
			ok = false;
		*hook = NULL;
	}

	if (g_module) {
		if (!FreeLibrary(g_module))
			ok = false;
		g_module = NULL;
	}

	if (mutex && *mutex) {
		if (!CloseHandle(*mutex))
			ok = false;
		*mutex = NULL;
	}

	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}


// ----------------------------------------------------------------------------
// Launches target exe file with provided args and working directory
DLL_EXPORT int StartProcess(LPCWSTR exe_path, LPCWSTR work_dir, LPCWSTR start_args)
{
	HINSTANCE result = ShellExecute(NULL, NULL, exe_path, start_args, work_dir, SW_SHOWNORMAL);
	if ((INT_PTR)result <= 32) {
		return (INT_PTR)result;
	}
	return EXIT_SUCCESS;
}


// ----------------------------------------------------------------------------
// Injects a dll into a process using WriteProcessMemory
//
// Error codes:
//100 - Process not found / cannot open
//110 - Invalid DLL path
//120 - Failed to resolve kernel32.dll
//130 - Failed to resolve LoadLibraryW
//200 - Failed to allocate remote memory
//300 - Failed to write DLL path
//400 - Failed to create remote thread
//500 - Injection thread timed out
//510 - Injection thread wait failed
//600 - DLL injection failed (LoadLibraryW returned NULL)
//700 - Unknown error
DLL_EXPORT int Inject(DWORD pid, LPCWSTR module_path, int timeout = 15)
{
	HANDLE process, thread = NULL;
	LPVOID memory = NULL;
	DWORD thread_exit_code = 0;
	int exit_code = EXIT_SUCCESS;
	wchar_t module_full_path[MAX_PATH];

	if (!resolve_module_path(module_path, module_full_path))
		return 110;

	// Open process with minimal rights
	process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);

	if (!process)
		return 100;

	// Resolve LoadLibraryW
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

	// Length of module path in bytes
	// Path is a wide string so the length must be multiplied by the size of a wide character
	size_t module_path_length = (wcslen(module_full_path) + 1) * sizeof(wchar_t);

	// Allocate memory to hold the module path
	memory = VirtualAllocEx(process, NULL, module_path_length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (!memory) {
		CloseHandle(process);
		return 200;
	}

	// Write module path to allocated memory
	if (!WriteProcessMemory(process, memory, module_full_path, module_path_length, NULL)) {
		VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		CloseHandle(process);
		return 300;
	}

	// Create a thread in the process to load the module from path in memory
	thread = CreateRemoteThreadEx(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library, memory, 0, NULL, NULL);

	if (!thread) {
		VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		CloseHandle(process);
		return 400;
	}

	// Wait for completion
	DWORD wait_code = WaitForSingleObject(thread, timeout * 1000);

	if (wait_code == WAIT_TIMEOUT) {
		CloseHandle(thread);
		VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		CloseHandle(process);
		return 500;
	}

	if (wait_code == WAIT_FAILED) {
		CloseHandle(thread);
		VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		CloseHandle(process);
		return 510;
	}

	// Check LoadLibraryW result
	if (!GetExitCodeThread(thread, &thread_exit_code)) {
		exit_code = 700; // Unknown error
	}
	else if (thread_exit_code == 0) {
		exit_code = 600; // LoadLibrary failed
	}

	// Cleanup
	CloseHandle(thread);
	VirtualFreeEx(process, memory, 0, MEM_RELEASE);
	CloseHandle(process);

	return exit_code;
}


DLL_EXPORT BOOL APIENTRY DllMain(
	_In_  HINSTANCE hinstDLL,
	_In_  DWORD fdwReason,
	_In_  LPVOID lpvReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hinstDLL);
		return true;

	case DLL_PROCESS_DETACH:
		break;

	case DLL_THREAD_ATTACH:
		break;

	case DLL_THREAD_DETACH:
		break;
	}
	return true;
}
