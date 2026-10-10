//go:build windows

package wincap

/*
#cgo windows CXXFLAGS: -std=c++17 -I${SRCDIR}
#cgo windows LDFLAGS: -lole32 -loleaut32 -luuid -lavrt -lmfplat -lmfreadwrite -lmfuuid -lwinmm -lgdi32 -luser32

#include <stdlib.h>
#include "bridge.h"

extern void autoScreenMicCB(int event, unsigned int pid);
extern void autoScreenMonitorCB(int from_idx, int to_idx, unsigned long long hmon);
*/
import "C"
import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
	"unsafe"

	"AutoScreenRecorder/internal/config"

	"golang.org/x/sys/windows"
)

func utf16Buf(s string) ([]uint16, *C.wchar_t, error) {
	buf, err := windows.UTF16FromString(s)
	if err != nil || len(buf) == 0 {
		return nil, nil, errors.New("invalid utf-16 string")
	}
	return buf, (*C.wchar_t)(unsafe.Pointer(&buf[0])), nil
}

type Platform struct {
	mu        sync.Mutex
	onMic     func(event int, pid uint32)
	onMonitor func(from, to int, hmon uint64)
}

var defaultPlatform Platform

//export autoScreenMicCB
func autoScreenMicCB(event C.int, pid C.uint32_t) {
	defaultPlatform.mu.Lock()
	fn := defaultPlatform.onMic
	defaultPlatform.mu.Unlock()
	if fn != nil {
		fn(int(event), uint32(pid))
	}
}

//export autoScreenMonitorCB
func autoScreenMonitorCB(from, to C.int, hmon C.ulonglong) {
	defaultPlatform.mu.Lock()
	fn := defaultPlatform.onMonitor
	defaultPlatform.mu.Unlock()
	if fn != nil {
		fn(int(from), int(to), uint64(hmon))
	}
}

var nativeLife sync.Mutex

func Start(onMic func(event int, pid uint32), onMonitor func(from, to int, hmon uint64)) error {
	nativeLife.Lock()
	defer nativeLife.Unlock()
	return startLocked(onMic, onMonitor)
}

func startLocked(onMic func(event int, pid uint32), onMonitor func(from, to int, hmon uint64)) error {
	defaultPlatform.mu.Lock()
	defaultPlatform.onMic = onMic
	defaultPlatform.onMonitor = onMonitor
	defaultPlatform.mu.Unlock()

	r := C.native_start(
		(C.native_mic_cb)(C.autoScreenMicCB),
		(C.native_monitor_cb)(C.autoScreenMonitorCB),
	)
	if r != 0 {
		return fmt.Errorf("原生服务启动失败，代码 %d", int(r))
	}
	log.Printf("原生服务已启动")
	NativeTrace("Go: native service started")
	return nil
}

func Stop() {
	nativeLife.Lock()
	defer nativeLife.Unlock()
	log.Printf("原生服务停止")
	NativeTrace("Go: native service stop")
	C.native_stop()
}

// Restart tears down capture and microphone watching, then starts them again.
func Restart() error {
	nativeLife.Lock()
	defer nativeLife.Unlock()
	log.Printf("重新初始化原生服务")
	NativeTrace("Go: native restart begin")
	C.native_stop()
	defaultPlatform.mu.Lock()
	onMic := defaultPlatform.onMic
	onMon := defaultPlatform.onMonitor
	defaultPlatform.mu.Unlock()
	if err := startLocked(onMic, onMon); err != nil {
		NativeTrace("Go: native restart failed: " + err.Error())
		return err
	}
	NativeTrace("Go: native restart ok")
	return nil
}

func CurrentMonitor() uint64 {
	return uint64(C.native_current_monitor())
}

type NativeRecorder struct {
	mu          sync.Mutex
	sessionPath string
}

func NewNativeRecorder() *NativeRecorder {
	return &NativeRecorder{}
}

func (n *NativeRecorder) CurrentMonitorHandle() uint64 {
	return CurrentMonitor()
}

func (n *NativeRecorder) Start(_ context.Context, path string, monitorHandle uint64, cfg config.Config) error {
	pathBuf, cpath, err := utf16Buf(path)
	if err != nil {
		return errors.New("invalid output path")
	}
	nc := toNativeRecConfig(cfg)
	mon := monitorHandle
	if mon == 0 {
		mon = CurrentMonitor()
	}
	log.Printf("开始录制 path=%s monitor=%d fps=%d bitrate=%dkbps container=%s audio=%t",
		path, mon, cfg.Record.FPS, cfg.Record.VideoBitrateKbps, cfg.Record.Container, cfg.Audio.Enabled)
	NativeTrace(fmt.Sprintf("Go: start path=%s monitor=%d fps=%d bitrate=%d container=%s",
		path, mon, cfg.Record.FPS, cfg.Record.VideoBitrateKbps, cfg.Record.Container))
	r := C.native_start_recording(cpath, C.ulonglong(mon), &nc)
	runtime.KeepAlive(pathBuf)
	if r != 0 {
		msg := startErr(r)
		log.Printf("开始录制失败 code=%d %s", int(r), msg)
		NativeTrace(fmt.Sprintf("Go: start failed code=%d %s", int(r), msg))
		return msg
	}
	n.mu.Lock()
	n.sessionPath = path
	n.mu.Unlock()
	return nil
}

