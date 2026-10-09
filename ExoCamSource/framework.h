#pragma once

#define WIN32_LEAN_AND_MEAN

#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <crtdbg.h>
#ifdef _DEBUG
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#else
#define DBG_NEW new
#endif

#include <windows.h>
#include <evntprov.h>
#include <strsafe.h>
#include <initguid.h>
#include <propvarutil.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <mferror.h>
#include <mfcaptureengine.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <uuids.h>
#include "winrt\Windows.ApplicationModel.h"

#include <string>
#include <format>

// WIL, requires the "Microsoft.Windows.ImplementationLibrary" nuget package
#include "wil/result.h"
#include "wil/stl.h"
#include "wil/win32_helpers.h"
#include "wil/com.h"

// C++/WinRT, requires the "Microsoft.Windows.CppWinRT" nuget package
#include "winrt/base.h"

#include "Log.h"

#pragma comment(lib, "mfsensorgroup")

// Defined in dllmain.cpp from kVCamSourceClsid in Shared/VCamClsid.h.
extern GUID CLSID_VCam;