#pragma once

// 接口地址的端点判定。刻意不带 <Windows.h> 与 libcurl：chat_request_params.h 这类纯 header 的
// 请求构造逻辑要按端点选字段，若从 network_proxy.h 取这个函数，会把 Win32 的 min/max 宏带进
// 每个包含请求头文件的翻译单元，最终逼着每个测试目标都补 NOMINMAX。

#include <string_view>

namespace NetworkProxy
{

// 地址是否指向本机回环：host 精确等于 localhost、127.0.0.1 或 ::1。不做后缀匹配
// （localhost.evil.com 判否），也不含私网网段：局域网里那台谁都能访问的 Ollama 仍要凭据。
bool IsLoopbackEndpoint(std::string_view endpoint);

} // namespace NetworkProxy
