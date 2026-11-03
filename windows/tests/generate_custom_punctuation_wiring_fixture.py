"""Compile the production custom-punctuation wiring against a standalone service fixture.

The TSF service cannot be instantiated without a host, so the paths that carry a key's custom
punctuation pick from classification to the committed text are extracted verbatim: how both
classifiers fill PunctuationKeyInput, the punctuation commit with its paired-punctuation stack,
and the two message-window cases that re-post or commit a punctuation key. Every TSF member
those paths call is a double in the test. Regenerate whenever one of the extracted pieces
changes, so a dropped pick cannot hide behind a copy here.

Two headers are produced: `<...>_types_source.h` with the types the test's service stub needs
before it can be declared, then `<...>_source.h` with the extracted definitions.
"""

from pathlib import Path
import sys

from generate_commit_reply_fixture import find_body


def definition(source, marker, after=0):
    signature, body = find_body(source, marker, after)
    return f"{signature}\n{body}\n"


def type_definition(source, marker):
    signature, body = find_body(source, marker)
    return f"{signature}\n{body};\n"


def generate(
    base_header,
    ime_header,
    key_handler_source,
    composition_source,
    key_sink_source,
    deferred_source,
    window_source,
    types_output,
    source_output,
):
    base_h = Path(base_header).read_text(encoding="utf-8")
    ime_h = Path(ime_header).read_text(encoding="utf-8")
    key_handler = Path(key_handler_source).read_text(encoding="utf-8")
    composition = Path(composition_source).read_text(encoding="utf-8")
    key_sink = Path(key_sink_source).read_text(encoding="utf-8")
    deferred = Path(deferred_source).read_text(encoding="utf-8")
    window = Path(window_source).read_text(encoding="utf-8")

    banner = "// Generated from the production custom punctuation wiring; do not edit.\n"

    types = banner
    types += type_definition(base_h, "enum KEYSTROKE_CATEGORY")
    types += type_definition(base_h, "enum KEYSTROKE_FUNCTION")
    types += type_definition(base_h, "enum CANDIDATE_MODE")
    types += type_definition(base_h, "struct _KEYSTROKE_STATE")
    types += type_definition(ime_h, "struct AsyncKeyRequest")
    types += type_definition(ime_h, "struct PairedPunctuationEntry")
    types += type_definition(deferred, "struct DeferredShadowState")
    Path(types_output).write_text(types, encoding="utf-8")

    source = banner
    source += definition(key_sink, "PunctuationKeyInput CMetasequoiaIME::_SyncPunctuationKeyInput(")
    source += definition(deferred, "PunctuationKeyInput CMetasequoiaIME::_DeferredPunctuationKeyInput(")
    source += definition(key_handler, "HRESULT CMetasequoiaIME::_HandleCompositionPunctuation(")
    source += definition(composition, "void CMetasequoiaIME::_PushPairedPunctuation(")
    source += definition(composition, "void CMetasequoiaIME::_ClearPairedPunctuationStack(")
    source += definition(composition, "bool CMetasequoiaIME::_TryStepOverPairedPunctuation(")

    # The two cases live in the window procedure's switch; they are compiled inside a switch of
    # their own so their `break`s keep their meaning.
    source += "void DispatchAsyncPunctuationMessage(CMetasequoiaIME *pIME, UINT message, WPARAM wParam)\n{\n"
    source += "    switch (message)\n    {\n"
    for case in ("case WM_AsyncPunctuationCommit:", "case WM_AsyncServerCandidateKey:"):
        label, body = find_body(window, case)
        source += f"    {label}\n    {body}\n"
    source += "    default:\n        break;\n    }\n}\n"
    Path(source_output).write_text(source, encoding="utf-8")


if __name__ == "__main__":
    generate(*sys.argv[1:])
