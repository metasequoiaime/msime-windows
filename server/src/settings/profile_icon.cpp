#include "profile_icon.h"

#include <msctf.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace profile_icon
{
namespace
{

constexpr wchar_t kApplySwitch[] = L"--apply-profile-icon";
constexpr wchar_t kResetSwitch[] = L"--reset-profile-icon";

// Read by windows/src/Register/Register.cpp at registration time; keep the two in step.
constexpr wchar_t kRecordKey[] = L"Software\\Metasequoia\\MetasequoiaIME";
constexpr wchar_t kRecordFile[] = L"ProfileIconFile";
// Only for the settings page, which shows the choice; registration needs the file name alone.
constexpr wchar_t kRecordChoice[] = L"ProfileIconChoice";

// Same CLSID and profile GUID as windows/src/Global/Globals.cpp; see game_compat_name.cpp for the key.
constexpr CLSID kTipClsid = {0xe3062e9a, 0xd834, 0x4637, {0x89, 0x58, 0xed, 0x8c, 0xfa, 0x42, 0x7d, 0x01}};
constexpr GUID kTipProfile = {0x4d59b1b4, 0xd503, 0x44ae, {0x92, 0x59, 0xba, 0xd9, 0xbb, 0x27, 0x78, 0xab}};
constexpr LANGID kTipLangId = 0x0804;
constexpr wchar_t kTipProfileKey[] = L"SOFTWARE\\Microsoft\\CTF\\TIP\\{E3062E9A-D834-4637-8958-ED8CFA427D01}"
                                     L"\\LanguageProfile\\0x00000804\\{4D59B1B4-D503-44AE-9259-BAD9BB2778AB}";
// TEXTSERVICE_DESC in windows/src/Register/Register.cpp, used only if the profile has no Description to keep.
constexpr wchar_t kProductName[] = L"Metasequoia 水杉输入法";
// TEXTSERVICE_STABLE_ICON_FILE in windows/src/Register/Register.cpp, installed by msime_setup.iss.
constexpr wchar_t kInstalledIconFile[] = L"MetasequoiaImeTip.ico";
constexpr wchar_t kStoredPrefix[] = L"MetasequoiaImeTip-";
constexpr size_t kStoredHashDigits = 16;

// An image this large is not a logo; refuse before WIC allocates for it.
constexpr UINT kMaxSourceEdge = 8192;

enum ExitCode : int
{
    Ok = 0,
    BadArguments = 2,
    RecordFailed = 3,
    NotRegistered = 4,
    BadIcon = 5,
    NotInstalled = 6,
    CopyFailed = 7,
    RegisterFailed = 8,
};

std::string Narrow(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::wstring Widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

uint16_t ReadU16(const std::vector<uint8_t> &bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t ReadU32(const std::vector<uint8_t> &bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void WriteU16(std::vector<uint8_t> &bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void WriteU32(std::vector<uint8_t> &bytes, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

bool ReadFileBytes(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxIconBytes * 16)
        return false;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    bytes.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount()) == bytes.size();
}

std::wstring ReadHklmString(const wchar_t *subkey, const wchar_t *value)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr,
                     &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer.data(),
                     &bytes) != ERROR_SUCCESS)
        return {};
    return std::wstring(buffer.data());
}

bool WriteHklmString(const wchar_t *subkey, const wchar_t *value, const std::wstring &data)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS status = RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
                                          static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool DeleteHklmValue(const wchar_t *subkey, const wchar_t *value)
{
    // RegDeleteKeyValue has no WOW64 flag; this process is 64-bit, so it already sees the native view.
    const LSTATUS status = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, subkey, value);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

bool IsTipRegistered()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTipProfileKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

// <System32 or SysWOW64>\IME\MetasequoiaIME, the msime_setup.iss MyTsfRoot64 / MyTsfRoot32. This process is 64-bit,
// so neither path is redirected.
std::filesystem::path TsfRoot(bool wow64)
{
    wchar_t directory[MAX_PATH] = {};
    const UINT length =
        wow64 ? GetSystemWow64DirectoryW(directory, MAX_PATH) : GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(directory) / L"IME" / L"MetasequoiaIME";
}

bool IsDirectory(const std::filesystem::path &path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::is_directory(path, ec);
}

bool WriteFileAtomically(const std::filesystem::path &target, const std::vector<uint8_t> &bytes)
{
    std::filesystem::path temporary = target;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            out.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// Earlier choices. A file the shell still has open stays until the next change; nothing reads it any more.
void RemoveStoredIcons(const std::filesystem::path &root, const std::wstring &keep)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::wstring name = it->path().filename().wstring();
        if (IsStoredFileName(name) && _wcsicmp(name.c_str(), keep.c_str()) != 0)
            DeleteFileW(it->path().c_str());
    }
}

// Registers the profile again with `iconFile`, keeping its current name (the game compatibility switch may have
// changed it). RegisterProfile is what installs call too; writing IconFile directly is only the fallback.
bool RegisterIconFile(const std::wstring &iconFile)
{
    std::wstring description = ReadHklmString(kTipProfileKey, L"Description");
    if (description.empty())
        description = kProductName;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        ComPtr<ITfInputProcessorProfileMgr> manager;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(manager.GetAddressOf()))))
        {
            manager->RegisterProfile(kTipClsid, kTipLangId, kTipProfile, description.c_str(),
                                     static_cast<ULONG>(description.size()), iconFile.c_str(),
                                     static_cast<ULONG>(iconFile.size()), 0, nullptr, 0, TRUE, 0);
        }
    }
    if (SUCCEEDED(init))
        CoUninitialize();

    if (_wcsicmp(ReadHklmString(kTipProfileKey, L"IconFile").c_str(), iconFile.c_str()) == 0)
        return true;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTipProfileKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    const DWORD index = 0;
    const bool written = RegSetValueExW(key, L"IconFile", 0, REG_SZ, reinterpret_cast<const BYTE *>(iconFile.c_str()),
                                        static_cast<DWORD>((iconFile.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS &&
                         RegSetValueExW(key, L"IconIndex", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&index),
                                        sizeof(index)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return written;
}

bool IsValidChoice(const std::wstring &choice)
{
    if (choice == Widen(kCustomChoice))
        return true;
    const Builtin *builtin = FindBuiltin(Narrow(choice));
    return builtin != nullptr && std::string(builtin->id) != kDefaultChoice;
}

int ApplyElevated(const std::wstring &choice, const std::filesystem::path &source)
{
    if (!IsValidChoice(choice))
        return BadArguments;
    if (!IsTipRegistered())
        return NotRegistered;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(source, bytes) || bytes.size() > kMaxIconBytes || !IsValidIcon(bytes))
        return BadIcon;
    const std::filesystem::path root64 = TsfRoot(false);
    if (!IsDirectory(root64))
        return NotInstalled;

    // Both roots, like the installer: a 32-bit process reading the System32 path lands in SysWOW64.
    const std::wstring name = StoredFileName(bytes);
    for (const std::filesystem::path &root : {root64, TsfRoot(true)})
    {
        if (IsDirectory(root) && !WriteFileAtomically(root / name, bytes))
            return CopyFailed;
    }
    if (!WriteHklmString(kRecordKey, kRecordFile, name) || !WriteHklmString(kRecordKey, kRecordChoice, choice))
        return RecordFailed;
    if (!RegisterIconFile((root64 / name).wstring()))
        return RegisterFailed;
    for (const std::filesystem::path &root : {root64, TsfRoot(true)})
    {
        if (IsDirectory(root))
            RemoveStoredIcons(root, name);
    }
    return Ok;
}

int ResetElevated()
{
    if (!IsTipRegistered())
        return NotRegistered;
    const std::filesystem::path root64 = TsfRoot(false);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(root64 / kInstalledIconFile, ec))
        return NotInstalled;
    if (!DeleteHklmValue(kRecordKey, kRecordFile) || !DeleteHklmValue(kRecordKey, kRecordChoice))
        return RecordFailed;
    if (!RegisterIconFile((root64 / kInstalledIconFile).wstring()))
        return RegisterFailed;
    for (const std::filesystem::path &root : {root64, TsfRoot(true)})
    {
        if (IsDirectory(root))
            RemoveStoredIcons(root, {});
    }
    return Ok;
}

