#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Minimal ZIP support for the user-data backup. The writer emits stored
// entries so multi-hundred-megabyte model files can be streamed without
// buffering them in memory. The reader accepts stored and deflated entries,
// which keeps the format compatible with archives produced by other tools
// while we only promise round-tripping our own exports.
namespace user_data_zip
{
struct Entry
{
    std::string path;
    std::uint64_t size = 0;
    std::uint64_t compressed_size = 0;
    std::uint32_t crc = 0;
    std::uint16_t method = 0;
    std::uint32_t local_offset = 0;
};

struct Limits
{
    std::uint64_t max_entry_size = 4ull * 1024 * 1024 * 1024 - 1;
    std::uint64_t max_total_size = 4ull * 1024 * 1024 * 1024 - 1;
    std::size_t max_entries = 20000;
};

class Writer
{
  public:
    explicit Writer(std::filesystem::path archive_path);
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    bool AddFile(const std::string &archive_path, const std::filesystem::path &source, std::string &error,
                 Entry *added = nullptr);
    bool AddBytes(const std::string &archive_path, std::string_view bytes, std::string &error, Entry *added = nullptr);
    bool Finish(std::string &error);

  private:
    struct PendingEntry
    {
        std::string path;
        std::uint32_t crc = 0;
        std::uint64_t size = 0;
        std::uint32_t offset = 0;
    };

    bool AddStream(const std::string &archive_path, std::istream &input, std::uint64_t expected_size,
                   std::string &error, Entry *added);
    void Abort();
    bool WriteLocalHeader(const std::string &archive_path, std::uint32_t &offset, std::string &error);
    bool PatchLocalHeader(std::uint32_t offset, std::uint32_t crc, std::uint64_t size, std::string &error);
    bool WriteCentralDirectory(std::string &error);

    std::filesystem::path archive_path_;
    std::ofstream output_;
    std::vector<PendingEntry> entries_;
    std::uint64_t written_ = 0;
    bool finished_ = false;
    bool failed_ = false;
};

class Reader
{
  public:
    explicit Reader(std::filesystem::path archive_path, Limits limits = {});

    bool Open(std::string &error);
    const std::vector<Entry> &Entries() const
    {
        return entries_;
    }
    const Entry *Find(std::string_view path) const;
    bool ExtractTo(const Entry &entry, const std::filesystem::path &target, std::string &error);
    // Reads a small entry into memory (the backup manifest). Fails when the entry
    // declares more than max_size bytes.
    bool ExtractToMemory(const Entry &entry, std::string &out, std::uint64_t max_size, std::string &error);
    // Streams the entry through the decompressor and checks the CRC without writing it anywhere.
    bool Verify(const Entry &entry, std::string &error);

  private:
    bool ReadAt(std::uint64_t offset, void *buffer, std::size_t size, std::string &error);
    bool ExtractStored(const Entry &entry, std::ostream *output, std::string &error);
    bool ExtractDeflated(const Entry &entry, std::ostream *output, std::string &error);
    bool ReadDataOffset(const Entry &entry, std::uint64_t &data_offset, std::string &error);

    std::filesystem::path archive_path_;
    Limits limits_;
    std::ifstream input_;
    std::vector<Entry> entries_;
    std::uint64_t file_size_ = 0;
};

bool IsSafeArchivePath(std::string_view path);
} // namespace user_data_zip
