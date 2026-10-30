// 安装时把 scripts/pack-dictionary-source.py 导出的文本源还原成数据目录里的 msime.db、english.db、
// others.db 和 dict_japanese.dat。只做机械还原，不解释数据：拼音、权重这些规则都在导出之前定好了。
// 格式说明见 pack-dictionary-source.py 的文件头，两边要一起改。

#include <sqlite3.h>
#include <windows.h>
#include <bcrypt.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

std::ofstream g_log;

void log_line(const std::string &text)
{
    std::cout << text << '\n';
    if (g_log)
    {
        g_log << text << '\n';
        g_log.flush();
    }
}

std::string to_utf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string read_file(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open " + to_utf8(path.wstring()));
    input.seekg(0, std::ios::end);
    std::string data(static_cast<size_t>(input.tellg()), '\0');
    input.seekg(0);
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!input)
        throw std::runtime_error("cannot read " + to_utf8(path.wstring()));
    return data;
}

// 按 \n 逐行读，最后一个换行之后的空串不算一行。
class Lines
{
  public:
    explicit Lines(std::string_view text) : text_(text)
    {
    }

    bool next(std::string_view &line)
    {
        if (position_ >= text_.size())
            return false;
        const size_t end = text_.find('\n', position_);
        const size_t stop = end == std::string_view::npos ? text_.size() : end;
        line = text_.substr(position_, stop - position_);
        position_ = stop + 1;
        ++number_;
        return true;
    }

    std::string_view expect()
    {
        std::string_view line;
        if (!next(line))
            throw std::runtime_error("unexpected end of file after line " + std::to_string(number_));
        return line;
    }

    size_t number() const
    {
        return number_;
    }

  private:
    std::string_view text_;
    size_t position_ = 0;
    size_t number_ = 0;
};

std::vector<std::string_view> split_tabs(std::string_view line)
{
    std::vector<std::string_view> fields;
    size_t start = 0;
    while (true)
    {
        const size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos)
        {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

// \\、\t、\n、\r、\0 还原成原字符。
void unescape(std::string_view text, std::string &out)
{
    out.clear();
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\\')
        {
            out.push_back(text[i]);
            continue;
        }
        if (++i >= text.size())
            throw std::runtime_error("dangling escape");
        switch (text[i])
        {
        case '\\':
            out.push_back('\\');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case '0':
            out.push_back('\0');
            break;
        default:
            throw std::runtime_error("unknown escape");
        }
    }
}

template <typename T> T parse_number(std::string_view text)
{
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size())
        throw std::runtime_error("bad number: " + std::string(text));
    return value;
}

std::string elapsed(std::chrono::steady_clock::time_point started)
{
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    return std::to_string(ms / 1000) + "." + std::to_string(ms % 1000 / 100) + "s";
}

// ---- SQLite ----

class Database
{
  public:
    explicit Database(const fs::path &path)
    {
        const std::string utf8 = to_utf8(path.wstring());
        if (sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            throw std::runtime_error("cannot create " + utf8 + ": " + message);
        }
    }

    ~Database()
    {
        sqlite3_close(db_);
    }

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    void exec(const std::string &sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error != nullptr ? error : "unknown error";
            sqlite3_free(error);
            throw std::runtime_error(message + " in: " + sql.substr(0, 200));
        }
    }

    sqlite3 *handle() const
    {
        return db_;
    }

  private:
    sqlite3 *db_ = nullptr;
};

class Statement
{
  public:
    Statement(sqlite3 *db, const std::string &sql) : db_(db)
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string(sqlite3_errmsg(db)) + " in: " + sql);
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    sqlite3_stmt *get() const
    {
        return statement_;
    }

    void step_done()
    {
        if (sqlite3_step(statement_) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db_));
        sqlite3_reset(statement_);
    }

  private:
    sqlite3 *db_;
    sqlite3_stmt *statement_ = nullptr;
};

std::string quote_identifier(const std::string &name)
{
    std::string quoted = "\"";
    for (const char c : name)
    {
        quoted.push_back(c);
        if (c == '"')
            quoted.push_back('"');
    }
    return quoted + "\"";
}

