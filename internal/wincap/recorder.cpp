#define COBJMACROS
#include "bridge.h"
#include "crashlog.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <timeapi.h>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const GUID kSubTypeIeeeFloat = {
	0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubTypePCM = {
	0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

extern "C" const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = kSubTypeIeeeFloat;

static bool guid_equal(const GUID &a, const GUID &b) {
	return memcmp(&a, &b, sizeof(GUID)) == 0;
}

static bool is_pcm_format(const WAVEFORMATEX *wfx) {
	if (!wfx) return false;
	if (wfx->wFormatTag == WAVE_FORMAT_PCM) return true;
	if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const auto *ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wfx);
		return guid_equal(ex->SubFormat, kSubTypePCM);
	}
	return false;
}

static std::atomic<bool> g_rec_active{false};
static std::thread g_video_thread;
static std::thread g_audio_thread;
static std::mutex g_rec_mu;
struct PendingAudio {
	std::vector<int16_t> pcm;
	UINT32 frames = 0;
	LONGLONG pts = 0;
};
static std::mutex g_audio_q_mu;
static std::deque<PendingAudio> g_audio_q;
static LONGLONG g_audio_write_pts = 0;
static std::mutex g_sess_mu;
static std::condition_variable g_sess_cv;
static std::atomic<bool> g_session_done{true};
static std::atomic<bool> g_encoder_shutdown{false};
static std::atomic<bool> g_encoder_thread_running{false};
static std::atomic<bool> g_finalize_ok{false};
static std::atomic<bool> g_audio_capture_done{true};
static IMFSinkWriter *g_writer = nullptr;
static DWORD g_v_stream = 0;
static DWORD g_a_stream = 0;
static std::atomic<bool> g_with_audio{false};
static std::atomic<int> g_fail_code{0};
static std::mutex g_life_mu;
static std::mutex g_fin_mu;
static std::condition_variable g_fin_cv;
static bool g_fin_busy = false;
static std::atomic<bool> g_timer_period{false};
static float g_limit_gain = 1.f;
static std::atomic<long> g_last_mf_hr{0};
static std::wstring g_final_path;
static std::wstring g_write_path;
static native_rec_config g_cfg{};
static std::atomic<bool> g_writer_ready{false};
static std::atomic<int> g_video_frames{0};
static std::atomic<LONGLONG> g_last_video_ts{0};
static std::atomic<LONGLONG> g_last_audio_ts{0};
static std::atomic<uint64_t> g_hmon{0};
static bool g_mf_started = false;
static int g_canvas_w = 0;
static int g_canvas_h = 0;
static std::atomic<int> g_last_enc_w{0};
static std::atomic<int> g_last_enc_h{0};
static bool g_wmv_input_iyuv = false;
static bool g_h264_nv12 = false;
static int g_encode_audio_rate_hz = 0;

static std::wstring temp_recording_path(const std::wstring &final_mp4) {
	const auto dot = final_mp4.rfind(L'.');
	if (dot == std::wstring::npos) return final_mp4 + L".tmp.mp4";
	return final_mp4.substr(0, dot) + L".tmp" + final_mp4.substr(dot);
}

static int even_dim(int v) {
	if (v <= 1) return 2;
	return v & ~1;
}

static void cap_encode_size(int *w, int *h) {
	const int max_w = g_cfg.encode_max_width;
	const int max_h = g_cfg.encode_max_height;
	if (!w || !h || *w <= 0 || *h <= 0) return;
	if (max_w <= 0 && max_h <= 0) return;
	double sx = 1.0;
	double sy = 1.0;
	if (max_w > 0 && *w > max_w) sx = static_cast<double>(max_w) / *w;
	if (max_h > 0 && *h > max_h) sy = static_cast<double>(max_h) / *h;
	const double s = sx < sy ? sx : sy;
	if (s >= 1.0) return;
	*w = even_dim(static_cast<int>(*w * s));
	*h = even_dim(static_cast<int>(*h * s));
}

static int audio_sample_rate_hz(void) {
	return 48000;
}

static int audio_bitrate_bytes_per_sec(void) {
	return 320 * 1000 / 8;
}

static int effective_audio_sample_rate_hz(void) {
	if (g_encode_audio_rate_hz > 0) return g_encode_audio_rate_hz;
	return audio_sample_rate_hz();
}

static UINT32 video_bitrate_bps(void) {
	const int kbps = g_cfg.video_bitrate_kbps > 0 ? g_cfg.video_bitrate_kbps : 400;
	return static_cast<UINT32>(kbps) * 1000U;
}

static void rgb32_to_iyuv(const BYTE *rgb, int width, int height, int rgb_stride, std::vector<BYTE> *iyuv) {
	const int y_size = width * height;
	const int uv_w = width / 2;
	const int uv_h = height / 2;
	iyuv->assign(static_cast<size_t>(y_size + uv_w * uv_h * 2), 0);
	BYTE *y_plane = iyuv->data();
	BYTE *u_plane = y_plane + y_size;
	BYTE *v_plane = u_plane + uv_w * uv_h;

	for (int row = 0; row < height; row++) {
		const BYTE *src = rgb + static_cast<size_t>(row) * rgb_stride;
		for (int col = 0; col < width; col++) {
			const int b = src[col * 4 + 0];
			const int g = src[col * 4 + 1];
			const int r = src[col * 4 + 2];
			const int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			y_plane[row * width + col] = static_cast<BYTE>(Y < 0 ? 0 : (Y > 255 ? 255 : Y));
			if ((row & 1) == 0 && (col & 1) == 0) {
				const int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
				const int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
				const int uv_row = row / 2;
				const int uv_col = col / 2;
				u_plane[uv_row * uv_w + uv_col] = static_cast<BYTE>(U < 0 ? 0 : (U > 255 ? 255 : U));
				v_plane[uv_row * uv_w + uv_col] = static_cast<BYTE>(V < 0 ? 0 : (V > 255 ? 255 : V));
			}
		}
	}
}

static BYTE clamp_byte(int v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return static_cast<BYTE>(v);
}

// 屏幕像素是 B,G,R,X。按 BT.709 转成电视范围 NV12，避免编码器自己转换时整幅偏粉。
static void bgr32_to_nv12(const BYTE *bgr, int width, int height, int stride, std::vector<BYTE> *nv12) {
	const int y_size = width * height;
	nv12->assign(static_cast<size_t>(y_size) * 3 / 2, 0);
	BYTE *y_plane = nv12->data();
	BYTE *uv_plane = y_plane + y_size;
	for (int row = 0; row < height; row += 2) {
		const BYTE *row0 = bgr + static_cast<size_t>(row) * stride;
		const BYTE *row1 = row + 1 < height ? row0 + stride : row0;
		BYTE *y0 = y_plane + static_cast<size_t>(row) * width;
		BYTE *y1 = y_plane + static_cast<size_t>(row + (row + 1 < height ? 1 : 0)) * width;
		BYTE *uv = uv_plane + static_cast<size_t>(row / 2) * width;
		for (int col = 0; col < width; col += 2) {
			int cb_acc = 0;
			int cr_acc = 0;
			for (int dy = 0; dy < 2; dy++) {
				const BYTE *src = dy == 0 ? row0 : row1;
				BYTE *yp = dy == 0 ? y0 : y1;
				for (int dx = 0; dx < 2; dx++) {
					const int x = col + dx < width ? col + dx : col;
					const BYTE *p = src + x * 4;
					const int b = p[0];
					const int g = p[1];
					const int r = p[2];
					yp[x] = clamp_byte(((11966 * r + 40270 * g + 4064 * b + 32768) >> 16) + 16);
					cb_acc += (-6596 * r - 22189 * g + 28784 * b + 32768) >> 16;
					cr_acc += (28784 * r - 26145 * g - 2639 * b + 32768) >> 16;
				}
			}
			uv[col] = clamp_byte(cb_acc / 4 + 128);
			uv[col + 1] = clamp_byte(cr_acc / 4 + 128);
		}
	}
}

static LARGE_INTEGER g_qpc_freq{};
static LARGE_INTEGER g_qpc_start{};

static void reset_stream_indices(void) {
	g_v_stream = 0;
	g_a_stream = 0;
}

static LONGLONG qpc_delta_to_100ns(LONGLONG delta) {
	return delta * 10000000LL / g_qpc_freq.QuadPart;
}

static LONGLONG recording_clock_100ns(void) {
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	return qpc_delta_to_100ns(now.QuadPart - g_qpc_start.QuadPart);
}

static void mark_recording_clock_start(void) {
	QueryPerformanceFrequency(&g_qpc_freq);
	QueryPerformanceCounter(&g_qpc_start);
}

static void get_monitor_rect(HMONITOR hmon, RECT *rc) {
	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(hmon, &mi)) {
		*rc = mi.rcMonitor;
		return;
	}
	SetRect(rc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

static HMONITOR monitor_from_cursor(void) {
	POINT pt{};
	if (!GetCursorPos(&pt)) {
		return reinterpret_cast<HMONITOR>(g_hmon.load());
	}
	return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
}

static void monitor_capture_size(HMONITOR hmon, int *out_w, int *out_h) {
	RECT rc{};
	get_monitor_rect(hmon, &rc);
	*out_w = even_dim(rc.right - rc.left);
	*out_h = even_dim(rc.bottom - rc.top);
}

static void calc_capture_size(int *out_w, int *out_h) {
	if (!g_cfg.follow_mouse) {
		HMONITOR hmon = reinterpret_cast<HMONITOR>(g_hmon.load());
		if (!hmon) hmon = monitor_from_cursor();
		monitor_capture_size(hmon, out_w, out_h);
		return;
	}
	// 成片分辨率固定为面积最大的那块屏幕。较小的屏等比例缩放后居中，只在上下或左右留黑边。
	int best_area = 0;
	int best_w = 0;
	int best_h = 0;
	auto cb = [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfoW(h, &mi)) return TRUE;
		const int w = mi.rcMonitor.right - mi.rcMonitor.left;
		const int hgt = mi.rcMonitor.bottom - mi.rcMonitor.top;
		if (w <= 0 || hgt <= 0) return TRUE;
		const int area = w * hgt;
		int *best = reinterpret_cast<int *>(lp);
		if (area > best[0]) {
			best[0] = area;
			best[1] = w;
			best[2] = hgt;
		}
		return TRUE;
	};
	int best[3] = {0, 0, 0};
	EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(best));
	best_area = best[0];
	best_w = best[1];
	best_h = best[2];
	if (best_area <= 0) {
		HMONITOR hmon = reinterpret_cast<HMONITOR>(g_hmon.load());
		if (!hmon) hmon = monitor_from_cursor();
		monitor_capture_size(hmon, out_w, out_h);
		return;
	}
	*out_w = even_dim(best_w);
	*out_h = even_dim(best_h);
}

