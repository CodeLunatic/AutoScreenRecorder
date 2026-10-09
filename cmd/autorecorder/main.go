//go:build windows

package main

import (
	"context"
	"errors"
	"flag"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"AutoScreenRecorder/internal/applog"
	"AutoScreenRecorder/internal/config"
	"AutoScreenRecorder/internal/controller"
	"AutoScreenRecorder/internal/notify"
	"AutoScreenRecorder/internal/output"
	"AutoScreenRecorder/internal/process"
	"AutoScreenRecorder/internal/singleinstance"
	"AutoScreenRecorder/internal/wincap"

	"golang.org/x/sys/windows"
)

func main() {
	if notify.IsOpenVideoLaunch(os.Args[1:]) {
		notify.HideConsoleWindow()
	} else {
		notify.AttachParentConsole()
	}
	enablePerMonitorDPI()

	cfgPath := flag.String("config", defaultConfigPath(), "path to config yaml")
	openVideo := flag.String("open-video", "", "open saved video in Explorer (from toast activation)")
	flag.Parse()

	if *openVideo != "" {
		path := notify.ParseOpenVideoArg(*openVideo)
		cfg, err := config.Load(*cfgPath)
		if err != nil && !errors.Is(err, config.ErrFileMissing) {
			log.Printf("open video: config: %v", err)
			os.Exit(1)
		}
		if !output.WithinDir(cfg, path) {
			log.Printf("open video: refused path outside output dir")
			os.Exit(1)
		}
		if err := notify.RevealInExplorer(path); err != nil {
			log.Printf("open video location: %v", err)
			os.Exit(1)
		}
		os.Exit(0)
	}

	exePathEarly, _ := os.Executable()
	logPath := applog.Init(filepath.Dir(exePathEarly))
	defer applog.Close()
	log.Printf("诊断日志: %s", logPath)

	release, err := singleinstance.Acquire()
	if err != nil {
		log.Fatal(err)
	}
	defer release()

	cfg, err := config.Load(*cfgPath)
	if errors.Is(err, config.ErrFileMissing) {
		log.Printf("配置文件不存在，使用默认值: %s", *cfgPath)
	} else if err != nil {
		log.Fatalf("load config: %v", err)
	}
	if err := cfg.Validate(); err != nil {
		log.Fatalf("invalid config: %v", err)
	}
	if err := output.EnsureDir(cfg); err != nil {
		log.Fatalf("output dir: %v", err)
	}

	exePath, err := os.Executable()
	if err != nil {
		log.Fatalf("executable path: %v", err)
	}
	notify.Init(exePath)

	rec := wincap.NewNativeRecorder()
	ctrl := controller.New(rec, func(processName string, start time.Time) string {
		return output.BuildSessionPath(cfg, processName, start)
	}, cfg, func(finalPath string) {
		path := finalPath
		go func() {
			defer func() {
				if r := recover(); r != nil {
					applog.LogPanic("RecordingSaved toast", r)
				}
			}()
			log.Printf("toast: showing for %s", path)
			if err := notify.RecordingSaved(exePath, path); err != nil {
				log.Printf("notification: %v", err)
			}
			log.Printf("toast: done for %s", path)
		}()
	})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		ctrl.Run(ctx)
	}()

	if err := wincap.Start(
		func(event int, pid uint32) { handleMic(ctrl, event, pid) },
		func(from, to int, hmon uint64) {
			ctrl.PostMonitor(controller.MonitorEvent{FromIndex: from, ToIndex: to, Handle: hmon})
		},
	); err != nil {
		log.Fatalf("native start: %v", err)
	}
	defer wincap.Stop()

	log.Printf("AutoScreenRecorder running (config: %s)", *cfgPath)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig
	cancel()
	wg.Wait()
	log.Println("shutting down")
}

func handleMic(ctrl *controller.Controller, event int, pid uint32) {
	cfg := ctrl.Config()
	switch event {
	case 1:
		ok, exe, err := process.MatchesAnyWatch(pid, cfg.MicTrigger.Watch)
		if err != nil || !ok {
			return
		}
		ctrl.PostMic(controller.MicEvent{Type: controller.MicActive, PID: pid, Exe: exe})
	case 2:
		ctrl.PostMic(controller.MicEvent{Type: controller.MicReleased, PID: pid})
	}
}

func enablePerMonitorDPI() {
	user32 := windows.NewLazySystemDLL("user32.dll")
	proc := user32.NewProc("SetProcessDpiAwarenessContext")
	if err := proc.Find(); err != nil {
		log.Printf("DPI awareness: %v", err)
		return
	}
	// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
	r, _, _ := proc.Call(^uintptr(3))
	if r == 0 {
		log.Printf("DPI awareness: SetProcessDpiAwarenessContext failed")
	}
}

func defaultConfigPath() string {
	exe, err := os.Executable()
	if err != nil {
		return "config.yaml"
	}
	return filepath.Join(filepath.Dir(exe), "config.yaml")
}
