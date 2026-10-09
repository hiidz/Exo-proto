#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "FrameGenerator.h"
#include "NoSignal.h"
#include "MediaStream.h"
#include "MediaSource.h"
#include "Log.h"
#include "VCamPort.h"
#include "VCamVideoSettings.h"
#include <algorithm>
#include <cstdarg>
#include <vector>

// One-shot guard: fires the first time, then stays silent for the life of the
// stream so a hot failure path (RequestSample runs per frame) can't flood the
// log. The flag is a member, not a local static -- see MediaStream.h.
#define SRCLOG_ONCE(flag, fmt, ...)      \
	do {                                 \
		if (!(flag))                     \
		{                                \
			(flag) = true;               \
			Log::Line(fmt, __VA_ARGS__); \
		}                                \
	} while (0)

namespace
{
	constexpr UINT AlignDown2(UINT v) { return v & ~1u; }

	HRESULT CreateNV12Type(UINT32 width, UINT32 height, UINT32 fps, IMFMediaType** type)
	{
		wil::com_ptr_nothrow<IMFMediaType> nv12Type;
		RETURN_IF_FAILED(MFCreateMediaType(&nv12Type));
		RETURN_IF_FAILED(nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		RETURN_IF_FAILED(nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
		RETURN_IF_FAILED(nv12Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		RETURN_IF_FAILED(nv12Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
		RETURN_IF_FAILED(MFSetAttributeSize(nv12Type.get(), MF_MT_FRAME_SIZE, width, height));
		RETURN_IF_FAILED(nv12Type->SetUINT32(MF_MT_DEFAULT_STRIDE, width));
		RETURN_IF_FAILED(MFSetAttributeRatio(nv12Type.get(), MF_MT_FRAME_RATE, fps, 1));

		// Describes THIS type -- uncompressed NV12 at 1.5 bytes per pixel -- not
		// the phone's H264 link. [video] bitrate is a different number entirely
		// and must never end up here. Computed in 64 bits and capped: the clamps
		// in VCamVideoSettings.h permit sizes that overflow 32.
		const uint64_t avg = (uint64_t)width * height * 3 / 2 * 8 * fps;
		RETURN_IF_FAILED(nv12Type->SetUINT32(MF_MT_AVG_BITRATE,
			(UINT32)min(avg, (uint64_t)UINT32_MAX)));

		RETURN_IF_FAILED(MFSetAttributeRatio(nv12Type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

		*type = nv12Type.detach();
		return S_OK;
	}
}

HRESULT MediaStream::Initialize(IMFMediaSource* source, int index)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;

	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 0));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

	// Exactly one type, built from [video] in vcam.ini -- the same file and the
	// same clamps that give the tray its camera_size. Offering a ladder of sizes
	// is actively harmful here: the only downscaler in this DLL is ScaleNV12,
	// which is nearest neighbour, so a client picking a smaller entry gets a
	// worse image than if it had taken the full frame and scaled it itself, and
	// every conferencing app does its own filtered downscale anyway. One type
	// also means there is no negotiated resolution to feed back to the phone.
	//
	// Read here rather than cached anywhere: Initialize runs each time the Frame
	// Server creates the source, so an edited ini is picked up the next time an
	// app opens the camera. An app already streaming keeps the old format.
	const VCamVideoSettings video = ReadVCamVideoSettings(VCamIniPathForThisModule());

	_width = video.width;
	_height = video.height;
	_frameDuration = 10000000LL / video.fps;

	// The ini path is logged because a DLL deployed apart from vcam.ini fails
	// exactly one way -- defaults on this side, the file on the tray's side --
	// and that is otherwise invisible until the scaling path shows up in frames.
	Log::Line(L"MediaStream::Initialize %ux%u @%ufps from %s",
		_width, _height, video.fps, VCamIniPathForThisModule().c_str());

	wil::com_ptr_nothrow<IMFMediaType> type;
	RETURN_IF_FAILED(CreateNV12Type(_width, _height, video.fps, &type));

	IMFMediaType* types[] = { type.get() };
	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, ARRAYSIZE(types), types, &_descriptor),
		"MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	RETURN_IF_FAILED(handler->SetCurrentMediaType(type.get()));

	return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	// SetStreamState(RUNNING) calls us with nullptr, so resolve the current type
	// first. Otherwise the subtype goes unread and _width/_height keep whatever
	// the previous session negotiated.
	wil::com_ptr_nothrow<IMFMediaType> current;
	if (!type && _descriptor)
	{
		wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
		if (SUCCEEDED(_descriptor->GetMediaTypeHandler(&handler)) &&
			SUCCEEDED(handler->GetCurrentMediaType(&current)))
		{
			type = current.get();
		}
	}

	if (type)
	{
		GUID subtype = GUID_NULL;
		RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype));

		UINT32 w = 0, h = 0;
		if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) && w && h)
		{
			_width = w;
			_height = h;
		}

		Log::Line(L"MediaStream::Start format: %s %ux%u", GUID_ToStringW(subtype).c_str(), _width, _height);
	}

