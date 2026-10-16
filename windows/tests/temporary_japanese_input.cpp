// Exercise the production TSF key result and shared editing rules without a TIP or host doubles.
#define NOMINMAX
#include "../src/Key/TemporaryJapaneseKeyPolicy.h"
#include <iostream>
#include <stdexcept>
#include <string>

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
            throw std::runtime_error(#x);                                                                              \
    } while (false)

namespace
{
using namespace FanyImeTemporaryJapanese;

_KEYSTROKE_STATE Classify(const std::wstring &raw, WCHAR ch, bool full = false, Policy policy = Allowed,
                          UINT modifiers = 0)
{
    const UINT key = ch == L'-' || ch == L'_' ? VK_OEM_MINUS : VK_OEM_PLUS;
    return ClassifyJapaneseKey(key, ch, modifiers, full, policy, raw.data(), raw.size(), !raw.empty());
}

void Classification()
{
    for (Policy policy : {Policy(0), Policy(1), Policy(2), Policy(3)})
        for (bool full : {false, true})
            for (const std::wstring raw : {L"", L"R", L"Rko", L"rko", L"ni"})
            {
                const bool expected = full || (policy == Allowed && !raw.empty() && raw.front() == L'R');
                const auto state = Classify(raw, L'-', full, policy);
                CHECK((state.Function == FUNCTION_INPUT) == expected);
                CHECK(state.Category == (expected ? CATEGORY_COMPOSING : CATEGORY_NONE));
                // Repeated key tests must leave the buffer and policy untouched.
                CHECK(Classify(raw, L'-', full, policy).Function == state.Function);
                for (WCHAR punctuation : {L'_', L'=', L'+'})
                {
                    const auto symbol = Classify(raw, punctuation, full, policy);
                    CHECK((symbol.Function == FUNCTION_PUNCTUATION) == (expected && !raw.empty()));
                }
                for (UINT modifiers : {2u, 4u, 6u})
                    CHECK(Classify(raw, L'-', full, policy, modifiers).Function == FUNCTION_NONE);
            }
    // Fresh compositions intentionally ignore the previous session's R prefix.
    CHECK(Classify(L"", L'-').Function == FUNCTION_NONE);
    CHECK(Classify(L"", L'-', true).Function == FUNCTION_INPUT);
    CHECK(Classify(L"Rko-hi--", L'-').Function == FUNCTION_INPUT);
    CHECK(ClassifyJapaneseKey(VK_SUBTRACT, L'-', 0, false, Allowed, L"Rko", 3, true).Function == FUNCTION_NONE);
    CHECK(ClassifyJapaneseKey(VK_NEXT, 0, 0, false, Allowed, L"Rko", 3, true).Function == FUNCTION_NONE);
    CHECK(ClassifyJapaneseKey(VK_OEM_4, L'[', 0, false, Allowed, L"Rko", 3, true).Function == FUNCTION_NONE);
}

void Editing()
{
    std::wstring raw;
    std::size_t caret = 0;
    // Supply each queued buffer to the same classifier as the real TSF buffer.
    for (WCHAR ch : std::wstring(L"Rko-hi-"))
    {
        if (ch == L'-')
            CHECK(Classify(raw, ch).Function == FUNCTION_INPUT);
        caret = ClampCaret(false, Allowed, raw.data(), raw.size(), caret);
        raw.insert(caret++, 1, ch);
    }
    CHECK(raw == L"Rko-hi-");
    CHECK(CanBackspace(false, Allowed, raw.data(), raw.size(), caret));
    raw.erase(--caret, 1);
    CHECK(raw == L"Rko-hi");
    --caret;
    CHECK(Classify(raw, L'-').Function == FUNCTION_INPUT);
    caret = ClampCaret(false, Allowed, raw.data(), raw.size(), caret);
    raw.insert(caret++, 1, L'-');
    CHECK(raw == L"Rko-h-i");
    CHECK(ClampCaret(false, Allowed, raw.data(), raw.size(), 0) == 1);
    CHECK(!CanMoveCaret(false, Allowed, raw.data(), raw.size(), 0));
    CHECK(CanMoveCaret(false, Allowed, raw.data(), raw.size(), 1));
    CHECK(!CanMoveCaret(false, Allowed, raw.data(), raw.size(), -1));
    CHECK(!CanMoveCaret(false, Allowed, raw.data(), raw.size(), raw.size() + 1));
    CHECK(!CanBackspace(false, Allowed, raw.data(), raw.size(), 1));
    CHECK(CanBackspace(false, Allowed, L"R", 1, 1));
    CHECK(!CanBackspace(false, Allowed, L"R", 1, 0));
    CHECK(ClampCaret(false, Allowed, L"", 0, 10) == 0);
    CHECK(ClampCaret(false, Allowed, L"Rko", 3, 10) == 3);
    CHECK(CanMoveCaret(true, Allowed, L"ko", 2, 0));
    CHECK(CanBackspace(true, Allowed, L"ko", 2, 1));
    CHECK(CanMoveCaret(false, 0, L"Rko", 3, 0));
}

void PolicyChanges()
{
    Policy policy = Allowed;
    CHECK(Classify(L"Rko", L'-', false, policy).Function == FUNCTION_INPUT);
    policy = ToggleEnglish(policy);
    CHECK(Classify(L"Rko", L'-', false, policy).Function == FUNCTION_NONE);
    policy = ToggleEnglish(policy);
    CHECK(Classify(L"Rko", L'-', false, policy).Function == FUNCTION_INPUT);
    policy = LeaveEnglish(ToggleEnglish(policy));
    CHECK(policy == Allowed);
    policy = 0; // The IPC owner resets policy before reconnect negotiation.
    CHECK(Classify(L"Rko", L'-', false, policy).Function == FUNCTION_NONE);
    for (WCHAR ch : {L'0', L'1', L'2', L'3', L'4', L'/', L'\0'})
    {
        WCHAR frame[]{ch, L'\0'};
        CHECK(IsValidPolicyFrame(frame, 2) == (ch >= L'0' && ch <= L'3'));
        if (IsValidPolicyFrame(frame, 2))
            CHECK(ToPayload(FromPayload(ch)) == ch);
        frame[1] = L'x';
        CHECK(!IsValidPolicyFrame(frame, 2));
        CHECK(!IsValidPolicyFrame(frame, 1));
    }
    CHECK(!IsValidPolicyFrame(static_cast<const WCHAR *>(nullptr), 2));
    policy = FromPayload(L'1');
    CHECK(Classify(L"Rko", L'-', false, policy).Function == FUNCTION_INPUT);
}
} // namespace

int main()
{
    try
    {
        Classification();
        Editing();
        PolicyChanges();
        std::cout << "temporary Japanese key classification and editing passed\n";
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
