#include "../../w32oop/w32use.hpp"
#include <commctrl.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <Shlobj.h>
#include <fstream>
#include <deque>
#include "inject.h"
#pragma comment(lib, "uxtheme.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
	name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
	processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace std;

bool running;
HANDLE hUpdateEvent;
bool isEnabled;
set<wstring> targets;
set<wstring> exclude_targets;
set<DWORD> has_disabled_visual_styles_pid;
HWINEVENTHOOK g_hEventHook;
LPTHREAD_START_ROUTINE setthemeappptr;
bool matchAny;
const time_t has_disabled_visual_styles_pid_max_age = 300;
time_t has_disabled_visual_styles_pid_fresh = 0;
struct taskinfo {
	int tasktype;
	char taskdata[256 - sizeof(int)]{};
};
deque<taskinfo> tasks;
recursive_mutex tasks_lock;

#pragma region 豆包

using namespace std::chrono;

static std::map<DWORD, std::wstring> g_pidMap;
static std::mutex g_mtx;
static steady_clock::time_point g_lastCacheUpdate;
static bool g_refreshInProgress = false;

constexpr milliseconds g_cacheMaxAge{ 10000 };
constexpr milliseconds g_minRefreshGap{ 200 };

// 一次性全量快照，填充缓存
static void RefreshPidCache()
{
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE)
		return;

	std::map<DWORD, std::wstring> tmpMap;
	PROCESSENTRY32 pe{};
	pe.dwSize = sizeof(PROCESSENTRY32);

	if (Process32First(hSnap, &pe))
	{
		do
		{
			tmpMap[pe.th32ProcessID] = pe.szExeFile;
		} while (Process32Next(hSnap, &pe));
	}
	CloseHandle(hSnap);

	std::lock_guard<std::mutex> lk(g_mtx);
	g_pidMap.swap(tmpMap);
	g_lastCacheUpdate = steady_clock::now();
	g_refreshInProgress = false;
}

// 直接返回 wstring；找不到返回空wstring
std::wstring GetProcessNameByPid(DWORD pid)
{
	if (pid == 0)
		return L"";

	auto now = steady_clock::now();

	{
		std::lock_guard<std::mutex> lk(g_mtx);
		auto elapsed = now - g_lastCacheUpdate;
		// 未过期且pid存在缓存，直接返回
		if (elapsed < g_cacheMaxAge && g_pidMap.contains(pid))
		{
			return g_pidMap[pid];
		}
		// 正在刷新中，直接返回空，避免雪崩式重复快照
		if (g_refreshInProgress)
		{
			return L"";
		}
		// 防抖动：距离上一次刷新太近，也不再触发
		if (now - g_lastCacheUpdate < g_minRefreshGap)
		{
			return L"";
		}
		g_refreshInProgress = true;
	}

	// 释放锁再做耗时快照IO
	RefreshPidCache();

	// 刷新完读取结果
	std::lock_guard<std::mutex> lk(g_mtx);
	auto it = g_pidMap.find(pid);
	if (it != g_pidMap.end())
		return it->second;
	return L"";
}

#pragma endregion


struct mpwdata { HWND hwnd; HANDLE cb; };
vector<mpwdata> needpost;
volatile bool mpwisreading = false;
volatile bool mpwiswriting = false;
HANDLE mpwevent;

DWORD WINAPI messageposterworker(PVOID) {
	while (running) {
		vector<mpwdata> copy;
		while (mpwiswriting) Sleep(1);
		mpwisreading = true;
		{
			copy = needpost;
			needpost.clear();
		}
		mpwisreading = false;
		for (auto& i : copy) {
			SetWindowTheme(i.hwnd, L"", L"");
			if (i.cb) SetEvent(i.cb);
		}
		WaitForSingleObject(mpwevent, INFINITE);
	}
	return 0;
}

#pragma warning(push)
#pragma warning(disable: 6258)
DWORD WINAPI worker(PVOID) {
	mpwevent = CreateEventW(0, 0, 0, 0);
	if (!mpwevent) __fastfail(2);
	HANDLE hmpw = CreateThread(0, 0, messageposterworker, 0, 0, 0);
	if (!hmpw) __fastfail(2);

	while (running) {
		if (tasks.size()) {
			deque<taskinfo> copy;
			{
				lock_guard gg(tasks_lock);
				copy = tasks;
				tasks.clear();
			}
			for (auto& i : copy) {
				switch (i.tasktype) {
				case 1:
				{
					// set theme
					HWND hwnd{};
					memcpy(&hwnd, i.taskdata, sizeof(hwnd));
					HANDLE cb = CreateEventW(0, 1, 0, 0);
					if (!cb) __fastfail(2);
					while (mpwisreading) Sleep(1);
					mpwiswriting = true;
					mpwdata mpwd{ .hwnd = hwnd,.cb = cb };
					size_t prevlen = needpost.size();
					needpost.push_back(mpwd);
					mpwiswriting = false;
					SetEvent(mpwevent);
					if (WAIT_TIMEOUT == WaitForSingleObject(cb, 1000)) {
						// send message time out
						TerminateThread(hmpw, ERROR_TIMEOUT);
						CloseHandle(hmpw);
						if (prevlen < needpost.size()) needpost.pop_back();
						hmpw = CreateThread(0, 0, messageposterworker, 0, 0, 0);
						if (!hmpw) __fastfail(2);
						mpwisreading = mpwiswriting = false;
					}
					CloseHandle(cb);
				}
				break;
				}
			}
		}
		
		WaitForSingleObject(hUpdateEvent, INFINITE);
	}

	TerminateThread(hmpw, 0);
	CloseHandle(hmpw);
	return 0;
}
#pragma warning(pop)

