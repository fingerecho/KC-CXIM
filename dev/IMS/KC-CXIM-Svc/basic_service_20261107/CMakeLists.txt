# ============================================================================
# 顶层 CMake 配置
# ----------------------------------------------------------------------------
# CMake 是跨平台的构建系统生成器：它读取这些 CMakeLists.txt，
# 为你的编译器（这里是 MSVC / Visual Studio 2022）生成工程并编译。
# 常用命令：
#   cmake -S . -B build                 # 配置（生成 build/ 工程）
#   cmake --build build --config Debug  # 编译
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(KC_CXIM_Server LANGUAGES CXX)

# 统一使用 C++17（用到了 std::string::data() 返回 char*、结构化绑定等特性）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # 编译器不支持 C++17 就直接报错
set(CMAKE_CXX_EXTENSIONS OFF)         # 关闭编译器私有扩展，保证可移植

# 统一输出目录：可执行文件放到 build/bin/<配置>/ 下。
# 这样 MSVC 的 Debug/Release 会自动分成 build/bin/Debug、build/bin/Release。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# 第三方库：RapidJSON 是 header-only 的，只需把它的根目录加入 include 路径。
# 目录结构应为 third-party/RapidJSON/rapidjson/xxx.h
# ---------------------------------------------------------------------------
set(RapidJSON_Header ${PROJECT_SOURCE_DIR}/third-party/RapidJSON)

if(WIN32)
    # 告诉 Windows SDK 目标系统是 Windows 10，避免 API 版本相关的编译告警。
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

if(MSVC)
    # 让 MSVC 按 UTF-8 读取源文件，避免源码里的中文注释触发 C4819 告警。
    add_compile_options(/utf-8)
endif()

# Boost 通过环境变量 BOOST_ROOT 提供。这里直接把它加入头文件搜索路径。
# （find_package(Boost ...) 是另一种更规范的方式，这里为了简单用环境变量。）
#find_package(Boost 1.70 REQUIRED COMPONENTS system)

include_directories(${BOOST_ROOT})

# 加入 RapidJSON 头文件目录（全局生效，子目录也能用）。
include_directories(${RapidJSON_Header})

# 线程库（部分平台需要显式链接 -pthread 之类）。
find_package(Threads REQUIRED)

# 三个子模块：服务器、测试客户端、验证脚本。
add_subdirectory(step1_tcp_echo)
add_subdirectory(step2_test_client)
add_subdirectory(step3_verify)
