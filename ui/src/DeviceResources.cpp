#include "msimeui/DeviceResources.h"

#include "msimeui/Compat.h"
#include "msimeui/Fonts.h"

#include <algorithm>
#include <cmath>
#include <d2d1_3.h>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <shlwapi.h>
#include <windows.h>

#ifndef MSIME_WIN7_LITE
#include <DispatcherQueue.h>
#include <d2d1effects.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <roapi.h>
#include <windows.graphics.effects.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.desktop.h>
#include <windows.ui.composition.h>
#include <windows.ui.composition.interop.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>
#endif

namespace msimeui
{
#ifndef MSIME_WIN7_LITE
namespace abi_comp = ABI::Windows::UI::Composition;

struct DeviceResources::SurfaceComposition
{
    Microsoft::WRL::ComPtr<abi_comp::ICompositor> compositor;
    Microsoft::WRL::ComPtr<abi_comp::Desktop::IDesktopWindowTarget> target;
    Microsoft::WRL::ComPtr<abi_comp::IVisual> backdrop;
    Microsoft::WRL::ComPtr<abi_comp::ISpriteVisual> backdropSprite;
    // Blurred sources for BackdropMaterial::Source; `wallpaper` is null where Windows lacks it.
    Microsoft::WRL::ComPtr<abi_comp::ICompositionBrush> windowsBehind;
    Microsoft::WRL::ComPtr<abi_comp::ICompositionBrush> wallpaper;
    Microsoft::WRL::ComPtr<abi_comp::ICompositionRoundedRectangleGeometry> backdropShape;
    Microsoft::WRL::ComPtr<abi_comp::IVisual> content;
    Microsoft::WRL::ComPtr<abi_comp::ICompositionGraphicsDevice> graphicsDevice;
    Microsoft::WRL::ComPtr<abi_comp::ICompositionDrawingSurfaceInterop> surface;
};
#else
// No Windows Runtime in the Windows 7 build; the member stays empty so the
// unique_ptr destructor keeps a complete type.
struct DeviceResources::SurfaceComposition
{
};
#endif

namespace
{
#ifndef MSIME_WIN7_LITE
// DWMWA_USE_HOSTBACKDROPBRUSH; older SDKs do not name it.
constexpr DWORD kDwmUseHostBackdropBrush = 17;

// Windows.UI.Composition needs a DispatcherQueue on the thread that owns the compositor,
// and a thread can only have one. The controller lives as long as the thread, so it is
// deliberately never released.
bool EnsureThreadDispatcherQueue()
{
    thread_local bool attempted = false;
    thread_local bool available = false;
    if (attempted)
    {
        return available;
    }
    attempted = true;
    // COM is already initialized on UI threads (WIC needs it); don't touch the apartment.
    const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    ABI::Windows::System::IDispatcherQueueController *controller = nullptr;
    const HRESULT hr = CreateDispatcherQueueController(options, &controller);
    // Failing because the thread already has a queue is fine: the compositor uses that one.
    available = SUCCEEDED(hr) || hr == RPC_E_WRONG_THREAD || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    return available;
}
#endif // !MSIME_WIN7_LITE

bool IsDeviceLost(HRESULT hr)
{
    return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

#ifndef MSIME_WIN7_LITE
namespace abi_fx = ABI::Windows::Graphics::Effects;
using PropertyValuePtr = Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IPropertyValue>;
using EffectSourcePtr = Microsoft::WRL::ComPtr<abi_fx::IGraphicsEffectSource>;

// A Direct2D effect described for Windows.UI.Composition, which compiles the graph into an
// effect brush. Composition reads the effect id, the properties by index and the sources;
// nothing is animated, so there are no named property mappings.
class CompositionEffect final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
                                          abi_fx::IGraphicsEffect, abi_fx::IGraphicsEffectSource,
                                          abi_fx::IGraphicsEffectD2D1Interop>
{
    InspectableClass(L"msimeui.CompositionEffect", BaseTrust)

        public : CompositionEffect(const GUID &id, std::vector<PropertyValuePtr> properties,
                                   std::vector<EffectSourcePtr> sources)
        : id_(id), properties_(std::move(properties)), sources_(std::move(sources))
    {
    }

    IFACEMETHODIMP get_Name(HSTRING *name) override
    {
        return name_.CopyTo(name);
    }
    IFACEMETHODIMP put_Name(HSTRING name) override
    {
        return name_.Set(name);
    }
    IFACEMETHODIMP GetEffectId(GUID *id) override
    {
        *id = id_;
        return S_OK;
    }
    IFACEMETHODIMP GetNamedPropertyMapping(LPCWSTR, UINT *, abi_fx::GRAPHICS_EFFECT_PROPERTY_MAPPING *) override
    {
        return E_INVALIDARG;
    }
    IFACEMETHODIMP GetPropertyCount(UINT *count) override
    {
        *count = static_cast<UINT>(properties_.size());
        return S_OK;
    }
    IFACEMETHODIMP GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue **value) override
    {
        return index < properties_.size() ? properties_[index].CopyTo(value) : E_BOUNDS;
    }
    IFACEMETHODIMP GetSource(UINT index, abi_fx::IGraphicsEffectSource **source) override
    {
        return index < sources_.size() ? sources_[index].CopyTo(source) : E_BOUNDS;
    }
    IFACEMETHODIMP GetSourceCount(UINT *count) override
    {
        *count = static_cast<UINT>(sources_.size());
        return S_OK;
    }

  private:
    GUID id_;
    Microsoft::WRL::Wrappers::HString name_;
    std::vector<PropertyValuePtr> properties_;
    std::vector<EffectSourcePtr> sources_;
};
#endif // !MSIME_WIN7_LITE

UINT AlignSwapExtent(UINT value)
{
    constexpr UINT kBucket = 64;
    value = (std::max)(value, 1U);
    return ((value + kBucket - 1U) / kBucket) * kBucket;
}

uint64_t QuantizeChannel(float value)
{
    return static_cast<uint64_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
}

RectF IntersectRect(const RectF &lhs, const RectF &rhs)
{
    const float left = (std::max)(lhs.x, rhs.x);
    const float top = (std::max)(lhs.y, rhs.y);
    const float right = (std::min)(lhs.x + lhs.width, rhs.x + rhs.width);
    const float bottom = (std::min)(lhs.y + lhs.height, rhs.y + rhs.height);
    return {left, top, (std::max)(right - left, 0.0f), (std::max)(bottom - top, 0.0f)};
}

Microsoft::WRL::ComPtr<ID3D11Device> CreateD3DDevice()
{
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 device.GetAddressOf(), nullptr, nullptr)))
    {
        device.Reset();
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                     device.GetAddressOf(), nullptr, nullptr)))
        {
            device.Reset();
        }
    }
    return device;
}

