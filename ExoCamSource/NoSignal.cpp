#include "pch.h"
#include "NoSignal.h"

#include <cstring>

namespace
{
    // Limited-range NV12. 16 is legal black; 24 is a very dark grey, which reads
    // as "deliberate" rather than "the codec died" on a conference call.
    constexpr BYTE kBackgroundY = 24;
    constexpr BYTE kTextY = 168;
    constexpr BYTE kNeutralUV = 128;

    // Classic 5x7 cell font, column-major, bit 0 = top row.
    struct Glyph
    {
        char c;
        uint8_t col[5];
    };

    const Glyph kFont[] =
    {
        { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
        { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
        { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
        { ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
        { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
        { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
        { '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
        { '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
        { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
        { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
        { '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
        { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
        { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
        { '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
        { 'A', { 0x7C, 0x12, 0x11, 0x12, 0x7C } },
        { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
        { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
        { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
        { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
        { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
        { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
        { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
        { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
        { 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
        { 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
        { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
        { 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } },
        { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
        { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
        { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
        { 'Q', { 0x3E, 0x41, 0x51, 0x21, 0x5E } },
        { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
        { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
        { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
        { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
        { 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } },
        { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
        { 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
        { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
        { 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
    };

    const uint8_t* FindGlyph(char c)
    {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');

        for (const auto& g : kFont)
        {
            if (g.c == c)
                return g.col;
        }
        return nullptr;
    }

    void DrawChar(BYTE* plane, LONG pitch, UINT w, UINT h, int x0, int y0, char c, int scale)
    {
        const uint8_t* glyph = FindGlyph(c);
        if (!glyph)
            return;

        for (int col = 0; col < 5; col++)
        {
            const uint8_t bits = glyph[col];
            if (!bits)
                continue;

            for (int row = 0; row < 7; row++)
            {
                if (!(bits & (1u << row)))
                    continue;

                for (int sy = 0; sy < scale; sy++)
                {
                    const int py = y0 + row * scale + sy;
                    if (py < 0 || (UINT)py >= h)
                        continue;

                    BYTE* dst = plane + (size_t)py * pitch;
                    for (int sx = 0; sx < scale; sx++)
                    {
                        const int px = x0 + col * scale + sx;
                        if (px < 0 || (UINT)px >= w)
                            continue;

                        dst[px] = kTextY;
                    }
                }
            }
        }
    }

    int TextWidth(const char* text, int scale)
    {
        const int len = (int)strlen(text);
        if (len <= 0)
            return 0;

        // 5 columns per glyph + 1 column of tracking, minus the trailing gap.
        return len * 6 * scale - scale;
    }

    void DrawText(BYTE* plane, LONG pitch, UINT w, UINT h, int x0, int y0, const char* text, int scale)
    {
        for (int i = 0; text[i]; i++)
        {
            DrawChar(plane, pitch, w, h, x0 + i * 6 * scale, y0, text[i], scale);
        }
    }

    const char* MessageFor(NoSignalReason reason)
    {
        switch (reason)
        {
        case NoSignalReason::Connecting: return "CONNECTING";
        case NoSignalReason::Lost:       return "SIGNAL LOST";
        case NoSignalReason::Waiting:
        default:                         return "NO CAMERA STREAM";
        }
    }
}

void RenderNoSignal(BYTE* scanline, LONG pitch, UINT width, UINT height, NoSignalReason reason)
{
    if (!scanline || pitch <= 0 || !width || !height)
        return;

    for (UINT y = 0; y < height; y++)
        memset(scanline + (size_t)y * pitch, kBackgroundY, width);

    BYTE* uv = scanline + (size_t)pitch * height;
    for (UINT y = 0; y < height / 2; y++)
        memset(uv + (size_t)y * pitch, kNeutralUV, width);

    const char* text = MessageFor(reason);

    // Keeps the text at roughly constant apparent size across resolutions:
    // a 6px cell at 1080p down to the 2px floor below 360p.
    int scale = (int)(height / 180);
    if (scale < 2)
        scale = 2;

    const int tw = TextWidth(text, scale);
    const int x0 = ((int)width - tw) / 2;
    const int y0 = ((int)height - 7 * scale) / 2;

    DrawText(scanline, pitch, width, height, x0, y0, text, scale);
}