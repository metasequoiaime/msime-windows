#include "settings/zip_archive.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <sstream>

#include <zlib.h>

namespace user_data_zip
{
namespace
{
constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
constexpr std::uint32_t kZip64EndOfCentralDirectorySignature = 0x06064b50;
constexpr std::uint16_t kStoredMethod = 0;
constexpr std::uint16_t kDeflatedMethod = 8;
constexpr std::uint16_t kVersionNeeded = 20;
constexpr std::uint16_t kFlagsUtf8 = 0x0800;
constexpr std::size_t kCopyBufferSize = 64 * 1024;

void Put16(std::vector<std::uint8_t> &out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void Put32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

std::uint16_t Get16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t Get32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

bool WriteRaw(std::ofstream &output, const void *data, std::size_t size)
{
    output.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    return output.good();
}

bool WriteBytes(std::ofstream &output, const std::vector<std::uint8_t> &bytes)
{
    return bytes.empty() || WriteRaw(output, bytes.data(), bytes.size());
}

std::string PathToUtf8(const std::filesystem::path &path)
{
    const std::string text = path.u8string();
    return text;
}

// Windows still resolves reserved device names in the middle of a path, so a
// segment such as "CON" or "NUL.txt" under the staging directory would open a
// device instead of a file. Both the writer and the reader use this predicate,
// which keeps a crafted archive from reaching extraction at all.
bool IsReservedDeviceName(std::string_view segment)
{
    const std::size_t dot = segment.find('.');
    std::string base(segment.substr(0, dot));
    for (char &ch : base)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$")
        return true;
    return base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '1' &&
           base[3] <= '9';
}

bool HasZip64Extra(const std::uint8_t *extra, std::size_t length)
{
    std::size_t offset = 0;
    while (offset + 4 <= length)
    {
        const std::uint16_t id = Get16(extra + offset);
        const std::uint16_t size = Get16(extra + offset + 2);
        if (id == 0x0001)
            return true;
        offset += 4u + size;
    }
    return false;
}
} // namespace

bool IsSafeArchivePath(std::string_view path)
{
    if (path.empty() || path.size() > 1024)
        return false;
    if (path.front() == '/')
        return false;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
        return false;
    for (const char ch : path)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte == '\\' || byte == ':' || byte < 0x20 || byte == 0x7F)
            return false;
    }

    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t end = path.find('/', start);
        const std::string_view segment =
            end == std::string_view::npos ? path.substr(start) : path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (segment.back() == '.' || segment.back() == ' ')
            return false;
        if (IsReservedDeviceName(segment))
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

Writer::Writer(std::filesystem::path archive_path) : archive_path_(std::move(archive_path))
{
    output_.open(archive_path_, std::ios::binary | std::ios::trunc);
}

Writer::~Writer()
{
    if (!finished_)
        Abort();
}

bool Writer::AddFile(const std::string &archive_path, const std::filesystem::path &source, std::string &error,
                     Entry *added)
{
    if (!output_)
    {
        error = "无法创建备份文件";
        failed_ = true;
        return false;
    }
    if (!IsSafeArchivePath(archive_path))
    {
        error = "备份路径不安全：" + archive_path;
        failed_ = true;
        return false;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(source, ec);
    if (ec || std::filesystem::is_symlink(source, ec))
    {
        error = "无法读取要备份的文件：" + PathToUtf8(source);
        failed_ = true;
        return false;
    }
    std::ifstream input(source, std::ios::binary);
    if (!input)
    {
        error = "无法打开要备份的文件：" + PathToUtf8(source);
        failed_ = true;
        return false;
    }
    return AddStream(archive_path, input, static_cast<std::uint64_t>(size), error, added);
}

bool Writer::AddBytes(const std::string &archive_path, std::string_view bytes, std::string &error, Entry *added)
{
    if (!output_)
    {
        error = "无法创建备份文件";
        failed_ = true;
        return false;
    }
    if (!IsSafeArchivePath(archive_path))
    {
        error = "备份路径不安全：" + archive_path;
        failed_ = true;
        return false;
    }
    std::istringstream input{std::string(bytes)};
    return AddStream(archive_path, input, bytes.size(), error, added);
}

bool Writer::AddStream(const std::string &archive_path, std::istream &input, std::uint64_t expected_size,
                       std::string &error, Entry *added)
{
    for (const PendingEntry &entry : entries_)
    {
        if (entry.path == archive_path)
        {
            error = "备份路径重复：" + archive_path;
            failed_ = true;
            return false;
        }
    }
    if (expected_size > std::numeric_limits<std::uint32_t>::max() ||
        written_ + expected_size > std::numeric_limits<std::uint32_t>::max())
    {
        error = "备份文件超过 4 GiB 上限";
        failed_ = true;
        return false;
    }

    std::uint32_t offset = 0;
    if (!WriteLocalHeader(archive_path, offset, error))
    {
        failed_ = true;
        return false;
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    std::uint64_t written = 0;
    std::array<char, kCopyBufferSize> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0)
            break;
        if (!WriteRaw(output_, buffer.data(), static_cast<std::size_t>(read)))
        {
            error = "写入备份文件失败";
            failed_ = true;
            return false;
        }
        crc = crc32(crc, reinterpret_cast<const Bytef *>(buffer.data()), static_cast<uInt>(read));
        written += static_cast<std::uint64_t>(read);
    }
    if (!input.eof() || written != expected_size)
    {
        error = "文件在备份过程中发生变化：" + archive_path;
        failed_ = true;
        return false;
    }
    if (!PatchLocalHeader(offset, static_cast<std::uint32_t>(crc), written, error))
    {
        failed_ = true;
        return false;
    }

    PendingEntry entry;
    entry.path = archive_path;
    entry.crc = static_cast<std::uint32_t>(crc);
    entry.size = written;
    entry.offset = offset;
    entries_.push_back(std::move(entry));
    if (added)
    {
        added->path = archive_path;
        added->size = written;
        added->compressed_size = written;
        added->crc = static_cast<std::uint32_t>(crc);
        added->method = kStoredMethod;
        added->local_offset = offset;
    }
    written_ += written;
    return true;
}

bool Writer::WriteLocalHeader(const std::string &archive_path, std::uint32_t &offset, std::string &error)
{
    if (written_ > std::numeric_limits<std::uint32_t>::max())
    {
        error = "备份文件超过 4 GiB 上限";
        return false;
    }
    offset = static_cast<std::uint32_t>(written_);
    std::vector<std::uint8_t> header;
    header.reserve(30 + archive_path.size());
    Put32(header, kLocalHeaderSignature);
    Put16(header, kVersionNeeded);
    Put16(header, kFlagsUtf8);
    Put16(header, kStoredMethod);
    Put16(header, 0); // time
    Put16(header, 0); // date
    Put32(header, 0); // crc, patched later
    Put32(header, 0); // compressed size, patched later
    Put32(header, 0); // uncompressed size, patched later
    Put16(header, static_cast<std::uint16_t>(archive_path.size()));
    Put16(header, 0);
    header.insert(header.end(), archive_path.begin(), archive_path.end());
    if (!WriteBytes(output_, header))
    {
        error = "写入备份文件失败";
        return false;
    }
    written_ += header.size();
    return true;
}

bool Writer::PatchLocalHeader(std::uint32_t offset, std::uint32_t crc, std::uint64_t size, std::string &error)
{
    output_.seekp(static_cast<std::streamoff>(offset + 14));
    std::vector<std::uint8_t> values;
    Put32(values, crc);
    Put32(values, static_cast<std::uint32_t>(size));
    Put32(values, static_cast<std::uint32_t>(size));
    if (!WriteBytes(output_, values))
    {
        error = "写入备份文件失败";
        return false;
    }
    output_.seekp(0, std::ios::end);
    return true;
}

bool Writer::Finish(std::string &error)
{
    if (finished_)
        return true;
    if (failed_ || !output_)
    {
        error = "备份文件未完成";
        return false;
    }
    if (!WriteCentralDirectory(error))
    {
        failed_ = true;
        return false;
    }
    output_.flush();
    output_.close();
    if (!output_)
    {
        error = "关闭备份文件失败";
        failed_ = true;
        return false;
    }
    finished_ = true;
    return true;
}

bool Writer::WriteCentralDirectory(std::string &error)
{
    if (written_ > std::numeric_limits<std::uint32_t>::max() || entries_.size() > 0xFFFF)
    {
        error = "备份文件超过 ZIP 格式上限";
        return false;
    }
    const std::uint32_t central_offset = static_cast<std::uint32_t>(written_);
    std::uint64_t central_size = 0;
    for (const PendingEntry &entry : entries_)
    {
        std::vector<std::uint8_t> header;
        header.reserve(46 + entry.path.size());
        Put32(header, kCentralHeaderSignature);
        Put16(header, kVersionNeeded);
        Put16(header, kVersionNeeded);
        Put16(header, kFlagsUtf8);
        Put16(header, kStoredMethod);
        Put16(header, 0);
        Put16(header, 0);
        Put32(header, entry.crc);
        Put32(header, static_cast<std::uint32_t>(entry.size));
        Put32(header, static_cast<std::uint32_t>(entry.size));
        Put16(header, static_cast<std::uint16_t>(entry.path.size()));
        Put16(header, 0);
        Put16(header, 0);
        Put16(header, 0);
        Put16(header, 0);
        Put32(header, 0);
        Put32(header, entry.offset);
        header.insert(header.end(), entry.path.begin(), entry.path.end());
        if (!WriteBytes(output_, header))
        {
            error = "写入备份目录失败";
            return false;
        }
        central_size += header.size();
    }
    if (central_offset + central_size + 22 > std::numeric_limits<std::uint32_t>::max())
    {
        error = "备份文件超过 4 GiB 上限";
        return false;
    }
    std::vector<std::uint8_t> end;
    Put32(end, kEndOfCentralDirectorySignature);
    Put16(end, 0);
    Put16(end, 0);
    Put16(end, static_cast<std::uint16_t>(entries_.size()));
    Put16(end, static_cast<std::uint16_t>(entries_.size()));
    Put32(end, static_cast<std::uint32_t>(central_size));
    Put32(end, central_offset);
    Put16(end, 0);
    if (!WriteBytes(output_, end))
    {
        error = "写入备份目录失败";
        return false;
    }
    written_ += central_size + end.size();
    return true;
}

void Writer::Abort()
{
    if (output_.is_open())
        output_.close();
    if (!finished_)
    {
        std::error_code ec;
        std::filesystem::remove(archive_path_, ec);
    }
    failed_ = true;
}

Reader::Reader(std::filesystem::path archive_path, Limits limits)
    : archive_path_(std::move(archive_path)), limits_(limits)
{
}

bool Reader::ReadAt(std::uint64_t offset, void *buffer, std::size_t size, std::string &error)
{
    if (offset > file_size_ || size > file_size_ - offset)
    {
        error = "备份文件已截断";
        return false;
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset));
    input_.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(input_.gcount()) != size)
    {
        error = "读取备份文件失败";
        return false;
    }
    return true;
}

