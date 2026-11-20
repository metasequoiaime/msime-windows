// config.toml 的文本级原语：保留格式的键值替换与插入、跨行值定位、赋值遍历，以及读文件、原子写文件与解析校验。
#include "config/ime_config_internal.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <system_error>

using namespace ime_config_detail;

namespace
{
std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
    {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

size_t FindTomlValueEnd(const std::string &line, size_t value_begin)
{
    if (value_begin >= line.size())
    {
        return value_begin;
    }

    const char quote = line[value_begin];
    if (quote == '"' || quote == '\'')
    {
        bool escaped = false;
        for (size_t i = value_begin + 1; i < line.size(); ++i)
        {
            if (quote == '"' && line[i] == '\\' && !escaped)
            {
                escaped = true;
                continue;
            }
            if (line[i] == quote && !escaped)
            {
                return i + 1;
            }
            escaped = false;
        }
        return line.size();
    }

    size_t end = value_begin;
    while (end < line.size() && line[end] != '#' && line[end] != '\r')
    {
        ++end;
    }
    while (end > value_begin && (line[end - 1] == ' ' || line[end - 1] == '\t'))
    {
        --end;
    }
    return end;
}

size_t FindTomlValueEndInText(const std::string &text, size_t value_begin);

void ClearReadOnlyAttribute(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY) == 0)
    {
        return;
    }
    SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
}

// 与 FindTomlValueEnd 相同，但值可以跨行（ai_assistant.prompt 用的是 """ 多行字符串）。
size_t FindTomlValueEndInText(const std::string &text, size_t value_begin)
{
    if (value_begin < text.size() && text[value_begin] == '[')
    {
        int depth = 1;
        for (size_t i = value_begin + 1; i < text.size(); ++i)
        {
            if (text[i] == '"' || text[i] == '\'')
                i = FindTomlValueEndInText(text, i) - 1;
            else if (text[i] == '#')
            {
                i = text.find('\n', i);
                if (i == std::string::npos)
                    return text.size();
            }
            else if (text[i] == '[')
                ++depth;
            else if (text[i] == ']' && --depth == 0)
                return i + 1;
        }
        return text.size();
    }
    for (const char *delimiter : {"\"\"\"", "'''"})
    {
        if (value_begin + 3 > text.size() || text.compare(value_begin, 3, delimiter) != 0)
        {
            continue;
        }
        const bool escapable = delimiter[0] == '"';
        size_t i = value_begin + 3;
        while (i + 3 <= text.size())
        {
            if (escapable && text[i] == '\\')
            {
                i += 2;
                continue;
            }
            if (text.compare(i, 3, delimiter) == 0)
            {
                return i + 3;
            }
            ++i;
        }
        return text.size();
    }

    const size_t newline = text.find('\n', value_begin);
    const size_t line_end = newline == std::string::npos ? text.size() : newline;
    return value_begin + FindTomlValueEnd(text.substr(value_begin, line_end - value_begin), 0);
}
} // namespace

namespace ime_config_detail
{
std::string EscapeTomlBasicString(const std::string &value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (char ch : value)
    {
        if (ch == '\n')
        {
            result += "\\n";
            continue;
        }
        if (ch == '\r')
        {
            result += "\\r";
            continue;
        }
        if (ch == '\t')
        {
            result += "\\t";
            continue;
        }
        if (ch == '\\' || ch == '"')
        {
            result.push_back('\\');
        }
        result.push_back(ch);
    }
    result.push_back('"');
    return result;
}

bool ReplaceTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                          const std::string &replacement)
{
    bool in_section = false;
    size_t line_begin = 0;
    while (line_begin <= text.size())
    {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        const std::string line = text.substr(line_begin, line_end - line_begin);
        const std::string trimmed = Trim(line);

        if (!trimmed.empty() && trimmed.front() == '[')
        {
            const size_t close = trimmed.find(']');
            in_section = close != std::string::npos && Trim(trimmed.substr(1, close - 1)) == section;
        }
        else if (in_section && !trimmed.empty() && trimmed.front() != '#')
        {
            const size_t equals = line.find('=');
            if (equals != std::string::npos && Trim(line.substr(0, equals)) == key)
            {
                const size_t value_begin = line.find_first_not_of(" \t", equals + 1);
                if (value_begin == std::string::npos)
                {
                    return false;
                }
                const size_t absolute_begin = line_begin + value_begin;
                const size_t value_end = FindTomlValueEndInText(text, absolute_begin);
                text.replace(absolute_begin, value_end - absolute_begin, replacement);
                return true;
            }
        }

        if (newline == std::string::npos)
        {
            break;
        }
        line_begin = newline + 1;
    }
    return false;
}

