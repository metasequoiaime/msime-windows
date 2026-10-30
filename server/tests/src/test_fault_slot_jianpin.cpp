#include "tests/includes/test_framework.h"
#include "engine/quanpin/quanpin_utils.h"

#include <string>
#include <vector>

namespace
{
using quanpin::FaultSlot;
using quanpin::FaultSlotKind;
using quanpin::Segments;

FaultSlot MakeSlot(const char *raw, FaultSlotKind kind, const char *base = "")
{
    FaultSlot slot;
    slot.raw_text = raw;
    slot.base_syllable = base;
    slot.kind = kind;
    return slot;
}
} // namespace

// 容错槽位把单字母段展开成同首字母的完整音节时，改写的是简拼本身，不是拼写纠错。
// 这类候选的 corrected_from 仍然保留给预编辑和重排用，但候选窗不加 * 号。
TEST_CASE(jianpin_expansion_is_not_a_correction)
{
    const std::vector<FaultSlot> slots = {MakeSlot("b", FaultSlotKind::SingleInitial),
                                          MakeSlot("z", FaultSlotKind::SingleInitial),
                                          MakeSlot("ni", FaultSlotKind::Determining, "ni")};
    REQUIRE(quanpin::fault_winner_is_pure_jianpin(slots, Segments{"bu", "zhi", "ni"}));
    // 单字母段原样保留，没有展开，同样不是纠错。
    REQUIRE(quanpin::fault_winner_is_pure_jianpin(slots, Segments{"b", "z", "ni"}));
}

TEST_CASE(fault_slot_keeps_the_correction_marker)
{
    const std::vector<FaultSlot> slots = {MakeSlot("b", FaultSlotKind::SingleInitial),
                                          MakeSlot("rfa", FaultSlotKind::Fault)};
    REQUIRE(!quanpin::fault_winner_is_pure_jianpin(slots, Segments{"bu", "ru"}));
}

TEST_CASE(rewritten_complete_syllable_keeps_the_correction_marker)
{
    const std::vector<FaultSlot> slots = {MakeSlot("ti", FaultSlotKind::Determining, "ti"),
                                          MakeSlot("yang", FaultSlotKind::Determining, "yang")};
    REQUIRE(!quanpin::fault_winner_is_pure_jianpin(slots, Segments{"ti", "yan"}));
    // ü 的两种写法归一到词库写法后仍然相同，不算改读。
    REQUIRE(
        quanpin::fault_winner_is_pure_jianpin({MakeSlot("nu", FaultSlotKind::Determining, "nue")}, Segments{"nve"}));
}

TEST_CASE(initial_expansion_must_share_the_first_letter)
{
    const std::vector<FaultSlot> slots = {MakeSlot("b", FaultSlotKind::SingleInitial)};
    REQUIRE(!quanpin::fault_winner_is_pure_jianpin(slots, Segments{"pi"}));
}

TEST_CASE(slot_and_reading_count_must_match)
{
    const std::vector<FaultSlot> slots = {MakeSlot("b", FaultSlotKind::SingleInitial),
                                          MakeSlot("z", FaultSlotKind::SingleInitial)};
    REQUIRE(!quanpin::fault_winner_is_pure_jianpin(slots, Segments{"bu"}));
}
