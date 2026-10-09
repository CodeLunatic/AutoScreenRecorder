//go:build windows

package notify

import (
	"fmt"
	"log"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"github.com/go-toast/toast"
	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

const appID = "AutoScreenRecorder"
const protocolName = "autorecorder"

var (
	setupMu sync.Mutex
	setupOK bool
	pushMu  sync.Mutex
)

func setup(exePath string) {
	setupMu.Lock()
	defer setupMu.Unlock()
	if setupOK {
		return
	}
	if err := registerProtocolHandler(exePath); err != nil {
		log.Printf("通知协议: %v", err)
		return
	}
	if err := installAssets(exePath); err != nil {
		log.Printf("通知图标: %v", err)
		return
	}
	setupOK = true
}

func cdataText(s string) string {
	s = strings.ReplaceAll(s, "\x00", "")
	for strings.Contains(s, "]]>") {
		s = strings.ReplaceAll(s, "]]>", "]] >")
	}
	return s
}

func xmlAttr(s string) string {
	replacer := strings.NewReplacer(
		"&", "&amp;",
		"<", "&lt;",
		">", "&gt;",
		`"`, "&quot;",
	)
	return replacer.Replace(s)
}

func registerProtocolHandler(exePath string) error {
	exePath = filepath.Clean(exePath)
	base := `Software\Classes\` + protocolName
	k, _, err := registry.CreateKey(registry.CURRENT_USER, base, registry.SET_VALUE)
	if err != nil {
		return err
	}
	_ = k.SetStringValue("", "URL:AutoScreenRecorder")
	_ = k.SetStringValue("URL Protocol", "")
	k.Close()

	cmdKey := base + `\shell\open\command`
	k, _, err = registry.CreateKey(registry.CURRENT_USER, cmdKey, registry.SET_VALUE)
	if err != nil {
		return err
	}
	cmd := directOpenVideoCommand(exePath)
	err = k.SetStringValue("", cmd)
	k.Close()
	_ = os.Remove(filepath.Join(filepath.Dir(exePath), "open-video.vbs"))
	return err
}

func directOpenVideoCommand(exePath string) string {
	return fmt.Sprintf(`"%s" -open-video "%%1"`, exePath)
}

// IsOpenVideoLaunch reports whether this process was started from a toast click.
func IsOpenVideoLaunch(args []string) bool {
	for _, a := range args {
		if a == "-open-video" || strings.HasPrefix(a, "-open-video=") {
			return true
		}
	}
	return false
}

// HideConsoleWindow hides a console that Windows already created for this process.
func HideConsoleWindow() {
	kernel32 := windows.NewLazySystemDLL("kernel32.dll")
	hwnd, _, _ := kernel32.NewProc("GetConsoleWindow").Call()
	if hwnd == 0 {
		return
	}
	user32 := windows.NewLazySystemDLL("user32.dll")
	_, _, _ = user32.NewProc("ShowWindow").Call(hwnd, 0) // SW_HIDE
}

// AttachParentConsole connects a window-subsystem build to the terminal that started it.
func AttachParentConsole() {
	kernel32 := windows.NewLazySystemDLL("kernel32.dll")
	r, _, _ := kernel32.NewProc("AttachConsole").Call(uintptr(0xFFFFFFFF)) // ATTACH_PARENT_PROCESS
	if r == 0 {
		return
	}
	if stdout, err := windows.GetStdHandle(windows.STD_OUTPUT_HANDLE); err == nil && stdout != 0 {
		os.Stdout = os.NewFile(uintptr(stdout), "stdout")
	}
	if stderr, err := windows.GetStdHandle(windows.STD_ERROR_HANDLE); err == nil && stderr != 0 {
		os.Stderr = os.NewFile(uintptr(stderr), "stderr")
	}
}

func protocolLaunchArg(videoPath string) string {
	videoPath = filepath.Clean(videoPath)
	return fmt.Sprintf("%s://open/%s", protocolName, url.PathEscape(filepath.ToSlash(videoPath)))
}

// ParseOpenVideoArg normalizes -open-video from toast / protocol handler.
func ParseOpenVideoArg(arg string) string {
	arg = strings.TrimSpace(arg)
	if arg == "" {
		return ""
	}
	for _, prefix := range []string{protocolName + "://open/", protocolName + ":"} {
		if strings.HasPrefix(arg, prefix) {
			arg = strings.TrimPrefix(arg, prefix)
			break
		}
	}
	if decoded, err := url.PathUnescape(arg); err == nil && decoded != "" {
		arg = decoded
	}
	return filepath.FromSlash(arg)
}

// Init registers the URL protocol used when the user clicks the toast.
func Init(exePath string) {
	setup(exePath)
}

// RevealInExplorer opens File Explorer with the file selected.
// An already open window for that folder is reused and brought to the foreground.
func RevealInExplorer(videoPath string) error {
	path, selectFile, err := prepareRevealPath(videoPath)
	if err != nil {
		return err
	}
	shellErr := revealShell(path, selectFile)
	if focusFolder(path, selectFile, 700*time.Millisecond) {
		return nil
	}
	execErr := shellExecuteSelect(path, selectFile)
	if focusFolder(path, selectFile, 1500*time.Millisecond) {
		return nil
	}
	if shellErr != nil {
		return shellErr
	}
	return execErr
}

func prepareRevealPath(videoPath string) (string, bool, error) {
	path := filepath.Clean(videoPath)
	if _, err := os.Stat(path); err != nil {
		dir := filepath.Dir(path)
		if dir == "" || dir == "." {
			return "", false, err
		}
		return dir, false, nil
	}
	return path, true, nil
}

func folderTitle(path string, selectFile bool) string {
	if selectFile {
		return filepath.Base(filepath.Dir(path))
	}
	return filepath.Base(path)
}

func shellExecuteSelect(path string, selectFile bool) error {
	verb, err := windows.UTF16PtrFromString("open")
	if err != nil {
		return err
	}
	file, err := windows.UTF16PtrFromString("explorer.exe")
	if err != nil {
		return err
	}
	params := `"` + path + `"`
	if selectFile {
		params = `/select,"` + path + `"`
	}
	arg, err := windows.UTF16PtrFromString(params)
	if err != nil {
		return err
	}
	err = windows.ShellExecute(0, verb, file, arg, nil, windows.SW_SHOWNORMAL)
	runtime.KeepAlive(verb)
	runtime.KeepAlive(file)
	runtime.KeepAlive(arg)
	return err
}

func focusFolder(path string, selectFile bool, wait time.Duration) bool {
	name := folderTitle(path, selectFile)
	deadline := time.Now().Add(wait)
	for {
		if hwnd := findExplorerWindow(name); hwnd != 0 {
			forceForeground(hwnd)
			return true
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func findExplorerWindow(folder string) uintptr {
	user32 := windows.NewLazySystemDLL("user32.dll")
	enumWindows := user32.NewProc("EnumWindows")
	var match uintptr
	cb := syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		if !isExplorerWindow(user32, hwnd) {
			return 1
		}
		if titleMatchesFolder(windowText(user32, hwnd), folder) {
			match = hwnd
			return 0
		}
		return 1
	})
	_, _, _ = enumWindows.Call(cb, 0)
	runtime.KeepAlive(cb)
	return match
}

func isExplorerWindow(user32 *windows.LazyDLL, hwnd uintptr) bool {
	class := windowClass(user32, hwnd)
	return class == "CabinetWClass" || class == "ExploreWClass"
}

func titleMatchesFolder(title, folder string) bool {
	title = strings.Trim(strings.TrimSpace(title), "\u200e\u200f\u202a\u202b\u202c\u202d\u202e")
	folder = strings.TrimSpace(folder)
	if title == "" || folder == "" || folder == `\` || folder == "/" {
		return false
	}
	title = strings.ToLower(title)
	folder = strings.ToLower(folder)
	if title == folder || strings.HasPrefix(title, folder+" ") || strings.HasPrefix(title, folder+"-") {
		return true
	}
	return strings.HasSuffix(title, `\`+folder) || strings.HasSuffix(title, `/`+folder)
}

func windowClass(user32 *windows.LazyDLL, hwnd uintptr) string {
	buf := make([]uint16, 64)
	_, _, _ = user32.NewProc("GetClassNameW").Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return windows.UTF16ToString(buf)
}

func windowText(user32 *windows.LazyDLL, hwnd uintptr) string {
	buf := make([]uint16, 512)
	_, _, _ = user32.NewProc("GetWindowTextW").Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return windows.UTF16ToString(buf)
}

func forceForeground(hwnd uintptr) {
	user32 := windows.NewLazySystemDLL("user32.dll")
	kernel32 := windows.NewLazySystemDLL("kernel32.dll")
	show := user32.NewProc("ShowWindow")
	if iconic, _, _ := user32.NewProc("IsIconic").Call(hwnd); iconic != 0 {
		_, _, _ = show.Call(hwnd, 9) // SW_RESTORE
	} else {
		_, _, _ = show.Call(hwnd, 5) // SW_SHOW
	}

	var pid uint32
	user32.NewProc("GetWindowThreadProcessId").Call(hwnd, uintptr(unsafe.Pointer(&pid)))
	if pid != 0 {
		_, _, _ = user32.NewProc("AllowSetForegroundWindow").Call(uintptr(pid))
	}

	fore, _, _ := user32.NewProc("GetForegroundWindow").Call()
	foreThread, _, _ := user32.NewProc("GetWindowThreadProcessId").Call(fore, 0)
	ourThread, _, _ := kernel32.NewProc("GetCurrentThreadId").Call()
	attach := user32.NewProc("AttachThreadInput")
	attached := false
	if foreThread != 0 && foreThread != ourThread {
		if r, _, _ := attach.Call(ourThread, foreThread, 1); r != 0 {
			attached = true
		}
	}
	// A momentary Alt lets SetForegroundWindow through the foreground lock.
	keybd := user32.NewProc("keybd_event")
	_, _, _ = keybd.Call(0x12, 0, 0, 0) // VK_MENU
	_, _, _ = user32.NewProc("SetWindowPos").Call(hwnd, 0, 0, 0, 0, 0, 0x0001|0x0002|0x0040)
	_, _, _ = user32.NewProc("BringWindowToTop").Call(hwnd)
	_, _, _ = user32.NewProc("SetForegroundWindow").Call(hwnd)
	_, _, _ = user32.NewProc("SwitchToThisWindow").Call(hwnd, 1)
	_, _, _ = keybd.Call(0x12, 0, 2, 0) // KEYEVENTF_KEYUP
	if attached {
		_, _, _ = attach.Call(ourThread, foreThread, 0)
	}
}

func revealShell(path string, selectFile bool) error {
	owned := coInitSTA()
	if owned {
		defer windows.CoUninitialize()
	}
	pidl, err := parseDisplayName(path)
	if err != nil {
		return err
	}
	defer ilFree(pidl)
	if !selectFile {
		return openFolder(pidl, 0, nil)
	}
	folder, _, _ := procILClone.Call(pidl)
	if folder == 0 {
		return fmt.Errorf("clone folder pidl")
	}
	defer ilFree(folder)
	if r, _, _ := procILRemoveLastID.Call(folder); r == 0 {
		return fmt.Errorf("folder pidl")
	}
	child, _, _ := procILFindLastID.Call(pidl)
	if child == 0 {
		return fmt.Errorf("child pidl")
	}
	return openFolder(folder, 1, &child)
}

func coInitSTA() bool {
	err := windows.CoInitializeEx(0, windows.COINIT_APARTMENTTHREADED)
	if err == nil || err == syscall.Errno(1) { // S_OK or S_FALSE
		return true
	}
	return false
}

func parseDisplayName(path string) (uintptr, error) {
	ptr, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}
	var pidl uintptr
	hr, _, _ := procSHParseDisplayName.Call(uintptr(unsafe.Pointer(ptr)), 0, uintptr(unsafe.Pointer(&pidl)), 0, 0)
	runtime.KeepAlive(ptr)
	if int32(hr) < 0 || pidl == 0 {
		return 0, fmt.Errorf("parse path: 0x%08X", uint32(hr))
	}
	return pidl, nil
}

func openFolder(folder uintptr, count uintptr, items *uintptr) error {
	var list uintptr
	if items != nil {
		list = uintptr(unsafe.Pointer(items))
	}
	hr, _, _ := procSHOpenFolderAndSelectItems.Call(folder, count, list, 0)
	runtime.KeepAlive(items)
	if int32(hr) < 0 {
		return fmt.Errorf("open folder: 0x%08X", uint32(hr))
	}
	return nil
}

func ilFree(pidl uintptr) {
	if pidl != 0 {
		_, _, _ = procILFree.Call(pidl)
	}
}

// RecordingSaved shows a Windows toast; click opens the file location via -open-video.
func RecordingSaved(exePath, videoPath string) error {
	setup(exePath)
	launch := xmlAttr(protocolLaunchArg(videoPath))
	n := toast.Notification{
		AppID:               appID,
		Title:               cdataText("录屏已保存"),
		Message:             cdataText(filepath.Base(videoPath)),
		Icon:                xmlAttr(toastIcon),
		ActivationType:      "protocol",
		ActivationArguments: launch,
		Audio:               toast.Default,
		Actions: []toast.Action{
			{
				Type:      "protocol",
				Label:     xmlAttr("打开所在位置"),
				Arguments: launch,
			},
		},
	}
	pushMu.Lock()
	defer pushMu.Unlock()
	return n.Push()
}