std::string DescribeExitCode(DWORD code)
{
    switch (code)
    {
    case BadArguments:
        return "图标选项无效。";
    case RecordFailed:
        return "无法写入设置，请确认在弹出的授权窗口中选择了「是」。";
    case NotRegistered:
        return "水杉输入法没有在系统中注册，请重新安装输入法。";
    case BadIcon:
        return "图标文件无效或过大。";
    case NotInstalled:
        return "找不到输入法的安装目录（开发版从构建目录注册时不支持更换图标），请使用安装包安装。";
    case CopyFailed:
        return "无法把图标复制到输入法安装目录。";
    case RegisterFailed:
        return "系统没有接受新的图标登记。";
    default:
        return "应用失败（错误码 " + std::to_string(code) + "）。";
    }
}

std::wstring OwnExecutablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
            return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

bool RunElevated(HWND owner, const std::wstring &parameters, std::string &error)
{
    const std::wstring executable = OwnExecutablePath();
    if (executable.empty())
    {
        error = "找不到设置程序自身的路径。";
        return false;
    }
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess)
    {
        error = GetLastError() == ERROR_CANCELLED ? "已取消：更换输入法图标需要管理员权限。"
                                                  : "无法以管理员身份运行设置程序。";
        return false;
    }
    const DWORD wait = WaitForSingleObject(execute.hProcess, 60 * 1000);
    DWORD code = static_cast<DWORD>(-1);
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(execute.hProcess, &code);
    CloseHandle(execute.hProcess);
    if (wait != WAIT_OBJECT_0)
    {
        error = "等待管理员权限的操作超时。";
        return false;
    }
    if (code != Ok)
    {
        error = DescribeExitCode(code);
        return false;
    }
    return true;
}

