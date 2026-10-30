"""Compile both production caret-extent handlers against a fake TSF context view"""

from pathlib import Path
import sys

from generate_reconnect_fixture import extract_body


def generate(layout_path, edit_path, output_path):
    layout = Path(layout_path).read_text(encoding='utf-8')
    edit = Path(edit_path).read_text(encoding='utf-8')
    generated = '// Generated from the production caret-extent handlers; do not edit\n'
    generated += 'HRESULT CTfTextLayoutSink::_GetTextExt(RECT *lpRect, POINT *lpAnchor) '
    generated += extract_body(layout, 'HRESULT CTfTextLayoutSink::_GetTextExt(') + '\n'
    generated += 'HRESULT CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) '
    generated += extract_body(edit, 'STDAPI CGetTextExtentEditSession::DoEditSession(') + '\n'
    Path(output_path).write_text(generated, encoding='utf-8')


if __name__ == '__main__':
    generate(*sys.argv[1:])
