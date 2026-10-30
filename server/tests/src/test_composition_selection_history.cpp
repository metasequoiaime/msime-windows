#include "global/globals.h"
#include "ipc/input_key_policy.h"
#include "tests/includes/test_framework.h"

namespace
{
GlobalIme::CreatingWordState MakeCreatingWord(const char *pinyin, const char *word)
{
    GlobalIme::CreatingWordState state;
    state.pinyin = pinyin;
    state.word = word;
    state.active = true;
    return state;
}
} // namespace

TEST_CASE(composition_selection_snapshots_restore_raw_and_previous_word)
{
    GlobalIme::CompositionState composition;

    // First selection ("te" -> 特) has nothing accumulated before it.
    composition.push_selection_snapshot("te");
    REQUIRE_EQ(composition.selection_history.size(), std::size_t(1));

    // Second selection ("le" -> 乐) has the earlier 特 already accumulated.
    composition.creating_word = MakeCreatingWord("te", "特");
    composition.push_selection_snapshot("le");
    REQUIRE_EQ(composition.selection_history.size(), std::size_t(2));

    // Retracting "le" returns to candidates for "le" with the earlier 特 kept.
    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("le"));
    REQUIRE_EQ(composition.caret_position, std::size_t(2));
    REQUIRE(composition.creating_word.active);
    REQUIRE_EQ(composition.creating_word.word, std::string("特"));
    REQUIRE_EQ(composition.creating_word.pinyin, std::string("te"));

    // Retracting "te" prepends it in front of the just-restored "le": the whole
    // original spelling is back and the caret keeps its distance to the suffix,
    // so nothing the user typed is lost between the two retracts.
    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("tele"));
    REQUIRE_EQ(composition.caret_position, std::size_t(4));
    REQUIRE(!composition.creating_word.active);
    REQUIRE(composition.creating_word.word.empty());
    REQUIRE(composition.creating_word.pinyin.empty());

    // Nothing left to retract.
    REQUIRE(!composition.restore_last_selection());
}

TEST_CASE(composition_restore_at_caret_zero_prepends_before_the_suffix)
{
    GlobalIme::CompositionState composition;

    // Selecting 你好 from "nihaoya" consumed "nihao" and left "ya" behind; the
    // caret then moved to stand right behind the selected word, i.e. at the
    // start of the remaining raw. The snapshot records the state before that
    // selection: no accumulated word yet.
    composition.push_selection_snapshot("nihao");
    composition.creating_word = MakeCreatingWord("nihao", "你好");
    composition.raw_input_with_cases = "ya";
    composition.caret_position = 0;

    REQUIRE(composition.restore_last_selection());
    // The spelling goes back in front of the untouched suffix so no typed input
    // is lost, and the caret lands where the restored word ends.
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("nihaoya"));
    REQUIRE_EQ(composition.caret_position, std::size_t(5));
    REQUIRE(!composition.creating_word.active);
    REQUIRE(composition.creating_word.word.empty());
}

TEST_CASE(composition_restore_at_end_of_raw_keeps_the_caret_at_the_end)
{
    GlobalIme::CompositionState composition;

    // Selecting 我们 from "womendjia" consumed "womende" and left "jia"; the
    // caret stands at the end of that suffix where the Backspace was pressed.
    composition.push_selection_snapshot("womende");
    composition.creating_word = MakeCreatingWord("womende", "我们");
    composition.raw_input_with_cases = "jia";
    composition.caret_position = 3;

    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("womendejia"));
    // The caret shifts with the insertion and stays at the end of the raw: the
    // next Backspace deletes through the suffix first, Rime's "the caret does
    // not move" over a fixed input string.
    REQUIRE_EQ(composition.caret_position, std::size_t(10));
    REQUIRE(!composition.creating_word.active);
}

