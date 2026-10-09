#include "pch.h"
#include "Tools.h"
#include "FrameGenerator.h"

namespace
{
	constexpr UINT kFramesForFps = 60;  // frames between fps recomputations
	constexpr UINT kNsPerMs = 10000;    // 100ns units per millisecond
	constexpr UINT kMsPerS = 1000;
	constexpr float kBlock = 20;        // test pattern block size in pixels
}

HRESULT FrameGenerator::EnsureRenderTarget(UINT width, UINT height)
{
	// Rebuild only on first use or a resolution change: this creates every D2D
	// object plus one brush per grid cell, which is thousands at 1080p.
	if (_renderTarget && _width == width && _height == height)
		return S_OK;

	wil::com_ptr_nothrow<ID2D1Factory> d2d1Factory;
	RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&d2d1Factory)));

	wil::com_ptr_nothrow<IWICImagingFactory> wicFactory;
	RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wicFactory)));

	RETURN_IF_FAILED(wicFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &_bitmap));

	D2D1_RENDER_TARGET_PROPERTIES props{};
	props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
	RETURN_IF_FAILED(d2d1Factory->CreateWicBitmapRenderTarget(_bitmap.get(), props, &_renderTarget));

	RETURN_IF_FAILED(_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &_whiteBrush));
	RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&_dwrite));
	RETURN_IF_FAILED(_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40, L"", &_textFormat));
	RETURN_IF_FAILED(_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
	RETURN_IF_FAILED(_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));

	_cols = (UINT)(width / kBlock);
	_rows = (UINT)(height / kBlock);
	_blockBrushes.clear();
	_blockBrushes.resize((size_t)_cols * _rows);

	for (UINT i = 0; i < _cols; i++)
	{
		for (UINT j = 0; j < _rows; j++)
		{
			const auto color = HSL2RGB((float)i / _rows, 1, (float)j / _cols);
			RETURN_IF_FAILED(_renderTarget->CreateSolidColorBrush(color, &_blockBrushes[(size_t)i * _rows + j]));
		}
	}

	_width = width;
	_height = height;
	_prevTime = MFGetSystemTime();
	_frame = 0;
	return S_OK;
}

HRESULT FrameGenerator::Generate(IMFSample* sample, IMFSample** outSample)
{
	RETURN_HR_IF_NULL(E_POINTER, sample);
	RETURN_HR_IF_NULL(E_POINTER, outSample);
	*outSample = nullptr;
	RETURN_HR_IF(E_NOT_VALID_STATE, !_renderTarget);

	_renderTarget->BeginDraw();

	for (UINT i = 0; i < _cols; i++)
	{
		for (UINT j = 0; j < _rows; j++)
		{
			_renderTarget->FillRectangle(
				D2D1::Rect(i * kBlock, j * kBlock, (i + 1) * kBlock, (j + 1) * kBlock),
				_blockBrushes[(size_t)i * _rows + j].get());
		}
	}

	const auto radius = kBlock * 2;
	constexpr float padding = 1;
	_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(radius + padding, radius + padding), radius, radius), _whiteBrush.get());
	_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(radius + padding, _height - radius - padding), radius, radius), _whiteBrush.get());
	_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(_width - radius - padding, radius + padding), radius, radius), _whiteBrush.get());
	_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(_width - radius - padding, _height - radius - padding), radius, radius), _whiteBrush.get());
	_renderTarget->DrawRectangle(D2D1::Rect(radius, radius, _width - radius, _height - radius), _whiteBrush.get());

	if (!_fps || !(_frame % kFramesForFps))
	{
		const auto time = MFGetSystemTime();
		_fps = (UINT)(kMsPerS * kNsPerMs * kFramesForFps / (time - _prevTime));
		_prevTime = time;
	}

	// _TRUNCATE rather than swprintf_s: on overflow swprintf_s invokes the
	// invalid parameter handler, which terminates the host process.
	wchar_t text[256];
	int len = _snwprintf_s(text, _countof(text), _TRUNCATE,
		L"Frame#: %I64u\nFps: %u\nResolution: %u x %u\nStream: %s %I64u KB\nDecoded: %u",
		_frame, _fps, _width, _height,
		_connected ? L"CONNECTED" : L"waiting...",
		_bytesReceived / 1024, _decodedFrames);

	if (len < 0)
		len = (int)wcsnlen(text, _countof(text));

	wil::com_ptr_nothrow<IDWriteTextLayout> layout;
	if (SUCCEEDED(_dwrite->CreateTextLayout(text, len, _textFormat.get(), (FLOAT)_width, (FLOAT)_height, &layout)))
	{
		_renderTarget->DrawTextLayout(D2D1::Point2F(0, 0), layout.get(), _whiteBrush.get());
	}

	// EndDraw must pair with BeginDraw on every path, so nothing above returns early.
	RETURN_IF_FAILED(_renderTarget->EndDraw());

	// WIC bitmap (RGB32) -> the caller's NV12 sample. NV12 is the only subtype
	// MediaStream advertises, so there is no format branch here.
	wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &mediaBuffer));

	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	RETURN_IF_FAILED(mediaBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));

	BYTE* scanline = nullptr;
	LONG pitch = 0;
	BYTE* start = nullptr;
	DWORD length = 0;
	RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));

	wil::com_ptr_nothrow<IWICBitmapLock> lock;
	HRESULT hr = _bitmap->Lock(nullptr, WICBitmapLockRead, &lock);
	if (SUCCEEDED(hr))
	{
		UINT w = 0, h = 0, wicStride = 0, wicSize = 0;
		WICInProcPointer wicPointer = nullptr;

		if (SUCCEEDED(hr = lock->GetSize(&w, &h)) &&
			SUCCEEDED(hr = lock->GetStride(&wicStride)) &&
			SUCCEEDED(hr = lock->GetDataPointer(&wicSize, &wicPointer)) &&
			assert_true(wicPointer))
		{
			hr = RGB32ToNV12(wicPointer, wicSize, wicStride, w, h, scanline, length, pitch);
		}

		lock.reset();
	}

	buffer2D->Unlock2D();
	RETURN_IF_FAILED(hr);

	_frame++;
	sample->AddRef();
	*outSample = sample;
	return S_OK;
}