#if defined(VCAM_DEBUG_PATTERN)
	RETURN_IF_FAILED(_generator.EnsureRenderTarget(_width, _height));
#endif

	if (!_allocatorReady)
	{
		RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, type));
		_allocatorReady = true;
	}

	_client.Start(kVCamStreamPort);
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_RUNNING;
	return S_OK;
}

HRESULT MediaStream::Stop()
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	// Deliberately NOT calling _client.Stop(). Closing the socket makes the scrcpy
	// server exit, which strands every other client and needs manual intervention
	// on the phone to recover.

	if (_allocatorReady)
	{
		RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
		_allocatorReady = false;
	}
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_STOPPED;
	return S_OK;
}

MFSampleAllocatorUsage MediaStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);

	winrt::slim_lock_guard lock(_lock);

	_allocator.reset();
	_allocatorReady = false;

	RETURN_HR(allocator->QueryInterface(&_allocator));
}

// Frames arrive already decoded as NV12 in system memory, so there is nothing
// for a D3D manager to do. Accepted and ignored.
HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);
	return S_OK;
}

void MediaStream::Shutdown()
{
	winrt::slim_lock_guard lock(_lock);
	_client.Stop();
	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}

	_descriptor.reset();
	_source.reset();
	_attributes.reset();
}

// IMFMediaEventGenerator
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaStream
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);

	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);

	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

// Nearest-neighbour letterbox fit, for when the phone is not producing the size
// the client negotiated. Not a normal path: with the tray and the DLL reading
// the same [video] section the sizes match and CopyDecodedNV12 memcpys instead.
//
// Bounds are the CALLER's responsibility. CopyDecodedNV12 validates both the
// source plane pair and that the destination holds pitch * dstH * 3/2 bytes
// with dstW <= pitch, so nothing here re-checks.
static void ScaleNV12(const BYTE* srcY, UINT srcStride, UINT srcPaddedHeight, UINT srcW, UINT srcH,
	BYTE* dstY, LONG pitch, UINT dstW, UINT dstH)
{
	UINT outW, outH;
	if ((uint64_t)srcW * dstH >= (uint64_t)srcH * dstW)
	{
		outW = dstW;
		outH = (UINT)((uint64_t)srcH * dstW / srcW);
	}
	else
	{
		outH = dstH;
		outW = (UINT)((uint64_t)srcW * dstH / srcH);
	}

	outW = AlignDown2(outW);
	outH = AlignDown2(outH);
	if (!outW || !outH)
		return;

	const UINT offX = AlignDown2((dstW - outW) / 2);
	const UINT offY = AlignDown2((dstH - outH) / 2);

	BYTE* dstUV = dstY + (size_t)pitch * dstH;

	// Letterbox bars.
	for (UINT y = 0; y < dstH; y++)
		memset(dstY + (size_t)y * pitch, 16, dstW);
	for (UINT y = 0; y < dstH / 2; y++)
		memset(dstUV + (size_t)y * pitch, 128, dstW);

	thread_local std::vector<UINT> xmap;
	xmap.resize(outW);
	for (UINT x = 0; x < outW; x++)
		xmap[x] = (UINT)((uint64_t)x * srcW / outW);

	for (UINT y = 0; y < outH; y++)
	{
		const UINT sy = (UINT)((uint64_t)y * srcH / outH);
		const BYTE* s = srcY + (size_t)sy * srcStride;
		BYTE* d = dstY + (size_t)(y + offY) * pitch + offX;
		for (UINT x = 0; x < outW; x++)
			d[x] = s[xmap[x]];
	}

	const BYTE* srcUV = srcY + (size_t)srcStride * srcPaddedHeight;
	const UINT srcCW = srcW / 2;
	const UINT srcCH = srcH / 2;
	const UINT outCW = outW / 2;
	const UINT outCH = outH / 2;

	thread_local std::vector<UINT> uvmap;
	uvmap.resize(outCW);
	for (UINT x = 0; x < outCW; x++)
		uvmap[x] = (UINT)((uint64_t)x * srcCW / outCW) * 2;

	for (UINT y = 0; y < outCH; y++)
	{
		const UINT sy = (UINT)((uint64_t)y * srcCH / outCH);
		const BYTE* s = srcUV + (size_t)sy * srcStride;
		BYTE* d = dstUV + (size_t)(y + offY / 2) * pitch + offX;
		for (UINT x = 0; x < outCW; x++)
		{
			d[x * 2] = s[uvmap[x]];
			d[x * 2 + 1] = s[uvmap[x] + 1];
		}
	}
}

