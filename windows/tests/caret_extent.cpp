#include "Key/CaretAnchorPolicy.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

// Only the host calls are doubles; both extent handlers come from production sources
using HRESULT = std::int32_t;
using TfEditCookie = int;
using BOOL = bool;
constexpr BOOL TRUE = true;
constexpr HRESULT S_OK = 0;
constexpr HRESULT TS_E_NOLAYOUT = -1;

bool SUCCEEDED(HRESULT hr)
{
    return hr >= 0;
}

bool FAILED(HRESULT hr)
{
    return hr < 0;
}

struct RECT
{
    int left, top, right, bottom;
};

struct POINT
{
    int x, y;
};

struct ITfRange
{
};

struct ITfContextView
{
    RECT extent;
    BOOL clipped;
    HRESULT result = S_OK;
    int releases = 0;

    HRESULT GetTextExt(TfEditCookie, ITfRange *, RECT *rect, BOOL *isClipped)
    {
        if (SUCCEEDED(result))
        {
            *rect = extent;
            *isClipped = clipped;
        }
        return result;
    }

    void Release()
    {
        ++releases;
    }
};

struct ITfContext
{
    ITfContextView *view;

    HRESULT GetActiveView(ITfContextView **result)
    {
        *result = view;
        return S_OK;
    }
};

namespace Global
{
int Point[2] = {};
constexpr int INVALID_Y = -100000;
int current_process_name = 0;
constexpr int ZEN_BROWSER = 1;
int firefox_like_cnt = 0;
std::atomic<bool> TsfDiagnosticLogEnabled{false};
} // namespace Global

void QueueTsfDiagnosticLog(const wchar_t *)
{
}

POINT GetPhysicalTextAnchor(ITfContextView *, const RECT &rect)
{
    return {rect.left, rect.bottom};
}

struct CTfTextLayoutSink
{
    ITfContext *_pContextDocument;
    ITfRange *_pRangeComposition = nullptr;
    TfEditCookie _tfEditCookie = 0;
    bool _hasValidAnchor = false;
    int notifications = 0;

    void _MarkAnchorValid()
    {
        _hasValidAnchor = true;
    }

    void _LayoutChangeNotification(const RECT *)
    {
        ++notifications;
    }

    HRESULT _GetTextExt(RECT *rect, POINT *anchor);
};

struct CGetTextExtentEditSession
{
    ITfContextView *_pContextView;
    CTfTextLayoutSink *_pTfTextLayoutSink;
    ITfRange *_pRangeComposition = nullptr;

    HRESULT DoEditSession(TfEditCookie ec);
};

#include "caret_extent_source.h"

int Check(bool condition, const char *test, const char *path)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s: %s\n", test, path);
    }
    return condition ? 0 : 1;
}

int main()
{
    struct ExtentCase
    {
        const char *name;
        RECT rect;
        bool clipped;
        bool usable;
    };
    const ExtentCase cases[] = {
        {"clipped caret", {10, 20, 10, 40}, true, true},
        {"clipped caret on left monitor", {-100, 20, -100, 40}, true, true},
        {"unclipped caret", {10, 20, 10, 40}, false, true},
        {"clipped text", {10, 20, 30, 40}, true, true},
        {"unclipped text", {10, 20, 30, 40}, false, true},
        {"zero height", {10, 20, 30, 20}, true, false},
        {"reversed height", {10, 40, 30, 20}, true, false},
        {"reversed width", {30, 20, 10, 40}, true, false},
        {"invisible text", {0, 0, 0, 0}, true, false},
    };
    int failures = 0;
    for (const auto &test : cases)
    {
        ITfContextView view{test.rect, test.clipped};
        ITfContext context{&view};
        CTfTextLayoutSink layout{&context};
        RECT rect{};
        POINT anchor{};
        const POINT expected = test.usable ? POINT{test.rect.left, test.rect.bottom} : POINT{0, Global::INVALID_Y};
        failures += Check(layout._GetTextExt(&rect, &anchor) == S_OK && anchor.x == expected.x &&
                              anchor.y == expected.y && layout._hasValidAnchor == test.usable && view.releases == 1,
                          test.name, "layout handler");

        CTfTextLayoutSink editLayout{&context};
        CGetTextExtentEditSession edit{&view, &editLayout};
        Global::Point[0] = Global::Point[1] = 0;
        failures +=
            Check(edit.DoEditSession(0) == S_OK && Global::Point[0] == expected.x && Global::Point[1] == expected.y &&
                      editLayout._hasValidAnchor == test.usable && editLayout.notifications == (test.usable ? 1 : 0),
                  test.name, "edit-session handler");
    }

    ITfContextView view{{}, true, TS_E_NOLAYOUT};
    ITfContext context{&view};
    CTfTextLayoutSink layout{&context};
    layout._hasValidAnchor = true;
    RECT rect{};
    POINT anchor{10, 40};
    failures += Check(layout._GetTextExt(&rect, &anchor) == TS_E_NOLAYOUT && anchor.x == 10 && anchor.y == 40 &&
                          layout._hasValidAnchor && view.releases == 1,
                      "transient TS_E_NOLAYOUT", "layout handler must keep the previous anchor");
    CGetTextExtentEditSession edit{&view, &layout};
    Global::Point[0] = 10;
    Global::Point[1] = 40;
    failures += Check(edit.DoEditSession(0) == S_OK && Global::Point[0] == 10 && Global::Point[1] == 40 &&
                          layout.notifications == 0,
                      "transient TS_E_NOLAYOUT", "edit-session handler must keep the previous anchor");
    return failures == 0 ? 0 : 1;
}
