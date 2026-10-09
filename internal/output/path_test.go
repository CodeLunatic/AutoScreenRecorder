package output

import (
	"path/filepath"
	"strings"
	"testing"
	"time"

	"AutoScreenRecorder/internal/config"
)

func TestForceContainerExtension(t *testing.T) {
	if got := forceContainerExtension("202601011200_Lark.wmv", ".mp4"); got != "202601011200_Lark.mp4" {
		t.Fatal(got)
	}
	if got := forceContainerExtension("clip", ".wmv"); got != "clip.wmv" {
		t.Fatal(got)
	}
	if got := forceContainerExtension("clip.ASF", ".mp4"); got != "clip.mp4" {
		t.Fatal(got)
	}
}

func TestBuildSessionPathFollowsContainer(t *testing.T) {
	cfg := config.Default()
	cfg.Output.Dir = t.TempDir()
	cfg.Output.Template = "{datetime}_{process}.wmv"
	cfg.Record.Container = "mp4"
	path := BuildSessionPath(cfg, "Lark.exe", time.Date(2026, 10, 8, 17, 57, 0, 0, time.Local))
	if !strings.HasSuffix(strings.ToLower(path), ".mp4") {
		t.Fatal(path)
	}
}

func TestWithinDir(t *testing.T) {
	root := t.TempDir()
	cfg := config.Default()
	cfg.Output.Dir = root
	inside := filepath.Join(root, "a.wmv")
	if !WithinDir(cfg, inside) {
		t.Fatal("inside")
	}
	if WithinDir(cfg, filepath.Join(root, "..", "outside.wmv")) {
		t.Fatal("outside")
	}
	if WithinDir(cfg, "") {
		t.Fatal("empty")
	}
}
