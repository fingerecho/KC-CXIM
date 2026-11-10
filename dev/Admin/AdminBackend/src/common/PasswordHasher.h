#pragma once

#include <string>

class PasswordHasher
{
  public:
    static std::string hash(const std::string &password,
                            int iterations = 120000);
    static bool verify(const std::string &password,
                       const std::string &stored);
};