static void calc_encode_size(int cap_w, int cap_h, int *out_w, int *out_h) {
	*out_w = cap_w;
	*out_h = cap_h;
	cap_encode_size(out_w, out_h);
}

static bool scale_rgb32_frame(const BYTE *src, int sw, int sh, std::vector<BYTE> *dst, int dw, int dh) {
	if (sw == dw && sh == dh) {
		dst->assign(src, src + static_cast<size_t>(sw) * sh * 4);
		return true;
	}
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;

	dst->assign(static_cast<size_t>(dw) * dh * 4, 0);
	HDC screen = GetDC(nullptr);
	HDC src_dc = CreateCompatibleDC(screen);
	HDC dst_dc = CreateCompatibleDC(screen);
	HBITMAP src_bmp = CreateCompatibleBitmap(screen, sw, sh);
	HBITMAP dst_bmp = CreateCompatibleBitmap(screen, dw, dh);
	HGDIOBJ old_src = SelectObject(src_dc, src_bmp);
	HGDIOBJ old_dst = SelectObject(dst_dc, dst_bmp);

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = sw;
	bi.bmiHeader.biHeight = -sh;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	SetDIBits(src_dc, src_bmp, 0, sh, src, &bi, DIB_RGB_COLORS);

	SetStretchBltMode(dst_dc, HALFTONE);
	StretchBlt(dst_dc, 0, 0, dw, dh, src_dc, 0, 0, sw, sh, SRCCOPY);

	BITMAPINFO bi_out = bi;
	bi_out.bmiHeader.biWidth = dw;
	bi_out.bmiHeader.biHeight = -dh;
	GetDIBits(dst_dc, dst_bmp, 0, dh, dst->data(), &bi_out, DIB_RGB_COLORS);

	SelectObject(src_dc, old_src);
	SelectObject(dst_dc, old_dst);
	DeleteObject(src_bmp);
	DeleteObject(dst_bmp);
	DeleteDC(src_dc);
	DeleteDC(dst_dc);
	ReleaseDC(nullptr, screen);
	return true;
}

static float db_to_linear(float db) {
	return static_cast<float>(pow(10.0, db / 20.0));
}

static void delete_file_if_exists(const wchar_t *path) {
	if (path && path[0]) DeleteFileW(path);
}

static uint64_t file_size_bytes(const wchar_t *path) {
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return 0;
	ULARGE_INTEGER sz{};
	sz.LowPart = info.nFileSizeLow;
	sz.HighPart = info.nFileSizeHigh;
	return sz.QuadPart;
}

static void clamp_audio(float *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (s[i] > 1.f) s[i] = 1.f;
		if (s[i] < -1.f) s[i] = -1.f;
	}
}

struct writer_create_opts {
	bool hw_accel;
	bool mpeg4_container;
	bool h264_extended_attrs;
};

struct wmv_attempt_cfg {
	GUID video_out;
	bool iyuv_in;
	bool asf_container;
	bool do_audio;
	bool wma_v9;
	int audio_hz;
	int fps_override;
};

static HRESULT new_media_type(IMFMediaType **out) {
	if (!out) return E_POINTER;
	*out = nullptr;
	const HRESULT hr = MFCreateMediaType(out);
	if (FAILED(hr)) return hr;
	if (!*out) return E_OUTOFMEMORY;
	return S_OK;
}

static HRESULT create_wmv_once(const wchar_t *path, int width, int height, int fps, bool with_audio, const wmv_attempt_cfg &ac, IMFSinkWriter **out) {
	reset_stream_indices();
	delete_file_if_exists(path);

	IMFSinkWriter *writer = nullptr;
	IMFAttributes *attrs = nullptr;
	MFCreateAttributes(&attrs, 4);
	if (attrs) {
		attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
		attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
		if (ac.asf_container) {
			attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_ASF);
		}
	}
	HRESULT hr = MFCreateSinkWriterFromURL(path, nullptr, attrs, &writer);
	if (attrs) attrs->Release();
	if (FAILED(hr)) {
		g_last_mf_hr.store(hr);
		return hr;
	}

	auto fail = [&](HRESULT e) -> HRESULT {
		g_last_mf_hr.store(e);
		if (writer) writer->Release();
		delete_file_if_exists(path);
		return e;
	};

	IMFMediaType *out_v = nullptr;
	hr = new_media_type(&out_v);
	if (FAILED(hr)) return fail(hr);
	out_v->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	out_v->SetGUID(MF_MT_SUBTYPE, ac.video_out);
	MFSetAttributeSize(out_v, MF_MT_FRAME_SIZE, width, height);
	MFSetAttributeRatio(out_v, MF_MT_FRAME_RATE, fps, 1);
	MFSetAttributeRatio(out_v, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	out_v->SetUINT32(MF_MT_AVG_BITRATE, video_bitrate_bps());
	out_v->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	hr = writer->AddStream(out_v, &g_v_stream);
	out_v->Release();
	if (FAILED(hr)) return fail(hr);

	const GUID in_sub = ac.iyuv_in ? MFVideoFormat_IYUV : MFVideoFormat_RGB32;
	IMFMediaType *in_v = nullptr;
	hr = new_media_type(&in_v);
	if (FAILED(hr)) return fail(hr);
	in_v->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	in_v->SetGUID(MF_MT_SUBTYPE, in_sub);
	MFSetAttributeSize(in_v, MF_MT_FRAME_SIZE, width, height);
	MFSetAttributeRatio(in_v, MF_MT_FRAME_RATE, fps, 1);
	if (ac.iyuv_in) {
		in_v->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
	} else {
		in_v->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
		in_v->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
	}
	hr = writer->SetInputMediaType(g_v_stream, in_v, nullptr);
	in_v->Release();
	if (FAILED(hr)) return fail(hr);

	g_with_audio.store(false);
	if (with_audio) {
		const int asr = ac.audio_hz > 0 ? ac.audio_hz : audio_sample_rate_hz();
		IMFMediaType *out_a = nullptr;
		hr = new_media_type(&out_a);
		if (FAILED(hr)) return fail(hr);
		out_a->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		const GUID wma = ac.wma_v9 ? MFAudioFormat_WMAudioV9 : MFAudioFormat_WMAudioV8;
		out_a->SetGUID(MF_MT_SUBTYPE, wma);
		out_a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		out_a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(asr));
		out_a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		{
			out_a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(audio_bitrate_bytes_per_sec()));
		}
		hr = writer->AddStream(out_a, &g_a_stream);
		out_a->Release();
		if (FAILED(hr)) return fail(hr);

		IMFMediaType *in_a = nullptr;
		hr = new_media_type(&in_a);
		if (FAILED(hr)) return fail(hr);
		in_a->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		in_a->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		in_a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		in_a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(asr));
		in_a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		in_a->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
		in_a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(asr * 4));
		hr = writer->SetInputMediaType(g_a_stream, in_a, nullptr);
		in_a->Release();
		if (FAILED(hr)) return fail(hr);
		g_with_audio.store(true);
	}

	hr = writer->BeginWriting();
	if (FAILED(hr)) return fail(hr);
	g_last_mf_hr.store(S_OK);
	*out = writer;
	return S_OK;
}

static HRESULT create_wmv_writer(const wchar_t *path, int width, int height, int fps, bool with_audio, IMFSinkWriter **out) {
	g_wmv_input_iyuv = false;
	g_encode_audio_rate_hz = 0;

	const wmv_attempt_cfg attempts[] = {
		{MFVideoFormat_WVC1, true, true, true, true, 48000, 0},
		{MFVideoFormat_WVC1, true, true, false, true, 0, 0},
		{MFVideoFormat_WVC1, true, false, true, true, 48000, 0},
		{MFVideoFormat_WVC1, true, false, false, true, 0, 0},
		{MFVideoFormat_WMV3, true, true, true, true, 48000, 0},
		{MFVideoFormat_WMV3, true, true, false, true, 0, 0},
		{MFVideoFormat_WMV3, false, true, true, true, 48000, 0},
		{MFVideoFormat_WMV3, false, true, false, true, 0, 0},
		{MFVideoFormat_WVC1, true, true, true, true, 48000, 30},
		{MFVideoFormat_WMV3, true, true, false, true, 0, 30},
	};

	for (const wmv_attempt_cfg &ac : attempts) {
		const int use_fps = ac.fps_override > 0 ? ac.fps_override : fps;
		const bool use_audio = with_audio && ac.do_audio;
		HRESULT hr = create_wmv_once(path, width, height, use_fps, use_audio, ac, out);
		if (FAILED(hr)) continue;
		g_wmv_input_iyuv = ac.iyuv_in;
		if (ac.audio_hz > 0) g_encode_audio_rate_hz = ac.audio_hz;
		return hr;
	}
	return E_FAIL;
}

