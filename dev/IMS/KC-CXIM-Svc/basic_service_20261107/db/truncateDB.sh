#!/usr/bin/env bash
# ============================================================================
# truncateDB.sh —— 按 DBConfig.ini 连接数据库，对目标库执行“全库 TRUNCATE”
# ----------------------------------------------------------------------------
# 功能：
#   1) 读取 DBConfig.ini 的 [db] 段：host / port / dbname / user / password；
#      默认找脚本同级目录的上一级（项目根）的 DBConfig.ini，可用 -c 覆盖，
#      也可用环境变量 DB_CONFIG 指定。
#   2) 列出该库（dbname）下所有 BASE TABLE（视图不能被 TRUNCATE，自动跳过）；
#   3) 打印将要清空的表，二次确认后，在同一个会话里：
#         SET FOREIGN_KEY_CHECKS=0;  逐表 TRUNCATE;  SET FOREIGN_KEY_CHECKS=1;
#      （关掉外键检查才能清空互相引用的表；TRUNCATE 会重置 AUTO_INCREMENT）
#   4) 密码通过临时 --defaults-extra-file 传入，不出现在进程列表里。
#
# 危险：TRUNCATE 是不可回滚的 DDL，会清空全部数据且重置自增主键。
#       请确认 host/port/dbname 指向的是“要清空的库”，执行前自行备份。
#
# 用法：
#   bash db/truncateDB.sh                 # 用默认 DBConfig.ini，交互确认
#   bash db/truncateDB.sh --dry-run       # 只列出会被清空的表，不动数据
#   bash db/truncateDB.sh --yes           # 跳过确认（无人值守 / CI）
#   bash db/truncateDB.sh -c /path/DBConfig.ini
#   DB_CONFIG=/path/DBConfig.ini bash db/truncateDB.sh
#   bash db/truncateDB.sh --help
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

tag()  { printf '%s[truncate]%s ' "$C_BLUE" "$C_RESET"; }
info() { tag; printf '%s\n' "$*"; }
ok()   { printf '%s[  ok  ]%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '%s[warn ]%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()  { printf '%s[ERROR ]%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

TMP_CNF=''
cleanup() { [[ -n "$TMP_CNF" ]] && rm -f "$TMP_CNF"; return 0; }
trap cleanup EXIT
trap 'die "命令执行失败：${BASH_COMMAND}（第 ${LINENO} 行）"' ERR

# ---------------------------------------------------------------------------
# 1. 定位脚本目录 / 默认配置路径
#    本脚本放在 db/ 下，DBConfig.ini 通常在项目根（db 的上一级）。
# ---------------------------------------------------------------------------
SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "$SCRIPT_PATH")"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"

usage() {
    cat <<'USAGE'
用法：
  bash db/truncateDB.sh [选项]

选项：
  -c, --config <PATH>   指定 DBConfig.ini（默认：DB_CONFIG > 项目根/db/../DBConfig.ini > 脚本同级）
  -y, --yes, --force    跳过交互确认（无人值守 / CI 用）
  -n, --dry-run         只列出将被 TRUNCATE 的表，不执行任何写操作
  -h, --help            显示本帮助

也可用环境变量：DB_CONFIG=/path/DBConfig.ini

危险：TRUNCATE 是不可回滚的 DDL，会清空全部数据并重置自增主键。
      执行前请务必确认 dbname 指向“要清空的库”，并自行备份。
USAGE
}

CONFIG_FILE="${DB_CONFIG:-}"
ASSUME_YES=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config)   [[ $# -ge 2 ]] || die "$1 需要一个参数"; CONFIG_FILE="$2"; shift 2 ;;
        --config=*)    CONFIG_FILE="${1#*=}"; shift ;;
        -y|--yes|--force) ASSUME_YES=1; shift ;;
        -n|--dry-run)  DRY_RUN=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "未知参数：$1" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$CONFIG_FILE" ]]; then
    for cand in "${PARENT_DIR}/DBConfig.ini" "${SCRIPT_DIR}/DBConfig.ini"; do
        if [[ -f "$cand" ]]; then CONFIG_FILE="$cand"; break; fi
    done
fi

if [[ ! -f "${CONFIG_FILE:-}" ]]; then
    if [[ -f "${PARENT_DIR}/DBConfig.ini.example" ]]; then
        die "找不到配置文件：${CONFIG_FILE:-<未指定>}
