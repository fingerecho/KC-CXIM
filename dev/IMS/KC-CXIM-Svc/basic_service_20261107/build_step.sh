
OPENSSL_ROOT_DIR=""
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$(uname -s)" == Linux* ]]; then
    Boost_INCLUDE_DIRS="/home/ubuntu/src/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    BOOST_ROOT="/home/ubuntu/src/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    MariaDB_ROOT="/home/ubuntu/src/KC-CXIM/sdk/mariadb-connector-cpp-1.1.9-ubu26.04"
    MariaDB_include="${MariaDB_ROOT}/include"
    MariaDB_lib="${MariaDB_ROOT}/lib/mariadb"
    echo "Linux ..."
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    Boost_INCLUDE_DIRS="D:/FKC/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    BOOST_ROOT="D:/FKC/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    MariaDB_ROOT="C:/Program Files/MariaDB/MariaDB C++ Connector 64-bit"
    MariaDB_include="${MariaDB_ROOT}/include"
    MariaDB_lib="${MariaDB_ROOT}"
    OPENSSL_ROOT_DIR="${SCRIPT_DIR}/vcpkg_installed/x64-windows"
    echo "Windows..."
fi

# 构造 cmake 参数
CMAKE_ARGS=(
    -S . -B build
    -DBoost_INCLUDE_DIRS="${Boost_INCLUDE_DIRS}"
    -DBOOST_ROOT="${BOOST_ROOT}"
    -DMariaDB_include="${MariaDB_include}"
    -DMariaDB_lib="${MariaDB_lib}"
)

# 只有 OPENSSL_ROOT_DIR 非空时才追加
if [[ -n "${OPENSSL_ROOT_DIR}" ]]; then
    CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}")
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build build -j

#bash step3_verify/verify_tcp.sh 8888 "$(pwd)/build"
