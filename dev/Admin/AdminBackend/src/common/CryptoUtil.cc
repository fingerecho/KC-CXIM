#include "common/CryptoUtil.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::string toHex(const unsigned char *data, std::size_t size)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i)
    {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}
}  // namespace

namespace CryptoUtil
{
std::string randomHex(std::size_t bytes)
{
    std::vector<unsigned char> buffer(bytes);
    if (bytes > 0 && RAND_bytes(buffer.data(), static_cast<int>(bytes)) != 1)
    {
        throw std::runtime_error("RAND_bytes failed");
    }
    return toHex(buffer.data(), buffer.size());
}

std::string sha256Hex(std::string_view data)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data.data()),
           data.size(),
           digest);
    return toHex(digest, sizeof(digest));
}

std::string hmacSha256(std::string_view key, std::string_view data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (HMAC(EVP_sha256(),
             key.data(),
             static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char *>(data.data()),
             data.size(),
             digest,
             &length) == nullptr)
    {
        throw std::runtime_error("HMAC failed");
    }
    return std::string(reinterpret_cast<const char *>(digest), length);
}

bool constantTimeEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
}  // namespace CryptoUtil
