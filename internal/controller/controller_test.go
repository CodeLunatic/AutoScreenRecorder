package controller

import (
	"context"
	"sync"
	"testing"
	"time"

	"AutoScreenRecorder/internal/config"
)

type fakeRecorder struct {
	mu         sync.Mutex
	starts     int
	stops      int
	failLeft   int
	startBlock chan struct{}
	mon        uint64
}

func (f *fakeRecorder) Start(context.Context, string, uint64, config.Config) error {
	if f.startBlock != nil {
		<-f.startBlock
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.starts++
	if f.failLeft > 0 {
		f.failLeft--
		return errStart
	}
	return nil
}

func (f *fakeRecorder) SetMonitor(uint64) error { return nil }
func (f *fakeRecorder) Stop(context.Context) (string, error) {
	f.mu.Lock()
	f.stops++
	f.mu.Unlock()
	return "saved.wmv", nil
}
func (f *fakeRecorder) CurrentMonitorHandle() uint64 { return f.mon }

type startErr struct{}

func (startErr) Error() string { return "start failed" }

var errStart startErr

func testCfg() config.Config {
	cfg := config.Default()
	cfg.Service.StartDelaySec = 0
	cfg.Service.StopDelaySec = 0
	return cfg
}

func waitUntil(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("condition not met")
}

func TestMicRefcountKeepsRecordingUntilLastSession(t *testing.T) {
	rec := &fakeRecorder{}
	ctrl := New(rec, func(string, time.Time) string { return "out.wmv" }, testCfg(), nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go ctrl.Run(ctx)

	ctrl.PostMic(MicEvent{Type: MicActive, PID: 10, Exe: "Lark.exe"})
	ctrl.PostMic(MicEvent{Type: MicActive, PID: 10, Exe: "Lark.exe"})
	waitUntil(t, func() bool {
		rec.mu.Lock()
		defer rec.mu.Unlock()
		return rec.starts == 1
	})

	ctrl.PostMic(MicEvent{Type: MicReleased, PID: 10})
	time.Sleep(50 * time.Millisecond)
	rec.mu.Lock()
	stops := rec.stops
	rec.mu.Unlock()
	if stops != 0 {
		t.Fatalf("recording stopped while another session was active, stops=%d", stops)
	}

	ctrl.PostMic(MicEvent{Type: MicReleased, PID: 10})
	waitUntil(t, func() bool {
		rec.mu.Lock()
		defer rec.mu.Unlock()
		return rec.stops == 1
	})
}

func TestReleaseBeforeStartDoesNotRecord(t *testing.T) {
	rec := &fakeRecorder{}
	cfg := testCfg()
	cfg.Service.StartDelaySec = 1
	ctrl := New(rec, func(string, time.Time) string { return "out.wmv" }, cfg, nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go ctrl.Run(ctx)

	ctrl.PostMic(MicEvent{Type: MicActive, PID: 3, Exe: "Lark.exe"})
	time.Sleep(20 * time.Millisecond)
	ctrl.PostMic(MicEvent{Type: MicReleased, PID: 3})
	time.Sleep(50 * time.Millisecond)
	rec.mu.Lock()
	starts := rec.starts
	rec.mu.Unlock()
	if starts != 0 {
		t.Fatalf("started after mic released, starts=%d", starts)
	}
}

func TestStartFailureRetriesWhileMicActive(t *testing.T) {
	rec := &fakeRecorder{failLeft: 1}
	ctrl := New(rec, func(string, time.Time) string { return "out.wmv" }, testCfg(), nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go ctrl.Run(ctx)

	ctrl.PostMic(MicEvent{Type: MicActive, PID: 8, Exe: "Feishu.exe"})
	waitUntil(t, func() bool {
		rec.mu.Lock()
		defer rec.mu.Unlock()
		return rec.starts >= 2
	})
}

func TestExtraReleaseIsIgnored(t *testing.T) {
	rec := &fakeRecorder{}
	ctrl := New(rec, func(string, time.Time) string { return "out.wmv" }, testCfg(), nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go ctrl.Run(ctx)

	ctrl.PostMic(MicEvent{Type: MicReleased, PID: 99})
	ctrl.PostMic(MicEvent{Type: MicActive, PID: 4, Exe: "Lark.exe"})
	waitUntil(t, func() bool {
		rec.mu.Lock()
		defer rec.mu.Unlock()
		return rec.starts == 1
	})
	time.Sleep(30 * time.Millisecond)
	rec.mu.Lock()
	stops := rec.stops
	rec.mu.Unlock()
	if stops != 0 {
		t.Fatalf("unexpected stop, stops=%d", stops)
	}
}
