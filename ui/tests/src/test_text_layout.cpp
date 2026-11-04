#include "tests/includes/test_framework.h"

#include "src/tsf/TextLayout.h"

#include <wrl/client.h>

namespace
{
class LayoutFixture
{
  public:
    LayoutFixture()
    {
        REQUIRE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown **>(factory.GetAddressOf()))));
        REQUIRE(layout.Initialize(factory.Get()));
    }

    void LayOut(const wchar_t *text)
    {
        LOGFONT font = {};
        font.lfHeight = -18;
        wcscpy_s(font.lfFaceName, L"Microsoft YaHei");
        REQUIRE(layout.Layout(text, static_cast<UINT>(wcslen(text)), &font, 400.0f, 200.0f, 96.0f, 96.0f, 10.0f, 6.0f,
                              10.0f, 6.0f, FALSE));
    }

    RECT Caret(UINT pos)
    {
        RECT rc = {};
        REQUIRE(layout.RectFromCharPos(pos, &rc));
        return rc;
    }

    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    CTextLayout layout;
};
} // namespace

// The caret after a trailing newline belongs to the empty line the newline opened. It used to
// fall through to the "past the last line" fallback and land one line lower than the text.
TEST_CASE(text_layout_caret_after_trailing_newline_stays_on_the_new_line)
{
    LayoutFixture fixture;
    fixture.LayOut(L"AB\nC");
    const RECT textLine = fixture.Caret(3);

    fixture.LayOut(L"AB\n");
    const RECT emptyLine = fixture.Caret(3);

    REQUIRE(emptyLine.top == textLine.top);
    REQUIRE(emptyLine.bottom == textLine.bottom);
}

// Same for an empty line in the middle of the text.
TEST_CASE(text_layout_caret_on_an_empty_line_between_lines)
{
    LayoutFixture fixture;
    fixture.LayOut(L"AB\nC");
    const RECT secondLine = fixture.Caret(3);

    fixture.LayOut(L"AB\n\nC");
    const RECT emptyLine = fixture.Caret(3);

    REQUIRE(emptyLine.top == secondLine.top);
    REQUIRE(emptyLine.bottom == secondLine.bottom);
}