请先执行：cp \"${PARENT_DIR}/DBConfig.ini.example\" \"${PARENT_DIR}/DBConfig.ini\"
再修改其中的连接信息，或直接用 -c 指定路径。"
    fi
    die "找不到配置文件：${CONFIG_FILE:-<未指定>}（可用 -c 指定 DBConfig.ini 路径）"
fi
CONFIG_FILE="$(cd "$(dirname "$CONFIG_FILE")" && pwd)/$(basename "$CONFIG_FILE")"
info "使用配置文件：$CONFIG_FILE"

# ---------------------------------------------------------------------------
# 2. 解析 INI（只需要 [db] 段）
#    - 支持 CRLF（文件从 Windows 推送过来时常见）；
#    - 只在行首是 # / ; 时才当注释，密码里出现 # 不受影响；
#    - 只按第一个 '=' 切分，密码里可以有任意字符（包括 = 之外的符号）。
# ---------------------------------------------------------------------------
trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

DB_HOST=''
DB_PORT=''
DB_NAME=''
DB_USER=''
DB_PASS=''

in_db=0
while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    line="${raw_line%$'\r'}"
    line="$(trim "$line")"
    [[ -z "$line" ]] && continue
    [[ "$line" == \#* || "$line" == \;* ]] && continue

    if [[ "$line" == \[*\] ]]; then
        section="${line#[}"; section="${section%]}"
        section="$(printf '%s' "$section" | tr '[:upper:]' '[:lower:]')"
        [[ "$section" == "db" ]] && in_db=1 || in_db=0
        continue
    fi
    [[ "$in_db" -eq 1 ]] || continue
    [[ "$line" == *=* ]] || continue

    key="$(trim "${line%%=*}")"
    val="$(trim "${line#*=}")"
    key="$(printf '%s' "$key" | tr '[:upper:]' '[:lower:]')"

    case "$key" in
        host)     DB_HOST="$val" ;;
        port)     DB_PORT="$val" ;;
        dbname)   DB_NAME="$val" ;;
        user)     DB_USER="$val" ;;
        password) DB_PASS="$val" ;;
        *)        : ;;
    esac
done < "$CONFIG_FILE"

# ---------------------------------------------------------------------------
# 3. 校验配置
# ---------------------------------------------------------------------------
: "${DB_NAME:?DBConfig.ini 缺少 dbname}"
: "${DB_USER:?DBConfig.ini 缺少 user}"
: "${DB_PASS:?DBConfig.ini 缺少 password}"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-3306}"

[[ "$DB_PORT" =~ ^[0-9]+$ ]]   || die "port 必须是数字：$DB_PORT"
[[ "$DB_NAME" =~ ^[A-Za-z0-9_]+$ ]] || die "dbname 只允许字母/数字/下划线：$DB_NAME"

info "目标：${DB_USER}@${DB_HOST}:${DB_PORT} / db=${DB_NAME}"

# ---------------------------------------------------------------------------
# 4. 找客户端，准备连接选项文件（密码不进进程列表）
# ---------------------------------------------------------------------------
MYSQL_BIN="$(command -v mariadb || command -v mysql || true)"
[[ -n "$MYSQL_BIN" ]] || die "找不到 mariadb/mysql 客户端命令，请先安装 mariadb-client/mysql-client。"

cnf_str() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    printf '"%s"' "$s"
}

TMP_CNF="$(mktemp)"
chmod 600 "$TMP_CNF"
{
    printf '[client]\n'
    printf 'host=%s\n'    "$(cnf_str "$DB_HOST")"
    printf 'port=%s\n'    "$DB_PORT"
    printf 'user=%s\n'    "$(cnf_str "$DB_USER")"
    printf 'password=%s\n' "$(cnf_str "$DB_PASS")"
} > "$TMP_CNF"

# db_exec: 用临时连接文件执行 mysql，参数原样透传
db_exec() {
    "$MYSQL_BIN" --defaults-extra-file="$TMP_CNF" --connect-timeout=10 "$@"
}

# sql_ident: 用反引号包裹标识符，内部反引号翻倍转义
sql_ident() {
    local s="$1"; s="${s//\`/\`\`}"
    printf '`%s`' "$s"
}

# ---------------------------------------------------------------------------
# 5. 连通性测试 + 列出 BASE TABLE
# ---------------------------------------------------------------------------
if ! db_exec -N -B -e "SELECT 1;" >/dev/null; then
    die "无法连接数据库。请检查 host/port/账号/密码、网络与账号权限。"
