#include "local_asr.h"

#include "local_asr_catalog.h"

#include "engine/voice/third_party/sherpa-onnx/c-api.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace local_asr
{
namespace
{

namespace fs = std::filesystem;

constexpr int kSampleRate = 16000;
constexpr int32_t kVadWindow = 512;
// 段首往前补（VadSegmentStart）：10 ms 一帧判断能量，最多往前 3 秒，低能量间隙不超过 0.25 秒，找到的起音前再留 0.3 秒。
constexpr long long kOnsetFrame = kSampleRate / 100;
constexpr long long kOnsetLookback = kSampleRate * 3;
constexpr long long kOnsetGap = kSampleRate / 4;
constexpr long long kOnsetPreRoll = kSampleRate * 3 / 10;
// 帧能量高出噪声底 4 倍（12 dB）算作语音；噪声底不低于 -100 dBFS。
constexpr float kOnsetRise = 4.0f;
constexpr float kOnsetFloor = 1e-5f;
// 闲置这么久的模型由后台线程释放；一个模型占几百 MB 到 1 GB 以上。
constexpr auto kIdleRelease = std::chrono::minutes(3);

// ---- runtime ----

#define MSIME_SHERPA_FUNCTIONS(X)                                                                                      \
    X(SherpaOnnxCreateOnlineRecognizer)                                                                                \
    X(SherpaOnnxDestroyOnlineRecognizer)                                                                               \
    X(SherpaOnnxCreateOnlineStream)                                                                                    \
    X(SherpaOnnxDestroyOnlineStream)                                                                                   \
    X(SherpaOnnxOnlineStreamAcceptWaveform)                                                                            \
    X(SherpaOnnxIsOnlineStreamReady)                                                                                   \
    X(SherpaOnnxDecodeOnlineStream)                                                                                    \
    X(SherpaOnnxGetOnlineStreamResult)                                                                                 \
    X(SherpaOnnxDestroyOnlineRecognizerResult)                                                                         \
    X(SherpaOnnxOnlineStreamReset)                                                                                     \
    X(SherpaOnnxOnlineStreamInputFinished)                                                                             \
    X(SherpaOnnxOnlineStreamIsEndpoint)                                                                                \
    X(SherpaOnnxCreateOfflineRecognizer)                                                                               \
    X(SherpaOnnxDestroyOfflineRecognizer)                                                                              \
    X(SherpaOnnxCreateOfflineStream)                                                                                   \
    X(SherpaOnnxDestroyOfflineStream)                                                                                  \
    X(SherpaOnnxAcceptWaveformOffline)                                                                                 \
    X(SherpaOnnxDecodeOfflineStream)                                                                                   \
    X(SherpaOnnxGetOfflineStreamResult)                                                                                \
    X(SherpaOnnxDestroyOfflineRecognizerResult)                                                                        \
    X(SherpaOnnxCreateVoiceActivityDetector)                                                                           \
    X(SherpaOnnxDestroyVoiceActivityDetector)                                                                          \
    X(SherpaOnnxVoiceActivityDetectorAcceptWaveform)                                                                   \
    X(SherpaOnnxVoiceActivityDetectorEmpty)                                                                            \
    X(SherpaOnnxVoiceActivityDetectorPop)                                                                              \
    X(SherpaOnnxVoiceActivityDetectorFront)                                                                            \
    X(SherpaOnnxDestroySpeechSegment)                                                                                  \
    X(SherpaOnnxVoiceActivityDetectorFlush)

struct SherpaApi
{
#define MSIME_SHERPA_POINTER(name) decltype(&::name) name = nullptr;
    MSIME_SHERPA_FUNCTIONS(MSIME_SHERPA_POINTER)
#undef MSIME_SHERPA_POINTER
};

std::mutex g_runtime_mutex;
SherpaApi g_api;
bool g_runtime_loaded = false;

std::string Sha256Hex(HANDLE file)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        bool ok = true;
        std::vector<unsigned char> buffer(1 << 20);
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                ok = false;
                break;
            }
            if (read == 0)
                break;
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0)
            {
                ok = false;
                break;
            }
        }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            for (const unsigned char byte : digest)
            {
                hex.push_back(kDigits[byte >> 4]);
                hex.push_back(kDigits[byte & 0x0f]);
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex;
}

