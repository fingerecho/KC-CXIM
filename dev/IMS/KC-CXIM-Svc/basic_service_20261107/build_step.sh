
if [[ "$(uname -s)" == Linux* ]]; then
    Boost_INCLUDE_DIRS="/home/ubuntu/src/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    BOOST_ROOT="/home/ubuntu/src/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    echo "Linux ..."
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    Boost_INCLUDE_DIRS="D:/FKC/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    BOOST_ROOT="D:/FKC/KC-CXIM/sdk/boost_1_92_0-bin-msvc-all-32-64/boost_1_92_0"
    echo "Windows..."
fi

cmake -S . -B build -DBoost_INCLUDE_DIRS=${Boost_INCLUDE_DIRS} -DBOOST_ROOT=${BOOST_ROOT}
cmake --build build -j

#bash step3_verify/verify_tcp.sh 8888 "$(pwd)/build"
