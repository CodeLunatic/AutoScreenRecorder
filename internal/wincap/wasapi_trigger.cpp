#include "crashlog.h"

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>
#include <thread>
#include <vector>
#include <map>
#include <mutex>

extern "C" void wasapi_on_session_active(uint32_t pid);
extern "C" void wasapi_on_session_inactive(uint32_t pid);

static std::atomic<bool> g_wasapi_running{false};
static std::atomic<int> g_bind_gen{0};
static std::atomic<int> g_bind_serial{0};
static std::atomic<bool> g_endpoint_ready{false};
static IMMDeviceEnumerator *g_enum = nullptr;
static IMMDevice *g_capture_dev = nullptr;
static IAudioSessionManager2 *g_session_mgr = nullptr;
static IAudioSessionNotification *g_session_notify = nullptr;

static std::mutex g_sessions_mu;
struct SessionSlot {
	IAudioSessionControl *control = nullptr;
	IAudioSessionEvents *events = nullptr;
	DWORD pid = 0;
	bool active = false;
	bool noted = false;
};
static std::vector<SessionSlot> g_slots;

static std::mutex g_pid_mu;
static std::map<DWORD, int> g_pid_refs;

static void note_pid_active(DWORD pid) {
	if (pid == 0) return;
	bool edge = false;
	{
		std::lock_guard<std::mutex> lock(g_pid_mu);
		int &n = g_pid_refs[pid];
		n++;
		edge = n == 1;
	}
	if (edge && g_wasapi_running.load()) wasapi_on_session_active(pid);
}

static void note_pid_inactive(DWORD pid) {
	if (pid == 0) return;
	bool edge = false;
	{
		std::lock_guard<std::mutex> lock(g_pid_mu);
		auto it = g_pid_refs.find(pid);
		if (it == g_pid_refs.end()) return;
		it->second--;
		if (it->second <= 0) {
			g_pid_refs.erase(it);
			edge = true;
		}
	}
	if (edge && g_wasapi_running.load()) wasapi_on_session_inactive(pid);
}

static DWORD pid_of(IAudioSessionControl *control) {
	if (!control) return 0;
	IAudioSessionControl2 *c2 = nullptr;
	if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&c2)) || !c2) return 0;
	DWORD pid = 0;
	c2->GetProcessId(&pid);
	c2->Release();
	return pid;
}

static void session_set_state(IAudioSessionControl *control, AudioSessionState state) {
	DWORD pid = 0;
	bool became_active = false;
	bool became_inactive = false;
	SessionSlot removed{};
	bool do_remove = false;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);
		for (auto it = g_slots.begin(); it != g_slots.end(); ++it) {
			if (it->control != control) continue;
			pid = it->pid;
			const bool now_active = state == AudioSessionStateActive;
			const bool now_dead = state == AudioSessionStateInactive || state == AudioSessionStateExpired;
			if (now_active && !it->active) {
				it->active = true;
				became_active = true;
			} else if (now_dead && it->active) {
				became_inactive = it->noted;
				it->active = false;
				it->noted = false;
			}
			if (state == AudioSessionStateExpired) {
				removed = *it;
				g_slots.erase(it);
				do_remove = true;
			}
			break;
		}
	}
	if (became_active) {
		note_pid_active(pid);
		bool still = false;
		{
			std::lock_guard<std::mutex> lock(g_sessions_mu);
			for (auto &slot : g_slots) {
				if (slot.control != control || !slot.active) continue;
				slot.noted = true;
				still = true;
				break;
			}
		}
		if (!still) note_pid_inactive(pid);
	}
	if (became_inactive) note_pid_inactive(pid);
	if (do_remove) {
		removed.noted = false;
		removed.active = false;
		if (removed.control && removed.events) removed.control->UnregisterAudioSessionNotification(removed.events);
		if (removed.events) removed.events->Release();
		if (removed.control) removed.control->Release();
	}
}

class SessionEvents final : public IAudioSessionEvents {
	long ref_{1};
	IAudioSessionControl *control_{nullptr};
public:
	explicit SessionEvents(IAudioSessionControl *c) : control_(c) {
		if (control_) control_->AddRef();
	}
	~SessionEvents() {
		if (control_) control_->Release();
	}
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
		if (!ppv) return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
			*ppv = static_cast<IAudioSessionEvents *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
	STDMETHODIMP_(ULONG) Release() override {
		auto n = InterlockedDecrement(&ref_);
		if (n == 0) delete this;
		return n;
	}
	STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
	STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
	STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
	STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
	STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
	STDMETHODIMP OnStateChanged(AudioSessionState state) override {
		session_set_state(control_, state);
		return S_OK;
	}
	STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
		session_set_state(control_, AudioSessionStateExpired);
		return S_OK;
	}
};

