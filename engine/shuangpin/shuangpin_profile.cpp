#include "shuangpin_profile.h"
#include <deque>
#include <mutex>

namespace
{
struct ProfileRegistry
{
    std::mutex mutex;
    std::deque<ShuangpinProfile> storage;
    std::unordered_map<std::string, const ShuangpinProfile *> by_name;
};

ProfileRegistry &Registry()
{
    static ProfileRegistry registry;
    return registry;
}

const ShuangpinProfile *FindRegisteredProfile(std::string_view name)
{
    auto &registry = Registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.by_name.find(std::string(name));
    return found == registry.by_name.end() ? nullptr : found->second;
}
} // namespace

const ShuangpinProfile &GetXiaoheShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "xiaohe",
        {
            {"sh", "u"},
            {"ch", "i"},
            {"zh", "v"},
        },
        {
            {"a", "aa"},
            {"ai", "ai"},
            {"an", "an"},
            {"ao", "ao"},
            {"ang", "ah"},
            {"e", "ee"},
            {"ei", "ei"},
            {"en", "en"},
            {"eng", "eg"},
            {"er", "er"},
            {"o", "oo"},
            {"ou", "ou"},
        },
        {
            {"iu", "q"},   {"ei", "w"},   {"e", "e"},    {"uan", "r"}, {"ue", "t"},  {"ve", "t"}, {"un", "y"},
            {"u", "u"},    {"i", "i"},    {"uo", "o"},   {"o", "o"},   {"ie", "p"},  {"a", "a"},  {"ong", "s"},
            {"iong", "s"}, {"ai", "d"},   {"en", "f"},   {"eng", "g"}, {"ang", "h"}, {"an", "j"}, {"uai", "k"},
            {"ing", "k"},  {"uang", "l"}, {"iang", "l"}, {"ou", "z"},  {"ua", "x"},  {"ia", "x"}, {"ao", "c"},
            {"ui", "v"},   {"v", "v"},    {"in", "b"},   {"iao", "n"}, {"ian", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetZiranmaShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "ziranma",
        {
            {"sh", "u"},
            {"ch", "i"},
            {"zh", "v"},
        },
        {
            {"a", "aa"},
            {"ai", "ai"},
            {"an", "an"},
            {"ao", "ao"},
            {"ang", "ah"},
            {"e", "ee"},
            {"ei", "ei"},
            {"en", "en"},
            {"eng", "eg"},
            {"er", "er"},
            {"o", "oo"},
            {"ou", "ou"},
        },
        {
            {"iu", "q"},  {"ia", "w"},   {"ua", "w"},  {"e", "e"},    {"uan", "r"},  {"ue", "t"}, {"ve", "t"},
            {"ing", "y"}, {"uai", "y"},  {"u", "u"},   {"i", "i"},    {"o", "o"},    {"uo", "o"}, {"un", "p"},
            {"a", "a"},   {"iong", "s"}, {"ong", "s"}, {"iang", "d"}, {"uang", "d"}, {"en", "f"}, {"eng", "g"},
            {"ang", "h"}, {"an", "j"},   {"ao", "k"},  {"ai", "l"},   {"ei", "z"},   {"ie", "x"}, {"iao", "c"},
            {"ui", "v"},  {"v", "v"},    {"ou", "b"},  {"in", "n"},   {"ian", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetShoudaoShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "shoudao",
        {
            {"sh", "e"},
            {"ch", "i"},
            {"zh", "v"},
        },
        {
            {"a", "aa"},
            {"ai", "ai"},
            {"an", "an"},
            {"ao", "ao"},
            {"ang", "ay"},
            // sh sits on "e", so "ee"/"ei"/"ef" would collide with she/shi/sheng.
            {"e", "ue"},
            {"ei", "ui"},
            {"en", "en"},
            {"eng", "uf"},
            {"er", "er"},
            {"o", "oo"},
            {"ou", "ou"},
        },
        {
            {"iu", "q"},  {"ua", "w"},  {"e", "e"},   {"ie", "r"},  {"uan", "t"},  {"ang", "y"},  {"u", "u"},
            {"i", "i"},   {"o", "o"},   {"uo", "o"},  {"iao", "p"}, {"a", "a"},    {"ou", "s"},   {"ao", "d"},
            {"eng", "f"}, {"uai", "g"}, {"ing", "g"}, {"ong", "h"}, {"iong", "h"}, {"an", "j"},   {"en", "k"},
            {"ia", "k"},  {"ai", "l"},  {"ue", "l"},  {"un", "z"},  {"iang", "x"}, {"uang", "x"}, {"in", "c"},
            {"v", "v"},   {"ui", "v"},  {"ve", "b"},  {"ian", "n"}, {"ei", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetMicrosoftShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "microsoft",
        {
            {"sh", "u"},
            {"ch", "i"},
            {"zh", "v"},
        },
        {
            {"a", "oa"},
            {"ai", "ol"},
            {"an", "oj"},
            {"ang", "oh"},
            {"ao", "ok"},
            {"e", "oe"},
            {"ei", "oz"},
            {"en", "of"},
            {"eng", "og"},
            {"er", "or"},
            {"o", "oo"},
            {"ou", "ob"},
        },
        {
            {"iu", "q"},  {"ia", "w"},   {"ua", "w"},  {"e", "e"},    {"uan", "r"},  {"ue", "t"}, {"ve", "v"},
            {"uai", "y"}, {"v", "y"},    {"u", "u"},   {"i", "i"},    {"o", "o"},    {"uo", "o"}, {"un", "p"},
            {"a", "a"},   {"iong", "s"}, {"ong", "s"}, {"iang", "d"}, {"uang", "d"}, {"en", "f"}, {"eng", "g"},
            {"ang", "h"}, {"an", "j"},   {"ao", "k"},  {"ai", "l"},   {"ing", ";"},  {"ei", "z"}, {"ie", "x"},
            {"iao", "c"}, {"ui", "v"},   {"ou", "b"},  {"in", "n"},   {"ian", "m"},
        },
        // Windows 11 的微软拼音里 t / v 都能打 ue 与 üe：学 = xt / xv，略 = lv / lt（#1175）。
        {
            {"ue", "v"},
            {"ve", "t"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetSogouShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "sogou",
        {
            {"sh", "u"},
            {"ch", "i"},
            {"zh", "v"},
        },
        {
            {"a", "oa"},
            {"ai", "ol"},
            {"an", "oj"},
            {"ang", "oh"},
            {"ao", "ok"},
            {"e", "oe"},
            {"ei", "oz"},
            {"en", "of"},
            {"eng", "og"},
            {"er", "or"},
            {"o", "oo"},
            {"ou", "ob"},
        },
        {
            {"iu", "q"},  {"ia", "w"},   {"ua", "w"},  {"e", "e"},    {"uan", "r"},  {"ue", "t"}, {"ve", "t"},
            {"uai", "y"}, {"v", "y"},    {"u", "u"},   {"i", "i"},    {"o", "o"},    {"uo", "o"}, {"un", "p"},
            {"a", "a"},   {"iong", "s"}, {"ong", "s"}, {"iang", "d"}, {"uang", "d"}, {"en", "f"}, {"eng", "g"},
            {"ang", "h"}, {"an", "j"},   {"ao", "k"},  {"ai", "l"},   {"ing", ";"},  {"ei", "z"}, {"ie", "x"},
            {"iao", "c"}, {"ui", "v"},   {"ou", "b"},  {"in", "n"},   {"ian", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetZiguangShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "ziguang",
        {
            {"sh", "i"},
            {"ch", "a"},
            {"zh", "u"},
        },
        {
            {"a", "oa"},
            {"ai", "op"},
            {"an", "or"},
            {"ang", "os"},
            {"ao", "oq"},
            {"e", "oe"},
            {"ei", "ok"},
            {"en", "ow"},
            {"eng", "ot"},
            {"er", "oj"},
            {"o", "oo"},
            {"ou", "oz"},
        },
        {
            {"ao", "q"},  {"en", "w"},  {"e", "e"},    {"an", "r"},   {"eng", "t"}, {"in", "y"},   {"uai", "y"},
            {"u", "u"},   {"i", "i"},   {"o", "o"},    {"uo", "o"},   {"ai", "p"},  {"a", "a"},    {"ang", "s"},
            {"ie", "d"},  {"ian", "f"}, {"iang", "g"}, {"uang", "g"}, {"ong", "h"}, {"iong", "h"}, {"iu", "j"},
            {"ei", "k"},  {"uan", "l"}, {"ing", ";"},  {"ou", "z"},   {"ia", "x"},  {"ua", "x"},   {"v", "v"},
            {"iao", "b"}, {"ui", "n"},  {"ue", "n"},   {"ve", "n"},   {"un", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetZhinengAbcShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "zhinengabc",
        {
            {"sh", "v"},
            {"ch", "e"},
            {"zh", "a"},
        },
        {
            {"a", "oa"},
            {"ai", "ol"},
            {"an", "oj"},
            {"ang", "oh"},
            {"ao", "ok"},
            {"e", "oe"},
            {"ei", "oq"},
            {"en", "of"},
            {"eng", "og"},
            {"er", "or"},
            {"o", "oo"},
            {"ou", "ob"},
        },
        {
            {"ei", "q"},
            {"ian", "w"},
            {"e", "e"},
            {"iu", "r"},
            {"iang", "t"},
            {"uang", "t"},
            {"ing", "y"},
            {"u", "u"},
            {"i", "i"},
            {"o", "o"},
            {"uo", "o"},
            {"uan", "p"},
            {"a", "a"},
            {"ong", "s"},
            {"iong", "s"},
            {"ia", "d"},
            {"ua", "d"},
            {"en", "f"},
            {"eng", "g"},
            {"ang", "h"},
            {"an", "j"},
            {"ao", "k"},
            {"ai", "l"},
            {"iao", "z"},
            {"ie", "x"},
            {"in", "c"},
            {"uai", "c"},
            {"v", "v"},
            {"ou", "b"},
            {"un", "n"},
            {"ue", "m"},
            {"ui", "m"},
            // The chart also prints "ve" on V, but then lv/nv would decode as both lü and lüe.
            // Standard ABC types lüe/nüe on M together with ue.
            {"ve", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetGuobiaoShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "guobiao",
        {
            {"sh", "u"},
            {"ch", "i"},
            {"zh", "v"},
        },
        {
            {"a", "aa"},
            {"ai", "ak"},
            {"an", "af"},
            {"ang", "ag"},
            {"ao", "ac"},
            {"e", "ae"},
            {"ei", "ab"},
            {"en", "ar"},
            {"eng", "ah"},
            {"er", "al"},
            {"o", "ao"},
            {"ou", "ap"},
        },
        {
            // "van"/"vn" on W/Z are the same keys as uan/un after j/q/x/y, so they need no entry.
            {"ia", "q"},  {"ua", "q"},   {"uan", "w"},  {"e", "e"},    {"en", "r"},  {"ie", "t"},  {"iu", "y"},
            {"uai", "y"}, {"u", "u"},    {"i", "i"},    {"o", "o"},    {"uo", "o"},  {"ou", "p"},  {"a", "a"},
            {"ong", "s"}, {"iong", "s"}, {"ian", "d"},  {"an", "f"},   {"ang", "g"}, {"eng", "h"}, {"ing", "j"},
            {"ai", "k"},  {"in", "l"},   {"un", "z"},   {"ue", "x"},   {"ve", "x"},  {"ao", "c"},  {"v", "v"},
            {"ui", "v"},  {"ei", "b"},   {"iang", "n"}, {"uang", "n"}, {"iao", "m"},
        },
    };
    return profile;
}

const ShuangpinProfile &GetPinyinJiajiaShuangpinProfile()
{
    static const ShuangpinProfile profile{
        "pinyinjiajia",
        {
            {"sh", "i"},
            {"ch", "u"},
            {"zh", "v"},
        },
        {
            {"a", "aa"},
            {"ai", "as"},
            {"an", "af"},
            {"ang", "ag"},
            {"ao", "ad"},
            {"e", "ee"},
            {"ei", "ew"},
            {"en", "er"},
            {"eng", "et"},
            {"er", "eq"},
            {"o", "oo"},
            {"ou", "op"},
        },
        {
            {"ing", "q"}, {"ei", "w"}, {"e", "e"},   {"en", "r"},   {"eng", "t"},  {"ong", "y"}, {"iong", "y"},
            {"u", "u"},   {"i", "i"},  {"o", "o"},   {"uo", "o"},   {"ou", "p"},   {"a", "a"},   {"ai", "s"},
            {"ao", "d"},  {"an", "f"}, {"ang", "g"}, {"iang", "h"}, {"uang", "h"}, {"ian", "j"}, {"iao", "k"},
            {"in", "l"},  {"un", "z"}, {"ue", "x"},  {"ve", "x"},   {"uai", "x"},  {"uan", "c"}, {"ui", "v"},
            {"v", "v"},   {"ia", "b"}, {"ua", "b"},  {"iu", "n"},   {"ie", "m"},
        },
    };
    return profile;
}

bool ShuangpinProfileUsesSemicolonFinal(const ShuangpinProfile &profile)
{
    for (const auto &pair : profile.finals)
    {
        if (pair.second == ";")
        {
            return true;
        }
    }
    return false;
}

const ShuangpinProfile &GetShuangpinProfile(std::string_view name)
{
    if (name == "sogou")
    {
        return GetSogouShuangpinProfile();
    }
    if (name == "ziguang")
    {
        return GetZiguangShuangpinProfile();
    }
    if (name == "zhinengabc")
    {
        return GetZhinengAbcShuangpinProfile();
    }
    if (name == "guobiao")
    {
        return GetGuobiaoShuangpinProfile();
    }
    if (name == "pinyinjiajia")
    {
        return GetPinyinJiajiaShuangpinProfile();
    }
    if (name == "ziranma")
    {
        return GetZiranmaShuangpinProfile();
    }
    if (name == "shoudao")
    {
        return GetShoudaoShuangpinProfile();
    }
    if (name == "microsoft")
    {
        return GetMicrosoftShuangpinProfile();
    }
    if (const ShuangpinProfile *registered = FindRegisteredProfile(name))
    {
        return *registered;
    }
    return GetXiaoheShuangpinProfile();
}

const ShuangpinProfile &RegisterShuangpinProfile(ShuangpinProfile profile)
{
    auto &registry = Registry();
    std::lock_guard lock(registry.mutex);
    // Config reloads re-register the same file; only an edited layout needs new storage.
    if (const auto found = registry.by_name.find(profile.name); found != registry.by_name.end())
    {
        const ShuangpinProfile &current = *found->second;
        if (current.initials == profile.initials && current.zero_initials == profile.zero_initials &&
            current.finals == profile.finals)
        {
            return current;
        }
    }
    // Sessions hold profiles by reference, so a replaced version is kept rather than freed.
    const ShuangpinProfile &stored = registry.storage.emplace_back(std::move(profile));
    registry.by_name[stored.name] = &stored;
    return stored;
}
