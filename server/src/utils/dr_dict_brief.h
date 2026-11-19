#pragma once

#include <functional>
#include <string>

// 向 DR Dict 要英文词的简短释义和英美音标（协议见 dr_dict_brief_policy.h）。自己带一条线程和一个只收消息的
// 窗口：请求经它发出，DR Dict 的回复也回到这个窗口。DR Dict 没在运行时什么都不做，不会去启动它：
// 释义和音标只是补充，输入法自带的照常显示。
namespace DrDictBrief
{
struct Answer
{
    std::string word;
    // 这次请求要了什么；回调方据此判断哪一样已经有了答复。
    bool asked_brief = false;
    bool asked_phonetics = false;
    // 摘要；空表示 DR Dict 没查到。
    std::string text;
    // DR Dict 认识音标请求时为真（旧版不认）；uk / us 不带斜杠，查不到时为空。
    bool has_phonetics = false;
    std::string uk;
    std::string us;
};

// 回调在 DrDictBrief 自己的线程上调用。
using Callback = std::function<void(Answer answer)>;

void Start(Callback callback);
void Stop();
// word 是小写英文词，brief / phonetics 至少要一样。只保留最后一次请求：用户移动高亮很快时，前面的不再发。
void Request(const std::string &word, bool brief, bool phonetics);
} // namespace DrDictBrief