void CALLBACK WinEventProc(
	HWINEVENTHOOK hWinEventHook,
	DWORD event,
	HWND hwnd,
	LONG idObject,
	LONG idChild,
	DWORD dwEventThread,
	DWORD dwmsEventTime
) {
	if (!hwnd || !isEnabled) return;
	switch (event) {
	case EVENT_OBJECT_CREATE:
	case EVENT_OBJECT_SHOW:
	{
		DWORD owner_pid{};
		GetWindowThreadProcessId(hwnd, &owner_pid);
		auto name = GetProcessNameByPid(owner_pid);
		if (name.empty()) break;
		if (!matchAny) {
			if (!targets.contains(name) && !targets.contains(to_wstring(owner_pid))) {
				break;
			}
		}
		else if (exclude_targets.contains(name) || exclude_targets.contains(to_wstring(owner_pid))) {
			break;
		}

		if (setthemeappptr && !has_disabled_visual_styles_pid.contains(owner_pid)) {
			// off the app
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, owner_pid);
			if (hProcess) {
				BOOL wow{}; IsWow64Process(hProcess, &wow);
				if (wow == false) {
					InjectDllToProcess_HANDLE(hProcess, L"Uxtheme.dll");
					HANDLE rem = CreateRemoteThread(hProcess, 0, 0, setthemeappptr, (PVOID)0, 0, 0);
					if (rem) CloseHandle(rem);
				}
				has_disabled_visual_styles_pid.insert(owner_pid);
				CloseHandle(hProcess);
			}
		}

		lock_guard gg(tasks_lock);
		taskinfo info{
			.tasktype = 1,
		};
		memcpy(info.taskdata, &hwnd, sizeof(hwnd));
		tasks.push_back(info);
		SetEvent(hUpdateEvent);
	}
		break;
	default:;
	}
}

