#pragma once

// Wraps the inbox Microsoft H.264 decoder MFT (CLSID_CMSH264DecoderMFT).
// Input is Annex-B (scrcpy sends 00 00 00 01 start codes), output is NV12.
class H264Decoder
{
public:
	~H264Decoder() { Shutdown(); }

	HRESULT Initialize(UINT width, UINT height);
	void Shutdown();

	bool IsInitialized() const { return _mft != nullptr; }

	// S_OK    -> *outSample holds a decoded NV12 frame
	// S_FALSE -> decoder needs more data, *outSample is null
	HRESULT Decode(const uint8_t* data, size_t length, uint64_t ptsUs, IMFSample** outSample);

	UINT OutputWidth() const { return _outWidth; }
	UINT OutputHeight() const { return _outHeight; }
	UINT OutputStride() const { return _outStride; }

private:
	HRESULT SelectOutputType();
	HRESULT PullOutputOnce(IMFSample** outSample);
	HRESULT EnsureOutputSample();

	wil::com_ptr_nothrow<IMFTransform> _mft;
	UINT32 _outWidth = 0;
	UINT32 _outHeight = 0;
	UINT32 _outStride = 0;
	DWORD _outSize = 0;

	// Reused across calls that return MF_E_TRANSFORM_NEED_MORE_INPUT, which at
	// 1080p30 is the overwhelming majority of PullOutputOnce calls. Allocating a
	// fresh IMFSample plus a ~3MB IMFMediaBuffer on each of those is roughly
	// 180MB/s of allocate-and-free inside a host process we do not own.
	//
	// Nulled the moment a frame is detached and handed to the caller, so the
	// decoder can never write into a buffer the caller is still reading, and
	// invalidated on a format change, where the new _outSize makes the old
	// buffer the wrong capacity.
	wil::com_ptr_nothrow<IMFSample> _outSample;
	DWORD _outSampleCapacity = 0;
};