#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "src/config/ime_config.h"
#include "src/config/ime_config_internal.h"
#include "engine/core/data_path.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "engine/shuangpin/shuangpin_query.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
std::string ReadExample()
{
    std::ifstream file{std::filesystem::path(MSIME_CUSTOM_SHUANGPIN_EXAMPLE_PATH), std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool AnyErrorContains(const std::vector<std::string> &errors, const std::string &needle)
{
    for (const auto &error : errors)
    {
        if (error.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

std::string JoinErrors(const std::vector<std::string> &errors)
{
    std::string text;
    for (const auto &error : errors)
        text += error + "\n";
    return text;
}

// Describes a built-in profile the way a user would write it in a custom file.
CustomShuangpinLayout LayoutOf(const ShuangpinProfile &profile, ShuangpinZeroInitialRule rule)
{
    CustomShuangpinLayout layout;
    layout.name = profile.name;
    layout.zero_initial_rule = rule;
    layout.initials = profile.initials;
    layout.finals = profile.finals;
    layout.finals["er"] = profile.zero_initials.at("er").substr(1);
    return layout;
}

// 自定义方案落在数据目录下，测试期间把数据目录换成一次性目录，免得写进开发机的安装数据。
// data_directory() 读的是 CRT 的环境副本，所以用 _wputenv_s 而不是 SetEnvironmentVariableW。
class ScopedDataRoot
{
  public:
    ScopedDataRoot()
    {
        wchar_t *previous = nullptr;
        size_t size = 0;
        if (_wdupenv_s(&previous, &size, L"METASEQUOIA_IME_DATA_DIR") == 0 && previous)
        {
            had_previous_ = true;
            previous_ = previous;
        }
        std::free(previous);
        root_ = test::TempRoot() / (L"msime-自定义双拼测试-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(custom_directory(), ec);
        REQUIRE(!ec);
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", root_.c_str());
    }
    ~ScopedDataRoot()
    {
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", had_previous_ ? previous_.c_str() : L"");
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    ScopedDataRoot(const ScopedDataRoot &) = delete;
    ScopedDataRoot &operator=(const ScopedDataRoot &) = delete;

    std::filesystem::path custom_directory() const
    {
        return root_ / L"shuangpin" / L"custom";
    }

    void Write(const std::wstring &file_name, const std::string &text) const
    {
        std::ofstream file(custom_directory() / file_name, std::ios::binary);
        file << text;
    }

  private:
    std::wstring previous_;
    bool had_previous_ = false;
    std::filesystem::path root_;
};

const char kMicrosoftLikeToml[] = R"(
name = "测试微软"
zero_initial_rule = "o"

[initials]
zh = "v"
ch = "i"
sh = "u"

[finals]
a = "a"
o = "o"
e = "e"
i = "i"
u = "u"
v = "y"
ai = "l"
ei = "z"
ui = "v"
ao = "k"
ou = "b"
iu = "q"
ie = "x"
ue = "t"
ve = "v"
an = "j"
en = "f"
in = "n"
un = "p"
ang = "h"
eng = "g"
ing = ";"
ong = "s"
ia = "w"
iao = "c"
ian = "m"
iang = "d"
iong = "s"
ua = "w"
uo = "o"
uai = "y"
uan = "r"
uang = "d"
)";
} // namespace

TEST_CASE(CustomShuangpinExampleFileReproducesXiaohe)
{
    const auto parsed = ParseCustomShuangpinSchema(ReadExample(), "custom/example");
    if (!parsed.errors.empty())
        std::fprintf(stderr, "%s", JoinErrors(parsed.errors).c_str());
    REQUIRE(parsed.errors.empty());
    REQUIRE_EQ(parsed.name, std::string("我的小鹤双拼"));
    REQUIRE_EQ(parsed.name_en, std::string("My Xiaohe Shuangpin"));
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    REQUIRE(parsed.profile.initials == xiaohe.initials);
    REQUIRE(parsed.profile.finals == xiaohe.finals);
    REQUIRE(parsed.profile.zero_initials == xiaohe.zero_initials);
}

TEST_CASE(CustomShuangpinZeroInitialRulesReproduceEveryBuiltInProfile)
{
    using Rule = ShuangpinZeroInitialRule;
    const std::vector<std::pair<const ShuangpinProfile *, Rule>> cases{
        {&GetXiaoheShuangpinProfile(), Rule::FullPinyin},
        {&GetZiranmaShuangpinProfile(), Rule::FullPinyin},
        {&GetShoudaoShuangpinProfile(), Rule::FullPinyin},
        {&GetMicrosoftShuangpinProfile(), Rule::LetterO},
        {&GetSogouShuangpinProfile(), Rule::LetterO},
        {&GetZiguangShuangpinProfile(), Rule::LetterO},
        {&GetZhinengAbcShuangpinProfile(), Rule::LetterO},
        {&GetGuobiaoShuangpinProfile(), Rule::LetterA},
        {&GetPinyinJiajiaShuangpinProfile(), Rule::FirstLetter},
    };
    for (const auto &[builtin, rule] : cases)
    {
        CustomShuangpinLayout layout = LayoutOf(*builtin, rule);
        if (builtin->name == "shoudao")
        {
            // 首道把 sh 放在 E 上，e/ei/eng 按规则会和 she/shi/sheng 撞车，只能逐个指定。
            layout.zero_initial_overrides = {{"e", "ue"}, {"ei", "ui"}, {"eng", "uf"}};
        }
        ShuangpinProfile profile;
        const auto errors = BuildCustomShuangpinProfile(layout, profile);
        if (!errors.empty())
            std::fprintf(stderr, "%s: %s", builtin->name.c_str(), JoinErrors(errors).c_str());
        REQUIRE(errors.empty());
        REQUIRE(profile.zero_initials == builtin->zero_initials);
        REQUIRE(profile.finals == builtin->finals);
    }
}

TEST_CASE(CustomShuangpinShoudaoWithoutOverridesReportsTheCollisions)
{
    ShuangpinProfile profile;
    const auto errors = BuildCustomShuangpinProfile(
        LayoutOf(GetShoudaoShuangpinProfile(), ShuangpinZeroInitialRule::FullPinyin), profile);
    REQUIRE(AnyErrorContains(errors, "音节 she 按 ee 输入时会被读成 e"));
    REQUIRE(AnyErrorContains(errors, "音节 sheng 按 ef 输入时会被读成 eng"));
    REQUIRE(profile.finals.empty());
}

TEST_CASE(CustomShuangpinParsesSemicolonFinalsAndDecodesThem)
{
    const auto parsed = ParseCustomShuangpinSchema(kMicrosoftLikeToml, "custom/ms");
    REQUIRE(parsed.errors.empty());
    REQUIRE_EQ(parsed.profile.name, std::string("custom/ms"));
    REQUIRE(parsed.profile.zero_initials == GetMicrosoftShuangpinProfile().zero_initials);
    REQUIRE(ShuangpinProfileUsesSemicolonFinal(parsed.profile));
    REQUIRE_EQ(shuangpin::normalize_input("m;", parsed.profile), std::string("ming"));
    REQUIRE_EQ(shuangpin::normalize_input("ud", parsed.profile), std::string("shuang"));
    REQUIRE_EQ(shuangpin::normalize_input("oh", parsed.profile), std::string("ang"));
}

TEST_CASE(CustomShuangpinZeroInitialOverrideReplacesOnlyThatSyllable)
{
    const std::string text = std::string(kMicrosoftLikeToml) + "\n[zero_initials]\ner = \"er\"\n";
    const auto parsed = ParseCustomShuangpinSchema(text, "custom/ms");
    REQUIRE(parsed.errors.empty());
    REQUIRE_EQ(parsed.profile.zero_initials.at("er"), std::string("er"));
    REQUIRE_EQ(parsed.profile.zero_initials.at("ang"), std::string("oh"));
    REQUIRE_EQ(shuangpin::normalize_input("er", parsed.profile), std::string("er"));
}

TEST_CASE(CustomShuangpinAcceptsUppercaseAndSingleUeOrVe)
{
    std::string text = kMicrosoftLikeToml;
    text.replace(text.find("zh = \"v\""), 8, "ZH = \"V\"");
    text.replace(text.find("ve = \"v\"\n"), 9, "");
    const auto parsed = ParseCustomShuangpinSchema(text, "custom/ms");
    if (!parsed.errors.empty())
        std::fprintf(stderr, "%s", JoinErrors(parsed.errors).c_str());
    REQUIRE(parsed.errors.empty());
    REQUIRE_EQ(parsed.profile.initials.at("zh"), std::string("v"));
    REQUIRE_EQ(parsed.profile.finals.at("ve"), std::string("t"));
}

TEST_CASE(CustomShuangpinReportsReadableErrors)
{
    const auto syntax = ParseCustomShuangpinSchema("zero_initial_rule = \"o\"\n[initials\n", "custom/x");
    REQUIRE(AnyErrorContains(syntax.errors, "第 2 行不是合法的 TOML"));

    const auto no_rule = ParseCustomShuangpinSchema("[initials]\nzh = \"v\"\n", "custom/x");
    REQUIRE(AnyErrorContains(no_rule.errors, "缺少 zero_initial_rule"));

    const auto bad_rule = ParseCustomShuangpinSchema("zero_initial_rule = \"b\"\n", "custom/x");
    REQUIRE(AnyErrorContains(bad_rule.errors, "zero_initial_rule 只能是"));

    const auto not_string = ParseCustomShuangpinSchema("zero_initial_rule = \"o\"\n[finals]\na = 1\n", "custom/x");
    REQUIRE(AnyErrorContains(not_string.errors, "[finals] 里 a 的值必须是用引号括起来的字符串"));

    const auto missing = ParseCustomShuangpinSchema("zero_initial_rule = \"o\"\n[initials]\nzh = \"v\"\nch = "
                                                    "\"i\"\nsh = \"u\"\n[finals]\na = \"a\"\n",
                                                    "custom/x");
    REQUIRE(AnyErrorContains(missing.errors, "缺少韵母的按键：o、e、i"));

    std::string retroflex = kMicrosoftLikeToml;
    retroflex.replace(retroflex.find("sh = \"u\"\n"), 9, "");
    REQUIRE(AnyErrorContains(ParseCustomShuangpinSchema(retroflex, "custom/x").errors, "缺少声母 sh 的按键"));

    std::string initial_clash = kMicrosoftLikeToml;
    initial_clash.replace(initial_clash.find("zh = \"v\""), 8, "zh = \"b\"");
    REQUIRE(AnyErrorContains(ParseCustomShuangpinSchema(initial_clash, "custom/x").errors, "都在 b 键上"));

    std::string final_clash = kMicrosoftLikeToml;
    final_clash.replace(final_clash.find("ong = \"s\""), 9, "ong = \"h\"");
    REQUIRE(AnyErrorContains(ParseCustomShuangpinSchema(final_clash, "custom/x").errors, "输入时会被读成"));

    std::string bad_key = kMicrosoftLikeToml;
    bad_key.replace(bad_key.find("ai = \"l\""), 8, "ai = \"ll\"");
    REQUIRE(AnyErrorContains(ParseCustomShuangpinSchema(bad_key, "custom/x").errors,
                             "韵母 ai 的按键必须是一个英文字母或分号"));

    const std::string bad_override = std::string(kMicrosoftLikeToml) + "\n[zero_initials]\nyi = \"yi\"\nan = \"o\"\n";
    const auto overrides = ParseCustomShuangpinSchema(bad_override, "custom/x");
    REQUIRE(AnyErrorContains(overrides.errors, "「yi」不是零声母音节"));
    REQUIRE(AnyErrorContains(overrides.errors, "零声母 an 的编码必须是两个键"));

    const std::string duplicate_zero = std::string(kMicrosoftLikeToml) + "\n[zero_initials]\nan = \"oh\"\n";
    REQUIRE(AnyErrorContains(ParseCustomShuangpinSchema(duplicate_zero, "custom/x").errors, "的编码都是 oh"));
}

TEST_CASE(RegisteredShuangpinProfilesResolveByNameAndStayAlive)
{
    ShuangpinProfile first = GetMicrosoftShuangpinProfile();
    first.name = "custom/注册测试";
    const ShuangpinProfile &stored = RegisterShuangpinProfile(first);
    REQUIRE_EQ(&GetShuangpinProfile("custom/注册测试"), &stored);
    // 内容相同的重复登记复用同一份，重新读文件不会让内存越涨越多。
    REQUIRE_EQ(&RegisterShuangpinProfile(first), &stored);

    ShuangpinProfile second = first;
    second.finals["ing"] = "k";
    const ShuangpinProfile &replacement = RegisterShuangpinProfile(second);
    REQUIRE(&replacement != &stored);
    REQUIRE_EQ(&GetShuangpinProfile("custom/注册测试"), &replacement);
    // 旧会话手里的引用仍然有效。
    REQUIRE_EQ(stored.finals.at("ing"), std::string(";"));
}

TEST_CASE(CustomShuangpinDirectoryIsScannedAndLoaded)
{
    ScopedDataRoot root;
    root.Write(L"微软改.toml", kMicrosoftLikeToml);
    root.Write(L"broken.toml", "zero_initial_rule = \"x\"\n");
    root.Write(L"note.txt", "not a schema");

    const auto schemas = GetCustomShuangpinSchemas();
    REQUIRE_EQ(schemas.size(), static_cast<size_t>(2));
    REQUIRE_EQ(schemas[0].schema, std::string("custom/broken"));
    REQUIRE_EQ(schemas[0].name, std::string("broken"));
    REQUIRE(!schemas[0].error.empty());
    REQUIRE_EQ(schemas[1].schema, std::string("custom/微软改"));
    REQUIRE_EQ(schemas[1].name, std::string("测试微软"));
    REQUIRE(schemas[1].error.empty());
    REQUIRE_EQ(std::filesystem::path(metasequoia::path_from_utf8(GetCustomShuangpinDirectory().c_str())),
               root.custom_directory());

    REQUIRE(ime_config_detail::LoadShuangpinSchema("custom/微软改"));
    REQUIRE_EQ(GetShuangpinProfile("custom/微软改").name, std::string("custom/微软改"));
    REQUIRE_EQ(shuangpin::normalize_input("m;", GetShuangpinProfile("custom/微软改")), std::string("ming"));

    REQUIRE(!ime_config_detail::LoadShuangpinSchema("custom/broken"));
    REQUIRE(!ime_config_detail::LoadShuangpinSchema("custom/missing"));
    REQUIRE(!ime_config_detail::LoadShuangpinSchema("custom/../微软改"));
    REQUIRE(!ime_config_detail::LoadShuangpinSchema("custom/"));
    REQUIRE(!ime_config_detail::LoadShuangpinSchema("not-a-schema"));
    REQUIRE(ime_config_detail::LoadShuangpinSchema("microsoft"));
}