class SessionNotify final : public IAudioSessionNotification {
	long ref_{1};
public:
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
		if (!ppv) return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
			*ppv = static_cast<IAudioSessionNotification *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
	STDMETHODIMP_(ULONG) Release() override {
		auto n = InterlockedDecrement(&ref_);
		if (n == 0) delete this;
		return n;
	}
	STDMETHODIMP OnSessionCreated(IAudioSessionControl *NewSession) override;
};

static SessionNotify *g_notify_obj = nullptr;
static std::atomic<bool> g_rebind_capture{false};
static HANDLE g_rebind_event = nullptr;
static std::thread g_rebind_thread;
static IMMNotificationClient *g_endpoint_notify = nullptr;

static void request_rebind(void) {
	if (!g_wasapi_running.load()) return;
	g_rebind_capture.store(true);
	if (g_rebind_event) SetEvent(g_rebind_event);
}

class TriggerEndpointNotify final : public IMMNotificationClient {
	volatile long ref_{1};

public:
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
			*ppv = static_cast<IMMNotificationClient *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
	STDMETHODIMP_(ULONG) Release() override {
		const ULONG n = static_cast<ULONG>(InterlockedDecrement(&ref_));
		if (n == 0) delete this;
		return n;
	}
	STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD state) override {
		if (state == DEVICE_STATE_ACTIVE) request_rebind();
		return S_OK;
	}
	STDMETHODIMP OnDeviceAdded(LPCWSTR) override {
		request_rebind();
		return S_OK;
	}
	STDMETHODIMP OnDeviceRemoved(LPCWSTR) override {
		request_rebind();
		return S_OK;
	}
	STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
		if (role == eConsole && flow == eCapture) request_rebind();
		return S_OK;
	}
	STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

static void clear_sessions(void) {
	std::vector<SessionSlot> slots;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);
		slots.swap(g_slots);
	}
	for (auto &s : slots) {
		if (s.noted && s.active) note_pid_inactive(s.pid);
		if (s.control && s.events) s.control->UnregisterAudioSessionNotification(s.events);
		if (s.events) s.events->Release();
		if (s.control) s.control->Release();
	}
}

static void register_session_events(IAudioSessionControl *control) {
	if (!control || !g_wasapi_running.load()) return;
	const int gen = g_bind_gen.load();
	if (gen < 0) return;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);
		for (const auto &slot : g_slots) {
			if (slot.control == control) return;
		}
	}
	auto *ev = new SessionEvents(control);
	if (FAILED(control->RegisterAudioSessionNotification(ev))) {
		ev->Release();
		return;
	}
	if (g_bind_gen.load() != gen) {
		control->UnregisterAudioSessionNotification(ev);
		ev->Release();
		return;
	}
	const DWORD pid = pid_of(control);
	AudioSessionState st = AudioSessionStateInactive;
	control->GetState(&st);
	const bool active = st == AudioSessionStateActive;
	control->AddRef();
	bool inserted = false;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);
		bool duplicate = false;
		for (const auto &slot : g_slots) {
			if (slot.control == control) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate && g_bind_gen.load() == gen) {
			g_slots.push_back(SessionSlot{control, ev, pid, active, false});
			inserted = true;
		}
	}
	if (!inserted) {
		control->UnregisterAudioSessionNotification(ev);
		ev->Release();
		control->Release();
		return;
	}
	if (!active) return;
	note_pid_active(pid);
	bool still = false;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);
		for (auto &slot : g_slots) {
			if (slot.events != ev) continue;
			if (slot.active) {
				slot.noted = true;
				still = true;
			}
			break;
		}
	}
	if (!still) note_pid_inactive(pid);
}

STDMETHODIMP SessionNotify::OnSessionCreated(IAudioSessionControl *NewSession) {
	register_session_events(NewSession);
	return S_OK;
}

static void enumerate_existing_sessions(void) {
	if (!g_session_mgr) return;
	IAudioSessionEnumerator *enumerator = nullptr;
	if (FAILED(g_session_mgr->GetSessionEnumerator(&enumerator)) || !enumerator) return;
	int count = 0;
	enumerator->GetCount(&count);
	for (int i = 0; i < count; i++) {
		IAudioSessionControl *control = nullptr;
		if (FAILED(enumerator->GetSession(i, &control)) || !control) continue;
		register_session_events(control);
		control->Release();
	}
	enumerator->Release();
}

