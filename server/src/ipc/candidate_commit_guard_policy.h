#pragma once

// Guards the candidate page a commit or selection is about to use.
//
// The Server's commit paths read Global::candidate_ui directly. That page is
// built by PrepareCandidateList for a particular input string. When the page
// was never built for the composition that is on screen, a space silently
// degrades to OutofRange and a punctuation key replaces the preedit with the
// punctuation alone. Both look like the first character was swallowed.
namespace FanyImeIpc
{
struct CommitPageState
{
    bool page_has_words = false;
    bool page_matches_composition = false;
    bool composition_empty = false;
};

// An empty composition with a live page is the pure-punctuation or
// creating-word case: the page belongs to whatever word is being assembled,
// so it is usable without an input-string identity.
constexpr bool CommitPageUsable(const CommitPageState &state)
{
    return state.page_has_words && (state.composition_empty || state.page_matches_composition);
}

// Only a non-empty composition can be queried. An empty composition with no
// page is not a missing page; it is the pure-punctuation case.
constexpr bool ShouldRebuildCommitPage(const CommitPageState &state)
{
    return !state.composition_empty && !CommitPageUsable(state);
}

// A punctuation key with a live composition must never answer Normal with an
// empty candidate string: TSF would replace the preedit with the punctuation
// and delete the typed characters. An out-of-range highlight is the same
// hazard even when the page itself has words.
constexpr bool ShouldRejectPunctuationCommit(bool has_active_composition, bool page_has_candidate,
                                             bool highlighted_text_empty)
{
    return has_active_composition && (!page_has_candidate || highlighted_text_empty);
}
} // namespace FanyImeIpc
