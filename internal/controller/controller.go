package controller

import (
	"context"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"AutoScreenRecorder/internal/config"
)

type MicEventType int

const (
	MicActive MicEventType = iota + 1
	MicReleased
)

type MicEvent struct {
	Type MicEventType
	PID  uint32
	Exe  string
}

type MonitorEvent struct {
	FromIndex int
	ToIndex   int
	Handle    uint64
}

type Recorder interface {
	Start(ctx context.Context, path string, monitorHandle uint64, cfg config.Config) error
	SetMonitor(monitorHandle uint64) error
	Stop(ctx context.Context) (finalPath string, err error)
	CurrentMonitorHandle() uint64
}

type micUse struct {
	exe  string
	refs int
}

type Controller struct {
	cfgMu sync.RWMutex
	cfg   config.Config

	recorder Recorder
	pathGen  func(process string, start time.Time) string
	onSaved  func(finalPath string)
	onFailed func(string)
	reinit   func() error

	failStreak  int
	lastFailMsg string
	lastFailAt  time.Time

	events chan any

	micMu sync.Mutex
	mics  map[uint32]*micUse
	dirty bool

	monMu    sync.Mutex
	monDirty bool
	latest   MonitorEvent

	activeMic map[uint32]string
	recording bool

	startTimer *time.Timer
	stopTimer  *time.Timer
	startGen   uint64
	stopGen    uint64

	pendingMu       sync.Mutex
	hasPendingStart bool
	pendingStart    startNow
	hasPendingStop  bool
	pendingStop     stopNow

	sessionStart time.Time
	lastProcess  string
}

func New(rec Recorder, pathGen func(process string, start time.Time) string, cfg config.Config, onSaved func(finalPath string)) *Controller {
	return &Controller{
		cfg:       cfg,
		recorder:  rec,
		pathGen:   pathGen,
		onSaved:   onSaved,
		events:    make(chan any, 64),
		mics:      make(map[uint32]*micUse),
		activeMic: make(map[uint32]string),
	}
}

func (c *Controller) SetOnFailed(fn func(string)) {
	c.onFailed = fn
}

func (c *Controller) SetReinit(fn func() error) {
	c.reinit = fn
}

// Recover 在控制器崩溃后清掉会话状态，并重新拉起采集。
func (c *Controller) Recover(reason string) {
	c.reinitialize(reason)
}

func (c *Controller) UpdateConfig(cfg config.Config) {
	c.cfgMu.Lock()
	c.cfg = cfg
	c.cfgMu.Unlock()
}

func (c *Controller) Config() config.Config {
	c.cfgMu.RLock()
	defer c.cfgMu.RUnlock()
	return c.cfg
}

func (c *Controller) poke() {
	select {
	case c.events <- poke{}:
	default:
	}
}

func (c *Controller) PostMic(ev MicEvent) {
	c.micMu.Lock()
	switch ev.Type {
	case MicActive:
		u := c.mics[ev.PID]
		if u == nil {
			u = &micUse{}
			c.mics[ev.PID] = u
		}
		if ev.Exe != "" {
			u.exe = ev.Exe
		}
		u.refs++
		c.dirty = true
	case MicReleased:
		u := c.mics[ev.PID]
		if u == nil || u.refs <= 0 {
			c.micMu.Unlock()
			return
		}
		u.refs--
		if u.refs == 0 {
			delete(c.mics, ev.PID)
		}
		c.dirty = true
	default:
		c.micMu.Unlock()
		return
	}
	c.micMu.Unlock()
	c.poke()
}

func (c *Controller) PostMonitor(ev MonitorEvent) {
	c.monMu.Lock()
	c.latest = ev
	c.monDirty = true
	c.monMu.Unlock()
	c.poke()
}

func (c *Controller) enqueue(ev any) {
	select {
	case c.events <- ev:
	default:
		c.pendingMu.Lock()
		switch e := ev.(type) {
		case startNow:
			c.hasPendingStart = true
			c.pendingStart = e
		case stopNow:
			c.hasPendingStop = true
			c.pendingStop = e
		}
		c.pendingMu.Unlock()
	}
}