// A Direct3D 11 device is free-threaded, so one created on the prewarm thread can be
// used by the window thread.
std::mutex g_prewarmMutex;
std::future<Microsoft::WRL::ComPtr<ID3D11Device>> g_prewarmedDevice;

Microsoft::WRL::ComPtr<ID3D11Device> TakePrewarmedDevice()
{
    std::future<Microsoft::WRL::ComPtr<ID3D11Device>> pending;
    {
        std::lock_guard lock(g_prewarmMutex);
        pending = std::move(g_prewarmedDevice);
    }
    return pending.valid() ? pending.get() : nullptr;
}

// Every window a thread draws shares that thread's Direct3D device. Each hardware device costs
// about 10 MB of private memory in the driver, and the windows of one thread never touch its
// immediate context concurrently. Each window still has its own Direct2D device and context.
// A removed device is replaced here; the other windows find out from their own next draw,
// discard their target and come back for the replacement.
Microsoft::WRL::ComPtr<ID3D11Device> AcquireThreadDevice()
{
    thread_local Microsoft::WRL::ComPtr<ID3D11Device> shared;
    if (shared && shared->GetDeviceRemovedReason() == S_OK)
    {
        return shared;
    }
    shared = TakePrewarmedDevice();
    if (!shared)
    {
        shared = CreateD3DDevice();
    }
    return shared;
}
} // namespace

DeviceResources::DeviceResources() = default;

DeviceResources::~DeviceResources()
{
    // WinRT visuals before the Direct3D device they draw with.
    drawContext_.Reset();
#ifndef MSIME_WIN7_LITE
    surface_.reset();
#endif
}

void DeviceResources::PrewarmComposition()
{
    std::lock_guard lock(g_prewarmMutex);
    if (!g_prewarmedDevice.valid())
    {
        g_prewarmedDevice = std::async(std::launch::async, &CreateD3DDevice);
    }
}

uint64_t DeviceResources::ColorKey(const D2D1_COLOR_F &color)
{
    return (QuantizeChannel(color.r) << 48) | (QuantizeChannel(color.g) << 32) | (QuantizeChannel(color.b) << 16) |
           QuantizeChannel(color.a);
}

void DeviceResources::BeginFrame()
{
    ++frame_;
    clipStack_.clear();
    TrimBrushCache();
    frameTime_ = AnimationClock::now();
    nextAnimationDeadline_.reset();
}

void DeviceResources::SetAnimationsEnabled(bool enabled)
{
    animationsEnabled_ = enabled;
    if (!enabled)
    {
        nextAnimationDeadline_.reset();
    }
}

bool DeviceResources::AreAnimationsEnabled() const
{
    return animationsEnabled_;
}

void DeviceResources::SetAnimationEpoch(AnimationClock::time_point epoch)
{
    animationEpoch_ = epoch;
}

void DeviceResources::SetFrameTime(AnimationClock::time_point now)
{
    frameTime_ = now;
}

std::optional<DeviceResources::AnimationClock::time_point> DeviceResources::GetNextAnimationDeadline() const
{
    return nextAnimationDeadline_;
}

void DeviceResources::ClearImageCaches()
{
    bitmapCache_.clear();
    animatedCache_.clear();
    animatedBytes_ = 0;
    stillImagePaths_.clear();
}

void DeviceResources::SetAnimatedBitmapBudget(size_t bytes)
{
    animatedBudget_ = bytes;
    TrimAnimatedBitmaps(0);
}

size_t DeviceResources::GetAnimatedBitmapBytes() const
{
    return animatedBytes_;
}

void DeviceResources::RememberStillImage(const std::wstring &filePath)
{
    if (std::find(stillImagePaths_.begin(), stillImagePaths_.end(), filePath) != stillImagePaths_.end())
    {
        return;
    }
    if (stillImagePaths_.size() >= kMaxRememberedStillImages)
    {
        stillImagePaths_.erase(stillImagePaths_.begin());
    }
    stillImagePaths_.push_back(filePath);
}

void DeviceResources::TrimAnimatedBitmaps(size_t incoming)
{
    // Least recently drawn first. Bitmaps already handed to the render target this frame stay
    // alive inside Direct2D until the batch is flushed, so dropping our reference is safe.
    while (!animatedCache_.empty() && animatedBytes_ + incoming > animatedBudget_)
    {
        auto oldest = std::min_element(
            animatedCache_.begin(), animatedCache_.end(),
            [](const AnimatedBitmapEntry &a, const AnimatedBitmapEntry &b) { return a.lastFrame < b.lastFrame; });
        animatedBytes_ -= oldest->bytes;
        animatedCache_.erase(oldest);
    }
}

DeviceResources::AnimatedBitmapEntry *DeviceResources::FindOrLoadAnimation(const std::wstring &filePath)
{
    for (auto &entry : animatedCache_)
    {
        if (entry.filePath == filePath)
        {
            return &entry;
        }
    }
    if (std::find(stillImagePaths_.begin(), stillImagePaths_.end(), filePath) != stillImagePaths_.end())
    {
        return nullptr;
    }
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target || !wicFactory_)
    {
        return nullptr;
    }
    // A cheap look at the container first: still images never pay for a full decode here.
    if (!AnimatedImage::MayBeAnimated(wicFactory_.Get(), filePath))
    {
        RememberStillImage(filePath);
        return nullptr;
    }
    std::optional<AnimatedImage> image = AnimatedImage::DecodeFile(wicFactory_.Get(), filePath);
    // Malformed animations decode to their still frame; an animation larger than the whole
    // budget is shown still rather than evicting everything else.
    if (!image || !image->IsAnimated() || image->DecodedBytes() > animatedBudget_)
    {
        RememberStillImage(filePath);
        return nullptr;
    }
    AnimatedBitmapEntry entry;
    entry.filePath = filePath;
    entry.timeline = image->Timeline();
    entry.bytes = static_cast<size_t>(image->DecodedBytes());
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    for (size_t index = 0; index < image->FrameCount(); ++index)
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(target->CreateBitmap(D2D1::SizeU(image->Width(), image->Height()), image->FramePixels(index).data(),
                                        image->Width() * 4, &properties, bitmap.GetAddressOf())))
        {
            return nullptr;
        }
        entry.frames.push_back(std::move(bitmap));
    }
    entry.size = D2D1::SizeF(static_cast<float>(image->Width()), static_cast<float>(image->Height()));
    TrimAnimatedBitmaps(entry.bytes);
    animatedBytes_ += entry.bytes;
    animatedCache_.push_back(std::move(entry));
    return &animatedCache_.back();
}

