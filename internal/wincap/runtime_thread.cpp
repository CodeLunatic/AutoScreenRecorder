#include <windows.h>
#include <atomic>

static HWND g_msg_hwnd = nullptr;
static std::atomic<bool> g_runtime_running{false};
static HANDLE g_thread = nullptr;
static DWORD g_thread_id = 0;

static LRESULT CALLBACK hidden_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI runtime_thread_main(LPVOID) {
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) {
		return 1;
	}

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = hidden_wnd_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"AutoScreenRecorderHidden";
	RegisterClassExW(&wc);

	g_msg_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

	extern int monitor_tracker_start(void);
	monitor_tracker_start();

	MSG msg;
	while (g_runtime_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (g_msg_hwnd) {
		DestroyWindow(g_msg_hwnd);
		g_msg_hwnd = nullptr;
	}
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	return 0;
}

HWND native_runtime_hwnd(void) {
	return g_msg_hwnd;
}

DWORD native_runtime_thread_id(void) {
	return g_thread_id;
}

int native_runtime_start(void) {
	if (g_runtime_running.load()) {
		return 0;
	}
	g_runtime_running.store(true);
	g_thread = CreateThread(nullptr, 0, runtime_thread_main, nullptr, 0, &g_thread_id);
	if (!g_thread) {
		g_runtime_running.store(false);
		return -1;
	}
	for (int i = 0; i < 200; i++) {
		if (g_msg_hwnd) {
			return 0;
		}
		Sleep(10);
	}
	return -2;
}

void native_runtime_stop(void) {
	g_runtime_running.store(false);
	if (g_msg_hwnd) {
		PostMessageW(g_msg_hwnd, WM_QUIT, 0, 0);
	}
	if (g_thread) {
		WaitForSingleObject(g_thread, 5000);
		CloseHandle(g_thread);
		g_thread = nullptr;
	}
}