func startErr(r C.int) error {
	if r == -4 || r == -5 {
		hr := uint32(int32(C.native_last_mf_hresult()))
		var ew, eh C.int
		C.native_last_encode_size(&ew, &eh)
		if r == -5 {
			return fmt.Errorf("抓屏失败，分辨率 %dx%d", int(ew), int(eh))
		}
		if hr == 0 {
			return fmt.Errorf("视频编码器启动失败，分辨率 %dx%d", int(ew), int(eh))
		}
		return fmt.Errorf("视频编码器启动失败，分辨率 %dx%d，错误码 0x%08X", int(ew), int(eh), hr)
	}
	if r == -7 {
		return errors.New("上一段录音线程还没结束")
	}
	if r == -8 {
		return errors.New("上一段录制还没结束")
	}
	return fmt.Errorf("无法开始录制，代码 %d", int(r))
}

func (n *NativeRecorder) SetMonitor(monitorHandle uint64) error {
	C.native_set_monitor(C.ulonglong(monitorHandle))
	return nil
}

func waitFinalize() {
	deadline := time.Now().Add(30 * time.Second)
	for C.native_finalize_pending() != 0 && time.Now().Before(deadline) {
		time.Sleep(200 * time.Millisecond)
	}
}

func (n *NativeRecorder) Stop(_ context.Context) (string, error) {
	n.mu.Lock()
	session := n.sessionPath
	n.sessionPath = ""
	n.mu.Unlock()

	if session == "" {
		return "", nil
	}

	log.Printf("native Stop: begin session=%s", session)
	NativeTrace("Go: native_stop_recording begin")
	C.native_stop_recording()
	NativeTrace("Go: native_stop_recording end")
	waitFinalize()
	log.Printf("native Stop: end session=%s", session)

	frames := int(C.native_last_video_frames())
	tempPath := tempRecordingPath(session)
	pending := C.native_finalize_pending() != 0

	if _, err := os.Stat(session); err != nil {
		if pending {
			return "", fmt.Errorf("视频还在保存")
		}
		if _, errTemp := os.Stat(tempPath); errTemp == nil {
			if errRename := os.Rename(tempPath, session); errRename == nil {
				log.Printf("从临时文件恢复录制: %s", session)
				return session, nil
			} else {
				log.Printf("临时文件改名失败: %v", errRename)
			}
		}
		if frames <= 0 {
			_ = os.Remove(tempPath)
			return "", fmt.Errorf("没有录到画面，文件未保存")
		}
		return "", fmt.Errorf("视频文件丢失，已编码 %d 帧", frames)
	}
	return session, nil
}

func tempRecordingPath(finalPath string) string {
	ext := filepath.Ext(finalPath)
	if ext == "" {
		return finalPath + ".tmp"
	}
	return strings.TrimSuffix(finalPath, ext) + ".tmp" + ext
}

func toNativeRecConfig(cfg config.Config) C.native_rec_config {
	a := cfg.Audio
	nc := C.native_rec_config{
		fps: C.int(cfg.Record.FPS),
	}
	if cfg.Record.FollowMouse {
		nc.follow_mouse = 1
	}
	kbps := cfg.Record.VideoBitrateKbps
	if kbps <= 0 {
		kbps = 400
	}
	nc.video_bitrate_kbps = C.int(kbps)
	nc.encode_max_width = C.int(cfg.Record.EncodeMaxWidth)
	nc.encode_max_height = C.int(cfg.Record.EncodeMaxHeight)
	switch strings.ToLower(strings.TrimSpace(cfg.Record.Container)) {
	case "mp4", "h264":
		nc.use_wmv = 0
	default:
		nc.use_wmv = 1
	}
	if a.Enabled {
		nc.audio_enabled = 1
	}
	if a.System.Enabled {
		nc.system_enabled = 1
	}
	if a.Microphone.Enabled {
		nc.mic_enabled = 1
	}
	nc.gain_system_db = C.float(a.Gain.SystemDB)
	nc.gain_mic_db = C.float(a.Gain.MicrophoneDB)
	if a.ClipLimit {
		nc.clip_limit = 1
	}
	return nc
}

// InitCrashLog enables native crash/terminate logging to the same file as Go logs.
func InitCrashLog(path string) {
	buf, p, err := utf16Buf(path)
	if err != nil {
		return
	}
	C.native_init_crash_log(p)
	runtime.KeepAlive(buf)
}

// NativeTrace writes a line to the native crash log (flushed immediately).
func NativeTrace(msg string) {
	cstr := C.CString(msg)
	defer C.free(unsafe.Pointer(cstr))
	C.native_crash_log_trace(cstr)
}