std::vector<unsigned char> decode_hex(std::string_view text)
{
    if (text.size() % 2 != 0)
        throw std::runtime_error("odd hex length");
    std::vector<unsigned char> bytes(text.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        const std::string_view pair = text.substr(i * 2, 2);
        const auto [end, error] = std::from_chars(pair.data(), pair.data() + 2, bytes[i], 16);
        if (error != std::errc() || end != pair.data() + 2)
            throw std::runtime_error("bad hex byte");
    }
    return bytes;
}

void bind_field(sqlite3_stmt *statement, int index, char codec, std::string_view field, std::string &buffer)
{
    int status = SQLITE_OK;
    if (codec == 'x')
    {
        if (field.empty())
            throw std::runtime_error("empty tagged value");
        codec = field[0];
        field.remove_prefix(1);
        if (codec == 'n')
        {
            status = sqlite3_bind_null(statement, index);
            codec = '\0';
        }
        else if (codec == 'r')
        {
            buffer.assign(field);
            char *end = nullptr;
            const double value = std::strtod(buffer.c_str(), &end);
            if (end != buffer.c_str() + buffer.size())
                throw std::runtime_error("bad real: " + buffer);
            status = sqlite3_bind_double(statement, index, value);
            codec = '\0';
        }
        else if (codec == 'b')
        {
            const auto bytes = decode_hex(field);
            status = sqlite3_bind_blob64(statement, index, bytes.data(), bytes.size(), SQLITE_TRANSIENT);
            codec = '\0';
        }
    }
    if (codec == 'i')
        status = sqlite3_bind_int64(statement, index, parse_number<long long>(field));
    else if (codec == 't')
    {
        unescape(field, buffer);
        status = sqlite3_bind_text64(statement, index, buffer.data(), buffer.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    }
    else if (codec != '\0')
        throw std::runtime_error(std::string("unknown codec ") + codec);
    if (status != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement)));
}

void restore_database(const fs::path &dump, const fs::path &target, const std::string &label)
{
    const auto started = std::chrono::steady_clock::now();
    const std::string text = read_file(dump);
    Lines lines(text);
    if (lines.expect() != "MSIME-DB-DUMP 1")
        throw std::runtime_error("not a dictionary dump: " + to_utf8(dump.wstring()));

    fs::remove(target);
    Database db(target);
    bool loading = false;
    std::vector<std::pair<std::string, long long>> tables;
    std::string sql;
    std::string buffer;
    std::string_view line;
    long long total_rows = 0;
    while (lines.next(line))
    {
        if (line.size() < 2 || line[1] != '\t')
            throw std::runtime_error("bad directive at line " + std::to_string(lines.number()));
        const char directive = line[0];
        line.remove_prefix(2);
        // page_size 等要在建第一张表之前设；之后才关日志、开事务。
        if (directive != 'P' && !loading)
        {
            db.exec("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA locking_mode=EXCLUSIVE; "
                    "PRAGMA cache_size=-262144; PRAGMA temp_store=MEMORY; BEGIN");
            loading = true;
        }
        if (directive == 'P' || directive == 'S')
        {
            unescape(line, sql);
            db.exec(sql);
            continue;
        }
        if (directive != 'T')
            throw std::runtime_error("unknown directive at line " + std::to_string(lines.number()));

        const auto header = split_tabs(line);
        if (header.size() != 3)
            throw std::runtime_error("bad table header at line " + std::to_string(lines.number()));
        std::string name;
        unescape(header[0], name);
        const std::string codecs(header[1]);
        const auto count = parse_number<long long>(header[2]);
        std::string insert = "INSERT INTO " + quote_identifier(name) + " VALUES (";
        for (size_t i = 0; i < codecs.size(); ++i)
            insert += i == 0 ? "?" : ", ?";
        Statement statement(db.handle(), insert + ")");
        for (long long row = 0; row < count; ++row)
        {
            const auto fields = split_tabs(lines.expect());
            if (fields.size() != codecs.size())
                throw std::runtime_error(name + ": expected " + std::to_string(codecs.size()) + " fields at line " +
                                         std::to_string(lines.number()));
            for (size_t i = 0; i < fields.size(); ++i)
                bind_field(statement.get(), static_cast<int>(i) + 1, codecs[i], fields[i], buffer);
            statement.step_done();
        }
        tables.emplace_back(name, count);
        total_rows += count;
    }
    if (!loading)
        throw std::runtime_error("dump has no schema");
    db.exec("COMMIT");

    // 核对每张表的行数，再改回发布的库用的回滚日志模式。sqlite_stat1 由导出文件原样写回，不重跑 ANALYZE。
    for (const auto &[name, count] : tables)
    {
        Statement statement(db.handle(), "SELECT count(*) FROM " + quote_identifier(name));
        if (sqlite3_step(statement.get()) != SQLITE_ROW || sqlite3_column_int64(statement.get(), 0) != count)
            throw std::runtime_error(name + ": row count does not match the dump");
    }
    db.exec("PRAGMA locking_mode=NORMAL; PRAGMA journal_mode=DELETE");
    log_line(label + ": " + std::to_string(total_rows) + " rows in " + std::to_string(tables.size()) + " tables, " +
             elapsed(started));
}

