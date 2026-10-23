#include "settings/native/native_page_skin_common.h"

#include "settings/native/native_page_help_widgets.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "window/candidate_skin_palette.h"
#include "window/floating_toolbar_skin.h"

#include "msimeui/AnimatedImage.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include <d2d1_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace native_settings
{
namespace skinpreview
{
namespace
{
ComPtr<IWICImagingFactory> WicFactory()
{
    static ComPtr<IWICImagingFactory> factory = [] {
        ComPtr<IWICImagingFactory> created;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(created.GetAddressOf()));
        return created;
    }();
    return factory;
}
} // namespace

bool SystemPrefersLight()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

bool ResolveSurfaceLight(const SettingsModel &model, const char *surfacePath)
{
    const std::string surface = model.GetString(surfacePath, "follow");
    if (surface == "light")
        return true;
    if (surface == "dark")
        return false;
    std::string mode = model.GetString("appearance.theme_mode", "dark");
    if (mode == "auto")
        mode = "system";
    if (mode == "system")
        return SystemPrefersLight();
    return mode == "light";
}

const std::vector<std::string> &BuiltinSkins()
{
    return CandidateSkinCatalog::BuiltInIds();
}

bool IsBuiltinSkin(const std::string &id)
{
    const auto &skins = BuiltinSkins();
    return std::find(skins.begin(), skins.end(), id) != skins.end();
}

const Json *FindExternalSkin(const SettingsModel &model, const std::string &id)
{
    const Json *skins = model.Find("appearance.external_candidate_skins");
    if (!skins || !skins->is_array())
        return nullptr;
    for (const auto &skin : *skins)
    {
        if (skin.is_object() && skin.value("id", std::string()) == id)
            return &skin;
    }
    return nullptr;
}

std::string SkinBase(const SettingsModel &model, const std::string &id)
{
    if (IsBuiltinSkin(id))
        return id;
    if (const Json *external = FindExternalSkin(model, id))
    {
        const std::string base = external->value("base", std::string("fluent"));
        return IsBuiltinSkin(base) || base == "canvas" ? base : "fluent";
    }
    return "fluent";
}

std::wstring SkinResourcePath(const SettingsModel &model, const std::string &id, const std::string &relative)
{
    std::filesystem::path root = std::filesystem::u8path(model.GetString("appearance.candidate_skin_directory"));
    if (root.empty())
        root = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    return (root / std::filesystem::u8path(id) / std::filesystem::u8path(relative)).lexically_normal().wstring();
}

CandidateFont ReadCandidateFont(const SettingsModel &model)
{
    CandidateFont font;
    const std::string primary = model.GetString("appearance.english_font_css_family");
    if (!primary.empty())
        font.primary = Widen(primary);
    if (const Json *fallbacks = model.Find("appearance.fallback_font_css_families"); fallbacks && fallbacks->is_array())
    {
        for (const auto &item : *fallbacks)
        {
            if (item.is_string() && !item.get<std::string>().empty())
                font.fallbacks.push_back(Widen(item.get<std::string>()));
        }
    }
    return font;
}

std::filesystem::path SkinsRoot(const SettingsModel &model)
{
    std::filesystem::path root = std::filesystem::u8path(model.GetString("appearance.candidate_skin_directory"));
    if (root.empty())
        root = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    return root;
}

namespace
{
// The external skin a preview applies, as the candidate window loads it: only when it declares
// the layout and theme; otherwise the window falls back to fluent and so does the preview.
std::optional<CandidateSkinCatalog::Package> LoadPreviewPackage(const std::filesystem::path &root,
                                                                const std::string &skinId, bool light)
{
    if (CandidateSkinCatalog::IsBuiltIn(skinId))
        return std::nullopt;
    // The previews show a skin in both layouts, so only the theme decides here.
    std::optional<CandidateSkinCatalog::Package> package = CandidateSkinCatalog::Load(root, skinId);
    if (package &&
        std::find(package->themes.begin(), package->themes.end(), light ? "light" : "dark") == package->themes.end())
        package.reset();
    return package;
}
} // namespace

CandidateLook ResolveCandidateLook(const SettingsModel &model, const std::string &skinId, bool light)
{
    // Mirrors CaretStateIndicator::RefreshPalette.
    const std::filesystem::path root = SkinsRoot(model);
    const std::optional<CandidateSkinCatalog::Package> package = LoadPreviewPackage(root, skinId, light);
    const CandidateSkinCatalog::Package baseLook =
        CandidateSkinCatalog::ResolveBuiltInLook(root, package ? package->base : skinId);
    const CandidateSkinCatalog::CandidateColors *packageColors =
        package ? &(light ? package->light : package->dark) : nullptr;
    const CandidateSkinPalette resolved = ResolveCandidateSkinPalette(baseLook, light, "auto", packageColors);
    const CandidateSkinPalette opaqueBase = ResolveCandidateSkinPalette(baseLook, light, "auto");
    const CandidateSkinPalette flat = FlattenCandidateSkinPaletteForGdi(resolved, opaqueBase.surface);
    CandidateLook look;
    look.bg = flat.surface;
    look.border = flat.border;
    look.text = flat.text;
    return look;
}

ToolbarLook ResolveToolbarLook(const SettingsModel &model, const std::string &skinId, bool light)
{
    // Mirrors FloatingToolbarPresenter::ApplyTheme: the base skin's [toolbar], then the external skin's.
    const std::filesystem::path root = SkinsRoot(model);
    const std::optional<CandidateSkinCatalog::Package> package = LoadPreviewPackage(root, skinId, light);
    FloatingToolbarSkin skin = ResolveFloatingToolbarSkin(
        CandidateSkinCatalog::ResolveBuiltInLook(root, package ? package->base : skinId), light);
    if (package)
        ApplyFloatingToolbarSkinOverrides(skin, *package, light);
    ToolbarLook t;
    t.bg = skin.fill;
    t.border = skin.border;
    t.handle = skin.handle;
    t.divider = skin.divider;
    t.icon = skin.glyph;
    t.radius = skin.radius;
    t.iconRadius = skin.iconRadius;
    // The toolbar's own shadow pair (8px 10px 24px, 2px 3px 8px), light α .18/.10, dark α .34/.22.
    t.shadows = {{8.0f, 10.0f, 24.0f, light ? 0.18f : 0.34f}, {2.0f, 3.0f, 8.0f, light ? 0.10f : 0.22f}};
    if (!package || package->toolbarMode != "image")
        return t;
    // The image mode; ToolbarPreview::Set places the buttons once it knows which ones show.
    t.imageMode = true;
    t.animated = package->schemaVersion >= 2;
    t.directory = root / std::filesystem::u8path(package->id);
    t.image = (t.directory / std::filesystem::u8path(package->toolbarImage)).lexically_normal().wstring();
    t.imageSize = {static_cast<float>(package->toolbarImageWidthDip),
                   static_cast<float>(package->toolbarImageHeightDip)};
    t.package = std::make_shared<const CandidateSkinCatalog::Package>(*package);
    return t;
}

namespace
{
// Premultiplied BGRA pixels of an image file, decoded off the window thread.
struct DecodedImage
{
    UINT width = 0, height = 0;
    std::vector<BYTE> pixels;
};

std::shared_ptr<DecodedImage> DecodeImageFile(IWICImagingFactory *wic, const std::wstring &path)
{
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    auto image = std::make_shared<DecodedImage>();
    // 读进内存再交给 D2D：直接用解码器建的 D2D 位图会一直占着皮肤图文件。
    if (!wic ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                              decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)) ||
        FAILED(converter->GetSize(&image->width, &image->height)) || image->width == 0 || image->height == 0 ||
        image->width > 16384 || image->height > 16384)
        return nullptr;
    image->pixels.resize(static_cast<size_t>(image->width) * image->height * 4);
    if (FAILED(converter->CopyPixels(nullptr, image->width * 4, static_cast<UINT>(image->pixels.size()),
                                     image->pixels.data())))
        return nullptr;
    return image;
}

