//go:build windows

package applog

import (
	"errors"
	"io"
	"log"
	"os"
	"path/filepath"
	"runtime/debug"
	"sync"
	"sync/atomic"
	"time"

	"AutoScreenRecorder/internal/wincap"

	"golang.org/x/sys/windows"
)

var (
	file     *os.File
	fileMu   sync.Mutex
	logMutex atomic.Uintptr
	lastSync time.Time
)

type flushWriter struct{}

func (flushWriter) Write(p []byte) (int, error) {
	if h := windows.Handle(logMutex.Load()); h != 0 {
		wr, err := windows.WaitForSingleObject(h, 5000)
		if err != nil || (wr != windows.WAIT_OBJECT_0 && wr != windows.WAIT_ABANDONED) {
			return len(p), nil
		}
		defer windows.ReleaseMutex(h)
	}
	fileMu.Lock()
	defer fileMu.Unlock()
	if file == nil {
		return len(p), nil
	}
	n, err := file.Write(p)
	if time.Since(lastSync) >= time.Second {
		_ = file.Sync()
		lastSync = time.Now()
	}
	return n, err
}

func openLogMutex() windows.Handle {
	name, err := windows.UTF16PtrFromString(`Local\AutoScreenRecorder_LogWrite`)
	if err != nil {
		return 0
	}
	h, err := windows.CreateMutex(nil, false, name)
	if h == 0 {
		return 0
	}
	if err != nil && !errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		windows.CloseHandle(h)
		return 0
	}
	return h
}

// Init writes Go log output to stderr and AutoScreenRecorder.log next to the executable.
func Init(exeDir string) string {
	path := filepath.Join(exeDir, "AutoScreenRecorder.log")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		log.Printf("applog: cannot open %s: %v", path, err)
		return path
	}
	file = f
	logMutex.Store(uintptr(openLogMutex()))
	mw := io.MultiWriter(os.Stderr, flushWriter{})
	log.SetOutput(mw)
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)
	log.Printf("===== session start pid=%d log=%s =====", os.Getpid(), path)
	wincap.InitCrashLog(path)
	return path
}

// Close closes the log file if opened.
func Close() {
	h := windows.Handle(logMutex.Load())
	if h != 0 {
		wr, err := windows.WaitForSingleObject(h, 5000)
		if err != nil || (wr != windows.WAIT_OBJECT_0 && wr != windows.WAIT_ABANDONED) {
			return
		}
	}
	fileMu.Lock()
	if file != nil {
		_ = file.Sync()
		_ = file.Close()
		file = nil
	}
	logMutex.Store(0)
	fileMu.Unlock()
	if h != 0 {
		windows.ReleaseMutex(h)
		windows.CloseHandle(h)
	}
}

// LogPanic logs a recovered panic with stack (for goroutines).
func LogPanic(where string, r any) {
	log.Printf("panic in %s: %v\n%s", where, r, debug.Stack())
}
