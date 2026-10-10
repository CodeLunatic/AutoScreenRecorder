//go:build windows

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
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

	exePathEarly, errExe := os.Executable()
	if errExe != nil {
		log.Printf("读取程序路径失败: %v", errExe)
		exePathEarly = ""
	}
	logDir := filepath.Dir(exePathEarly)
	if exePathEarly == "" {
		logDir = "."
	}
	logPath := applog.Init(logDir)
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
		log.Printf("读取配置失败，改用默认配置: %v", err)
		cfg = config.Default()
	}
	if err := cfg.Validate(); err != nil {
		log.Printf("配置无效，改用默认配置: %v", err)
		cfg = config.Default()
	}
	if err := output.EnsureDir(cfg); err != nil {
		log.Printf("创建保存目录失败，开录时会再试: %v", err)
	}

	exePath := exePathEarly
	notify.Init(exePath)

	rec := wincap.NewNativeRecorder()
	ctrl := controller.New(rec, func(processName string, start time.Time) string {
		return output.BuildSessionPath(cfg, processName, start)
	}, cfg, func(finalPath string) {
		go safeCall("保存通知", func() {
			log.Printf("通知: 录屏已保存 %s", finalPath)
			if err := notify.RecordingSaved(exePath, finalPath); err != nil {
				log.Printf("保存通知失败: %v", err)
			}
		})
	})
	ctrl.SetOnFailed(func(msg string) {
		go safeCall("失败通知", func() {
			log.Printf("通知: 录屏失败 %s", msg)
			if err := notify.RecordingFailed(exePath, msg); err != nil {
				log.Printf("失败通知发送失败: %v", err)
			}
		})
	})
	ctrl.SetReinit(func() error {
		return wincap.Restart()
	})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			func() {
				defer func() {
					r := recover()
					if r == nil {
						return
					}
					applog.LogPanic("controller", r)
					log.Printf("控制器异常，准备重新初始化: %v", r)
					func() {
						defer func() {
							if r2 := recover(); r2 != nil {
								applog.LogPanic("controller recover", r2)
							}
						}()
						ctrl.Recover(fmt.Sprint(r))
					}()
				}()
				ctrl.Run(ctx)
			}()
			if ctx.Err() != nil {
				return
			}
			log.Printf("控制器已退出，1 秒后重新运行")
			select {
			case <-ctx.Done():
				return
			case <-time.After(time.Second):
			}
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)

	var lastNativeFail time.Time
	for {
		err := wincap.Start(
			func(event int, pid uint32) { handleMic(ctrl, event, pid) },
			func(from, to int, hmon uint64) {
				ctrl.PostMonitor(controller.MonitorEvent{FromIndex: from, ToIndex: to, Handle: hmon})
			},
		)
		if err == nil {
			break
		}
		log.Printf("原生服务启动失败，5 秒后重试: %v", err)
		wincap.NativeTrace("Go: native start failed: " + err.Error())
		if time.Since(lastNativeFail) >= 30*time.Second {
			lastNativeFail = time.Now()
			msg := err.Error()
			go safeCall("失败通知", func() {
				if nerr := notify.RecordingFailed(exePath, msg); nerr != nil {
					log.Printf("失败通知发送失败: %v", nerr)
				}
			})
		}
		select {
		case <-sig:
			cancel()
			wg.Wait()
			log.Println("shutting down")
			return
		case <-time.After(5 * time.Second):
		}
	}
	defer func() {
		safeCall("native stop", wincap.Stop)
	}()

	log.Printf("AutoScreenRecorder running (config: %s)", *cfgPath)
	<-sig
	cancel()
	wg.Wait()
	log.Println("shutting down")
}

func safeCall(where string, fn func()) {
	defer func() {
		if r := recover(); r != nil {
			applog.LogPanic(where, r)
		}
	}()
	fn()
}

func handleMic(ctrl *controller.Controller, event int, pid uint32) {
	defer func() {
		if r := recover(); r != nil {
			applog.LogPanic("handleMic", r)
		}
	}()
	cfg := ctrl.Config()
	switch event {
	case 1:
		ok, exe, err := process.MatchesAnyWatch(pid, cfg.MicTrigger.Watch)
		if err != nil {
			log.Printf("查询占用麦克风的进程失败 pid=%d err=%v", pid, err)
			return
		}
		if !ok {
			return
		}
		log.Printf("名单内程序占用麦克风 pid=%d exe=%s", pid, exe)
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
