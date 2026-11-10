#!/usr/bin/env bash
# ============================================================================
# prepare_env_V0.sh —— 在 Ubuntu / Debian 上自动安装并配置 MariaDB
# ----------------------------------------------------------------------------
# 功能：
#   1) 安装并启动 MariaDB（幂等：已安装则跳过）；
#   2) 读取同目录（或指定路径）的 DBConfig.ini，取出 [db] 段的
#      host / port / dbname / user / password；
#   3) 写入 MariaDB 调优配置（内存、连接数、binlog 等）；
#   4) 创建数据库、业务账号，并授予该库的全部权限；
#   5) 测试阶段允许外网登录：bind-address=0.0.0.0，账号使用 'user'@'%'；
#   6) 自动完成 mariadb-secure-installation 的基础项（删匿名用户/test 库）；
#   7) 若存在 ./db/schema.sql，则自动导入业务表结构（可选）；
#   8) 以业务账号走 TCP 回连验证，确认账号与库都能正常使用。
#
# 用法：
#   sudo bash prepare_env_V0.sh [DBConfig.ini 路径]
#   sudo DB_CONFIG=/path/to/DBConfig.ini bash prepare_env_V0.sh
#   sudo MYSQL_ROOT_PASSWORD='root密码' bash prepare_env_V0.sh   # root 非 socket 认证时用
#
# 说明：
#   - 仅用于测试/预发布阶段（外网可登录）。正式环境请把 bind-address 改回
#     127.0.0.1，或只允许固定 IP，并删除 'user'@'%' 账号。
#   - 本脚本不会在客户端/本机安装任何东西；只在目标服务器上执行。
# ============================================================================

set -Eeuo pipefail

# ---------------------------------------------------------------------------
# 0. 颜色与日志小工具
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[36m'; C_RESET=$'\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_BLUE=''; C_RESET=''
fi

info() { printf '%s[prepare]%s %s\n' "$C_BLUE"  "$C_RESET" "$*"; }
ok()   { printf '%s[  ok  ]%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '%s[warn ]%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()  { printf '%s[ERROR ]%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

TMP_SQL=''
TMP_CLIENT_CNF=''
cleanup() {
    [[ -n "$TMP_SQL" ]]        && rm -f "$TMP_SQL"
    [[ -n "$TMP_CLIENT_CNF" ]] && rm -f "$TMP_CLIENT_CNF"
    return 0
}
trap cleanup EXIT
trap 'die "命令执行失败：${BASH_COMMAND}（第 ${LINENO} 行）"' ERR

# ---------------------------------------------------------------------------
# 1. 计算脚本目录；非 root 时自动用 sudo 重新执行自己
#    这样调用者无论从哪个目录执行都能找到同目录的 DBConfig.ini。
# ---------------------------------------------------------------------------
SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "$SCRIPT_PATH")"

usage() {
    cat <<'USAGE'
用法：
  sudo bash prepare_env_V0.sh [DBConfig.ini 路径]
  sudo DB_CONFIG=/path/to/DBConfig.ini bash prepare_env_V0.sh
  sudo MYSQL_ROOT_PASSWORD='root密码' bash prepare_env_V0.sh   # root 非 socket 认证时用

说明：
  - 在 Ubuntu / Debian 上自动安装并配置 MariaDB；
  - 读取 DBConfig.ini 的 [db] 段：dbname / user / password / host / port；
  - 创建数据库、业务账号（'user'@'%' 与 'user'@'localhost'）并授权；
  - 测试阶段开放外网登录（bind-address=0.0.0.0）；
  - 写入 2C2G 调优配置并开启 binlog；
  - 若存在 ./db/schema.sql 会自动导入。
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ "${EUID}" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "需要 root 权限，且系统没有 sudo。"
    info "需要 root 权限，自动通过 sudo 重新执行……"
    exec sudo -- bash "$SCRIPT_PATH" "$@"
fi

# ---------------------------------------------------------------------------
# 2. 解析命令行 / 配置路径
# ---------------------------------------------------------------------------
CONFIG_FILE="${1:-${DB_CONFIG:-${SCRIPT_DIR}/DBConfig.ini}}"

if [[ ! -f "$CONFIG_FILE" ]]; then
    if [[ -f "${SCRIPT_DIR}/DBConfig.ini.example" ]]; then
        die "找不到配置文件：$CONFIG_FILE
请先执行：cp \"${SCRIPT_DIR}/DBConfig.ini.example\" \"${SCRIPT_DIR}/DBConfig.ini\"
然后修改其中的 user / password，再重新运行本脚本。"
    fi
    die "找不到配置文件：$CONFIG_FILE"
fi

CONFIG_FILE="$(cd "$(dirname "$CONFIG_FILE")" && pwd)/$(basename "$CONFIG_FILE")"
info "使用配置文件：$CONFIG_FILE"

# ---------------------------------------------------------------------------
# 3. 解析 INI（只需要 [db] 段）
#    - 支持 CRLF（文件从 Windows 推送过来时常见）；
#    - 只在行首是 # / ; 时才当作注释，密码里出现 # 不受影响；
#    - 只按第一个 '=' 切分，密码里可以有任意字符（包括 = 之外的符号）。
# ---------------------------------------------------------------------------
DB_HOST=''
DB_PORT=''
DB_NAME=''
DB_USER=''
DB_PASS=''
BIND_ADDRESS='0.0.0.0'   # 测试阶段默认监听所有网卡；可在 [db] 用 bind_address 覆盖

trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

in_db=0
while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    line="${raw_line%$'\r'}"          # 去掉 Windows CRLF 的 \r
    line="$(trim "$line")"
    [[ -z "$line" ]] && continue
    [[ "$line" == \#* || "$line" == \;* ]] && continue

    if [[ "$line" == \[*\] ]]; then
        section="${line#[}"; section="${section%]}"
        section="$(printf '%s' "$section" | tr '[:upper:]' '[:lower:]')"
        if [[ "$section" == "db" ]]; then
            in_db=1
        else
            in_db=0
        fi
        continue
    fi
    [[ "$in_db" -eq 1 ]] || continue
    [[ "$line" == *=* ]] || continue

    key="$(trim "${line%%=*}")"
    val="$(trim "${line#*=}")"
    key="$(printf '%s' "$key" | tr '[:upper:]' '[:lower:]')"

    case "$key" in
        host)         DB_HOST="$val" ;;
        port)         DB_PORT="$val" ;;
        dbname)       DB_NAME="$val" ;;
        user)         DB_USER="$val" ;;
        password)     DB_PASS="$val" ;;
        bind_address) BIND_ADDRESS="$val" ;;
        *)            : ;;   # 忽略未知键，保持向后兼容
    esac