fi
ok "数据库连接正常"

# dbname 已校验为 [A-Za-z0-9_]，可安全直接拼进 SQL
if ! db_exec -N -B -e \
    "SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='${DB_NAME}' LIMIT 1;" \
    >/dev/null; then
    die "目标库 ${DB_NAME} 不存在（或当前账号看不到它）。"
fi

TABLES=()
while IFS= read -r t; do
    [[ -n "$t" ]] && TABLES+=("$t")
done < <(db_exec -N -B -e \
    "SELECT TABLE_NAME FROM information_schema.TABLES \
       WHERE TABLE_SCHEMA='${DB_NAME}' AND TABLE_TYPE='BASE TABLE' \
     ORDER BY TABLE_NAME;")

if [[ ${#TABLES[@]} -eq 0 ]]; then
    warn "库 ${DB_NAME} 下没有任何 BASE TABLE，无需 TRUNCATE。"
    exit 0
fi

info "库 ${DB_NAME} 下共 ${#TABLES[@]} 张表，将被清空："
for t in "${TABLES[@]}"; do
    printf '    - %s.%s\n' "$(sql_ident "$DB_NAME")" "$(sql_ident "$t")"
done

if [[ "$DRY_RUN" -eq 1 ]]; then
    ok "dry-run：未执行任何写操作。"
    exit 0
fi

# ---------------------------------------------------------------------------
# 6. 二次确认
# ---------------------------------------------------------------------------
if [[ "$ASSUME_YES" -ne 1 ]]; then
    printf '%s[危险 ]%s 即将对 %s.%s 执行全库 TRUNCATE（不可回滚，会重置自增主键）。\n' \
        "$C_YELLOW" "$C_RESET" "$DB_HOST" "$DB_NAME" >&2
    printf '        输入 yes 继续，其它任意输入取消：' >&2
    ans=''
    if [[ -r /dev/tty ]]; then
        read -r ans </dev/tty || die "无终端可输入确认，如需跳过请加 --yes。"
    else
        read -r ans || die "无标准输入可确认，如需跳过请加 --yes。"
    fi
    case "$(printf '%s' "$ans" | tr '[:upper:]' '[:lower:]')" in
        yes|y) : ;;
        *)     warn "已取消，未做任何改动。"; exit 0 ;;
    esac
fi

# ---------------------------------------------------------------------------
# 7. 同一会话内关闭外键检查 -> 逐表 TRUNCATE -> 恢复外键检查
#    SET FOREIGN_KEY_CHECKS 是会话级开关，必须和 TRUNCATE 在同一次连接里执行。
# ---------------------------------------------------------------------------
{
    printf 'SET SESSION FOREIGN_KEY_CHECKS=0;\n'
    for t in "${TABLES[@]}"; do
        printf 'TRUNCATE TABLE %s.%s;\n' "$(sql_ident "$DB_NAME")" "$(sql_ident "$t")"
    done
    printf 'SET SESSION FOREIGN_KEY_CHECKS=1;\n'
} | db_exec

ok "全库 TRUNCATE 完成：${#TABLES[@]} 张表已清空。"

# ---------------------------------------------------------------------------
# 8. 复核剩余行数（尽力而为，不影响前面已成功的结论）
# ---------------------------------------------------------------------------
LEFTOVER=0
CHECK_SQL=""
for t in "${TABLES[@]}"; do
    [[ -n "$CHECK_SQL" ]] && CHECK_SQL+=" UNION ALL "
    CHECK_SQL+="SELECT '$(printf '%s' "$t" | sed "s/'/''/g")' AS tbl, COUNT(*) AS c FROM $(sql_ident "$DB_NAME").$(sql_ident "$t")"
done
if RES="$(db_exec -N -B -e "$CHECK_SQL" 2>/dev/null)"; then
    while IFS=$'\t' read -r tbl cnt; do
        [[ -n "$cnt" && "$cnt" != "0" ]] && { warn "  表 ${tbl} 仍有 ${cnt} 行（可能由外键/触发器写入）"; LEFTOVER=1; }
    done <<< "$RES"
    [[ "$LEFTOVER" -eq 0 ]] && ok "复核：所有目标表行数均为 0。"
else
    warn "复核查询失败，可忽略（TRUNCATE 已成功执行）。"
fi