ID2D1Bitmap *DeviceResources::GetImageFrame(const std::wstring &filePath, D2D1_SIZE_F *size)
{
    if (filePath.empty() || !GetRenderTarget())
    {
        return nullptr;
    }
    // Without animations an image is exactly what a still decoder shows: the APNG default image,
    // the first GIF frame. Nothing animated is decoded or kept on the GPU for it.
    if (!animationsEnabled_)
    {
        return GetBitmapFromFile(filePath, size);
    }
    AnimatedBitmapEntry *entry = FindOrLoadAnimation(filePath);
    if (!entry)
    {
        return GetBitmapFromFile(filePath, size);
    }
    entry->lastFrame = frame_;
    if (size)
    {
        *size = entry->size;
    }
    const AnimationClock::time_point now = frameTime_.value_or(AnimationClock::now());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - animationEpoch_).count();
    const AnimationTimeline::Sample sample =
        entry->timeline.SampleAt(static_cast<uint64_t>((std::max)(elapsed, static_cast<decltype(elapsed)>(0))));
    if (sample.nextChangeMs)
    {
        const AnimationClock::time_point deadline =
            animationEpoch_ + std::chrono::milliseconds(static_cast<int64_t>(*sample.nextChangeMs));
        if (!nextAnimationDeadline_ || deadline < *nextAnimationDeadline_)
        {
            nextAnimationDeadline_ = deadline;
        }
    }
    return entry->frames[(std::min)(sample.frameIndex, entry->frames.size() - 1)].Get();
}

void DeviceResources::TrimBrushCache()
{
    if (brushCache_.size() <= kBrushCacheSoftLimit)
    {
        return;
    }
    // Only brushes untouched in the current frame go: a caller may still hold one it
    // fetched a moment ago.
    for (auto it = brushCache_.begin(); it != brushCache_.end();)
    {
        it = it->second.lastFrame + 1 < frame_ ? brushCache_.erase(it) : std::next(it);
    }
}

void DeviceResources::PushClip(const RectF &rect, D2D1_ANTIALIAS_MODE mode)
{
    if (ID2D1RenderTarget *target = GetRenderTarget())
    {
        target->PushAxisAlignedClip(D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), mode);
    }
    clipStack_.push_back(clipStack_.empty() ? rect : IntersectRect(clipStack_.back(), rect));
}

void DeviceResources::PopClip()
{
    if (clipStack_.empty())
    {
        return;
    }
    clipStack_.pop_back();
    if (ID2D1RenderTarget *target = GetRenderTarget())
    {
        target->PopAxisAlignedClip();
    }
}

bool DeviceResources::IsVisible(const RectF &rect) const
{
    if (clipStack_.empty())
    {
        return true;
    }
    const RectF &clip = clipStack_.back();
    return rect.x < clip.x + clip.width && rect.x + rect.width > clip.x && rect.y < clip.y + clip.height &&
           rect.y + rect.height > clip.y;
}

FLOAT DeviceResources::DpiForHwnd() const
{
    // The override wins over the system window DPI: window sizing and
    // rendering must share one scale even when GetDpiForWindow disagrees
    // with the content's real scale (RDP client-scaling sync).
    if (dpiOverride_ > 0.0f)
    {
        return dpiOverride_;
    }
    if (!hwnd_)
    {
        return 96.0f;
    }
    const UINT dpi = compat::GetWindowDpi(hwnd_);
    return dpi > 0 ? static_cast<FLOAT>(dpi) : 96.0f;
}

void DeviceResources::SetDpiOverride(FLOAT dpi)
{
    const FLOAT normalized = dpi > 0.0f ? dpi : 0.0f;
    if (normalized == dpiOverride_)
    {
        return;
    }
    dpiOverride_ = normalized;
    // EnsureForComposition early-returns when the existing swap chain already
    // covers the requested size, so a live target would keep the stale DPI.
    // Push the new DPI into live targets right away; freshly created targets
    // read it through DpiForHwnd() instead.
    const FLOAT effective = DpiForHwnd();
    if (hwndRenderTarget_)
    {
        hwndRenderTarget_->SetDpi(effective, effective);
    }
    if (deviceContext_)
    {
        deviceContext_->SetDpi(effective, effective);
    }
}

bool DeviceResources::EnsureFactories()
{
    if (!d2dFactory_)
    {
        Microsoft::WRL::ComPtr<ID2D1Factory1> factory1;
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory1.GetAddressOf())))
        {
            d2dFactory1_ = factory1;
            d2dFactory_ = factory1;
        }
        else if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf())))
        {
            return false;
        }
    }

    if (!dwriteFactory_)
    {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown **>(dwriteFactory_.GetAddressOf()))))
        {
            return false;
        }
    }

    if (!wicFactory_)
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(wicFactory_.GetAddressOf()))))
        {
            return false;
        }
    }
    return true;
}

bool DeviceResources::EnsureForWindow(HWND hwnd)
{
    hwnd_ = hwnd;
    composition_ = false;
    if (!EnsureFactories())
    {
        return false;
    }
    if (hwndRenderTarget_)
    {
        const FLOAT dpi = DpiForHwnd();
        hwndRenderTarget_->SetDpi(dpi, dpi);
        return true;
    }

    DiscardTarget();
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    pixelWidth_ = static_cast<UINT>((std::max)(rc.right, 1L));
    pixelHeight_ = static_cast<UINT>((std::max)(rc.bottom, 1L));
    const auto size = D2D1::SizeU(pixelWidth_, pixelHeight_);
    if (FAILED(d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                   D2D1::HwndRenderTargetProperties(hwnd, size),
                                                   hwndRenderTarget_.GetAddressOf())))
    {
        return false;
    }

    const FLOAT dpi = DpiForHwnd();
    hwndRenderTarget_->SetDpi(dpi, dpi);
    return true;
}

bool DeviceResources::BindCompositionSurface()
{
    if (!swapChain_ || !deviceContext_)
    {
        return false;
    }

    deviceContext_->SetTarget(nullptr);
    dxgiBitmap_.Reset();

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()))))
    {
        return false;
    }

    const FLOAT dpi = DpiForHwnd();
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          compositionOpaque_ ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi);
    if (FAILED(deviceContext_->CreateBitmapFromDxgiSurface(surface.Get(), &props, dxgiBitmap_.GetAddressOf())))
    {
        return false;
    }
    deviceContext_->SetTarget(dxgiBitmap_.Get());
    deviceContext_->SetDpi(dpi, dpi);
    return true;
}

