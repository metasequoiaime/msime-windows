#include "../../core/data_path.h"
#include "../../japanese/japanese_sentence_decoder.h"
#include "../../japanese/romaji_converter.h"
#include "../../providers/japanese_candidate_provider.h"
#include "../../schemes/japanese_romaji_scheme.h"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

void test_pending_predictions(const std::filesystem::path &root)
{
    std::filesystem::create_directories(root);
    const auto model = root / "dict_japanese.dat";
    const auto database = root / "msime.db";
    // One-token MSJPDT1 fixture, using the explicit little-endian test layout.
    const std::string reading = "おんながわ";
    const std::string surface = "女川";
    std::ofstream output(model, std::ios::binary);
    const auto integer = [&](std::uint64_t value, int width) {
        for (int i = 0; i < width; ++i)
            output.put(static_cast<char>((value >> (i * 8)) & 255));
    };
    output.write("MSJPDT1", 8);
    integer(1, 4);
    integer(1, 4);
    integer(1, 4);
    integer(0, 4);
    integer(56, 8);
    integer(76, 8);
    integer(78, 8);
    integer(reading.size() + surface.size(), 8);
    integer(0, 4);
    integer(reading.size(), 2);
    integer(reading.size(), 4);
    integer(surface.size(), 2);
    integer(0, 2);
    integer(0, 2);
    integer(1, 4);
    integer(0, 2);
    output << reading << surface;
    output.close();
    require(japanese::JapaneseSentenceDecoder(metasequoia::path_to_utf8(model)).ready(),
            "Japanese prediction fixture did not load.");
    sqlite3 *db = nullptr;
    require(sqlite3_open(metasequoia::path_to_utf8(database).c_str(), &db) == SQLITE_OK,
            "Could not create Japanese dictionary fixture.");
    const int status =
        sqlite3_exec(db, "CREATE TABLE japanese_lexicon(code TEXT,value TEXT,weight INTEGER,PRIMARY KEY(code,value));",
                     nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(status == SQLITE_OK, "Could not create Japanese dictionary table.");

    JapaneseCandidateProvider provider(metasequoia::path_to_utf8(database), metasequoia::path_to_utf8(model));
    for (const std::string spelling : {"onnnag", "on'nag", "ONNNAG"})
    {
        JapaneseRomajiScheme scheme;
        scheme.set_raw_input(spelling, spelling);
        const auto candidates = provider.query(scheme.build_request());
        if (candidates.empty() || candidates.front().word != surface)
            throw std::runtime_error("Nasal spelling lost its pending prediction: " + spelling);
    }
}

void run_test(const std::filesystem::path &root)
{
    struct Case
    {
        const char *romaji;
        const char *hiragana;
    };
    // nn completes one nasal without leaving a pending n, matching Mozc's
    // default romanji-hiragana table. n' remains an explicit separator.
    const Case cases[] = {{"n", "ん"},
                          {"nn", "ん"},
                          {"NN", "ん"},
                          {"nnn", "んん"},
                          {"nnnn", "んん"},
                          {"nna", "んあ"},
                          {"nni", "んい"},
                          {"nnya", "んや"},
                          {"na", "な"},
                          {"ni", "に"},
                          {"nya", "にゃ"},
                          {"ann", "あん"},
                          {"annai", "あんあい"},
                          {"annnai", "あんない"},
                          {"onnna", "おんな"},
                          {"rennai", "れんあい"},
                          {"ren'ai", "れんあい"},
                          {"ren'nai", "れんない"},
                          {"konnichiha", "こんいちは"},
                          {"konnnichiha", "こんにちは"},
                          {"kon'nichiha", "こんにちは"},
                          {"shin'you", "しんよう"},
                          {"shinnya", "しんや"},
                          {"kanpai", "かんぱい"},
                          {"nihongo", "にほんご"},
                          {"gakkou", "がっこう"},
                          {"n-", "んー"},
                          {"ko-hi-", "こーひー"}};
    for (const auto &item : cases)
    {
        const auto converted = japanese::ConvertRomaji(item.romaji);
        if (converted.hiragana != item.hiragana || !converted.pending.empty() || !converted.complete)
            throw std::runtime_error(std::string("Unexpected romaji conversion for ") + item.romaji);
    }

    const auto pending = japanese::ConvertRomaji("nnk");
    require(pending.hiragana == "ん" && pending.pending == "k" && !pending.complete,
            "A consonant after nn should remain pending without duplicating the nasal.");

    JapaneseRomajiScheme scheme;
    scheme.handle_key('N', 0, u'n');
    scheme.handle_key('N', 0, u'n');
    const auto nasal = scheme.build_request();
    require(nasal.raw_input == "nn" && nasal.segmentation == "ん",
            "The scheme must retain both typed keys while converting a single nasal.");
    scheme.handle_key('A', 0, u'a');
    require(scheme.build_request().segmentation == "んあ", "A vowel after nn reused the second n.");
    scheme.handle_key(ImeKey::Backspace, 0, 0);
    require(scheme.build_request().segmentation == "ん", "Deleting the vowel duplicated the nasal.");
    scheme.handle_key(ImeKey::Backspace, 0, 0);
    require(scheme.build_request().raw_input == "n", "Backspace did not remove exactly one typed key.");
    test_pending_predictions(root);
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("msime-japanese-romaji-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int result = 0;
    try
    {
        run_test(root);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}
