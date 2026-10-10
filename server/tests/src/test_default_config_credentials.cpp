#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cloud/translation_gloss.h"
#include "tests/includes/test_framework.h"
#include "voice-input/voice_providers.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct ConfigEntry
{
    std::string key;
    std::string value;
    int line = 0;
};

bool KeyNamesACredential(const std::string &key)
{
    return key.find("token") != std::string::npos || key.find("secret") != std::string::npos ||
           key.find("app_key") != std::string::npos;
}

std::string Trim(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

// Reads every single-line `key = "value"` pair whose key names a credential. The multi-line
// prompt blocks use `"""` and are skipped: their keys do not name credentials anyway.
std::vector<ConfigEntry> ReadCredentialEntries(const std::string &path, bool &opened)
{
    std::vector<ConfigEntry> entries;
    std::ifstream file(path);
    opened = static_cast<bool>(file);
    if (!opened)
        return entries;

    std::string line;
    int number = 0;
    while (std::getline(file, line))
    {
        ++number;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = Trim(trimmed.substr(0, equals));
        if (!KeyNamesACredential(key))
            continue;
        const std::string rest = Trim(trimmed.substr(equals + 1));
        if (rest.rfind("\"\"\"", 0) == 0 || rest.size() < 2 || rest.front() != '"' || rest.back() != '"')
            continue;
        entries.push_back({key, rest.substr(1, rest.size() - 2), number});
    }
    return entries;
}
} // namespace

// The shipped default config must not contain anything the runtime mistakes for a real credential.
// ai_assistant.enabled, voice_input.voice_input and tencent_tmt.enabled now ship off, but a user who
// switches one on before filling in a key must not end up sending the composition, the recorded
// audio or the candidate text to a third party under a placeholder that reads as usable.
// This reads the file the installer actually ships rather than restating today's placeholder
// spellings, so introducing a fourth spelling fails here instead of in someone's traffic log.
TEST_CASE(default_config_ships_no_usable_credentials)
{
    bool opened = false;
    const std::vector<ConfigEntry> entries = ReadCredentialEntries(MSIME_DEFAULT_CONFIG_PATH, opened);
    REQUIRE(opened);
    REQUIRE(!entries.empty());

    for (const ConfigEntry &entry : entries)
    {
        const bool usable_as_token = !VoiceInput::UsableToken(entry.value).empty();
        const bool usable_as_secret = CloudTranslation::IsUsableSecret(entry.value);
        if (usable_as_token || usable_as_secret)
        {
            // Thrown rather than REQUIRE'd so the failure names the offending key and line.
            throw std::runtime_error("config.default.toml line " + std::to_string(entry.line) + ": " + entry.key +
                                     " reads as a real credential");
        }
    }
}

TEST_CASE(placeholder_credentials_are_rejected_in_every_shipped_spelling)
{
    REQUIRE(VoiceInput::IsPlaceholderToken(""));
    REQUIRE(VoiceInput::IsPlaceholderToken("<YOUR_OWN_DOUBAO_ACCESS_TOKEN>"));
    REQUIRE(VoiceInput::IsPlaceholderToken("<YOUR_AI_TOKEN_DEEPSEEK>"));
    REQUIRE(VoiceInput::IsPlaceholderToken("<YOUR_ASR_APP_KEY>"));
    REQUIRE(VoiceInput::IsPlaceholderToken("FAKESECRET_e1f2g3h4i5j6k7l8m9n0"));
    REQUIRE(!VoiceInput::IsPlaceholderToken("sk-a-real-looking-token"));

    REQUIRE(!CloudTranslation::IsUsableSecret(""));
    REQUIRE(!CloudTranslation::IsUsableSecret("<YOUR_OWN_TENCENT_SECRET_ID>"));
    REQUIRE(!CloudTranslation::IsUsableSecret("<YOUR_TENCENT_SECRET_ID>"));
    REQUIRE(!CloudTranslation::IsUsableSecret("  <YOUR_TENCENT_SECRET_KEY>  "));
    REQUIRE(!CloudTranslation::IsUsableSecret("FAKESECRET_q2r3s4t5u6v7w8x9y0z1"));
    REQUIRE(CloudTranslation::IsUsableSecret("AKIDrealLookingSecretId"));
}
