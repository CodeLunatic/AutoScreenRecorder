//go:build windows

package singleinstance

import (
	"errors"
	"fmt"

	"golang.org/x/sys/windows"
)

var mutexName = `Local\AutoScreenRecorder_SingleInstance`

func Acquire() (release func(), err error) {
	name, err := windows.UTF16PtrFromString(mutexName)
	if err != nil {
		return nil, err
	}
	h, err := windows.CreateMutex(nil, false, name)
	if h == 0 {
		if err == nil {
			err = fmt.Errorf("create mutex failed")
		}
		return nil, err
	}
	if errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		windows.CloseHandle(h)
		return nil, fmt.Errorf("another instance is already running")
	}
	if err != nil {
		windows.CloseHandle(h)
		return nil, err
	}
	return func() { windows.CloseHandle(h) }, nil
}
