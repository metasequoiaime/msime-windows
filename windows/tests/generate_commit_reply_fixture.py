"""Compile the production commit-reply decision paths against a standalone service fixture.

The TSF service, its message window and the reply pipe cannot be instantiated without a host,
so the paths that decide what an *unready* committing reply costs are extracted verbatim
instead of copied into the test: the three-state reply read, the session's wait bound, the
hand-off that keeps a request alive, the poll timer pass, and both session completion hooks. Only the pipe probe, the message window and the
clock remain test doubles. Regenerate whenever one of the extracted functions changes, so a
removed step cannot hide behind a copy here.

Two headers are produced, because the test's service stub has to declare the extracted
`AsyncKeyRequest` before it can own a request table: `<...>_types_source.h` with the types the
stub needs, then `<...>_source.h` with the extracted definitions.
"""

from pathlib import Path
import re
import sys


TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
    re.DOTALL,
)

# SAL annotations travel in the production signatures; the fixture has no sal.h.
# Longest spellings first: alternation takes the first match, so `In` ahead of
# `Inout` would leave `out_` behind.
SAL = re.compile(
    r"\b_(?:Outptr_result_maybenull_|Inout_opt_|Inout|In_opt_|In_z_|In|Out_opt_|Out_z_|Out)_?[ \t]*"
)


def find_body(source, marker, after=0):
    """Return (signature, body) for the definition the marker names."""
    start = source.index(marker, after)
    opening = None
    depth = 0
    for token in TOKENS.finditer(source, start):
        if token.group() == "{":
            if opening is None:
                opening = token.start()
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if opening is not None and depth == 0:
                return SAL.sub("", source[start:opening]).strip(), source[
                    opening : token.end()
                ]
    raise ValueError(f"Unclosed production definition: {marker}")


def find_declaration(source, marker):
    """Return the verbatim `marker ... ;` declaration."""
    start = source.index(marker)
    return source[start : source.index(";", start) + 1]


def generate(
    ipc_header,
    ipc_source,
    ime_header,
    async_source,
    session_source,
    types_output,
    source_output,
):
    ipc_h = Path(ipc_header).read_text(encoding="utf-8")
    ipc_cpp = Path(ipc_source).read_text(encoding="utf-8")
    ime_h = Path(ime_header).read_text(encoding="utf-8")
    async_cpp = Path(async_source).read_text(encoding="utf-8")
    session_cpp = Path(session_source).read_text(encoding="utf-8")

    banner = "// Generated from the production commit-reply paths; do not edit.\n"

    status_signature, status_body = find_body(ipc_h, "enum class CommitReplyWaitStatus")
    request_signature, request_body = find_body(ime_h, "struct AsyncKeyRequest")

    types = banner
    types += f"{status_signature}\n{status_body};\n"
    types += (
        find_declaration(ipc_h, "constexpr DWORD FANY_IME_COMMIT_REPLY_TIMEOUT_MS")
        + "\n"
    )
    types += (
        find_declaration(ipc_h, "constexpr DWORD FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS")
        + "\n"
    )
    types += (
        find_declaration(ipc_h, "constexpr HRESULT FANY_S_COMMIT_REPLY_NOT_READY")
        + "\n"
    )
    types += (
        find_declaration(ime_h, "static constexpr size_t kMaxPendingAsyncKeys")
        + "\n"
    )
    types += (
        find_declaration(ipc_h, "constexpr HRESULT FANY_E_COMMIT_REPLY_AMBIGUOUS")
        + "\n"
    )
    types += f"{request_signature}\n{request_body};\n"
    Path(types_output).write_text(types, encoding="utf-8")

    # The punctuation commit session precedes the Completion struct it defines; anchoring the
    # search there keeps the extraction on that struct if another `struct Completion` appears.
    punctuation_session = async_cpp.index("class CPunctuationCommitEditSession")
    reply_session = session_cpp.index("STDAPI CKeyHandlerEditSession::DoEditSession")

    poll_signature, poll_body = find_body(
        async_cpp, "bool IsCommitReplyPollMessage(UINT message)"
    )
    requeue_signature, requeue_body = find_body(
        async_cpp, "bool CMetasequoiaIME::_RequeueAsyncKeyRequest("
    )
    handoff_signature, handoff_body = find_body(
        async_cpp, "void CMetasequoiaIME::_HandOffUnreadyCommitReply("
    )
    wait_signature, wait_body = find_body(
        async_cpp, "DWORD CMetasequoiaIME::_SessionCommitReplyWaitMs()"
    )
    dispatch_signature, dispatch_body = find_body(
        async_cpp, "void CMetasequoiaIME::_DispatchPendingCommitReplyPolls()"
    )
    read_signature, read_body = find_body(
        ipc_cpp, "CommitReplyWaitStatus TryReadCommitReplyFromServerPipe("
    )
    completion_signature, completion_body = find_body(
        async_cpp, "struct Completion", punctuation_session
    )
    replay_signature, replay_body = find_body(
        session_cpp, "struct DeferredReplayCompletion", reply_session
    )

    source = banner
    source += f"{poll_signature}\n{poll_body}\n"
    source += f"{requeue_signature}\n{requeue_body}\n"
    source += f"{handoff_signature}\n{handoff_body}\n"
    source += f"{wait_signature}\n{wait_body}\n"
    source += f"{dispatch_signature}\n{dispatch_body}\n"
    source += f"{read_signature}\n{read_body}\n"
    source += f"{completion_signature}\n{completion_body};\n"
    source += f"{replay_signature}\n{replay_body};\n"
    Path(source_output).write_text(source, encoding="utf-8")


if __name__ == "__main__":
    generate(*sys.argv[1:])