// ---- 日文模型（MSJPDT1，布局见 scripts/dictionary_build/japanese_model.py）----

template <typename T> void put(std::string &out, T value)
{
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T)); // x86/x64 都是小端，与文件格式一致。
    out.append(bytes, sizeof(T));
}

std::string sha256_hex(const std::string &data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("cannot open SHA-256 provider");
    unsigned char digest[32];
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                       static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status))
        throw std::runtime_error("SHA-256 failed");
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

void restore_japanese(const fs::path &tokens_path, const fs::path &connection_path, const fs::path &target,
                      const std::string &label)
{
    const auto started = std::chrono::steady_clock::now();
    const std::string text = read_file(tokens_path);
    const std::string connection = read_file(connection_path);
    Lines lines(text);
    if (lines.expect() != "MSIME-JP-DUMP 1")
        throw std::runtime_error("not a Japanese model dump: " + to_utf8(tokens_path.wstring()));
    const std::string_view summary = lines.expect();
    const size_t first = summary.find(' ');
    const size_t second = summary.find(' ', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos)
        throw std::runtime_error("bad Japanese model summary line");
    const auto size = parse_number<uint32_t>(summary.substr(0, first));
    const auto count = parse_number<uint32_t>(summary.substr(first + 1, second - first - 1));
    const std::string expected_sha256(summary.substr(second + 1));
    if (connection.size() != static_cast<size_t>(size) * size * 2)
        throw std::runtime_error("connection matrix size does not match the model");

    // 字符串按词条顺序（先读音后表层）首次出现时追加，重复的复用，与 japanese_model.pack 一致。
    std::string strings;
    std::unordered_map<std::string, std::pair<uint32_t, uint16_t>> locations;
    std::string records;
    records.reserve(static_cast<size_t>(count) * 20);
    std::string value;
    auto intern = [&](std::string_view field) {
        unescape(field, value);
        const auto found = locations.find(value);
        if (found != locations.end())
            return found->second;
        if (value.size() > 0xFFFF)
            throw std::runtime_error("dictionary string exceeds u16 length");
        const std::pair<uint32_t, uint16_t> location{static_cast<uint32_t>(strings.size()),
                                                     static_cast<uint16_t>(value.size())};
        strings += value;
        locations.emplace(value, location);
        return location;
    };
    for (uint32_t index = 0; index < count; ++index)
    {
        const auto fields = split_tabs(lines.expect());
        if (fields.size() != 5)
            throw std::runtime_error("bad Japanese token at line " + std::to_string(lines.number()));
        const auto reading = intern(fields[0]);
        const auto surface = intern(fields[1]);
        put(records, reading.first);
        put(records, reading.second);
        put(records, surface.first);
        put(records, surface.second);
        put(records, parse_number<uint16_t>(fields[2]));
        put(records, parse_number<uint16_t>(fields[3]));
        put(records, parse_number<int32_t>(fields[4]));
    }
    std::string_view rest;
    if (lines.next(rest))
        throw std::runtime_error("trailing data after Japanese tokens");

    std::string model;
    model.reserve(56 + records.size() + connection.size() + strings.size());
    model.append("MSJPDT1", 8); // 含结尾的 NUL
    put<uint32_t>(model, 1);
    put<uint32_t>(model, count);
    put<uint32_t>(model, size);
    put<uint32_t>(model, 0);
    const uint64_t token_offset = 56;
    const uint64_t connection_offset = token_offset + records.size();
    const uint64_t string_offset = connection_offset + connection.size();
    put(model, token_offset);
    put(model, connection_offset);
    put(model, string_offset);
    put<uint64_t>(model, strings.size());
    model += records;
    model += connection;
    model += strings;
    if (sha256_hex(model) != expected_sha256)
        throw std::runtime_error("rebuilt Japanese model does not match the original SHA-256");

    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    output.write(model.data(), static_cast<std::streamsize>(model.size()));
    output.close();
    if (!output)
        throw std::runtime_error("cannot write " + to_utf8(target.wstring()));
    log_line(label + ": " + std::to_string(count) + " tokens, " + elapsed(started));
}

