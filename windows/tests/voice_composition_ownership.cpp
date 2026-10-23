// Production write/cancel bodies run with an in-memory text store. No windows or installed TIP.
#include <windows.h>
#include <msctf.h>
#include <functional>
#include <iostream>
#include <string>

struct TestRange
{
    std::wstring text;
    std::function<void()> duringWrite;
    HRESULT Collapse(TfEditCookie, TfAnchor)
    {
        return S_OK;
    }
    HRESULT ShiftEndToRange(TfEditCookie, TestRange *, TfAnchor)
    {
        return S_OK;
    }
    void Release()
    {
    }
};

// The generated methods use only this small text-store surface.
#define ITfRange TestRange
struct TestSelection
{
    TestRange *range = nullptr;
    TF_SELECTIONSTYLE style{};
};
#define TF_SELECTION TestSelection
struct TestContext
{
    TestRange range;
    HRESULT GetSelection(TfEditCookie, ULONG, ULONG, TestSelection *selection, ULONG *fetched)
    {
        selection->range = &range;
        *fetched = 1;
        return S_OK;
    }
    HRESULT GetStart(TfEditCookie, TestRange **out)
    {
        *out = &range;
        return S_OK;
    }
    HRESULT SetSelection(TfEditCookie, ULONG, TestSelection *)
    {
        return S_OK;
    }
};
#define ITfContext TestContext
struct TestComposition
{
    TestRange *range;
    HRESULT GetRange(TestRange **out)
    {
        *out = range;
        return S_OK;
    }
};
struct CStringRange
{
    std::wstring text;
    void Set(const wchar_t *value, size_t length)
    {
        text.assign(value, length);
    }
    const wchar_t *Get() const
    {
        return text.c_str();
    }
    size_t GetLength() const
    {
        return text.size();
    }
};
struct TestEngine
{
    int purges = 0;
    void PurgeVirtualKey()
    {
        ++purges;
    }
};
namespace GlobalIme
{
std::wstring word_for_creating_word, pending_create_word_preedit;
}
namespace MsimeStats
{
int ClassifyText(const wchar_t *, size_t)
{
    return 0;
}
void QueueStatisticsEvent(int)
{
}
} // namespace MsimeStats
HRESULT SafeRangeSetText(TestRange *range, TfEditCookie, DWORD, const wchar_t *text, LONG length)
{
    range->text.assign(text, length);
    if (range->duringWrite)
        range->duringWrite();
    return S_OK;
}

class CMetasequoiaIME
{
  public:
    TestEngine engine;
    TestEngine *_pCompositionProcessorEngine = &engine;
    TestComposition composition{nullptr};
    TestComposition *_pComposition = nullptr;
    bool _voiceCompositionActive = false;
    int _gaDisplayAttributeInput = 0;
    int cancels = 0;
    int commits = 0;
    HRESULT _AddComposingAndChar(TfEditCookie, TestContext *, CStringRange *, bool = false);
    HRESULT _AddCharAndFinalize(TfEditCookie, TestContext *, CStringRange *, bool = false);
    HRESULT _InsertTextToComposition(TfEditCookie, TestContext *, CStringRange *);
    HRESULT _SetCompositionTextAndSelection(TfEditCookie, TestContext *, CStringRange *);
    HRESULT _HandleUpdateVoiceComposition(TfEditCookie, TestContext *, const std::wstring &);
    HRESULT _HandleCommitVoiceComposition(TfEditCookie, TestContext *, const std::wstring &);
    HRESULT _HandleCancelVoiceComposition(TfEditCookie, TestContext *);
    void _StartComposition(TestContext *context)
    {
        composition.range = &context->range;
        _pComposition = &composition;
    }
    void _DeleteCandidateList(BOOL, TestContext *)
    {
    }
    void _SetCompositionDisplayAttributesForRange(TfEditCookie, TestContext *, TestRange *, int)
    {
    }
    BOOL _FindComposingRange(TfEditCookie, TestContext *, TestRange *, TestRange **out)
    {
        *out = nullptr;
        return FALSE;
    }
    void _SetInputString(TfEditCookie ec, TestContext *context, TestRange *, CStringRange *text, BOOL)
    {
        SafeRangeSetText(&context->range, ec, 0, text->Get(), static_cast<LONG>(text->GetLength()));
    }
    HRESULT _HandleCancel(TfEditCookie, TestContext *context)
    {
        ++cancels;
        context->range.text.clear();
        _pComposition = nullptr;
        return S_OK;
    }
    HRESULT _HandleCompleteCommitFirst(TfEditCookie, TestContext *)
    {
        ++commits;
        _pComposition = nullptr;
        return S_OK;
    }
};

