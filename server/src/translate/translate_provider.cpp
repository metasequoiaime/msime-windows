#include "translate/translate_provider.h"

#include "cloud/ai_translation.h"
#include "cloud/aliyun_mt.h"
#include "cloud/custom_translation.h"
#include "cloud/niutrans_translation.h"
#include "cloud/tencent_tmt.h"

#include <algorithm>
#include <future>

namespace Translate
{
Outcome TranslateWithProvider(const RuntimeConfig &config, const Batch &batch)
{
    Outcome outcome;
    switch (config.vendor)
    {
    case Vendor::Ai:
        outcome.translations = AiTranslation::TextTranslateBatch(config.ai, batch.texts, batch.source, batch.target);
        break;
    case Vendor::Aliyun:
        outcome.translations = AliyunMt::TextTranslateBatch(config.aliyun, batch.texts, batch.source, batch.target);
        break;
    case Vendor::NiuTrans:
        outcome.translations =
            NiuTransTranslation::TextTranslateBatch(config.niutrans, batch.texts, batch.source, batch.target);
        break;
    case Vendor::Custom: {
        // 整句标志只对自定义服务有意义：它决定超时、输出上限和提示词。
        CustomTranslation::Config custom = config.custom;
        custom.sentence = batch.sentence;
        // 一条一条发时十个候选要十秒，看着就像没翻译：这里并发发出，最多八路。
        constexpr size_t kParallel = 8;
        outcome.translations.assign(batch.texts.size(), std::string{});
        for (size_t begin = 0; begin < batch.texts.size(); begin += kParallel)
        {
            const size_t end = std::min(begin + kParallel, batch.texts.size());
            std::vector<std::future<std::vector<std::string>>> futures;
            futures.reserve(end - begin);
            for (size_t i = begin; i < end; ++i)
            {
                const std::string text = batch.texts[i];
                const std::string source = batch.source;
                const std::string target = batch.target;
                futures.push_back(std::async(std::launch::async, [custom, text, source, target]() {
                    return CustomTranslation::TextTranslateBatch(custom, {text}, source, target);
                }));
            }
            for (size_t i = begin; i < end; ++i)
            {
                auto part = futures[i - begin].get();
                if (!part.empty())
                    outcome.translations[i] = std::move(part.front());
            }
        }
        break;
    }
    case Vendor::Tencent:
    default:
        outcome.translations = TencentTmt::TextTranslateBatch(config.tencent, batch.texts, batch.source, batch.target);
        break;
    }
    return outcome;
}
} // namespace Translate