func (c *Controller) Run(ctx context.Context) {
	for {
		c.syncMics()
		c.syncMonitor()
		c.firePending()
		select {
		case <-ctx.Done():
			if c.recording {
				c.stopRecording(context.Background())
			}
			return
		case ev := <-c.events:
			switch e := ev.(type) {
			case startNow:
				c.handleStartNow(e)
			case stopNow:
				c.handleStopNow(e)
			case poke:
			}
		}
	}
}

func (c *Controller) firePending() {
	c.pendingMu.Lock()
	var st *startNow
	var sp *stopNow
	if c.hasPendingStart {
		e := c.pendingStart
		st = &e
		c.hasPendingStart = false
	}
	if c.hasPendingStop {
		e := c.pendingStop
		sp = &e
		c.hasPendingStop = false
	}
	c.pendingMu.Unlock()
	if st != nil {
		c.handleStartNow(*st)
	}
	if sp != nil {
		c.handleStopNow(*sp)
	}
}

func (c *Controller) syncMics() {
	c.micMu.Lock()
	if !c.dirty {
		c.micMu.Unlock()
		return
	}
	c.dirty = false
	snap := make(map[uint32]string, len(c.mics))
	for pid, u := range c.mics {
		if u != nil && u.refs > 0 {
			snap[pid] = u.exe
		}
	}
	c.micMu.Unlock()

	c.activeMic = snap
	if len(c.activeMic) > 0 {
		c.invalidateStop()
		if !c.recording && c.startTimer == nil {
			c.scheduleStart(c.currentProcess(), c.startDelay())
		}
		return
	}
	c.invalidateStart()
	if c.recording {
		c.scheduleStop()
	}
}

func (c *Controller) syncMonitor() {
	c.monMu.Lock()
	if !c.monDirty {
		c.monMu.Unlock()
		return
	}
	ev := c.latest
	c.monDirty = false
	c.monMu.Unlock()
	c.onMonitor(ev)
}

func (c *Controller) onMonitor(ev MonitorEvent) {
	cfg := c.Config()
	if !cfg.Record.FollowMouse || !c.recording {
		return
	}
	if ev.ToIndex < 0 || ev.FromIndex == ev.ToIndex {
		return
	}
	log.Printf("鼠标已切换到显示器 %d（来自 %d），录屏画面跟随", ev.ToIndex, ev.FromIndex)
	if err := c.recorder.SetMonitor(ev.Handle); err != nil {
		log.Printf("切换录屏显示器失败: %v", err)
	}
}

func (c *Controller) startDelay() time.Duration {
	cfg := c.Config()
	if cfg.Service.StartDelaySec < 0 {
		return 0
	}
	return time.Duration(cfg.Service.StartDelaySec) * time.Second
}

func (c *Controller) currentProcess() string {
	if c.lastProcess != "" {
		for _, exe := range c.activeMic {
			if exe == c.lastProcess {
				return exe
			}
		}
	}
	for _, exe := range c.activeMic {
		if exe != "" {
			return exe
		}
	}
	return "unknown"
}

func (c *Controller) scheduleStart(process string, delay time.Duration) {
	c.invalidateStop()
	c.startGen++
	gen := c.startGen
	if c.startTimer != nil {
		c.startTimer.Stop()
		c.startTimer = nil
	}
	c.startTimer = time.AfterFunc(delay, func() {
		c.enqueue(startNow{process: process, gen: gen})
	})
}

func (c *Controller) scheduleStop() {
	c.invalidateStart()
	c.stopGen++
	gen := c.stopGen
	if c.stopTimer != nil {
		c.stopTimer.Stop()
		c.stopTimer = nil
	}
	cfg := c.Config()
	delay := time.Duration(cfg.Service.StopDelaySec) * time.Second
	if delay < 0 {
		delay = 0
	}
	c.stopTimer = time.AfterFunc(delay, func() {
		c.enqueue(stopNow{gen: gen})
	})
}

func (c *Controller) invalidateStart() {
	c.startGen++
	if c.startTimer != nil {
		c.startTimer.Stop()
		c.startTimer = nil
	}
}

func (c *Controller) invalidateStop() {
	c.stopGen++
	if c.stopTimer != nil {
		c.stopTimer.Stop()
		c.stopTimer = nil
	}
}

