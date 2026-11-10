OPENSSL_ROOT_DIR=""
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$(uname -s)" == Linux* ]]; then
    VCPKG_Root_Dir=""
    echo "Linux ..."
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    VCPKG_Root_Dir="C:/Program Files/Microsoft Visual Studio/18/Community/VC/vcpkg"
    #VCPKG_Root_Dir="C:/Users/28328/AppData/Local/vcpkg"
    echo "Windows..."
fi

cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_Root_Dir}/scripts/buildsystems/vcpkg.cmake"

if [[ "$(uname -s)" == Linux* ]]; then
    cmake --build build -j
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    cmake --build build --config Debug -j
fi