class HandleGuard
{
  public:
    explicit HandleGuard(HANDLE handle) : handle_(handle)
    {
    }
    ~HandleGuard()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }
    HandleGuard(const HandleGuard &) = delete;
    HandleGuard &operator=(const HandleGuard &) = delete;
    HandleGuard(HandleGuard &&other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }
    HANDLE get() const
    {
        return handle_;
    }

  private:
    HANDLE handle_;
};

// 失败返回中文原因；成功时 g_api 可用。调用方持有 g_runtime_mutex。
std::string LoadRuntimeLocked()
{
    if (g_runtime_loaded)
        return {};
    if (!RuntimeInstalled())
        return "本地识别运行库尚未下载，请在设置的“语音输入”里下载模型";
    const fs::path directory = RuntimeDirectory();
    // 先把每个 DLL 以拒绝写入、删除的方式打开并校验，句柄一直留到全部载入之后：这期间文件换不掉，
    // 载入的就是校验过的那一份。
    std::vector<HandleGuard> locks;
    for (const RuntimeLibrary &library : RuntimeLibraries())
    {
        const fs::path path = directory / library.name;
        HandleGuard file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
            return std::string("本地识别运行库缺少 ") + library.name + "，请在设置里重新下载";
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart != library.size ||
            Sha256Hex(file.get()) != library.sha256)
            return std::string("本地识别运行库 ") + library.name + " 校验失败，请在设置里删除后重新下载";
        locks.push_back(std::move(file));
    }
    HMODULE sherpa = nullptr;
    for (const RuntimeLibrary &library : RuntimeLibraries())
    {
        const fs::path path = (directory / library.name).lexically_normal().make_preferred();
        HMODULE module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
            return std::string("无法载入本地识别运行库 ") + library.name + "（错误 " + std::to_string(GetLastError()) +
                   "）";
        sherpa = module; // 最后一个是 sherpa-onnx-c-api.dll
    }
    SherpaApi api;
#define MSIME_SHERPA_BIND(name)                                                                                        \
    api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(sherpa, #name));                                    \
    if (!api.name)                                                                                                     \
        return std::string("本地识别运行库缺少接口 ") + #name;
    MSIME_SHERPA_FUNCTIONS(MSIME_SHERPA_BIND)
#undef MSIME_SHERPA_BIND
    g_api = api;
    g_runtime_loaded = true;
    return {};
}

const SherpaApi *LoadRuntime(std::string &error)
{
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    error = LoadRuntimeLocked();
    return error.empty() ? &g_api : nullptr;
}

// ---- recognizer cache ----

int ThreadCount()
{
    const unsigned hardware = (std::max)(1u, std::thread::hardware_concurrency());
    return static_cast<int>(std::clamp(hardware / 2u, 1u, 4u));
}

// SenseVoice 自己判断语言，普通话夹英文时这样最好；只有明确选了英文才钉住。
std::string SenseVoiceLanguage(const std::string &language)
{
    std::string value = language;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "en" ? "en" : "auto";
}

struct LoadedRecognizer
{
    const SherpaApi *api = nullptr;
    const ModelEntry *model = nullptr;
    const SherpaOnnxOnlineRecognizer *online = nullptr;
    const SherpaOnnxOfflineRecognizer *offline = nullptr;
    std::string vad_model;
    std::atomic<std::chrono::steady_clock::rep> last_used{std::chrono::steady_clock::now().time_since_epoch().count()};

    ~LoadedRecognizer()
    {
        if (online)
            api->SherpaOnnxDestroyOnlineRecognizer(online);
        if (offline)
            api->SherpaOnnxDestroyOfflineRecognizer(offline);
    }

    void Touch()
    {
        last_used.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }
};

std::string FilePath(const ModelEntry &model, const char *key)
{
    for (const ModelFileEntry &file : model.files)
    {
        if (std::string_view(file.key) == key)
            return (ModelDirectory(model) / file.path).u8string();
    }
    return {};
}