static HRESULT CopyDecodedNV12(IMFSample* src, UINT srcStride, UINT srcPaddedHeight,
	UINT srcWidth, UINT srcHeight,
	IMFSample* dst, UINT dstWidth, UINT dstHeight)
{
	RETURN_HR_IF(E_INVALIDARG, !srcStride || !srcPaddedHeight || !srcWidth || !srcHeight);
	RETURN_HR_IF(E_INVALIDARG, srcWidth > srcStride || srcHeight > srcPaddedHeight);

	wil::com_ptr_nothrow<IMFMediaBuffer> srcBuf;
	RETURN_IF_FAILED(src->GetBufferByIndex(0, &srcBuf));

	wil::com_ptr_nothrow<IMFMediaBuffer> dstBuf;
	RETURN_IF_FAILED(dst->GetBufferByIndex(0, &dstBuf));

	wil::com_ptr_nothrow<IMF2DBuffer2> dst2D;
	RETURN_IF_FAILED(dstBuf->QueryInterface(IID_PPV_ARGS(&dst2D)));

	BYTE* srcPtr = nullptr;
	DWORD srcLen = 0;
	RETURN_IF_FAILED(srcBuf->Lock(&srcPtr, nullptr, &srcLen));

	// Both bounds checks matter, and for different reasons: reading past the
	// source corrupts our own frame, writing past the destination corrupts the
	// Frame Server's heap.
	if (srcLen < (size_t)srcStride * srcPaddedHeight * 3 / 2)
	{
		srcBuf->Unlock();
		RETURN_HR(E_UNEXPECTED);
	}

	BYTE* scanline = nullptr;
	LONG pitch = 0;
	BYTE* start = nullptr;
	DWORD len = 0;
	HRESULT hr = dst2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &len);

	if (SUCCEEDED(hr))
	{
		const size_t dstStride = (size_t)(pitch < 0 ? -pitch : pitch);

		if (dstWidth > dstStride || len < dstStride * dstHeight * 3 / 2)
		{
			hr = E_UNEXPECTED;
		}
		else if (srcWidth == dstWidth && srcHeight == dstHeight)
		{
			// Y: copy `height` rows out of a buffer that is srcPaddedHeight tall.
			// Skipping this crop is what makes the image shear diagonally.
			for (UINT y = 0; y < dstHeight; y++)
				memcpy(scanline + (size_t)y * pitch, srcPtr + (size_t)y * srcStride, dstWidth);

			// UV: half height, interleaved, same stride. Source plane starts after
			// the PADDED luma plane, destination after the cropped one.
			const BYTE* srcUV = srcPtr + (size_t)srcStride * srcPaddedHeight;
			BYTE* dstUV = scanline + (size_t)pitch * dstHeight;

			for (UINT y = 0; y < dstHeight / 2; y++)
				memcpy(dstUV + (size_t)y * pitch, srcUV + (size_t)y * srcStride, dstWidth);
		}
		else
		{
			ScaleNV12(srcPtr, srcStride, srcPaddedHeight, srcWidth, srcHeight,
				scanline, pitch, dstWidth, dstHeight);
		}

		dst2D->Unlock2D();
	}

	srcBuf->Unlock();
	return hr;
}

static HRESULT FillNoSignal(IMFSample* dst, UINT width, UINT height, NoSignalReason reason)
{
	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(dst->GetBufferByIndex(0, &buffer));

	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	RETURN_IF_FAILED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));

	BYTE* scanline = nullptr;
	LONG pitch = 0;
	BYTE* start = nullptr;
	DWORD len = 0;
	RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &len));

	RenderNoSignal(scanline, pitch, width, height, reason);

	buffer2D->Unlock2D();
	return S_OK;
}

// Last resort when FillNoSignal fails. FillNoSignal needs IMF2DBuffer2, which is
// a single point of failure for the whole no-signal path; this uses plain
// IMFMediaBuffer::Lock, which returns the contiguous representation (stride ==
// width) and so works on any buffer. Video-range black.
static HRESULT FillFlatGrey(IMFSample* dst, UINT width, UINT height)
{
	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(dst->GetBufferByIndex(0, &buffer));

	BYTE* data = nullptr;
	DWORD maxLength = 0;
	RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));

	const size_t ySize = (size_t)width * height;
	const size_t total = ySize * 3 / 2;

	HRESULT hr = S_OK;
	if (!data || maxLength < total)
	{
		hr = E_UNEXPECTED;
	}
	else
	{
		memset(data, 16, ySize);
		memset(data + ySize, 128, total - ySize);
	}

	buffer->Unlock();
	return hr;
}

