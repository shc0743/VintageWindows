#include "inject.h"

HMODULE InjectDllToProcess(DWORD dwProcessId, LPCWSTR szDllPath, DWORD dwTimeout) {
	HANDLE hProcess = NULL; // the handle of the target process
	// Open the process
	hProcess = OpenProcess(GENERIC_READ | GENERIC_WRITE | /*Read and write*/
							 PROCESS_CREATE_THREAD         , FALSE, dwProcessId);
	if (!hProcess) return NULL;

	HMODULE ret = InjectDllToProcess_HANDLE(hProcess, szDllPath, dwTimeout);
	CloseHandle(hProcess);
	return ret;
}

HMODULE InjectDllToProcess_HANDLE(HANDLE hProcess, LPCWSTR szDllPath, DWORD dwTimeout) {
	// Status code
	//DWORD dwStatCode = ERROR_SUCCESS;

	// Get the addresses
	// Note: the address of [ntdll,user32,kernel32] is same for all processes
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (!kernel32) return NULL;
	LPTHREAD_START_ROUTINE pLoadLibraryW = (LPTHREAD_START_ROUTINE)
		GetProcAddress(kernel32, "LoadLibraryW");
	if (!pLoadLibraryW) return NULL;

	// Alloc memory for the process to write dll path
	SIZE_T pPathSize = wcslen(szDllPath) * 2;
	PVOID pAddrDllPath = VirtualAllocEx
		(hProcess, NULL, pPathSize, MEM_COMMIT, PAGE_READWRITE);
	if (!pAddrDllPath) {
		//CloseHandle(hProcess);
		return NULL;
	}

	// Write the dll path to the memory
	if (!WriteProcessMemory(hProcess, pAddrDllPath, szDllPath, pPathSize, NULL)) {
		VirtualFreeEx(hProcess, pAddrDllPath, 0, MEM_RELEASE);
		//CloseHandle(hProcess);
		return NULL;
	}

	/*
							***EXECUTE INJECT***    
	*/
	HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0,
		pLoadLibraryW, pAddrDllPath, CREATE_SUSPENDED, NULL);
	if (!hRemoteThread) {
		VirtualFreeEx(hProcess, pAddrDllPath, 0, MEM_RELEASE);
		//CloseHandle(hProcess);
		return NULL;
	}
	HMODULE hModuleRemoteDll = ((HMODULE)(LONG_PTR)-1);
	ResumeThread(hRemoteThread);
	if (WAIT_TIMEOUT != WaitForSingleObject(hRemoteThread, dwTimeout)) {
		DWORD dwExitCode = 0;
		GetExitCodeThread(hRemoteThread, &dwExitCode);
		hModuleRemoteDll = *((HMODULE*)(&dwExitCode)); // Ignore the warning
	}
	CloseHandle(hRemoteThread);

	// do some cleaning
	if (pAddrDllPath) VirtualFreeEx(hProcess, pAddrDllPath, 0, MEM_RELEASE);
	/*CloseHandle(hProcess);*/

	// and return
	return hModuleRemoteDll;
}

DWORD InjectCodeToProcess(
	HANDLE hProcess, PVOID pCodeAddress, SIZE_T size, PVOID paramter, DWORD dwTimeout
) {
	DWORD ret_value = (DWORD)-1;
	// Alloc memory for the process to write dll path
	PVOID pRemoteAddr = VirtualAllocEx
		(hProcess, NULL, size, MEM_COMMIT, PAGE_READWRITE);
	if (!pRemoteAddr) {
		return ret_value;
	}

	// Write the dll path to the memory
	if (!WriteProcessMemory(hProcess, pRemoteAddr, pCodeAddress, size, NULL)) {
		VirtualFreeEx(hProcess, pRemoteAddr, 0, MEM_RELEASE);
		return ret_value;
	}

	/*
							***EXECUTE INJECT***    
	*/
	HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0,
		pRemoteAddr, paramter, CREATE_SUSPENDED, NULL);
	if (!hRemoteThread) {
		VirtualFreeEx(hProcess, pRemoteAddr, 0, MEM_RELEASE);
		return ret_value;
	}
	ResumeThread(hRemoteThread);
	if (WAIT_TIMEOUT != WaitForSingleObject(hRemoteThread, dwTimeout)) {
		DWORD dwExitCode = 0;
		GetExitCodeThread(hRemoteThread, &dwExitCode);
		ret_value = dwExitCode; // Ignore the warning
	}
	CloseHandle(hRemoteThread);

	// do some cleaning
	if (pRemoteAddr) VirtualFreeEx(hProcess, pRemoteAddr, 0, MEM_RELEASE);
	// and return
	return ret_value;
	return (DWORD)-1;
}


#if 0
// trashed
typedef struct {
	HMODULE hModule;
	LPCSTR lpProcName;
} GetThisProcAddr_t, *PGetThisProcAddr_t;
static FARPROC GetThisProcAddr_t(PGetThisProcAddr_t arg) {
	return GetProcAddress(arg->hModule, arg->lpProcName);
}

FARPROC GetRemoteProcAddress(DWORD dwProcessId, HMODULE hModule, LPCSTR lpProcName) {
	// Get the addresses
	// Note: the address of [ntdll,user32,kernel32] is same for all processes
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (!kernel32) return NULL;
	LPTHREAD_START_ROUTINE pLoadLibraryW = (LPTHREAD_START_ROUTINE)
		GetProcAddress(kernel32, "LoadLibraryW");
	if (!pLoadLibraryW) return NULL;

	HANDLE hProcess = NULL; // the handle of the target process
	// Open the process
	hProcess = OpenProcess(GENERIC_READ | GENERIC_WRITE | /*Read and write*/
							 PROCESS_CREATE_THREAD         , FALSE, dwProcessId);
	if (!hProcess) return NULL;

	// Alloc memory for the process to write dll path
	SIZE_T pPathSize = sizeof(GetThisProcAddr_t);
	PVOID pAddrDllPath = VirtualAllocEx
		(hProcess, NULL, pPathSize, MEM_COMMIT, PAGE_READWRITE);
	if (!pAddrDllPath) {
		CloseHandle(hProcess);
		return NULL;
	}

	// Write the dll path to the memory
	if (!WriteProcessMemory(hProcess, pAddrDllPath, szDllPath, pPathSize, NULL)) {
		VirtualFreeEx(hProcess, pAddrDllPath, 0, MEM_RELEASE);
		CloseHandle(hProcess);
		return NULL;
	}

	/*
							***EXECUTE INJECT***    
	*/
	HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0,
		pLoadLibraryW, pAddrDllPath, CREATE_SUSPENDED, NULL);
	if (!hRemoteThread) {
		VirtualFreeEx(hProcess, pAddrDllPath, 0, MEM_RELEASE);
		CloseHandle(hProcess);
		return NULL;
	}
	HMODULE hModuleRemoteDll = ((HMODULE)(LONG_PTR)-1);
	ResumeThread(hRemoteThread);
	if (WAIT_TIMEOUT != WaitForSingleObject(hRemoteThread, INFINITE)) {
		DWORD dwExitCode = 0;
		GetExitCodeThread(hRemoteThread, &dwExitCode);
		hModuleRemoteDll = *((HMODULE*)(&dwExitCode)); // Ignore the warning
	}
	CloseHandle(hRemoteThread);

	// do some cleaning
	if (pAddrDllPath) VirtualFreeEx(hProcess, pAddrDllPath, 0, MEM_RELEASE);
	CloseHandle(hProcess);
	// and return
	return hModuleRemoteDll;
}
#endif

