#pragma once

// CLSID of the virtual camera's COM media source. MUST stay in sync with
// CLSID_VCam in dllmain.cpp, which is what regsvr32 writes under HKLM: the tray
// passes this string to MFCreateVirtualCamera as SourceClsid, and Media
// Foundation uses it to CoCreate the source in the Frame Server and in every
// consuming app.
inline constexpr wchar_t kVCamSourceClsid[] = L"{3cad447d-f283-4af4-a3b2-6f5363309f52}";

// Friendly name shown in Windows Camera, Discord's camera picker, etc. Changing
// it after release makes every app forget which camera the user had picked.
inline constexpr wchar_t kVCamFriendlyName[] = L"Phone Webcam";