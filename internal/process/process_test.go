//go:build windows

package process

import (
	"testing"

	"AutoScreenRecorder/internal/config"
)

func TestPathPrefixBoundary(t *testing.T) {
	rule := config.ProcessRule{Name: "Lark.exe", PathPrefix: `C:\App\Lark`}
	if !MatchesWatchRule("Lark.exe", `C:\App\Lark\Lark.exe`, rule) {
		t.Fatal("expected prefix match")
	}
	if MatchesWatchRule("Lark.exe", `C:\App\LarkExtra\Lark.exe`, rule) {
		t.Fatal("prefix matched a different directory")
	}
	if !MatchesWatchRule("lark.exe", `c:\app\lark`, rule) {
		t.Fatal("expected case-insensitive exact directory")
	}
	open := config.ProcessRule{Name: "Lark.exe"}
	if !MatchesWatchRule("Lark.exe", `D:\anywhere\Lark.exe`, open) {
		t.Fatal("empty prefix should match")
	}
}
