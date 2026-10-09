#pragma once

// The COM media source Media Foundation activates for the virtual camera. Owns
// the event queue, the presentation descriptor and the streams; all the actual
// video work is in MediaStream.
//
// Instantiated separately in every process that opens the camera -- the Frame
// Server as LOCAL SERVICE, and each consuming app under its own account -- so
// nothing here may assume a per-machine singleton or reach HKCU.

struct MediaStream;

struct MediaSource : winrt::implements<MediaSource, CBaseAttributes<IMFAttributes>, IMFMediaSourceEx, IMFGetService, IKsControl, IMFSampleAllocatorControl>
{
public:
	// IMFMediaEventGenerator
	STDMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState);
	STDMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
	STDMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent);
	STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue);

	// IMFMediaSource
	STDMETHOD(CreatePresentationDescriptor)(IMFPresentationDescriptor** ppPresentationDescriptor);
	STDMETHOD(GetCharacteristics)(DWORD* pdwCharacteristics);
	STDMETHOD(Pause)();
	STDMETHOD(Shutdown)();
	STDMETHOD(Start)(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition);
	STDMETHOD(Stop)();

	// IMFMediaSourceEx
	STDMETHOD(GetSourceAttributes)(IMFAttributes** ppAttributes);
	STDMETHOD(GetStreamAttributes)(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes);
	STDMETHOD(SetD3DManager)(IUnknown* pManager);

	// IMFMediaSource2. Not currently used: change IMFMediaSourceEx to
	// IMFMediaSource2 in the implements<> list above to enable it.
	STDMETHOD(SetMediaType)(DWORD dwStreamID, IMFMediaType* pMediaType);

	// IMFGetService
	STDMETHOD(GetService)(REFGUID guidService, REFIID riid, LPVOID* ppvObject);

	// IMFSampleAllocatorControl
	STDMETHOD(SetDefaultAllocator)(DWORD dwOutputStreamID, IUnknown* pAllocator);
	STDMETHOD(GetAllocatorUsage)(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage);

	// IKsControl
	STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned);

public:
	MediaSource() :
		_streams(_numStreams)
	{
		SetBaseAttributesTraceName(L"MediaSourceAtts");
		for (auto i = 0; i < _numStreams; i++)
		{
			auto stream = winrt::make_self<MediaStream>();
			stream->Initialize(this, i);
			// attach(), not assignment: assigning a detached raw pointer to a
			// wil::com_ptr_nothrow AddRefs it again and leaks the stream.
			_streams[i].attach(stream.detach());
		}
	}

	HRESULT Initialize(IMFAttributes* attributes);

private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept override
	{
		// Undocumented interfaces the pipeline probes for. Answered quietly so
		// they skip the logging RETURN_HR_MSG at the bottom, which would
		// otherwise report a failure on every activation.
		if (id == winrt::guid_of<IMFDeviceSourceInternal>() ||
			id == winrt::guid_of<IMFDeviceSourceInternal2>() ||
			id == winrt::guid_of<IMFDeviceTransformManager>() ||
			id == winrt::guid_of<IMFCollection>() ||
			id == winrt::guid_of<IMFDeviceController2>() ||
			id == winrt::guid_of<IMFDeviceSourceStatus>())
			return E_NOINTERFACE;

		if (id == winrt::guid_of<IMFRealTimeClientEx>() ||
			id == winrt::guid_of<IMFMediaSource2>())
			return E_NOINTERFACE;

		RETURN_HR_MSG(E_NOINTERFACE, "MediaSource QueryInterface failed on IID %s", GUID_ToStringW(id).c_str());
	}
#endif

	int GetStreamIndexById(DWORD id);

private:
	// One device, one stream, one client -- MF_DEVICESTREAM_FRAMESERVER_SHARED
	// is set to 0 to match. Multi-device is a non-goal, not a missing feature.
	const int _numStreams = 1;
	winrt::slim_mutex _lock;
	winrt::com_array<wil::com_ptr_nothrow<MediaStream>> _streams;
	wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
	wil::com_ptr_nothrow<IMFPresentationDescriptor> _descriptor;
};