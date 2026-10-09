#include "crashlog.h"

#include <windows.h>
#include <vector>
#include <atomic>

extern "C" void monitor_on_changed(int from_idx, int to_idx, uint64_t hmon);

static std::atomic<uint64_t> g_current_hmon{0};
static std::atomic<int> g_current_idx{-1};
static std::vector<HMONITOR> g_monitors;
static HWND g_display_hwnd = nullptr;
static const UINT_PTR kPollTimer = 1;

static int index_of_monitor(HMONITOR h) {
	for (size_t i = 0; i < g_monitors.size(); i++) {
		if (g_monitors[i] == h) return static_cast<int>(i);
	}
	return -1;
}

static BOOL CALLBACK enum_proc(HMONITOR h, HDC, LPRECT, LPARAM) {
	g_monitors.push_back(h);
	return TRUE;
}

static void refresh_monitors(void) {
	g_monitors.clear();
	EnumDisplayMonitors(nullptr, nullptr, enum_proc, 0);
}

static void poll_cursor(void) {
	POINT pt{};
	if (!GetCursorPos(&pt)) return;
	HMONITOR h = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	int to = index_of_monitor(h);
	if (to < 0) {
		refresh_monitors();
		to = index_of_monitor(h);
	}
	if (to < 0) return;
	const uint64_t prev = g_current_hmon.load();
	const int from = g_current_idx.load();
	const uint64_t now = reinterpret_cast<uint64_t>(h);
	if (prev == now && from == to) return;
	g_current_hmon.store(now);
	g_current_idx.store(to);
	if (prev != 0 && from != to) {
		monitor_on_changed(from, to, now);
	}
}

static LRESULT CALLBACK display_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_DISPLAYCHANGE) {
		refresh_monitors();
		poll_cursor();
	} else if (msg == WM_TIMER && wp == kPollTimer) {
		poll_cursor();
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int monitor_tracker_start(void) {
	refresh_monitors();
	poll_cursor();

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = display_wnd_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"AutoScreenRecorderDisplayNotify";
	RegisterClassExW(&wc);
	g_display_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	if (!g_display_hwnd) {
		crash_log_trace("monitor tracker: hidden window failed");
		return -1;
	}
	if (!SetTimer(g_display_hwnd, kPollTimer, 50, nullptr)) {
		crash_log_trace("monitor tracker: timer failed");
		return -1;
	}
	return 0;
}

void monitor_tracker_stop(void) {
	if (g_display_hwnd) {
		KillTimer(g_display_hwnd, kPollTimer);
		DestroyWindow(g_display_hwnd);
		g_display_hwnd = nullptr;
	}
}

extern "C" uint64_t monitor_tracker_current(void) {
	return g_current_hmon.load();
}
