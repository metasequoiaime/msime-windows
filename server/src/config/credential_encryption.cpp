#include "config/credential_encryption.h"
#include <Windows.h>
#include <dpapi.h>
#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

namespace ime_config_detail
{

namespace
{
// Base64 encoding alphabet.
constexpr const char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode binary data to base64 string.
std::string Base64Encode(const uint8_t *data, size_t size)
{
    std::string result;
    result.reserve((size + 2) / 3 * 4);

    for (size_t i = 0; i < size; i += 3)
    {
        uint32_t chunk = 0;
        int bytes = std::min(size_t(3), size - i);

        for (int j = 0; j < bytes; ++j)
        {
            chunk = (chunk << 8) | data[i + j];
        }
        chunk <<= (3 - bytes) * 8;

        result += kBase64Alphabet[(chunk >> 18) & 0x3F];
        result += kBase64Alphabet[(chunk >> 12) & 0x3F];
        result += (bytes > 1) ? kBase64Alphabet[(chunk >> 6) & 0x3F] : '=';
        result += (bytes > 2) ? kBase64Alphabet[chunk & 0x3F] : '=';
    }

    return result;
}

// Decode base64 string to binary data. Returns empty vector on failure.
std::vector<uint8_t> Base64Decode(const std::string &encoded)
{
    std::vector<uint8_t> result;
    if (encoded.size() % 4 != 0)
    {
        return result;
    }

    result.reserve(encoded.size() / 4 * 3);

    for (size_t i = 0; i < encoded.size(); i += 4)
    {
        auto decode_char = [](char c) -> int {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };

        int v[4];
        for (int j = 0; j < 4; ++j)
        {
            v[j] = decode_char(encoded[i + j]);
            if (v[j] < 0 && encoded[i + j] != '=')
            {
                return std::vector<uint8_t>();
            }
            if (v[j] < 0)
            {
                v[j] = 0;
            }
        }

        uint32_t chunk = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];

        result.push_back((chunk >> 16) & 0xFF);
        if (encoded[i + 2] != '=')
        {
            result.push_back((chunk >> 8) & 0xFF);
        }
        if (encoded[i + 3] != '=')
        {
            result.push_back(chunk & 0xFF);
        }
    }

    return result;
}
} // namespace

std::string CredentialEncryption::Encrypt(const std::string &plaintext)
{
    if (plaintext.empty())
    {
        return plaintext;
    }

    // Already encrypted, return as-is.
    if (IsEncrypted(plaintext))
    {
        return plaintext;
    }

    // Convert UTF-8 string to UTF-16 for Windows API.
    int wsize = MultiByteToWideChar(CP_UTF8, 0, plaintext.c_str(), -1, nullptr, 0);
    if (wsize <= 0)
    {
        return "";
    }

    std::wstring wide_plaintext(wsize - 1, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, plaintext.c_str(), -1, &wide_plaintext[0], wsize) <= 0)
    {
        return "";
    }

    // Prepare DATA_BLOB for DPAPI.
    DATA_BLOB input_blob;
    input_blob.pbData = reinterpret_cast<BYTE *>(wide_plaintext.data());
    input_blob.cbData = wide_plaintext.size() * sizeof(wchar_t);

    DATA_BLOB output_blob = {};

    // Encrypt with DPAPI (DPAPI_PROTECT_DATA_NO_UI ensures no UI prompts).
    if (!CryptProtectData(&input_blob, L"MetasequoiaIME Credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output_blob))
    {
        return "";
    }

    // Encode to base64 and prepend prefix.
    std::string base64 = Base64Encode(output_blob.pbData, output_blob.cbData);
    LocalFree(output_blob.pbData);

    return std::string(kEncryptionPrefix) + base64;
}

std::string CredentialEncryption::Decrypt(const std::string &ciphertext)
{
    if (ciphertext.empty())
    {
        return ciphertext;
    }

    // Not encrypted, return as-is.
    if (!IsEncrypted(ciphertext))
    {
        return ciphertext;
    }

    // Remove prefix and decode from base64.
    std::string base64 = ciphertext.substr(std::strlen(kEncryptionPrefix));
    std::vector<uint8_t> encrypted_data = Base64Decode(base64);

    if (encrypted_data.empty())
    {
        return "";
    }

    // Prepare DATA_BLOB for DPAPI.
    DATA_BLOB input_blob;
    input_blob.pbData = encrypted_data.data();
    input_blob.cbData = encrypted_data.size();

    DATA_BLOB output_blob = {};

    // Decrypt with DPAPI.
    if (!CryptUnprotectData(&input_blob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output_blob))
    {
        return "";
    }

    // Convert UTF-16 back to UTF-8.
    int utf8_size = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t *>(output_blob.pbData),
                                        output_blob.cbData / sizeof(wchar_t), nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0)
    {
        LocalFree(output_blob.pbData);
        return "";
    }

    std::string plaintext(utf8_size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t *>(output_blob.pbData),
                        output_blob.cbData / sizeof(wchar_t), &plaintext[0], utf8_size, nullptr, nullptr);
    LocalFree(output_blob.pbData);

    // Remove trailing null terminator if present.
    if (!plaintext.empty() && plaintext.back() == '\0')
    {
        plaintext.pop_back();
    }

    return plaintext;
}

bool CredentialEncryption::IsEncrypted(const std::string &value)
{
    return value.compare(0, std::strlen(kEncryptionPrefix), kEncryptionPrefix) == 0;
}

} // namespace ime_config_detail