bool Reader::Open(std::string &error)
{
    input_.open(archive_path_, std::ios::binary);
    if (!input_)
    {
        error = "无法打开备份文件";
        return false;
    }
    input_.seekg(0, std::ios::end);
    const std::streamoff end = input_.tellg();
    if (end < 22)
    {
        error = "不是有效的 ZIP 文件";
        return false;
    }
    file_size_ = static_cast<std::uint64_t>(end);

    const std::uint64_t tail_size = (std::min)(file_size_, static_cast<std::uint64_t>(65557));
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
    if (!ReadAt(file_size_ - tail_size, tail.data(), tail.size(), error))
        return false;

    std::size_t eocd = std::string::npos;
    for (std::size_t i = tail.size() - 22 + 1; i-- > 0;)
    {
        if (Get32(tail.data() + i) == kEndOfCentralDirectorySignature &&
            i + 22 + Get16(tail.data() + i + 20) == tail.size())
        {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos)
    {
        error = "不是有效的 ZIP 文件";
        return false;
    }

    const std::uint8_t *record = tail.data() + eocd;
    const std::uint16_t disk = Get16(record + 4);
    const std::uint16_t central_disk = Get16(record + 6);
    const std::uint16_t disk_entries = Get16(record + 8);
    const std::uint16_t total_entries = Get16(record + 10);
    const std::uint32_t central_size = Get32(record + 12);
    const std::uint32_t central_offset = Get32(record + 16);
    if (disk != 0 || central_disk != 0 || disk_entries != total_entries || total_entries == 0 ||
        total_entries == 0xFFFF)
    {
        error = "不支持分卷或多磁盘 ZIP";
        return false;
    }
    if (central_size == 0xFFFFFFFF || central_offset == 0xFFFFFFFF || total_entries > limits_.max_entries)
    {
        error = "不支持 ZIP64 或备份条目过多";
        return false;
    }
    if (central_offset > file_size_ || central_size > file_size_ - central_offset)
    {
        error = "ZIP 中央目录超出文件范围";
        return false;
    }
    // A ZIP64 EOCD record sits after the central directory; reject the archive even when the
    // 32-bit fields still fit, because the reader is explicitly 32-bit only. Keep the sum in
    // 64 bits: a file larger than 4 GiB would otherwise wrap the offset and read the wrong bytes.
    const std::uint64_t central_end = static_cast<std::uint64_t>(central_offset) + central_size;
    if (central_end + 4 <= file_size_)
    {
        std::uint8_t signature[4];
        if (ReadAt(central_end, signature, sizeof(signature), error) &&
            Get32(signature) == kZip64EndOfCentralDirectorySignature)
        {
            error = "不支持 ZIP64 备份文件";
            return false;
        }
    }

    std::vector<std::uint8_t> central(central_size);
    if (central_size > 0 && !ReadAt(central_offset, central.data(), central.size(), error))
        return false;

    std::size_t cursor = 0;
    entries_.clear();
    entries_.reserve(total_entries);
    for (std::uint16_t index = 0; index < total_entries; ++index)
    {
        if (cursor + 46 > central.size() || Get32(central.data() + cursor) != kCentralHeaderSignature)
        {
            error = "ZIP 中央目录损坏";
            return false;
        }
        const std::uint8_t *header = central.data() + cursor;
        const std::uint16_t flags = Get16(header + 8);
        const std::uint16_t method = Get16(header + 10);
        const std::uint32_t crc = Get32(header + 16);
        const std::uint32_t compressed_size = Get32(header + 20);
        const std::uint32_t size = Get32(header + 24);
        const std::uint16_t name_size = Get16(header + 28);
        const std::uint16_t extra_size = Get16(header + 30);
        const std::uint16_t comment_size = Get16(header + 32);
        const std::uint32_t local_offset = Get32(header + 42);
        const std::size_t record_size = 46u + name_size + extra_size + comment_size;
        if (cursor + record_size > central.size())
        {
            error = "ZIP 中央目录损坏";
            return false;
        }
        if ((flags & 0x0001) != 0)
        {
            error = "不支持加密的 ZIP";
            return false;
        }
        if (method != kStoredMethod && method != kDeflatedMethod)
        {
            error = "不支持该 ZIP 压缩方式";
            return false;
        }
        if (HasZip64Extra(header + 46 + name_size, extra_size))
        {
            error = "不支持 ZIP64 备份文件";
            return false;
        }
        if (compressed_size != size && method == kStoredMethod)
        {
            error = "ZIP 条目大小不合法";
            return false;
        }
        if (size > limits_.max_entry_size)
        {
            error = "ZIP 条目超过大小上限";
            return false;
        }
        Entry entry;
        entry.path.assign(reinterpret_cast<const char *>(header + 46), name_size);
        entry.crc = crc;
        entry.size = size;
        entry.compressed_size = compressed_size;
        entry.method = method;
        entry.local_offset = local_offset;
        if (!IsSafeArchivePath(entry.path))
        {
            error = "ZIP 内含不安全路径：" + entry.path;
            return false;
        }
        entries_.push_back(std::move(entry));
        cursor += record_size;
    }
    if (cursor != central.size())
    {
        error = "ZIP 中央目录损坏";
        return false;
    }
    std::uint64_t total_size = 0;
    for (const Entry &entry : entries_)
    {
        if (entry.size > limits_.max_total_size - total_size)
        {
            error = "ZIP 解压后总大小超过上限";
            return false;
        }
        total_size += entry.size;
    }
    return true;
}

const Entry *Reader::Find(std::string_view path) const
{
    for (const Entry &entry : entries_)
    {
        if (entry.path == path)
            return &entry;
    }
    return nullptr;
}

bool Reader::ReadDataOffset(const Entry &entry, std::uint64_t &data_offset, std::string &error)
{
    std::uint8_t header[30];
    if (!ReadAt(entry.local_offset, header, sizeof(header), error))
        return false;
    if (Get32(header) != kLocalHeaderSignature)
    {
        error = "ZIP 本地文件头损坏";
        return false;
    }
    const std::uint16_t flags = Get16(header + 6);
    const std::uint16_t method = Get16(header + 8);
    const std::uint16_t name_size = Get16(header + 26);
    const std::uint16_t extra_size = Get16(header + 28);
    if ((flags & 0x0001) != 0 || method != entry.method)
    {
        error = "ZIP 本地文件头与中央目录不一致";
        return false;
    }
    std::string name(name_size, '\0');
    if (name_size > 0 && !ReadAt(entry.local_offset + 30, name.data(), name.size(), error))
        return false;
    if (name != entry.path)
    {
        error = "ZIP 本地文件头与中央目录不一致";
        return false;
    }
    data_offset = entry.local_offset + 30ull + name_size + extra_size;
    if (data_offset > file_size_ || entry.compressed_size > file_size_ - data_offset)
    {
        error = "ZIP 条目数据超出文件范围";
        return false;
    }
    return true;
}

bool Reader::ExtractTo(const Entry &entry, const std::filesystem::path &target, std::string &error)
{
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        error = "无法写入临时文件";
        return false;
    }
    const bool ok =
        entry.method == kStoredMethod ? ExtractStored(entry, &output, error) : ExtractDeflated(entry, &output, error);
    output.close();
    if (!ok)
    {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        return false;
    }
    return true;
}