done < "$CONFIG_FILE"

# ---------------------------------------------------------------------------
# 4. 校验配置
# ---------------------------------------------------------------------------
: "${DB_NAME:?DBConfig.ini 缺少 dbname}"
: "${DB_USER:?DBConfig.ini 缺少 user}"
: "${DB_PASS:?DBConfig.ini 缺少 password}"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-3306}"

[[ "$DB_PORT" =~ ^[0-9]+$ ]] || die "port 必须是数字：$DB_PORT"
[[ "$DB_NAME" =~ ^[A-Za-z0-9_]+$ ]] || die "dbname 只允许字母/数字/下划线：$DB_NAME"
[[ "$DB_USER" =~ ^[A-Za-z0-9_]+$ ]] || die "user 只允许字母/数字/下划线：$DB_USER"
[[ "$BIND_ADDRESS" =~ ^[0-9A-Za-z\.\:]+$ ]] || die "bind_address 非法：$BIND_ADDRESS"

ok "配置读取成功：db=${DB_NAME} user=${DB_USER} host=${DB_HOST} port=${DB_PORT}"

# ---------------------------------------------------------------------------
# 5. SQL 字符串转义：先把反斜杠翻倍，再把单引号翻倍，最后包上单引号。
#    这样密码里出现 ' 或 \ 也不会破坏 SQL。
# ---------------------------------------------------------------------------
sql_str() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\'/\'\'}"
    printf "'%s'" "$s"
}

# 连接选项文件的转义（用于验证阶段，避免密码出现在进程列表里）。
cnf_str() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    printf '"%s"' "$s"
}

DB_PASS_SQL="$(sql_str "$DB_PASS")"

# ---------------------------------------------------------------------------
# 6. 安装 MariaDB（若未安装）
# ---------------------------------------------------------------------------
command -v apt-get >/dev/null 2>&1 || die "本脚本仅支持 Ubuntu / Debian（未找到 apt-get）。"

# 防止和 Oracle MySQL 冲突：二者不能共存于同一实例。
if dpkg -s mysql-server >/dev/null 2>&1 && ! dpkg -s mariadb-server >/dev/null 2>&1; then
    die "检测到已安装 Oracle MySQL Server。为避免冲突，请先手动卸载 MySQL 或改用对应的 MySQL 配置脚本。"