TEST_CASE(composition_typing_after_selection_locks_only_the_newest_snapshot)
{
    GlobalIme::CompositionState composition;
    REQUIRE(!composition.last_selection_raw_edited());

    composition.push_selection_snapshot("te");
    REQUIRE(!composition.last_selection_raw_edited());

    // Typing after the selection locks that snapshot (selected_before_editing).
    composition.note_raw_inserted();
    REQUIRE(composition.last_selection_raw_edited());

    // A later, unlocked selection above it becomes the newest decision again.
    composition.creating_word = MakeCreatingWord("te", "特");
    composition.push_selection_snapshot("le");
    REQUIRE(!composition.last_selection_raw_edited());

    // Popping the unlocked snapshot brings the lock back: it never leaked away.
    REQUIRE(composition.restore_last_selection());
    REQUIRE(composition.last_selection_raw_edited());
}

TEST_CASE(composition_restore_hands_back_the_picked_candidate_position)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("te", /*selected_absolute_index=*/3, /*selected_page_prefix=*/"ni");
    REQUIRE_EQ(composition.take_restored_selection_highlight().absolute_index, -1);

    REQUIRE(composition.restore_last_selection());
    const GlobalIme::RestoredSelectionHighlight highlight = composition.take_restored_selection_highlight();
    REQUIRE_EQ(highlight.absolute_index, 3);
    // The page identity travels with the position: the caller can only apply it
    // after rebuilding a page with the same prefix.
    REQUIRE_EQ(highlight.page_prefix, std::string("ni"));
    // One-shot: consuming again must not re-apply a stale highlight to a page
    // that has moved on.
    REQUIRE_EQ(composition.take_restored_selection_highlight().absolute_index, -1);

    // Ending the composition clears a position still waiting to be applied.
    composition.restored_selection_highlight = {7, "le"};
    composition.clear();
    REQUIRE_EQ(composition.take_restored_selection_highlight().absolute_index, -1);
}

TEST_CASE(composition_selection_snapshot_requires_a_consumed_spelling)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("");
    REQUIRE(composition.selection_history.empty());
    REQUIRE(!composition.restore_last_selection());
    REQUIRE(!composition.drop_last_selection());
}

TEST_CASE(composition_drop_last_selection_discards_the_spelling)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("te");
    composition.creating_word = MakeCreatingWord("te", "特");
    composition.push_selection_snapshot("le");

    // Ctrl+Backspace drops 乐 without restoring "le": the accumulated 特 stays
    // and the raw spelling the segment consumed is gone for good.
    REQUIRE(composition.drop_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string(""));
    REQUIRE_EQ(composition.caret_position, std::size_t(0));
    REQUIRE(composition.creating_word.active);
    REQUIRE_EQ(composition.creating_word.word, std::string("特"));

    // Dropping the earlier 特 ends the word state; nothing is left to drop.
    REQUIRE(composition.drop_last_selection());
    REQUIRE(!composition.creating_word.active);
    REQUIRE(composition.selection_history.empty());
    REQUIRE(!composition.drop_last_selection());
}

TEST_CASE(composition_clear_drops_history_while_clear_creating_word_keeps_it)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("te");

    // Finishing a word inside a still-running composition keeps the history.
    composition.clear_creating_word();
    REQUIRE_EQ(composition.selection_history.size(), std::size_t(1));

    // Ending the composition drops everything a later retraction could misuse.
    composition.clear();
    REQUIRE(composition.selection_history.empty());
    REQUIRE(!composition.restore_last_selection());
}

// 繁体写法的造词状态（显示文字、第一写法标记、前缀文字）随撤回快照一起恢复，清空时回到初值。
TEST_CASE(composition_selection_snapshots_restore_the_traditional_spelling_state)
{
    GlobalIme::CompositionState composition;
    composition.creating_word = MakeCreatingWord("gan", "干");
    composition.creating_word.output_text = "乾";
    composition.creating_word.all_first_spellings = false;
    composition.creating_word.prefix_text = "乾";
    composition.push_selection_snapshot("zao");

    composition.creating_word.word = "干燥";
    composition.creating_word.output_text = "乾燥";
    composition.creating_word.all_first_spellings = true;
    composition.creating_word.prefix_text = "乾燥";
    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.creating_word.output_text, std::string("乾"));
    REQUIRE(!composition.creating_word.all_first_spellings);
    REQUIRE_EQ(composition.creating_word.prefix_text, std::string("乾"));

    composition.clear_creating_word();
    REQUIRE(composition.creating_word.output_text.empty());
    REQUIRE(composition.creating_word.all_first_spellings);
    REQUIRE(composition.creating_word.prefix_text.empty());
}

