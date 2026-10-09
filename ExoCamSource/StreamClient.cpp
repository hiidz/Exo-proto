#include "pch.h"
#include "StreamClient.h"

#include <mutex>
#include <string>
#include <cstdarg>
#pragma comment(lib, "ws2_32.lib")

namespace
{
    void EnsureWinsock()
    {
        static std::once_flag once;
        std::call_once(once, []
            {
                WSADATA wsa{};
                WSAStartup(MAKEWORD(2, 2), &wsa);
                // Deliberately no matching WSACleanup. This DLL is loaded into
                // Teams / Chrome / svchost; we must not be the component that
                // decrements winsock's refcount during host teardown.
            });
    }

    uint32_t ReadBE32(const uint8_t* p)
    {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }

    uint64_t ReadBE64(const uint8_t* p)
    {
        return ((uint64_t)ReadBE32(p) << 32) | ReadBE32(p + 4);
    }
}

void StreamClient::Start(uint16_t port)
{
    if (_running.exchange(true))
        return;

    _thread = std::thread(&StreamClient::ThreadProc, this, port);
}

void StreamClient::Stop()
{
    if (!_running.exchange(false))
        return;

    // shutdown(), never closesocket(), and _socket is read rather than
    // exchanged. shutdown() is enough to unblock the reader's recv(); closing
    // here would free the handle number while that recv() is still in flight,
    // and Winsock is free to hand the same number straight back to an unrelated
    // socket() elsewhere in the process. This DLL loads into Teams, Chrome and
    // svchost, so "elsewhere" is not hypothetical. Leaving the handle in place
    // lets the reader close it exactly once, after recv() has returned.
    const SOCKET s = _socket.load();
    if (s != INVALID_SOCKET)
        shutdown(s, SD_BOTH);

    if (_thread.joinable())
        _thread.join();
}

SOCKET StreamClient::ConnectOnce(uint16_t port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;

    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    // Non-blocking connect with a short deadline, then back to blocking for the
    // read loop with a receive timeout so an idle stream can't wedge the thread.
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(s, &writable);
    timeval timeout{ 0, kConnectTimeoutMs * 1000 };

    if (select(0, nullptr, &writable, nullptr, &timeout) != 1)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }

    int err = 0;
    int errLen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen);
    if (err != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }

    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);

    DWORD recvTimeoutMs = kRecvTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs), sizeof(recvTimeoutMs));

    return s;
}

void StreamClient::ThreadProc(uint16_t port)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    EnsureWinsock();

    std::vector<uint8_t> buffer(64 * 1024);

    while (_running.load(std::memory_order_relaxed))
    {
        const SOCKET s = ConnectOnce(port);
        if (s == INVALID_SOCKET)
        {
            Log::Line(L"StreamClient: connect to 127.0.0.1:%u failed, WSA error %u", port, WSAGetLastError());
            for (int i = 0; i < kReconnectRetryCount && _running.load(std::memory_order_relaxed); i++)
                Sleep(kReconnectRetryDelayMs);
            continue;
        }

        _socket.store(s);
        _connected.store(true);
        ResetParserState();

        Log::Line(L"StreamClient: CONNECTED to 127.0.0.1:%u", port);

        while (_running.load(std::memory_order_relaxed))
        {
            const int n = recv(s, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);

            if (n > 0)
            {
                _bytes.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
                _rx.insert(_rx.end(), buffer.data(), buffer.data() + n);
                Consume();

                // A desync is unrecoverable within the connection: we have no
                // idea where the next packet header starts. Drop the socket and
                // resync from a fresh preamble.
                if (_desynced)
                {
                    Log::Line(L"StreamClient: parser desynced, dropping connection to resync");
                    break;
                }

                if (Unconsumed() > kMaxBufferedBytes)
                {
                    Log::Line(L"StreamClient: receive buffer exceeded %zu bytes, dropping connection", kMaxBufferedBytes);
                    break;
                }

                continue;
            }

            if (n == 0)
            {
                Log::Line(L"StreamClient: peer closed the connection");
                break;
            }

            const int wsaError = WSAGetLastError();
            if (wsaError == WSAETIMEDOUT)
                continue;

            Log::Line(L"StreamClient: recv failed, WSA error %u", wsaError);
            break;
        }

        _connected.store(false);

        // _latestFrame is deliberately kept: on an unplug the last good frame
        // should persist rather than snapping to a placeholder mid-sentence.
        // GetLatestFrame ages it out after ~2s.
        ShutdownDecoder();

        const SOCKET stale = _socket.exchange(INVALID_SOCKET);
        if (stale != INVALID_SOCKET)
            closesocket(stale);
    }

    Log::Line(L"StreamClient: reader thread exiting");

    {
        winrt::slim_lock_guard lock(_frameLock);
        _latestFrame.reset();
    }

    ShutdownDecoder();
    _decodedFrames.store(0, std::memory_order_relaxed);

    MFShutdown();
    CoUninitialize();
}