std::shared_ptr<LoadedRecognizer> CreateRecognizer(const SherpaApi &api, const ModelEntry &model,
                                                   const std::string &language, std::string &error)
{
    auto loaded = std::make_shared<LoadedRecognizer>();
    loaded->api = &api;
    loaded->model = &model;
    const int threads = ThreadCount();
    if (model.kind == ModelKind::OnlineTransducer)
    {
        const std::string encoder = FilePath(model, "encoder");
        const std::string decoder = FilePath(model, "decoder");
        const std::string joiner = FilePath(model, "joiner");
        const std::string tokens = FilePath(model, "tokens");
        SherpaOnnxOnlineRecognizerConfig config{};
        config.feat_config.sample_rate = kSampleRate;
        config.feat_config.feature_dim = 80;
        config.model_config.transducer.encoder = encoder.c_str();
        config.model_config.transducer.decoder = decoder.c_str();
        config.model_config.transducer.joiner = joiner.c_str();
        config.model_config.tokens = tokens.c_str();
        config.model_config.num_threads = threads;
        config.model_config.provider = "cpu";
        if (*model.model_type)
            config.model_config.model_type = model.model_type;
        config.decoding_method = "greedy_search";
        config.max_active_paths = 4;
        config.enable_endpoint = 1;
        config.rule1_min_trailing_silence = 2.4f;
        config.rule2_min_trailing_silence = 1.0f;
        config.rule3_min_utterance_length = 20.0f;
        loaded->online = api.SherpaOnnxCreateOnlineRecognizer(&config);
        if (!loaded->online)
        {
            error = "本地语音模型载入失败，模型文件可能已损坏，请删除后重新下载";
            return nullptr;
        }
        return loaded;
    }

    loaded->vad_model = VadFile().u8string();
    SherpaOnnxOfflineRecognizerConfig config{};
    config.feat_config.sample_rate = kSampleRate;
    config.feat_config.feature_dim = 80;
    config.model_config.num_threads = threads;
    config.model_config.provider = "cpu";
    config.decoding_method = "greedy_search";
    if (*model.model_type)
        config.model_config.model_type = model.model_type;
    std::string tokens, sense_voice, sense_language, adaptor, llm, embedding, tokenizer, encoder, decoder, joiner;
    if (model.kind == ModelKind::OfflineTransducer)
    {
        encoder = FilePath(model, "encoder");
        decoder = FilePath(model, "decoder");
        joiner = FilePath(model, "joiner");
        tokens = FilePath(model, "tokens");
        config.model_config.transducer.encoder = encoder.c_str();
        config.model_config.transducer.decoder = decoder.c_str();
        config.model_config.transducer.joiner = joiner.c_str();
        config.model_config.tokens = tokens.c_str();
        // 整段解码时 encoder 只跑一遍，beam 的代价只落在 decoder/joiner 上，宽 beam 划算。
        config.decoding_method = "modified_beam_search";
        config.max_active_paths = 16;
    }
    else if (model.kind == ModelKind::OfflineSenseVoice)
    {
        tokens = FilePath(model, "tokens");
        sense_voice = FilePath(model, "model");
        sense_language = SenseVoiceLanguage(language);
        config.model_config.tokens = tokens.c_str();
        config.model_config.sense_voice.model = sense_voice.c_str();
        config.model_config.sense_voice.language = sense_language.c_str();
        config.model_config.sense_voice.use_itn = 1;
    }
    else
    {
        adaptor = FilePath(model, "encoder_adaptor");
        llm = FilePath(model, "llm");
        embedding = FilePath(model, "embedding");
        tokenizer = FilePath(model, "tokenizer");
        config.model_config.funasr_nano.encoder_adaptor = adaptor.c_str();
        config.model_config.funasr_nano.llm = llm.c_str();
        config.model_config.funasr_nano.embedding = embedding.c_str();
        config.model_config.funasr_nano.tokenizer = tokenizer.c_str();
        config.model_config.funasr_nano.itn = 1;
        config.model_config.funasr_nano.hotwords = "";
    }
    loaded->offline = api.SherpaOnnxCreateOfflineRecognizer(&config);
    if (!loaded->offline)
    {
        error = "本地语音模型载入失败，模型文件可能已损坏，请删除后重新下载";
        return nullptr;
    }
    return loaded;
}

