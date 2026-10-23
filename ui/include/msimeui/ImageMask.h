#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace msimeui
{
// The pixels of a 32-bit BGRA bitmap whose alpha is above `threshold`, as rectangles: runs of
// such pixels in a row, with identical runs in consecutive rows merged into one taller rectangle.
// Only the alpha byte is read, so premultiplied and straight alpha give the same result.
// Rectangles come out top to bottom and left to right, the order a region needs.
std::vector<RECT> AlphaMaskRects(const BYTE *pixels, UINT width, UINT height, UINT stride, BYTE threshold);

// The image at `filePath` scaled to width x height pixels, as a region of its pixels with alpha
// above `threshold`. Animated images (APNG, GIF) count a pixel when any frame covers it, since a
// window region clips drawing as well as clicks. Suits SetWindowRgn for a window shaped like the
// image. Null when the image cannot be read or has no such pixel; the caller owns the region.
HRGN CreateAlphaMaskRegion(const std::wstring &filePath, UINT width, UINT height, BYTE threshold = 0);
} // namespace msimeui
