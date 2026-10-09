//go:build windows

package notify

import "testing"

func TestCDATATextSplitsTerminator(t *testing.T) {
	got := cdataText("a]]>b\x00")
	if got != "a]] >b" {
		t.Fatal(got)
	}
}

func TestOpenVideoLaunchHidesConsole(t *testing.T) {
	if !IsOpenVideoLaunch([]string{"-open-video", "autorecorder://open/C:/a.wmv"}) {
		t.Fatal("two args")
	}
	if !IsOpenVideoLaunch([]string{"-open-video=autorecorder://open/C:/a.wmv"}) {
		t.Fatal("equals")
	}
	if IsOpenVideoLaunch([]string{"-config", "config.yaml"}) {
		t.Fatal("config")
	}
}

func TestDirectOpenVideoCommand(t *testing.T) {
	cmd := directOpenVideoCommand(`C:\App\AutoScreenRecorder.exe`)
	if cmd != `"C:\App\AutoScreenRecorder.exe" -open-video "%1"` {
		t.Fatal(cmd)
	}
}

func TestTitleMatchesFolder(t *testing.T) {
	if !titleMatchesFolder("AutoScreenRecorder", "AutoScreenRecorder") {
		t.Fatal("exact")
	}
	if !titleMatchesFolder("AutoScreenRecorder - 文件资源管理器", "AutoScreenRecorder") {
		t.Fatal("suffix")
	}
	if !titleMatchesFolder(`C:\Users\dnydi\Videos\AutoScreenRecorder`, "AutoScreenRecorder") {
		t.Fatal("full path")
	}
	if titleMatchesFolder("AutoScreenRecorderExtra", "AutoScreenRecorder") {
		t.Fatal("prefix boundary")
	}
}

func TestXMLAttrEscapes(t *testing.T) {
	got := xmlAttr(`a&b<c>d"e`)
	if got != "a&amp;b&lt;c&gt;d&quot;e" {
		t.Fatal(got)
	}
}
