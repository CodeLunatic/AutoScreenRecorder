#include "crashlog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>

static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_log_mutex = nullptr;

static bool lock_log(DWORD wait_ms) {
	if (!g_log_mutex) return true;
	const DWORD wr = WaitForSingleObject(g_log_mutex, wait_ms);
	return wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED;
}

static void unlock_log(bool owned) {
	if (owned && g_log_mutex) ReleaseMutex(g_log_mutex);
}

static void log_write_raw(const char *data, size_t len) {
	if (g_log == INVALID_HANDLE_VALUE || !data || len == 0) return;
	DWORD written = 0;
	WriteFile(g_log, data, static_cast<DWORD>(len), &written, nullptr);
}

static void log_write_line_unlocked(const char *msg) {
	if (!msg) return;
	SYSTEMTIME st{};
	GetLocalTime(&st);
	char prefix[80];
	snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u [native] ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	log_write_raw(prefix, strlen(prefix));
	log_write_raw(msg, strlen(msg));
	log_write_raw("\r\n", 2);
	if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log);
}

static void log_stack_unlocked(const char *tag) {
	void *frames[48];
	const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
	char hdr[128];
	snprintf(hdr, sizeof(hdr), "%s stack (%hu frames):", tag, n);
	log_write_line_unlocked(hdr);
	for (USHORT i = 0; i < n; i++) {
		char line[64];
		snprintf(line, sizeof(line), "  #%hu %p", i, frames[i]);
		log_write_line_unlocked(line);
	}
}

static void log_exception(EXCEPTION_POINTERS *ep) {
	if (!ep || !ep->ExceptionRecord) {
		log_write_line_unlocked("unhandled exception (no details)");
		return;
	}
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	void *addr = ep->ExceptionRecord->ExceptionAddress;
	char line[256];
	snprintf(line, sizeof(line), "unhandled exception code=0x%08lX at %p thread=%lu",
		static_cast<unsigned long>(code), addr, GetCurrentThreadId());
	log_write_line_unlocked(line);
	log_stack_unlocked("exception");
}

static LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *ep) {
	const bool owned = lock_log(0);
	log_write_line_unlocked("=== UNHANDLED SEH EXCEPTION ===");
	log_exception(ep);
	unlock_log(owned);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void on_terminate(void) {
	const bool owned = lock_log(0);
	log_write_line_unlocked("=== std::terminate ===");
	log_stack_unlocked("terminate");
	unlock_log(owned);
	std::abort();
}

void crash_log_init(const wchar_t *path) {
	if (!path || g_log != INVALID_HANDLE_VALUE) return;
	g_log_mutex = CreateMutexW(nullptr, FALSE, L"Local\\AutoScreenRecorder_LogWrite");
	g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (g_log == INVALID_HANDLE_VALUE) return;
	SetFilePointer(g_log, 0, nullptr, FILE_END);
	SetUnhandledExceptionFilter(unhandled_exception_filter);
	std::set_terminate(on_terminate);
	char hdr[128];
	snprintf(hdr, sizeof(hdr), "===== native crash log pid=%lu =====", GetCurrentProcessId());
	if (lock_log(1000)) {
		log_write_line_unlocked(hdr);
		unlock_log(true);
	}
}

void crash_log_trace(const char *msg) {
	if (!lock_log(1000)) return;
	log_write_line_unlocked(msg);
	unlock_log(true);
}