int run(int argc, wchar_t **argv)
{
    fs::path source;
    fs::path data_dir;
    fs::path log_path;
    bool usage = false;
    for (int i = 1; i < argc && !usage; ++i)
    {
        const std::wstring argument = argv[i];
        if (argument == L"--source" && i + 1 < argc)
            source = argv[++i];
        else if (argument == L"--data-dir" && i + 1 < argc)
            data_dir = argv[++i];
        else if (argument == L"--log" && i + 1 < argc)
            log_path = argv[++i];
        else
            usage = true;
    }
    if (usage || source.empty() || data_dir.empty())
    {
        std::wcerr << L"Usage: MetasequoiaImeDictionaryBuild.exe --source <directory> --data-dir <directory> "
                      L"[--log <file>]\n";
        return 2;
    }
    if (!log_path.empty())
    {
        fs::create_directories(log_path.parent_path());
        // 追加而不是覆盖：安装程序失败后让用户重试，成功的那次不能把失败的原因冲掉。
        g_log.open(log_path, std::ios::binary | std::ios::app);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                      now.wMinute, now.wSecond);
        log_line(std::string("---- dictionary build ") + stamp);
    }

    const auto started = std::chrono::steady_clock::now();
    // 先全部建成 .building，整套成功后再替换，失败时不留半套新词库。
    const std::vector<std::pair<fs::path, fs::path>> outputs = {
        {L"msime.dump", L"msime.db"},
        {L"english.dump", L"english.db"},
        {L"others.dump", L"others.db"},
    };
    std::vector<fs::path> built;
    try
    {
        for (const auto &[dump, name] : outputs)
        {
            const fs::path building = data_dir / (name.wstring() + L".building");
            built.push_back(building);
            restore_database(source / dump, building, to_utf8(name.wstring()));
        }
        const fs::path japanese = data_dir / L"dict_japanese.dat.building";
        built.push_back(japanese);
        restore_japanese(source / L"dict_japanese.tokens", source / L"dict_japanese.connection", japanese,
                         "dict_japanese.dat");
    }
    catch (...)
    {
        std::error_code ignored;
        for (const auto &path : built)
            fs::remove(path, ignored);
        throw;
    }
    for (const auto &path : built)
    {
        const fs::path final_path = fs::path(path).replace_extension();
        // 刚写完的大文件可能正被杀毒软件扫描，替换会短暂地拒绝访问或共享冲突，等一等再试。
        // 被别的进程长期打开着（比如 Server 抢先建了空库）就等不完，报出来让安装程序处理。
        DWORD error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            if (MoveFileExW(path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = ERROR_SUCCESS;
                break;
            }
            error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)
                break;
            if (attempt == 0)
                log_line("waiting for " + to_utf8(final_path.wstring()) + " to be released (error " +
                         std::to_string(error) + ")");
            Sleep(500);
        }
        if (error != ERROR_SUCCESS)
        {
            std::error_code ignored;
            for (const auto &leftover : built)
                fs::remove(leftover, ignored);
            throw std::runtime_error("cannot replace " + to_utf8(final_path.wstring()) + " (error " +
                                     std::to_string(error) + "); another program may have it open");
        }
    }
    log_line("dictionaries ready in " + elapsed(started));
    return 0;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    // 安装程序看退出码决定是否中止，异常逃出去会直接 fastfail，用户只看到安装失败而没有原因。
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception &error)
    {
        log_line(std::string("dictionary build failed: ") + error.what());
        return 1;
    }
    catch (...)
    {
        log_line("dictionary build failed with an unknown error");
        return 1;
    }
}
