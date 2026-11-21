

OPENSSL_ROOT_DIR=""
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# .deb 包信息（Linux 打包使用）
DEB_PACKAGE_NAME="cxim"
DEB_PACKAGE_VERSION="1.0.0"
DEB_PACKAGE_ARCH=""

# ============================================================================
# 压测/取证档（DEV）与生产档必须各自成套，不要"半开门"
# ----------------------------------------------------------------------------
# 之前这里只放开 AUTH_RATE_*，但准入还有另外三道闸：
#   HTTP_MAX_INFLIGHT（在途响应 + SQL 队列深度）、DB_QUEUE_LIMIT、TCP_MAX_SESSIONS。
# 只开一道的结果是：认证不再被 429，但请求仍然会在准入层被 503/关连接，
# 于是测到的既不是"服务端真实容量"，也不是"保护生效下的用户体验"，
# 而是一份两种闸门混合的数据 —— 这种数据没法和 readme §11.8 的任何预测对照。
#
# 因此 DEV 档把**所有**闸门一起抬到高于真实容量，让压测直接撞在服务端资源上；
# 同时打开日志，因为 DEV 档常常是用来定位"进程为什么没了"的，
# 而 QUIET_LOG=1 会把普通 cerr 流水一起吞掉（logFatal 那一路永不被静音）。
#
# ⚠️ DEV 档不是生产配置：它关掉了过载保护。正式部署请置为 False。
#    现在支持外部覆盖：OPEN_STRESS_TEST_DEV_MODE=False bash build.sh
# ============================================================================
OPEN_STRESS_TEST_DEV_MODE="${OPEN_STRESS_TEST_DEV_MODE:-True}"
CLEAR_LOGS="${CLEAR_LOGS:-True}"

if [[ "${OPEN_STRESS_TEST_DEV_MODE}" = "True" ]]; then
    # —— 认证限流：抬到远高于 2 核容量（~28 哈希/s @cost=10）——
    AUTH_RATE_BURST=10000
    AUTH_RATE_PER_SEC=10000
    AUTH_IP_PER_SEC=10000
    # —— 其余准入闸门一起抬，否则数据仍是"混合闸门"——
    HTTP_MAX_INFLIGHT=20000
    DB_QUEUE_LIMIT=200000
    TCP_MAX_SESSIONS=20000
    TCP_MAX_OUTSTANDING=10000
    # —— 取证档必须留日志（见上面说明）——
    QUIET_LOG=0
fi

# 清楚所有systemd日志
if [[ "${CLEAR_LOGS}" = "True" ]]; then
    sudo journalctl --rotate
    sudo journalctl --vacuum-time=1s -u cxim.service
fi

# ============================================================================
# 运行期参数（写进 CXIM.service，见 generate_service_file）
# ----------------------------------------------------------------------------
# 性能优化 P0-1 / P0-2 / P0-4 / P0-7：
#   目标机是 2 核 / 2GB / 3Mbps，且 mariadbd 与应用同机。线程数一律按"核数"收敛：
#   超订（例如 8 个 worker 抢 2 个核）不会提高吞吐，只会让所有接口——包括完全不
#   碰 bcrypt 的发消息/查好友——一起变慢，因为同机的 mariadbd 抢不到 CPU。
#
#   DB_WORKER_COUNT  SQL worker 线程数（默认给 4：2 核机器上留算力给 mariadbd）
#   DB_POOL_SIZE     连接池上限，与 SQL worker 一致即可（并发连接数不会超过 worker）
#   DB_HASH_WORKER_COUNT  bcrypt worker 线程数 = 核数
#   BCRYPT_COST      bcrypt 成本因子（12→10：单次哈希 ~138ms → ~35ms）
#   DB_QUEUE_LIMIT   任务队列深度上限（P0-4 背压）
#   HTTP_MAX_INFLIGHT / HTTP_BODY_MAX / AUTH_RATE_* / TCP_MAX_*  过载保护阈值
#   OFFLINE_CURSOR   1=用 conversation_members.last_delivered_seq 游标补推离线消息，
#                    不再写 offline_messages 行级队列（P1-4，多端投递语义见 readme.md）
#   RELATION_CACHE_TTL  发消息前置关系校验的缓存毫秒数（P1-1，0=关闭）
#
# 需要覆盖时：在环境里先 export 再执行本脚本（用 ${VAR:-default} 允许外部传入）。
# ============================================================================
DB_WORKER_COUNT="${DB_WORKER_COUNT:-4}"
DB_POOL_SIZE="${DB_POOL_SIZE:-$DB_WORKER_COUNT}"
DB_HASH_WORKER_COUNT="${DB_HASH_WORKER_COUNT:-2}"
BCRYPT_COST="${BCRYPT_COST:-10}"
DB_QUEUE_LIMIT="${DB_QUEUE_LIMIT:-2000}"
OFFLINE_CURSOR="${OFFLINE_CURSOR:-0}"
RELATION_CACHE_TTL="${RELATION_CACHE_TTL:-10000}"
HTTP_MAX_INFLIGHT="${HTTP_MAX_INFLIGHT:-512}"
HTTP_BODY_MAX="${HTTP_BODY_MAX:-65536}"
AUTH_RATE_BURST="${AUTH_RATE_BURST:-40}"
AUTH_RATE_PER_SEC="${AUTH_RATE_PER_SEC:-20}"
# 每 IP 阈值刻意**宽于**全局阈值：全局桶是"机器容量闸门"，每 IP 桶是"反滥用"。
# 反过来配（例如每 IP 5/s）会让每 IP 桶变成实际限制——办公室/CGNAT 共享出口 IP 的
# 真实用户会互相拖死，单机压测也会在服务器空闲时被大面积 429。
# 若服务将来挂在 nginx/CLB 后面，需要改成"只信任可信代理的 X-Forwarded-For"，
# 否则客户端自己塞一个 XFF 就能绕过每 IP 限制（这是安全配置，不能顺手加）。
AUTH_IP_PER_SEC="${AUTH_IP_PER_SEC:-30}"
TCP_MAX_SESSIONS="${TCP_MAX_SESSIONS:-2000}"
TCP_MAX_OUTSTANDING="${TCP_MAX_OUTSTANDING:-64}"
IDLE_TIMEOUT_SEC="${IDLE_TIMEOUT_SEC:-60}"
TCP_PORT=10001
HTTP_PORT=10002
# 用 :- 而不是直接赋值：DEV 档在上面已经把 QUIET_LOG 置 0（取证必须留日志），
# 这里无条件写 1 会把那一档吃掉，表现为"日志开关改了没反应"。
QUIET_LOG="${QUIET_LOG:-1}"

