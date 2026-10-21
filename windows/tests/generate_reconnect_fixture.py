"""Compile the real reconnect handlers against a standalone session-state fixture.

The TSF service and its window cannot be instantiated without the Windows SDK
and COM host. Extract the handler bodies instead of copying their sequencing
into the test; this checks the production call sites, including omitted steps.
Win32 timer calls and the transport handshake remain test doubles.
"""

from pathlib import Path
import re
import sys


TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
    re.DOTALL,
)


def extract_body(source, marker):
    start = source.index(marker) + len(marker)
    opening = None
    depth = 0
    for token in TOKENS.finditer(source, start):
        if token.group() == '{':
            if opening is None:
                opening = token.start()
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if opening is not None and depth == 0:
                return source[opening:token.end()]
    raise ValueError(f'Unclosed production handler: {marker}')


def generate(window_path, key_path, output_path):
    window = Path(window_path).read_text(encoding='utf-8')
    keys = Path(key_path).read_text(encoding='utf-8')
    fallback = extract_body(keys, 'void CMetasequoiaIME::_TryLeaveServerUnavailableFallback()')
    timer = extract_body(window, 'if (wParam == TIMER_CONNECT_ALL_NAMEDPIPE)')
    message = extract_body(window, 'case WM_IpcReconnect:')
    thread_focus = extract_body(window, 'case WM_ThreadFocus:')
    generated = '// Generated from the production reconnect handlers; do not edit.\n'
    generated += 'void CMetasequoiaIME::_TryLeaveServerUnavailableFallback() ' + fallback + '\n'
    for name, body in (('RunReconnectTimer', timer), ('RunReconnectMessage', message),
                       ('RunThreadFocusMessage', thread_focus)):
        generated += f'void {name}(CMetasequoiaIME &service) {{\n'
        generated += '    CMetasequoiaIME *pIME = &service;\n    HWND hWnd = 1;\n    do '
        generated += body + ' while (false);\n}\n'
    Path(output_path).write_text(generated, encoding='utf-8')


if __name__ == '__main__':
    generate(*sys.argv[1:])