TEST_CASE(composition_terminal_helpcode_restore_preserves_current_suffix_and_caret)
{
    for (const std::string tail : {"f", "fR", "|f", "|fR", "Fr"})
    {
        for (const std::string suffix : {"hc", "h", ""})
        {
            for (const std::size_t caret : {std::size_t(0), std::size_t(1), suffix.size(), std::size_t(99)})
            {
                GlobalIme::CompositionState composition;
                composition.creating_word = MakeCreatingWord("wo", "我");
                composition.push_selection_snapshot("ni'", 2, "nihc", tail);
                composition.creating_word = MakeCreatingWord("wo'ni", "我拟");
                composition.raw_input_with_cases = suffix;
                composition.caret_position = caret;
                REQUIRE(composition.restore_last_selection());
                REQUIRE_EQ(composition.raw_input_with_cases, std::string("ni'") + suffix + tail);
                const auto clamped = (std::min)(caret, suffix.size());
                REQUIRE_EQ(composition.caret_position, 3 + clamped + (clamped == suffix.size() ? tail.size() : 0));
                REQUIRE_EQ(composition.creating_word.word, std::string("我"));
                const auto highlight = composition.take_restored_selection_highlight();
                REQUIRE_EQ(highlight.absolute_index, 2);
                REQUIRE_EQ(highlight.page_prefix, std::string("nihc"));
                REQUIRE(!composition.restore_last_selection());
                REQUIRE_EQ(composition.raw_input_with_cases, std::string("ni'") + suffix + tail);
            }
        }
    }
}

TEST_CASE(composition_terminal_helpcode_history_restores_each_selection_once)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("ni", -1, "", "|fR");
    composition.creating_word = MakeCreatingWord("ni", "拟");
    composition.note_raw_inserted();
    composition.push_selection_snapshot("hc");
    composition.creating_word = MakeCreatingWord("ni'hao", "拟好");
    composition.raw_input_with_cases = "buhc";
    composition.caret_position = 4;
    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("hcbuhc"));
    REQUIRE_EQ(composition.creating_word.word, std::string("拟"));
    REQUIRE(composition.last_selection_raw_edited());
    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("nihcbuhc|fR"));
    REQUIRE_EQ(composition.caret_position, std::size_t(11));
    REQUIRE(!composition.creating_word.active);
    REQUIRE(!composition.restore_last_selection());
}

TEST_CASE(composition_terminal_helpcode_drop_and_clear_never_restore_the_tail)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("ni", -1, "", "|fR");
    composition.creating_word = MakeCreatingWord("ni", "拟");
    REQUIRE(composition.drop_last_selection());
    REQUIRE(composition.raw_input_with_cases.empty());
    REQUIRE(!composition.restore_last_selection());
    composition.push_selection_snapshot("ni", -1, "", "|fR");
    composition.clear();
    REQUIRE(!composition.restore_last_selection());
    REQUIRE(composition.raw_input_with_cases.empty());
}

TEST_CASE(composition_terminal_helpcode_restore_respects_edit_lock_and_empty_raw_override)
{
    GlobalIme::CompositionState composition;
    composition.push_selection_snapshot("ni", -1, "", "|fR");
    composition.creating_word = MakeCreatingWord("ni", "拟");
    composition.raw_input_with_cases = "hcx";
    composition.caret_position = 3;
    composition.note_raw_inserted();
    const auto should_retreat = [&] {
        return FanyImeIpc::ShouldRetreatCreatingWordSelection(
            composition.creating_word.active, false, true, composition.raw_input_with_cases.size(),
            composition.selection_history.size(), composition.last_selection_raw_edited());
    };
    REQUIRE(!should_retreat());
    composition.raw_input_with_cases = "hc";
    REQUIRE(!should_retreat());
    composition.raw_input_with_cases.clear();
    composition.caret_position = 0;
    REQUIRE(should_retreat());
    REQUIRE(composition.restore_last_selection());
    REQUIRE_EQ(composition.raw_input_with_cases, std::string("ni|fR"));
    REQUIRE_EQ(composition.caret_position, std::size_t(5));
}