void DeviceResources::SetCompositionOpaque(bool opaque)
{
    if (compositionOpaque_ == opaque)
    {
        return;
    }
    compositionOpaque_ = opaque;
    if (composition_)
    {
        DiscardTarget();
    }
}

bool DeviceResources::CreateCompositionDevice()
{
    d3dDevice_ = AcquireThreadDevice();
    return d3dDevice_ && SUCCEEDED(d3dDevice_.As(&dxgiDevice_)) &&
           SUCCEEDED(d2dFactory1_->CreateDevice(dxgiDevice_.Get(), d2dDevice_.GetAddressOf())) &&
           SUCCEEDED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, deviceContext_.GetAddressOf()));
}

void DeviceResources::SetBackdropComposition(bool enabled)
{
    if (backdropComposition_ == enabled)
    {
        return;
    }
    backdropComposition_ = enabled;
    if (composition_)
    {
        DiscardTarget();
    }
}

bool DeviceResources::UsesBackdropComposition() const
{
    return backdropComposition_;
}

#ifndef MSIME_WIN7_LITE
bool DeviceResources::EnsureSurfaceComposition(UINT width, UINT height)
{
    namespace abi_dx = ABI::Windows::Graphics::DirectX;
    using Microsoft::WRL::ComPtr;

    if (composition_ && surface_ && deviceContext_)
    {
        const FLOAT dpi = DpiForHwnd();
        deviceContext_->SetDpi(dpi, dpi);
        return GrowDrawingSurface(width, height);
    }

    DiscardTarget();
    composition_ = true;
    if (!CreateCompositionDevice() || !EnsureThreadDispatcherQueue())
    {
        DiscardTarget();
        return false;
    }

    auto composition = std::make_unique<SurfaceComposition>();
    ComPtr<IInspectable> instance;
    ComPtr<abi_comp::ICompositor3> compositor3;
    ComPtr<abi_comp::ICompositor5> compositor5;
    ComPtr<abi_comp::ICompositor6> compositor6;
    ComPtr<abi_comp::Desktop::ICompositorDesktopInterop> desktopInterop;
    ComPtr<abi_comp::ICompositorInterop> interop;
    if (FAILED(::Windows::Foundation::ActivateInstance(
            Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Composition_Compositor).Get(),
            instance.GetAddressOf())) ||
        FAILED(instance.As(&composition->compositor)) || FAILED(instance.As(&compositor3)) ||
        FAILED(instance.As(&compositor5)) || FAILED(instance.As(&compositor6)) ||
        FAILED(instance.As(&desktopInterop)) || FAILED(instance.As(&interop)))
    {
        DiscardTarget();
        return false;
    }

    // The DirectComposition target this window may have had is gone with DiscardTarget above,
    // which leaves the topmost slot free; fall back to the other slot if it is not free yet.
    if (FAILED(desktopInterop->CreateDesktopWindowTarget(hwnd_, TRUE, composition->target.GetAddressOf())) &&
        FAILED(desktopInterop->CreateDesktopWindowTarget(hwnd_, FALSE, composition->target.GetAddressOf())))
    {
        DiscardTarget();
        return false;
    }

    ComPtr<abi_comp::IContainerVisual> root;
    ComPtr<abi_comp::IVisual> rootVisual;
    ComPtr<abi_comp::ICompositionTarget> target;
    ComPtr<abi_comp::IVisualCollection> children;
    ComPtr<abi_comp::ICompositionBackdropBrush> backdropBrush;
    ComPtr<abi_comp::ICompositionGeometry> shapeGeometry;
    ComPtr<abi_comp::ICompositionGeometricClip> geometricClip;
    ComPtr<abi_comp::ICompositionClip> clip;
    ComPtr<abi_comp::ICompositionDrawingSurface> drawingSurface;
    ComPtr<abi_comp::ICompositionSurface> surface;
    ComPtr<abi_comp::ICompositionSurfaceBrush> surfaceBrush;
    ComPtr<abi_comp::ICompositionBrush> surfaceBrushBase;
    ComPtr<abi_comp::ISpriteVisual> contentSprite;
    abi_comp::ICompositor *compositor = composition->compositor.Get();
    const ABI::Windows::Foundation::Size sizePixels{static_cast<FLOAT>(width), static_cast<FLOAT>(height)};
    if (FAILED(compositor->CreateContainerVisual(root.GetAddressOf())) || FAILED(root.As(&rootVisual)) ||
        FAILED(composition->target.As(&target)) || FAILED(target->put_Root(rootVisual.Get())) ||
        FAILED(root->get_Children(children.GetAddressOf())) ||
        // Backdrop: the blurred desktop behind the window, clipped to a rounded rectangle.
        FAILED(compositor3->CreateHostBackdropBrush(backdropBrush.GetAddressOf())) ||
        FAILED(backdropBrush.As(&composition->windowsBehind)) ||
        FAILED(compositor->CreateSpriteVisual(composition->backdropSprite.GetAddressOf())) ||
        FAILED(composition->backdropSprite.As(&composition->backdrop)) ||
        FAILED(compositor5->CreateRoundedRectangleGeometry(composition->backdropShape.GetAddressOf())) ||
        FAILED(composition->backdropShape.As(&shapeGeometry)) ||
        FAILED(compositor6->CreateGeometricClipWithGeometry(shapeGeometry.Get(), geometricClip.GetAddressOf())) ||
        FAILED(geometricClip.As(&clip)) || FAILED(composition->backdrop->put_Clip(clip.Get())) ||
        FAILED(composition->backdrop->put_IsVisible(false)) ||
        FAILED(children->InsertAtBottom(composition->backdrop.Get())) ||
        // Content: Direct2D draws into a drawing surface shown at its pixel size, top-left aligned.
        FAILED(interop->CreateGraphicsDevice(d2dDevice_.Get(), composition->graphicsDevice.GetAddressOf())) ||
        FAILED(composition->graphicsDevice->CreateDrawingSurface(
            sizePixels, abi_dx::DirectXPixelFormat_B8G8R8A8UIntNormalized, abi_dx::DirectXAlphaMode_Premultiplied,
            drawingSurface.GetAddressOf())) ||
        FAILED(drawingSurface.As(&surface)) || FAILED(drawingSurface.As(&composition->surface)) ||
        FAILED(compositor->CreateSurfaceBrushWithSurface(surface.Get(), surfaceBrush.GetAddressOf())) ||
        FAILED(surfaceBrush->put_Stretch(abi_comp::CompositionStretch_None)) ||
        FAILED(surfaceBrush->put_HorizontalAlignmentRatio(0.0f)) ||
        FAILED(surfaceBrush->put_VerticalAlignmentRatio(0.0f)) || FAILED(surfaceBrush.As(&surfaceBrushBase)) ||
        FAILED(compositor->CreateSpriteVisual(contentSprite.GetAddressOf())) ||
        FAILED(contentSprite->put_Brush(surfaceBrushBase.Get())) || FAILED(contentSprite.As(&composition->content)) ||
        FAILED(composition->content->put_Size({sizePixels.Width, sizePixels.Height})) ||
        FAILED(children->InsertAtTop(composition->content.Get())))
    {
        DiscardTarget();
        return false;
    }

    // The blurred wallpaper (Mica's source) arrived with Windows 11; without it Wallpaper
    // falls back to the windows behind.
    ComPtr<abi_comp::ICompositorWithBlurredWallpaperBackdropBrush> wallpaperCompositor;
    ComPtr<abi_comp::ICompositionBackdropBrush> wallpaperBrush;
    if (SUCCEEDED(instance.As(&wallpaperCompositor)) &&
        SUCCEEDED(wallpaperCompositor->TryCreateBlurredWallpaperBackdropBrush(wallpaperBrush.GetAddressOf())) &&
        wallpaperBrush)
    {
        wallpaperBrush.As(&composition->wallpaper);
    }

    BOOL useHostBackdrop = TRUE;
    DwmSetWindowAttribute(hwnd_, kDwmUseHostBackdropBrush, &useHostBackdrop, sizeof(useHostBackdrop));
    surface_ = std::move(composition);
    if (!ApplyBackdropMaterial())
    {
        DiscardTarget();
        return false;
    }
    pixelWidth_ = width;
    pixelHeight_ = height;
    const FLOAT dpi = DpiForHwnd();
    deviceContext_->SetDpi(dpi, dpi);
    return true;
}
#endif // !MSIME_WIN7_LITE

