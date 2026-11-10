#include "engine/contracts/candidate_action_key.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_action_key_parses_default_and_aliases)
{
    FanyImeKeyChord::Binding binding;
    REQUIRE(FanyImeCandidateActionKey::Parse("Backquote", binding));
    REQUIRE_EQ(binding.key, static_cast<unsigned>(0xc0));
    REQUIRE_EQ(binding.modifiers, 0u);
    REQUIRE(binding.Matches(0xc0, 0));
    REQUIRE(!binding.Matches(0xc0, FanyImeKeyChord::Shift));

    REQUIRE(FanyImeCandidateActionKey::Parse("GraveAccent", binding));
    REQUIRE_EQ(binding.key, static_cast<unsigned>(0xc0));
    REQUIRE_EQ(binding.modifiers, 0u);
}

TEST_CASE(candidate_action_key_parses_ctrl_chords)
{
    FanyImeKeyChord::Binding binding;
    REQUIRE(FanyImeCandidateActionKey::Parse("Ctrl+Backquote", binding));
    REQUIRE_EQ(binding.key, static_cast<unsigned>(0xc0));
    REQUIRE_EQ(binding.modifiers, FanyImeKeyChord::Control);
    REQUIRE(binding.Matches(0xc0, FanyImeKeyChord::Control));
    REQUIRE(!binding.Matches(0xc0, FanyImeKeyChord::Control | FanyImeKeyChord::Shift));

    REQUIRE(FanyImeCandidateActionKey::Parse("Ctrl+Shift+Backquote", binding));
    REQUIRE_EQ(binding.modifiers, FanyImeKeyChord::Control | FanyImeKeyChord::Shift);
    REQUIRE(binding.Matches(0xc0, FanyImeKeyChord::Control | FanyImeKeyChord::Shift));
}

TEST_CASE(candidate_action_key_rejects_bare_non_backquote_and_invalid_chords)
{
    FanyImeKeyChord::Binding binding;
    REQUIRE(FanyImeCandidateActionKey::Parse("", binding));
    REQUIRE_EQ(binding.key, 0u);

    REQUIRE(!FanyImeCandidateActionKey::Parse("KeyA", binding));
    REQUIRE_EQ(binding.key, 0u);
    REQUIRE(!FanyImeCandidateActionKey::Parse("Shift+Backquote", binding));
    REQUIRE(!FanyImeCandidateActionKey::Parse("Alt+Backquote", binding));
    REQUIRE(!FanyImeCandidateActionKey::Parse("Ctrl+Alt+Backquote", binding));
    REQUIRE(!FanyImeCandidateActionKey::Parse("Ctrl+Shift+Alt+Backquote", binding));
}