bool EndsWithFileName(const std::wstring &path, const std::wstring &name)
{
    return _wcsicmp(std::filesystem::path(path).filename().c_str(), name.c_str()) == 0;
}

// Validates and hashes the icon here as well, so a bad file is reported without a UAC prompt and the result can be
// checked against the name the elevated instance must have registered.
bool ApplyIconFile(HWND owner, const std::string &choice, const std::filesystem::path &icon, std::string &error)
{
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(icon, bytes) || bytes.size() > kMaxIconBytes || !IsValidIcon(bytes))
    {
        error = DescribeExitCode(BadIcon);
        return false;
    }
    const std::wstring parameters = std::wstring(kApplySwitch) + L" " + Widen(choice) + L" \"" + icon.wstring() + L"\"";
    if (!RunElevated(owner, parameters, error))
        return false;
    if (!EndsWithFileName(QueryStatus().iconFile, StoredFileName(bytes)))
    {
        error = DescribeExitCode(RegisterFailed);
        return false;
    }
    return true;
}

struct WicFrame
{
    std::vector<uint8_t> pixels; // 32bpp premultiplied BGRA, tightly packed
    UINT width = 0;
    UINT height = 0;
};

bool EncodePng(IWICImagingFactory *factory, const std::vector<uint8_t> &bgra, int edge, std::vector<uint8_t> &png)
{
    ComPtr<IWICBitmap> bitmap;
    const UINT stride = static_cast<UINT>(edge) * 4;
    if (FAILED(factory->CreateBitmapFromMemory(edge, edge, GUID_WICPixelFormat32bppBGRA, stride,
                                               static_cast<UINT>(bgra.size()), const_cast<BYTE *>(bgra.data()),
                                               bitmap.GetAddressOf())))
        return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf())))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(edge, edge)) || FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat32bppBGRA || FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    ULARGE_INTEGER end = {};
    if (FAILED(stream->Seek({}, STREAM_SEEK_CUR, &end)) || end.QuadPart == 0 || end.QuadPart > kMaxIconBytes)
        return false;
    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global)))
        return false;
    const void *data = GlobalLock(global);
    if (data == nullptr)
        return false;
    const auto *begin = static_cast<const uint8_t *>(data);
    png.assign(begin, begin + end.QuadPart);
    GlobalUnlock(global);
    return true;
}

