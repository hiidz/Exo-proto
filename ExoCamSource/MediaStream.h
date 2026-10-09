#pragma once
#include "StreamClient.h"

// The single video stream: owns the StreamClient that pulls decoded frames off
// the socket, and fills each sample the Frame Server asks for.
//
// Offers exactly one media type, built at Initialize from [video] in vcam.ini.
// See the note there for why there is one rather than a ladder of sizes.

struct MediaStream : winrt::implements<MediaStream, CBaseAttributes<IMFAttributes>, IMFMediaStream2, IKsControl>
{
public:
	// IMFMediaEventGenerator
	STDMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState);
	STDMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
	STDMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent);
	STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue);

	// IMFMediaStream
	STDMETHOD(GetMediaSource)(IMFMediaSource** ppMediaSource);
	STDMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** ppStreamDescriptor);
	STDMETHOD(RequestSample)(IUnknown* pToken);

	// IMFMediaStream2
	STDMETHOD(SetStreamState)(MF_STREAM_STATE value);
	STDMETHOD(GetStreamState)(MF_STREAM_STATE* value);

	// IKsControl
	STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned);

public:
	MediaStream()
	{
		SetBaseAttributesTraceName(L"MediaStreamAtts");
	}

	HRESULT Initialize(IMFMediaSource* source, int index);
	HRESULT SetAllocator(IUnknown* allocator);
	MFSampleAllocatorUsage GetAllocatorUsage();
	HRESULT SetD3DManager(IUnknown* manager);
	HRESULT Start(IMFMediaType* type);
	HRESULT Stop();
	void Shutdown();

private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept override
	{
		RETURN_HR_MSG(E_NOINTERFACE, "MediaStream QueryInterface failed on IID %s", GUID_ToStringW(id).c_str());
	}
#endif

	// True if `sample` now holds a decoded frame from the phone.
	bool FillFromStream(IMFSample* sample);

	winrt::slim_mutex _lock;
	MF_STREAM_STATE _state = MF_STREAM_STATE_STOPPED;
	bool _allocatorReady = false;
	StreamClient _client;

#if defined(VCAM_DEBUG_PATTERN)
	FrameGenerator _generator;
#endif

	// The size the CLIENT negotiated, not necessarily the size the phone sends.
	// Set in Start() from the current media type; everything writing into an
	// allocator sample must use these, never a compile-time constant.
	UINT32 _width = 1920;
	UINT32 _height = 1080;
	// Derived from [video] fps at Initialize. Must stay consistent with the
	// MF_MT_FRAME_RATE on the offered type, or the sample durations contradict
	// the format the client negotiated.
	LONGLONG _frameDuration = 10000000LL / 30;

	// One-shot log guards. Members, not function-local statics: a static inside a
	// member function is shared by every stream in the process, so whichever
	// failed first would suppress the rest.
	bool _loggedAlloc = false;
	bool _loggedCopy = false;
	bool _loggedFill = false;

	wil::com_ptr_nothrow<IMFStreamDescriptor> _descriptor;
	wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
	wil::com_ptr_nothrow<IMFMediaSource> _source;
	wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> _allocator;
	int _index = 0;
};