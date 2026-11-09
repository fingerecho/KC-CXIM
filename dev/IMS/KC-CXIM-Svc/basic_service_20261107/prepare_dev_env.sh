if [[ "$(uname -s)" == Linux* ]]; then
    if dpkg -s libssl-dev >/dev/null 2>&1; then
        echo "[skip] libssl-dev already installed."
        echo "[skip] libssl-dev已经装好了."
    else
        echo "[install] Installing OpenSSL dev package via apt..."
        echo "[install] 通过apt 安装OpenSSL 开发版"
        sudo apt-get update
        sudo apt-get install -y libssl-dev
        echo "[done] OpenSSL installed via apt."
        echo "[done] 通过apt安装OpenSSL成功."
    fi

    echo "Linux ..."
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    echo "Windows..."
    if command -v vcpkg >/dev/null 2>&1; then
        if vcpkg list | grep -q '^openssl:x64-windows'; then
            echo "[skip] OpenSSL (vcpkg) already installed."
            echo "[skip] OpenSSL (vcpkg) 已经装好了."
        else
            echo "[install] Installing OpenSSL via vcpkg..."
            echo "[install] 正在通过vcpkg安装OpenSSL"
            vcpkg x-update-baseline --add-initial-baseline
            if ! vcpkg install ; then
                echo "[error] vcpkg install failed. Check if you are using the VS-bundled vcpkg."
                echo "[Error] vcpkg 安装失败。"
            else
                echo "[done] OpenSSL installed via vcpkg."
                echo "[done] OpenSSL 已经通过vcpkg安装成功."
            fi
        fi
    else
        echo "[error] vcpkg not found in PATH. Install vcpkg first."
        echo "[error] vcpkg 没在系统上安装，请先安装vcpkg."
    fi
fi