bool DecodeLargestFrame(IWICImagingFactory *factory, const std::filesystem::path &image, ComPtr<IWICBitmapSource> &out,
                        UINT &width, UINT &height)
{
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(image.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  decoder.GetAddressOf())))
        return false;
    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count)) || count == 0)
        return false;
    ComPtr<IWICBitmapFrameDecode> best;
    UINT64 bestArea = 0;
    for (UINT i = 0; i < count && i < 64; ++i)
    {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0;
        UINT h = 0;
        if (FAILED(decoder->GetFrame(i, frame.GetAddressOf())) || FAILED(frame->GetSize(&w, &h)))
            continue;
        if (static_cast<UINT64>(w) * h > bestArea)
        {
            bestArea = static_cast<UINT64>(w) * h;
            best = frame;
            width = w;
            height = h;
        }
    }
    if (!best || width == 0 || height == 0 || width > kMaxSourceEdge || height > kMaxSourceEdge)
        return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(best.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
        return false;
    out = converter;
    return true;
}

// One square frame: the source scaled to fit, centered on transparency, un-premultiplied for PNG.
bool RenderFrame(IWICImagingFactory *factory, IWICBitmapSource *source, UINT width, UINT height, int edge,
                 std::vector<uint8_t> &bgra)
{
    const double scale = static_cast<double>(edge) / static_cast<double>((std::max)(width, height));
    const UINT scaledWidth = (std::max<UINT>)(1, static_cast<UINT>(std::lround(width * scale)));
    const UINT scaledHeight = (std::max<UINT>)(1, static_cast<UINT>(std::lround(height * scale)));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(source, scaledWidth, scaledHeight, WICBitmapInterpolationModeHighQualityCubic)))
        return false;
    std::vector<uint8_t> scaled(static_cast<size_t>(scaledWidth) * scaledHeight * 4);
    if (FAILED(scaler->CopyPixels(nullptr, scaledWidth * 4, static_cast<UINT>(scaled.size()), scaled.data())))
        return false;

    bgra.assign(static_cast<size_t>(edge) * edge * 4, 0);
    const UINT left = (static_cast<UINT>(edge) - scaledWidth) / 2;
    const UINT top = (static_cast<UINT>(edge) - scaledHeight) / 2;
    for (UINT y = 0; y < scaledHeight; ++y)
    {
        for (UINT x = 0; x < scaledWidth; ++x)
        {
            const uint8_t *in = &scaled[(static_cast<size_t>(y) * scaledWidth + x) * 4];
            uint8_t *out = &bgra[(static_cast<size_t>(y + top) * edge + x + left) * 4];
            const uint8_t alpha = in[3];
            out[3] = alpha;
            if (alpha == 0)
                continue;
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>((std::min)(255, (in[c] * 255 + alpha / 2) / alpha));
        }
    }
    return true;
}

} // namespace

const std::vector<Builtin> &Builtins()
{
    static const std::vector<Builtin> builtins = {
        {kDefaultChoice, L"水杉（默认）", L"profile-icons/default.ico"},
        {"logo-v2", L"水杉 v2", L"profile-icons/logo-v2.ico"},
    };
    return builtins;
}

const Builtin *FindBuiltin(const std::string &id)
{
    for (const Builtin &builtin : Builtins())
    {
        if (id == builtin.id)
            return &builtin;
    }
    return nullptr;
}

Status QueryStatus()
{
    Status status;
    status.registered = IsTipRegistered();
    status.iconFile = ReadHklmString(kTipProfileKey, L"IconFile");
    const std::string choice = Narrow(ReadHklmString(kRecordKey, kRecordChoice));
    status.choice = choice.empty() ? std::string(kDefaultChoice) : choice;
    return status;
}