fi

if ! dpkg -s mariadb-server >/dev/null 2>&1; then
    info "安装 mariadb-server（首次安装可能需要几分钟）……"
    apt-get update -y
    DEBIAN_FRONTEND=noninteractive apt-get install -y mariadb-server mariadb-client
    ok "MariaDB 安装完成"
else
    ok "MariaDB 已安装，跳过安装步骤"
fi

# 识别服务名（Ubuntu 是 mariadb，部分发行版是 mysql）。
# 注意：这里先把输出存进变量再 grep，避免 `systemctl | grep -q` 下
# systemctl 因 SIGPIPE(141) 被 pipefail 判为失败而误报“找不到服务”。
SVC=''
UNIT_FILES=''
if command -v systemctl >/dev/null 2>&1; then
    UNIT_FILES="$(systemctl list-unit-files 2>/dev/null || true)"
fi
if [[ -n "$UNIT_FILES" ]]; then
    if grep -qE '^mariadb\.service' <<<"$UNIT_FILES"; then
        SVC=mariadb
    elif grep -qE '^mysql\.service' <<<"$UNIT_FILES"; then
        SVC=mysql
    fi
fi

# 回退：直接检查 unit 文件（容器 / 精简镜像里 list-unit-files 可能不可用）。
if [[ -z "$SVC" ]]; then
    for f in /lib/systemd/system/mariadb.service \
             /usr/lib/systemd/system/mariadb.service \
             /etc/systemd/system/mariadb.service; do
        if [[ -f "$f" ]]; then SVC=mariadb; break; fi
    done
fi
if [[ -z "$SVC" ]]; then
    for f in /lib/systemd/system/mysql.service \
             /usr/lib/systemd/system/mysql.service \
             /etc/systemd/system/mysql.service; do
        if [[ -f "$f" ]]; then SVC=mysql; break; fi
    done
fi

# 最后回退：看二进制名（配合后面的 service 命令）。
if [[ -z "$SVC" ]]; then
    if command -v mariadbd >/dev/null 2>&1 || command -v mariadbd-safe >/dev/null 2>&1; then
        SVC=mariadb
    elif command -v mysqld >/dev/null 2>&1; then
        SVC=mysql
    fi
fi

[[ -n "$SVC" ]] || die "找不到 mariadb/mysql 服务。请确认安装是否成功：dpkg -l | grep -i mariadb"

MYSQL_BIN="$(command -v mariadb || command -v mysql || true)"
[[ -n "$MYSQL_BIN" ]] || die "找不到 mariadb/mysql 客户端命令。"
info "服务：${SVC}.service    客户端：${MYSQL_BIN}"

# root 认证：默认走 unix_socket；如已设置密码，可用 MYSQL_ROOT_PASSWORD 传入。
mysql_root() {
    if [[ -n "${MYSQL_ROOT_PASSWORD:-}" ]]; then
        MYSQL_PWD="$MYSQL_ROOT_PASSWORD" "$MYSQL_BIN" --protocol=socket -uroot "$@"
    else
        "$MYSQL_BIN" --protocol=socket -uroot "$@"
    fi
}

# ---------------------------------------------------------------------------
# 7. 写入 MariaDB 配置
#    Ubuntu 的 my.cnf 会自动 include /etc/mysql/mariadb.conf.d/ 下的文件，
#    用 99- 前缀保证最后加载、覆盖 50-server.cnf 里的 bind-address 等设置。
# ---------------------------------------------------------------------------
if [[ -d /etc/mysql/mariadb.conf.d ]]; then
    CNF_DIR=/etc/mysql/mariadb.conf.d
elif [[ -d /etc/mysql/conf.d ]]; then
    CNF_DIR=/etc/mysql/conf.d
else
    CNF_DIR=/etc/mysql/mariadb.conf.d
    mkdir -p "$CNF_DIR"
fi
CNF_FILE="${CNF_DIR}/99-cxim.cnf"

# 确保 binlog 目录存在且属主正确（log_bin 指向这里）。
install -d -o mysql -g mysql -m 750 /var/log/mysql

if [[ -f "$CNF_FILE" ]]; then
    cp -a "$CNF_FILE" "${CNF_FILE}.bak.$(date +%Y%m%d%H%M%S)"
    info "已备份旧配置：${CNF_FILE}.bak.*"
fi

info "写入 MariaDB 配置：$CNF_FILE"
cat > "$CNF_FILE" <<EOF
# ============================================================================
# 由 prepare_env_V0.sh 自动生成，请勿手工修改。
# 重新执行脚本会覆盖本文件（并保留带时间戳的 .bak 备份）。
# ============================================================================
[mysqld]

