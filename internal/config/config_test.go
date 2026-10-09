package config

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestValidateDelays(t *testing.T) {
	cfg := Default()
	cfg.Service.StartDelaySec = -1
	if err := cfg.Validate(); err == nil {
		t.Fatal("expected negative start delay to fail")
	}
	cfg = Default()
	cfg.Service.StopDelaySec = -1
	if err := cfg.Validate(); err == nil {
		t.Fatal("expected negative stop delay to fail")
	}
	cfg = Default()
	cfg.Record.VideoBitrateKbps = -5
	if err := cfg.Validate(); err == nil {
		t.Fatal("expected negative bitrate to fail")
	}
}

func TestLoadMissingFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "no-such.yaml")
	cfg, err := Load(path)
	if !errors.Is(err, ErrFileMissing) {
		t.Fatalf("err=%v", err)
	}
	if cfg.Record.FPS != Default().Record.FPS {
		t.Fatalf("fps=%d", cfg.Record.FPS)
	}
	if _, statErr := os.Stat(path); !errors.Is(statErr, os.ErrNotExist) {
		t.Fatal("load created a file")
	}
}
