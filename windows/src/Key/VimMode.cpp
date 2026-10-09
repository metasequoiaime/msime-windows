#include "VimMode.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>

namespace
{
std::string FoldExecutableName(std::string name)
{
    // Executable basenames normally use ASCII; leave UTF-8 bytes unchanged.
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
    });
    return name;
}
} // namespace

bool VimMode::EnabledForProcess(const std::string &yaml, const std::string &processName)
{
    if (processName.empty())
        return false;
    try
    {
        const YAML::Node root = YAML::Load(yaml);
        if (!root.IsMap())
            return false;
        const YAML::Node apps = root["app_options"];
        if (!apps.IsMap())
            return false;
        const std::string process = FoldExecutableName(processName);
        for (const auto &entry : apps)
        {
            if (!entry.first.IsScalar() || FoldExecutableName(entry.first.as<std::string>()) != process)
                continue;
            const YAML::Node options = entry.second;
            if (!options.IsMap())
                return false;
            const YAML::Node enabled = options["vim_mode"];
            return enabled.IsScalar() && enabled.as<bool>();
        }
    }
    catch (const YAML::Exception &)
    {
        // An optional config must never break the host application's input path.
    }
    return false;
}