std::mutex g_cache_mutex;
// 键：模型 id + SenseVoice 的语言（建识别器时就定了）。故意不析构：进程退出时 onnxruntime 自己的
// 静态对象可能已经没了，那时再销毁识别器会在死锁上崩溃，内存交给系统回收。
std::map<std::pair<std::string, std::string>, std::shared_ptr<LoadedRecognizer>> &g_cache =
    *new std::map<std::pair<std::string, std::string>, std::shared_ptr<LoadedRecognizer>>();
std::once_flag g_reaper_started;

void ReleaseIdle()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto idle = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kIdleRelease).count();
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    for (auto it = g_cache.begin(); it != g_cache.end();)
    {
        if (it->second.use_count() == 1 && now - it->second->last_used.load(std::memory_order_relaxed) >= idle)
            it = g_cache.erase(it);
        else
            ++it;
    }
}

void StartReaper()
{
    std::call_once(g_reaper_started, [] {
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                ReleaseIdle();
            }
        }).detach();
    });
}

std::shared_ptr<LoadedRecognizer> Acquire(const SherpaApi &api, const ModelEntry &model, const std::string &language,
                                          std::string &error)
{
    const std::string language_key = model.kind == ModelKind::OfflineSenseVoice ? SenseVoiceLanguage(language) : "";
    const auto key = std::make_pair(std::string(model.id), language_key);
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto found = g_cache.find(key);
    if (found == g_cache.end())
    {
        // 同时只驻留一个模型：两个常驻会让本来就是进程里最大的那块内存翻倍。
        for (auto it = g_cache.begin(); it != g_cache.end();)
        {
            if (it->second.use_count() == 1)
                it = g_cache.erase(it);
            else
                ++it;
        }
        auto created = CreateRecognizer(api, model, language, error);
        if (!created)
            return nullptr;
        found = g_cache.emplace(key, std::move(created)).first;
    }
    found->second->Touch();
    return found->second;
}

// ---- transcript ----

std::vector<std::string> Utf8Characters(std::string_view text)
{
    std::vector<std::string> characters;
    for (std::size_t i = 0; i < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t width = lead < 0x80           ? 1
                                  : (lead >> 5) == 0x6  ? 2
                                  : (lead >> 4) == 0xE  ? 3
                                  : (lead >> 3) == 0x1E ? 4
                                                        : 1;
        characters.emplace_back(text.substr(i, width));
        i += width;
    }
    return characters;
}

bool IsCjkPunctuation(std::string_view character)
{
    static const std::set<std::string_view> marks = {"，", "。", "？", "！", "、", "：", "；", "“",
                                                     "”",  "‘",  "’",  "（", "）", "《", "》", "…"};
    return marks.count(character) > 0;
}

bool IsCjkIdeograph(std::string_view character)
{
    if (character.size() < 3)
        return false;
    const auto byte = [&](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(character[i])); };
    const char32_t code = character.size() == 3 ? ((byte(0) & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) | (byte(2) & 0x3F)
                                                : ((byte(0) & 0x07) << 18) | ((byte(1) & 0x3F) << 12) |
                                                      ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F);
    return (code >= 0x3400 && code <= 0x9FFF) || (code >= 0xF900 && code <= 0xFAFF) ||
           (code >= 0x20000 && code <= 0x3FFFF);
}

bool IsSingleCapital(const std::vector<std::string> &characters, std::size_t index)
{
    if (index >= characters.size() || characters[index].size() != 1)
        return false;
    if (!std::isupper(static_cast<unsigned char>(characters[index][0])))
        return false;
    const bool left_ok = index == 0 || characters[index - 1] == " " || characters[index - 1].size() > 1;
    const bool right_ok =
        index + 1 >= characters.size() || characters[index + 1] == " " || characters[index + 1].size() > 1;
    return left_ok && right_ok;
}

bool IsAsciiAlnum(char c)
{
    return static_cast<unsigned char>(c) < 0x80 && std::isalnum(static_cast<unsigned char>(c));
}

