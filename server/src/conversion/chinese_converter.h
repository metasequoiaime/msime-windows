#pragma once

#include "conversion/traditional_forms.h"
#include "ipc/traditional_region_policy.h"

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ChineseConverter
{
// Converts simplified text to traditional. Returns the original text when the conversion
// resources cannot be loaded or the conversion fails; only an allocation failure
// (std::bad_alloc) can propagate.
std::string ToTraditional(const std::string &text);

/**
 * 给出 simplified 的标准、台湾、香港三种繁体写法。字典首次使用时载入。
 * 某一步的配置或字典载入失败时，那一步原样返回它的输入，不抛异常。
 */
TraditionalForms ToTraditionalForms(const std::string &simplified);

/**
 * text 是不是简体写法：每个字在 OpenCC 繁转简字表（TSCharacters）里都没有条目、或繁转简结果是它自己时为真，
 * 有一个字转成别的字时为假。空串为真。字表载入失败时为真，不抛异常。
 */
bool IsSimplifiedSpelling(const std::string &text);

/**
 * 载入 ToTraditional、ToTraditionalForms、IsSimplifiedSpelling 以及 opencc_character_tables.h 里各查询
 * 用到的全部配置与字典；
 * 重复调用和并发调用都只载入一次。
 */
void Preload();

/**
 * 在一条后台线程里跑一次 work。Start 起线程后立即返回；线程在跑或已经跑完时，之后的 Start 什么都不做。
 * Join 等 work 返回；没起过线程或已经等过时立即返回。析构时还没等过就等。
 */
class BackgroundTask
{
  public:
    explicit BackgroundTask(std::function<void()> work);
    BackgroundTask(const BackgroundTask &) = delete;
    BackgroundTask &operator=(const BackgroundTask &) = delete;
    ~BackgroundTask();

    void Start();
    void Join();

  private:
    std::function<void()> work_;
    std::mutex mutex_;
    std::thread thread_;
    bool started_ = false;
};

/**
 * 在一条后台线程里做一次 Preload 并读取 UserTraditionalRegion，调用方立即返回。
 * 进程内只起一次线程：线程在跑或已经跑完时，之后的调用什么都不做。进程正常退出前调用
 * JoinBackgroundPreload 等它结束。
 */
void PreloadInBackground();

/** 等 PreloadInBackground 起的线程结束；没起过线程或已经等过时立即返回。 */
void JoinBackgroundPreload();

/** 判定繁体写法地区要读的两项系统设置，各由一个读取函数给出；读不到时返回空串。 */
struct RegionSettingReaders
{
    // 系统「所在地」的两字母地区码。
    std::function<std::string()> location;
    // 系统「区域格式」的区域名。
    std::function<std::string()> locale;
};

/**
 * 读本机设置的那一对读取函数：所在地取 GetUserDefaultGeoName（Windows 10 1709 之前没有这个接口，
 * 恒为空），区域格式取 GetUserDefaultLocaleName。非 ASCII 字符读成 '?'。
 */
RegionSettingReaders SystemRegionSettingReaders();

/**
 * 第一次调用 Get 时用 readers 各读一次两项设置，按 FanyImeIpc::ResolveTraditionalRegion 判定地区；
 * 之后的调用返回同一结果，不再读取。可以从多个线程同时调用 Get。没给的读取函数当作读不到。
 */
class CachedTraditionalRegion
{
  public:
    explicit CachedTraditionalRegion(RegionSettingReaders readers);
    FanyImeIpc::TraditionalRegion Get();

  private:
    RegionSettingReaders readers_;
    std::once_flag once_;
    FanyImeIpc::TraditionalRegion region_ = FanyImeIpc::kFallbackTraditionalRegion;
};

/**
 * 按系统「所在地」与「区域格式」判定的繁体写法地区：进程内一个用 SystemRegionSettingReaders 构造的
 * CachedTraditionalRegion，第一次调用时读取系统设置，之后返回同一结果。
 * 系统没有「所在地」接口（Windows 10 1709 之前）或读取失败时，当作读不到，改看区域格式。
 */
FanyImeIpc::TraditionalRegion UserTraditionalRegion();

// Converts traditional text to simplified. It is not an identity on simplified text: a few
// simplified words are rewritten as well (乾清宫 becomes 干清宫). Returns the original text when
// the conversion resources cannot be loaded or the conversion fails; only an allocation failure
// (std::bad_alloc) can propagate.
std::string ToSimplified(const std::string &text);
} // namespace ChineseConverter
