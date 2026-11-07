#pragma once

#include <string>
#include <vector>

namespace CustomTranslation
{
struct Config
{
    std::string endpoint;
    std::string api_key;
    // openai / responses / anthropic / gemini / deeplx
    std::string format = "openai";
    std::string model = "gpt-4o-mini";
    // 自定义提示词，留空用内置释义提示；支持 {lang} 占位符。
    std::string prompt;
    // 整句模式：输入是一句完整的话，不是词条。提示词、输出长度和超时都按句子放宽，
    // 译文也不再压成「最多两个义项」。
    bool sentence = false;
    // 译后校对：只对自定义服务生效，由 [custom_translation].review 控制。
    bool review = true;
    // 思考强度：空 = 不干预，disabled = 显式关掉，low/high/max = 开启。
    // 中转站的模型常常默认思考，思考 token 会吃满 max_tokens，正文变空。
    std::string thinking;
};

bool IsSupportedEndpoint(const std::string &endpoint);
std::string ParseTranslationResponse(const std::string &response);
// 最近一次失败的原因（HTTP 状态码 + 响应片段），供「测试配置」展示。
std::string LastError();

// openai 格式走 Chat Completions，responses/anthropic/gemini 走各自的原生协议；
// 都是一条文本一个请求，空字符串表示这一条失败。
std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);

// 译后校对：把「原文|译文」成批交给同一模型复核，只回需要改的行。
// 返回与 texts 等长，空串表示这条不用改。sentence 决定超时档位；
// deeplx 没有聊天接口，直接返回全空。
std::vector<std::string> ReviewTranslations(const Config &config, const std::vector<std::string> &texts,
                                            const std::vector<std::string> &translations, const std::string &source,
                                            const std::string &target, bool sentence);
} // namespace CustomTranslation