type startNow struct {
	process string
	gen     uint64
}

type stopNow struct {
	gen uint64
}

type poke struct{}

func (c *Controller) handleStartNow(e startNow) {
	if e.gen != c.startGen {
		return
	}
	c.startTimer = nil
	if c.recording || len(c.activeMic) == 0 {
		return
	}
	cfg := c.Config()
	c.sessionStart = time.Now()
	if e.process != "" {
		c.lastProcess = e.process
	} else {
		c.lastProcess = c.currentProcess()
	}
	mon := c.recorder.CurrentMonitorHandle()
	path := c.pathGen(c.lastProcess, c.sessionStart)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		log.Printf("创建保存目录失败 path=%s err=%v", path, err)
		c.noteFailure(fmtErr("创建保存目录失败: %v", err))
		return
	}
	log.Printf("准备开录 process=%s path=%s monitor=%d", c.lastProcess, path, mon)
	if err := c.recorder.Start(context.Background(), path, mon, cfg); err != nil {
		log.Printf("开录失败 process=%s path=%s err=%v", c.lastProcess, path, err)
		c.noteFailure(err)
		return
	}
	c.failStreak = 0
	c.lastFailMsg = ""
	c.recording = true
	log.Printf("录制已开始: %s", path)
}

func fmtErr(format string, err error) error {
	return &wrappedErr{s: sprintf(format, err)}
}

func sprintf(format string, err error) string {
	return strings.ReplaceAll(format, "%v", err.Error())
}

type wrappedErr struct{ s string }

func (e *wrappedErr) Error() string { return e.s }

func (c *Controller) noteFailure(err error) {
	if err == nil {
		return
	}
	msg := err.Error()
	log.Printf("录制失败: %s", msg)
	c.reportFailure(msg)
	c.failStreak++
	stuck := strings.Contains(msg, "还没结束")
	if stuck || c.failStreak >= 3 {
		c.reinitialize(msg)
		c.failStreak = 0
		return
	}
	c.retryStartWhileMicHeld()
}

func (c *Controller) reportFailure(msg string) {
	now := time.Now()
	if msg == c.lastFailMsg && now.Sub(c.lastFailAt) < 30*time.Second {
		log.Printf("相同失败 30 秒内不再重复通知")
		return
	}
	c.lastFailMsg = msg
	c.lastFailAt = now
	if c.onFailed != nil {
		c.onFailed(msg)
	}
}

func (c *Controller) reinitialize(reason string) {
	log.Printf("重新初始化: %s", reason)
	c.recording = false
	c.invalidateStart()
	c.invalidateStop()
	c.micMu.Lock()
	c.mics = make(map[uint32]*micUse)
	c.dirty = false
	c.micMu.Unlock()
	c.activeMic = make(map[uint32]string)
	if c.reinit == nil {
		c.retryStartWhileMicHeld()
		return
	}
	if err := c.reinit(); err != nil {
		log.Printf("重新初始化失败: %v", err)
		c.reportFailure("重新初始化失败: " + err.Error())
	}
}

func (c *Controller) retryStartWhileMicHeld() {
	if len(c.activeMic) == 0 {
		return
	}
	retry := c.startDelay()
	if retry < time.Second {
		retry = time.Second
	}
	log.Printf("麦克风仍在占用，%s 后重试开录", retry)
	c.scheduleStart(c.currentProcess(), retry)
}

func (c *Controller) handleStopNow(e stopNow) {
	if e.gen != c.stopGen {
		return
	}
	c.stopTimer = nil
	if len(c.activeMic) == 0 {
		c.stopRecording(context.Background())
	}
}

func (c *Controller) stopRecording(ctx context.Context) {
	c.invalidateStart()
	c.invalidateStop()
	if !c.recording {
		return
	}
	finalPath, err := c.recorder.Stop(ctx)
	c.recording = false
	if err != nil {
		log.Printf("停止录制失败: %v", err)
		c.reportFailure("停止录制失败: " + err.Error())
		return
	}
	if finalPath != "" {
		log.Printf("recording saved: %s", finalPath)
		if c.onSaved != nil {
			c.onSaved(finalPath)
		}
	}
}