# ---- 字符集：统一 utf8mb4，避免中文 / emoji 乱码 ----
character-set-server = utf8mb4
collation-server = utf8mb4_unicode_ci

# ---- 内存与连接（2C2G 服务器） ----
innodb_buffer_pool_size = 384M
max_connections = 100
performance_schema = OFF
skip_name_resolve = ON

# ---- 可靠性：IM 消息优先不丢，保持 1 ----
innodb_flush_log_at_trx_commit = 1

# ---- Binlog（MariaDB 参数名是 log_bin，不是 log-bin） ----
server-id = 1
log_bin = /var/log/mysql/mariadb-bin
binlog_format = ROW
expire_logs_days = 7

# ---- 测试阶段：允许外网连接 ----
# 注意：正式环境应改为 127.0.0.1，或仅绑定内网地址，并收紧账号 Host。
bind-address = ${BIND_ADDRESS}
EOF
ok "配置写入完成"

# ---------------------------------------------------------------------------
# 8. 启动 / 重启 MariaDB，并等待其可用
# ---------------------------------------------------------------------------
info "重启 ${SVC} 使配置生效……"
systemctl enable "$SVC" >/dev/null 2>&1 || true

if ! systemctl restart "$SVC"; then
    # 不是所有环境都有可用的 systemd（例如容器 / 精简镜像），回退到 service 命令。
    warn "systemctl 重启失败，尝试使用 service 命令……"
    if command -v service >/dev/null 2>&1 && service "$SVC" restart >/dev/null 2>&1; then
        ok "已通过 service 命令重启 ${SVC}"
    else
        warn "服务启动失败，最近日志如下："
        journalctl -u "$SVC" -n 40 --no-pager 2>/dev/null || true
        die "请检查上面的日志。若是 AppArmor 拦截 /var/log/mysql，请放行后重试。"
    fi
fi

info "等待 MariaDB 就绪……"
READY=0
for _ in $(seq 1 30); do
    if mysql_root -e 'SELECT 1' >/dev/null 2>&1; then READY=1; break; fi
    sleep 1
done
if [[ "$READY" -ne 1 ]]; then
    warn "无法以 root 通过 unix_socket 登录 MariaDB。可能是之前执行过 mariadb-secure-installation 给 root 设了密码。"
    die "请用以下方式重试：
  sudo MYSQL_ROOT_PASSWORD='你的root密码' bash prepare_env_V0.sh
若仍失败，排查：systemctl status ${SVC} / journalctl -u ${SVC} -n 50 --no-pager"
fi

# ---------------------------------------------------------------------------
# 9. 创建数据库、业务账号并授权
#    同时把 mariadb-secure-installation 的基础项自动化掉。
# ---------------------------------------------------------------------------
info "创建数据库与账号，并授权……"
TMP_SQL="$(mktemp)"; chmod 600 "$TMP_SQL"
cat > "$TMP_SQL" <<SQL
-- 业务库：统一 utf8mb4，避免中文/emoji 乱码
CREATE DATABASE IF NOT EXISTS ${DB_NAME}
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- 业务账号：测试阶段允许任意主机登录（%）
CREATE USER IF NOT EXISTS '${DB_USER}'@'%' IDENTIFIED BY ${DB_PASS_SQL};
ALTER  USER '${DB_USER}'@'%' IDENTIFIED BY ${DB_PASS_SQL};
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'%';

-- 同时也建 localhost 账号，方便本机运维
CREATE USER IF NOT EXISTS '${DB_USER}'@'localhost' IDENTIFIED BY ${DB_PASS_SQL};
ALTER  USER '${DB_USER}'@'localhost' IDENTIFIED BY ${DB_PASS_SQL};
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'localhost';

-- 等价于 mariadb-secure-installation 的基础清理
DROP USER IF EXISTS ''@'localhost';
DROP USER IF EXISTS ''@'%';
DROP DATABASE IF EXISTS test;

FLUSH PRIVILEGES;
SQL

mysql_root < "$TMP_SQL"

# 彻底清掉所有匿名账号（不同主机名可能残留，避免它们抢先匹配到连接）
ANON_HOSTS=()
while IFS= read -r anon_host; do
    if [[ -n "$anon_host" ]]; then
        ANON_HOSTS+=("$anon_host")
    fi
