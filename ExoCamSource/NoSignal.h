#pragma once

#include <windows.h>
#include <cstdint>

// Why this is not part of FrameGenerator:
// FrameGenerator is D2D-backed and needs a render target that has to be created
// and kept alive. The no-signal path must work in every situation where the real
// video path has failed -- including "we never got far enough to build a render
// target". So this writes NV12 bytes directly and depends on nothing.

enum class NoSignalReason
{
    Waiting,      // socket has never connected -- scrcpy/adb not running yet
    Connecting,   // socket is up, no decoded frame has arrived yet
    Lost,         // we had frames and they stopped (phone unplugged, server died)
};

// Fills a locked NV12 2D buffer with a dark frame and a short centred status line.
// `scanline` and `pitch` are exactly what IMF2DBuffer2::Lock2DSize hands back.
// Writes `height` luma rows starting at `scanline`, and `height/2` chroma rows
// starting at `scanline + pitch * height`.
void RenderNoSignal(BYTE* scanline, LONG pitch, UINT width, UINT height, NoSignalReason reason);