std::string JoinSegments(const std::vector<std::string> &segments)
{
    std::string joined;
    for (const auto &segment : segments)
    {
        if (segment.empty())
            continue;
        if (!joined.empty() && IsAsciiAlnum(joined.back()) && IsAsciiAlnum(segment.front()))
            joined += ' ';
        joined += segment;
    }
    return joined;
}

std::string ResultText(const SherpaApi &api, const SherpaOnnxOnlineRecognizer *recognizer,
                       const SherpaOnnxOnlineStream *stream)
{
    const auto *result = api.SherpaOnnxGetOnlineStreamResult(recognizer, stream);
    std::string text = result && result->text ? result->text : "";
    if (result)
        api.SherpaOnnxDestroyOnlineRecognizerResult(result);
    return text;
}

// 一路流式解码：音频可以一次送完（批量），也可以录音时分批送（流式），两者结果一致。
class OnlineDecoder
{
  public:
    explicit OnlineDecoder(const LoadedRecognizer &recognizer)
        : api_(*recognizer.api), recognizer_(recognizer.online), stream_(api_.SherpaOnnxCreateOnlineStream(recognizer_))
    {
    }
    ~OnlineDecoder()
    {
        if (stream_)
            api_.SherpaOnnxDestroyOnlineStream(stream_);
    }
    OnlineDecoder(const OnlineDecoder &) = delete;
    OnlineDecoder &operator=(const OnlineDecoder &) = delete;

    bool Valid() const
    {
        return stream_ != nullptr;
    }

    void Accept(const float *samples, std::size_t count)
    {
        // C API 的采样数是 int32，长录音分片送入。
        constexpr std::size_t kSlice = kSampleRate * 10;
        for (std::size_t offset = 0; offset < count; offset += kSlice)
        {
            const auto slice = static_cast<int32_t>((std::min)(kSlice, count - offset));
            api_.SherpaOnnxOnlineStreamAcceptWaveform(stream_, kSampleRate, samples + offset, slice);
            Decode();
        }
    }

    // 到目前为止识别出的全部文字（已断句的段加上正在识别的这一段）。
    std::string Text() const
    {
        std::vector<std::string> all = segments_;
        if (!current_.empty())
            all.push_back(current_);
        return JoinSegments(all);
    }

    std::string Finish()
    {
        // 尾部补 1.2 秒静音，让最后一块音频越过 encoder 的前瞻再结束输入；松手即停的按住说话没有句末停顿，
        // 补短了会丢最后一两个字。
        const std::vector<float> tail(kSampleRate * 12 / 10, 0.0f);
        api_.SherpaOnnxOnlineStreamAcceptWaveform(stream_, kSampleRate, tail.data(), static_cast<int32_t>(tail.size()));
        api_.SherpaOnnxOnlineStreamInputFinished(stream_);
        Decode();
        if (!current_.empty())
            segments_.push_back(std::move(current_));
        current_.clear();
        return JoinSegments(segments_);
    }

  private:
    void Decode()
    {
        while (api_.SherpaOnnxIsOnlineStreamReady(recognizer_, stream_))
            api_.SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
        current_ = ResultText(api_, recognizer_, stream_);
        if (api_.SherpaOnnxOnlineStreamIsEndpoint(recognizer_, stream_))
        {
            if (!current_.empty())
                segments_.push_back(current_);
            current_.clear();
            api_.SherpaOnnxOnlineStreamReset(recognizer_, stream_);
        }
    }

    const SherpaApi &api_;
    const SherpaOnnxOnlineRecognizer *recognizer_;
    const SherpaOnnxOnlineStream *stream_;
    std::vector<std::string> segments_;
    std::string current_;
};

std::string RecognizeOnline(const LoadedRecognizer &recognizer, const std::vector<float> &samples)
{
    OnlineDecoder decoder(recognizer);
    if (!decoder.Valid())
        return {};
    decoder.Accept(samples.data(), samples.size());
    return decoder.Finish();
}

