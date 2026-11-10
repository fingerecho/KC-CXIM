cmake_minimum_required(VERSION 3.16)
project(kc-cxim-AdminBackend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Drogon 1.9 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

set(CX_SOURCES
    src/main.cc

    src/common/AppContext.cc
    src/common/CryptoUtil.cc
    src/common/JwtCodec.cc
    src/common/PasswordHasher.cc

    src/config/AppConfig.cc

    src/controllers/AuthController.cc
    src/controllers/HealthController.cc
    src/controllers/PlaceholderController.cc
    src/controllers/StatsController.cc
    src/controllers/UserController.cc

    src/services/AuditService.cc
    src/services/AuthService.cc
    src/services/StatsService.cc
    src/services/UserService.cc

    src/repositories/AdminRepository.cc
    src/repositories/AuditRepository.cc
    src/repositories/SessionRepository.cc
    src/repositories/StatsRepository.cc
    src/repositories/UserRepository.cc

    src/filters/AccessLogAdvice.cc
    src/filters/AuthFilter.cc
    src/filters/CorsAdvice.cc
    src/filters/ErrorAdvice.cc
    src/filters/RbacFilter.cc
    src/filters/RequestIdAdvice.cc
)

add_executable(kc-cxim-admin-backend ${CX_SOURCES})

target_include_directories(kc-cxim-admin-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kc-cxim-admin-backend PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
)

if(MSVC)
    target_compile_options(kc-cxim-admin-backend PRIVATE /utf-8)
endif()

set_target_properties(kc-cxim-admin-backend PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
