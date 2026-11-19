#pragma once

#include <functional>
#include <string>

// 向 DR Dict 要英文词的简短释义（协议见 dr_dict_brief_policy.h）。自己带一条线程和一个只收消息的
// 窗口：请求经它发出，DR Dict 的回复也回到这个窗口。DR Dict 没在运行时什么都不做，不会去启动它：
// 释义只是补充，输入法自带的释义照常显示。
namespace DrDictBrief
{
// 回调在 DrDictBrief 自己的线程上调用；text 为空表示 DR Dict 没查到。
using Callback = std::function<void(std::string word, std::string text)>;

void Start(Callback callback);
void Stop();
// word 是小写英文词。只保留最后一次请求：用户移动高亮很快时，前面的不再发。
void Request(const std::string &word);
} // namespace DrDictBrief