bool ApplyBuiltin(HWND owner, const std::string &id, const std::filesystem::path &assetDirectory, std::string &error)
{
    const Builtin *builtin = FindBuiltin(id);
    if (builtin == nullptr)
    {
        error = DescribeExitCode(BadArguments);
        return false;
    }
    if (id == kDefaultChoice)
    {
        if (!RunElevated(owner, kResetSwitch, error))
            return false;
        if (!EndsWithFileName(QueryStatus().iconFile, kInstalledIconFile))
        {
            error = DescribeExitCode(RegisterFailed);
            return false;
        }
        return true;
    }
    return ApplyIconFile(owner, id, assetDirectory / builtin->asset, error);
}

bool ApplyCustom(HWND owner, const std::filesystem::path &image, std::string &error)
{
    std::vector<uint8_t> icon;
    if (!ConvertToIcon(image, icon, error))
        return false;
    wchar_t directory[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(MAX_PATH + 1, directory);
    if (length == 0 || length > MAX_PATH)
    {
        error = "找不到临时目录。";
        return false;
    }
    const std::filesystem::path temporary =
        std::filesystem::path(directory) / (L"msime-profile-icon-" + std::to_wstring(GetCurrentProcessId()) + L".ico");
    if (!WriteFileAtomically(temporary, icon))
    {
        error = "无法写入临时图标文件。";
        return false;
    }
    const bool ok = ApplyIconFile(owner, kCustomChoice, temporary, error);
    DeleteFileW(temporary.c_str());
    return ok;
}

std::filesystem::path PromptForImage(HWND owner)
{
    // 设置窗口的 UI 线程启动时已初始化过 STA，这里的 CoInitializeEx 只是配平引用计数。
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path selected;
    {
        ComPtr<IFileOpenDialog> dialog;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(dialog.GetAddressOf()))))
        {
            const COMDLG_FILTERSPEC filters[] = {
                {L"图片 (*.ico;*.png;*.jpg;*.jpeg;*.bmp)", L"*.ico;*.png;*.jpg;*.jpeg;*.bmp"}, {L"所有文件", L"*.*"}};
            dialog->SetFileTypes(ARRAYSIZE(filters), filters);
            dialog->SetTitle(L"选择输入法图标（建议使用 256×256 以上的正方形 PNG 或多尺寸 ICO）");
            DWORD options = 0;
            if (SUCCEEDED(dialog->GetOptions(&options)))
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(item.GetAddressOf())))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    selected = path;
                    CoTaskMemFree(path);
                }
            }
        }
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return selected;
}

int RunElevatedApplyIfRequested(const wchar_t *commandLine)
{
    if (commandLine == nullptr || *commandLine == L'\0')
        return -1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(commandLine, &argc);
    if (argv == nullptr)
        return -1;
    int result = -1;
    if (argc >= 1 && wcscmp(argv[0], kResetSwitch) == 0)
        result = ResetElevated();
    else if (argc >= 3 && wcscmp(argv[0], kApplySwitch) == 0)
        result = ApplyElevated(argv[1], argv[2]);
    else if (argc >= 1 && wcscmp(argv[0], kApplySwitch) == 0)
        result = BadArguments;
    LocalFree(argv);
    return result;
}

bool IsValidIcon(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() < 6 || bytes.size() > kMaxIconBytes)
        return false;
    if (ReadU16(bytes, 0) != 0 || ReadU16(bytes, 2) != 1)
        return false;
    const size_t count = ReadU16(bytes, 4);
    if (count == 0 || count > 64 || bytes.size() < 6 + 16 * count)
        return false;
    static const uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    for (size_t i = 0; i < count; ++i)
    {
        const size_t entry = 6 + 16 * i;
        const uint32_t size = ReadU32(bytes, entry + 8);
        const uint32_t offset = ReadU32(bytes, entry + 12);
        if (size < 40 || offset < 6 + 16 * count || offset > bytes.size() || size > bytes.size() - offset)
            return false;
        const bool png = std::memcmp(&bytes[offset], kPngSignature, sizeof(kPngSignature)) == 0;
        const bool dib = ReadU32(bytes, offset) == 40;
        if (!png && !dib)
            return false;
    }
    return true;
}

