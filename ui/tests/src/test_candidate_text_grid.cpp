#include "tests/includes/test_framework.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
class GridHiddenWindow
{
  public:
    GridHiddenWindow()
    {
        REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
        hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"Candidate grid test", WS_POPUP, 0, 0, 64, 64,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        REQUIRE(hwnd != nullptr);
    }

    ~GridHiddenWindow()
    {
        DestroyWindow(hwnd);
        CoUninitialize();
    }

    HWND hwnd = nullptr;
};

struct Pixels
{
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> bgra;

    int Gray(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
            return 0;
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return (bgra[i] + bgra[i + 1] + bgra[i + 2]) / 3;
    }
};

template <typename Draw> Pixels RenderTo(ID2D1DeviceContext *dc, UINT width, UINT height, float dpi, Draw &&draw)
{
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID2D1Bitmap1> readback;
    REQUIRE(SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                                       D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, dpi, dpi),
                                       target.GetAddressOf())));
    REQUIRE(SUCCEEDED(dc->CreateBitmap(
        D2D1::SizeU(width, height), nullptr, 0,
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, dpi, dpi),
        readback.GetAddressOf())));
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(previous.GetAddressOf());
    dc->SetTarget(target.Get());
    dc->SetDpi(dpi, dpi);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x202020));
    draw();
    REQUIRE(SUCCEEDED(dc->EndDraw()));
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->SetTarget(previous.Get());
    REQUIRE(SUCCEEDED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)));
    D2D1_MAPPED_RECT mapped{};
    REQUIRE(SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)));
    Pixels pixels{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
    for (UINT row = 0; row < height; ++row)
    {
        std::copy_n(mapped.bits + static_cast<size_t>(row) * mapped.pitch, width * 4,
                    pixels.bgra.begin() + static_cast<size_t>(row) * width * 4);
    }
    readback->Unmap();
    return pixels;
}

// Ink rows/columns of a device-pixel box, used to find a glyph regardless of where layout put it.
struct Ink
{
    int left = 0;
    int top = 0;
    bool found = false;
};

Ink FindInk(const Pixels &p, int x0, int y0, int x1, int y1)
{
    Ink ink{x1, y1, false};
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (p.Gray(x, y) > 0x60)
            {
                ink.left = (std::min)(ink.left, x);
                ink.top = (std::min)(ink.top, y);
                ink.found = true;
            }
    return ink;
}
} // namespace

