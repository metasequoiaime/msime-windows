#include "session_factory.h"

#include "config/ime_config.h"
#include "engine_input_session.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "utils/common_utils.h"
#include <algorithm>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>

namespace
{
std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string ResolveEffectiveBackend(std::string configured_backend, SchemeType scheme)
{
    // Old installations may still persist "legacy". It is a configuration alias
    // for the shared engine now; keeping a second composition owner is unsafe.
    if (configured_backend.empty() || configured_backend == "legacy" || configured_backend == "engine")
    {
        switch (scheme)
        {
        case SchemeType::Shuangpin:
            return "engine-shuangpin";
        case SchemeType::Quanpin:
            return "engine-quanpin";
        case SchemeType::Wubi:
            return "engine-wubi";
        case SchemeType::JapaneseRomaji:
            return "engine-japanese-romaji";
        default:
            throw std::runtime_error("Unknown input scheme.");
        }
    }

    throw std::runtime_error("Unsupported input session backend: " + configured_backend);
}
} // namespace

std::shared_ptr<IInputSession> CreateInputSessionFromConfig()
{
    // The dictionary writer commits learned weights into msime.db while this
    // session queries it on the key thread; in WAL mode those queries no longer
    // wait for the commit. An installer upgrade ships a fresh rollback-mode file,
    // so this runs at every session creation, not once.
    (void)user_dictionary::enable_write_ahead_log(CommonUtils::get_ime_data_path() + "\\msime.db");
    static std::once_flag expire_once;
    std::call_once(expire_once, [] {
        (void)user_dictionary::expire_temporary_pinyin_words(CommonUtils::get_ime_data_path() + "\\msime.db",
                                                             user_dictionary::default_user_db_path(),
                                                             static_cast<std::int64_t>(std::time(nullptr)));
    });
    const std::string backend = DescribeEffectiveInputSessionBackendFromConfig();
    const ShuangpinProfile &shuangpin_profile = GetShuangpinProfile(GetConfiguredShuangpinSchema());
    if (backend == "engine-shuangpin")
    {
        return std::make_shared<EngineInputSession>(SchemeType::Shuangpin, shuangpin_profile);
    }
    if (backend == "engine-quanpin")
    {
        return std::make_shared<EngineInputSession>(SchemeType::Quanpin);
    }
    if (backend == "engine-wubi")
    {
        return std::make_shared<EngineInputSession>(SchemeType::Wubi);
    }
    if (backend == "engine-japanese-romaji")
    {
        return std::make_shared<EngineInputSession>(SchemeType::JapaneseRomaji);
    }

    throw std::runtime_error("Unsupported effective input session backend: " + backend);
}

std::shared_ptr<IInputSession> CreateTemporaryJapaneseInputSession()
{
    return std::make_shared<EngineInputSession>(SchemeType::JapaneseRomaji);
}

std::string DescribeConfiguredInputSessionBackendFromConfig()
{
    return ToLowerAscii(GetConfiguredSessionBackend());
}

std::string DescribeEffectiveInputSessionBackendFromConfig()
{
    return ResolveEffectiveBackend(DescribeConfiguredInputSessionBackendFromConfig(), GetConfiguredActiveInputScheme());
}
