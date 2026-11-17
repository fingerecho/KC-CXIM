#pragma once
#include <iostream>
#include <streambuf>
#include <cstdlib>

class NullStreambuf : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
    int sync() override { return 0; }
};

inline void setupLogQuiet() {
    const char* q = std::getenv("QUIET_LOG");
    if (!q || q[0] == '\0') return;          // 未设置或空：正常输出
    if (std::string(q) == "0") return;       // 显式 0：正常输出

    static NullStreambuf nullBuf;
    std::cout.rdbuf(&nullBuf);
    std::cerr.rdbuf(&nullBuf);
}
