package output

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"AutoScreenRecorder/internal/config"
)

func EnsureDir(cfg config.Config) error {
	dir := cfg.ExpandOutputDir()
	return os.MkdirAll(dir, 0o755)
}

// RecordExtension 由 record.container 决定默认扩展名。
func RecordExtension(cfg config.Config) string {
	switch strings.ToLower(strings.TrimSpace(cfg.Record.Container)) {
	case "mp4", "h264":
		return ".mp4"
	default:
		return ".wmv"
	}
}

// BuildSessionPath 单次录制最终成片，例如 202610081757_Lark.wmv
func BuildSessionPath(cfg config.Config, process string, start time.Time) string {
	dir := cfg.ExpandOutputDir()
	tpl := cfg.Output.Template
	repl := strings.NewReplacer(
		"{datetime}", start.Format("200601021504"),
		"{date}", start.Format("2006-01-02"),
		"{time}", start.Format("15-04-05"),
		"{process}", sanitize(process),
	)
	name := repl.Replace(tpl)
	name = forceContainerExtension(name, RecordExtension(cfg))
	return uniquePath(filepath.Join(dir, name))
}

// forceContainerExtension 让成片扩展名跟 record.container 一致，避免 MP4 内容落成 .wmv。
func forceContainerExtension(name, ext string) string {
	lower := strings.ToLower(name)
	for _, known := range []string{".mp4", ".wmv", ".asf"} {
		if strings.HasSuffix(lower, known) {
			return name[:len(name)-len(known)] + ext
		}
	}
	return name + ext
}

// WithinDir reports whether path is dir itself or a file inside dir.
func WithinDir(cfg config.Config, path string) bool {
	dir := filepath.Clean(cfg.ExpandOutputDir())
	p := filepath.Clean(path)
	if dir == "" || p == "" || p == "." {
		return false
	}
	rel, err := filepath.Rel(dir, p)
	if err != nil {
		return false
	}
	if rel == ".." || strings.HasPrefix(rel, ".."+string(os.PathSeparator)) {
		return false
	}
	return true
}

// uniquePath 避免同一分钟内的下一次录制覆盖已有文件：name.mp4、name_2.mp4、name_3.mp4。
func uniquePath(path string) string {
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return path
	}
	ext := filepath.Ext(path)
	base := strings.TrimSuffix(path, ext)
	for i := 2; i < 1000; i++ {
		candidate := fmt.Sprintf("%s_%d%s", base, i, ext)
		if _, err := os.Stat(candidate); os.IsNotExist(err) {
			return candidate
		}
	}
	return fmt.Sprintf("%s_%d%s", base, time.Now().UnixNano(), ext)
}

func sanitize(s string) string {
	if ext := filepath.Ext(s); strings.EqualFold(ext, ".exe") {
		s = strings.TrimSuffix(s, ext)
	}
	for _, r := range []string{`<`, `>`, `:`, `"`, `/`, `\`, `|`, `?`, `*`} {
		s = strings.ReplaceAll(s, r, "_")
	}
	if s == "" {
		return "unknown"
	}
	return s
}
