#pragma once

#include <cstddef>
#include <string>

// 录音期间边收音频边识别的会话：豆包流式接口和本地流式模型都实现它，语音服务据此统一处理
// 「识别中的文字实时上屏（inline 预编辑）或显示在提示条上」，松手后再取最终结果。
//
// 调用约定：Start 成功后，录音回调线程反复调 PushFloatSamples（必须很快返回）；结束时调一次 Finish
// 取最终文本，或调 Cancel 放弃。识别中的文字由实现自己的回调送出，回调可能在实现的工作线程上触发。
class StreamingAsr
{
  public:
    virtual ~StreamingAsr() = default;

    virtual bool Start() = 0;
    // 单声道 16 kHz float PCM。
    virtual void PushFloatSamples(const float *samples, std::size_t count) = 0;
    // 阻塞到最终结果出来；失败时返回空串，原因见 LastError。
    virtual std::string Finish() = 0;
    virtual std::string LastError() const = 0;
    virtual void Cancel() = 0;
};
