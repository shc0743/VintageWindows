#pragma once
#ifndef _WIN32
#error This is a inject header FOR WINDOWS
#endif
// Windows headers
#ifndef _WINDOWS_
#include<Windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
@brief Inject a DLL file into a process (Unicode)
@param dwProcessId The Process ID to inject.
@param szDllPath The path of the dll.
@param dwTimeout The timeout waiting load finished.
@return Success - the handle of the dll in the process.  Failed - NULL or -1.\
		NULL: Use GetLastError to learn more. -1: timeout
*/
HMODULE InjectDllToProcess(DWORD dwProcessId, LPCWSTR szDllPath, DWORD dwTimeout
#ifdef __cplusplus
	= INFINITE
#endif
);

/**
@brief Inject a DLL file into a process (Unicode) (HANDLE)
@param hProcess The Process handle to inject.
@param szDllPath The path of the dll.
@param dwTimeout The timeout waiting load finished.
@return Success - the handle of the dll in the process.  Failed - NULL or -1.\
		NULL: Use GetLastError to learn more. -1: timeout
*/
HMODULE InjectDllToProcess_HANDLE(HANDLE hProcess, LPCWSTR szDllPath, DWORD dwTimeout
#ifdef __cplusplus
	= INFINITE
#endif
);

/**
@brief Inject shellcode into a process (Unicode)
@param hProcess The process handle to inject.
@param pCodeAddress  shellcode start address
@param size  shellcode size
@param dwTimeout The timeout waiting load finished.
@return Success - the return value of shellcode.  Failed - DWORD(-1).
*/
DWORD InjectCodeToProcess(
	HANDLE hProcess, PVOID pCodeAddress, SIZE_T size, PVOID paramter, DWORD dwTimeout
#ifdef __cplusplus
	= INFINITE
#endif
);

#if 0
@trashed
	/**
	@brief Get Remote Process Function Address
	*/
#ifdef __cplusplus
	extern "C"
#endif
	FARPROC GetRemoteProcAddress(DWORD dwProcessId, HMODULE hModule, LPCSTR lpProcName);
#endif


#ifdef __cplusplus
}
#endif

