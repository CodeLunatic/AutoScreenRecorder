//go:build windows

package singleinstance

import "testing"

func TestSecondAcquireFails(t *testing.T) {
	prev := mutexName
	mutexName = `Local\AutoScreenRecorder_SingleInstance_Test`
	t.Cleanup(func() { mutexName = prev })

	release, err := Acquire()
	if err != nil {
		t.Fatal(err)
	}
	defer release()

	_, err = Acquire()
	if err == nil {
		t.Fatal("second acquire succeeded")
	}
}
