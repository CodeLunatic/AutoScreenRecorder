#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <vector>

static HRESULT get_file_duration_100ns(IMFSourceReader *reader, LONGLONG *duration) {
	if (!reader || !duration) return E_POINTER;
	*duration = 0;
	PROPVARIANT var{};
	PropVariantInit(&var);
	HRESULT hr = reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
	if (SUCCEEDED(hr) && var.vt == VT_UI8) {
		*duration = var.uhVal.QuadPart;
	}
	PropVariantClear(&var);
	return hr;
}

static HRESULT copy_selected_stream(IMFSourceReader *reader, IMFSinkWriter *writer, DWORD reader_stream, DWORD out_stream, LONGLONG time_offset, bool *has_data) {
	if (has_data) *has_data = false;
	if (!reader || !writer) return E_POINTER;
	for (;;) {
		DWORD stream_index = 0;
		DWORD flags = 0;
		LONGLONG ts = 0;
		IMFSample *sample = nullptr;
		HRESULT hr = reader->ReadSample(reader_stream, 0, &stream_index, &flags, &ts, &sample);
		if (FAILED(hr)) return hr;
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
		if (!sample) continue;
		sample->SetSampleTime(ts + time_offset);
		hr = writer->WriteSample(out_stream, sample);
		sample->Release();
		if (FAILED(hr)) return hr;
		if (has_data) *has_data = true;
	}
	return S_OK;
}

static void safe_release(IUnknown *p) {
	if (p) p->Release();
}

int merge_mp4_files(const wchar_t *out_path, const wchar_t **inputs, int count) {
	if (!out_path || !inputs || count <= 0) return -1;
	if (count == 1) {
		if (!CopyFileW(inputs[0], out_path, FALSE)) return -2;
		return 0;
	}

	HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr)) return -3;
	bool mf_started = true;

	auto shutdown = [&]() {
		if (mf_started) {
			MFShutdown();
			mf_started = false;
		}
	};

	IMFSourceReader *first_reader = nullptr;
	hr = MFCreateSourceReaderFromURL(inputs[0], nullptr, &first_reader);
	if (FAILED(hr) || !first_reader) {
		shutdown();
		return -4;
	}

	IMFMediaType *v_native = nullptr;
	hr = first_reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &v_native);
	if (FAILED(hr) || !v_native) {
		safe_release(v_native);
		first_reader->Release();
		shutdown();
		return -4;
	}
	IMFMediaType *a_native = nullptr;
	const bool has_audio = SUCCEEDED(first_reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &a_native)) && a_native;

	IMFSinkWriter *writer = nullptr;
	hr = MFCreateSinkWriterFromURL(out_path, nullptr, nullptr, &writer);
	if (FAILED(hr) || !writer) {
		safe_release(v_native);
		safe_release(a_native);
		first_reader->Release();
		shutdown();
		return -5;
	}

	DWORD v_stream = 0, a_stream = 0;
	hr = writer->AddStream(v_native, &v_stream);
	v_native->Release();
	v_native = nullptr;
	if (FAILED(hr)) {
		safe_release(a_native);
		writer->Release();
		first_reader->Release();
		shutdown();
		return -5;
	}
	if (has_audio) {
		hr = writer->AddStream(a_native, &a_stream);
		if (FAILED(hr)) {
			safe_release(a_native);
			writer->Release();
			first_reader->Release();
			shutdown();
			return -5;
		}
	}
	safe_release(a_native);
	a_native = nullptr;

	IMFMediaType *v_in = nullptr;
	hr = first_reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &v_in);
	if (FAILED(hr) || !v_in) {
		safe_release(v_in);
		writer->Release();
		first_reader->Release();
		shutdown();
		return -5;
	}
	hr = writer->SetInputMediaType(v_stream, v_in, nullptr);
	v_in->Release();
	if (FAILED(hr)) {
		writer->Release();
		first_reader->Release();
		shutdown();
		return -5;
	}
	if (has_audio) {
		IMFMediaType *a_in = nullptr;
		hr = first_reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &a_in);
		if (FAILED(hr) || !a_in) {
			safe_release(a_in);
			writer->Release();
			first_reader->Release();
			shutdown();
			return -5;
		}
		hr = writer->SetInputMediaType(a_stream, a_in, nullptr);
		a_in->Release();
		if (FAILED(hr)) {
			writer->Release();
			first_reader->Release();
			shutdown();
			return -5;
		}
	}

	hr = writer->BeginWriting();
	if (FAILED(hr)) {
		writer->Release();
		first_reader->Release();
		shutdown();
		return -5;
	}

	LONGLONG offset = 0;
	for (int i = 0; i < count; i++) {
		IMFSourceReader *reader = nullptr;
		if (i == 0) {
			reader = first_reader;
			reader->AddRef();
		} else {
			hr = MFCreateSourceReaderFromURL(inputs[i], nullptr, &reader);
			if (FAILED(hr) || !reader) {
				hr = E_FAIL;
				break;
			}
		}

		bool v_ok = false;
		HRESULT chr = copy_selected_stream(reader, writer, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, v_stream, offset, &v_ok);
		if (FAILED(chr)) {
			hr = chr;
			reader->Release();
			break;
		}
		if (has_audio) {
			bool a_ok = false;
			chr = copy_selected_stream(reader, writer, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, a_stream, offset, &a_ok);
			if (FAILED(chr)) {
				hr = chr;
				reader->Release();
				break;
			}
		}

		LONGLONG dur = 0;
		get_file_duration_100ns(reader, &dur);
		if (dur > 0) offset += dur;
		reader->Release();
		hr = S_OK;
	}

	HRESULT fin = writer->Finalize();
	if (FAILED(fin) && SUCCEEDED(hr)) hr = fin;
	writer->Release();
	first_reader->Release();
	shutdown();
	return SUCCEEDED(hr) ? 0 : -6;
}