std::string DecodeOfflineSegment(const LoadedRecognizer &recognizer, const float *samples, int32_t count)
{
    const SherpaApi &api = *recognizer.api;
    const SherpaOnnxOfflineStream *stream = api.SherpaOnnxCreateOfflineStream(recognizer.offline);
    if (!stream)
        return {};
    api.SherpaOnnxAcceptWaveformOffline(stream, kSampleRate, samples, count);
    api.SherpaOnnxDecodeOfflineStream(recognizer.offline, stream);
    const auto *result = api.SherpaOnnxGetOfflineStreamResult(stream);
    std::string text = result && result->text ? result->text : "";
    if (result)
        api.SherpaOnnxDestroyOfflineRecognizerResult(result);
    api.SherpaOnnxDestroyOfflineStream(stream);
    return text;
}

std::string RecognizeOffline(const LoadedRecognizer &recognizer, const std::vector<float> &samples, std::string &error)
{
    const SherpaApi &api = *recognizer.api;
    SherpaOnnxVadModelConfig vad{};
    vad.silero_vad.model = recognizer.vad_model.c_str();
    vad.silero_vad.threshold = 0.5f;
    vad.silero_vad.min_silence_duration = 0.5f;
    vad.silero_vad.min_speech_duration = 0.25f;
    vad.silero_vad.window_size = kVadWindow;
    // Fun-ASR-Nano 的音频与文本共用 512 token 的上下文，超过约 25 秒就什么都不出。
    vad.silero_vad.max_speech_duration = recognizer.model->kind == ModelKind::OfflineFunAsrNano ? 20.0f : 28.0f;
    vad.sample_rate = kSampleRate;
    vad.num_threads = 1;
    vad.provider = "cpu";
    const SherpaOnnxVoiceActivityDetector *detector = api.SherpaOnnxCreateVoiceActivityDetector(&vad, 60.0f);
    if (!detector)
    {
        error = "本地语音的断句模型载入失败，请在设置里删除模型后重新下载";
        return {};
    }
    std::vector<std::string> segments;
    long long decoded_end = 0;
    std::vector<float> padded;
    const auto drain = [&] {
        while (!api.SherpaOnnxVoiceActivityDetectorEmpty(detector))
        {
            const SherpaOnnxSpeechSegment *segment = api.SherpaOnnxVoiceActivityDetectorFront(detector);
            if (segment && segment->n > 0)
            {
                // sherpa 的段首只比概率越过阈值的那一窗早约 26 ms，电平低时句首的字落在段外。整段录音都在手里，
                // 往前补回起音再解码，但不越过上一段的段尾，免得两段重复同一段音频里的字。
                const long long start = segment->start;
                const long long begin = VadSegmentStart(samples.data(), samples.size(), start, decoded_end);
                decoded_end = start + segment->n;
                std::string text;
                if (begin >= start)
                {
                    text = DecodeOfflineSegment(recognizer, segment->samples, segment->n);
                }
                else
                {
                    padded.assign(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                                  samples.begin() + static_cast<std::ptrdiff_t>(start));
                    padded.insert(padded.end(), segment->samples, segment->samples + segment->n);
                    text = DecodeOfflineSegment(recognizer, padded.data(), static_cast<int32_t>(padded.size()));
                }
                if (!text.empty())
                    segments.push_back(std::move(text));
            }
            if (segment)
                api.SherpaOnnxDestroySpeechSegment(segment);
            api.SherpaOnnxVoiceActivityDetectorPop(detector);
        }
    };
    // 检测器按每次调用整体判断，所以一窗一窗地送，长缓冲一次送入会把所有停顿并成一段。
    const auto total = static_cast<int32_t>((std::min<std::size_t>)(samples.size(), INT32_MAX));
    for (int32_t at = 0; at < total; at += kVadWindow)
    {
        const int32_t window = (std::min)(kVadWindow, total - at);
        api.SherpaOnnxVoiceActivityDetectorAcceptWaveform(detector, samples.data() + at, window);
        drain();
    }
    api.SherpaOnnxVoiceActivityDetectorFlush(detector);
    drain();
    api.SherpaOnnxDestroyVoiceActivityDetector(detector);
    return JoinSegments(segments);
}

} // namespace