bool InsertTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                         const std::string &value)
{
    const std::string section_header = "[" + section + "]";
    // 只认独占一行的段头：注释里也会写「[translation].provider」，按子串找会把键插进别的段，
    // 下次写入在真正的段里找不到它又插一遍，重复键让整份配置解析失败（设置页报「保存失败」）。
    size_t section_begin = std::string::npos;
    for (size_t line_begin = 0; line_begin < text.size();)
    {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        const std::string trimmed = Trim(text.substr(line_begin, line_end - line_begin));
        if (trimmed.compare(0, section_header.size(), section_header) == 0)
        {
            const std::string rest = Trim(trimmed.substr(section_header.size()));
            if (rest.empty() || rest.front() == '#')
            {
                section_begin = line_begin + text.substr(line_begin, line_end - line_begin).find('[');
                break;
            }
        }
        if (newline == std::string::npos)
            break;
        line_begin = newline + 1;
    }
    if (section_begin == std::string::npos)
    {
        // 出厂模板不带全部设置段（如 [quanpin]），首次安装后第一次写这类键不能失败，
        // 否则设置页报「保存失败」。把缺失的段追加到文件尾部，语义由末尾的 toml::parse 校验兜底。
        if (!text.empty() && text.back() != '\n')
        {
            text.push_back('\n');
        }
        text.append(section_header + "\n" + key + " = " + value + "\n");
        return true;
    }

    const size_t section_line_end = text.find('\n', section_begin + section_header.size());
    if (section_line_end == std::string::npos)
    {
        text.append("\n" + key + " = " + value + "\n");
        return true;
    }

    size_t insert_pos = text.find("\n[", section_line_end);
    if (insert_pos == std::string::npos)
    {
        insert_pos = text.size();
        if (!text.empty() && text.back() != '\n')
        {
            text.push_back('\n');
            insert_pos = text.size();
        }
    }
    else
    {
        ++insert_pos;
        // 下一段前面那几行注释和空行是写给下一段的：新键放在它们之前，紧跟本段最后一个值。
        while (insert_pos > section_line_end + 1)
        {
            const size_t previous_begin = text.rfind('\n', insert_pos - 2);
            const size_t begin = previous_begin == std::string::npos ? 0 : previous_begin + 1;
            if (begin <= section_line_end)
                break;
            const std::string trimmed = Trim(text.substr(begin, insert_pos - 1 - begin));
            if (!trimmed.empty() && trimmed.front() != '#')
                break;
            insert_pos = begin;
        }
    }

    text.insert(insert_pos, key + " = " + value + "\n");
    return true;
}

std::string ReadFileText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool WriteFileBytes(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    return static_cast<bool>(output);
}

bool WriteFileTextAtomically(const std::filesystem::path &path, const std::string &text)
{
    std::filesystem::path temp_path = path;
    temp_path += L".tmp";
    ClearReadOnlyAttribute(path);
    ClearReadOnlyAttribute(temp_path);
    if (!WriteFileBytes(temp_path, text))
    {
        return false;
    }
    if (MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return true;
    }
    const bool replaced = WriteFileBytes(path, text);
    std::error_code error;
    std::filesystem::remove(temp_path, error);
    return replaced;
}

bool TomlTextIsParseable(const std::string &text)
{
    if (text.empty())
    {
        return false;
    }
    try
    {
        (void)toml::parse(text);
        return true;
    }
    catch (const toml::parse_error &)
    {
        return false;
    }
}

void ForEachTomlAssignment(const std::string &text, const TomlAssignmentVisitor &visit)
{
    std::string section;
    size_t line_begin = 0;
    while (line_begin < text.size())
    {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        const std::string line = text.substr(line_begin, line_end - line_begin);
        const std::string trimmed = Trim(line);
        size_t next_line_begin = newline == std::string::npos ? text.size() : newline + 1;

        if (!trimmed.empty() && trimmed.front() == '[')
        {
            const size_t close = trimmed.find(']');
            section = close == std::string::npos ? std::string() : Trim(trimmed.substr(1, close - 1));
        }
        else if (!trimmed.empty() && trimmed.front() != '#')
        {
            const size_t equals = line.find('=');
            const std::string key = equals == std::string::npos ? std::string() : Trim(line.substr(0, equals));
            const size_t value_offset =
                equals == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", equals + 1);
            if (!key.empty() && value_offset != std::string::npos)
            {
                const size_t value_begin = line_begin + value_offset;
                const size_t value_end = FindTomlValueEndInText(text, value_begin);
                visit(section, key, value_begin, value_end);
                if (value_end > line_end)
                {
                    // 多行值：跳过它占用的所有行，避免把字符串内容当成新的键。
                    const size_t after = text.find('\n', value_end);
                    next_line_begin = after == std::string::npos ? text.size() : after + 1;
                }
            }
        }

        line_begin = next_line_begin;
    }
}

std::string MakeTomlAssignmentId(const std::string &section, const std::string &key)
{
    return section + '\x01' + key;
}

std::map<std::string, std::string> ParseTomlAssignments(const std::string &text)
{
    std::map<std::string, std::string> values;
    ForEachTomlAssignment(
        text, [&](const std::string &section, const std::string &key, size_t value_begin, size_t value_end) {
            values[MakeTomlAssignmentId(section, key)] = text.substr(value_begin, value_end - value_begin);
        });
    return values;
}
} // namespace ime_config_detail
