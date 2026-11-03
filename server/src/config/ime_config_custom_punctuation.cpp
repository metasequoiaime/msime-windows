// 自定义标点配置：总开关、映射的读写，以及发给 TSF 的载荷。
#include "config/ime_config_internal.h"

#include "engine/contracts/custom_punctuation.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace ime_config_detail;

namespace
{
namespace cp = FanyImeCustomPunctuation;

// 候选窗线程重载配置时写，管道线程在 TSF 连上时读（组补发的载荷），所以两样都在锁里读写。
std::mutex g_mutex;
bool g_enabled = false;
std::vector<std::string> g_map;

std::vector<std::string> FormatEntries(const std::vector<cp::Entry> &entries)
{
    std::vector<std::string> elements;
    for (const cp::Entry &entry : entries)
        elements.push_back(cp::FormatEntry(entry));
    return elements;
}
} // namespace

namespace ime_config_detail
{
void StoreCustomPunctuationConfig(bool enabled, const std::vector<std::string> &elements)
{
    std::vector<std::string> normalized = FormatEntries(cp::NormalizeEntries(elements));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = enabled;
    g_map = std::move(normalized);
}
} // namespace ime_config_detail

bool GetConfiguredCustomPunctuationEnabled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enabled;
}

bool SetConfiguredCustomPunctuationEnabled(bool enabled)
{
    if (!WriteConfiguredValue("input", "custom_punctuation", enabled ? "true" : "false"))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = enabled;
    return true;
}

std::vector<std::string> GetConfiguredCustomPunctuationMap()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_map;
}

bool SetConfiguredCustomPunctuationMap(const std::vector<std::string> &entries)
{
    bool allValid = false;
    std::vector<std::string> normalized = FormatEntries(cp::NormalizeEntries(entries, &allValid));
    if (!allValid)
        return false;
    std::string value = "[";
    for (size_t i = 0; i < normalized.size(); ++i)
    {
        if (i != 0)
            value += ", ";
        value += EscapeTomlBasicString(normalized[i]);
    }
    value += "]";
    if (!WriteConfiguredValue("input", "custom_punctuation_map", value))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_map = std::move(normalized);
    return true;
}

std::wstring FormatCustomPunctuationWorkerPayload()
{
    bool enabled = false;
    std::vector<std::string> map;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        enabled = g_enabled;
        map = g_map;
    }
    if (!enabled)
        return {};
    return cp::FormatPayload(cp::BuildTable(cp::NormalizeEntries(map)));
}
