//go:build windows

package notify

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	ole32                = windows.NewLazySystemDLL("ole32.dll")
	procCoCreateInstance = ole32.NewProc("CoCreateInstance")
	procCoInitializeEx   = ole32.NewProc("CoInitializeEx")
	procCoUninitialize   = ole32.NewProc("CoUninitialize")

	oleaut32           = windows.NewLazySystemDLL("oleaut32.dll")
	procSysAllocString = oleaut32.NewProc("SysAllocString")
	procSysFreeString  = oleaut32.NewProc("SysFreeString")

	shell32      = windows.NewLazySystemDLL("shell32.dll")
	procSetAppID = shell32.NewProc("SetCurrentProcessExplicitAppUserModelID")

	procSHParseDisplayName         = shell32.NewProc("SHParseDisplayName")
	procSHOpenFolderAndSelectItems = shell32.NewProc("SHOpenFolderAndSelectItems")
	procILFree                     = shell32.NewProc("ILFree")
	procILClone                    = shell32.NewProc("ILClone")
	procILFindLastID               = shell32.NewProc("ILFindLastID")
	procILRemoveLastID             = shell32.NewProc("ILRemoveLastID")
)

var (
	clsidShellLink = windows.GUID{Data1: 0x00021401, Data2: 0x0000, Data3: 0x0000, Data4: [8]byte{0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	iidShellLinkW  = windows.GUID{Data1: 0x000214F9, Data2: 0x0000, Data3: 0x0000, Data4: [8]byte{0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	iidPersistFile = windows.GUID{Data1: 0x0000010B, Data2: 0x0000, Data3: 0x0000, Data4: [8]byte{0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	iidPropStore   = windows.GUID{Data1: 0x886D8EEB, Data2: 0x8CF2, Data3: 0x4446, Data4: [8]byte{0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}}
)

type unknownVtbl struct {
	QueryInterface uintptr
	AddRef         uintptr
	Release        uintptr
}

type shellLinkVtbl struct {
	QueryInterface      uintptr
	AddRef              uintptr
	Release             uintptr
	GetPath             uintptr
	GetIDList           uintptr
	SetIDList           uintptr
	GetDescription      uintptr
	SetDescription      uintptr
	GetWorkingDirectory uintptr
	SetWorkingDirectory uintptr
	GetArguments        uintptr
	SetArguments        uintptr
	GetHotkey           uintptr
	SetHotkey           uintptr
	GetShowCmd          uintptr
	SetShowCmd          uintptr
	GetIconLocation     uintptr
	SetIconLocation     uintptr
	SetRelativePath     uintptr
	Resolve             uintptr
	SetPath             uintptr
}

type propertyStoreVtbl struct {
	QueryInterface uintptr
	AddRef         uintptr
	Release        uintptr
	GetCount       uintptr
	GetAt          uintptr
	GetValue       uintptr
	SetValue       uintptr
	Commit         uintptr
}

type persistFileVtbl struct {
	QueryInterface uintptr
	AddRef         uintptr
	Release        uintptr
	GetClassID     uintptr
	IsDirty        uintptr
	Load           uintptr
	Save           uintptr
	SaveCompleted  uintptr
	GetCurFile     uintptr
}

type propertyKey struct {
	fmtid windows.GUID
	pid   uint32
}

type propVariant struct {
	vt        uint16
	reserved1 uint16
	reserved2 uint16
	reserved3 uint16
	val       uintptr
	pad       uintptr
}

func hrErr(hr uintptr) error {
	if int32(hr) >= 0 {
		return nil
	}
	return fmt.Errorf("HRESULT 0x%08X", uint32(hr))
}

func comRelease(p unsafe.Pointer) {
	if p == nil {
		return
	}
	vtbl := *(**unknownVtbl)(p)
	_, _, _ = syscall.SyscallN(vtbl.Release, uintptr(p))
}

func comQI(p unsafe.Pointer, iid *windows.GUID) (unsafe.Pointer, error) {
	vtbl := *(**unknownVtbl)(p)
	var out unsafe.Pointer
	hr, _, _ := syscall.SyscallN(vtbl.QueryInterface, uintptr(p), uintptr(unsafe.Pointer(iid)), uintptr(unsafe.Pointer(&out)))
	if err := hrErr(hr); err != nil {
		return nil, err
	}
	return out, nil
}

func ensureStartMenuShortcut(exePath, icoPath string) error {
	appData := os.Getenv("APPDATA")
	if appData == "" {
		return fmt.Errorf("APPDATA is empty")
	}
	lnk := filepath.Join(appData, "Microsoft", "Windows", "Start Menu", "Programs", "AutoScreenRecorder.lnk")
	return createShortcut(lnk, exePath, icoPath, appID)
}

func createShortcut(lnkPath, exePath, icoPath, aumid string) error {
	hr, _, _ := procCoInitializeEx.Call(0, 2) // COINIT_APARTMENTTHREADED
	changedMode := uint32(hr) == 0x80010106
	if err := hrErr(hr); err != nil && !changedMode {
		return err
	}
	if !changedMode {
		defer procCoUninitialize.Call()
	}

	var link unsafe.Pointer
	hr, _, _ = procCoCreateInstance.Call(
		uintptr(unsafe.Pointer(&clsidShellLink)),
		0,
		1, // CLSCTX_INPROC_SERVER
		uintptr(unsafe.Pointer(&iidShellLinkW)),
		uintptr(unsafe.Pointer(&link)),
	)
	if err := hrErr(hr); err != nil {
		return fmt.Errorf("create shortcut: %w", err)
	}
	defer comRelease(link)

	exePtr, err := windows.UTF16PtrFromString(exePath)
	if err != nil {
		return err
	}
	icoPtr, err := windows.UTF16PtrFromString(icoPath)
	if err != nil {
		return err
	}
	dirPtr, err := windows.UTF16PtrFromString(filepath.Dir(exePath))
	if err != nil {
		return err
	}
	lnkPtr, err := windows.UTF16PtrFromString(lnkPath)
	if err != nil {
		return err
	}

	linkVtbl := *(**shellLinkVtbl)(link)
	if hr, _, _ = syscall.SyscallN(linkVtbl.SetPath, uintptr(link), uintptr(unsafe.Pointer(exePtr))); hrErr(hr) != nil {
		return fmt.Errorf("shortcut path: %w", hrErr(hr))
	}
	runtime.KeepAlive(exePtr)
	if hr, _, _ = syscall.SyscallN(linkVtbl.SetWorkingDirectory, uintptr(link), uintptr(unsafe.Pointer(dirPtr))); hrErr(hr) != nil {
		return fmt.Errorf("shortcut directory: %w", hrErr(hr))
	}
	runtime.KeepAlive(dirPtr)
	if hr, _, _ = syscall.SyscallN(linkVtbl.SetIconLocation, uintptr(link), uintptr(unsafe.Pointer(icoPtr)), 0); hrErr(hr) != nil {
		return fmt.Errorf("shortcut icon: %w", hrErr(hr))
	}
	runtime.KeepAlive(icoPtr)

	store, err := comQI(link, &iidPropStore)
	if err != nil {
		return fmt.Errorf("shortcut properties: %w", err)
	}
	defer comRelease(store)

	src, err := windows.UTF16PtrFromString(aumid)
	if err != nil {
		return err
	}
	bstr, _, _ := procSysAllocString.Call(uintptr(unsafe.Pointer(src)))
	runtime.KeepAlive(src)
	if bstr == 0 {
		return fmt.Errorf("shortcut app id alloc failed")
	}
	defer procSysFreeString.Call(bstr)

	key := propertyKey{
		fmtid: windows.GUID{
			Data1: 0x9F4C2855,
			Data2: 0x9F79,
			Data3: 0x4B39,
			Data4: [8]byte{0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3},
		},
		pid: 5, // PKEY_AppUserModel_ID
	}
	pv := propVariant{vt: 31, val: bstr} // VT_LPWSTR
	storeVtbl := *(**propertyStoreVtbl)(store)
	if hr, _, _ = syscall.SyscallN(storeVtbl.SetValue, uintptr(store), uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&pv))); hrErr(hr) != nil {
		return fmt.Errorf("shortcut app id: %w", hrErr(hr))
	}
	if hr, _, _ = syscall.SyscallN(storeVtbl.Commit, uintptr(store)); hrErr(hr) != nil {
		return fmt.Errorf("shortcut commit: %w", hrErr(hr))
	}

	persist, err := comQI(link, &iidPersistFile)
	if err != nil {
		return fmt.Errorf("shortcut save interface: %w", err)
	}
	defer comRelease(persist)
	persistVtbl := *(**persistFileVtbl)(persist)
	if hr, _, _ = syscall.SyscallN(persistVtbl.Save, uintptr(persist), uintptr(unsafe.Pointer(lnkPtr)), 1); hrErr(hr) != nil {
		return fmt.Errorf("shortcut save: %w", hrErr(hr))
	}
	runtime.KeepAlive(lnkPtr)
	return nil
}

func setExplicitAppUserModelID(id string) {
	ptr, err := windows.UTF16PtrFromString(id)
	if err != nil {
		return
	}
	_, _, _ = procSetAppID.Call(uintptr(unsafe.Pointer(ptr)))
	runtime.KeepAlive(ptr)
}
