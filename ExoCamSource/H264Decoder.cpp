#include "pch.h"
#include "Tools.h"
#include "H264Decoder.h"
#include "Log.h"
#include <codecapi.h>     // CODECAPI_AVLowLatencyMode

// Declared by hand rather than via <wmcodecdsp.h> / <icodecapi.h>: those pull in
// strmif.h -> ddraw.h, which redefines every DirectDraw GUID because this project
// defines INITGUID. Do not "clean up" into the real headers without checking that.
extern "C" const GUID CLSID_CMSH264DecoderMFT =
{ 0x62CE7E72, 0x4C71, 0x4d20, { 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D } };

MIDL_INTERFACE("901db4c7-31ce-41a2-85dc-8fa0bf41b8da")
ICodecAPI : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID*) = 0;
	virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID*, VARIANT**, ULONG*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID*, VARIANT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID*, VARIANT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID*, VARIANT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID*, LONG_PTR) = 0;
	virtual HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetAllDefaults() = 0;
	virtual HRESULT STDMETHODCALLTYPE SetValueWithNotify(const GUID*, VARIANT*, GUID**, ULONG*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetAllDefaultsWithNotify(GUID**, ULONG*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetAllSettings(IStream*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetAllSettings(IStream*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetAllSettingsWithNotify(IStream*, GUID**, ULONG*) = 0;
};

#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfplat.lib")

HRESULT H264Decoder::Initialize(UINT width, UINT height)
{
	Shutdown();

	RETURN_IF_FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_mft)));

	// MF_LOW_LATENCY and CODECAPI_AVLowLatencyMode are both required. With
	// either one missing the decoder buffers several frames for reorder, which
	// on its own blows the <100ms latency target.
	wil::com_ptr_nothrow<IMFAttributes> atts;
	if (SUCCEEDED(_mft->GetAttributes(&atts)) && atts)
	{
		atts->SetUINT32(MF_LOW_LATENCY, TRUE);
	}

	wil::com_ptr_nothrow<ICodecAPI> codec;
	if (SUCCEEDED(_mft->QueryInterface(IID_PPV_ARGS(&codec))) && codec)
	{
		VARIANT v{};
		v.vt = VT_BOOL;
		v.boolVal = VARIANT_TRUE;
		codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
	}

	wil::com_ptr_nothrow<IMFMediaType> inputType;
	RETURN_IF_FAILED(MFCreateMediaType(&inputType));
	RETURN_IF_FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	RETURN_IF_FAILED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
	RETURN_IF_FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	RETURN_IF_FAILED(MFSetAttributeSize(inputType.get(), MF_MT_FRAME_SIZE, width, height));
	RETURN_IF_FAILED(MFSetAttributeRatio(inputType.get(), MF_MT_FRAME_RATE, 30, 1));
	RETURN_IF_FAILED(_mft->SetInputType(0, inputType.get(), 0));

	RETURN_IF_FAILED(SelectOutputType());

	RETURN_IF_FAILED(_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
	RETURN_IF_FAILED(_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

	Log::Line(L"H264Decoder: initialized, output %ux%u size %u", _outWidth, _outHeight, _outSize);
	return S_OK;
}

void H264Decoder::Shutdown()
{
	if (_mft)
	{
		_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
		_mft.reset();
	}
	_outWidth = 0;
	_outHeight = 0;
	_outStride = 0;
	_outSize = 0;

	_outSample.reset();
	_outSampleCapacity = 0;
}

HRESULT H264Decoder::SelectOutputType()
{
	for (DWORD i = 0; ; i++)
	{
		wil::com_ptr_nothrow<IMFMediaType> type;
		const HRESULT hr = _mft->GetOutputAvailableType(0, i, &type);
		if (hr == MF_E_NO_MORE_TYPES)
			break;

		RETURN_IF_FAILED(hr);

		GUID subtype{};
		if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12)
			continue;

		RETURN_IF_FAILED(_mft->SetOutputType(0, type.get(), 0));
		MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &_outWidth, &_outHeight);

		UINT32 stride = 0;
		_outStride = (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride) ? stride : _outWidth;

		MFT_OUTPUT_STREAM_INFO info{};
		RETURN_IF_FAILED(_mft->GetOutputStreamInfo(0, &info));
		_outSize = info.cbSize;
		return S_OK;
	}

	RETURN_HR_MSG(MF_E_INVALIDMEDIATYPE, "H264Decoder: no NV12 output type offered");
}

