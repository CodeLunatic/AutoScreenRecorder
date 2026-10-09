//go:build windows

package notify

import (
	"os"
	"path/filepath"
	"testing"
)

func TestPNGToICO(t *testing.T) {
	ico := pngToICO(iconPNG)
	if len(ico) != 22+len(iconPNG) {
		t.Fatalf("ico length %d", len(ico))
	}
	if ico[2] != 1 || ico[4] != 1 {
		t.Fatalf("ico header %v", ico[:6])
	}
	w, h := pngSize(iconPNG)
	if w != 256 || h != 256 {
		t.Fatalf("png size %dx%d", w, h)
	}
	if ico[6] != 0 || ico[7] != 0 {
		t.Fatalf("256px icon dimensions should be stored as 0, got %d %d", ico[6], ico[7])
	}
}

func TestInstallAssets(t *testing.T) {
	exe, err := filepath.Abs(filepath.Join("..", "..", "AutoScreenRecorder.exe"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(exe); err != nil {
		t.Skip(err)
	}
	if err := installAssets(exe); err != nil {
		t.Fatal(err)
	}
	if toastIcon == "" {
		t.Fatal("toast icon path empty")
	}
	pngPath := filepath.Join(mustCache(t), "AutoScreenRecorder", "icon.png")
	if _, err := os.Stat(pngPath); err != nil {
		t.Fatal(err)
	}
	lnk := filepath.Join(os.Getenv("APPDATA"), "Microsoft", "Windows", "Start Menu", "Programs", "AutoScreenRecorder.lnk")
	if _, err := os.Stat(lnk); err != nil {
		t.Fatal(err)
	}
}

func mustCache(t *testing.T) string {
	t.Helper()
	dir, err := os.UserCacheDir()
	if err != nil {
		t.Fatal(err)
	}
	return dir
}