#ifndef MSIME_WIN7_LITE
bool DeviceResources::GrowDrawingSurface(UINT width, UINT height)
{
    if (!surface_ || (pixelWidth_ >= width && pixelHeight_ >= height))
    {
        return surface_ != nullptr;
    }
    pixelWidth_ = (std::max)(pixelWidth_, width);
    pixelHeight_ = (std::max)(pixelHeight_, height);
    return SUCCEEDED(surface_->surface->Resize({static_cast<LONG>(pixelWidth_), static_cast<LONG>(pixelHeight_)})) &&
           SUCCEEDED(surface_->content->put_Size({static_cast<FLOAT>(pixelWidth_), static_cast<FLOAT>(pixelHeight_)}));
}
#endif // !MSIME_WIN7_LITE

void DeviceResources::SetBackdropMaterial(const BackdropMaterial &material)
{
#ifndef MSIME_WIN7_LITE
    if (backdropMaterial_ == material)
    {
        return;
    }
    backdropMaterial_ = material;
    if (surface_ && !ApplyBackdropMaterial())
    {
        DiscardTarget();
    }
#endif
}

#ifndef MSIME_WIN7_LITE
bool DeviceResources::ApplyBackdropMaterial()
{
    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::Wrappers::HStringReference;
    const BackdropMaterial &material = backdropMaterial_;
    const ComPtr<abi_comp::ICompositionBrush> &source =
        material.source == BackdropMaterial::Source::Wallpaper && surface_->wallpaper ? surface_->wallpaper
                                                                                      : surface_->windowsBehind;
    if (material.tintOpacity <= 0.0f && material.luminosityOpacity <= 0.0f)
    {
        return SUCCEEDED(surface_->backdropSprite->put_Brush(source.Get()));
    }

    ComPtr<ABI::Windows::Foundation::IPropertyValueStatics> values;
    ComPtr<abi_comp::ICompositionEffectSourceParameterFactory> parameters;
    ComPtr<abi_comp::ICompositionEffectSourceParameter> parameter;
    EffectSourcePtr backdropInput;
    if (FAILED(::Windows::Foundation::GetActivationFactory(
            HStringReference(RuntimeClass_Windows_Foundation_PropertyValue).Get(), values.GetAddressOf())) ||
        FAILED(::Windows::Foundation::GetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Composition_CompositionEffectSourceParameter).Get(),
            parameters.GetAddressOf())) ||
        FAILED(parameters->Create(HStringReference(L"Backdrop").Get(), parameter.GetAddressOf())) ||
        FAILED(parameter.As(&backdropInput)))
    {
        return false;
    }
    const auto box = [](HRESULT hr, const ComPtr<IInspectable> &inspectable) {
        PropertyValuePtr value;
        if (SUCCEEDED(hr))
        {
            inspectable.As(&value);
        }
        return value;
    };
    // Flood fill of the tint colour at `alpha`; D2D floods take straight (unpremultiplied) colour.
    const auto flood = [&](float alpha) -> EffectSourcePtr {
        FLOAT rgba[4] = {material.tint.r, material.tint.g, material.tint.b, std::clamp(alpha, 0.0f, 1.0f)};
        ComPtr<IInspectable> inspectable;
        PropertyValuePtr color = box(values->CreateSingleArray(4, rgba, inspectable.GetAddressOf()), inspectable);
        if (!color)
        {
            return nullptr;
        }
        return Microsoft::WRL::Make<CompositionEffect>(CLSID_D2D1Flood, std::vector<PropertyValuePtr>{color},
                                                       std::vector<EffectSourcePtr>{});
    };
    // D2D blend inputs: 0 is the background (destination), 1 the foreground (source).
    const auto blend = [&](D2D1_BLEND_MODE mode, const EffectSourcePtr &background,
                           const EffectSourcePtr &foreground) -> EffectSourcePtr {
        ComPtr<IInspectable> inspectable;
        PropertyValuePtr modeValue =
            box(values->CreateUInt32(static_cast<UINT32>(mode), inspectable.GetAddressOf()), inspectable);
        if (!modeValue || !background || !foreground)
        {
            return nullptr;
        }
        return Microsoft::WRL::Make<CompositionEffect>(CLSID_D2D1Blend, std::vector<PropertyValuePtr>{modeValue},
                                                       std::vector<EffectSourcePtr>{background, foreground});
    };
    // Same graph as the WinUI AcrylicBrush. Composition swaps the Color and Luminosity blend
    // modes, so like WinUI the luminosity step asks for COLOR and the colour step for LUMINOSITY.
    const EffectSourcePtr luminosity = blend(D2D1_BLEND_MODE_COLOR, backdropInput, flood(material.luminosityOpacity));
    const EffectSourcePtr tinted = blend(D2D1_BLEND_MODE_LUMINOSITY, luminosity, flood(material.tintOpacity));

    ComPtr<abi_fx::IGraphicsEffect> effect;
    ComPtr<abi_comp::ICompositionEffectFactory> factory;
    ComPtr<abi_comp::ICompositionEffectBrush> effectBrush;
    ComPtr<abi_comp::ICompositionBrush> brush;
    return tinted && SUCCEEDED(tinted.As(&effect)) &&
           SUCCEEDED(surface_->compositor->CreateEffectFactory(effect.Get(), factory.GetAddressOf())) &&
           SUCCEEDED(factory->CreateBrush(effectBrush.GetAddressOf())) &&
           SUCCEEDED(effectBrush->SetSourceParameter(HStringReference(L"Backdrop").Get(), source.Get())) &&
           SUCCEEDED(effectBrush.As(&brush)) && SUCCEEDED(surface_->backdropSprite->put_Brush(brush.Get()));
}
#endif // !MSIME_WIN7_LITE

