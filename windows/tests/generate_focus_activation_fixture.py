"""Compile the production IPC focus gate and activation handshake in isolation."""
from pathlib import Path
import sys

from generate_reconnect_fixture import extract_body


def generate(ipc_path, output_path):
    source = Path(ipc_path).read_text(encoding='utf-8')
    generated = '// Generated from the production IPC functions; do not edit.\n'
    for signature in ('bool IsNamedpipeThreadFocused()', 'bool EnsureNamedpipeFocusSessionActivated()'):
        generated += signature + ' ' + extract_body(source, signature) + '\n'
    Path(output_path).write_text(generated, encoding='utf-8')


if __name__ == '__main__':
    generate(*sys.argv[1:])
