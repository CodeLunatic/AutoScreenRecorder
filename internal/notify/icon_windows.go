//go:build windows

package notify

import (
	_ "embed"
	"encoding/binary"
	"os"
	"path/filepath"
)

//go:embed icon.png
var iconPNG []byte

var toastIcon string

func installAssets(exePath string) error {
	base, err := os.UserCacheDir()
	if err != nil {
		return err
	}
	dir := filepath.Join(base, "AutoScreenRecorder")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	pngPath := filepath.Join(dir, "icon.png")
	icoPath := filepath.Join(dir, "icon.ico")
	if err := os.WriteFile(pngPath, iconPNG, 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(icoPath, pngToICO(iconPNG), 0o644); err != nil {
		return err
	}
	toastIcon = "file:///" + filepath.ToSlash(pngPath)
	if err := ensureStartMenuShortcut(exePath, icoPath); err != nil {
		return err
	}
	setExplicitAppUserModelID(appID)
	return nil
}

// pngToICO wraps a PNG in a Vista-style icon container. 0 means 256px.
func pngToICO(png []byte) []byte {
	width, height := pngSize(png)
	var wb, hb byte
	if width > 0 && width < 256 {
		wb = byte(width)
	}
	if height > 0 && height < 256 {
		hb = byte(height)
	}
	buf := make([]byte, 0, 22+len(png))
	hdr := make([]byte, 6)
	binary.LittleEndian.PutUint16(hdr[2:], 1)
	binary.LittleEndian.PutUint16(hdr[4:], 1)
	buf = append(buf, hdr...)
	entry := make([]byte, 16)
	entry[0] = wb
	entry[1] = hb
	binary.LittleEndian.PutUint16(entry[4:], 1)
	binary.LittleEndian.PutUint16(entry[6:], 32)
	binary.LittleEndian.PutUint32(entry[8:], uint32(len(png)))
	binary.LittleEndian.PutUint32(entry[12:], 22)
	buf = append(buf, entry...)
	buf = append(buf, png...)
	return buf
}

func pngSize(png []byte) (int, int) {
	if len(png) < 24 || string(png[12:16]) != "IHDR" {
		return 0, 0
	}
	return int(binary.BigEndian.Uint32(png[16:20])), int(binary.BigEndian.Uint32(png[20:24]))
}