// At 125% a CJK glyph is 22.5 px wide, so a candidate that grows by one character moves every later
// candidate by half a pixel; and the composition atlas hands the list a translation that is a whole
// number of pixels expressed in DIPs (x 0.8), which only round-trips approximately. Neither may change
// how a later candidate's text is rasterized: the same text must come out as the same pixels.
TEST_CASE(candidate_text_rasterizes_identically_wherever_it_lands)
{
    GridHiddenWindow window;
    msimeui::DeviceResources resources;
    REQUIRE(resources.EnsureForComposition(window.hwnd));
    ID2D1DeviceContext *dc = resources.GetDeviceContext();
    REQUIRE(dc != nullptr);

    constexpr float kDpi = 120.0f;
    constexpr float kScale = kDpi / 96.0f;
    msimeui::CandidateList::Appearance appearance;
    appearance.fontFamily = L"Segoe UI";
    appearance.fallbackFontFamilies = {L"Noto Sans SC", L"Microsoft YaHei"};
    appearance.fontSize = 18.0f;
    appearance.labelFontSize = 18.0f * 0.72f;
    appearance.annotationFontSize = 18.0f;
    appearance.itemHeight = 18.0f * 1.35f + 6.0f;
    appearance.contentPadBottom = 3.0f;
    appearance.itemGap = 4.0f;
    appearance.labelGap = 3.0f;

    // The probe candidate's text, drawn after a first candidate of varying length, in a list that
    // wraps it to a second row on some lengths.
    const std::wstring probe = L"\u6211\u6211\u6211\u6211";
    std::vector<Pixels> glyphs;
    int worst = 0;
    for (const float availableWidth : {2000.0f, 700.0f})
    {
        for (int firstLength = 6; firstLength <= 13; ++firstLength)
        {
            for (const int atlasOffset : {0, 31, 62, 93, 117})
            {
                msimeui::CandidateList list(appearance.itemHeight);
                list.SetAppearance(appearance);
                list.SetOrientation(msimeui::CandidateList::Orientation::Horizontal);
                std::vector<msimeui::CandidateList::Item> items;
                items.push_back({L"1", std::wstring(static_cast<size_t>(firstLength), L'\u6211'), {}, {}, {}});
                items.push_back({L"2", L"\u543e\u95ee\u65e0\u4e3a\u8c13", {}, {}, {}});
                items.push_back({L"3", probe, {}, {}, {}});
                list.SetItems(std::move(items));
                const auto size = list.Measure({availableWidth, 10000.0f});
                const msimeui::RectF bounds{8.0f, 8.0f, size.width, size.height};
                list.Arrange(bounds);
                const auto probeBounds = list.GetItemBounds(2);

                // Same as DeviceResources::BeginDraw: a whole-pixel atlas offset converted to DIPs.
                const float origin = static_cast<float>(atlasOffset) * (96.0f / kDpi);
                const UINT width = static_cast<UINT>(std::ceil((bounds.width + 16.0f) * kScale)) + atlasOffset + 4;
                const UINT height = static_cast<UINT>(std::ceil((bounds.height + 16.0f) * kScale)) + atlasOffset + 4;
                const Pixels pixels = RenderTo(dc, width, height, kDpi, [&]() {
                    dc->SetTransform(D2D1::Matrix3x2F::Translation(origin, origin));
                    list.Render(resources);
                });

                // Box around the probe's text (skip the label at its start), in device pixels.
                const int x0 = static_cast<int>(std::floor((probeBounds.x + 22.0f) * kScale)) + atlasOffset;
                const int y0 = static_cast<int>(std::floor(probeBounds.y * kScale)) + atlasOffset;
                const int x1 = static_cast<int>(std::ceil((probeBounds.x + probeBounds.width) * kScale)) + atlasOffset;
                const int y1 = static_cast<int>(std::ceil((probeBounds.y + probeBounds.height) * kScale)) + atlasOffset;
                const Ink ink = FindInk(pixels, x0, y0, x1, y1);
                REQUIRE(ink.found);
                Pixels glyph{30, 34, std::vector<uint8_t>(30 * 34 * 4)};
                for (int y = 0; y < 34; ++y)
                    for (int x = 0; x < 30; ++x)
                    {
                        const int g = pixels.Gray(ink.left - 2 + x, ink.top - 4 + y);
                        const size_t i = (static_cast<size_t>(y) * 30 + x) * 4;
                        glyph.bgra[i] = glyph.bgra[i + 1] = glyph.bgra[i + 2] = static_cast<uint8_t>(g);
                    }
                // The ink box of a CJK glyph also starts on an anti-aliased fringe; comparing whole
                // crops anchored on it catches both a shift and a different rasterization.
                if (!glyphs.empty())
                {
                    int diff = 0;
                    for (size_t i = 0; i < glyph.bgra.size(); ++i)
                        diff = (std::max)(
                            diff, std::abs(static_cast<int>(glyph.bgra[i]) - static_cast<int>(glyphs.front().bgra[i])));
                    if (diff > 0)
                    {
                        std::printf("probe differs: width=%.0f first=%d offset=%d probe=(%.2f,%.2f) max=%d\n",
                                    availableWidth, firstLength, atlasOffset, probeBounds.x, probeBounds.y, diff);
                    }
                    worst = (std::max)(worst, diff);
                }
                glyphs.push_back(std::move(glyph));
            }
        }
    }
    std::printf("candidate probe worst channel difference %d/255 over %zu renders\n", worst, glyphs.size());
    REQUIRE(worst == 0);
}
