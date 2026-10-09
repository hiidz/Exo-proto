#pragma once

#include <vector>

// Debug-only D2D test pattern, compiled in by VCAM_DEBUG_PATTERN. The shipping
// placeholder is RenderNoSignal in NoSignal.h, which shares nothing with this.
//
// CPU only: the render target is a WIC bitmap. A GPU path would be dead code
// regardless, since MediaStream::SetD3DManager accepts a manager and ignores it.
class FrameGenerator
{
	UINT _width = 0;
	UINT _height = 0;
	ULONGLONG _frame = 0;
	MFTIME _prevTime;
	UINT _fps = 0;

	bool _connected = false;
	uint64_t _bytesReceived = 0;
	uint32_t _decodedFrames = 0;

	wil::com_ptr_nothrow<ID2D1RenderTarget> _renderTarget;
	wil::com_ptr_nothrow<ID2D1SolidColorBrush> _whiteBrush;
	wil::com_ptr_nothrow<IDWriteTextFormat> _textFormat;
	wil::com_ptr_nothrow<IDWriteFactory> _dwrite;
	wil::com_ptr_nothrow<IWICBitmap> _bitmap;

	// The block colours depend only on grid position, so they are built once.
	// Creating them per frame meant ~5000 COM allocations per frame.
	std::vector<wil::com_ptr_nothrow<ID2D1SolidColorBrush>> _blockBrushes;
	UINT _cols = 0;
	UINT _rows = 0;

public:
	FrameGenerator() : _prevTime(MFGetSystemTime()) {}

	void SetStatus(bool connected, uint64_t bytes, uint32_t decodedFrames)
	{
		_connected = connected;
		_bytesReceived = bytes;
		_decodedFrames = decodedFrames;
	}

	HRESULT EnsureRenderTarget(UINT width, UINT height);
	HRESULT Generate(IMFSample* sample, IMFSample** outSample);
};