struct BitmapEntry
{
    ID2D1RenderTarget *target = nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    // Pixels a worker decoded, waiting to become a bitmap on the next frame.
    std::shared_ptr<DecodedImage> decoded;
    bool pending = false;
    bool failed = false;
};

std::map<std::wstring, BitmapEntry> &BitmapCache()
{
    static std::map<std::wstring, BitmapEntry> cache;
    return cache;
}

HWND &ImageLoadWindow()
{
    static HWND hwnd = nullptr;
    return hwnd;
}

// Decodes on a worker and repaints the window when the pixels are in. A large or unusual
// image (Niya Demo's 1280x720 WebP background takes 100-200 ms the first time the codec
// loads) would otherwise freeze the page for that long when it scrolls into view.
void RequestDecode(const std::wstring &path, BitmapEntry &entry)
{
    if (entry.pending || entry.failed || entry.decoded)
        return;
    const HWND hwnd = ImageLoadWindow();
    if (!hwnd)
    {
        entry.decoded = DecodeImageFile(WicFactory().Get(), path);
        entry.failed = !entry.decoded;
        return;
    }
    entry.pending = true;
    auto result = std::make_shared<std::shared_ptr<DecodedImage>>();
    RunInBackground(
        hwnd,
        [path, result]() {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            {
                ComPtr<IWICImagingFactory> wic;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(wic.GetAddressOf()));
                *result = DecodeImageFile(wic.Get(), path);
            }
            if (SUCCEEDED(init))
                CoUninitialize();
            return std::string();
        },
        [path, result, hwnd](const std::string &) {
            BitmapEntry &done = BitmapCache()[path];
            done.pending = false;
            done.decoded = *result;
            done.failed = !done.decoded;
            if (done.decoded && IsWindow(hwnd))
                InvalidateRect(hwnd, nullptr, FALSE);
        });
}
} // namespace

