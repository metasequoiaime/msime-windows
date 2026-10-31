#pragma once

// 候选展开面板的类别无关框架。面板 = 若干「提供方(provider)」按调用方给定的顺序产出的条目
// 平铺拼接、连续编号。这里只定义类别元数据（id、设置页显示名、淡色前缀标签）、提供方接口
// 与平铺管线；具体类别的取数在 ProduceForCategory 里。翻译从上下文里的 gloss 拆义项，emoji 与
// 颜文字把调用方预取的对应条目套上前缀。
//
// 与 candidate_translation_policy.h 一样是 header-only 纯字节策略：全程用 UTF-8 std::string，
// 不碰 wstring、不链接 Server 工具，测试可直接钉住管线。wstring 转换只发生在 Server 填快照的边界。
// 注意与 candidate_expansion_policy.h 区分：那份是候选列表「展开更多候选、保持页位置」，与此无关。

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "candidate_translation_policy.h"

namespace FanyImeIpc
{
// 类别 id。稳定字符串，进配置、进快照、进调试日志；改名要同步迁移。
inline constexpr std::string_view kCategoryTranslation = "translation";
inline constexpr std::string_view kCategoryEmoji = "emoji";
inline constexpr std::string_view kCategoryKaomoji = "kaomoji";
inline constexpr std::string_view kCategorySymbols = "symbols";

// 一个类别的静态元数据。prefix_tag 是面板里数字后面那段淡色文字，每个类别自带自己的；
// display_name 给设置页用。新增类别 = 在这里加一行 + 在 ProduceForCategory 里实现取数。
// 非 ASCII 一律写成 UTF-8 字节转义，和 SplitTranslationGloss 里的全角分号同一种写法。
struct ExpansionCategory
{
    std::string id;
    std::string display_name;
    std::string prefix_tag;
};

// 已知类别，按默认顺序（翻译 → emoji → 颜文字 → 符号）。这是框架的注册表；是否启用、实际顺序由
// 调用方（读配置的 Server）决定，这里只提供默认与元数据。
inline std::vector<ExpansionCategory> KnownExpansionCategories()
{
    return {
        {std::string(kCategoryTranslation), "\xE7\xBF\xBB\xE8\xAF\x91", "\xE8\xAF\x91"},
        {std::string(kCategoryEmoji), "Emoji", "\xE8\xA1\xA8\xE6\x83\x85"},
        {std::string(kCategoryKaomoji), "\xE9\xA2\x9C\xE6\x96\x87\xE5\xAD\x97", "\xE9\xA2\x9C"},
        {std::string(kCategorySymbols), "\xE7\xAC\xA6\xE5\x8F\xB7", "\xE7\xAC\xA6"},
    };
}

inline const ExpansionCategory *FindExpansionCategory(std::string_view id)
{
    static const std::vector<ExpansionCategory> kCategories = KnownExpansionCategories();
    for (const auto &category : kCategories)
    {
        if (category.id == id)
            return &category;
    }
    return nullptr;
}

// 提供方产出的一个面板条目。display 是面板上显示的文字（翻译可按 show_pos 带词性），
// commit 是上屏写进文档的文字（翻译始终剥词性；emoji/颜文字即其本身）。tag 是类别前缀，
// 由提供方从类别元数据填，presenter 原样画。全部 UTF-8。
struct ExpansionItem
{
    std::string display;
    std::string commit;
    std::string tag;
    std::string category_id;
};

// 提供方取数的上下文。以「高亮候选」为锚点，不是用户输入串。翻译用 anchor_gloss（已在
// page_glosses 里，UTF-8）；emoji、颜文字与符号由调用方（Server）按 anchor_word 查 others.db 预取进
// emoji_candidates / kaomoji_candidates / symbols_candidates（见 engine 的 query_emoji_by_keyword、
// query_kaomoji_by_keyword、query_symbols_by_keyword），这里保持纯字节、不碰 DB，测试可直接喂一批
// emoji/颜文字/符号钉住「套前缀 + 拼接」管线。
struct ExpansionContext
{
    std::string anchor_word;
    std::string anchor_pinyin;
    std::string anchor_gloss;
    std::vector<std::string> emoji_candidates;
    std::vector<std::string> kaomoji_candidates;
    std::vector<std::string> symbols_candidates;
    bool show_pos = true;
    // 每个类别各自的显示上限（调用方按配置填入：UTF-8 类别 id → 上限）。缺省、0 或负值都表示不限制
    // （0 = 不限制条数，不是隐藏该类；隐藏类别走各自的启用开关）；
    // BuildExpansionItems 据此对每个类别的产出截断，翻译与 emoji/颜文字一视同仁。上限是类别无关的
    // 框架能力，不绑定任何具体类别。
    std::map<std::string, int> category_max_count;
};

// 单个类别的取数。翻译从 anchor_gloss 拆义项；emoji、颜文字与符号把预取的对应条目套上类别前缀、保持原顺序。
inline std::vector<ExpansionItem> ProduceForCategory(std::string_view category_id, const ExpansionContext &context)
{
    std::vector<ExpansionItem> items;
    const ExpansionCategory *category = FindExpansionCategory(category_id);
    if (category == nullptr)
        return items;
    if (category_id == kCategoryTranslation)
    {
        // 保留词性的完整义项，逐条同时算出展示与上屏文本，保证 display/commit 一一对应。
        for (const auto &sense : SplitTranslationGloss(context.anchor_gloss, false))
        {
            const std::string_view stripped = StripPartOfSpeechLabel(sense);
            ExpansionItem item;
            item.category_id = category->id;
            item.tag = category->prefix_tag;
            item.display = context.show_pos ? std::string(sense) : std::string(stripped);
            item.commit = std::string(stripped);
            items.push_back(std::move(item));
        }
        return items;
    }
    if (category_id == kCategoryEmoji)
    {
        // emoji 即其本身：display 与 commit 相同，没有词性可剥。顺序沿用预取顺序（others.db 目录序）。
        for (const auto &emoji : context.emoji_candidates)
        {
            ExpansionItem item;
            item.category_id = category->id;
            item.tag = category->prefix_tag;
            item.display = emoji;
            item.commit = emoji;
            items.push_back(std::move(item));
        }
        return items;
    }
    if (category_id == kCategoryKaomoji)
    {
        // 颜文字与 emoji 同构：即其本身，display 与 commit 相同，顺序沿用预取顺序（others.db 目录序）。
        for (const auto &kaomoji : context.kaomoji_candidates)
        {
            ExpansionItem item;
            item.category_id = category->id;
            item.tag = category->prefix_tag;
            item.display = kaomoji;
            item.commit = kaomoji;
            items.push_back(std::move(item));
        }
        return items;
    }
    if (category_id == kCategorySymbols)
    {
        // 符号与 emoji/颜文字同构：即其本身，display 与 commit 相同，顺序沿用预取顺序（others.db 目录序）。
        for (const auto &symbol : context.symbols_candidates)
        {
            ExpansionItem item;
            item.category_id = category->id;
            item.tag = category->prefix_tag;
            item.display = symbol;
            item.commit = symbol;
            items.push_back(std::move(item));
        }
        return items;
    }
    return items;
}

// 平铺管线：按 enabled_order 给定的顺序，把每个启用类别的条目依次拼接成一个扁平列表。
// 调用方（Server）决定启用哪些、什么顺序；这里只负责按序拼接。连续编号由 presenter 负责。
// 每个类别的条目在拼接前先按 context.category_max_count 里该类别的上限截断（缺省/0/负值=不限制，
// 0 是「不限制条数」而不是「不显示该类」），翻译同样受限——「每类最多显示」是框架级能力，不是某个类别的私有逻辑。
inline std::vector<ExpansionItem> BuildExpansionItems(const std::vector<std::string> &enabled_order,
                                                      const ExpansionContext &context)
{
    std::vector<ExpansionItem> items;
    for (const auto &id : enabled_order)
    {
        std::vector<ExpansionItem> produced = ProduceForCategory(id, context);
        const auto limit = context.category_max_count.find(id);
        if (limit != context.category_max_count.end() && limit->second > 0 &&
            static_cast<int>(produced.size()) > limit->second)
        {
            produced.resize(static_cast<size_t>(limit->second));
        }
        items.insert(items.end(), std::make_move_iterator(produced.begin()), std::make_move_iterator(produced.end()));
    }
    return items;
}
} // namespace FanyImeIpc