class VintageWindowsLauncher : public Window {
public:
	VintageWindowsLauncher() : Window(L"", 640, 480, 0, 0, WS_OVERLAPPEDWINDOW) {}
protected:
	wstring appPath;
	CheckBox enabled;
	Edit processes;
	void onCreated() override {
		wstring title = L"Vintage Windows Launcher";
		text(title);
		if (IsUserAnAdmin()) {
			text(L"[Administrator] " + title);
		}

		auto buffer = make_unique<WCHAR[]>(32768);
		GetModuleFileNameW(GetModuleHandleW(NULL), buffer.get(), 32768);
		appPath = buffer.get();

		enabled.set_parent(this);
		enabled.create(L"Enable Vintage Windows", 1, 1);
		processes.set_parent(this);
		processes.create(L"# Enter process name or id here, one row one process\r\n"
			L"# Input only \"*\" to match ALL processes on your computer! (Be careful!)\r\n"
			L"# Use !<process> to exclude a process! \r\n# (If process name includes ! or = please use \"=<process>\")\r\n"
			L"# If some program seems not to be affected please run as admin\r\n\r\n"
			L"# Write your own rules here!!\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n"
			L"# Default rules\r\n!csrss.exe\r\n!winlogon.exe\r\n!fontdrvhost.exe\r\n!dwm.exe\r\n!explorer.exe\r\n!svchost.exe\r\n!SearchHost.exe\r\n!msedgewebview2.exe\r\n!StartMenuExperienceHost.exe\r\n!Widgets.exe\r\n!RuntimeBroker.exe\r\n!TextInputHost.exe\r\n!ShellExperienceHost.exe\r\n!ApplicationFrameHost.exe\r\n!SystemSettings.exe\r\n"
			L"\r\n\r\n",
			1, 1, 0, 0, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_HSCROLL | WS_VSCROLL);

		ifstream fp(appPath + L".list.txt", ios::in | ios::binary);
		if (fp) {
			auto buffer = make_unique<char[]>(65537);
			fp.read((buffer.get()), 65536);
			processes.text(w32oop::util::str::encodings::utf8_utf16(buffer.get()));
		}

		enabled.onChanged([this](EventData& ev) {
			if (enabled.checked()) {
				processes.disable();
				targets.clear();
				exclude_targets.clear();
				wstring txt = processes.text();
				vector<wstring> dest;
				matchAny = false;
				w32oop::util::str::operations::split(txt, L"\r\n", dest);
				for (const auto& i : dest) {
					if (i.empty() || i.starts_with(L"#")) continue;
					auto firstChar = i[0];
					if (firstChar == L'*') matchAny = true;
					else if (firstChar == L'=') {
						if (i.length() < 2) continue;
						targets.insert(i.substr(1));
					}
					else if (firstChar == L'!') {
						if (i.length() < 2) continue;
						exclude_targets.insert(i.substr(1));
					}
					else targets.insert(i);
				}
				isEnabled = true;
				SetEvent(hUpdateEvent);
			}
			else {
				isEnabled = matchAny = false;
				targets.clear();
				exclude_targets.clear();
				SetEvent(hUpdateEvent);
				MessageBoxTimeoutW(hwnd, L"You might need to restart the impacted applications", L"Oh no!", MB_ICONWARNING, 0, 500);
				processes.enable();
			}
		});

		SetTimer(hwnd, 1, 10000, NULL);
	}
	void onResize(EventData&) {
		RECT rc{}; GetClientRect(hwnd, &rc);
		enabled.resize(0, 0, rc.right - rc.left, 30);
		processes.resize(0, 30, rc.right - rc.left, rc.bottom - rc.top - 30);
	}
	void onBeforeClose(EventData&) {
		ofstream fp(appPath + L".list.txt", ios::out | ios::binary);
		if (fp) {
			wstring t = processes.text();
			string u8 = w32oop::util::str::encodings::utf16_utf8(t);
			fp.write(u8.data(), u8.size());
		}
	}
	void onTimer(EventData& ev) {
		switch (ev.wParam) {
		case 1:
		{
			time_t now = time(0);
			if (now - has_disabled_visual_styles_pid_fresh > has_disabled_visual_styles_pid_max_age) {
				has_disabled_visual_styles_pid.clear();
				has_disabled_visual_styles_pid_fresh = now;
			}
		}
			break;
		default:
			return;
		}
		ev.preventDefault();
	}
	virtual void setup_event_handlers() override {
		WINDOW_add_handler(WM_SIZING, onResize);
		WINDOW_add_handler(WM_SIZE, onResize);
		WINDOW_add_handler(WM_CLOSE, onBeforeClose);
		WINDOW_add_handler(WM_TIMER, onTimer);
	}
};

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ PWSTR pCmdLine,
	_In_ int nCmdShow
) {
	SetThemeAppProperties(1);
	HHOOK hHook = SetWindowsHookExW(WH_CBT, [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
		if (nCode == HCBT_CREATEWND || nCode == HCBT_ACTIVATE) {
			HWND hWnd = (HWND)wParam;
			SetWindowTheme(hWnd, L"", L"");
		}
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	}, nullptr, GetCurrentThreadId());
	// Running your app
	if (HWND prev = FindWindowW(VintageWindowsLauncher().get_class_name().c_str(), NULL)) {
		const auto focus = [prev] {
			SetForegroundWindow(prev);
			return 0;
		};

		HWND fg = GetForegroundWindow();
		if (!fg) return focus(); // fallback to normal focus
		DWORD pid = 0;
		GetWindowThreadProcessId(fg, &pid);
		if (!pid || pid == GetCurrentProcessId()) return focus();
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (!user32) return focus();
		LPTHREAD_START_ROUTINE AllowSetForegroundWindow = (LPTHREAD_START_ROUTINE)GetProcAddress(user32, "AllowSetForegroundWindow");
		if (!AllowSetForegroundWindow) return focus();
		HANDLE hProcess = OpenProcess(
			PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
			PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
			FALSE, pid
		);
		if (!hProcess) return focus();
		HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, AllowSetForegroundWindow, (LPVOID)(size_t)GetCurrentProcessId(), CREATE_SUSPENDED, NULL);
		CloseHandle(hProcess);
		if (!hThread) return focus();
		ResumeThread(hThread);
		WaitForSingleObject(hThread, 1000);
		DWORD exitCode = 0;
		GetExitCodeThread(hThread, &exitCode);
		CloseHandle(hThread);
		return focus();
	}
	HMODULE ux = LoadLibraryW(L"Uxtheme.dll");
	if (ux) {
		setthemeappptr = (LPTHREAD_START_ROUTINE)GetProcAddress(ux, "SetThemeAppProperties");
	}
	g_hEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
	hUpdateEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!hUpdateEvent) __fastfail(2);
	running = true;
	HANDLE hThread = CreateThread(0, 0, worker, 0, 0, 0);
	if (!hThread) {
		MessageBoxW(0, ErrorChecker().message().c_str(), 0, MB_ICONERROR);
		return GetLastError();
	}
	VintageWindowsLauncher app;
	app.create();
	app.set_main_window();
	app.center();
	app.show(nCmdShow);
	int value = app.run();
	running = false;
	SetEvent(hUpdateEvent);
	WaitForSingleObject(hThread, INFINITE);
	CloseHandle(hThread);
	CloseHandle(hUpdateEvent);
	return value;
}