bool Reader::Verify(const Entry &entry, std::string &error)
{
    return entry.method == kStoredMethod ? ExtractStored(entry, nullptr, error)
                                         : ExtractDeflated(entry, nullptr, error);
}

bool Reader::ExtractToMemory(const Entry &entry, std::string &out, std::uint64_t max_size, std::string &error)
{
    if (entry.size > max_size)
    {
        error = "ZIP 条目超过内存读取上限";
        return false;
    }
    std::ostringstream buffer;
    const bool ok =
        entry.method == kStoredMethod ? ExtractStored(entry, &buffer, error) : ExtractDeflated(entry, &buffer, error);
    if (ok)
        out = std::move(buffer).str();
    return ok;
}

bool Reader::ExtractStored(const Entry &entry, std::ostream *output, std::string &error)
{
    std::uint64_t data_offset = 0;
    if (!ReadDataOffset(entry, data_offset, error))
        return false;
    uLong crc = crc32(0L, Z_NULL, 0);
    std::uint64_t remaining = entry.compressed_size;
    std::array<char, kCopyBufferSize> buffer{};
    while (remaining > 0)
    {
        const std::size_t amount = static_cast<std::size_t>((std::min)(remaining, kCopyBufferSize));
        if (!ReadAt(data_offset, buffer.data(), amount, error))
            return false;
        data_offset += amount;
        remaining -= amount;
        if (output)
        {
            output->write(buffer.data(), static_cast<std::streamsize>(amount));
            if (!*output)
            {
                error = "写入临时文件失败";
                return false;
            }
        }
        crc = crc32(crc, reinterpret_cast<const Bytef *>(buffer.data()), static_cast<uInt>(amount));
    }
    if (static_cast<std::uint64_t>(crc) != entry.crc)
    {
        error = "ZIP 条目 CRC 校验失败";
        return false;
    }
    return true;
}