void SetImageLoadWindow(HWND hwnd)
{
    ImageLoadWindow() = hwnd;
}

void PrefetchImage(const std::wstring &path)
{
    if (path.empty())
        return;
    BitmapEntry &entry = BitmapCache()[path];
    if (!entry.bitmap)
        RequestDecode(path, entry);
}

ID2D1Bitmap *CachedBitmap(msimeui::DeviceResources &dr, const std::wstring &path, D2D1_SIZE_F *size)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || path.empty())
        return nullptr;
    BitmapEntry &entry = BitmapCache()[path];
    if (entry.target != target)
    {
        // A new render target reads the file again, and retries one that failed.
        entry.target = target;
        entry.bitmap.Reset();
        entry.failed = false;
    }
    if (!entry.bitmap)
    {
        RequestDecode(path, entry);
        if (!entry.decoded)
            return nullptr;
        const DecodedImage &image = *entry.decoded;
        const D2D1_BITMAP_PROPERTIES properties =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target->CreateBitmap(D2D1::SizeU(image.width, image.height), image.pixels.data(), image.width * 4,
                                        properties, entry.bitmap.GetAddressOf())))
            entry.failed = true;
        entry.decoded.reset();
    }
    if (!entry.bitmap)
        return nullptr;
    if (size)
    {
        UINT w = 0, h = 0;
        D2D1_SIZE_U pixels = entry.bitmap->GetPixelSize();
        w = pixels.width;
        h = pixels.height;
        *size = D2D1::SizeF(static_cast<float>(w), static_cast<float>(h));
    }
    return entry.bitmap.Get();
}

ID2D1Bitmap *PreviewFrame(msimeui::DeviceResources &dr, const std::wstring &path, bool animated, D2D1_SIZE_F *size)
{
    // Only containers that can hold frames go through msimeui (which keeps just a few still
    // bitmaps); everything else, and every image with animations off, stays in our cache.
    static std::map<std::wstring, bool> mayAnimate;
    if (!animated || path.empty() || !dr.AreAnimationsEnabled())
        return CachedBitmap(dr, path, size);
    auto it = mayAnimate.find(path);
    if (it == mayAnimate.end())
    {
        ComPtr<IWICImagingFactory> wic = WicFactory();
        it = mayAnimate.emplace(path, wic && msimeui::AnimatedImage::MayBeAnimated(wic.Get(), path)).first;
    }
    if (!it->second)
        return CachedBitmap(dr, path, size);
    return dr.GetImageFrame(path, size);
}

