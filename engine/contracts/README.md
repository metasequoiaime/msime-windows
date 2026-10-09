# Product contracts

These headers are independently consumable C++17 contracts. Including them does not link the input engine, start threads, access dictionaries, or require any third-party code. The TSF text service consumes only these headers; the server consumes the same source by linking the engine. Both live in this repository — `windows/` and `server/` — and so does the engine, so one commit covers all three and there is no pin to keep in step.

`windows_ipc.h` owns the wire layouts, pipe names and opcodes. `voice_composition_pipe.h` owns voice framing. Consumers may wrap these headers but must not duplicate their definitions.

`punctuation/policy.json` is the source of truth for the ASCII punctuation keys and their Chinese
translations. Its generated `policy.h` is consumed by the engine and included directly by the hosts
that route punctuation keys. Stateful quote alternation and nested book-title marks remain
part of the same contract, rather than being re-created in each host.

`product_lock.py` contains the shared product-lock primitives used by the three platform locks.
See [`product_lock.md`](product_lock.md) for the vendoring and byte-equality rule.

## Main handshake

Reverse endpoints retain their 16-byte hello and PipeReady acknowledgement. Once both endpoints are registered, the new client sends the existing 304-byte ClientHello with:

| Field | Meaning |
|---|---|
| keycode | MSIP magic |
| wch | Major wire protocol version |
| point[0] | Minor additive version |
| modifiers_down | Supported capability bits |
| point[1] | Required capability bits |
| request_id | Nonzero registration correlation ID |

The Server sends ProtocolReady or ProtocolMismatch on the registered reply endpoint before activating the main route. The reply echoes the correlation ID and contains major/minor, negotiated capabilities and magic in the first six UTF-16 units. These packets are consumed only during registration and are never text commits.

Existing DLLs with unversioned hello remain accepted by the new Server using the established v1 semantics; they receive no new opcode. New DLLs require an acknowledgement from the new Server within the existing bounded handshake budget. Against an old or incompatible Server they remain disconnected and use the existing raw-input fallback, rather than interpreting unnegotiated frames. A late acknowledgement from an old registration cannot authorize another one.

Append opcodes; never change released values or reuse them. An incompatible layout needs a new major protocol and an explicit migration, not another copy of a header. Minor additions must be optional capabilities.

`CharacterSetShortcut` is optional and is not included in the default capability set.
Windows implementations that support it pass `Capabilities | CharacterSetShortcut`
to `Hello` / `Negotiate`. Only after negotiating this bit may a Chinese-mode client
consume Ctrl+Shift+F (no Alt or Windows modifier) and send the existing `KeyEvent`
with keycode `F` and Shift/Control modifier bits. The server toggles the persisted
character set without clearing composition, and refreshes the current candidate
page and toolbar. It sends no key reply. Clients suppress auto-repeat and must not
replay an ambiguously delivered toggle; older peers simply omit the optional bit.

`tests/windows_ipc_contract.cpp` executes wire-layout/upgrade/framing cases on all Engine CI platforms and both Windows TSF architectures.

`TrilingualCycle` is optional. A supporting client advertises its capability and consumes
the existing configured language hotkeys only while the Server's `TrilingualCycleChanged`
setting (`[keybindings] trilingual_cycle`) is enabled. After matching a hotkey through the
existing shortcut handler, the client sends a normalized `VK_SHIFT` whose `modifiers_down`
carries `FanyImeTrilingualInput::EncodeCycleRequest(ime_open, japanese)`: an explicit request
bit plus the client's own OPENCLOSE and native-language state. A plain `VK_SHIFT` is still the
binary toggle and is never treated as a cycle, and raw Shift key-down events never request one.
The Server computes the destination from the request's state with
`FanyImeTrilingualInput::Destination`; if its setting was turned off while the request was in
flight, it still answers, with the binary-toggle destination.
The Server retains any already selected word prefix and commits the remaining raw spelling,
matching the existing language toggle. It replies with `TrilingualCycle`: destination
(`0` Chinese, `1` Japanese, `2` English),
a tab separator, and the exact text to commit. The client commits and ends composition before
applying the destination compartment in the same edit session. The Server must not also send
a worker candidate commit or compartment switch for that key, and leaves the requester out of
the `InputModeChanged` broadcast: the reply is that client's language update.
Clients that did not negotiate the capability retain their existing language toggle.

`backend/protocol.json` 定义 MSIME-Backend 的可选共通 HTTP 服务 API，详见[后端协议与兼容性](backend/README.md)。
