#pragma once

// 本地（离线）语音识别：经 sherpa-onnx 的 C API 在本机识别，不联网。实现移植自 ../msime 的
// shared/voice/LocalAsr.cpp，只保留 Server 批量识别要用的部分（没有热词、没有逐字回显）。
//
// 运行库不链接：第一次识别时从 <DataDir>/models/voice-asr/runtime/ 用 LoadLibraryEx 载入，载入前
// 按 local_asr_catalog.h 钉死的大小与 SHA256 逐个校验，校验期间以拒绝写入/删除的共享方式占住文件，
// 校验与载入之间换不掉。数据目录对用户可写，而 Server 带 uiAccess，不能载入来路不明的 DLL。
// 运行库一旦载入就不再卸载（缓存的识别器指向它，onnxruntime 也不支持卸载）。
//
// 模型载入后留给下一次听写复用，闲置几分钟后由后台线程释放；同一时刻只驻留一个模型。

#include <cstddef>
#include <string>
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

// 识别一整段录音：单声道 16 kHz float PCM。language 取 voice_input.language（zh-cn | en | auto）。
// 阻塞，须在工作线程调用。
RecognitionResult Recognize(const std::vector<float> &samples, const std::string &model_id,
                            const std::string &language);

// 去掉 sherpa 输出里多余的空格、把中文后的半角句末标点换成全角。纯函数，供测试。
std::string TidyTranscript(const std::string &text);

// VAD 段首往前补回起音的位置（见 local_asr.cpp）。纯函数，供测试。
long long VadSegmentStart(const float *audio, std::size_t count, long long vad_start, long long earliest);

} // namespace local_asr