SizeF ImageFileSize(const std::wstring &path)
{
    static std::map<std::wstring, SizeF> cache;
    auto it = cache.find(path);
    if (it != cache.end())
        return it->second;
    SizeF size{};
    ComPtr<IWICImagingFactory> wic = WicFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (wic &&
        SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())) && SUCCEEDED(frame->GetSize(&w, &h)))
        size = {static_cast<float>(w), static_cast<float>(h)};
    cache[path] = size;
    return size;
}

// ---------------------------------------------------------------------------------
// CandidatePreview
// ---------------------------------------------------------------------------------
namespace
{
const wchar_t *kSampleWords[] = {L"你们", L"你", L"尼", L"妮", L"泥", L"逆", L"拟", L"腻", L"拧"};
const wchar_t *kSampleCodes[] = {L"(rR)", L"(rX)", L"(uV)", L"(nV)", L"(dV)", L"(zQ)", L"(fR)", L"(oD)", L"(nG)"};
const wchar_t kSamplePreedit[] = L"ni'mf";
// Replaced scene roots kept alive (see CandidatePreview::retired_).
constexpr size_t kRetiredRoots = 8;
} // namespace

CandidatePreview::CandidatePreview() : scene_(std::make_unique<CandidateSkinScene>())
{
}

void CandidatePreview::Set(const SettingsModel &model, const std::string &skinId, bool light, const CandidateFont &font,
                           const Options &options)
{
    CandidateSkinScene::Settings settings;
    settings.skinsRoot = SkinsRoot(model);
    settings.skinId = skinId;
    settings.light = light;
    settings.horizontal = options.horizontal;
    settings.fontSize = font.size;
    settings.preeditFontSize = options.preeditFontSize > 0.0f ? options.preeditFontSize : font.size;
    settings.fontFamily = font.primary;
    settings.fallbackFontFamilies = font.fallbacks;
    settings.textColor = options.textColor;
    settings.preeditHidden = !options.preedit;
    settings.fitContent = options.fitContent;
    settings.pageSize = static_cast<int>(options.pageSize);
    // The page's stage around the preview already leaves room for the shadow.
    settings.framePadding = {};
    // No acrylic or mica in the settings window: such skins show their opaque surface.
    settings.backdropAvailable = false;
    const CandidateSkinScene::Result result = scene_->Apply(settings);
    if (result.rebuilt || child_ != scene_->Root())
    {
        if (child_)
        {
            retired_.push_back(child_);
            if (retired_.size() > kRetiredRoots)
                retired_.erase(retired_.begin());
        }
        SetChild(scene_->Root());
    }

    msimeui::TextBlock &preedit = *scene_->Preedit();
    if (options.preedit)
    {
        preedit.ClearHeight();
        preedit.SetText(kSamplePreedit);
        preedit.SetCaretIndex(std::size(kSamplePreedit) - 1);
    }
    else
    {
        preedit.SetText(L"");
        preedit.ClearCaret();
        preedit.SetHeight(0.0f);
    }
    std::vector<msimeui::CandidateList::Item> items;
    std::vector<CandidateSkinScene::PageWord> words;
    const size_t count = std::min<size_t>(options.pageSize, std::size(kSampleWords));
    for (size_t i = 0; i < count; ++i)
    {
        msimeui::CandidateList::Item item;
        item.label = std::to_wstring(i + 1);
        item.text = kSampleWords[i];
        if (options.helpcode)
            item.annotation = kSampleCodes[i];
        items.push_back(std::move(item));
        words.push_back({kSampleWords[i], L""});
    }
    scene_->List()->SetItems(std::move(items));
    scene_->List()->SetSelectedIndex(0);
    scene_->SetDecorationCandidates(words, 0);
    if (const auto &pager = scene_->Pager())
        pager->SetEnabled(false, true);
    InvalidateMeasure();
    InvalidateVisual();
}
} // namespace skinpreview
} // namespace native_settings
