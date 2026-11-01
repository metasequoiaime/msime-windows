#pragma once

#include "cloud/ai_translation.h"
#include "cloud/aliyun_mt.h"
#include "cloud/custom_translation.h"
#include "cloud/niutrans_translation.h"
#include "cloud/tencent_tmt.h"
#include "config/ime_config.h"
#include "translate/translate_types.h"
#include <optional>
#include <string>

namespace Translate
{
// 解析后的运行时配置：只有当前生效的那一家被填满，其余保持默认。
struct RuntimeConfig
{
    Vendor vendor = Vendor::Tencent;
    TencentTmt::Credentials tencent;
    AliyunMt::Credentials aliyun;
    CustomTranslation::Config custom;
    NiuTransTranslation::Config niutrans;
    AiTranslation::Config ai;
    std::string target_language;
};

// [translation].provider 单一决定当前提供方，服务端与页面不再各写一份优先级。
Vendor ActiveVendor(const TranslationConfig &config);
// 缓存域：同一地址换协议、换模型或换语种，旧译文都不该继续命中。
std::string VendorScope(const TranslationConfig &config);
std::string TargetLanguage(const TranslationConfig &config);
// 凭据不全或地址非法时返回 nullopt，工作线程据此跳过这一轮。
std::optional<RuntimeConfig> Resolve(const TranslationConfig &config);
} // namespace Translate