static HRESULT create_h264_mp4_writer_internal(const wchar_t *path, int width, int height, int fps, bool with_audio, writer_create_opts opts, IMFSinkWriter **out) {
	reset_stream_indices();
	delete_file_if_exists(path);

	IMFSinkWriter *writer = nullptr;
	IMFAttributes *attrs = nullptr;
	MFCreateAttributes(&attrs, 4);
	if (attrs) {
		attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, opts.hw_accel ? TRUE : FALSE);
		attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
		if (opts.mpeg4_container) {
			attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
		}
	}
	HRESULT hr = MFCreateSinkWriterFromURL(path, nullptr, attrs, &writer);
	if (attrs) attrs->Release();
	if (FAILED(hr)) {
		g_last_mf_hr.store(hr);
		return hr;
	}

	auto fail = [&](HRESULT e) -> HRESULT {
		g_last_mf_hr.store(e);
		if (writer) writer->Release();
		delete_file_if_exists(path);
		return e;
	};

	IMFMediaType *out_v = nullptr;
	hr = new_media_type(&out_v);
	if (FAILED(hr)) return fail(hr);
	out_v->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	out_v->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	MFSetAttributeSize(out_v, MF_MT_FRAME_SIZE, width, height);
	MFSetAttributeRatio(out_v, MF_MT_FRAME_RATE, fps, 1);
	MFSetAttributeRatio(out_v, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	out_v->SetUINT32(MF_MT_AVG_BITRATE, video_bitrate_bps());
	out_v->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	out_v->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
	out_v->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
	out_v->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
	out_v->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
	out_v->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(fps > 0 ? fps * 5 : 75));
	if (opts.h264_extended_attrs) {
		out_v->SetUINT32(MF_MT_MPEG2_PROFILE, 77); // Main
		out_v->SetUINT32(MF_MT_MPEG2_LEVEL, 51);
		out_v->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
	}
	hr = writer->AddStream(out_v, &g_v_stream);
	out_v->Release();
	if (FAILED(hr)) return fail(hr);

	auto set_video_input = [&](bool nv12) -> HRESULT {
		IMFMediaType *in_v = nullptr;
		const HRESULT created = new_media_type(&in_v);
		if (FAILED(created)) return created;
		in_v->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		in_v->SetGUID(MF_MT_SUBTYPE, nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
		MFSetAttributeSize(in_v, MF_MT_FRAME_SIZE, width, height);
		MFSetAttributeRatio(in_v, MF_MT_FRAME_RATE, fps, 1);
		in_v->SetUINT32(MF_MT_DEFAULT_STRIDE, nv12 ? static_cast<UINT32>(width) : static_cast<UINT32>(width * 4));
		in_v->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, nv12 ? MFNominalRange_16_235 : MFNominalRange_0_255);
		in_v->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
		in_v->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
		in_v->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
		const HRESULT input_hr = writer->SetInputMediaType(g_v_stream, in_v, nullptr);
		in_v->Release();
		return input_hr;
	};
	hr = set_video_input(true);
	g_h264_nv12 = SUCCEEDED(hr);
	if (FAILED(hr)) hr = set_video_input(false);
	if (FAILED(hr)) return fail(hr);

	g_with_audio.store(false);
	if (with_audio) {
		IMFMediaType *out_a = nullptr;
		hr = new_media_type(&out_a);
		if (FAILED(hr)) return fail(hr);
		out_a->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		out_a->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
		out_a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		out_a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(audio_sample_rate_hz()));
		out_a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		{
			out_a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(audio_bitrate_bytes_per_sec()));
		}
		hr = writer->AddStream(out_a, &g_a_stream);
		out_a->Release();
		if (FAILED(hr)) {
			writer->Release();
			delete_file_if_exists(path);
			return create_h264_mp4_writer_internal(path, width, height, fps, false, opts, out);
		}
		IMFMediaType *in_a = nullptr;
		hr = new_media_type(&in_a);
		if (FAILED(hr)) return fail(hr);
		in_a->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		in_a->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		in_a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		{
			const UINT32 asr = static_cast<UINT32>(audio_sample_rate_hz());
			in_a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, asr);
			in_a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			in_a->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
			in_a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, asr * 4);
		}
		hr = writer->SetInputMediaType(g_a_stream, in_a, nullptr);
		in_a->Release();
		if (FAILED(hr)) {
			writer->Release();
			delete_file_if_exists(path);
			return create_h264_mp4_writer_internal(path, width, height, fps, false, opts, out);
		}
		g_with_audio.store(true);
	}

	hr = writer->BeginWriting();
	if (FAILED(hr)) return fail(hr);
	g_last_mf_hr.store(S_OK);
	*out = writer;
	return S_OK;
}

static HRESULT create_writer(const wchar_t *path, int width, int height, int fps, bool with_audio, IMFSinkWriter **out) {
	if (g_cfg.use_wmv) {
		g_wmv_input_iyuv = false;
		g_h264_nv12 = false;
		g_encode_audio_rate_hz = 0;
		HRESULT hr = create_wmv_writer(path, width, height, fps, with_audio, out);
		if (SUCCEEDED(hr)) return hr;
		return hr;
	}
	g_wmv_input_iyuv = false;
	g_h264_nv12 = false;
	g_encode_audio_rate_hz = 0;

	static const writer_create_opts attempts[] = {
		{true, true, false},
		{false, true, false},
		{false, true, true},
		{false, false, false},
	};
	HRESULT hr = E_FAIL;
	for (const writer_create_opts &opts : attempts) {
		hr = create_h264_mp4_writer_internal(path, width, height, fps, with_audio, opts, out);
		if (SUCCEEDED(hr)) return hr;
	}
	hr = create_h264_mp4_writer_internal(path, width, height, fps, false, attempts[3], out);
	if (SUCCEEDED(hr)) return hr;
	return hr;
}

static HRESULT write_video_frame(IMFSinkWriter *writer, const BYTE *pixels, LONG stride, int width, int height, LONGLONG ts100ns, LONGLONG dur100ns) {
	static thread_local std::vector<BYTE> iyuv_scratch;
	static thread_local std::vector<BYTE> nv12_scratch;
	const BYTE *frame = pixels;
	DWORD cb = static_cast<DWORD>(stride * height);
	const bool packed_yuv = g_wmv_input_iyuv || g_h264_nv12;
	if (g_wmv_input_iyuv) {
		rgb32_to_iyuv(pixels, width, height, stride, &iyuv_scratch);
		frame = iyuv_scratch.data();
		cb = static_cast<DWORD>(static_cast<size_t>(width) * height * 3 / 2);
	} else if (g_h264_nv12) {
		bgr32_to_nv12(pixels, width, height, stride, &nv12_scratch);
		frame = nv12_scratch.data();
		cb = static_cast<DWORD>(static_cast<size_t>(width) * height * 3 / 2);
	}

	IMFMediaBuffer *buf = nullptr;
	HRESULT hr = MFCreateMemoryBuffer(cb, &buf);
	if (FAILED(hr)) return hr;
	BYTE *dst = nullptr;
	buf->Lock(&dst, nullptr, nullptr);
	if (packed_yuv) {
		memcpy(dst, frame, cb);
	} else {
		for (int y = 0; y < height; y++) {
			memcpy(dst + static_cast<size_t>(y) * stride, frame + static_cast<size_t>(y) * stride, stride);
		}
	}
	buf->Unlock();
	buf->SetCurrentLength(cb);

	IMFSample *sample = nullptr;
	hr = MFCreateSample(&sample);
	if (FAILED(hr)) {
		buf->Release();
		return hr;
	}
	sample->AddBuffer(buf);
	buf->Release();
	sample->SetSampleTime(ts100ns);
	sample->SetSampleDuration(dur100ns);
	hr = writer->WriteSample(g_v_stream, sample);
	sample->Release();
	return hr;
}

static HRESULT write_audio_pcm(IMFSinkWriter *writer, const int16_t *pcm, DWORD frames, LONGLONG ts100ns) {
	DWORD cb = frames * 2 * sizeof(int16_t);
	IMFMediaBuffer *buf = nullptr;
	HRESULT hr = MFCreateMemoryBuffer(cb, &buf);
	if (FAILED(hr)) return hr;
	BYTE *dst = nullptr;
	buf->Lock(&dst, nullptr, nullptr);
	memcpy(dst, pcm, cb);
	buf->Unlock();
	buf->SetCurrentLength(cb);
	IMFSample *sample = nullptr;
	hr = MFCreateSample(&sample);
	if (FAILED(hr)) {
		buf->Release();
		return hr;
	}
	sample->AddBuffer(buf);
	buf->Release();
	sample->SetSampleTime(ts100ns);
	sample->SetSampleDuration((10000000LL * frames) / effective_audio_sample_rate_hz());
	hr = writer->WriteSample(g_a_stream, sample);
	sample->Release();
	return hr;
}

extern "C" uint64_t monitor_tracker_current(void);

static void fit_into_canvas(int src_w, int src_h, int canvas_w, int canvas_h, int *dst_x, int *dst_y, int *dst_w, int *dst_h) {
	int dw = src_w;
	int dh = src_h;
	if (src_w > 0 && src_h > 0 && canvas_w > 0 && canvas_h > 0) {
		dw = canvas_w;
		dh = static_cast<int>(std::llround(static_cast<double>(src_h) * canvas_w / src_w));
		if (dh > canvas_h) {
			dh = canvas_h;
			dw = static_cast<int>(std::llround(static_cast<double>(src_w) * canvas_h / src_h));
		}
	}
	if (dw > canvas_w) dw = canvas_w;
	if (dh > canvas_h) dh = canvas_h;
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	*dst_x = (canvas_w - dw) / 2;
	*dst_y = (canvas_h - dh) / 2;
	*dst_w = dw;
	*dst_h = dh;
}

static int scale_coord(int v, int src, int dst) {
	if (src <= 0) return 0;
	return static_cast<int>(std::llround(static_cast<double>(v) * dst / src));
}

