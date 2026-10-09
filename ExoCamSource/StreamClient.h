#pragma once

// winsock2.h MUST precede windows.h. If pch.h pulls in windows.h first you get a
// wall of redefinition errors from winsock.h. Include this header first, or put
// WIN32_LEAN_AND_MEAN at the top of pch.h.
#include "H264Decoder.h"
#include "Log.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// Connects to a local TCP port, parses scrcpy's framing, decodes H264 and keeps
// the most recent NV12 sample available for the media stream to pick up.
class StreamClient
{
public:
    StreamClient() = default;
    ~StreamClient() { Stop(); }

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // Returns immediately. The reader thread retries forever until Stop().
    void Start(uint16_t port);
    void Stop();

    bool IsConnected() const { return _connected.load(std::memory_order_relaxed); }
    uint64_t BytesReceived() const { return _bytes.load(std::memory_order_relaxed); }
    uint32_t DecodedFrames() const { return _decodedFrames.load(std::memory_order_relaxed); }

    // Distinguishes "never worked" from "worked then stopped", which get
    // different placeholder text. Reset per connection, not per process.
    bool HasDecodedAnyFrame() const { return DecodedFrames() > 0; }

    // S_OK with a frame, or S_FALSE if there is nothing fresh. srcWidth/srcHeight
    // are the VISIBLE dimensions; stride/paddedHeight describe the decoder's
    // buffer geometry, which is larger (macroblock padding). Passing the visible
    // size out is what stops the caller reading off the end of the buffer when the
    // negotiated media type doesn't match what the phone sent.
    HRESULT GetLatestFrame(IMFSample** sample, UINT* stride, UINT* paddedHeight,
        UINT* srcWidth, UINT* srcHeight);

private:
    void ThreadProc(uint16_t port);
    SOCKET ConnectOnce(uint16_t port);

    void ParsePreamble();
    void Consume();
    void Compact();
    void OnPacket(uint64_t ptsAndFlags, const uint8_t* data, size_t length);
    void LogPacket(uint64_t ptsAndFlags, const uint8_t* data, size_t length);

    // Clears parsing state so a fresh connection starts from a known point.
    // Everything Consume()/ParsePreamble()/OnPacket() treat as "where are we in
    // the stream" lives here, in one place, so a new parser field can't be added
    // without also being reset.
    void ResetParserState();

    // Tears down the decoder and the resolution it was built for, so the next
    // OnPacket() rebuilds against what the new connection actually sends.
    void ShutdownDecoder();

    size_t Unconsumed() const { return _rx.size() - _rxRead; }

    static constexpr size_t kPreambleSize = 81;   // 1 dummy + 64 name + 16 codec meta
    static constexpr size_t kHeaderSize = 12;     // 8 pts/flags + 4 length

    // A desync stops us consuming, so _rx would otherwise grow until the process
    // dies. Cap it and force a reconnect instead.
    static constexpr size_t kMaxBufferedBytes = 16 * 1024 * 1024;

    // Reclaim the consumed prefix only when it is worth the memmove. Below this
    // the read cursor just advances.
    static constexpr size_t kCompactThresholdBytes = 1 * 1024 * 1024;

    // A missing phone must never stall camera startup: the Frame Server has its
    // own init timeout and will report the camera as broken if we sit here.
    static constexpr int kConnectTimeoutMs = 300;
    static constexpr DWORD kRecvTimeoutMs = 2000;
    static constexpr int kReconnectRetryCount = 10;
    static constexpr int kReconnectRetryDelayMs = 100;

    std::thread _thread;
    std::atomic<bool> _running{ false };
    std::atomic<bool> _connected{ false };
    std::atomic<uint64_t> _bytes{ 0 };
    std::atomic<uint32_t> _decodedFrames{ 0 };
    std::atomic<SOCKET> _socket{ INVALID_SOCKET };

    winrt::slim_mutex _frameLock;
    wil::com_ptr_nothrow<IMFSample> _latestFrame;
    UINT _frameStride = 0;
    UINT _framePaddedHeight = 0;
    UINT _frameWidth = 0;
    UINT _frameHeight = 0;
    MFTIME _latestFrameTime = 0;

    std::vector<uint8_t> _rx;
    size_t _rxRead = 0;          // first unconsumed byte in _rx
    bool _preambleDone = false;
    bool _desynced = false;
    uint32_t _packetIndex = 0;
    uint64_t _lastPts = 0;
    uint32_t _width = 0;
    uint32_t _height = 0;

    H264Decoder _decoder;
    uint32_t _decoderWidth = 0;
    uint32_t _decoderHeight = 0;
};