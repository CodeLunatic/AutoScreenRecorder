//go:build windows

package process

import (
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"

	"AutoScreenRecorder/internal/config"

	"golang.org/x/sys/windows"
)

var (
	modKernel32                   = windows.NewLazySystemDLL("kernel32.dll")
	procQueryFullProcessImageName = modKernel32.NewProc("QueryFullProcessImageNameW")
)

func ExecutableBaseName(pid uint32) (string, error) {
	path, err := ExecutablePath(pid)
	if err != nil {
		return "", err
	}
	return filepath.Base(path), nil
}

func ExecutablePath(pid uint32) (string, error) {
	h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		return "", err
	}
	defer windows.CloseHandle(h)

	buf := make([]uint16, 32768)
	size := uint32(len(buf))
	r1, _, e1 := procQueryFullProcessImageName.Call(
		uintptr(h),
		uintptr(0),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(unsafe.Pointer(&size)),
	)
	if r1 == 0 && e1 == windows.ERROR_INSUFFICIENT_BUFFER && size > uint32(len(buf)) {
		buf = make([]uint16, size)
		size = uint32(len(buf))
		r1, _, e1 = procQueryFullProcessImageName.Call(
			uintptr(h),
			uintptr(0),
			uintptr(unsafe.Pointer(&buf[0])),
			uintptr(unsafe.Pointer(&size)),
		)
	}
	if r1 == 0 {
		if e1 != nil && e1 != syscall.Errno(0) {
			return "", e1
		}
		return "", syscall.EINVAL
	}
	return windows.UTF16ToString(buf[:size]), nil
}

func IsIgnored(exe string, ignore []string) bool {
	for _, ig := range ignore {
		if strings.EqualFold(exe, ig) {
			return true
		}
	}
	return false
}

func MatchesWatchRule(exeBase, fullPath string, rule config.ProcessRule) bool {
	if !strings.EqualFold(exeBase, rule.Name) {
		return false
	}
	return pathHasPrefixBoundary(fullPath, rule.PathPrefix)
}

// pathHasPrefixBoundary matches prefix as a path, so C:\App\Lark does not match C:\App\LarkExtra.
func pathHasPrefixBoundary(fullPath, prefix string) bool {
	if prefix == "" {
		return true
	}
	full := strings.ToLower(filepath.Clean(fullPath))
	pre := strings.ToLower(filepath.Clean(prefix))
	if !strings.HasPrefix(full, pre) {
		return false
	}
	if len(full) == len(pre) {
		return true
	}
	last := pre[len(pre)-1]
	if last == '\\' || last == '/' {
		return true
	}
	next := full[len(pre)]
	return next == '\\' || next == '/'
}

func MatchesAnyWatch(pid uint32, watch []config.ProcessRule, ignore []string) (bool, string, error) {
	exe, err := ExecutableBaseName(pid)
	if err != nil {
		return false, "", err
	}
	if IsIgnored(exe, ignore) {
		return false, exe, nil
	}
	path, err := ExecutablePath(pid)
	if err != nil {
		path = exe
	}
	for _, rule := range watch {
		if MatchesWatchRule(exe, path, rule) {
			return true, exe, nil
		}
	}
	return false, exe, nil
}