std::string ReadinessError(const std::string &model_id)
{
    const ModelEntry *model = FindModel(model_id);
    if (!model)
        return "请先在设置的“语音输入”里选择一个本地语音模型";
    if (!ModelFilesInstalled(*model))
        return std::string("本地语音模型“") + model->title + "”尚未下载，请在设置的“语音输入”里下载";
    if (!RuntimeInstalled())
        return "本地识别运行库尚未下载，请在设置的“语音输入”里重新点一次下载";
    if (NeedsVad(*model) && !VadInstalled())
        return "本地语音的断句模型（silero_vad）尚未下载，请在设置的“语音输入”里重新点一次下载";
    return {};
}

RecognitionResult Recognize(const std::vector<float> &samples, const std::string &model_id, const std::string &language)
{
    if (std::string error = ReadinessError(model_id); !error.empty())
        return {{}, error};
    const ModelEntry &model = *FindModel(model_id);
    std::string error;
    const SherpaApi *api = LoadRuntime(error);
    if (!api)
        return {{}, error};
    StartReaper();
    std::shared_ptr<LoadedRecognizer> recognizer;
    try
    {
        recognizer = Acquire(*api, model, language, error);
        if (!recognizer)
            return {{}, error};
        std::string text = model.kind == ModelKind::OnlineTransducer ? RecognizeOnline(*recognizer, samples)
                                                                     : RecognizeOffline(*recognizer, samples, error);
        recognizer->Touch();
        if (!error.empty())
            return {{}, error};
        return {TidyTranscript(text), {}};
    }
    catch (const std::exception &)
    {
        return {{}, "本地语音识别失败"};
    }
}

bool SupportsStreaming(const std::string &model_id)
{
    const ModelEntry *model = FindModel(model_id);
    return model && model->kind == ModelKind::OnlineTransducer;
}

StreamingSession::StreamingSession(std::string model_id, PartialCallback on_partial)
    : model_id_(std::move(model_id)), on_partial_(std::move(on_partial))
{
}

StreamingSession::~StreamingSession()
{
    Stop(true);
}

bool StreamingSession::Start()
{
    if (!SupportsStreaming(model_id_))
        return false;
    if (std::string error = ReadinessError(model_id_); !error.empty())
    {
        error_ = std::move(error);
        return false;
    }
    worker_ = std::thread(&StreamingSession::Run, this);
    return true;
}

void StreamingSession::PushFloatSamples(const float *samples, std::size_t count)
{
    if (!samples || count == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finishing_ || canceled_)
            return;
        pending_.insert(pending_.end(), samples, samples + count);
    }
    cv_.notify_one();
}

std::string StreamingSession::Finish()
{
    Stop(false);
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

std::string StreamingSession::LastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void StreamingSession::Cancel()
{
    Stop(true);
}

void StreamingSession::Stop(bool cancel)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finishing_ = true;
        canceled_ = canceled_ || cancel;
    }
    cv_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

void StreamingSession::Run()
{
    const auto fail = [this](std::string error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::move(error);
    };
    std::string error;
    const SherpaApi *api = LoadRuntime(error);
    if (!api)
        return fail(error);
    StartReaper();
    std::shared_ptr<LoadedRecognizer> recognizer;
    try
    {
        recognizer = Acquire(*api, *FindModel(model_id_), {}, error);
    }
    catch (const std::exception &)
    {
        error = "本地语音识别失败";
    }
    if (!recognizer)
        return fail(error.empty() ? "本地语音识别失败" : error);

    OnlineDecoder decoder(*recognizer);
    if (!decoder.Valid())
        return fail("本地语音识别失败");
    std::string shown;
    std::vector<float> chunk;
    for (;;)
    {
        bool finishing = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !pending_.empty() || finishing_; });
            if (canceled_)
                break;
            chunk.swap(pending_);
            pending_.clear();
            finishing = finishing_;
        }
        if (!chunk.empty())
        {
            decoder.Accept(chunk.data(), chunk.size());
            chunk.clear();
            const std::string text = TidyTranscript(decoder.Text());
            if (text != shown && !text.empty() && on_partial_)
            {
                shown = text;
                on_partial_(text);
            }
        }
        if (finishing)
        {
            // finishing_ 之后不再收音频（PushFloatSamples 拒收），刚才那批就是最后一批。
            std::string text = TidyTranscript(decoder.Finish());
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = std::move(text);
            break;
        }
    }
    recognizer->Touch();
}

