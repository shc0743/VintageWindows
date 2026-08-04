// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include <windows.h>
#include <uxtheme.h>
#include <map>
#include <mutex>
#pragma comment(lib,"uxtheme.lib")
using namespace std;

map<DWORD, HHOOK> hooks;
recursive_mutex mylock;

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    std::lock_guard gg(mylock);
    DWORD tid = GetCurrentThreadId();
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        SetThemeAppProperties(0);
        break;
    case DLL_THREAD_ATTACH:
    {
        HHOOK hHook = SetWindowsHookExW(WH_CBT, [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HCBT_CREATEWND || nCode == HCBT_ACTIVATE) {
                HWND hWnd = (HWND)wParam;
                SetWindowTheme(hWnd, L"", L"");
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }, nullptr, GetCurrentThreadId());
        if (hHook) hooks.insert(make_pair(tid, hHook));
    }
        break;
    case DLL_THREAD_DETACH:
        if (hooks.contains(tid)) {
            UnhookWindowsHookEx(hooks.at(tid));
            hooks.erase(tid);
        }
        break;
    case DLL_PROCESS_DETACH:
        SetThemeAppProperties(STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS | STAP_ALLOW_WEBCONTENT);
        break;
    }
    return TRUE;
}