static void draw_cursor_on_canvas(HDC mem, int canvas_w, int canvas_h, const RECT *capture_rc, int dst_x, int dst_y, int src_w, int src_h, int fit_w, int fit_h) {
	CURSORINFO ci{};
	ci.cbSize = sizeof(ci);
	if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;

	const bool identity = src_w == fit_w && src_h == fit_h;
	int draw_x = ci.ptScreenPos.x;
	int draw_y = ci.ptScreenPos.y;
	if (capture_rc) {
		draw_x = dst_x + scale_coord(ci.ptScreenPos.x - capture_rc->left, src_w, fit_w);
		draw_y = dst_y + scale_coord(ci.ptScreenPos.y - capture_rc->top, src_h, fit_h);
	} else {
		draw_x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
		draw_y -= GetSystemMetrics(SM_YVIRTUALSCREEN);
	}

	ICONINFO ii{};
	if (GetIconInfo(ci.hCursor, &ii)) {
		draw_x -= identity ? static_cast<int>(ii.xHotspot) : scale_coord(static_cast<int>(ii.xHotspot), src_w, fit_w);
		draw_y -= identity ? static_cast<int>(ii.yHotspot) : scale_coord(static_cast<int>(ii.yHotspot), src_h, fit_h);
		if (ii.hbmMask) DeleteObject(ii.hbmMask);
		if (ii.hbmColor) DeleteObject(ii.hbmColor);
	}

	int cur_w = GetSystemMetrics(SM_CXCURSOR);
	int cur_h = GetSystemMetrics(SM_CYCURSOR);
	if (!identity) {
		cur_w = scale_coord(cur_w, src_w, fit_w);
		cur_h = scale_coord(cur_h, src_h, fit_h);
		if (cur_w < 1) cur_w = 1;
		if (cur_h < 1) cur_h = 1;
	}
	if (draw_x + cur_w < 0 || draw_y + cur_h < 0 || draw_x >= canvas_w || draw_y >= canvas_h) return;

	DrawIconEx(mem, draw_x, draw_y, ci.hCursor, identity ? 0 : cur_w, identity ? 0 : cur_h, 0, nullptr, DI_NORMAL);
}

struct CaptureCache {
	HDC screen = nullptr;
	HDC mem = nullptr;
	HBITMAP bmp = nullptr;
	HGDIOBJ old = nullptr;
	void *bits = nullptr;
	int w = 0;
	int h = 0;
	int icm_old = 0;
	bool icm_set = false;
};

static CaptureCache g_cap;

static void release_capture_cache(void) {
	if (g_cap.mem && g_cap.old) SelectObject(g_cap.mem, g_cap.old);
	if (g_cap.bmp) DeleteObject(g_cap.bmp);
	if (g_cap.mem) DeleteDC(g_cap.mem);
	if (g_cap.screen) {
		if (g_cap.icm_set) SetICMMode(g_cap.screen, g_cap.icm_old);
		ReleaseDC(nullptr, g_cap.screen);
	}
	g_cap = CaptureCache{};
}

static bool ensure_capture_cache(int canvas_w, int canvas_h) {
	if (g_cap.bmp && g_cap.bits && g_cap.w == canvas_w && g_cap.h == canvas_h) return true;
	release_capture_cache();
	g_cap.screen = GetDC(nullptr);
	if (!g_cap.screen) return false;
	const int prev = SetICMMode(g_cap.screen, ICM_OFF);
	g_cap.icm_old = prev;
	g_cap.icm_set = prev != 0;
	g_cap.mem = CreateCompatibleDC(g_cap.screen);
	if (!g_cap.mem) {
		release_capture_cache();
		return false;
	}
	SetICMMode(g_cap.mem, ICM_OFF);
	BITMAPINFO dib{};
	dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	dib.bmiHeader.biWidth = canvas_w;
	dib.bmiHeader.biHeight = -canvas_h;
	dib.bmiHeader.biPlanes = 1;
	dib.bmiHeader.biBitCount = 32;
	dib.bmiHeader.biCompression = BI_RGB;
	g_cap.bmp = CreateDIBSection(g_cap.screen, &dib, DIB_RGB_COLORS, &g_cap.bits, nullptr, 0);
	if (!g_cap.bmp || !g_cap.bits) {
		release_capture_cache();
		return false;
	}
	g_cap.old = SelectObject(g_cap.mem, g_cap.bmp);
	g_cap.w = canvas_w;
	g_cap.h = canvas_h;
	return true;
}

static bool capture_frame(std::vector<BYTE> *pixels, int canvas_w, int canvas_h) {
	if (!pixels || canvas_w <= 0 || canvas_h <= 0) return false;
	if (!ensure_capture_cache(canvas_w, canvas_h)) return false;

	const size_t nbytes = static_cast<size_t>(canvas_w) * canvas_h * 4;
	if (pixels->size() != nbytes) pixels->resize(nbytes);

	HMONITOR hmon = reinterpret_cast<HMONITOR>(g_hmon.load());
	if (g_cfg.follow_mouse) {
		hmon = monitor_from_cursor();
		g_hmon.store(reinterpret_cast<uint64_t>(hmon));
	} else if (!hmon) {
		hmon = monitor_from_cursor();
	}
	RECT rc{};
	get_monitor_rect(hmon, &rc);
	const int sw = rc.right - rc.left;
	const int sh = rc.bottom - rc.top;
	if (sw <= 0 || sh <= 0) return false;

	int dst_x = 0;
	int dst_y = 0;
	int fit_w = sw;
	int fit_h = sh;
	fit_into_canvas(sw, sh, canvas_w, canvas_h, &dst_x, &dst_y, &fit_w, &fit_h);
	if (fit_w != canvas_w || fit_h != canvas_h) {
		HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		RECT fill{0, 0, canvas_w, canvas_h};
		FillRect(g_cap.mem, &fill, black);
	}
	const bool identity = fit_w == sw && fit_h == sh;
	if (identity) {
		if (!BitBlt(g_cap.mem, dst_x, dst_y, sw, sh, g_cap.screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT)) return false;
	} else {
		SetStretchBltMode(g_cap.mem, HALFTONE);
		SetBrushOrgEx(g_cap.mem, 0, 0, nullptr);
		if (!StretchBlt(g_cap.mem, dst_x, dst_y, fit_w, fit_h, g_cap.screen, rc.left, rc.top, sw, sh, SRCCOPY | CAPTUREBLT)) return false;
	}
	draw_cursor_on_canvas(g_cap.mem, canvas_w, canvas_h, &rc, dst_x, dst_y, sw, sh, fit_w, fit_h);
	memcpy(pixels->data(), g_cap.bits, nbytes);
	return true;
}

static void finalize_writer(void);
static void commit_output_file(void);

static void clear_pending_audio(void) {
	std::lock_guard<std::mutex> lock(g_audio_q_mu);
	g_audio_q.clear();
}

static void drain_pending_audio(IMFSinkWriter *writer, LONGLONG max_pts) {
	if (!writer || !g_with_audio.load()) return;
	for (;;) {
		PendingAudio chunk;
		{
			std::lock_guard<std::mutex> lock(g_audio_q_mu);
			if (g_audio_q.empty()) break;
			if (max_pts > 0 && g_audio_q.front().pts >= max_pts) {
				g_audio_q.clear();
				break;
			}
			chunk = std::move(g_audio_q.front());
			g_audio_q.pop_front();
		}
		if (chunk.frames == 0 || chunk.pcm.empty()) continue;
		if (SUCCEEDED(write_audio_pcm(writer, chunk.pcm.data(), chunk.frames, chunk.pts))) {
			const LONGLONG adur = (10000000LL * chunk.frames) / effective_audio_sample_rate_hz();
			g_audio_write_pts = chunk.pts + adur;
			g_last_audio_ts.store(g_audio_write_pts);
		}
	}
}

static void pump_audio_into_writer(void) {
	IMFSinkWriter *wtr = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_rec_mu);
		wtr = g_writer;
		if (wtr) wtr->AddRef();
	}
	if (wtr) {
		drain_pending_audio(wtr, 0);
		wtr->Release();
	}
}

static void flush_pending_audio_queue(void) {
	const LONGLONG vts = g_last_video_ts.load();
	const LONGLONG max_pts = vts > 0 ? vts + 3000000LL : 0;
	for (int i = 0; i < 200; i++) {
		IMFSinkWriter *wtr = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_rec_mu);
			wtr = g_writer;
			if (wtr) wtr->AddRef();
		}
		if (wtr) {
			drain_pending_audio(wtr, max_pts);
			wtr->Release();
		}
		size_t pending = 0;
		{
			std::lock_guard<std::mutex> lock(g_audio_q_mu);
			pending = g_audio_q.size();
		}
		if (pending == 0) break;
		Sleep(2);
	}
}

