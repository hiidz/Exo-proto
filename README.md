# Exo

Use your Android phone's camera as a webcam on Windows 11.

Exo runs in the system tray. It uses [scrcpy](https://github.com/Genymobile/scrcpy) over adb (USB or wireless debugging) to pull H.264 video from the phone's camera, and it exposes that video as a native Windows virtual camera called **Phone Webcam**. The camera works in Windows Camera, browsers, Teams, Zoom, Discord, OBS and any other app that lists webcams. No phone app and no kernel driver are needed.

> **Status:** personal project, works on my machine. Expect rough edges, and some latency, which I'm still working on.

## How it works

```
Phone camera ──scrcpy-server (H.264)──► adb forward ──► 127.0.0.1:1234
                                                              │
Exo.exe (tray)                                                ▼
  • finds the phone (USB or mDNS wireless)        ExoCamSource.dll (Media Foundation source)
  • pushes and runs scrcpy-server                   • decodes H.264
  • registers the virtual camera                    • serves NV12 frames to Windows
                                                    • shows a "no signal" frame while the phone is away
```

| Project | What it is |
|---|---|
| `Exo` | Tray app. Registers the camera with `MFCreateVirtualCamera` and provides the Start/Stop, Device, Camera and Orientation menus. |
| `ExoCore` | Static library: adb client, device discovery, and scrcpy session supervision with auto-reconnect. |
| `ExoCamSource` | The COM media source DLL that Windows' Frame Server loads into every app using the camera. |
| `Shared` | Settings, logging and constants used by both sides. |

## Requirements

- Windows 11 on x64. The `MFCreateVirtualCamera` API doesn't exist on Windows 10, and ARM64 isn't supported yet.
- An Android phone that scrcpy can use for camera mirroring (Android 12 or later), with USB debugging or wireless debugging enabled.
- [scrcpy v4.1 for Windows x64](https://github.com/Genymobile/scrcpy/releases). Exo uses its `adb.exe` and `scrcpy-server`.
- Visual Studio 2026 with the C++ desktop workload, to build Exo.

## Build

Open `Exo.sln` and build **x64** (Debug or Release). From the command line:

```
msbuild Exo.sln -restore -p:RestorePackagesConfig=true -p:Configuration=Release -p:Platform=x64
```

Output goes to `x64\Release\`.

## Install

1. Create a folder outside your user profile, for example `C:\Exo`. The camera DLL is loaded by system services, so it must be somewhere they can read.
2. Copy `Exo.exe` and `ExoCamSource.dll` there.
3. Extract the scrcpy release next to them, so that `C:\Exo\scrcpy-win64-v4.1\adb.exe` exists.
4. Register the media source from an **administrator** prompt. Virtual camera sources must be registered under HKLM.
   ```
   regsvr32 C:\Exo\ExoCamSource.dll
   ```
5. Run `Exo.exe`. A tray icon appears and **Phone Webcam** shows up in camera apps.

To uninstall: exit Exo, run `regsvr32 /u C:\Exo\ExoCamSource.dll` as administrator, and delete the folder.

## Connecting a phone

- **USB:** enable USB debugging, plug the phone in, and accept the authorisation prompt.
- **Wireless:** enable *Wireless debugging* and pair this PC once with `adb pair` (from the scrcpy folder). After that, Exo finds the phone through mDNS and reconnects automatically when wireless debugging is toggled back on.

The tray menu lets you pick the device, which camera to use (front, back, and so on) and the capture orientation. Your choices are saved.

## Configuration

On first run, Exo writes `vcam.ini` next to `Exo.exe`. Every key in it is documented by a comment in the file. The main settings are:

| Section | Keys |
|---|---|
| `[adb]` | `serial` (blank means auto-detect), `wireless`, `port` |
| `[scrcpy]` | `version` (must match the scrcpy-server you use), `camera_id`, `capture_orientation` |
| `[video]` | `width`, `height`, `fps`, `bitrate` |
| `[log]` | `detail` |

The DLL reads the same file to learn the video format, so `vcam.ini` must stay in the same folder as `ExoCamSource.dll`. After changing `[video]`, restart the tray **and** reopen the app that uses the camera.

## Logs

Both the tray and the DLL write to `C:\Windows\Temp\Exo\logs`, one file per process. Set `detail=1` in `[log]` for per-packet tracing.

## Credits

- Exo started as a fork of [VCamSample](https://github.com/smourier/VCamSample) by Simon Mourier. Its Media Foundation virtual camera source is the foundation of `ExoCamSource`.
- Video capture is done by [scrcpy](https://github.com/Genymobile/scrcpy) (Apache-2.0) by Genymobile. scrcpy is not included in this repository.

## License

MIT. See [LICENSE](LICENSE).
