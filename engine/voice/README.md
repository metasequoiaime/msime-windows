# Shared VoiceInput（水杉公共语音模块）

VoiceInput is part of the engine. A host links this C++17 library without linking the keyboard engine or copying recognition code.

| Target | Responsibility | Dependencies |
|---|---|---|
| `MetasequoiaIme::Voice` | Mono PCM/WAV, RMS voice segmentation, configurable HTTP transcription and optional text cleanup | libcurl, nlohmann-json |
| `MetasequoiaIme::VoiceCapture` | Per-instance microphone capture, converted to mono 16 kHz float PCM | vendored miniaudio |

Local Whisper recognition (`MetasequoiaIme::VoiceWhisper`) was removed when the engine was vendored into this repository; this product transcribes through a cloud provider. See [UPSTREAM.md](../UPSTREAM.md).

UI, microphone permissions, hotkeys, token storage and committing text belong to the host. The library returns UTF-8 text and does not simulate keys. Root engine builds have `METASEQUOIA_IME_BUILD_VOICE=OFF` by default. For a root vcpkg build with voice enabled, add `VCPKG_MANIFEST_FEATURES=voice`. Voice can also be built independently, so a host that only needs speech does not acquire Boost, SQLite or dictionary dependencies.

## Build

miniaudio is vendored in-tree at `voice/third_party/miniaudio`, so a plain checkout is enough. From
`engine/`, Windows uses the manifest in `voice/vcpkg.json`. Select the same MSVC runtime as the dependency triplet; the static triplet below requires `/MT` (and `/MTd` for Debug). When embedding Voice in a root/host build, set this at the host level so all linked C++ targets agree:

```powershell
cmake -S voice -B build-voice -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
cmake --build build-voice --config Release --parallel
ctest --test-dir build-voice -C Release --output-on-failure
```

Set `MSIME_VOICE_CAPTURE=OFF` if the host already supplies PCM. The library downloads no models. Imported Silero/ONNX code is built only by the standalone Windows host below, outside the supported public targets; the supported VAD here is RMS-based.

## Call from a host

```cmake
# After adding the engine with METASEQUOIA_IME_BUILD_VOICE=ON, or independently:
add_subdirectory(engine/voice voice)
target_link_libraries(MyHost PRIVATE MetasequoiaIme::Voice MetasequoiaIme::VoiceCapture)
```

```cpp
#include <msime/voice/cloud_stt_worker.h>
using namespace metasequoia::voice;
RequestOptions options{endpoint, model, token, 10000, cancellationFlag};
CloudSttWorker recognizer(options);
std::string text = recognizer.recognize(pcm); // worker queue, mono 16 kHz floats
// Host dispatches text to its UI/input-method thread and commits it.
```

`CloudSttWorker` supports multipart `file` + `model` with JSON `text`, `transcription` or `result.text` responses. This is a protocol adapter, not a claim that every provider supports that protocol. Endpoints/models are host configuration; the token-only constructor retains the old SiliconFlow defaults for the imported Windows host. The WebSocket/Doubao transport lives in `server/`; the codecs here support a host that keeps its own transport.

### Streaming scope

Streaming recognition is implemented by `server/`, not here. This API intentionally provides
bounded, one-shot HTTP recognition; it exposes no streaming transport and no partial-text commit
semantics. Do not reach for it expecting the server's incremental transcription behaviour — moving
streaming down into this library would be an API and frontend change, not a provider swap.

`provider_protocol.h` exposes the same multipart and JSON codecs to hosts with an existing HTTP transport. `make_transcription_request` takes an encoded WAV (up to 20 MiB), preserves binary bytes and accepts an optional language field; omit language for services that reject it. `make_chat_transcription_request` packages the same clip as base64 `input_audio` in a chat completion for gateways that transcribe through Chat Completions (MiMo-style) and take neither multipart uploads nor a language field. `make_polish_request` sends the supplied user message verbatim, so hosts retain their prompt/delimiter policy. Response parsers reject malformed, missing, empty or oversized text responses with `VoiceError`; `parse_transcription` also accepts a chat completion whose assistant message carries the transcript. Hosts retain endpoint validation, credentials, timeouts, cancellation, status checks and stricter upload/response limits. The PCM recognizer still enforces the 60-second contract below.

Each request has a timeout and optional shared atomic cancellation flag. Set that flag to abort an in-flight request; a cancelled flag stays cancelled until the host supplies a new one. Recognition errors throw `VoiceError`. `TextPolisher` returns the original text on failure, timeout, cancellation or an empty result. It never logs tokens, audio or response bodies. Redirects are rejected, HTTP status is checked and responses are capped at 1 MiB.

Input PCM must be finite mono 16 kHz float samples, at most 60 seconds per request. WAV encoding clips samples to [-1,1]. `WavWriter::create_wav` accepts an explicit sample limit for hosts with a different bounded upload duration, such as Windows clips with provider padding; omitting it retains the 60-second limit. RIFF size overflow is always rejected. Hosts serialize capture start/stop/destruction, keep callback work short, and never stop or destroy capture from its callback. `stop()` is idempotent and waits for callbacks. A callback exception is contained and reported by `callback_failed()`. VAD callers drain `take_audio()` when appropriate; oversized unconsumed blocks throw rather than growing without limit.

## Portability

The sources still carry the `if(APPLE)` branches and Objective-C++ entry points from when this was a
cross-platform library. They are kept rather than stripped: they are working code, they cost nothing
in a Windows build, and rewriting them out would be a large untested change. The macOS example host
that exercised them was removed — see [UPSTREAM.md](../UPSTREAM.md) — so treat those branches as
unverified here.

## Imported Windows host

The original standalone UI and hotkeys remain under `platforms/windows/`. Build it with `MSIME_VOICE_WINDOWS_APP=ON` and `VCPKG_MANIFEST_FEATURES=windows-app`. It links the shared targets and is off by default; the shipped product's voice input is `server/`, not this. See [the imported usage guide](platforms/windows/README.md) for the asset/config layout.

Common capture, WAV and provider codecs belong here; `server/` keeps its evolved provider transport, native interaction and streaming behaviour.

Original VoiceInput history and GPL-3.0 `LICENSE` are preserved; third-party libraries retain their own licenses. Import source: `413f734e1d4694748d3cf88b8df95f37528e8a97` in `metasequoiaime/MetasequoiaVoiceInput`.