# /*************图片文件功能（七牛云直传）：部署档位默认值***************/
# QINIU_ENABLED：0=MockObjectStore（联调/CI），1=真实七牛（生产必须为 1，
#   且 /opt/CXIM/QiniuConfig.json 必须就位——缺文件服务直接启动失败，不静默降级）。
# 与 DBConfig.ini 同理：QiniuConfig.json 含 AK/SK，**不打进 deb 包**，由部署方放置。
QINIU_ENABLED="${QINIU_ENABLED:-1}"
QINIU_CONFIG="${QINIU_CONFIG:-/opt/CXIM/QiniuConfig.json}"
FILE_MAX_SIZE_MB="${FILE_MAX_SIZE_MB:-20}"
FILE_URL_TTL_SEC="${FILE_URL_TTL_SEC:-600}"
FILE_USER_DAILY_QUOTA_MB="${FILE_USER_DAILY_QUOTA_MB:-500}"
# FILE_BASE_URL：媒体消息元数据 url 前缀，必须填**客户端可达**的公网 HTTP 地址，
# 例如 http://110.40.206.253:${HTTP_PORT}——留空时服务端跟随 HTTP_PORT 默认回环
# （开发可用、生产图片全裂），所以这里要求部署方显式给出。
FILE_BASE_URL="${FILE_BASE_URL:-http://127.0.0.1:${HTTP_PORT}}"
# /*************图片文件功能（七牛云直传）：部署档位默认值***************/