bool MediaStream::FillFromStream(IMFSample* sample)
{
	wil::com_ptr_nothrow<IMFSample> decoded;
	UINT srcStride = 0, srcPaddedHeight = 0, srcWidth = 0, srcHeight = 0;

	if (_client.GetLatestFrame(&decoded, &srcStride, &srcPaddedHeight, &srcWidth, &srcHeight) != S_OK || !decoded)
		return false;

	const HRESULT hr = CopyDecodedNV12(decoded.get(), srcStride, srcPaddedHeight,
		srcWidth, srcHeight, sample, _width, _height);

	if (FAILED(hr))
	{
		SRCLOG_ONCE(_loggedCopy, L"[%p] CopyDecodedNV12 failed hr=0x%08X src=%ux%u stride=%u padded=%u dst=%ux%u",
			this, hr, srcWidth, srcHeight, srcStride, srcPaddedHeight, _width, _height);
		return false;
	}

	return true;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);

	// A request racing Stop() would otherwise reach AllocateSample on an
	// allocator Stop() has already uninitialised. Failing here does not
	// contradict the fill ladder below: the guarantee is "while running, always
	// queue a sample", not "never fail".
	RETURN_HR_IF(MF_E_INVALIDREQUEST, !_allocatorReady || _state != MF_STREAM_STATE_RUNNING);

	wil::com_ptr_nothrow<IMFSample> sample;
	const HRESULT allocHr = _allocator->AllocateSample(&sample);
	if (FAILED(allocHr) || !sample)
	{
		SRCLOG_ONCE(_loggedAlloc, L"[%p] AllocateSample failed hr=0x%08X", this, allocHr);
		RETURN_HR(FAILED(allocHr) ? allocHr : E_UNEXPECTED);
	}

	LOG_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
	LOG_IF_FAILED(sample->SetSampleDuration(_frameDuration));

	// Fill ladder: live video, then placeholder, then flat grey. Nothing below
	// returns early, and exactly one sample is queued per call. A failed
	// RequestSample stalls the Frame Server and shows the device as broken rather
	// than blank, so if every fill fails the raw allocator sample still goes out:
	// garbage pixels, but a live pipeline.
	wil::com_ptr_nothrow<IMFSample> outSample = sample;

	if (!FillFromStream(sample.get()))
	{
#if defined(VCAM_DEBUG_PATTERN)
		_generator.SetStatus(_client.IsConnected(), _client.BytesReceived(), _client.DecodedFrames());

		wil::com_ptr_nothrow<IMFSample> generated;
		const HRESULT genHr = _generator.Generate(sample.get(), &generated);
		if (SUCCEEDED(genHr) && generated)
		{
			outSample = generated;
			LOG_IF_FAILED(outSample->SetSampleTime(MFGetSystemTime()));
			LOG_IF_FAILED(outSample->SetSampleDuration(_frameDuration));
		}
		else
		{
			SRCLOG_ONCE(_loggedFill, L"[%p] Generate failed hr=0x%08X", this, genHr);
			LOG_IF_FAILED(FillFlatGrey(sample.get(), _width, _height));
		}
#else
		const NoSignalReason reason =
			_client.HasDecodedAnyFrame() ? NoSignalReason::Lost :
			_client.IsConnected() ? NoSignalReason::Connecting :
			NoSignalReason::Waiting;

		const HRESULT fillHr = FillNoSignal(sample.get(), _width, _height, reason);
		if (FAILED(fillHr))
		{
			SRCLOG_ONCE(_loggedFill, L"[%p] FillNoSignal failed hr=0x%08X dst=%ux%u", this, fillHr, _width, _height);
			LOG_IF_FAILED(FillFlatGrey(sample.get(), _width, _height));
		}
#endif
	}

	if (pToken)
	{
		LOG_IF_FAILED(outSample->SetUnknown(MFSampleExtension_Token, pToken));
	}

	RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, outSample.get()));
	return S_OK;
}

// IMFMediaStream2
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
	Log::Line(L"MediaStream::SetStreamState current:%u value:%u", _state, value);
	if (_state == value)
		return S_OK;

	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
		RETURN_HR_IF(MF_E_INVALID_STATE_TRANSITION, _state != MF_STREAM_STATE_RUNNING);
		_state = value;
		break;

	case MF_STREAM_STATE_RUNNING:
		RETURN_IF_FAILED(Start(nullptr));
		break;

	case MF_STREAM_STATE_STOPPED:
		RETURN_IF_FAILED(Stop());
		break;

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
	}

	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
	RETURN_HR_IF_NULL(E_POINTER, value);
	*value = _state;
	return S_OK;
}

// IKsControl. Nothing is exposed; see the note in MediaSource::KsProperty.
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	LOG_DETAIL(L"MediaStream::KsProperty prop:%s", PKSIDENTIFIER_ToString(property, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	LOG_DETAIL(L"MediaStream::KsMethod method:%s", PKSIDENTIFIER_ToString(method, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	LOG_DETAIL(L"MediaStream::KsEvent event:%s", PKSIDENTIFIER_ToString(evt, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}