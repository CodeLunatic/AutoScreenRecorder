#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void crash_log_init(const wchar_t *path);
void crash_log_trace(const char *msg);

#ifdef __cplusplus
}
#endif
