if [[ "$(uname -s)" == Linux* ]]; then
    if dpkg -s libssl-dev >/dev/null 2>&1; then
        #echo "[skip] libssl-dev already installed."
        #echo "[skip] libssl-dev已经装好了."
        echo "[skip]"
    else
        # echo "[install] Installing OpenSSL dev package via apt..."
        # echo "[install] 通过apt 安装OpenSSL 开发版"
        # sudo apt-get update
        # sudo apt-get install -y libssl-dev
        # echo "[done] OpenSSL installed via apt."
        # echo "[done] 通过apt安装OpenSSL成功."
        echo "[skip]"
    fi

    echo "Linux ..."
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    echo "Windows..."
    if command -v vcpkg >/dev/null 2>&1; then
        if vcpkg list | grep -q '^drogon\[mysql\]:x64-windows' && \
           vcpkg list | grep -q '^nlohmann-json:x64-windows' && \
           vcpkg list | grep -q '^openssl:x64-windows'; then
            echo "[skip] drogon[mysql],nlohmann-json,openssl (vcpkg) already installed."
            echo "[skip] drogon[mysql],nlohmann-json,openssl (vcpkg) (vcpkg) 已经装好了."
        else
            echo "[install] Installing drogon[mysql],nlohmann-json,openssl via vcpkg..."
            echo "[install] 正在通过vcpkg安装drogon[mysql],nlohmann-json,openssl"
            vcpkg x-update-baseline --add-initial-baseline
            if ! vcpkg install ; then
                echo "[error] library in vcpkg.json install failed. Check if you are using the VS-bundled vcpkg."
                echo "[Error] vcpkg配置中的库安装失败。"
            else
                echo "[done] library in vcpkg.json installed via vcpkg."
                echo "[done] vckpkg中的必要库已经通过vcpkg安装成功."
            fi
        fi
    else
        echo "[error] vcpkg not found in PATH. Install vcpkg first."
        echo "[error] vcpkg 没在系统上安装，请先安装vcpkg."
    fi
fi
