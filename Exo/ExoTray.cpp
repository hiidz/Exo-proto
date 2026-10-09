// The tray application. Owns three things and nothing else: the virtual camera
// registration, the ScrcpySupervisor that keeps a phone session alive, and the
// notification icon and its menu.
//
// The camera registration and the streaming session are independent. The camera
// registers at startup and stays registered whether or not a phone is reachable
// -- the DLL shows its NoSignal frame in the gap -- so nothing in the streaming
// path can take the camera out of an app's device list.
//
// Everything here runs on the message-loop thread, including the adb calls the
// Device and Camera menus make. There is no worker thread in this file; the
// supervisor owns the only one.
#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <string>

#include "ScrcpySupervisor.h"
#include "VCamConfig.h"
#include "VCamClsid.h"
#include "DeviceResolver.h"
#include "CameraList.h"
#include "Log.h"

#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT_PTR kStatusTimerId = 1;
    constexpr UINT ID_TRAY_EXIT = 1001;
    constexpr UINT ID_TRAY_STARTSTOP = 1002;
    constexpr UINT ID_TRAY_DEVICE_FIRST = 1100;
    constexpr UINT ID_TRAY_CAMERA_FIRST = 1200;
    constexpr UINT ID_TRAY_ORIENTATION_FIRST = 1300;

    struct OrientationOption
    {
        const wchar_t* value;   // what capture_orientation takes; empty = omit it
        const wchar_t* label;
    };

    // Fixed set defined by scrcpy, not a device capability -- unlike camera ids
    // there is nothing to enumerate, so this submenu is built inline.
    constexpr OrientationOption kOrientations[] =
    {
        { L"",        L"Default" },
        { L"0",       L"0" },
        { L"90",      L"90" },
        { L"180",     L"180" },
        { L"270",     L"270" },
        { L"flip0",   L"Flip" },
        { L"flip90",  L"Flip + 90" },
        { L"flip180", L"Flip + 180" },
        { L"flip270", L"Flip + 270" },
    };


    // Bundled scrcpy release folder (as downloaded from
    // github.com/Genymobile/scrcpy) -- adb.exe, its companion DLLs, and the
    // scrcpy-server binary all come from here as one matched set. Update this
    // one constant when the bundled scrcpy version changes; nothing else
    // needs to, except `version` in vcam.ini which must match the jar.
    constexpr const wchar_t* kScrcpyToolsDir = L"scrcpy-win64-v4.1";

    NOTIFYICONDATAW g_nid{};
    HWND g_hwnd = nullptr;
    ScrcpySupervisor g_supervisor;
    ScrcpySupervisor::State g_lastState = ScrcpySupervisor::State::Stopped;
    VCamConfig g_config;
    std::vector<DeviceCandidate> g_deviceMenu;

    // Enumerating cameras costs a jar push and a JVM start, and the ids do not
    // change under a device, so the list is kept. g_cameraDevice records which
    // device it was built from.
    std::vector<CameraInfo> g_cameraList;
    std::string g_cameraDevice;
    HMENU g_cameraSubmenu = nullptr;

    // Raw pointer, not wil::com_ptr: this project has no other dependency on
    // WIL, and with MFVirtualCameraLifetime_Session the only contract that
    // matters is release no later than process exit, which WM_DESTROY already
    // guarantees. Created and released on the message-loop thread, which
    // IMFVirtualCamera requires to outlive the camera.
    IMFVirtualCamera* g_vcam = nullptr;

    // Resolves to the folder containing this EXE, regardless of what the
    // process's current working directory happens to be -- double-click from
    // Explorer, launched via a shortcut, autostart via Task Scheduler, or run
    // from the VS debugger (which sets CWD to the project folder, not the
    // output folder) all give different CWDs, but the EXE's own location
    // never moves. Everything bundled next to the EXE should be found via
    // this, not via ".\\".
    std::wstring GetExeDirectory()
    {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path, len);
        const size_t lastSlash = dir.find_last_of(L"\\/");
        return (lastSlash != std::wstring::npos) ? dir.substr(0, lastSlash + 1) : L".\\";
    }

    // Fails loudly at startup instead of the supervisor silently retrying
    // forever in a background thread with nothing but a log to explain why --
    // a missing bundled tool is a setup mistake, not a runtime condition.
    bool CheckFileExists(const std::wstring& path, const wchar_t* whatFor)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            const std::wstring msg = L"Missing " + std::wstring(whatFor) + L":\n" + path +
                L"\n\nCopy the scrcpy release folder (adb.exe, scrcpy-server, and its DLLs) "
                L"next to Exo.exe.";
            MessageBoxW(nullptr, msg.c_str(), L"VCam Tray", MB_OK | MB_ICONERROR);
            return false;
        }
        return true;
    }

    // Registration failures are the one class of error a user can act on
    // (usually an unregistered DLL), so they get a message box with the system
    // text for the HRESULT rather than a log line.
    std::wstring FormatHResult(HRESULT hr)
    {
        wchar_t errorText[256]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, hr, 0, errorText, _countof(errorText), nullptr);
        wchar_t buf[512];
        wsprintfW(buf, L"Error 0x%08X: %s", hr, errorText);
        return buf;
    }

    // MFVirtualCameraLifetime_Session ties the camera's existence to this
    // process, so tray lifetime is camera lifetime and Start/Stop on the menu
    // needs no protocol with anything. Pair with UnregisterVirtualCamera().
    HRESULT RegisterVirtualCamera()
    {
        wchar_t clsid[39];
        StringFromGUID2(kVCamSourceClsid, clsid, ARRAYSIZE(clsid));

        HRESULT hr = MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser,
            kVCamFriendlyName,
            clsid,
            nullptr,
            0,
            &g_vcam);
        if (FAILED(hr))
            return hr;

        hr = g_vcam->Start(nullptr);
        if (FAILED(hr))
        {
            g_vcam->Release();
            g_vcam = nullptr;
            return hr;
        }

        Log::Line(L"virtual camera registered and started");
        return S_OK;
    }

    // Remove(), not Shutdown(): Shutdown() causes a double Shutdown on the
    // media source and leaves the camera behind. Load-bearing upstream detail.
    void UnregisterVirtualCamera()
    {
        if (!g_vcam)
            return;

        const HRESULT hr = g_vcam->Remove();
        Log::Line(L"virtual camera removed, hr=0x%08X", hr);

        g_vcam->Release();
        g_vcam = nullptr;
    }

    const wchar_t* StatusText(ScrcpySupervisor::State s)
    {
        switch (s)
        {
        case ScrcpySupervisor::State::Stopped:          return L"vcam: stopped";
        case ScrcpySupervisor::State::WaitingForDevice: return L"vcam: no phone";
        case ScrcpySupervisor::State::Starting:         return L"vcam: connecting...";
        case ScrcpySupervisor::State::Running:          return L"vcam: streaming";
        case ScrcpySupervisor::State::Failed:           return L"vcam: error, see log";
        default:                                        return L"vcam";
        }
    }

    void UpdateTrayTooltip()
    {
        const auto status = g_supervisor.GetStatus();
        if (status.state == g_lastState)
            return;
        g_lastState = status.state;

        wcsncpy_s(g_nid.szTip, StatusText(status.state), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }

    void SupervisorLog(const std::string& line)
    {
        Log::Line("%s", line.c_str());
    }

    std::wstring CameraMenuText(const CameraInfo& camera)
    {
        std::wstring text = Widen(camera.id);

        if (!camera.facing.empty())
            text += L"  -  " + Widen(camera.facing);
        if (!camera.size.empty())
            text += L", " + Widen(camera.size);

        return text;
    }

    // Called from WM_INITMENUPOPUP, not when the tray menu is built: filling
    // this runs adb and starts a JVM on the phone, which is seconds rather than
    // milliseconds, and it blocks the message loop for that whole time.
    void PopulateCameraMenu(HMENU menu)
    {
        while (DeleteMenu(menu, 0, MF_BYPOSITION)) {}

        const auto status = g_supervisor.GetStatus();

        // Rebuilt only when the supervisor is on a different device than the
        // list was built from, which also covers auto-selection landing
        // somewhere new without the pin having changed. With no active device
        // the cache is trusted, since there is nothing to invalidate against.
        const bool cached =
            !g_cameraList.empty() &&
            (!status.hasActiveDevice || status.activeSerial == g_cameraDevice);

        if (!cached)
        {
            g_cameraList.clear();
            g_cameraDevice.clear();

            AdbConfig resolved;
            DeviceCandidate chosen;
            std::string error;

            if (!ResolveDevice(g_config.adb, resolved, error, SupervisorLog, &chosen))
            {
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Widen(error).c_str());
                return;
            }

            g_cameraList = ListCameras(AdbClient(resolved), g_config.scrcpy, error);

            if (g_cameraList.empty())
            {
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Widen(error).c_str());
                return;
            }

            g_cameraDevice = chosen.serial;
        }

        for (size_t i = 0; i < g_cameraList.size(); ++i)
        {
            const auto& camera = g_cameraList[i];

            // Unset means the server picks, and the server picks the first
            // camera it listed -- so the check mark goes on entry 0.
            const bool active = g_config.scrcpy.cameraId.empty()
                ? (i == 0)
                : Widen(camera.id) == g_config.scrcpy.cameraId;

            AppendMenuW(
                menu,
                MF_STRING | (active ? MF_CHECKED : 0),
                ID_TRAY_CAMERA_FIRST + (UINT)i,
                CameraMenuText(camera).c_str());
        }
    }

    // The supervisor takes its options by value at Start(), so a change to
    // g_config only reaches the phone through a stop and a start. A stopped
    // supervisor stays stopped -- the change is picked up whenever it next runs.
    void RestartIfRunning()
    {
        if (g_supervisor.GetStatus().state == ScrcpySupervisor::State::Stopped)
            return;

        g_supervisor.Stop();
        g_supervisor.Start(g_config.adb, g_config.scrcpy, SupervisorLog);
    }

    void ShowContextMenu(HWND hwnd)
    {
        POINT pt;
        GetCursorPos(&pt);
        const auto status = g_supervisor.GetStatus();
        const bool running = status.state != ScrcpySupervisor::State::Stopped;

        // Enumerating runs adb on the message-loop thread before the menu can
        // appear. Normally tens of milliseconds; the short timeout bounds how
        // long a wedged adb server can hold the menu shut.
        g_deviceMenu = ListDeviceCandidates(g_config.adb, 3000);

        const HMENU devices = CreatePopupMenu();

        if (g_deviceMenu.empty())
        {
            AppendMenuW(devices, MF_STRING | MF_GRAYED, 0, L"(none found)");
        }
        else
        {
            for (size_t i = 0; i < g_deviceMenu.size(); ++i)
            {
                const auto& candidate = g_deviceMenu[i];

                const bool active =
                    status.hasActiveDevice &&
                    candidate.serial == status.activeSerial &&
                    candidate.wireless == status.activeWireless;

                const std::wstring text = Widen(candidate.serial) +
                    (candidate.wireless ? L"  (wireless)" : L"  (usb)");

                AppendMenuW(
                    devices,
                    MF_STRING | (active ? MF_CHECKED : 0),
                    ID_TRAY_DEVICE_FIRST + (UINT)i,
                    text.c_str());
            }
        }

        const HMENU cameras = CreatePopupMenu();
        AppendMenuW(cameras, MF_STRING | MF_GRAYED, 0, L"(checking...)");
        g_cameraSubmenu = cameras;

        const HMENU orientation = CreatePopupMenu();
        for (UINT i = 0; i < (UINT)ARRAYSIZE(kOrientations); ++i)
        {
            const bool active =
                g_config.scrcpy.captureOrientation == kOrientations[i].value;

            AppendMenuW(
                orientation,
                MF_STRING | (active ? MF_CHECKED : 0),
                ID_TRAY_ORIENTATION_FIRST + i,
                kOrientations[i].label);
        }

        const HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_STARTSTOP, running ? L"Stop" : L"Start");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)devices, L"Device");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)cameras, L"Camera");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)orientation, L"Orientation");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);

        g_cameraSubmenu = nullptr;
        DestroyMenu(menu);   // takes the submenus with it
    }

    void PinDevice(const std::string& serial, bool wireless)
    {
        const auto status = g_supervisor.GetStatus();

        const bool alreadyActive =
            status.hasActiveDevice &&
            status.activeSerial == serial &&
            status.activeWireless == wireless;

        // An explicit tray selection is the user's persistent preference, and
        // is the only thing that writes a serial to the ini. Automatic
        // resolution never does.
        if (g_config.adb.serial != serial ||
            g_config.adb.wireless != wireless)
        {
            g_config.adb.serial = serial;
            g_config.adb.wireless = wireless;

            if (!g_config.SavePinned(serial, wireless))
            {
                Log::Line("could not write device pin to vcam.ini");
            }
        }

        // Clicking the device that is already running still promotes an
        // auto-selected device into a persistent preference, but there is no
        // reason to tear down and rebuild the same session.
        if (alreadyActive)
        {
            UpdateTrayTooltip();
            return;
        }

        // The camera list belongs to the old device and has to go. camera_id
        // itself is deliberately kept: it is a preference, and it becomes
        // correct again the moment the user switches back.
        g_cameraList.clear();
        g_cameraDevice.clear();

        RestartIfRunning();
        UpdateTrayTooltip();
    }

    void SelectCamera(const std::string& id)
    {
        const std::wstring wide = Widen(id);
        if (g_config.scrcpy.cameraId == wide)
            return;

        g_config.scrcpy.cameraId = wide;

        if (!g_config.SaveCameraId(wide))
            Log::Line("could not write camera_id to vcam.ini");

        RestartIfRunning();
        UpdateTrayTooltip();
    }

    void SelectOrientation(const std::wstring& orientation)
    {
        if (g_config.scrcpy.captureOrientation == orientation)
            return;

        g_config.scrcpy.captureOrientation = orientation;

        if (!g_config.SaveOrientation(orientation))
            Log::Line("could not write capture_orientation to vcam.ini");

        RestartIfRunning();
        UpdateTrayTooltip();
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP)
                ShowContextMenu(hwnd);
            return 0;

        case WM_COMMAND:
        {
            const UINT id = LOWORD(wParam);

            if (id == ID_TRAY_EXIT)
            {
                DestroyWindow(hwnd);
            }
            else if (id == ID_TRAY_STARTSTOP)
            {
                // Stop() joins the supervisor's worker on this thread. The
                // camera registration is untouched either way -- stopping just
                // leaves the camera showing NoSignal -- but it does release the
                // phone's camera, since killing the job kills the remote server.
                if (g_supervisor.GetStatus().state == ScrcpySupervisor::State::Stopped)
                {
                    g_supervisor.Start(g_config.adb, g_config.scrcpy, SupervisorLog);
                }
                else
                {
                    g_supervisor.Stop();
                }

                // Ahead of the one-second status timer, so the tooltip is not
                // stale the moment the menu closes.
                UpdateTrayTooltip();
            }
            else if (id >= ID_TRAY_DEVICE_FIRST && id - ID_TRAY_DEVICE_FIRST < g_deviceMenu.size())
            {
                const auto& c = g_deviceMenu[id - ID_TRAY_DEVICE_FIRST];
                PinDevice(c.serial, c.wireless);
            }
            else if (id >= ID_TRAY_CAMERA_FIRST && id - ID_TRAY_CAMERA_FIRST < g_cameraList.size())
            {
                SelectCamera(g_cameraList[id - ID_TRAY_CAMERA_FIRST].id);
            }
            else if (id >= ID_TRAY_ORIENTATION_FIRST &&
                id - ID_TRAY_ORIENTATION_FIRST < (UINT)ARRAYSIZE(kOrientations))
            {
                SelectOrientation(kOrientations[id - ID_TRAY_ORIENTATION_FIRST].value);
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kStatusTimerId)
                UpdateTrayTooltip();
            return 0;

        case WM_DESTROY:
            // Order matters. Stop() joins the supervisor's worker, and that
            // worker's ScrcpySession destructor is what removes the adb port
            // forward -- skipping the join leaks a forward that breaks the next
            // launch. Only then tear down the camera that would otherwise still
            // be asked for frames, and take the icon down last so it stays
            // visible for the whole shutdown.
            //
            // No kill-server, here or anywhere. The adb server on 5037 is
            // shared with whatever else on the machine is using it.
            g_supervisor.Stop();
            UnregisterVirtualCamera();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;

        case WM_INITMENUPOPUP:
            if ((HMENU)wParam == g_cameraSubmenu)
            {
                PopulateCameraMenu(g_cameraSubmenu);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // Single-instance guard: two copies would both fight over the same
    // device, port forward, remote scrcpy-server process, AND the same
    // MFVirtualCameraLifetime_Session camera registration.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\ExoTrayApp_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex)
            CloseHandle(hMutex);
        return 0;
    }

    // STA, because this is a single message-loop thread and virtual camera
    // creation is COM activation underneath. CoInitializeEx directly rather
    // than winrt::init_apartment, to avoid pulling in C++/WinRT for one call.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(MFStartup(MF_VERSION)))
    {
        MessageBoxW(nullptr, L"MFStartup failed.", L"VCam Tray", MB_OK | MB_ICONERROR);
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    const wchar_t* kClassName = L"ExoTrayAppWindowClass";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    // Never shown. It exists to own the notification icon and pump messages.
    g_hwnd = CreateWindowW(kClassName, L"VCam Tray", 0, 0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);   // placeholder
    wcsncpy_s(g_nid.szTip, StatusText(ScrcpySupervisor::State::Stopped), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Resolved against the EXE's own folder, not CWD -- see GetExeDirectory().
    // Both come from the same bundled scrcpy release, so they move together.
    const std::wstring exeDir = GetExeDirectory();
    const std::wstring adbPath = exeDir + kScrcpyToolsDir + L"\\adb.exe";
    const std::wstring serverPath = exeDir + kScrcpyToolsDir + L"\\scrcpy-server";

    if (!CheckFileExists(adbPath, L"adb.exe") || !CheckFileExists(serverPath, L"scrcpy-server"))
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        MFShutdown();
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // The camera appears in every app's device list as soon as the tray does,
    // and shows NoSignal until a phone turns up. A phone that is never found is
    // not an error condition here.
    const HRESULT vcamHr = RegisterVirtualCamera();
    if (FAILED(vcamHr))
    {
        const std::wstring msg = L"Could not start the virtual camera.\n"
            L"Make sure ExoCamSource.dll is registered (regsvr32, as admin).\n\n"
            + FormatHResult(vcamHr);
        MessageBoxW(nullptr, msg.c_str(), L"VCam Tray", MB_OK | MB_ICONERROR);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        MFShutdown();
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // vcam.ini supplies the device and video parameters, but not the tool
    // paths: those are overwritten from the bundled folder above, which is
    // already resolved against the EXE and checked to exist.
    std::string problem;
    g_config = VCamConfig::Load(problem);
    g_config.adb.adbPath = adbPath;
    g_config.scrcpy.serverJarPath = serverPath;

    if (!g_config.loaded)
        g_config.WriteTemplate();

    if (!problem.empty())
        Log::Line("%s", problem.c_str());

    // No device resolution here on purpose. The supervisor re-resolves on every
    // connection attempt, so wireless debugging can be toggled on after launch
    // and picked up without restarting anything.
    //
    // Started unconditionally: with no phone present this parks in
    // WaitingForDevice and retries, which is the intended resting state.
    g_supervisor.Start(g_config.adb, g_config.scrcpy, SupervisorLog);

    SetTimer(g_hwnd, kStatusTimerId, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // g_vcam and g_supervisor are already torn down in WM_DESTROY by the time
    // GetMessageW returns 0; only process-wide MF/COM teardown is left.
    MFShutdown();
    CoUninitialize();

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}