done < <(mysql_root -N -B -e "SELECT Host FROM mysql.user WHERE User=''" 2>/dev/null || true)
if (( ${#ANON_HOSTS[@]} > 0 )); then
    for anon_host in "${ANON_HOSTS[@]}"; do
        mysql_root -e "DROP USER IF EXISTS ''@'${anon_host}'" >/dev/null 2>&1 || true
    done
fi

ok "数据库 ${DB_NAME} 与账号 ${DB_USER} 创建/更新完成"

# 可选：自动导入业务表结构
SCHEMA_FILE="${SCRIPT_DIR}/db/schema.sql"
if [[ -f "$SCHEMA_FILE" ]]; then
    info "发现业务表结构，自动导入：$SCHEMA_FILE"
    mysql_root "$DB_NAME" < "$SCHEMA_FILE"
    ok "表结构导入完成"
else
    info "未发现 ${SCHEMA_FILE}，跳过建表（后续可放在该路径自动导入）。"
fi

# 可选：自动导入业务表结构
ADMIN_SCHEMA_FILE="${SCRIPT_DIR}/../../../Admin/AdminBackend/db/schema_admin.sql"
if [[ -f "$ADMIN_SCHEMA_FILE" ]]; then
    info "发现Admin业务表结构，自动导入：$ADMIN_SCHEMA_FILE"
    mysql_root "$DB_NAME" < "$ADMIN_SCHEMA_FILE"
    ok "Admin表结构导入完成"
else
    info "未发现 ${ADMIN_SCHEMA_FILE}，跳过建表（后续可放在该路径自动导入）。"
fi
# ---------------------------------------------------------------------------
# 10. 以业务账号走 TCP 回连验证（模拟外网登录，避免把密码写进进程列表）
# ---------------------------------------------------------------------------
info "验证业务账号能否通过 TCP 登录并访问数据库……"
TMP_CLIENT_CNF="$(mktemp)"; chmod 600 "$TMP_CLIENT_CNF"
cat > "$TMP_CLIENT_CNF" <<EOF
[client]
host=127.0.0.1
port=${DB_PORT}
user=${DB_USER}
password=$(cnf_str "$DB_PASS")
EOF

"$MYSQL_BIN" --defaults-extra-file="$TMP_CLIENT_CNF" \
    -e "USE ${DB_NAME}; SELECT CONCAT('login_ok=', CURRENT_USER()) AS result;" \
    >/dev/null
ok "业务账号 TCP 登录验证通过"

# 打印监听地址，确认 0.0.0.0（外网可达）
if command -v ss >/dev/null 2>&1; then
    LISTEN_LINE="$(ss -lnt 2>/dev/null | grep -E "[:.]${DB_PORT}[[:space:]]" || true)"
    [[ -n "$LISTEN_LINE" ]] && info "监听状态：$LISTEN_LINE"
fi

# 若本机 ufw 已启用，顺手放行 3306（未启用则不处理）
if command -v ufw >/dev/null 2>&1; then
    UFW_STATUS="$(ufw status 2>/dev/null || true)"
    if [[ "$UFW_STATUS" == *"Status: active"* ]]; then
        ufw allow "${DB_PORT}/tcp" >/dev/null 2>&1 || true
        info "已在本机 ufw 放行 ${DB_PORT}/tcp"
    fi
fi

# ---------------------------------------------------------------------------
# 11. 汇总
# ---------------------------------------------------------------------------
cat <<EOF

${C_GREEN}============================ 配置完成 ============================${C_RESET}
  数据库名 : ${DB_NAME}（utf8mb4 / utf8mb4_unicode_ci）
  业务账号 : '${DB_USER}'@'%' 与 '${DB_USER}'@'localhost'
  监听地址 : ${BIND_ADDRESS}:${DB_PORT}
  配置文件 : ${CNF_FILE}
  Binlog   : /var/log/mysql/mariadb-bin（保留 7 天）
  数据来源 : ${CONFIG_FILE}

${C_YELLOW}提醒：${C_RESET}
  1) 测试阶段才允许外网登录。请到【腾讯云控制台 -> 安全组】放行 ${DB_PORT}/tcp，
     否则即使本机监听 0.0.0.0 外部也连不上。
  2) 正式环境请把 ${CNF_FILE} 里的 bind-address 改回 127.0.0.1，
     并删除 '${DB_USER}'@'%'，只保留必要来源。
  3) 数据库密码保存在 ${CONFIG_FILE}，该文件已在 .gitignore 中，请勿提交。
  4) 常用排查命令：systemctl status ${SVC} | journalctl -u ${SVC} -f
==================================================================
EOF
