#pragma once

#include <string>

namespace ime_config_detail
{

// DPAPI-based credential encryption for sensitive config values (tokens, API keys, secrets).
// Encrypts on write, decrypts on read. Uses Windows DPAPI with current-user scope.
class CredentialEncryption
{
public:
    // Encrypt plaintext using DPAPI. Returns base64-encoded ciphertext prefixed with "DPAPI:".
    // Returns empty string on failure.
    static std::string Encrypt(const std::string &plaintext);

    // Decrypt base64-encoded ciphertext (with "DPAPI:" prefix) using DPAPI.
    // Returns plaintext on success, empty string on failure or if ciphertext is not encrypted.
    static std::string Decrypt(const std::string &ciphertext);

    // Check if a value is encrypted (has "DPAPI:" prefix).
    static bool IsEncrypted(const std::string &value);

private:
    static constexpr const char *kEncryptionPrefix = "DPAPI:";
};

} // namespace ime_config_detail
