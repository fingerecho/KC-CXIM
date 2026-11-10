#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace CryptoUtil
{
std::string randomHex(std::size_t bytes);
std::string sha256Hex(std::string_view data);
std::string hmacSha256(std::string_view key, std::string_view data);
bool constantTimeEquals(std::string_view a, std::string_view b);
}  // namespace CryptoUtil
