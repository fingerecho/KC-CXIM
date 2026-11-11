
OPENSSL_ROOT_DIR=""
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# .deb 包信息（Linux 打包使用）
DEB_PACKAGE_NAME="cxim"
DEB_PACKAGE_VERSION="1.0.0"
DEB_PACKAGE_ARCH=""

# 生成 systemd 用户级服务文件到项目当前目录。
# 安装后 CXIM.service 位于 /opt/CXIM/CXIM.service，由 postinst 拷贝到
# 目标用户的 ~/.config/systemd/user/ 下。
generate_service_file() {
    cat > "${SCRIPT_DIR}/CXIM.service" <<'EOF'
[Unit]
Description=CXIM IMS Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/CXIM
ExecStart=/opt/CXIM/bin/tcp_echo_server
Environment=DB_CONFIG=/opt/CXIM/DBConfig.ini
Environment=JWT_CONFIG=/opt/CXIM/JWTConfig.json
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
EOF
}

# 将 build/bin 打包为 .deb：
#   安装时 build/bin 的内容 -> /opt/CXIM/bin/
#   CXIM.service          -> /opt/CXIM/CXIM.service
#   postinst             -> 拷贝服务文件到 ~/.config/systemd/user/ 并 daemon-reload
package_deb() {
    local bin_dir="${SCRIPT_DIR}/build/bin"
    local service_file="${SCRIPT_DIR}/CXIM.service"

    if [[ ! -d "${bin_dir}" ]]; then
        echo "[deb] ${bin_dir} 不存在，跳过打包" >&2
        return 1
    fi
    if ! command -v dpkg-deb >/dev/null 2>&1; then
        echo "[deb] 未找到 dpkg-deb，无法打包 .deb" >&2
        return 1
    fi

    generate_service_file

    DEB_PACKAGE_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
    local deb_name="${DEB_PACKAGE_NAME}_${DEB_PACKAGE_VERSION}_${DEB_PACKAGE_ARCH}.deb"
    local stage_dir="${SCRIPT_DIR}/build/deb/${DEB_PACKAGE_NAME}_${DEB_PACKAGE_VERSION}_${DEB_PACKAGE_ARCH}"

    rm -rf "${stage_dir}"
    mkdir -p "${stage_dir}/DEBIAN" "${stage_dir}/opt/CXIM/bin"

    # build/bin 下的全部内容安装到 /opt/CXIM/bin/
    cp -a "${bin_dir}/." "${stage_dir}/opt/CXIM/bin/"
    find "${stage_dir}/opt/CXIM/bin" -type f -name '*.pdb' -delete

    # 服务文件与运行所需配置放到 /opt/CXIM/
    cp -f "${service_file}" "${stage_dir}/opt/CXIM/CXIM.service"
    [[ -f "${SCRIPT_DIR}/DBConfig.ini" ]] && \
        cp -f "${SCRIPT_DIR}/DBConfig.ini" "${stage_dir}/opt/CXIM/DBConfig.ini"
    [[ -f "${SCRIPT_DIR}/JWTConfig.json" ]] && \
        cp -f "${SCRIPT_DIR}/JWTConfig.json" "${stage_dir}/opt/CXIM/JWTConfig.json"

    # 保证主程序可执行
    chmod 0755 "${stage_dir}/opt/CXIM/bin/tcp_echo_server" 2>/dev/null || true

    cat > "${stage_dir}/DEBIAN/control" <<EOF
Package: ${DEB_PACKAGE_NAME}
Version: ${DEB_PACKAGE_VERSION}
Section: utils
Priority: optional
Architecture: ${DEB_PACKAGE_ARCH}
Maintainer: KC-CXIM <dev@kc-cxim.local>
Depends: libc6, libstdc++6, libssl3 | libssl3t64, libmariadb3 | libmariadb3-compat
Description: KC-CXIM IMS server
 KC-CXIM instant messaging service, including TCP echo server and HTTP API.
EOF

    cat > "${stage_dir}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

SERVICE_NAME="CXIM.service"
SERVICE_SRC="/opt/CXIM/${SERVICE_NAME}"

# 安装 .deb 通常通过 sudo 执行，postinst 运行在 root 下。
# 优先把服务安装到触发 sudo 的真实用户，而不是 root。
TARGET_USER="${SUDO_USER:-}"
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER="$(logname 2>/dev/null || true)"
fi
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER="$(awk -F: '$3 >= 1000 && $3 != 65534 {print $1; exit}' /etc/passwd)"
fi

if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
    TARGET_GROUP="$(id -gn "$TARGET_USER")"
    TARGET_UID="$(id -u "$TARGET_USER")"
    [ -n "$TARGET_HOME" ] || TARGET_HOME="/home/${TARGET_USER}"
    #UNIT_DIR="${TARGET_HOME}/.config/systemd/user"
    UNIT_DIR="/etc/systemd/system"
    mkdir -p "$UNIT_DIR"
    cp -f "$SERVICE_SRC" "${UNIT_DIR}/${SERVICE_NAME}"
    chmod 0644 "${UNIT_DIR}/${SERVICE_NAME}"
    chown "${TARGET_USER}:${TARGET_GROUP}" "${TARGET_HOME}/.config" 2>/dev/null || true
    chown -R "${TARGET_USER}:${TARGET_GROUP}" "${TARGET_HOME}/.config/systemd" 2>/dev/null || true
else
    # 没有可用的普通用户时，回退到当前用户（通常是 root）的 ~/.config/systemd/user/
    TARGET_USER=""
    #UNIT_DIR="${HOME}/.config/systemd/user"
    UNIT_DIR="/etc/systemd/system"
    mkdir -p "$UNIT_DIR"
    cp -f "$SERVICE_SRC" "${UNIT_DIR}/${SERVICE_NAME}"
    chmod 0644 "${UNIT_DIR}/${SERVICE_NAME}"
fi

# 系统级 daemon-reload（按需求执行）
systemctl daemon-reload 2>/dev/null || true

# 服务是用户级单元，尽量以真实用户身份重载并启用。
if [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != "root" ]; then
    if command -v runuser >/dev/null 2>&1; then
        runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/${TARGET_UID}" \
            systemctl --user daemon-reload 2>/dev/null || true
        runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/${TARGET_UID}" \
            systemctl --user enable --now "$SERVICE_NAME" 2>/dev/null || true
    elif command -v su >/dev/null 2>&1; then
        su - "$TARGET_USER" -c "XDG_RUNTIME_DIR=/run/user/${TARGET_UID} systemctl --user daemon-reload" 2>/dev/null || true
        su - "$TARGET_USER" -c "XDG_RUNTIME_DIR=/run/user/${TARGET_UID} systemctl --user enable --now ${SERVICE_NAME}" 2>/dev/null || true
    fi
fi

echo "CXIM.service 已安装到 ${UNIT_DIR}/${SERVICE_NAME}"
exit 0
EOF
    chmod 0755 "${stage_dir}/DEBIAN/postinst"

    cat > "${stage_dir}/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e

SERVICE_NAME="CXIM.service"

TARGET_USER="${SUDO_USER:-}"
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER="$(logname 2>/dev/null || true)"
fi
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER="$(awk -F: '$3 >= 1000 && $3 != 65534 {print $1; exit}' /etc/passwd)"
fi

if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    TARGET_UID="$(id -u "$TARGET_USER")"
    if command -v runuser >/dev/null 2>&1; then
        runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/${TARGET_UID}" \
            systemctl --user disable --now "$SERVICE_NAME" 2>/dev/null || true
    fi
fi

exit 0
EOF
    chmod 0755 "${stage_dir}/DEBIAN/prerm"

    cat > "${stage_dir}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e

case "$1" in
    remove|purge)
        SERVICE_NAME="CXIM.service"

        TARGET_USER="${SUDO_USER:-}"
        if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
            TARGET_USER="$(logname 2>/dev/null || true)"
        fi
        if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
            TARGET_USER="$(awk -F: '$3 >= 1000 && $3 != 65534 {print $1; exit}' /etc/passwd)"
        fi

        if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
            TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
            [ -n "$TARGET_HOME" ] || TARGET_HOME="/home/${TARGET_USER}"
            rm -f "${TARGET_HOME}/.config/systemd/user/${SERVICE_NAME}"
        else
            rm -f "${HOME}/.config/systemd/user/${SERVICE_NAME}"
        fi

        systemctl daemon-reload 2>/dev/null || true
        ;;
esac

exit 0
EOF
    chmod 0755 "${stage_dir}/DEBIAN/postrm"

    dpkg-deb --build --root-owner-group "${stage_dir}" "${SCRIPT_DIR}/${deb_name}"
    mkdir -p "${SCRIPT_DIR}/../deployed/"
    mv "${SCRIPT_DIR}/${deb_name}" "${SCRIPT_DIR}/../deployed/"
    echo "[deb] 打包完成: ${deb_name}"

}

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

# Linux 下编译完成后自动生成服务文件并打包 .deb
if [[ "$(uname -s)" == Linux* ]]; then
    package_deb
fi

#bash step3_verify/verify_tcp.sh 8888 "$(pwd)/build"
