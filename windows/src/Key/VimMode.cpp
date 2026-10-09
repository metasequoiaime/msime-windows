#include "VimMode.h"
#include <algorithm>
#include <utility>
#include <vector>

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

std::string TrimAscii(const std::string &text)
{
    const size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos)
        return {};
    const size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

// A flat TOML array of single-line strings, which may span several lines and
// carry comments. Anything else (numbers, nested arrays, unsupported escapes)
// rejects the whole list so a typo cannot half-enable the feature.
bool ParseStringArray(const std::string &text, size_t pos, std::vector<std::string> &items)
{
    if (pos >= text.size() || text[pos] != '[')
        return false;
    ++pos;
    bool expectItem = true;
    while (pos < text.size())
    {
        const char ch = text[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            ++pos;
            continue;
        }
        if (ch == '#')
        {
            pos = text.find('\n', pos);
            if (pos == std::string::npos)
                return false;
            continue;
        }
        if (ch == ']')
            return true;
        if (ch == ',')
        {
            if (expectItem)
                return false;
            expectItem = true;
            ++pos;
            continue;
        }
        if (!expectItem || (ch != '"' && ch != '\''))
            return false;

        const char quote = ch;
        std::string item;
        bool closed = false;
        ++pos;
        while (pos < text.size() && text[pos] != '\n')
        {
            const char c = text[pos++];
            if (c == quote)
            {
                closed = true;
                break;
            }
            if (quote == '"' && c == '\\')
            {
                if (pos >= text.size() || (text[pos] != '\\' && text[pos] != '"'))
                    return false;
                item.push_back(text[pos++]);
                continue;
            }
            item.push_back(c);
        }
        if (!closed)
            return false;
        items.push_back(std::move(item));
        expectItem = false;
    }
    return false;
}
} // namespace

bool VimMode::EnabledForProcess(const std::string &configToml, const std::string &processName)
{
    if (processName.empty())
        return false;

    size_t lineStart = configToml.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    bool inInputSection = false;
    while (lineStart < configToml.size())
    {
        size_t lineEnd = configToml.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = configToml.size();
        const std::string rawLine = configToml.substr(lineStart, lineEnd - lineStart);
        const size_t rawLineStart = lineStart;
        lineStart = lineEnd + 1;

        const std::string line = TrimAscii(rawLine.substr(0, rawLine.find('#')));
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = line == "[input]";
            continue;
        }
        if (!inInputSection)
            continue;
        const size_t eq = rawLine.find('=');
        if (eq == std::string::npos || TrimAscii(rawLine.substr(0, eq)) != "vim_mode_apps")
            continue;

        const size_t valueStart = configToml.find_first_not_of(" \t", rawLineStart + eq + 1);
        std::vector<std::string> apps;
        if (valueStart == std::string::npos || !ParseStringArray(configToml, valueStart, apps))
            return false;
        const std::string process = FoldExecutableName(processName);
        return std::any_of(apps.begin(), apps.end(),
                           [&](const std::string &app) { return FoldExecutableName(app) == process; });
    }
    return false;
}
