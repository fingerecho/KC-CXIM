#include "common/PasswordHasher.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
constexpr int kSaltSize = 16;
constexpr int kHashSize = 32;
constexpr const char *kPrefix = "pbkdf2-sha256";

std::vector<std::string> split(const std::string &input, char delimiter)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : input)
    {
        if (ch == delimiter)
        {
            parts.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}
}  // namespace

std::string PasswordHasher::hash(const std::string &password, int iterations)
{
    if (iterations <= 0)
    {
        iterations = 120000;
    }

    std::vector<unsigned char> salt(kSaltSize);
    if (RAND_bytes(salt.data(), kSaltSize) != 1)
    {
        throw std::runtime_error("RAND_bytes failed");
    }

    std::vector<unsigned char> digest(kHashSize);
    if (PKCS5_PBKDF2_HMAC(password.data(),
                          static_cast<int>(password.size()),
                          salt.data(),
                          static_cast<int>(salt.size()),
                          iterations,
                          EVP_sha256(),
                          static_cast<int>(digest.size()),
                          digest.data()) != 1)
    {
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    }

    const std::string saltBase64 = drogon::utils::base64Encode(
        std::string_view(reinterpret_cast<const char *>(salt.data()),
                         salt.size()));
    const std::string hashBase64 = drogon::utils::base64Encode(
        std::string_view(reinterpret_cast<const char *>(digest.data()),
                         digest.size()));

    return std::string(kPrefix) + "$" + std::to_string(iterations) + "$" +
           saltBase64 + "$" + hashBase64;
}

bool PasswordHasher::verify(const std::string &password,
                            const std::string &stored)
{
    try
    {
        const auto parts = split(stored, '$');
        if (parts.size() != 4 || parts[0] != kPrefix)
        {
            return false;
        }

        const int iterations = std::stoi(parts[1]);
        if (iterations <= 0)
        {
            return false;
        }

        const std::string salt = drogon::utils::base64Decode(parts[2]);
        const std::string expected = drogon::utils::base64Decode(parts[3]);
        if (salt.empty() || expected.empty())
        {
            return false;
        }

        std::vector<unsigned char> digest(expected.size());
        if (PKCS5_PBKDF2_HMAC(password.data(),
                              static_cast<int>(password.size()),
                              reinterpret_cast<const unsigned char *>(
                                  salt.data()),
                              static_cast<int>(salt.size()),
                              iterations,
                              EVP_sha256(),
                              static_cast<int>(digest.size()),
                              digest.data()) != 1)
        {
            return false;
        }

        return CRYPTO_memcmp(digest.data(), expected.data(), digest.size()) ==
               0;
    }
    catch (...)
    {
        return false;
    }
}