static void run_video_session(void) {
	struct CapGuard {
		~CapGuard() { release_capture_cache(); }
	} cap_guard;

	const int fps = g_cfg.fps > 0 ? g_cfg.fps : 15;
	const LONGLONG frame_dur = 10000000LL / fps;
	const bool with_audio = g_cfg.audio_enabled && (g_cfg.system_enabled || g_cfg.mic_enabled);

	std::vector<BYTE> pixels;
	std::vector<BYTE> enc_pixels;
	int fixed_w = 0, fixed_h = 0;
	calc_capture_size(&fixed_w, &fixed_h);
	int enc_w = fixed_w, enc_h = fixed_h;
	calc_encode_size(fixed_w, fixed_h, &enc_w, &enc_h);
	g_last_enc_w.store(enc_w);
	g_last_enc_h.store(enc_h);
	if (!g_rec_active.load()) return;
	if (!capture_frame(&pixels, fixed_w, fixed_h)) {
		g_fail_code.store(-5);
		g_rec_active.store(false);
		return;
	}
	g_canvas_w = fixed_w;
	g_canvas_h = fixed_h;
	char canvas_line[64];
	snprintf(canvas_line, sizeof(canvas_line), "canvas: %dx%d", fixed_w, fixed_h);
	crash_log_trace(canvas_line);

	IMFSinkWriter *writer = nullptr;
	const HRESULT cw = create_writer(g_write_path.c_str(), enc_w, enc_h, fps, with_audio, &writer);
	if (FAILED(cw) || !writer) {
		g_last_mf_hr.store(cw);
		g_fail_code.store(-4);
		g_rec_active.store(false);
		return;
	}
	if (!g_rec_active.load()) {
		writer->Release();
		delete_file_if_exists(g_write_path.c_str());
		g_fail_code.store(-4);
		return;
	}
	g_video_frames.store(0);
	g_last_video_ts.store(0);
	g_last_audio_ts.store(0);
	g_audio_write_pts = 0;
	{
		std::lock_guard<std::mutex> lock(g_rec_mu);
		g_writer = writer;
	}
	g_writer_ready.store(true);

	auto submit_frame = [&](const BYTE *frame, int frame_w, int frame_h, LONGLONG ts, LONGLONG dur) {
		IMFSinkWriter *wtr = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_rec_mu);
			wtr = g_writer;
			if (wtr) wtr->AddRef();
		}
		if (wtr) {
			if (SUCCEEDED(write_video_frame(wtr, frame, frame_w * 4, frame_w, frame_h, ts, dur))) {
				g_video_frames.fetch_add(1);
				g_last_video_ts.store(ts + dur);
			}
			wtr->Release();
		}
	};

	LONGLONG last_ts = -1;
	bool need_scale = (enc_w != fixed_w || enc_h != fixed_h);
	const BYTE *frame = pixels.data();
	int frame_w = fixed_w;
	int frame_h = fixed_h;
	if (need_scale) {
		if (!scale_rgb32_frame(pixels.data(), fixed_w, fixed_h, &enc_pixels, enc_w, enc_h)) {
			g_rec_active.store(false);
			finalize_writer();
			return;
		}
		frame = enc_pixels.data();
		frame_w = enc_w;
		frame_h = enc_h;
	}
	LONGLONG ts0 = recording_clock_100ns();
	submit_frame(frame, frame_w, frame_h, ts0, frame_dur);
	pump_audio_into_writer();
	last_ts = ts0;

	while (g_rec_active.load()) {
		LONGLONG frame_begin = recording_clock_100ns();
		if (last_ts >= 0) {
			LONGLONG wait_until = last_ts + frame_dur;
			while (g_rec_active.load() && recording_clock_100ns() < wait_until) {
				Sleep(1);
			}
		}

		if (!g_rec_active.load()) break;
		pump_audio_into_writer();
		if (!capture_frame(&pixels, fixed_w, fixed_h)) continue;

		frame = pixels.data();
		frame_w = fixed_w;
		frame_h = fixed_h;
		if (frame_w != enc_w || frame_h != enc_h) {
			if (!scale_rgb32_frame(pixels.data(), fixed_w, fixed_h, &enc_pixels, enc_w, enc_h)) continue;
			frame = enc_pixels.data();
			frame_w = enc_w;
			frame_h = enc_h;
		}

		LONGLONG ts = recording_clock_100ns();
		LONGLONG dur = (last_ts >= 0) ? (ts - last_ts) : frame_dur;
		if (dur <= 0) dur = frame_dur;
		submit_frame(frame, frame_w, frame_h, ts, dur);
		pump_audio_into_writer();
		last_ts = ts;
		(void)frame_begin;
	}
	crash_log_trace("run_video_session: wait audio");
	for (int i = 0; i < 4000 && !g_audio_capture_done.load(); i++) Sleep(2);
	crash_log_trace("run_video_session: encoder shutdown");
	flush_pending_audio_queue();
	finalize_writer();
	if (g_finalize_ok.load()) {
		crash_log_trace("run_video_session: commit");
		commit_output_file();
	}
}

static void mark_session_done(void) {
	g_session_done.store(true);
	g_writer_ready.store(false);
	g_sess_cv.notify_all();
}

static void abandon_writer_locked(void) {
	if (!g_writer) return;
	crash_log_trace("abandon_writer: Release without Finalize");
	g_writer->Release();
	g_writer = nullptr;
}

static bool wait_session_done(void) {
	if (g_session_done.load()) return true;
	crash_log_trace("wait_session_done: enter");
	for (int i = 0; i < 3000; i++) {
		if (g_session_done.load()) {
			crash_log_trace("wait_session_done: ok");
			return true;
		}
		if (i > 0 && i % 500 == 0) crash_log_trace("wait_session_done: still waiting");
		Sleep(10);
	}
	crash_log_trace("wait_session_done: timeout");
	return g_session_done.load();
}

static bool join_audio_thread(void) {
	if (!g_audio_thread.joinable()) return true;
	crash_log_trace("join_audio_thread: begin");
	HANDLE h = reinterpret_cast<HANDLE>(g_audio_thread.native_handle());
	const DWORD wr = WaitForSingleObject(h, 15000);
	if (wr == WAIT_TIMEOUT) {
		crash_log_trace("join_audio_thread: timeout");
		return false;
	}
	g_audio_thread.join();
	crash_log_trace("join_audio_thread: returned");
	return true;
}

static void ensure_encoder_thread(void) {
	if (g_encoder_thread_running.load() && g_video_thread.joinable()) return;
	if (g_video_thread.joinable()) {
		crash_log_trace("ensure_encoder_thread: join dead thread handle");
		g_video_thread.join();
	}
	g_encoder_shutdown.store(false);
	g_encoder_thread_running.store(true);
	g_video_thread = std::thread([] {
		crash_log_trace("encoder_thread: enter");
		const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const bool com_owned = SUCCEEDED(coinit);
		if (!com_owned && coinit != RPC_E_CHANGED_MODE) {
			g_last_mf_hr.store(coinit);
			g_fail_code.store(-4);
			g_rec_active.store(false);
			g_encoder_thread_running.store(false);
			mark_session_done();
			return;
		}

		for (;;) {
			{
				std::unique_lock<std::mutex> lock(g_sess_mu);
				g_sess_cv.wait(lock, [] { return g_rec_active.load() || g_encoder_shutdown.load(); });
			}
			if (g_encoder_shutdown.load()) break;
			if (!g_rec_active.load()) continue;

			if (!g_mf_started) {
				const HRESULT mfhr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
				if (FAILED(mfhr)) {
					g_last_mf_hr.store(mfhr);
					g_fail_code.store(-4);
					g_rec_active.store(false);
					mark_session_done();
					crash_log_trace("encoder_thread: MFStartup failed");
					continue;
				}
				g_mf_started = true;
			}

			g_session_done.store(false);
			crash_log_trace("encoder_thread: session begin");
			run_video_session();
			crash_log_trace("encoder_thread: session end");
			mark_session_done();
		}

		finalize_writer();
		if (g_mf_started) {
			MFShutdown();
			g_mf_started = false;
		}
		if (com_owned) CoUninitialize();
		g_encoder_thread_running.store(false);
		crash_log_trace("encoder_thread: exit");
	});
}

struct WasapiCapture {
	struct Resampler {
		std::vector<float> tail;
		double pos = 0;
	};
	IAudioClient *client = nullptr;
	IAudioCaptureClient *cap = nullptr;
	HANDLE event = nullptr;
	float gain = 1.f;
	WAVEFORMATEX *wfx = nullptr;
	bool poll_mode = false;
	Resampler resample;
};

static WAVEFORMATEX *dup_format(const WAVEFORMATEX *src) {
	if (!src) return nullptr;
	size_t bytes = sizeof(WAVEFORMATEX) + src->cbSize;
	auto *d = static_cast<WAVEFORMATEX *>(CoTaskMemAlloc(bytes));
	if (!d) return nullptr;
	memcpy(d, src, bytes);
	return d;
}

static bool is_float_format(const WAVEFORMATEX *wfx) {
	if (!wfx) return false;
	if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
	if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const auto *ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wfx);
		return guid_equal(ex->SubFormat, kSubTypeIeeeFloat);
	}
	return false;
}

static int open_wasapi_impl(IMMDevice *dev, bool loopback, DWORD capture_flags, bool event_mode, WasapiCapture *out) {
	*out = WasapiCapture{};
	IAudioClient *client = nullptr;
	if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) return -1;

	WAVEFORMATEX *mix = nullptr;
	if (FAILED(client->GetMixFormat(&mix)) || !mix) {
		client->Release();
		return -1;
	}

	DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : capture_flags;
	HANDLE ev = nullptr;
	if (event_mode && !loopback) {
		ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!ev) {
			CoTaskMemFree(mix);
			client->Release();
			return -1;
		}
		flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	}

	HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, mix, nullptr);
	if (FAILED(hr)) {
		if (ev) CloseHandle(ev);
		CoTaskMemFree(mix);
		client->Release();
		return -2;
	}

	out->wfx = dup_format(mix);
	CoTaskMemFree(mix);

	if (event_mode && ev) {
		if (FAILED(client->SetEventHandle(ev))) {
			client->Release();
			if (out->wfx) CoTaskMemFree(out->wfx);
			CloseHandle(ev);
			*out = WasapiCapture{};
			return -3;
		}
		out->event = ev;
		out->poll_mode = false;
	} else {
		out->poll_mode = true;
	}

	IAudioCaptureClient *cap = nullptr;
	if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap))) {
		client->Release();
		if (out->wfx) CoTaskMemFree(out->wfx);
		if (ev) CloseHandle(ev);
		*out = WasapiCapture{};
		return -3;
	}
	if (FAILED(client->Start())) {
		cap->Release();
		client->Release();
		if (out->wfx) CoTaskMemFree(out->wfx);
		if (ev) CloseHandle(ev);
		*out = WasapiCapture{};
		return -3;
	}
	out->client = client;
	out->cap = cap;
	return 0;
}

static int open_wasapi_impl(IMMDevice *dev, bool loopback, DWORD capture_flags, bool event_mode, WasapiCapture *out);
static void close_wasapi(WasapiCapture *c);