# 构建类型（P0-5）：默认 Release，可 export CMAKE_BUILD_TYPE=RelWithDebInfo 覆盖。
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
# 生成 systemd 用户级服务文件到项目当前目录。
# 安装后 CXIM.service 位于 /opt/CXIM/CXIM.service，由 postinst 拷贝到
# 目标用户的 ~/.config/systemd/user/ 下。
generate_service_file() {
    cat > "${SCRIPT_DIR}/CXIM.service" <<EOF
[Unit]
Description=CXIM IMS Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/CXIM
ExecStart=/opt/CXIM/bin/cxim-server
Environment=DB_CONFIG=/opt/CXIM/DBConfig.ini
Environment=JWT_CONFIG=/opt/CXIM/JWTConfig.json
Environment=TCP_PORT=${TCP_PORT}
Environment=HTTP_PORT=${HTTP_PORT}
Environment=DB_POOL_SIZE=${DB_POOL_SIZE}
Environment=DB_WORKER_COUNT=${DB_WORKER_COUNT}
Environment=DB_HASH_WORKER_COUNT=${DB_HASH_WORKER_COUNT}
Environment=BCRYPT_COST=${BCRYPT_COST}
Environment=DB_QUEUE_LIMIT=${DB_QUEUE_LIMIT}
Environment=OFFLINE_CURSOR=${OFFLINE_CURSOR}
Environment=RELATION_CACHE_TTL=${RELATION_CACHE_TTL}
Environment=HTTP_MAX_INFLIGHT=${HTTP_MAX_INFLIGHT}
Environment=HTTP_BODY_MAX=${HTTP_BODY_MAX}
Environment=AUTH_RATE_BURST=${AUTH_RATE_BURST}
Environment=AUTH_RATE_PER_SEC=${AUTH_RATE_PER_SEC}
Environment=AUTH_IP_PER_SEC=${AUTH_IP_PER_SEC}
Environment=TCP_MAX_SESSIONS=${TCP_MAX_SESSIONS}
Environment=TCP_MAX_OUTSTANDING=${TCP_MAX_OUTSTANDING}
Environment=IDLE_TIMEOUT_SEC=${IDLE_TIMEOUT_SEC}
Environment=QUIET_LOG=${QUIET_LOG}
# /*************图片文件功能（七牛云直传）：文件模块运行参数***************/
Environment=QINIU_ENABLED=${QINIU_ENABLED}
Environment=QINIU_CONFIG=${QINIU_CONFIG}
Environment=FILE_MAX_SIZE_MB=${FILE_MAX_SIZE_MB}
Environment=FILE_URL_TTL_SEC=${FILE_URL_TTL_SEC}
Environment=FILE_USER_DAILY_QUOTA_MB=${FILE_USER_DAILY_QUOTA_MB}
Environment=FILE_BASE_URL=${FILE_BASE_URL}
# /*************图片文件功能（七牛云直传）：文件模块运行参数***************/
# P0-7：500 并发 = 500 条 TCP 长连接 + 500 条 HTTP 短连接 + 若干 DB/binlog/日志 fd，
# 不设限时默认值可能只有 1024，压测里 connect 超时(10060) 就有它一份。
# 装好后请用 \`systemctl show CXIM -p LimitNOFILE\` 和 /proc/<pid>/limits 实证生效。
LimitNOFILE=${LIMIT_NOFILE:-65535}
# /*************core-dump 修复 ⑦：出事必须留下 core***************/
# 这里原来是硬编码 LimitCORE=0：**内核不落盘 core**。于是 journal 里只有
#     code=dumped, status=11/SEGV / Failed with result 'core-dump'.
# 而 \`coredumpctl list\` 是空的 —— "SEGV 在哪一行"根本无从回答，只能靠读代码猜。
# 默认改成 infinity（有 core 才能 gdb bt 看真实栈）；磁盘紧张的机器可以
# 用 LIMIT_CORE=0 bash build.sh 关掉，但不要同时又抱怨"看不到崩溃现场"。
# 配套：step1_tcp_echo/CMakeLists.txt 给服务端加了 -g -fno-omit-frame-pointer，
#       main.cpp 装了信号处理器（没有 core 时 journal 里也有一份原生栈帧）。
LimitCORE=${LIMIT_CORE:-infinity}
# /*************core-dump 修复 ⑬：unit 里的注释必须独占一行***************/
# systemd 的 unit 解析器**不支持行尾注释**：# 只有出现在行首（前面最多有空白）才算注释。
# 原来这一行写成 Restart=always          # 原来的 on-failure 不覆盖…\`，
# 于是整串 always          # 原来的… 都被当成 Restart 的取值 → 解析失败 →
# **静默回落到默认值 no**。本轮实测证据：
#     sudo systemctl show CXIM -p Restart  →  Restart=no
#     15:15:09 SEGV 之后 journal 里没有任何 "Scheduled restart job" —— 服务就这么躺着。
# 也就是说：上一轮"改成 Restart=always"的动作**实际上从未生效过**。
# 装好后必须用 systemctl show CXIM -p Restart 验，而不是看 unit 文件写了什么。
Restart=always
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
    [[ -f "${SCRIPT_DIR}/QiniuConfig.json" ]] && \
        cp -f "${SCRIPT_DIR}/QiniuConfig.json" "${stage_dir}/opt/CXIM/QiniuConfig.json"

    # 保证主程序可执行
    chmod 0755 "${stage_dir}/opt/CXIM/bin/cxim-server" 2>/dev/null || true

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
# P0-5：显式传 CMAKE_BUILD_TYPE。
#   * Linux（Unix Makefiles，单配置）：不传就得到空的 CXX_FLAGS = -O0；
#   * Windows（Visual Studio，多配置）：这里传的值不生效，靠下面 --config 决定。
CMAKE_ARGS=(
    -S . -B build
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DBoost_INCLUDE_DIRS="${Boost_INCLUDE_DIRS}"
    -DBOOST_ROOT="${BOOST_ROOT}"
    -DMariaDB_include="${MariaDB_include}"
    -DMariaDB_lib="${MariaDB_lib}"
)

# 只有 OPENSSL_ROOT_DIR 非空时才追加
if [[ -n "${OPENSSL_ROOT_DIR}" ]]; then
    CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}")
fi

# 只有BUILD_TYPE为Release时，加上 -DCXIM_NO_DEBUG_INFO=ON
if [[ "${OPENSSL_ROOT_DIR}" == "Release" ]]; then
    CMAKE_ARGS+=(-DCXIM_NO_DEBUG_INFO=ON)
fi

cmake "${CMAKE_ARGS[@]}"

if [[ "$(uname -s)" == Linux* ]]; then
    # 单配置生成器：上面已传 -DCMAKE_BUILD_TYPE=${BUILD_TYPE}
    cmake --build build -j2
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    # 多配置生成器：不写 --config 默认编 Debug(/Od)，压测数据会被客户端自身拖慢。
    # 产物在 build/bin/${BUILD_TYPE}/，用它跑 stress 才与服务端同级。
    cmake --build build --config "${BUILD_TYPE}" -j
fi
# Linux 下编译完成后自动生成服务文件并打包 .deb
if [[ "$(uname -s)" == Linux* ]]; then
    package_deb
fi