bool Reader::ExtractDeflated(const Entry &entry, std::ostream *output, std::string &error)
{
    std::uint64_t data_offset = 0;
    if (!ReadDataOffset(entry, data_offset, error))
        return false;

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
    {
        error = "初始化 ZIP 解压失败";
        return false;
    }
    struct Guard
    {
        z_stream *stream;
        ~Guard()
        {
            inflateEnd(stream);
        }
    } guard{&stream};

    std::array<char, kCopyBufferSize> input{};
    std::array<char, kCopyBufferSize> output_buffer{};
    std::uint64_t remaining = entry.compressed_size;
    uLong crc = crc32(0L, Z_NULL, 0);
    std::uint64_t produced = 0;
    int result = Z_OK;
    while (result != Z_STREAM_END)
    {
        if (stream.avail_in == 0)
        {
            if (remaining == 0)
            {
                error = "ZIP 压缩数据已截断";
                return false;
            }
            const std::size_t amount = static_cast<std::size_t>((std::min)(remaining, input.size()));
            if (!ReadAt(data_offset, input.data(), amount, error))
                return false;
            data_offset += amount;
            remaining -= amount;
            stream.next_in = reinterpret_cast<Bytef *>(input.data());
            stream.avail_in = static_cast<uInt>(amount);
        }
        stream.next_out = reinterpret_cast<Bytef *>(output_buffer.data());
        stream.avail_out = static_cast<uInt>(output_buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END)
        {
            error = "ZIP 解压失败";
            return false;
        }
        const std::size_t produced_now = output_buffer.size() - stream.avail_out;
        if (produced_now > 0)
        {
            produced += produced_now;
            if (produced > entry.size)
            {
                error = "ZIP 条目解压后大小超出声明";
                return false;
            }
            if (output)
            {
                output->write(output_buffer.data(), static_cast<std::streamsize>(produced_now));
                if (!*output)
                {
                    error = "写入临时文件失败";
                    return false;
                }
            }
            crc = crc32(crc, reinterpret_cast<const Bytef *>(output_buffer.data()), static_cast<uInt>(produced_now));
        }
    }
    if (produced != entry.size || static_cast<std::uint64_t>(crc) != entry.crc)
    {
        error = "ZIP 条目 CRC 校验失败";
        return false;
    }
    return true;
}
} // namespace user_data_zip
