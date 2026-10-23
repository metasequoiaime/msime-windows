#pragma once

#include "config/ai_provider.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AiAssistant
{
// Persists the user's provider registry as ai_providers.json in the data directory - NOT in
// config.toml, whose template merge drops non-credential keys it does not know, which would
// delete user-added providers on every upgrade. HanaAgent keeps its provider plugins out of the
// app config for the same reason.
//
// Tokens are deliberately not part of the file. They stay in config.toml as token_<id>, where
// they are DPAPI-protected and survive template merges as credential keys.
namespace ProviderStore
{
using Entries = std::vector<AiProvider>;

// The provider ids with config.toml slots (endpoint_<id> / model_<id> / token_<id>). Their values
// are config.toml-authoritative; the registry only lists them so the menu can order or hide them.
const std::vector<std::string_view> &BuiltinProviderIds();
bool IsBuiltinProvider(std::string_view id);

// 内置服务商出厂默认的接口地址与模型。配置层用它补空槽位，设置页用它填占位符，
// 两边读同一份表，避免各写一份镜像后改一处忘另一处。未知 id 返回空串。
std::string BuiltinDefaultEndpoint(std::string_view id);
std::string BuiltinDefaultModel(std::string_view id);

// Entries as stored. Nullopt when the file does not exist yet, so the caller can seed a first
// generation of entries. A present-but-corrupt file also reads as nullopt: the next Save()
// rewrites it wholesale. An explicit path keeps tests off the real data directory.
std::optional<Entries> Load(const std::filesystem::path &path = {});

// Persists the whole list, without tokens. Writing is atomic (tmp file + rename) so a crash
// never leaves a half-written store behind. Returns false when the data directory is unavailable.
bool Save(const Entries &entries, const std::filesystem::path &path = {});

std::filesystem::path StorePath();

// The first generation of entries: one per built-in provider, in menu order.
Entries SeedBuiltins();

// Copies a registry-only provider's endpoint/model over the legacy single fields so the request
// path keeps working unchanged. Built-in and unknown ids leave them as the config file had them.
void ResolveActive(AiProvider &active_out, std::string &endpoint, std::string &model, const std::string &provider,
                   const Entries &entries);
} // namespace ProviderStore
} // namespace AiAssistant
