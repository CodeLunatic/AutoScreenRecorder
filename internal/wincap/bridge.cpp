#include "bridge.h"
#include "crashlog.h"
#include <windows.h>
#include <objbase.h>

extern int native_runtime_start(void);
extern void native_runtime_stop(void);
extern int wasapi_trigger_start(void);
extern void wasapi_trigger_stop(void);
extern int monitor_tracker_start(void);
extern void monitor_tracker_stop(void);
extern int recorder_start(const wchar_t *path, uint64_t hmon, const native_rec_config *cfg);
extern void recorder_set_monitor(uint64_t hmon);
extern void recorder_stop(void);
extern void recorder_shutdown(void);
extern int native_last_video_frames(void);
extern int merge_mp4_files(const wchar_t *out_path, const wchar_t **inputs, int count);

static native_mic_cb g_mic_cb = nullptr;
static native_monitor_cb g_mon_cb = nullptr;
static bool g_com_owned = false;

extern "C" void wasapi_on_session_active(uint32_t pid) {
	if (g_mic_cb) g_mic_cb(1, pid);
}

extern "C" void wasapi_on_session_inactive(uint32_t pid) {
	if (g_mic_cb) g_mic_cb(2, pid);
}

extern "C" void monitor_on_changed(int from_idx, int to_idx, uint64_t hmon) {
	if (g_mon_cb) g_mon_cb(from_idx, to_idx, hmon);
}

static void enable_dpi_awareness(void) {
	using Fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
	auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
	if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int native_start(native_mic_cb mic, native_monitor_cb mon) {
	g_mic_cb = mic;
	g_mon_cb = mon;
	enable_dpi_awareness();
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (hr == RPC_E_CHANGED_MODE) {
		g_com_owned = false;
	} else if (FAILED(hr)) {
		return -1;
	} else {
		g_com_owned = true;
	}
	if (native_runtime_start() != 0) {
		return -2;
	}
	if (wasapi_trigger_start() != 0) {
		return -3;
	}
	return 0;
}

void native_stop(void) {
	recorder_shutdown();
	wasapi_trigger_stop();
	monitor_tracker_stop();
	native_runtime_stop();
	g_mic_cb = nullptr;
	g_mon_cb = nullptr;
	if (g_com_owned) {
		CoUninitialize();
		g_com_owned = false;
	}
}

extern "C" uint64_t monitor_tracker_current(void);

uint64_t native_current_monitor(void) {
	return monitor_tracker_current();
}

int native_start_recording(const wchar_t *path, uint64_t hmon, const native_rec_config *cfg) {
	return recorder_start(path, hmon, cfg);
}

void native_set_monitor(uint64_t hmon) {
	recorder_set_monitor(hmon);
}

void native_stop_recording(void) {
	recorder_stop();
}

extern int recorder_finalize_pending(void);

int native_finalize_pending(void) {
	return recorder_finalize_pending();
}

void native_init_crash_log(const wchar_t *path) {
	crash_log_init(path);
}

void native_crash_log_trace(const char *msg) {
	crash_log_trace(msg);
}

int native_merge_videos(const wchar_t *out_path, const wchar_t **inputs, int count) {
	return merge_mp4_files(out_path, inputs, count);
}
