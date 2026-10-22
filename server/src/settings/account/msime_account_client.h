#pragma once

// C++ binding for the shared Rust account/cloud-sync implementation
// (metasequoiaime/msime, crates/host-api). The Rust side owns all session
// management, token refresh and sync orchestration; this wrapper only moves
// JSON across the C ABI and converts to the UTF-16 strings the settings page
// uses. The header shipped with the import library describes the exact
// boundary (see msime_client.h next to this file).
//
// Every call is blocking network I/O except `status`/`preferences_schema`:
// callers must keep them off the UI thread (the settings window's worker
// queue). Responses use the upstream envelope {"ok":bool,"value":...} or
// {"ok":false,"error":"<code>"}; this wrapper surfaces that as CallResult.

#include <string>

namespace msime_account
{
// Result of one msime_client_account / msime_client_cloud_sync call.
struct CallResult
{
    bool ok = false;
    // Envelope error code, e.g. "account_rate_limited" ("" when ok).
    std::string error;
    // retry_after_seconds on rate-limit failures, 0 otherwise.
    long retry_after_seconds = 0;
    // The envelope's "value" verbatim (UTF-8 JSON), "" when null/absent.
    std::string value_json;
};

// Account directory shared with the input method:
// <data dir>\account. Resolved once per call from CommonUtils paths.
std::wstring AccountDirectory();
// Legacy Tauri-era session directory migrated on first use, if it exists:
// %LOCALAPPDATA%\app.msime.windows.
std::wstring LegacyAccountDirectory();
// Directory for cloud-sync state files (cloud-sync/state.json): <data dir>.
std::wstring CloudSyncStateDirectory();
// Preferences directory the settings window writes config.toml into.
std::wstring PreferencesDirectory();

// One msime_client_account call. `action_json` is the UTF-8 JSON of the
// "action" object ({"operation":"status"} etc.); the envelope (directory,
// legacy_directory, client_agent) is filled in here.
CallResult CallAccount(const std::string &action_json);
// One msime_client_cloud_sync call with the same envelope treatment.
CallResult CallCloudSync(const std::string &action_json);

// User-Agent reported to the backend for this build, e.g.
// "msime-windows/0.7.1 (Windows)". Empty parts fall back to the library's
// default agent.
std::string ClientAgent();
} // namespace msime_account