std::wstring StoredFileName(const std::vector<uint8_t> &bytes)
{
    uint64_t hash = 0xcbf29ce484222325ull; // FNV-1a
    for (const uint8_t byte : bytes)
    {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    wchar_t digits[kStoredHashDigits + 1] = {};
    swprintf_s(digits, L"%016llx", static_cast<unsigned long long>(hash));
    return std::wstring(kStoredPrefix) + digits + L".ico";
}

bool IsStoredFileName(const std::wstring &name)
{
    const std::wstring prefix = kStoredPrefix;
    const std::wstring suffix = L".ico";
    if (name.size() != prefix.size() + kStoredHashDigits + suffix.size())
        return false;
    if (_wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0 ||
        _wcsicmp(name.c_str() + prefix.size() + kStoredHashDigits, suffix.c_str()) != 0)
        return false;
    return std::all_of(name.begin() + prefix.size(), name.begin() + prefix.size() + kStoredHashDigits,
                       [](wchar_t ch) { return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'); });
}

std::vector<uint8_t> PackPngIcon(const std::vector<std::pair<int, std::vector<uint8_t>>> &frames)
{
    std::vector<uint8_t> icon;
    WriteU16(icon, 0);
    WriteU16(icon, 1);
    WriteU16(icon, static_cast<uint16_t>(frames.size()));
    uint32_t offset = static_cast<uint32_t>(6 + 16 * frames.size());
    for (const auto &[edge, png] : frames)
    {
        const uint8_t dimension = edge >= 256 ? 0 : static_cast<uint8_t>(edge); // 0 means 256
        icon.push_back(dimension);
        icon.push_back(dimension);
        icon.push_back(0); // palette size
        icon.push_back(0); // reserved
        WriteU16(icon, 1); // planes
        WriteU16(icon, 32);
        WriteU32(icon, static_cast<uint32_t>(png.size()));
        WriteU32(icon, offset);
        offset += static_cast<uint32_t>(png.size());
    }
    for (const auto &frame : frames)
        icon.insert(icon.end(), frame.second.begin(), frame.second.end());
    return icon;
}

const std::vector<int> &IconFrameSizes()
{
    // Same set as scripts/convert-png-to-ico.ps1 and windows/image/msime.ico.
    static const std::vector<int> sizes = {16, 20, 24, 28, 32, 36, 40, 44, 48, 64, 128, 256};
    return sizes;
}

bool ConvertToIcon(const std::filesystem::path &image, std::vector<uint8_t> &icon, std::string &error)
{
    std::wstring extension = image.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension == L".ico")
    {
        if (!ReadFileBytes(image, icon) || !IsValidIcon(icon))
        {
            error = "ICO 文件无效或超过 4 MB。";
            return false;
        }
        return true;
    }

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapSource> source;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf()))))
            error = "无法初始化图片解码组件。";
        else if (!DecodeLargestFrame(factory.Get(), image, source, width, height))
            error = "无法读取这张图片，或图片边长超过 8192 像素。";
        else
        {
            std::vector<std::pair<int, std::vector<uint8_t>>> frames;
            ok = true;
            for (const int edge : IconFrameSizes())
            {
                std::vector<uint8_t> bgra;
                std::vector<uint8_t> png;
                if (!RenderFrame(factory.Get(), source.Get(), width, height, edge, bgra) ||
                    !EncodePng(factory.Get(), bgra, edge, png))
                {
                    ok = false;
                    error = "生成图标失败。";
                    break;
                }
                frames.emplace_back(edge, std::move(png));
            }
            if (ok)
            {
                icon = PackPngIcon(frames);
                ok = IsValidIcon(icon);
                if (!ok)
                    error = "生成的图标超过 4 MB。";
            }
        }
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return ok;
}

} // namespace profile_icon
