#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*native_mic_cb)(int event, uint32_t pid);
typedef void (*native_monitor_cb)(int from_idx, int to_idx, uint64_t hmon);

typedef struct native_rec_config {
	int fps;
	int follow_mouse;
	int video_bitrate_kbps;
	int encode_max_width;
	int encode_max_height;
	int use_wmv;
	int audio_enabled;
	int system_enabled;
	int mic_enabled;
	float gain_system_db;
	float gain_mic_db;
	int clip_limit;
} native_rec_config;

int native_start(native_mic_cb mic, native_monitor_cb mon);
void native_stop(void);

uint64_t native_current_monitor(void);

int native_start_recording(const wchar_t *path, uint64_t hmon, const native_rec_config *cfg);
void native_set_monitor(uint64_t hmon);
void native_stop_recording(void);
int native_finalize_pending(void);
int native_last_video_frames(void);
long native_last_mf_hresult(void);
void native_last_encode_size(int *w, int *h);

int native_merge_videos(const wchar_t *out_path, const wchar_t **inputs, int count);

void native_init_crash_log(const wchar_t *path);
void native_crash_log_trace(const char *msg);

#ifdef __cplusplus
}
#endif