HRESULT H264Decoder::Decode(const uint8_t* data, size_t length, uint64_t ptsUs, IMFSample** outSample)
{
	RETURN_HR_IF_NULL(E_POINTER, outSample);
	*outSample = nullptr;
	RETURN_HR_IF(E_NOT_VALID_STATE, !_mft);

	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(MFCreateMemoryBuffer((DWORD)length, &buffer));

	BYTE* dest = nullptr;
	RETURN_IF_FAILED(buffer->Lock(&dest, nullptr, nullptr));
	memcpy(dest, data, length);
	RETURN_IF_FAILED(buffer->Unlock());
	RETURN_IF_FAILED(buffer->SetCurrentLength((DWORD)length));

	wil::com_ptr_nothrow<IMFSample> input;
	RETURN_IF_FAILED(MFCreateSample(&input));
	RETURN_IF_FAILED(input->AddBuffer(buffer.get()));
	RETURN_IF_FAILED(input->SetSampleTime((LONGLONG)(ptsUs * 10)));
	RETURN_IF_FAILED(input->SetSampleDuration(333333));

	RETURN_IF_FAILED(_mft->ProcessInput(0, input.get(), 0));

	// Drain everything available and keep only the newest: this is a live feed,
	// so a backlogged frame is already stale by the time we would hand it over.
	wil::com_ptr_nothrow<IMFSample> latest;

	for (;;)
	{
		wil::com_ptr_nothrow<IMFSample> sample;
		const HRESULT hr = PullOutputOnce(&sample);

		if (hr == S_FALSE)
			break;

		RETURN_IF_FAILED(hr);
		latest = sample;
	}

	if (!latest)
		return S_FALSE;

	*outSample = latest.detach();
	return S_OK;
}

HRESULT H264Decoder::EnsureOutputSample()
{
	if (_outSample && _outSampleCapacity == _outSize)
		return S_OK;

	// Either the first call, or a stream change resized _outSize out from under
	// a buffer we were still holding onto -- either way the old one (if any) is
	// the wrong capacity and gets replaced, not reused.
	_outSample.reset();

	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(MFCreateSample(&_outSample));
	RETURN_IF_FAILED(MFCreateMemoryBuffer(_outSize, &buffer));
	RETURN_IF_FAILED(_outSample->AddBuffer(buffer.get()));

	_outSampleCapacity = _outSize;
	return S_OK;
}

HRESULT H264Decoder::PullOutputOnce(IMFSample** outSample)
{
	RETURN_HR_IF_NULL(E_POINTER, outSample);
	*outSample = nullptr;

	for (;;)
	{
		RETURN_IF_FAILED(EnsureOutputSample());

		MFT_OUTPUT_DATA_BUFFER out{};
		out.pSample = _outSample.get();

		DWORD status = 0;
		const HRESULT hr = _mft->ProcessOutput(0, 1, &out, &status);

		if (out.pEvents)
		{
			out.pEvents->Release();
			out.pEvents = nullptr;
		}

		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			// No output this round, and by far the most common outcome. A
			// ProcessOutput that fails this way leaves _outSample untouched, so
			// it carries over to the next call rather than being reallocated.
			return S_FALSE;
		}

		if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			RETURN_IF_FAILED(SelectOutputType());
			Log::Line(L"H264Decoder: format change -> %ux%u size %u", _outWidth, _outHeight, _outSize);
			// EnsureOutputSample() at the top of the next iteration notices
			// _outSampleCapacity no longer matches and replaces the buffer.
			continue;
		}

		RETURN_IF_FAILED(hr);

		// A real frame: the caller now owns these pixels, so this instance can no
		// longer write into that buffer. Detach and let EnsureOutputSample()
		// allocate a fresh one next time, rather than risk the decoder filling a
		// buffer the caller is still reading.
		*outSample = _outSample.detach();
		_outSampleCapacity = 0;
		return S_OK;
	}
}