void StreamClient::ResetParserState()
{
    _rx.clear();
    _rxRead = 0;
    _preambleDone = false;
    _desynced = false;
    _packetIndex = 0;
    _lastPts = 0;
    _width = 0;
    _height = 0;

    // Per connection, not per process: this counter is what picks between the
    // Connecting and Lost placeholders. Carried across connections it would
    // read "Lost" while a fresh connection was still coming up.
    _decodedFrames.store(0, std::memory_order_relaxed);
}

void StreamClient::ShutdownDecoder()
{
    _decoder.Shutdown();
    _decoderWidth = 0;
    _decoderHeight = 0;
}

void StreamClient::ParsePreamble()
{
    const uint8_t* p = _rx.data() + _rxRead;

    char name[65] = {};
    memcpy(name, p + 1, 64);

    char codec[5] = {};
    memcpy(codec, p + 65, 4);

    // 4 undocumented bytes sit at +69, between the codec id and the dimensions.
    _width = ReadBE32(p + 73);
    _height = ReadBE32(p + 77);

    Log::Line(L"device '%S'  codec '%S'  %ux%u", name, codec, _width, _height);

    if (strcmp(codec, "h264") != 0)
    {
        Log::Line(L"!! codec id is not h264 -- preamble layout is wrong");
        _desynced = true;
        return;
    }

    if (!_width || _width > 8192 || !_height || _height > 8192)
    {
        Log::Line(L"!! implausible %ux%u -- preamble layout is wrong", _width, _height);
        _desynced = true;
    }
}

// Per-packet NAL dump. Checks the detail flag up front rather than relying on
// LOG_DETAIL's argument evaluation: the scan below walks every byte of the
// packet, and by the time the macro sees its arguments that has already run.
//
// After the first 40 packets only config and keyframe packets are dumped -- a
// line per frame at 30fps buries everything else.
void StreamClient::LogPacket(uint64_t ptsAndFlags, const uint8_t* data, size_t length)
{
    if (!Log::IsDetail())
        return;

    static constexpr uint64_t kFlagConfig = 1ull << 62;
    static constexpr uint64_t kFlagKeyFrame = 1ull << 61;
    static constexpr uint64_t kPtsMask = (1ull << 61) - 1;

    const bool isConfig = (ptsAndFlags & kFlagConfig) != 0;
    const bool isKey = (ptsAndFlags & kFlagKeyFrame) != 0;
    const uint64_t pts = ptsAndFlags & kPtsMask;

    if (_packetIndex >= 40 && !isConfig && !isKey)
    {
        _lastPts = pts;
        return;
    }

    std::wstring nals;
    for (size_t i = 0; i + 4 < length; i++)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        {
            wchar_t t[8];
            swprintf_s(t, L"%u ", data[i + 4] & 0x1F);
            nals += t;
        }
    }

    Log::Line(L"#%-4u flags=0x%016llX pts=%llu delta=%lld len=%u nal=[ %s]",
        _packetIndex,
        ptsAndFlags & 0xE000000000000000ull,
        pts,
        _packetIndex ? (int64_t)(pts - _lastPts) : 0,
        (unsigned)length,
        nals.c_str());

    _lastPts = pts;
}