static void ensure_endpoint_callback(void) {
	if (!g_enum || g_endpoint_notify) return;
	g_endpoint_notify = new TriggerEndpointNotify();
	if (FAILED(g_enum->RegisterEndpointNotificationCallback(g_endpoint_notify))) {
		g_endpoint_notify->Release();
		g_endpoint_notify = nullptr;
	}
}

static int bind_capture_endpoint(void) {
	g_bind_gen.store(-1);
	clear_sessions();
	if (g_session_mgr) {
		if (g_session_notify) {
			g_session_mgr->UnregisterSessionNotification(g_session_notify);
			g_session_notify->Release();
			g_session_notify = nullptr;
		}
		g_session_mgr->Release();
		g_session_mgr = nullptr;
	}
	g_notify_obj = nullptr;
	if (g_capture_dev) {
		g_capture_dev->Release();
		g_capture_dev = nullptr;
	}
	if (!g_enum) {
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&g_enum))) {
			return -1;
		}
	}
	ensure_endpoint_callback();
	if (FAILED(g_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &g_capture_dev))) {
		return -2;
	}
	if (FAILED(g_capture_dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&g_session_mgr))) {
		return -3;
	}
	const int gen = g_bind_serial.fetch_add(1) + 1;
	g_bind_gen.store(gen);
	auto *notify = new SessionNotify();
	if (FAILED(g_session_mgr->RegisterSessionNotification(notify))) {
		notify->Release();
		g_bind_gen.store(-1);
		return -4;
	}
	g_notify_obj = notify;
	g_session_notify = notify;
	enumerate_existing_sessions();
	return 0;
}

int wasapi_trigger_start(void) {
	if (g_wasapi_running.exchange(true)) return 0;
	g_rebind_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!g_rebind_event) {
		g_wasapi_running.store(false);
		return -1;
	}
	const int rc = bind_capture_endpoint();
	if (rc != 0) crash_log_trace("wasapi: capture endpoint not ready, will retry");
	else g_endpoint_ready.store(true);
	g_rebind_thread = std::thread([] {
		const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const bool com_owned = SUCCEEDED(cohr);
		while (g_wasapi_running.load()) {
			if (g_rebind_event) WaitForSingleObject(g_rebind_event, 2000);
			if (!g_wasapi_running.load()) break;
			if (!g_rebind_capture.exchange(false) && g_session_mgr != nullptr && g_bind_gen.load() >= 0) continue;
			const int bind_rc = bind_capture_endpoint();
			if (bind_rc == 0) {
				if (!g_endpoint_ready.exchange(true)) crash_log_trace("wasapi: capture endpoint ready");
			} else if (g_endpoint_ready.exchange(false)) {
				crash_log_trace("wasapi: capture endpoint lost, will retry");
			}
		}
		if (com_owned) CoUninitialize();
	});
	return 0;
}

void wasapi_trigger_stop(void) {
	if (!g_wasapi_running.exchange(false)) return;
	if (g_rebind_event) SetEvent(g_rebind_event);
	if (g_rebind_thread.joinable()) g_rebind_thread.join();
	g_bind_gen.store(-1);
	g_endpoint_ready.store(false);
	if (g_enum && g_endpoint_notify) g_enum->UnregisterEndpointNotificationCallback(g_endpoint_notify);
	if (g_endpoint_notify) {
		g_endpoint_notify->Release();
		g_endpoint_notify = nullptr;
	}
	if (g_rebind_event) {
		CloseHandle(g_rebind_event);
		g_rebind_event = nullptr;
	}
	std::vector<DWORD> dying;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);
		for (const auto &s : g_slots) {
			if (!s.noted || !s.active || !s.pid) continue;
			bool seen = false;
			for (DWORD have : dying) {
				if (have == s.pid) {
					seen = true;
					break;
				}
			}
			if (!seen) dying.push_back(s.pid);
		}
	}
	clear_sessions();
	{
		std::lock_guard<std::mutex> lock(g_pid_mu);
		g_pid_refs.clear();
	}
	for (DWORD pid : dying) wasapi_on_session_inactive(pid);
	if (g_session_mgr && g_session_notify) {
		g_session_mgr->UnregisterSessionNotification(g_session_notify);
		g_session_notify->Release();
		g_session_notify = nullptr;
		g_notify_obj = nullptr;
	}
	if (g_session_mgr) {
		g_session_mgr->Release();
		g_session_mgr = nullptr;
	}
	if (g_capture_dev) {
		g_capture_dev->Release();
		g_capture_dev = nullptr;
	}
	if (g_enum) {
		g_enum->Release();
		g_enum = nullptr;
	}
}
