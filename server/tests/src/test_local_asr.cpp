// 本地语音识别（voice-input/local_asr.*）的独立测试：目录一致性、磁盘就绪判断、转写整理与 VAD 段首补偿。
// 不联网、不开麦、不载入运行库。
//
// 可选的真机识别：设置 MSIME_LOCAL_ASR_TEST_DATA_DIR（已装好运行库与模型的数据目录）、
// MSIME_LOCAL_ASR_TEST_MODEL（模型 id）与 MSIME_LOCAL_ASR_TEST_WAV（16 kHz 单声道 16 位 PCM WAV），
// 会载入真实的 sherpa-onnx 识别这段录音并打印结果。CI 不设这些变量，这一段跳过。
#include "voice-input/local_asr.h"
#include "voice-input/local_asr_catalog.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            throw std::runtime_error("CHECK failed: " #expr);                                                          \
    } while (false)

bool IsLowerHex64(const std::string &value)
{
    if (value.size() != 64)
        return false;
    for (const char c : value)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

void TestCatalog()
{
    std::set<std::string> ids;
    for (const local_asr::ModelEntry &model : local_asr::Catalog())
    {
        CHECK(ids.insert(model.id).second);
        CHECK(IsLowerHex64(model.archive.sha256));
        CHECK(model.archive.size > 0);
        CHECK(!model.files.empty());
        CHECK(std::string(model.archive.name).rfind(model.root, 0) == 0);
        CHECK(local_asr::SourceUrl(model.archive).rfind("https://github.com/", 0) == 0);
        CHECK(local_asr::FindModel(model.id) == &model);
    }
    CHECK(ids.size() == 4);
    CHECK(!local_asr::NeedsVad(*local_asr::FindModel("x-asr-zh-en-streaming")));
    CHECK(local_asr::NeedsVad(*local_asr::FindModel("sense-voice-small")));
    CHECK(local_asr::FindModel("missing") == nullptr);
    CHECK(IsLowerHex64(local_asr::RuntimeArchive().sha256));
    CHECK(IsLowerHex64(local_asr::VadModel().sha256));
    // sherpa-onnx-c-api.dll 必须最后载入：它的导入表要解析到前面已载入的 onnxruntime。
    CHECK(std::string(local_asr::RuntimeLibraries().back().name) == "sherpa-onnx-c-api.dll");
    for (const local_asr::RuntimeLibrary &library : local_asr::RuntimeLibraries())
    {
        CHECK(IsLowerHex64(library.sha256));
        CHECK(library.size > 0);
    }
}

struct DataDir
{
    explicit DataDir(const std::filesystem::path &path) : path(path)
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", path.c_str());
    }
    ~DataDir()
    {
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", L"");
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

void Touch(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    std::ofstream(directory / local_asr::kInstalledMarker) << "x\n";
}

void TestReadiness()
{
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    DataDir data(std::filesystem::path(temp) / L"msime-local-asr-test");
    CHECK(local_asr::Root() == data.path / "models" / "voice-asr");
    CHECK(!local_asr::ReadinessError("").empty());
    CHECK(!local_asr::ReadinessError("sense-voice-small").empty());

    const local_asr::ModelEntry &streaming = *local_asr::FindModel("x-asr-zh-en-streaming");
    const local_asr::ModelEntry &sense = *local_asr::FindModel("sense-voice-small");
    Touch(local_asr::ModelDirectory(streaming));
    CHECK(!local_asr::ReadinessError(streaming.id).empty()); // 缺运行库
    Touch(local_asr::RuntimeDirectory());
    CHECK(local_asr::ReadinessError(streaming.id).empty()); // 流式模型不需要 VAD
    CHECK(local_asr::ModelReady(streaming));

    Touch(local_asr::ModelDirectory(sense));
    CHECK(!local_asr::ReadinessError(sense.id).empty()); // 整句模型缺 VAD
    Touch(local_asr::VadDirectory());
    CHECK(local_asr::ReadinessError(sense.id).empty());

    // 运行库目录有标记但 DLL 不对：识别要报错，而不是载入来路不明的文件。
    std::ofstream(local_asr::RuntimeDirectory() / "onnxruntime_providers_shared.dll") << "not a dll";
    const local_asr::RecognitionResult result =
        local_asr::Recognize(std::vector<float>(16000, 0.0f), sense.id, "zh-cn");
    CHECK(result.text.empty());
    CHECK(!result.error.empty());
}

void TestTidy()
{
    CHECK(local_asr::TidyTranscript("今天 是 星期三.") == "今天是星期三。");
    CHECK(local_asr::TidyTranscript("你好 ， 世界") == "你好，世界");
    CHECK(local_asr::TidyTranscript("hello world") == "hello world");
    CHECK(local_asr::TidyTranscript("it is monday.Today") == "it is monday. Today");
    CHECK(local_asr::TidyTranscript("用 A I 写代码") == "用 AI 写代码");
    CHECK(local_asr::TidyTranscript("  spaced  out  ") == "spaced out");
    CHECK(local_asr::TidyTranscript("").empty());
}

void TestVadSegmentStart()
{
    constexpr int rate = 16000;
    // 2 秒近乎静音，再 1 秒较响的音频：VAD 报的段首比真实起音晚 0.4 秒。
    std::vector<float> audio(rate * 3, 0.0005f);
    for (int i = rate * 2; i < rate * 3; ++i)
        audio[i] = 0.2f * std::sin(static_cast<float>(i) * 0.05f);
    const long long vad_start = rate * 2 + rate * 4 / 10;
    const long long begin = local_asr::VadSegmentStart(audio.data(), audio.size(), vad_start, 0);
    // 补回到起音之前（起音 2.0 秒，再留 0.3 秒）。
    CHECK(begin <= rate * 2);
    CHECK(begin >= rate * 2 - rate * 4 / 10);
    // 不越过上一段的段尾。
    CHECK(local_asr::VadSegmentStart(audio.data(), audio.size(), vad_start, rate * 2 + rate / 10) >=
          rate * 2 + rate / 10);
    // 段首之前全是静音时不往前补。
    std::vector<float> silent(rate * 3, 0.0005f);
    CHECK(local_asr::VadSegmentStart(silent.data(), silent.size(), rate * 2, 0) >= rate * 2 - rate * 3 / 10);
}

std::vector<float> ReadWav16(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    // 找 "data" 块。
    for (size_t i = 12; i + 8 <= bytes.size();)
    {
        const std::string id(bytes.data() + i, 4);
        const uint32_t size = *reinterpret_cast<const uint32_t *>(bytes.data() + i + 4);
        if (id == "data")
        {
            std::vector<float> samples;
            const size_t count = (std::min<size_t>)(size, bytes.size() - i - 8) / 2;
            for (size_t n = 0; n < count; ++n)
                samples.push_back(*reinterpret_cast<const int16_t *>(bytes.data() + i + 8 + n * 2) / 32768.0f);
            return samples;
        }
        i += 8 + size + (size & 1);
    }
    return {};
}

int TestRealModel()
{
    // 宽字符读取：路径可能含中文，窄 getenv 会按 ANSI 代码页丢字。
    const wchar_t *data = _wgetenv(L"MSIME_LOCAL_ASR_TEST_DATA_DIR");
    const char *model = std::getenv("MSIME_LOCAL_ASR_TEST_MODEL");
    const wchar_t *wav = _wgetenv(L"MSIME_LOCAL_ASR_TEST_WAV");
    if (!data || !model || !wav || !*data || !*model || !*wav)
        return 0;
    _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", data);
    const std::vector<float> samples = ReadWav16(std::filesystem::path(wav));
    CHECK(!samples.empty());
    const auto started = GetTickCount64();
    const local_asr::RecognitionResult first = local_asr::Recognize(samples, model, "zh-cn");
    const auto loaded = GetTickCount64();
    const local_asr::RecognitionResult second = local_asr::Recognize(samples, model, "zh-cn");
    const auto finished = GetTickCount64();
    std::cout << "model " << model << ": error=\"" << first.error << "\" text=\"" << first.text << "\" (cold "
              << loaded - started << " ms, warm " << finished - loaded << " ms)\n";
    CHECK(first.error.empty());
    CHECK(!first.text.empty());
    CHECK(second.text == first.text);
    // 纯静音不出字，也不报错。
    const local_asr::RecognitionResult silence =
        local_asr::Recognize(std::vector<float>(16000 * 2, 0.0f), model, "zh-cn");
    CHECK(silence.error.empty());
    CHECK(silence.text.empty());
    return 1;
}
} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        TestCatalog();
        TestReadiness();
        TestTidy();
        TestVadSegmentStart();
        if (TestRealModel())
            std::cout << "real model recognition passed\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }
    std::cout << "local_asr tests passed\n";
    return 0;
}