void StreamClient::OnPacket(uint64_t ptsAndFlags, const uint8_t* data, size_t length)
{
    static constexpr uint64_t kPtsMask = (1ull << 61) - 1;
    const uint64_t pts = ptsAndFlags & kPtsMask;

    LogPacket(ptsAndFlags, data, length);
    _packetIndex++;

    // Resolution only changes across connections in practice, but if the
    // preamble ever disagrees with what the decoder was built for, rebuild
    // rather than emit frames with the wrong geometry.
    if (_decoder.IsInitialized() && (_decoderWidth != _width || _decoderHeight != _height))
    {
        Log::Line(L"resolution changed %ux%u -> %ux%u, reinitialising decoder",
            _decoderWidth, _decoderHeight, _width, _height);
        ShutdownDecoder();
    }

    if (!_decoder.IsInitialized())
    {
        if (!_width || !_height)
            return;

        const HRESULT hr = _decoder.Initialize(_width, _height);
        Log::Line(L"decoder init %ux%u -> 0x%08X", _width, _height, hr);
        if (FAILED(hr))
        {
            _desynced = true;
            return;
        }

        _decoderWidth = _width;
        _decoderHeight = _height;
    }

    wil::com_ptr_nothrow<IMFSample> decoded;
    const HRESULT hr = _decoder.Decode(data, length, pts, &decoded);

    if (FAILED(hr))
    {
        Log::Line(L"!! decode failed at #%u: 0x%08X", _packetIndex, hr);
        return;
    }

    if (hr != S_OK || !decoded)
        return;

    winrt::slim_lock_guard lock(_frameLock);
    _latestFrame = decoded;
    _frameStride = _decoder.OutputStride();
    _framePaddedHeight = _decoder.OutputHeight();
    _frameWidth = _width;
    _frameHeight = _height;
    _latestFrameTime = MFGetSystemTime();
    _decodedFrames.fetch_add(1, std::memory_order_relaxed);
}

HRESULT StreamClient::GetLatestFrame(IMFSample** sample, UINT* stride, UINT* paddedHeight,
    UINT* srcWidth, UINT* srcHeight)
{
    *sample = nullptr;

    winrt::slim_lock_guard lock(_frameLock);

    if (!_latestFrame)
        return S_FALSE;

    constexpr MFTIME kMaxFrameAge = 2 * 1000 * 10000; // 2s in 100ns units

    if (MFGetSystemTime() - _latestFrameTime > kMaxFrameAge)
    {
        _latestFrame.reset();
        return S_FALSE;
    }

    *stride = _frameStride;
    *paddedHeight = _framePaddedHeight;
    *srcWidth = _frameWidth;
    *srcHeight = _frameHeight;

    return _latestFrame.copy_to(sample);
}

void StreamClient::Consume()
{
    while (!_desynced)
    {
        // Recomputed each iteration because _rxRead moves at the bottom. _rx
        // itself is not resized in between, so p stays valid across OnPacket --
        // the append and the Compact() both happen outside this loop.
        const uint8_t* const p = _rx.data() + _rxRead;
        const size_t available = Unconsumed();

        if (!_preambleDone)
        {
            if (available < kPreambleSize)
                break;

            ParsePreamble();
            if (_desynced)
                break;

            _rxRead += kPreambleSize;
            _preambleDone = true;
            continue;
        }

        if (available < kHeaderSize)
            break;

        const uint64_t pts = ReadBE64(p);
        const uint32_t len = ReadBE32(p + 8);

        if (len == 0 || len > 4 * 1024 * 1024)
        {
            Log::Line(L"!! implausible length %u at packet #%u -- desync", len, _packetIndex);
            _desynced = true;
            break;
        }

        if (available < kHeaderSize + len)
            break;

        OnPacket(pts, p + kHeaderSize, len);
        _rxRead += kHeaderSize + len;
    }

    Compact();
}

void StreamClient::Compact()
{
    if (_rxRead == 0)
        return;

    if (_rxRead >= _rx.size())
    {
        _rx.clear();
        _rxRead = 0;
        return;
    }

    if (_rxRead >= kCompactThresholdBytes)
    {
        _rx.erase(_rx.begin(), _rx.begin() + _rxRead);
        _rxRead = 0;
    }
}