std::string TidyTranscript(const std::string &text)
{
    const auto characters = Utf8Characters(text);
    std::string out;
    for (std::size_t i = 0; i < characters.size(); ++i)
    {
        const auto &character = characters[i];
        if (character == " ")
        {
            const bool after_mark = i > 0 && IsCjkPunctuation(characters[i - 1]);
            const bool before_mark = i + 1 < characters.size() && IsCjkPunctuation(characters[i + 1]);
            const bool after_cjk = i > 0 && IsCjkIdeograph(characters[i - 1]) && i + 1 < characters.size() &&
                                   IsCjkIdeograph(characters[i + 1]);
            const bool inside_initialism =
                i > 0 && IsSingleCapital(characters, i - 1) && IsSingleCapital(characters, i + 1);
            const bool leading = out.empty();
            const bool repeated = !out.empty() && out.back() == ' ';
            if (after_mark || before_mark || after_cjk || inside_initialism || leading || repeated)
                continue;
        }
        // X-ASR 离线模型在中文后面也会打半角句末标点（「是星期三.」），在英文句子之间不留空格（「monday.Today」）。
        if (character == "." || character == "?" || character == "!" || character == ",")
        {
            if (i > 0 && IsCjkIdeograph(characters[i - 1]))
            {
                out += character == "." ? "。" : character == "?" ? "？" : character == "!" ? "！" : "，";
                continue;
            }
            const bool between_sentences = i > 0 && characters[i - 1].size() == 1 &&
                                           std::islower(static_cast<unsigned char>(characters[i - 1][0])) &&
                                           i + 1 < characters.size() && characters[i + 1].size() == 1 &&
                                           std::isupper(static_cast<unsigned char>(characters[i + 1][0])) &&
                                           character != ",";
            if (between_sentences)
            {
                out += character;
                out += ' ';
                continue;
            }
        }
        out += character;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

long long VadSegmentStart(const float *audio, std::size_t count, long long vad_start, long long earliest)
{
    const auto audio_end = static_cast<long long>(count);
    const long long lower = (std::max)({earliest, 0LL, vad_start - kOnsetLookback});
    if (lower >= vad_start || vad_start > audio_end)
        return vad_start;
    const auto frame_rms = [&](long long at) {
        double energy = 0.0;
        for (long long i = at; i < at + kOnsetFrame; ++i)
        {
            const double sample = audio[static_cast<std::size_t>(i)];
            energy += sample * sample;
        }
        return static_cast<float>(std::sqrt(energy / static_cast<double>(kOnsetFrame)));
    };
    // 噪声底取整段录音帧能量的第 10 百分位：说话之间的停顿都在里面，比只看段首之前稳。
    std::vector<float> levels;
    levels.reserve(count / static_cast<std::size_t>(kOnsetFrame));
    for (long long at = 0; at + kOnsetFrame <= audio_end; at += kOnsetFrame)
        levels.push_back(frame_rms(at));
    float noise_floor = kOnsetFloor;
    if (!levels.empty())
    {
        const auto nth = levels.begin() + static_cast<std::ptrdiff_t>(levels.size() / 10);
        std::nth_element(levels.begin(), nth, levels.end());
        noise_floor = (std::max)(noise_floor, *nth);
    }
    const float speech = noise_floor * kOnsetRise;
    long long onset = vad_start;
    long long gap = 0;
    for (long long at = vad_start - kOnsetFrame; at >= lower; at -= kOnsetFrame)
    {
        if (frame_rms(at) > speech)
        {
            onset = at;
            gap = 0;
        }
        else if ((gap += kOnsetFrame) > kOnsetGap)
        {
            break;
        }
    }
    return (std::max)(lower, onset - kOnsetPreRoll);
}

} // namespace local_asr
