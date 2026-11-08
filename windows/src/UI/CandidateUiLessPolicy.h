#pragma once

// The TSF host has two ways to tell the IME who draws the candidate UI:
// the activation flags, sampled once in ActivateEx, and the per-composition
// answer to ITfUIElementMgr::BeginUIElement. Games often activate the TIP
// without TF_TMF_UIELEMENTENABLEDONLY and still return pbShow=FALSE for every
// composition. The BeginUIElement answer is the one that describes the
// composition in front of the user, so it must win.
namespace CandidateUiLessPolicy
{
// Mirrors the _isShowMode decision in CCandidateListUIPresenter::BeginUIElement.
// true means the host paints the UI element, false means the IME owns rendering.
constexpr bool HostDrawsElement(bool host_show, bool ignore_host_ui_less, bool activate_ui_less)
{
    if (ignore_host_ui_less)
    {
        return true;
    }
    if (activate_ui_less)
    {
        return false;
    }
    return host_show;
}

// The first key of a composition is sent before that composition's
// BeginUIElement can run. When the host turns out to draw candidates itself,
// that key went to the Server without the UI-less flag and the Server skipped
// the candidate page. Ask for the page once the answer is known, so the first
// character is not left without a candidate list.
constexpr bool ShouldRequestPageAfterBegin(bool was_ui_less, bool host_draws_element)
{
    return !was_ui_less && !host_draws_element;
}
} // namespace CandidateUiLessPolicy