static int open_wasapi(IMMDevice *dev, bool loopback, WasapiCapture *out) {
	if (!dev) return -1;
	*out = WasapiCapture{};
	IAudioClient *client = nullptr;
	if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) return -1;

	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = 48000;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 4;
	fmt.nAvgBytesPerSec = 48000 * 4;
	DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

	HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &fmt, nullptr);
	if (SUCCEEDED(hr)) {
		out->wfx = dup_format(&fmt);
		IAudioCaptureClient *cap = nullptr;
		if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap))) {
			client->Release();
			if (out->wfx) CoTaskMemFree(out->wfx);
			*out = WasapiCapture{};
			return -3;
		}
		if (FAILED(client->Start())) {
			cap->Release();
			client->Release();
			if (out->wfx) CoTaskMemFree(out->wfx);
			*out = WasapiCapture{};
			return -3;
		}
		out->client = client;
		out->cap = cap;
		out->poll_mode = true;
		return 0;
	}
	client->Release();
	if (loopback) return open_wasapi_impl(dev, true, 0, false, out);
	const DWORD kAuto = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (open_wasapi_impl(dev, false, kAuto, false, out) == 0) return 0;
	close_wasapi(out);
	return open_wasapi_impl(dev, false, 0, false, out);
}

static IMMDevice *default_capture_device(IMMDeviceEnumerator *enumerator);
static void close_wasapi(WasapiCapture *c);

static int try_open_mic_device(IMMDevice *dev, WasapiCapture *out) {
	if (!dev) return -1;
	const DWORD kAuto = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (open_wasapi_impl(dev, false, kAuto, false, out) == 0) return 0;
	close_wasapi(out);
	if (open_wasapi_impl(dev, false, 0, true, out) == 0) return 0;
	close_wasapi(out);
	if (open_wasapi_impl(dev, false, kAuto, true, out) == 0) return 0;
	close_wasapi(out);
	return open_wasapi_impl(dev, false, 0, false, out);
}

static int open_mic_capture(IMMDeviceEnumerator *enumerator, WasapiCapture *out) {
	IMMDevice *dev = default_capture_device(enumerator);
	if (try_open_mic_device(dev, out) == 0) {
		if (dev) dev->Release();
		return 0;
	}
	if (dev) dev->Release();

	IMMDeviceCollection *coll = nullptr;
	if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)) || !coll) return -1;
	UINT count = 0;
	coll->GetCount(&count);
	for (UINT i = 0; i < count; i++) {
		IMMDevice *d = nullptr;
		if (FAILED(coll->Item(i, &d))) continue;
		if (try_open_mic_device(d, out) == 0) {
			d->Release();
			coll->Release();
			return 0;
		}
		d->Release();
	}
	coll->Release();
	return -1;
}

static IMMDevice *default_render_device(IMMDeviceEnumerator *enumerator) {
	IMMDevice *dev = nullptr;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return dev;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return dev;
	return nullptr;
}

static IMMDevice *default_capture_device(IMMDeviceEnumerator *enumerator) {
	IMMDevice *dev = nullptr;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) return dev;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &dev))) return dev;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &dev))) return dev;
	return nullptr;
}

static void close_wasapi(WasapiCapture *c) {
	if (!c) return;
	if (c->client) c->client->Stop();
	if (c->cap) c->cap->Release();
	if (c->client) c->client->Release();
	if (c->event) CloseHandle(c->event);
	if (c->wfx) CoTaskMemFree(c->wfx);
	*c = WasapiCapture{};
}

static void decode_to_stereo_float(const WasapiCapture *cap, const BYTE *data, UINT32 frames, DWORD flags, float *out_lr) {
	const WAVEFORMATEX *wfx = cap->wfx;
	const int ch = wfx && wfx->nChannels > 0 ? wfx->nChannels : 2;
	const int block = wfx && wfx->nBlockAlign > 0 ? wfx->nBlockAlign : (ch * (wfx && wfx->wBitsPerSample ? wfx->wBitsPerSample / 8 : 4));

	if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
		memset(out_lr, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
		return;
	}

	if (is_float_format(wfx)) {
		for (UINT32 i = 0; i < frames; i++) {
			const float *f = reinterpret_cast<const float *>(data + static_cast<size_t>(i) * block);
			float l = f[0];
			float r = (ch > 1) ? f[1] : l;
			out_lr[i * 2 + 0] = l * cap->gain;
			out_lr[i * 2 + 1] = r * cap->gain;
		}
		return;
	}

	if (wfx && is_pcm_format(wfx) && wfx->wBitsPerSample == 16) {
		for (UINT32 i = 0; i < frames; i++) {
			const int16_t *s = reinterpret_cast<const int16_t *>(data + static_cast<size_t>(i) * block);
			float l = s[0] / 32768.f;
			float r = (ch > 1) ? (s[1] / 32768.f) : l;
			out_lr[i * 2 + 0] = l * cap->gain;
			out_lr[i * 2 + 1] = r * cap->gain;
		}
		return;
	}

	if (wfx && is_pcm_format(wfx) && wfx->wBitsPerSample == 32) {
		for (UINT32 i = 0; i < frames; i++) {
			const int32_t *s = reinterpret_cast<const int32_t *>(data + static_cast<size_t>(i) * block);
			float l = s[0] / 2147483648.f;
			float r = (ch > 1) ? (s[1] / 2147483648.f) : l;
			out_lr[i * 2 + 0] = l * cap->gain;
			out_lr[i * 2 + 1] = r * cap->gain;
		}
		return;
	}

	memset(out_lr, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
}

static void resample_to_output_rate(WasapiCapture::Resampler *st, const float *in, UINT32 in_frames, int in_rate, std::vector<float> *out) {
	out->clear();
	if (!st || !in || in_frames == 0) return;
	const int out_rate = effective_audio_sample_rate_hz();
	if (in_rate <= 0) in_rate = out_rate;
	if (in_rate == out_rate) {
		if (!st->tail.empty()) {
			out->insert(out->end(), st->tail.begin(), st->tail.end());
			st->tail.clear();
			st->pos = 0;
		}
		out->insert(out->end(), in, in + static_cast<size_t>(in_frames) * 2);
		return;
	}
	st->tail.insert(st->tail.end(), in, in + static_cast<size_t>(in_frames) * 2);
	const size_t max_tail = static_cast<size_t>(in_rate) * 4;
	if (st->tail.size() > max_tail) {
		size_t drop = st->tail.size() - max_tail;
		drop -= drop % 2;
		st->tail.erase(st->tail.begin(), st->tail.begin() + drop);
		st->pos = 0;
	}
	const UINT32 frames = static_cast<UINT32>(st->tail.size() / 2);
	if (frames < 2) return;
	const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
	double p = st->pos;
	while (p + 1.0 < static_cast<double>(frames)) {
		const UINT32 i0 = static_cast<UINT32>(p);
		const float t = static_cast<float>(p - static_cast<double>(i0));
		for (int c = 0; c < 2; c++) {
			const float a = st->tail[static_cast<size_t>(i0) * 2 + c];
			const float b = st->tail[(static_cast<size_t>(i0) + 1) * 2 + c];
			out->push_back(a + (b - a) * t);
		}
		p += step;
	}
	const UINT32 consumed = static_cast<UINT32>(p);
	if (consumed > 0 && consumed < frames) {
		st->tail.erase(st->tail.begin(), st->tail.begin() + static_cast<size_t>(consumed) * 2);
		p -= consumed;
	}
	st->pos = p;
}

static void trim_audio_hold(std::vector<float> *hold) {
	if (!hold) return;
	const size_t max_floats = static_cast<size_t>(effective_audio_sample_rate_hz()) * 2;
	if (hold->size() > max_floats) {
		hold->erase(hold->begin(), hold->begin() + (hold->size() - max_floats));
	}
}

static void consume_audio_hold(std::vector<float> *hold, UINT32 frames) {
	if (!hold) return;
	const size_t n = static_cast<size_t>(frames) * 2;
	if (n == 0) return;
	if (n >= hold->size()) hold->clear();
	else hold->erase(hold->begin(), hold->begin() + n);
}

static void shape_mix(float *s, size_t n) {
	if (!s || n == 0) return;
	if (g_cfg.clip_limit) {
		clamp_audio(s, n);
		g_limit_gain = 1.f;
		return;
	}
	float peak = 0.f;
	for (size_t i = 0; i < n; i++) {
		const float a = fabsf(s[i]);
		if (a > peak) peak = a;
	}
	float target = peak > 1.f ? 1.f / peak : 1.f;
	for (size_t i = 0; i < n; i++) {
		g_limit_gain += (target - g_limit_gain) * 0.002f;
		s[i] *= g_limit_gain;
		if (s[i] > 1.f) s[i] = 1.f;
		if (s[i] < -1.f) s[i] = -1.f;
	}
}

static bool drain_and_mix(WasapiCapture *sys, WasapiCapture *mic, std::vector<float> *sys_hold, std::vector<float> *mic_hold, std::vector<int16_t> *pcm_out, UINT32 *out_frames, LONGLONG *capture_pts) {
	if (capture_pts) *capture_pts = -1;
	auto drain_one = [&](WasapiCapture *cap, std::vector<float> *chunks) {
		if (!cap || !cap->cap || !chunks) return;
		UINT32 packet = 0;
		if (FAILED(cap->cap->GetNextPacketSize(&packet))) return;
		int packet_budget = 64;
		while (packet && packet_budget-- > 0 && g_rec_active.load()) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			UINT64 qpc = 0;
			const HRESULT hr = cap->cap->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
			if (FAILED(hr)) break;
			if (frames > 0 && data) {
				if (capture_pts && *capture_pts < 0 && qpc != 0 && g_qpc_freq.QuadPart != 0) {
					LONGLONG pts = qpc_delta_to_100ns(static_cast<LONGLONG>(qpc) - g_qpc_start.QuadPart);
					if (pts < 0) pts = 0;
					*capture_pts = pts;
				}
				std::vector<float> tmp(static_cast<size_t>(frames) * 2);
				decode_to_stereo_float(cap, data, frames, flags, tmp.data());
				const int rate = cap->wfx ? static_cast<int>(cap->wfx->nSamplesPerSec) : 48000;
				std::vector<float> rs;
				resample_to_output_rate(&cap->resample, tmp.data(), frames, rate, &rs);
				chunks->insert(chunks->end(), rs.begin(), rs.end());
			}
			cap->cap->ReleaseBuffer(frames);
			if (FAILED(cap->cap->GetNextPacketSize(&packet))) break;
		}
		trim_audio_hold(chunks);
	};

	drain_one(sys, sys_hold);
	drain_one(mic, mic_hold);

	const bool has_sys = sys && sys->cap;
	const bool has_mic = mic && mic->cap;
	UINT32 sn = has_sys ? static_cast<UINT32>(sys_hold->size() / 2) : 0;
	UINT32 mn = has_mic ? static_cast<UINT32>(mic_hold->size() / 2) : 0;
	const UINT32 lead = static_cast<UINT32>(std::max(1, effective_audio_sample_rate_hz() / 10));
	bool use_sys = has_sys;
	bool use_mic = has_mic;
	UINT32 n = 0;
	if (has_sys && has_mic) {
		if (sn == 0 && mn > lead) {
			n = mn - lead;
			use_sys = false;
		} else if (mn == 0 && sn > lead) {
			n = sn - lead;
			use_mic = false;
		} else {
			n = std::min(sn, mn);
		}
	} else if (has_sys) {
		n = sn;
	} else if (has_mic) {
		n = mn;
	}
	if (n == 0) return false;

	std::vector<float> mix(static_cast<size_t>(n) * 2, 0.f);
	for (UINT32 i = 0; i < n; i++) {
		float l = 0.f, r = 0.f;
		if (use_sys && sys_hold->size() >= static_cast<size_t>(i) * 2 + 2) {
			l += (*sys_hold)[i * 2 + 0];
			r += (*sys_hold)[i * 2 + 1];
		}
		if (use_mic && mic_hold->size() >= static_cast<size_t>(i) * 2 + 2) {
			l += (*mic_hold)[i * 2 + 0];
			r += (*mic_hold)[i * 2 + 1];
		}
		mix[i * 2 + 0] = l;
		mix[i * 2 + 1] = r;
	}
	if (use_sys) consume_audio_hold(sys_hold, n);
	if (use_mic) consume_audio_hold(mic_hold, n);

	shape_mix(mix.data(), mix.size());
	pcm_out->resize(mix.size());
	for (size_t i = 0; i < mix.size(); i++) {
		float v = mix[i];
		if (v > 1.f) v = 1.f;
		if (v < -1.f) v = -1.f;
		(*pcm_out)[i] = static_cast<int16_t>(v * 32767.f);
	}
	*out_frames = n;
	return true;
}

