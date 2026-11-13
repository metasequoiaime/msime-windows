#pragma once

// 本地（离线）语音识别：经 sherpa-onnx 的 C API 在本机识别，不联网。实现移植自 ../msime 的
// shared/voice/LocalAsr.cpp（没有热词）。流式模型录音时边收边解码，识别中的文字经 StreamingSession
// 的回调实时送出；整句模型录完才整段识别。
//
// 运行库不链接：第一次识别时从 <DataDir>/models/voice-asr/runtime/ 用 LoadLibraryEx 载入，载入前
// 按 local_asr_catalog.h 钉死的大小与 SHA256 逐个校验，校验期间以拒绝写入/删除的共享方式占住文件，
// 校验与载入之间换不掉。数据目录对用户可写，而 Server 带 uiAccess，不能载入来路不明的 DLL。
// 运行库一旦载入就不再卸载（缓存的识别器指向它，onnxruntime 也不支持卸载）。
//
// 模型载入后留给下一次听写复用，闲置几分钟后由后台线程释放；同一时刻只驻留一个模型。

#include "streaming_asr.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace local_asr
{

struct RecognitionResult
{
    std::string text;
    std::string error; // 非空表示失败，是给用户与诊断日志看的中文原因
};

// 不载入任何东西，只看磁盘：所选模型及其依赖是否都已下载。返回空串表示就绪。
std::string ReadinessError(const std::string &model_id);

// 所选模型能否边录边出字。流式模型逐字出；SenseVoice 与中英整句是伪流式：VAD 断出一句就识别定稿一句，
// 正在说的这句每半秒重算一次草稿。Fun-ASR-Nano 解码太慢，反复重算会拖住整机，仍录完再识别。只看目录，不载入。
bool SupportsStreaming(const std::string &model_id);

// 边录边出字的一次听写（SupportsStreaming 的模型）。Start 立即返回：运行库与模型在工作线程上载入，
// 载入期间送来的音频先排队，载入后补上，所以冷启动只会让第一段文字晚一点出来，不会丢音频。
// 识别中的文字（已整理过）每变一次就在工作线程上回调一次。
class StreamingSession : public StreamingAsr
{
  public:
    using PartialCallback = std::function<void(const std::string &)>;

    // language 取 voice_input.language，只有 SenseVoice 用得上。
    StreamingSession(std::string model_id, std::string language, PartialCallback on_partial);
    ~StreamingSession() override;

    StreamingSession(const StreamingSession &) = delete;
    StreamingSession &operator=(const StreamingSession &) = delete;

    bool Start() override;
    void PushFloatSamples(const float *samples, std::size_t count) override;
    std::string Finish() override;
    std::string LastError() const override;
    void Cancel() override;

  private:
    void Run();
    // 取走排队的音频；返回 false 表示已取消。finishing 为真表示这是最后一批。
    bool Take(std::vector<float> &chunk, bool &finishing);
    bool HasPending() const;
    void Publish(const std::string &text, std::string &shown);
    void Stop(bool cancel);

    std::string model_id_;
    std::string language_;
    PartialCallback on_partial_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<float> pending_;
    bool finishing_ = false;
    bool canceled_ = false;
    std::thread worker_;
    std::string result_;
    std::string error_;
};

// 识别一整段录音：单声道 16 kHz float PCM。language 取 voice_input.language（zh-cn | en | auto）。
// 阻塞，须在工作线程调用。
RecognitionResult Recognize(const std::vector<float> &samples, const std::string &model_id,
                            const std::string &language);

// 去掉 sherpa 输出里多余的空格、把中文后的半角句末标点换成全角。纯函数，供测试。
std::string TidyTranscript(const std::string &text);

// VAD 段首往前补回起音的位置（见 local_asr.cpp）。纯函数，供测试。
long long VadSegmentStart(const float *audio, std::size_t count, long long vad_start, long long earliest);

} // namespace local_asr