void DeviceResources::SetBackdrop(const RectF &rectDips, float radiusDips)
{
#ifndef MSIME_WIN7_LITE
    if (!surface_)
    {
        return;
    }
    const bool visible = rectDips.width > 0.0f && rectDips.height > 0.0f;
    surface_->backdrop->put_IsVisible(visible);
    if (!visible)
    {
        return;
    }
    // Composition visuals of a window target are laid out in physical pixels.
    const float scale = DpiForHwnd() / 96.0f;
    const float left = std::round(rectDips.x * scale);
    const float top = std::round(rectDips.y * scale);
    const float right = std::round((rectDips.x + rectDips.width) * scale);
    const float bottom = std::round((rectDips.y + rectDips.height) * scale);
    const float radius = (std::max)(radiusDips, 0.0f) * scale;
    surface_->backdrop->put_Offset({left, top, 0.0f});
    surface_->backdrop->put_Size({right - left, bottom - top});
    surface_->backdropShape->put_Size({right - left, bottom - top});
    surface_->backdropShape->put_CornerRadius({radius, radius});
#endif // !MSIME_WIN7_LITE
}

bool DeviceResources::BeginDraw()
{
    if (!surface_)
    {
        ID2D1RenderTarget *target = GetRenderTarget();
        if (!target)
        {
            return false;
        }
        target->BeginDraw();
        return true;
    }

#ifndef MSIME_WIN7_LITE
    // The whole surface is redrawn every frame, like a swap chain buffer.
    const RECT update = {0, 0, static_cast<LONG>(pixelWidth_), static_cast<LONG>(pixelHeight_)};
    POINT offset = {};
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    const HRESULT hr = surface_->surface->BeginDraw(&update, IID_PPV_ARGS(context.GetAddressOf()), &offset);
    if (FAILED(hr))
    {
        if (IsDeviceLost(hr))
        {
            DiscardTarget();
        }
        return false;
    }
    // The context draws into a shared atlas: `offset` is where this surface starts in it,
    // in pixels. Clip to the surface so Clear stays inside it, and move the origin there.
    const FLOAT dpi = DpiForHwnd();
    const float toDips = 96.0f / dpi;
    const float originX = static_cast<float>(offset.x) * toDips;
    const float originY = static_cast<float>(offset.y) * toDips;
    context->SetDpi(dpi, dpi);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->PushAxisAlignedClip(D2D1::RectF(originX, originY, originX + static_cast<float>(pixelWidth_) * toDips,
                                             originY + static_cast<float>(pixelHeight_) * toDips),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
    context->SetTransform(D2D1::Matrix3x2F::Translation(originX, originY));
    drawContext_ = context;
    return true;
#else
    // Unreachable in the Windows 7 build: without backdrop composition there is
    // no drawing surface, and the branch above already handled the target.
    return false;
#endif
}

HRESULT DeviceResources::EndDraw()
{
    if (!drawContext_)
    {
        ID2D1RenderTarget *target = GetRenderTarget();
        return target ? target->EndDraw() : S_OK;
    }
#ifndef MSIME_WIN7_LITE
    drawContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    drawContext_->PopAxisAlignedClip();
    drawContext_.Reset();
    const HRESULT hr = surface_ ? surface_->surface->EndDraw() : S_OK;
    return IsDeviceLost(hr) ? D2DERR_RECREATE_TARGET : hr;
#else
    return S_OK;
#endif
}

bool DeviceResources::EnsureForComposition(HWND hwnd)
{
#ifdef MSIME_WIN7_LITE
    // Windows 7 has neither DirectComposition nor the Windows Runtime; there is
    // no composition target to build. Fall back to the plain HWND render target,
    // which also resets composition_ so the frame draws through BeginDraw's
    // target branch and Present() becomes a no-op.
    return EnsureForWindow(hwnd);
#else
    hwnd_ = hwnd;
    if (!EnsureFactories() || !d2dFactory1_ || !hwnd)
    {
        return false;
    }

    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const UINT width = AlignSwapExtent(static_cast<UINT>((std::max)(rc.right, 1L)));
    const UINT height = AlignSwapExtent(static_cast<UINT>((std::max)(rc.bottom, 1L)));

    if (backdropComposition_)
    {
        return EnsureSurfaceComposition(width, height);
    }

    if (composition_ && deviceContext_ && swapChain_ && pixelWidth_ >= width && pixelHeight_ >= height)
    {
        const FLOAT dpi = DpiForHwnd();
        deviceContext_->SetDpi(dpi, dpi);
        return true;
    }

    if (!composition_ || !d3dDevice_ || !swapChain_)
    {
        DiscardTarget();
        composition_ = true;
        if (!CreateCompositionDevice())
        {
            composition_ = false;
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(dxgiDevice_->GetAdapter(adapter.GetAddressOf())) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
        {
            composition_ = false;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = compositionOpaque_ ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Scaling = DXGI_SCALING_STRETCH;
        if (FAILED(factory->CreateSwapChainForComposition(d3dDevice_.Get(), &desc, nullptr, swapChain_.GetAddressOf())))
        {
            composition_ = false;
            return false;
        }

        if (FAILED(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(dcompDevice_.GetAddressOf()))) ||
            FAILED(dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, dcompTarget_.GetAddressOf())) ||
            FAILED(dcompDevice_->CreateVisual(dcompVisual_.GetAddressOf())) ||
            FAILED(dcompVisual_->SetContent(swapChain_.Get())) || FAILED(dcompTarget_->SetRoot(dcompVisual_.Get())))
        {
            composition_ = false;
            return false;
        }
        pixelWidth_ = width;
        pixelHeight_ = height;
        if (!BindCompositionSurface())
        {
            composition_ = false;
            return false;
        }
        dcompDevice_->Commit();
        return true;
    }

    const UINT newWidth = (std::max)(pixelWidth_, width);
    const UINT newHeight = (std::max)(pixelHeight_, height);
    pixelWidth_ = newWidth;
    pixelHeight_ = newHeight;
    deviceContext_->SetTarget(nullptr);
    dxgiBitmap_.Reset();
    if (FAILED(swapChain_->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
    {
        return false;
    }
    return BindCompositionSurface();
#endif
}

void DeviceResources::Resize(UINT width, UINT height)
{
    width = (std::max)(width, 1U);
    height = (std::max)(height, 1U);
    if (hwndRenderTarget_)
    {
        hwndRenderTarget_->Resize(D2D1::SizeU(width, height));
        pixelWidth_ = width;
        pixelHeight_ = height;
        return;
    }
#ifndef MSIME_WIN7_LITE
    if (composition_ && surface_)
    {
        GrowDrawingSurface(AlignSwapExtent(width), AlignSwapExtent(height));
        return;
    }
    if (composition_ && swapChain_ && deviceContext_)
    {
        const UINT neededW = AlignSwapExtent(width);
        const UINT neededH = AlignSwapExtent(height);
        if (pixelWidth_ >= neededW && pixelHeight_ >= neededH)
        {
            return;
        }
        pixelWidth_ = (std::max)(pixelWidth_, neededW);
        pixelHeight_ = (std::max)(pixelHeight_, neededH);
        deviceContext_->SetTarget(nullptr);
        dxgiBitmap_.Reset();
        if (SUCCEEDED(swapChain_->ResizeBuffers(0, pixelWidth_, pixelHeight_, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
        {
            BindCompositionSurface();
        }
    }
#endif // !MSIME_WIN7_LITE
}

void DeviceResources::DiscardTarget()
{
    drawContext_.Reset();
#ifndef MSIME_WIN7_LITE
    if (surface_)
    {
        // WinRT visuals go before the Direct3D device they draw with.
        surface_.reset();
        BOOL useHostBackdrop = FALSE;
        DwmSetWindowAttribute(hwnd_, kDwmUseHostBackdropBrush, &useHostBackdrop, sizeof(useHostBackdrop));
    }
#endif
    if (deviceContext_)
    {
        deviceContext_->SetTarget(nullptr);
    }
    dxgiBitmap_.Reset();
    hwndRenderTarget_.Reset();
    deviceContext_.Reset();
    d2dDevice_.Reset();
    swapChain_.Reset();
#ifndef MSIME_WIN7_LITE
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
#endif
    dxgiDevice_.Reset();
    d3dDevice_.Reset();
    brushCache_.clear();
    bitmapCache_.clear();
    svgCache_.clear();
    animatedCache_.clear();
    animatedBytes_ = 0;
    clipStack_.clear();
    composition_ = false;
}

HRESULT DeviceResources::Present()
{
    // A drawing surface has nothing to present: EndDraw handed the frame to the compositor,
    // which commits it together with the backdrop when the thread returns to its message loop.
    if (!swapChain_)
    {
        return S_OK;
    }
    const HRESULT hr = swapChain_->Present(0, 0);
#ifndef MSIME_WIN7_LITE
    if (dcompDevice_)
    {
        dcompDevice_->Commit();
    }
#endif
    return hr;
}

ID2D1RenderTarget *DeviceResources::GetRenderTarget() const
{
    if (drawContext_)
    {
        return drawContext_.Get();
    }
    if (deviceContext_)
    {
        return deviceContext_.Get();
    }
    return hwndRenderTarget_.Get();
}

ID2D1DeviceContext *DeviceResources::GetDeviceContext() const
{
    return deviceContext_.Get();
}

bool DeviceResources::UsesComposition() const
{
    return composition_;
}

IDWriteFactory *DeviceResources::GetDWriteFactory() const
{
    return dwriteFactory_.Get();
}

ID2D1SolidColorBrush *DeviceResources::GetSolidColorBrush(const D2D1_COLOR_F &color)
{
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target)
    {
        return nullptr;
    }

    const uint64_t key = ColorKey(color);
    auto found = brushCache_.find(key);
    if (found != brushCache_.end() && found->second.brush)
    {
        found->second.lastFrame = frame_;
        return found->second.brush.Get();
    }

    BrushCacheEntry entry;
    entry.lastFrame = frame_;
    if (FAILED(target->CreateSolidColorBrush(color, entry.brush.GetAddressOf())))
    {
        return nullptr;
    }

    ID2D1SolidColorBrush *brush = entry.brush.Get();
    brushCache_[key] = std::move(entry);
    return brush;
}

IDWriteTextFormat *DeviceResources::GetTextFormat(const std::wstring &fontFamily, float fontSize,
                                                  DWRITE_FONT_WEIGHT fontWeight, DWRITE_TEXT_ALIGNMENT textAlignment,
                                                  DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                                                  DWRITE_WORD_WRAPPING wordWrapping)
{
    if (!dwriteFactory_)
    {
        return nullptr;
    }

    for (auto &entry : textFormatCache_)
    {
        if (entry.fontFamily == fontFamily && entry.fontSize == fontSize && entry.fontWeight == fontWeight &&
            entry.textAlignment == textAlignment && entry.paragraphAlignment == paragraphAlignment &&
            entry.wordWrapping == wordWrapping && entry.format)
        {
            return entry.format.Get();
        }
    }

    TextFormatCacheEntry entry;
    entry.fontFamily = fontFamily;
    entry.fontSize = fontSize;
    entry.fontWeight = fontWeight;
    entry.textAlignment = textAlignment;
    entry.paragraphAlignment = paragraphAlignment;
    entry.wordWrapping = wordWrapping;
    if (FAILED(dwriteFactory_->CreateTextFormat(entry.fontFamily.c_str(), nullptr, entry.fontWeight,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, entry.fontSize,
                                                L"", entry.format.GetAddressOf())))
    {
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Microsoft YaHei", nullptr, entry.fontWeight,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                    entry.fontSize, L"", entry.format.GetAddressOf())))
        {
            return nullptr;
        }
    }

    entry.format->SetTextAlignment(entry.textAlignment);
    entry.format->SetParagraphAlignment(entry.paragraphAlignment);
    entry.format->SetWordWrapping(entry.wordWrapping);
    ApplyUiFontFallback(dwriteFactory_.Get(), entry.format.Get());
    textFormatCache_.push_back(std::move(entry));
    return textFormatCache_.back().format.Get();
}

ID2D1Bitmap *DeviceResources::GetBitmapFromFile(const std::wstring &filePath, D2D1_SIZE_F *size)
{
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target || !wicFactory_ || filePath.empty())
    {
        return nullptr;
    }

    for (auto it = bitmapCache_.begin(); it != bitmapCache_.end(); ++it)
    {
        if (it->filePath == filePath && it->bitmap)
        {
            // 命中的挪到末尾，末尾即最近使用；淘汰从头部开始。
            if (std::next(it) != bitmapCache_.end())
            {
                std::rotate(it, std::next(it), bitmapCache_.end());
            }
            if (size)
            {
                *size = bitmapCache_.back().size;
            }
            return bitmapCache_.back().bitmap.Get();
        }
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())))
    {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
    {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
    {
        return nullptr;
    }

    // 先把像素解进内存里的 WIC 位图再建 D2D 位图：直接拿解码器建的 D2D 位图会一直引用解码器，
    // 缓存期间文件句柄不放，皮肤图删不掉也覆盖不了。
    Microsoft::WRL::ComPtr<IWICBitmap> pixels;
    if (FAILED(wicFactory_->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, pixels.GetAddressOf())))
    {
        return nullptr;
    }

    BitmapCacheEntry entry;
    entry.filePath = filePath;
    if (FAILED(target->CreateBitmapFromWicBitmap(pixels.Get(), nullptr, entry.bitmap.GetAddressOf())))
    {
        return nullptr;
    }
    entry.size = entry.bitmap->GetSize();
    // 位图按原始分辨率解码成 32bpp，调用方每帧按路径重新取、不持有指针，所以超出上限时
    // 丢掉最久没用的只是下次重新解码。不设上限的话，换过的皮肤图会一直留到设备重建。
    if (bitmapCache_.size() >= kMaxCachedBitmaps)
    {
        bitmapCache_.erase(bitmapCache_.begin());
    }
    bitmapCache_.push_back(std::move(entry));
    if (size)
    {
        *size = bitmapCache_.back().size;
    }
    return bitmapCache_.back().bitmap.Get();
}

ID2D1Bitmap *DeviceResources::GetSvgBitmap(const std::string &markup, const SizeF &sizeDips)
{
    // SVG rasterization needs ID2D1DeviceContext5 (Windows 10 1703+, d2d1_3.h
    // is empty under a Windows 7 target). The skin loader treats a null return
    // as "not available" and skips the image, so the Win7 build just renders
    // skins without SVG artwork.
#ifdef MSIME_WIN7_LITE
    (void)markup;
    (void)sizeDips;
    return nullptr;
#else
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target || markup.empty() || sizeDips.width <= 0.0f || sizeDips.height <= 0.0f)
    {
        return nullptr;
    }

    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target->GetDpi(&dpiX, &dpiY);
    const size_t hash = std::hash<std::string>{}(markup);
    for (auto it = svgCache_.begin(); it != svgCache_.end(); ++it)
    {
        if (it->markupHash == hash && it->dpi == dpiX && it->size.width == sizeDips.width &&
            it->size.height == sizeDips.height && it->markup == markup && it->bitmap)
        {
            if (std::next(it) != svgCache_.end())
            {
                std::rotate(it, std::next(it), svgCache_.end());
            }
            return svgCache_.back().bitmap.Get();
        }
    }

    // Rasterize once into a bitmap of the target's own pixel format and DPI; drawing a
    // bitmap afterwards is far cheaper than walking the SVG tree every frame, and it can
    // take an opacity like any other bitmap. The pixel size is rounded rather than left to
    // D2D, which rounds up: a box of 27.0001 px would get a 28 px bitmap squeezed into it.
    const D2D1_SIZE_U pixelSize =
        D2D1::SizeU(static_cast<UINT32>(std::max(std::lround(sizeDips.width * dpiX / 96.0f), 1L)),
                    static_cast<UINT32>(std::max(std::lround(sizeDips.height * dpiY / 96.0f), 1L)));
    Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> raster;
    if (FAILED(target->CreateCompatibleRenderTarget(D2D1::SizeF(sizeDips.width, sizeDips.height), pixelSize,
                                                    raster.GetAddressOf())))
    {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> context;
    if (FAILED(raster.As(&context)))
    {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(markup.data()), static_cast<UINT>(markup.size())));
    Microsoft::WRL::ComPtr<ID2D1SvgDocument> document;
    if (!stream || FAILED(context->CreateSvgDocument(stream.Get(), D2D1::SizeF(sizeDips.width, sizeDips.height),
                                                     document.GetAddressOf())))
    {
        return nullptr;
    }
    // Scale the drawing to the requested box like an <img> does: the root's own
    // width/height would otherwise draw it at its intrinsic size inside the viewport.
    Microsoft::WRL::ComPtr<ID2D1SvgElement> root;
    document->GetRoot(root.GetAddressOf());
    if (root)
    {
        if (!root->IsAttributeSpecified(L"viewBox"))
        {
            D2D1_SVG_LENGTH width = {sizeDips.width, D2D1_SVG_LENGTH_UNITS_NUMBER};
            D2D1_SVG_LENGTH height = {sizeDips.height, D2D1_SVG_LENGTH_UNITS_NUMBER};
            root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &width, sizeof(width));
            root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &height, sizeof(height));
            const D2D1_SVG_VIEWBOX viewBox = {0.0f, 0.0f, width.value, height.value};
            root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &viewBox, sizeof(viewBox));
        }
        const D2D1_SVG_LENGTH width = {sizeDips.width, D2D1_SVG_LENGTH_UNITS_NUMBER};
        const D2D1_SVG_LENGTH height = {sizeDips.height, D2D1_SVG_LENGTH_UNITS_NUMBER};
        root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &width, sizeof(width));
        root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &height, sizeof(height));
    }

    raster->BeginDraw();
    raster->Clear(D2D1::ColorF(0, 0.0f));
    context->DrawSvgDocument(document.Get());
    SvgCacheEntry entry;
    if (FAILED(raster->EndDraw()) || FAILED(raster->GetBitmap(entry.bitmap.GetAddressOf())))
    {
        return nullptr;
    }
    entry.markupHash = hash;
    entry.markup = markup;
    entry.size = sizeDips;
    entry.dpi = dpiX;
    if (svgCache_.size() >= kMaxCachedSvgBitmaps)
    {
        svgCache_.erase(svgCache_.begin());
    }
    svgCache_.push_back(std::move(entry));
    return svgCache_.back().bitmap.Get();
#endif // !MSIME_WIN7_LITE
}
} // namespace msimeui
