// Profile icon helpers: icon validation, the stored file name Register.cpp accepts, and the WIC conversion the
// settings page runs on an imported image. The elevated half (HKLM, System32) is not exercised here.
#include "settings/profile_icon.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::filesystem::path TempFile(const wchar_t *name)
{
    wchar_t directory[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, directory);
    return std::filesystem::path(directory) / name;
}

// A width x height opaque red PNG.
bool WriteRedPng(IWICImagingFactory *factory, const std::filesystem::path &path, UINT width, UINT height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i + 0] = 0;   // B
        pixels[i + 1] = 0;   // G
        pixels[i + 2] = 255; // R
        pixels[i + 3] = 255; // A
    }
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                                     static_cast<UINT>(pixels.size()), pixels.data(),
                                                     bitmap.GetAddressOf())) &&
           SUCCEEDED(factory->CreateStream(stream.GetAddressOf())) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(encoder->Commit());
}

// Pixels (BGRA) of the frame whose edge is `edge`, decoded back from the .ico bytes.
bool DecodeFrame(IWICImagingFactory *factory, const std::vector<uint8_t> &icon, UINT edge, std::vector<uint8_t> &out)
{
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    UINT count = 0;
    if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(icon.data()), static_cast<DWORD>(icon.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                                decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrameCount(&count)))
        return false;
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0;
        UINT h = 0;
        if (FAILED(decoder->GetFrame(i, frame.GetAddressOf())) || FAILED(frame->GetSize(&w, &h)) || w != edge)
            continue;
        ComPtr<IWICFormatConverter> converter;
        out.assign(static_cast<size_t>(w) * h * 4, 0);
        return SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())) &&
               SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                               nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
               SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()));
    }
    return false;
}

void TestStoredFileName()
{
    const std::vector<uint8_t> a = {1, 2, 3};
    const std::vector<uint8_t> b = {1, 2, 4};
    const std::wstring name = profile_icon::StoredFileName(a);
    Check(name.rfind(L"MetasequoiaImeTip-", 0) == 0 && name.size() == 18 + 16 + 4, "stored name has the fixed shape");
    Check(name == profile_icon::StoredFileName(a), "stored name is stable");
    Check(name != profile_icon::StoredFileName(b), "stored name follows the content");
    Check(profile_icon::IsStoredFileName(name), "stored name is recognized");
    Check(!profile_icon::IsStoredFileName(L"MetasequoiaImeTip.ico"), "installed icon is not a stored name");
    Check(!profile_icon::IsStoredFileName(L"MetasequoiaImeTip-..\\..\\aaaaaaaa.ico"), "separators are rejected");
    Check(!profile_icon::IsStoredFileName(L"MetasequoiaImeTip-0123456789abcdeg.ico"), "non-hex digits are rejected");
}

void TestValidation()
{
    static const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> frame(png, png + sizeof(png));
    frame.resize(64, 0);
    const std::vector<uint8_t> icon = profile_icon::PackPngIcon({{16, frame}, {256, frame}});
    Check(profile_icon::IsValidIcon(icon), "packed icon is valid");
    Check(icon[6 + 16] == 0, "a 256 px entry is written as 0");

    std::vector<uint8_t> truncated = icon;
    truncated.resize(truncated.size() - 1);
    Check(!profile_icon::IsValidIcon(truncated), "a frame past the end is rejected");
    std::vector<uint8_t> wrongType = icon;
    wrongType[2] = 2; // cursor
    Check(!profile_icon::IsValidIcon(wrongType), "a cursor file is rejected");
    std::vector<uint8_t> garbage = icon;
    garbage[6 + 32] = 'X'; // first frame's PNG signature
    Check(!profile_icon::IsValidIcon(garbage), "a frame that is neither PNG nor DIB is rejected");
    Check(!profile_icon::IsValidIcon({}), "empty bytes are rejected");
}

void TestConversion(IWICImagingFactory *factory)
{
    // Tall source: every frame must come out square with transparent side bars and a red middle.
    const std::filesystem::path source = TempFile(L"msime-profile-icon-test.png");
    Check(WriteRedPng(factory, source, 300, 600), "test PNG written");
    std::vector<uint8_t> icon;
    std::string error;
    Check(profile_icon::ConvertToIcon(source, icon, error), "PNG converts");
    Check(profile_icon::IsValidIcon(icon), "converted icon is valid");
    Check(icon.size() > 6 && icon[4] == profile_icon::IconFrameSizes().size(), "converted icon has every frame");
    for (const int edge : {16, 32, 256})
    {
        std::vector<uint8_t> pixels;
        Check(DecodeFrame(factory, icon, static_cast<UINT>(edge), pixels), "frame decodes");
        if (pixels.empty())
            continue;
        const auto at = [&](int x, int y) { return &pixels[(static_cast<size_t>(y) * edge + x) * 4]; };
        Check(at(0, edge / 2)[3] == 0, "left bar is transparent");
        Check(at(edge - 1, edge / 2)[3] == 0, "right bar is transparent");
        const uint8_t *middle = at(edge / 2, edge / 2);
        Check(middle[3] == 255 && middle[2] > 240 && middle[1] < 16, "middle is opaque red");
    }

    // An .ico is taken as is.
    const std::filesystem::path ico = TempFile(L"msime-profile-icon-test.ico");
    {
        std::ofstream out(ico, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(icon.data()), static_cast<std::streamsize>(icon.size()));
    }
    std::vector<uint8_t> passthrough;
    Check(profile_icon::ConvertToIcon(ico, passthrough, error) && passthrough == icon, ".ico passes through");

    std::vector<uint8_t> none;
    Check(!profile_icon::ConvertToIcon(TempFile(L"msime-profile-icon-missing.png"), none, error) && !error.empty(),
          "missing file reports an error");
    std::filesystem::remove(source);
    std::filesystem::remove(ico);
}

void TestShippedIcons()
{
#ifdef MSIME_PROFILE_ICONS_DIR
    for (const profile_icon::Builtin &builtin : profile_icon::Builtins())
    {
        if (std::string(builtin.id) == profile_icon::kDefaultChoice)
            continue; // packaged from windows/image/msime.ico
        const std::filesystem::path path =
            std::filesystem::path(MSIME_PROFILE_ICONS_DIR) / (std::string(builtin.id) + ".ico");
        std::vector<uint8_t> bytes;
        std::string error;
        Check(profile_icon::ConvertToIcon(path, bytes, error),
              "every built-in icon ships in windows/image/profile-icons");
    }
#endif
}
} // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        std::fprintf(stderr, "FAIL: WIC unavailable\n");
        return 1;
    }
    TestStoredFileName();
    TestValidation();
    TestConversion(factory.Get());
    TestShippedIcons();
    factory.Reset();
    CoUninitialize();
    if (g_failures == 0)
        std::printf("profile_icon: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