#include "voice_ownership_source.h"

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char *label) {
        std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
        failures += !ok;
    };
    // Cover each ordinary write entry, including candidate/punctuation and explicit commits.
    for (int entry = 0; entry != 4; ++entry)
    {
        CMetasequoiaIME service;
        TestContext context;
        check(service._HandleUpdateVoiceComposition(0, &context, L"voice") == S_OK && service._voiceCompositionActive &&
                  context.range.text == L"voice",
              "voice update owns the preview");
        bool voiceMarkerDuringWrite = true;
        context.range.duringWrite = [&] { voiceMarkerDuringWrite = service._voiceCompositionActive; };
        CStringRange keyboard{L"keyboard"};
        HRESULT hr = entry == 0   ? service._AddComposingAndChar(0, &context, &keyboard)
                     : entry == 1 ? service._AddCharAndFinalize(0, &context, &keyboard)
                     : entry == 2 ? service._InsertTextToComposition(0, &context, &keyboard)
                                  : service._SetCompositionTextAndSelection(0, &context, &keyboard);
        check(hr == S_OK && !voiceMarkerDuringWrite && !service._voiceCompositionActive,
              "ordinary write releases voice ownership before reentrant host callbacks");
        int purges = service.engine.purges;
        check(service._HandleCancelVoiceComposition(0, &context) == S_OK && service.cancels == 0 &&
                  service.engine.purges == purges && context.range.text == L"keyboard",
              "late voice cancel preserves keyboard text and engine state");
        GlobalIme::word_for_creating_word = L"keyboard word";
        GlobalIme::pending_create_word_preedit = L"keyboard pending";
        check(service._HandleUpdateVoiceComposition(0, &context, L"late preview") == S_OK &&
                  !service._voiceCompositionActive && context.range.text == L"keyboard" &&
                  service.engine.purges == purges,
              "late voice update cannot reclaim keyboard composition");
        for (const std::wstring text : {std::wstring(), std::wstring(L"late result")})
        {
            check(service._HandleCommitVoiceComposition(0, &context, text) == S_OK && service.commits == 0 &&
                      context.range.text == L"keyboard" && service.engine.purges == purges &&
                      GlobalIme::word_for_creating_word == L"keyboard word" &&
                      GlobalIme::pending_create_word_preedit == L"keyboard pending",
                  "late voice commit preserves keyboard text and pending word state");
        }
    }
    CMetasequoiaIME service;
    TestContext context;
    service._HandleUpdateVoiceComposition(0, &context, L"first");
    bool voiceMarkerDuringWrite = false;
    context.range.duringWrite = [&] { voiceMarkerDuringWrite = service._voiceCompositionActive; };
    service._HandleUpdateVoiceComposition(0, &context, L"second");
    check(voiceMarkerDuringWrite && context.range.text == L"second", "streaming update retains voice ownership");
    service._HandleCancelVoiceComposition(0, &context);
    check(service.cancels == 1 && context.range.text.empty() && !service._voiceCompositionActive,
          "voice cancellation still removes its own preview");
    service._HandleCancelVoiceComposition(0, &context);
    check(service.cancels == 1, "duplicate cancellation is harmless");
    service._HandleUpdateVoiceComposition(0, &context, L"new voice");
    service._HandleCommitVoiceComposition(0, &context, L"final voice");
    check(service.commits == 1 && !service._voiceCompositionActive && context.range.text == L"final voice",
          "a live voice composition still commits normally");
    return failures ? 1 : 0;
}
