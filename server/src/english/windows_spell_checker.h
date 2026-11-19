#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Windows 自带拼写检查（ISpellChecker，en-US）的薄封装，给英文候选模式做纠错补充。
// 对应 hallelujahIM 里 macOS 的 NSSpellChecker。系统没装英文拼写检查时 available() 为假，
// suggest() 返回空，调用方只剩 phonex 纠错。
//
// 只能在一个线程上用：构造、调用、析构都在英文候选的后台线程里，那条线程自己负责
// CoInitializeEx。
class WindowsSpellChecker
{
  public:
    WindowsSpellChecker();
    ~WindowsSpellChecker();

    WindowsSpellChecker(const WindowsSpellChecker &) = delete;
    WindowsSpellChecker &operator=(const WindowsSpellChecker &) = delete;

    bool available() const;
    // 只返回由 ASCII 字母、撇号和连字符组成的单个词，不含 word 本身（大小写不敏感）。
    std::vector<std::string> suggest(const std::string &word, std::size_t limit);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
