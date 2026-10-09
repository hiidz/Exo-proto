#pragma once

#include <guiddef.h>

// CLSID of the virtual camera's COM media source, and the only copy of it.
// The DLL registers it under HKLM (regsvr32); the tray passes it as a string
// to MFCreateVirtualCamera as SourceClsid, and Media Foundation uses it to
// CoCreate the source in the Frame Server and in every consuming app.
// {356054e5-c733-40e8-b513-099c25e3075d}
inline constexpr GUID kVCamSourceClsid = { 0x356054e5, 0xc733, 0x40e8, { 0xb5, 0x13, 0x09, 0x9c, 0x25, 0xe3, 0x07, 0x5d } };

// Friendly name shown in Windows Camera, Discord's camera picker, etc. Changing
// it after release makes every app forget which camera the user had picked.
inline constexpr wchar_t kVCamFriendlyName[] = L"Phone Webcam";