static std::atomic<int> g_default_dev_gen{0};

class DefaultEndpointNotify final : public IMMNotificationClient {
	volatile long ref_{1};

public:
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
			*ppv = static_cast<IMMNotificationClient *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
	STDMETHODIMP_(ULONG) Release() override {
		const ULONG n = static_cast<ULONG>(InterlockedDecrement(&ref_));
		if (n == 0) delete this;
		return n;
	}
	STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
	STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
	STDMETHODIMP OnDeviceRemoved(LPCWSTR) override {
		g_default_dev_gen.fetch_add(1);
		return S_OK;
	}
	STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
		if (role == eConsole && (flow == eRender || flow == eCapture)) g_default_dev_gen.fetch_add(1);
		return S_OK;
	}
	STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

static std::wstring device_id_of(IMMDevice *dev) {
	if (!dev) return L"";
	LPWSTR id = nullptr;
	if (FAILED(dev->GetId(&id)) || !id) return L"";
	std::wstring s = id;
	CoTaskMemFree(id);
	return s;
}

static void bind_default_speaker(IMMDeviceEnumerator *en, WasapiCapture *sys, std::wstring *cur_id, std::vector<float> *hold) {
	if (!g_cfg.system_enabled || !en) return;
	IMMDevice *dev = default_render_device(en);
	const std::wstring nid = device_id_of(dev);
	if (sys->cap && nid == *cur_id) {
		if (dev) dev->Release();
		return;
	}
	close_wasapi(sys);
	if (hold) hold->clear();
	if (dev && open_wasapi(dev, true, sys) == 0) {
		sys->gain = db_to_linear(g_cfg.gain_system_db);
		*cur_id = nid;
		crash_log_trace("audio: using system default speaker");
	} else {
		*cur_id = L"";
		crash_log_trace("audio: default speaker unavailable");
	}
	if (dev) dev->Release();
}

static void bind_default_microphone(IMMDeviceEnumerator *en, WasapiCapture *mic, std::wstring *cur_id, std::vector<float> *hold) {
	if (!g_cfg.mic_enabled || !en) return;
	IMMDevice *dev = default_capture_device(en);
	const std::wstring nid = device_id_of(dev);
	if (mic->cap && nid == *cur_id) {
		if (dev) dev->Release();
		return;
	}
	close_wasapi(mic);
	if (hold) hold->clear();
	if (dev && open_wasapi(dev, false, mic) == 0) {
		mic->gain = db_to_linear(g_cfg.gain_mic_db);
		*cur_id = nid;
		crash_log_trace("audio: using system default microphone");
	} else {
		*cur_id = L"";
		crash_log_trace("audio: default microphone unavailable");
	}
	if (dev) dev->Release();
}

static void audio_loop(void) {
	crash_log_trace("audio_loop: enter");
	struct DoneGuard {
		~DoneGuard() { g_audio_capture_done.store(true); }
	} done_guard;
	if (!g_cfg.audio_enabled) return;
	const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool com_owned = SUCCEEDED(cohr);

	DWORD mm_index = 0;
	HANDLE mm_task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mm_index);

	for (int i = 0; i < 500 && g_rec_active.load() && !g_writer_ready.load(); i++) Sleep(10);

	IMMDeviceEnumerator *enumerator = nullptr;
	CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
	DefaultEndpointNotify *notify = nullptr;
	if (enumerator) {
		notify = new DefaultEndpointNotify();
		if (FAILED(enumerator->RegisterEndpointNotificationCallback(notify))) {
			notify->Release();
			notify = nullptr;
		}
	}
	WasapiCapture sys{}, mic{};
	std::wstring sys_id, mic_id;
	std::vector<float> sys_hold, mic_hold;
	int seen_gen = g_default_dev_gen.load();
	bind_default_speaker(enumerator, &sys, &sys_id, &sys_hold);
	bind_default_microphone(enumerator, &mic, &mic_id, &mic_hold);

	std::vector<int16_t> pcm;
	LONGLONG next_pts = 0;
	while (g_rec_active.load()) {
		const int gen = g_default_dev_gen.load();
		if (gen != seen_gen) {
			seen_gen = gen;
			crash_log_trace("audio: system default device changed");
			bind_default_speaker(enumerator, &sys, &sys_id, &sys_hold);
			bind_default_microphone(enumerator, &mic, &mic_id, &mic_hold);
		}

		HANDLE waits[2];
		DWORD wait_n = 0;
		if (sys.event) waits[wait_n++] = sys.event;
		if (mic.event) waits[wait_n++] = mic.event;
		if (wait_n > 0) {
			WaitForMultipleObjects(wait_n, waits, FALSE, 20);
		} else {
			Sleep(5);
		}

		UINT32 frames = 0;
		LONGLONG capture_pts = -1;
		if (!drain_and_mix(&sys, &mic, &sys_hold, &mic_hold, &pcm, &frames, &capture_pts) || frames == 0) continue;

		if (!g_with_audio.load()) continue;
		const LONGLONG dur = (10000000LL * frames) / effective_audio_sample_rate_hz();
		LONGLONG pts = capture_pts >= 0 ? capture_pts : recording_clock_100ns() - dur;
		if (pts < 0) pts = 0;
		if (pts < next_pts) pts = next_pts;
		next_pts = pts + dur;

		PendingAudio chunk;
		chunk.frames = frames;
		chunk.pcm = std::move(pcm);
		chunk.pts = pts;
		{
			std::lock_guard<std::mutex> lock(g_audio_q_mu);
			g_audio_q.push_back(std::move(chunk));
			while (g_audio_q.size() > 256) g_audio_q.pop_front();
		}
	}
	close_wasapi(&sys);
	close_wasapi(&mic);
	if (enumerator && notify) enumerator->UnregisterEndpointNotificationCallback(notify);
	if (notify) notify->Release();
	if (enumerator) enumerator->Release();
	if (mm_task) AvRevertMmThreadCharacteristics(mm_task);
	if (com_owned) CoUninitialize();
	crash_log_trace("audio_loop: exit");
}

static void fin_set_busy(bool busy) {
	std::lock_guard<std::mutex> lk(g_fin_mu);
	g_fin_busy = busy;
	if (!busy) g_fin_cv.notify_all();
}

static bool fin_is_busy(void) {
	std::lock_guard<std::mutex> lk(g_fin_mu);
	return g_fin_busy;
}

static bool fin_wait_for(std::chrono::milliseconds d) {
	std::unique_lock<std::mutex> lk(g_fin_mu);
	return g_fin_cv.wait_for(lk, d, [] { return !g_fin_busy; });
}

static void begin_hires_timer(void) {
	bool expected = false;
	if (g_timer_period.compare_exchange_strong(expected, true)) {
		if (timeBeginPeriod(1) != TIMERR_NOERROR) g_timer_period.store(false);
	}
}

static void end_hires_timer(void) {
	if (g_timer_period.exchange(false)) timeEndPeriod(1);
}

struct FinalizeJob {
	IMFSinkWriter *writer = nullptr;
	HRESULT hr = E_FAIL;
	HANDLE done = nullptr;
	std::atomic<int> state{0};
	std::wstring tmp;
	std::wstring final_path;
	int frames = 0;
};

static std::atomic<bool> g_output_committed{false};

