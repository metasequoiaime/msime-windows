// 凭证类配置项的识别，以及它们在 config.toml 里的加密存储：加载后解密、写入前加密、旧明文就地迁移。
#include "config/ime_config_internal.h"
#include "config/credential_protection.h"
#include <set>
#include <string>
#include <vector>
#include "voice-input/voice_providers.h"

namespace
{
// TOML 字面量是否是一条还没加密的真实凭证；是就返回明文。占位符和空值保持明文，模板合并靠它们识别「用户没填」。
std::optional<std::string> UnprotectedCredential(const std::string &literal)
{
    try
    {
        const toml::table parsed = toml::parse("v = " + literal);
        const auto value = parsed["v"].value<std::string>();
        if (value && !VoiceInput::IsPlaceholderToken(*value) && !CredentialProtection::IsProtected(*value))
            return value;
    }
    catch (const toml::parse_error &)
    {
    }
    return std::nullopt;
}
} // namespace

namespace ime_config_detail
{
// 凭证类键：token / api_key / secret 等，以及每个提供商单独保存的 token 槽位
// （token_<provider> / asr_token[_<provider>] / polish_token[_<provider>]）。这类值一旦丢失，
// 用户就得重新去各家控制台申请再填一遍，是升级里代价最高的配置，所以单独兜底：不管模板怎么漂移、
// 文件是否损坏，只要用户填过真值就一条都不能丢。
bool IsCredentialKey(const std::string &key)
{
    // s3_access_key 是标识符不是密钥：S3 的签名里它是明文参与 canonical request 的，
    // 加密存储只会让设置页读不出来，而它本身并不保密。真正要保护的是 secret key。
    static const std::set<std::string> exact = {"token",        "api_key",         "apikey",       "secret_id",
                                                "secret_key",   "app_id",          "asr_app_key",  "asr_token",
                                                "polish_token", "webdav_password", "s3_secret_key"};
    if (exact.count(key) != 0)
    {
        return true;
    }
    return key.rfind("token_", 0) == 0 || key.rfind("asr_token_", 0) == 0 || key.rfind("polish_token_", 0) == 0;
}

std::optional<std::string> ProtectCredentialLiteral(const std::string &key, const std::string &literal)
{
    if (!IsCredentialKey(key))
        return literal;
    const auto plaintext = UnprotectedCredential(literal);
    if (!plaintext)
        return literal;
    const auto cipher = CredentialProtection::Protect(*plaintext);
    if (!cipher)
        return std::nullopt;
    return EscapeTomlBasicString(*cipher);
}

// 解不开的密文（配置被拷到别的账户或机器）当作没填，免得把密文当 token 发出去。
void RevealStoredCredentials(toml::table &tbl)
{
    for (auto &&[section_name, section_node] : tbl)
    {
        toml::table *section = section_node.as_table();
        if (!section)
            continue;
        for (auto &&[key, node] : *section)
        {
            toml::value<std::string> *value = node.as_string();
            if (value && IsCredentialKey(std::string(key.str())))
                value->get() = CredentialProtection::Unprotect(value->get()).value_or(std::string());
        }
    }
}

// 旧版本写下的、或用户手工填进 config.toml 的明文凭证，启动时就地换成密文，不用重新填。
void ProtectPlaintextCredentials()
{
    std::vector<ConfigValueUpdate> updates;
    for (const auto &[id, literal] : ParseTomlAssignments(ReadFileText(g_config_path)))
    {
        const size_t separator = id.find('\x01');
        const std::string key = id.substr(separator + 1);
        if (IsCredentialKey(key) && UnprotectedCredential(literal))
            updates.push_back({id.substr(0, separator), key, literal});
    }
    if (!updates.empty())
        WriteConfiguredValues(updates);
}
} // namespace ime_config_detail