static bool commit_paths(const std::wstring &tmp, const std::wstring &final_path, int frames) {
	bool expected = false;
	if (!g_output_committed.compare_exchange_strong(expected, true)) return false;
	if (tmp.empty() || final_path.empty() || frames <= 0 || file_size_bytes(tmp.c_str()) == 0) {
		delete_file_if_exists(tmp.c_str());
		g_output_committed.store(false);
		return false;
	}
	if (!MoveFileExW(tmp.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
		if (!CopyFileW(tmp.c_str(), final_path.c_str(), TRUE)) {
			g_output_committed.store(false);
			return false;
		}
	}
	delete_file_if_exists(tmp.c_str());
	crash_log_trace("commit_output_file: ok");
	return true;
}

static DWORD WINAPI finalize_thread_proc(void *param) {
	auto *job = static_cast<FinalizeJob *>(param);
	const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool com_owned = SUCCEEDED(cohr);
	crash_log_trace("finalize thread: Finalize()");
	job->hr = job->writer->Finalize();
	char line[96];
	snprintf(line, sizeof(line), "finalize thread: returned hr=0x%08lX", static_cast<unsigned long>(job->hr));
	crash_log_trace(line);
	job->writer->Release();
	job->writer = nullptr;
	int expected = 0;
	if (job->state.compare_exchange_strong(expected, 1)) {
		if (job->done) SetEvent(job->done);
		if (com_owned) CoUninitialize();
		return 0;
	}
	g_finalize_ok.store(SUCCEEDED(job->hr));
	if (FAILED(job->hr)) g_last_mf_hr.store(job->hr);
	if (SUCCEEDED(job->hr)) commit_paths(job->tmp, job->final_path, job->frames);
	else delete_file_if_exists(job->tmp.c_str());
	if (job->done) CloseHandle(job->done);
	delete job;
	fin_set_busy(false);
	if (com_owned) CoUninitialize();
	return 0;
}

static void finalize_writer(void) {
	crash_log_trace("finalize_writer: enter");
	IMFSinkWriter *writer = nullptr;
	DWORD v_stream = 0;
	DWORD a_stream = 0;
	const bool with_audio = g_with_audio.load();
	{
		std::lock_guard<std::mutex> lock(g_rec_mu);
		writer = g_writer;
		g_writer = nullptr;
		v_stream = g_v_stream;
		a_stream = g_a_stream;
	}
	if (!writer) {
		crash_log_trace("finalize_writer: no writer");
		g_finalize_ok.store(false);
		return;
	}

	LONGLONG end_ts = g_last_video_ts.load();
	const LONGLONG ats = g_last_audio_ts.load();
	if (with_audio && ats > end_ts) end_ts = ats;
	if (end_ts > 0) {
		writer->SendStreamTick(v_stream, end_ts);
		if (with_audio) writer->SendStreamTick(a_stream, end_ts);
	}

	fin_set_busy(true);
	auto *job = new FinalizeJob();
	job->writer = writer;
	job->tmp = g_write_path;
	job->final_path = g_final_path;
	job->frames = g_video_frames.load();
	job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE th = job->done ? CreateThread(nullptr, 0, finalize_thread_proc, job, 0, nullptr) : nullptr;
	if (!th) {
		job->hr = writer->Finalize();
		writer->Release();
		g_finalize_ok.store(SUCCEEDED(job->hr));
		if (FAILED(job->hr)) g_last_mf_hr.store(job->hr);
		if (job->done) CloseHandle(job->done);
		delete job;
		fin_set_busy(false);
		crash_log_trace("finalize_writer: inline");
		return;
	}

	const DWORD started = GetTickCount();
	bool finished = false;
	for (;;) {
		DWORD index = 0;
		const HRESULT waited = CoWaitForMultipleHandles(0, 200, 1, &job->done, &index);
		if (waited == S_OK) {
			finished = true;
			break;
		}
		if (GetTickCount() - started > 60000) break;
	}
	if (finished) {
		WaitForSingleObject(th, 5000);
		CloseHandle(th);
		g_finalize_ok.store(SUCCEEDED(job->hr));
		if (FAILED(job->hr)) g_last_mf_hr.store(job->hr);
		CloseHandle(job->done);
		delete job;
		fin_set_busy(false);
		crash_log_trace("finalize_writer: done");
		return;
	}
	int expected = 0;
	if (job->state.compare_exchange_strong(expected, 2)) {
		crash_log_trace("finalize_writer: handed off after 60s");
		CloseHandle(th);
		g_finalize_ok.store(false);
		return;
	}
	WaitForSingleObject(job->done, 5000);
	WaitForSingleObject(th, 5000);
	CloseHandle(th);
	g_finalize_ok.store(SUCCEEDED(job->hr));
	if (FAILED(job->hr)) g_last_mf_hr.store(job->hr);
	if (job->done) CloseHandle(job->done);
	delete job;
	fin_set_busy(false);
	crash_log_trace("finalize_writer: done");
}

static void commit_output_file(void) {
	commit_paths(g_write_path, g_final_path, g_video_frames.load());
}

int recorder_finalize_pending(void) {
	return fin_is_busy() ? 1 : 0;
}

int recorder_start(const wchar_t *path, uint64_t hmon, const native_rec_config *cfg) {
	crash_log_trace("recorder_start: enter");
	if (!cfg || !path) return -1;
	if (fin_is_busy()) {
		crash_log_trace("recorder_start: previous finalize still running");
		return -8;
	}
	std::lock_guard<std::mutex> life(g_life_mu);
	if (g_rec_active.load()) return -2;
	if (!g_session_done.load()) {
		g_rec_active.store(false);
		g_sess_cv.notify_all();
		if (!wait_session_done()) return -8;
	}
	if (!join_audio_thread()) {
		crash_log_trace("recorder_start: audio thread still running");
		return -7;
	}
	finalize_writer();
	clear_pending_audio();
	g_cfg = *cfg;
	g_wmv_input_iyuv = false;
	g_h264_nv12 = false;
	g_encode_audio_rate_hz = 0;
	g_limit_gain = 1.f;
	g_fail_code.store(0);
	g_hmon.store(hmon ? hmon : monitor_tracker_current());
	g_final_path = path;
	g_write_path = temp_recording_path(g_final_path);
	delete_file_if_exists(g_write_path.c_str());
	g_video_frames.store(0);
	g_writer_ready.store(false);
	g_output_committed.store(false);
	mark_recording_clock_start();
	clear_pending_audio();
	g_finalize_ok.store(false);
	g_audio_capture_done.store(!(g_cfg.audio_enabled && (g_cfg.system_enabled || g_cfg.mic_enabled)));
	g_session_done.store(false);
	begin_hires_timer();
	ensure_encoder_thread();
	{
		std::lock_guard<std::mutex> lock(g_sess_mu);
		g_rec_active.store(true);
	}
	g_sess_cv.notify_all();
	if (g_cfg.audio_enabled && (g_cfg.system_enabled || g_cfg.mic_enabled)) {
		if (g_audio_thread.joinable()) g_audio_thread.join();
		g_audio_thread = std::thread(audio_loop);
	}
	for (int i = 0; i < 2000; i++) {
		if (g_writer_ready.load()) {
			crash_log_trace("recorder_start: writer ready");
			return 0;
		}
		if (!g_rec_active.load() || !g_encoder_thread_running.load()) break;
		Sleep(10);
	}
	g_rec_active.store(false);
	g_sess_cv.notify_all();
	if (!wait_session_done()) {
		end_hires_timer();
		crash_log_trace("recorder_start: session still running");
		return -8;
	}
	end_hires_timer();
	const int code = g_fail_code.load();
	if (!g_output_committed.load()) delete_file_if_exists(g_write_path.c_str());
	crash_log_trace("recorder_start: failed");
	return code != 0 ? code : -4;
}

int native_last_video_frames(void) {
	return g_video_frames.load();
}

long native_last_mf_hresult(void) {
	return g_last_mf_hr.load();
}

void native_last_encode_size(int *w, int *h) {
	if (w) *w = g_last_enc_w.load();
	if (h) *h = g_last_enc_h.load();
}

void recorder_set_monitor(uint64_t hmon) {
	if (hmon) g_hmon.store(hmon);
}

void recorder_stop(void) {
	crash_log_trace("recorder_stop: enter");
	std::lock_guard<std::mutex> life(g_life_mu);
	g_rec_active.store(false);
	g_writer_ready.store(false);
	g_sess_cv.notify_all();
	if (!join_audio_thread()) {
		crash_log_trace("recorder_stop: audio join timed out");
	}
	const bool session_done = wait_session_done();
	bool fin_idle = false;
	{
		std::unique_lock<std::mutex> lk(g_fin_mu);
		fin_idle = g_fin_cv.wait_for(lk, std::chrono::seconds(60), [] { return !g_fin_busy; });
	}
	if (g_output_committed.load()) {
		crash_log_trace("recorder_stop: already committed");
	} else if (g_finalize_ok.load()) {
		crash_log_trace("recorder_stop: commit");
		commit_output_file();
	} else if (session_done && fin_idle) {
		crash_log_trace("recorder_stop: discard incomplete file");
		delete_file_if_exists(g_write_path.c_str());
	} else {
		crash_log_trace("recorder_stop: encoder still finishing; file will be committed later");
	}
	end_hires_timer();
	crash_log_trace("recorder_stop: done");
}

void recorder_shutdown(void) {
	crash_log_trace("recorder_shutdown: enter");
	std::lock_guard<std::mutex> life(g_life_mu);
	g_encoder_shutdown.store(true);
	g_rec_active.store(false);
	g_sess_cv.notify_all();
	wait_session_done();
	if (!join_audio_thread() && g_audio_thread.joinable()) {
		crash_log_trace("recorder_shutdown: detach stuck audio thread");
		g_audio_thread.detach();
	}
	if (!fin_wait_for(std::chrono::seconds(5))) {
		crash_log_trace("recorder_shutdown: finalize still running");
	}
	if (g_video_thread.joinable()) {
		crash_log_trace("recorder_shutdown: join encoder thread");
		g_video_thread.join();
		crash_log_trace("recorder_shutdown: encoder joined");
	}
	end_hires_timer();
	release_capture_cache();
	crash_log_trace("recorder_shutdown: done");
}

