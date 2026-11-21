#!/usr/bin/env bash
# ============================================================================
# /*************图片文件功能（七牛云**真实**直传）：真机一键验收脚本***************/
# ----------------------------------------------------------------------------
# 与 verify_file.sh（mock 模式）的分工：
#   verify_file.sh   ：QINIU_ENABLED=0，无七牛账号即可跑全链路控制面（CI 默认）；
#   本脚本            ：QINIU_ENABLED=1，把数据面换成**真实**的七牛表单直传，
#                      验证"凭证→真实上传→stat 核验 commit→302→跟签名直链下载
#                      →删除"端到端跑通（字节出网、逐字节核对完整性）。
#
# 覆盖链路：
#   1) 确保数据库已应用最新 db/schema.sql（包含 files 表；幂等）；
#   2) 校验 QiniuConfig.json 已填**真实**密钥（非模板占位），否则不空跑；
#      并顺手检查 domain 与 zone 是否明显不配套（测试域名带区域标记）；
#   3) 启动 TCP + HTTP 服务（QINIU_ENABLED=1，读 QiniuConfig.json）；
#   4) 运行 test_Qiniu_file_client：真实**分片直传 V2**（单片 + 1.5MB 两片两种
#      对象）→ commit（服务器向七牛 stat 核验）→ 单聊 type=2 → 接收端 302 换取
#      真实私有签名直链 → 跟随下载逐字节核对 → 群聊 type=4 → 删除；
#      负向：未登录/超限/黑名单/虚报 commit/去签名裸直链被拒（后者用一个从未被
#      签名访问过的专用对象测——否则 CDN 的"忽略参数缓存"会让 tokenless 请求
#      照样 200，测的就不是七牛而是缓存了）。
#
# 用法: ./verify_Qiniu_file.sh [tcp_port] [build_dir] [http_port]
#   默认: tcp=8888  build=$(pwd)/build  http=8080
#
# 真实模式**必须**显式开启（避免误在 CI 上打外网 / 拿假密钥空跑给出误导性结论）：
#   RUN_QINIU_REAL=1 bash step3_verify/verify_Qiniu_file.sh
#
# 可调开关（本脚本原样透传给客户端，缺省即"与官方 SDK 示例同款"的行为）：
#   QINIU_UPLOAD_MODE=v2|form   直传方式。默认 v2=分片上传 V2（同官方 C SDK
#                               examples/up_multipart_sample.c 的 Qiniu_Multipart_PutFile）；
#                               form=表单直传（同 examples/up_form_upload.c），
#                               用于交叉判定"是链路/凭证问题还是某种上传方式的问题"。
#   QINIU_PART_SIZE_MB=1..1024  分片大小，默认 4（SDK 默认值；调小到 1 能让几 MB
#                               的样本真的走多片路径）。
#   QINIU_VERIFY_CERTS=1 + QINIU_CA_BUNDLE=<cacert.pem>  打开证书校验。
#
# 说明：
#   * QiniuConfig.json 含密钥，禁止提交仓库（与 JWTConfig/DBConfig 同规则）；
#   * Windows 下 OpenSSL 不读系统证书库：直传/下载默认不校验七牛证书，可用
#     QINIU_VERIFY_CERTS=1 + QINIU_CA_BUNDLE=<cacert.pem> 打开校验；
#   * FILE_BASE_URL 指向客户端可达的 HTTP 地址（单机回环即指回环）。
# ============================================================================
set -euo pipefail

TCP_PORT="${1:-8888}"
BUILD_DIR="${2:-$(pwd)/build}"
HTTP_PORT="${3:-8080}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${DB_CONFIG:-${PROJECT_DIR}/DBConfig.ini}"
JWT_FILE="${JWT_CONFIG:-${PROJECT_DIR}/JWTConfig.json}"
QINIU_FILE="${QINIU_CONFIG:-${PROJECT_DIR}/QiniuConfig.json}"
SCHEMA_SQL="${PROJECT_DIR}/db/schema.sql"

SERVER_BIN=''
CLIENT_BIN=''
SEED_BIN=''
if [[ "$(uname -s)" == Linux* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/cxim-server"
    CLIENT_BIN="${BUILD_DIR}/bin/test_Qiniu_file_client"
    SEED_BIN="${BUILD_DIR}/bin/seed_db"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/Release/cxim-server.exe"
    CLIENT_BIN="${BUILD_DIR}/bin/Release/test_Qiniu_file_client.exe"
    SEED_BIN="${BUILD_DIR}/bin/Release/seed_db.exe"
fi

if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    echo "[verify-qiniu-file] 找不到可执行文件:"
    echo "  server=$SERVER_BIN"
    echo "  client=$CLIENT_BIN"
    echo "  先构建: bash build.sh"
    exit 1
fi

# ---------------------------------------------------------------------------
# 真实模式开关：默认不跑（CI 用 verify_file.sh 跑 mock，不碰外网/不依赖账号）。
# ---------------------------------------------------------------------------
if [[ "${RUN_QINIU_REAL:-0}" != "1" ]]; then
    echo "[verify-qiniu-file] 真实七牛验收默认关闭（避免误打外网 / 假密钥空跑）。"
    echo "  mock 全链路请用: bash step3_verify/verify_file.sh"
    echo "  确认已配好真实 QiniuConfig.json 后手动开启:"
    echo "    RUN_QINIU_REAL=1 bash step3_verify/verify_Qiniu_file.sh"
    exit 0
fi

# ---------------------------------------------------------------------------
# 校验 QiniuConfig.json 存在且填的是真实密钥（不是模板占位）。
# 只读非敏感字段做判定，**绝不打印** access_key/secret_key。
# ---------------------------------------------------------------------------
if [[ ! -f "$QINIU_FILE" ]]; then
    echo "[verify-qiniu-file] 未找到 $QINIU_FILE（真实模式缺配置=启动失败）。"
    echo "  复制 QiniuConfig.json.example 并填入真实 access_key/secret_key/bucket/up_host/domain。"
    exit 1
fi
export QINIU_CONFIG="$QINIU_FILE"

python3 - "$QINIU_FILE" <<'PY' || { echo "[verify-qiniu-file] QiniuConfig.json 仍是模板/缺字段，拒绝空跑。"; exit 1; }
import json,sys,re
cfg=json.load(open(sys.argv[1],encoding="utf-8"))
def placeholder(v):
    return (not v) or ("your" in v.lower()) or ("xxx" in v.lower()) or (v.strip()=="")
need=["access_key","secret_key","bucket","up_host","domain"]
bad=[k for k in need if placeholder(str(cfg.get(k,"")))]
# 模板里示例 domain 常见 hd-bkt.clouddn.com 测试域名——保留通过，仅拦明显占位。
if bad:
    print("  missing/placeholder:", bad); sys.exit(1)
# ---- 只打印**非敏感**字段，AK/SK 一个字符都不出现在验收日志里 ----
zone = cfg.get("zone", 0)
print("  bucket=%s zone=%s up_host=%s domain=%s private_space=%s rs=%s://%s" % (
    cfg.get("bucket"), zone, cfg.get("up_host"), cfg.get("domain"),
    cfg.get("private_space", True), cfg.get("rs_scheme", "https"), cfg.get("rs_host", "rs.qbox.me")))
# ---- domain / up_host / zone 三者是否明显不配套（区域不一致的典型症状就是
# 直传 200、下载却 403：字节确实进了 z2 的空间，域名却指向另一个区域的 bucket）。
# 测试域名的区域标记（hd=华东 z0 / nb=华北 z1 / hn=华南 z2）只是**推断**，
# 认不出来就闭嘴；认出来了也只 WARN 不拦——自有域名没有这种标记，属正常。
marker = None
m = re.search(r"[./]([a-z]{2})-bkt\.", str(cfg.get("domain", "")))
if m: marker = m.group(1)
zone_marker = {0: "hd", 1: "nb", 2: "hn"}.get(int(zone) if str(zone).isdigit() else 0)
if marker and zone_marker and marker != zone_marker:
    print("  ⚠ WARN: domain 的区域标记是 '%s-bkt'，但 zone=%s 对应 '%s-bkt'，"
          "up_host=%s。" % (marker, zone, zone_marker, cfg.get("up_host")))
    print("    直传按 up_host 走（能 200），下载按 domain 走——两者区域不配套时"
          "表现就是『上传成功、commit 通过、下载 403/404』。")
    print("    请到七牛控制台【对象存储 → 空间管理 → %s → 域名】确认这个 bucket 真正"
          "绑定的下载域名（测试域名形如 <前缀>.<区域标记>-bkt.clouddn.com，"
          "且一级测试域名会被回收，生产必须用已备案自有域名）。" % cfg.get("bucket"))
sys.exit(0)
PY
echo "[verify-qiniu-file] QiniuConfig.json 校验通过（真实模式）"

# Windows 下 OpenSSL 运行时 DLL 在 vcpkg 目录，加入 PATH 才能启动服务器。
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    export PATH="${PROJECT_DIR}/vcpkg_installed/x64-windows/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# 应用 schema：保证 files 表存在（可重复执行）
# ---------------------------------------------------------------------------
if [[ -x "$SEED_BIN" ]]; then
    echo "[verify-qiniu-file] 应用 schema.sql ..."
    "$SEED_BIN" "$CONFIG_FILE" "$SCHEMA_SQL"
else
    echo "[verify-qiniu-file] 未找到 seed_db，跳过 schema 自动导入（请确认 files 表已存在）"
fi

export DB_CONFIG="$CONFIG_FILE"
export JWT_CONFIG="$JWT_FILE"
export TCP_PORT HTTP_PORT

########## 图片文件功能（七牛云**真实**直传）：真实模式验收 ##########
export QINIU_ENABLED=1
export FILE_BASE_URL="http://127.0.0.1:${HTTP_PORT}"
# 依据 QiniuConfig.json 的 private_space 告诉客户端"直链该长什么样"：
#   true  → 私有空间：Location 带 e/token 时效签名，额外跑"去签名应被拒"用例；
#   false → 公共空间：Location 是裸直链，跳过时效性用例（无签名参数可言）。
PRIV="$(python3 -c 'import json,sys;print(1 if json.load(open(sys.argv[1],encoding="utf-8")).get("private_space",True) else 0)' "$QINIU_FILE" 2>/dev/null || echo 1)"
export QINIU_PRIVATE_SPACE="$PRIV"
echo "[verify-qiniu-file] private_space=$([ "$PRIV" = 1 ] && echo true || echo false) (QINIU_PRIVATE_SPACE=$PRIV)"
# 直传方式与分片大小原样透传给客户端（客户端里缺省 = v2 + 4MB，与官方 SDK 示例一致）。
export QINIU_UPLOAD_MODE="${QINIU_UPLOAD_MODE:-v2}"
export QINIU_PART_SIZE_MB="${QINIU_PART_SIZE_MB:-4}"
echo "[verify-qiniu-file] 直传方式=$QINIU_UPLOAD_MODE 分片=${QINIU_PART_SIZE_MB}MB"
########## 图片文件功能：环境变量结束 ##########

# ---------------------------------------------------------------------------
# P0 校验：端口必须空着（否则测到的是"别人的服务器"）
# ---------------------------------------------------------------------------
for p in "$TCP_PORT" "$HTTP_PORT"; do
    if (echo > /dev/tcp/127.0.0.1/"$p") 2>/dev/null; then
        echo "[verify-qiniu-file] 端口 :$p 已被占用，先停掉占用它的进程（或换端口）："
        echo "  Windows: netstat -ano | grep :$p   →  taskkill //PID <pid> //F"
        echo "  Linux  : ss -ltnp | grep :$p"
        exit 1
    fi
done

echo "[verify-qiniu-file] 启动服务器 tcp=:$TCP_PORT http=:$HTTP_PORT (QINIU_ENABLED=1 真实七牛)"
echo "[verify-qiniu-file] 可执行文件 server=$SERVER_BIN"
( cd "$PROJECT_DIR" && exec "$SERVER_BIN" "$TCP_PORT" "$CONFIG_FILE" "$HTTP_PORT" ) \
    > "${TMPDIR:-/tmp}/verify_qiniu_file_server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等 TCP 与 HTTP 两个端口就绪（最多约 10 秒），同时盯着我们自己的进程。
ready=0
for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[verify-qiniu-file] 服务器进程（pid=$SERVER_PID）已退出，日志如下："
        cat "${TMPDIR:-/tmp}/verify_qiniu_file_server.log" || true
        exit 1
    fi
    if (echo > /dev/tcp/127.0.0.1/"$TCP_PORT") 2>/dev/null &&
       (echo > /dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ $ready -ne 1 ]]; then
    echo "[verify-qiniu-file] 服务器端口未就绪，日志如下："
    cat "${TMPDIR:-/tmp}/verify_qiniu_file_server.log" || true
    exit 1
fi

# 打印本次实际生效的运行期参数（确认确实跑在真实模式：[qiniu]/[file] 行）。
echo "[verify-qiniu-file] 服务端自述参数："
grep -E "\[db\] pool max|\[server\] (listening|io threads)|\[http\] listening|\[file\]|\[qiniu\]" \
    "${TMPDIR:-/tmp}/verify_qiniu_file_server.log" || echo "  (日志里没找到参数行)"

echo "[verify-qiniu-file] 运行真机验收客户端 test_Qiniu_file_client"
# 真机模式要把 ~1.6MB 的样本**上传两次、再下载回来逐字节比对**（多分片用例），
# 240s 在慢链路上会把好结果掐死成超时——默认给 600s，可用 VERIFY_QINIU_TIMEOUT 调。
CLIENT_TIMEOUT="${VERIFY_QINIU_TIMEOUT:-600}"
# `| tr -d '\000'`：客户端失败时会把**对象字节**（二进制）打进诊断正文，bash 的
# 命令替换遇到 NUL 会报 "ignored null byte in input" 并截断，把关键那行吃掉。
# 客户端那边已经做了可见字符化，这里再兜一道（pipefail 仍取 timeout/客户端的退出码）。
set +e
if command -v timeout >/dev/null 2>&1; then
    OUT="$(timeout "$CLIENT_TIMEOUT" "$CLIENT_BIN" "$HTTP_PORT" "$TCP_PORT" 2>&1 | tr -d '\000')"
    RC=$?
else
    OUT="$("$CLIENT_BIN" "$HTTP_PORT" "$TCP_PORT" 2>&1 | tr -d '\000')"
    RC=$?
fi
set -e

echo "$OUT"

if [[ $RC -ne 0 ]] || ! grep -q "ALL PASS" <<<"$OUT"; then
    # ---- 失败分类：别让人拿着"下载失败"去查签名 --------------------------------
    # 客户端的 [探针] 已经把"域名接入层拒"与"七牛业务层拒"分开了，这里直接复用它的
    # 结论：https 拿 HTML(421/403/429)、http 拿到七牛 JSON（如 download token not
    # specified）⇒ 域名绑定了 bucket、Kodo 也在服务，只是**这个域名没有 HTTPS 站点**
    # （一级测试域名 *-bkt.clouddn.com 就是这样，且它有 QPS/带宽限制，反复自动化
    # 拉取会吃 429）。这两种情况的修法都在七牛控制台的域名里，不在本项目代码里。
    PASSED=$(grep -c "^  PASS:" <<<"$OUT" || true)
    FAILED=$(grep -c "^  FAIL:" <<<"$OUT" || true)
    echo "[verify-qiniu-file] 结果：通过 $PASSED 项，失败 $FAILED 项"
    if grep -q "七牛业务层回话" <<<"$OUT" && grep -q "边缘节点 HTML" <<<"$OUT"; then
        echo "[verify-qiniu-file] 判定：上传/commit(stat 核验)/消息链路/换取入口都已打通，"
        echo "                  失败集中在**下载域名的 HTTPS**：该域名只在 http 上服务（且测试域名有 QPS 限制）。"
        echo "                  修法二选一："
        echo "                    A. 给 bucket 绑已备案自有域名 + 在七牛配 HTTPS 证书，"
        echo "                       再把 QiniuConfig.json 的 domain 换成 https://<你的域名>（推荐，生产必须如此）；"
        echo "                    B. 只在本地测试期把 domain 改成 http://<测试域名> —— 这会让私有空间的"
        echo "                       时效签名 token 走明文链路，属 knowingly 降低安全性，须由你本人改并知情。"
        echo "                  本脚本不替你改 QiniuConfig.json（含密钥与部署口径，改动该由人来做）。"
    fi
    echo "[verify-qiniu-file] 真机验收失败，服务器日志尾部："
    tail -n 60 "${TMPDIR:-/tmp}/verify_qiniu_file_server.log" || true
    echo "[verify-qiniu-file] 排查提示（客户端/服务端都已把 X-Reqid 与七牛原话打全，优先看那几行）："
    echo "  * 【直传】bad token / BadToken：**先怀疑签名口径，再怀疑账号**——uptoken 的 sign 必须"
    echo "     =urlsafe_b64(hmac_sha1(SK, encodedPolicy))，待签串里混进上传 URL 的 path 是本项目踩过的坑；"
    echo "     排除后才看 AK-SK 不配对 / AK 无该 bucket 上传权限 / up_host 区域≠bucket 实际区域。"
    echo "     定位办法：python3 step3_verify/qiniu_token_probe.py（与服务端同一份配置）；"
    echo "  * 【直传】expired token：**先查 policy 的有效期字段名**，必须是 deadline（绝对 Unix 秒），写成"
    echo "     expires 会被七牛整字段忽略→按 0 处理→永远 expired（签名却是好的）；字段名没错时才去校时；"
    echo "  * 【直传】V2 三步(init/part#N/complete)死在哪一步客户端会写在 step= 里：init 就 401 通常是凭证/"
    echo "     scope 问题；part 401 而 init 过了多半是 UpToken 头没带上；complete 400 常见是 parts 的"
    echo "     etag/partNumber 与实发分片对不上（片大小、顺序）。可先 QINIU_UPLOAD_MODE=form 交叉验证；"
    echo "  * 【commit】qiniu stat http=401 BadToken：管理 API 走的是**官方 v7 签名**，signStr 是"
    echo "     '<METHOD> <Path>[?Query]\\nHost: <Host>\\n[Content-Type: ..\\n][X-Qiniu-Date: ..\\n]\\n[Body]'"
    echo "     ——method 与 path 之间是**空格**、Host 是 'Host: xxx' 头形式、头与 body 之间**必有一个空行**、"
    echo "     且'发出去的头'与'签进去的头'必须成对（见 qiniu_signer.cpp::makeMacAuthorization 与"
    echo "     object_store.cpp::stat/remove）。本项目在这一处连摔两次：先是 q-sign v2，后是"
    echo "     'METHOD\\\\nHost\\\\nPath'，两次**单元测试都全绿**（自洽≠与官方一致），只有真机能揭穿；"
    echo "  * 【commit】qiniu stat http=612：签名与权限都是对的，七牛说'查无此 entry'。先确认真的直传成功"
    echo "     过（看客户端 [直传] 那行的 hash），再查 entry 的拼接顺序必须是 **bucket:key**"
    echo "     （qsign::entryBase64；反了不会报鉴权错，只会'查无此对象'，极易误判）；"
    echo "  * 【commit】qiniu stat http=401 且响应体不是七牛的 JSON 而是 nginx/openresty 的 HTML：那是"
    echo "     链路上的默认站点拒的，与签名无关——本服务器已无条件给管理 API 连接发 SNI，若仍如此，检查"
    echo "     rs_host 是否被本地代理/hosts 改写；"
    echo "  * 【下载】非 200 且响应体是 HTML（421 Misdirected Request / 403 Forbidden / 429）：这是"
    echo "     CDN **域名接入层**拒的，七牛业务层还没看过这个请求，所以与我们的签名/entry/凭证**无关**——"
    echo "     客户端这时会自己再打两条不带签名的 [探针] 请求来分开两种根因："
    echo "       - https 那条是 HTML、http 那条回到七牛业务层 ⇒ 该域名**没有可用的 HTTPS 站点**。"
    echo "         一级测试域名(*-bkt.clouddn.com)就是这种：它只在 http 上服务，且限 QPS（连续自动化"
    echo "         下载会吃 429）。正解是在七牛控制台给这个 bucket 绑**已备案自有域名**并配好 HTTPS"
    echo "         证书，然后把 QiniuConfig.json 的 domain 换成它；"
    echo "       - 两条都是 HTML/连不上 ⇒ domain 根本没绑定这个 bucket（或测试域名已被回收/解析漂走）。"
    echo "     注意私有空间的下载签名只签 path+e、不含 host，所以换域名/scheme 不需要重签——"
    echo "     但也别为了跑绿就把 domain 改成 http:// 长期留在配置里：签名 token 是密钥派生的凭证，"
    echo "     走明文等于把它暴露在链路上（本脚本刻意不替你改这个）。"
    echo "     另外核对 private_space 与 QINIU_PRIVATE_SPACE 是否一致；"
    echo "  * 【证书】ca_bundle 留空 = 管理 API 不校验对端证书（启动日志有 WARNING）。真机排查够用，"
    echo "     生产请配 ca_bundle 并打开 QINIU_VERIFY_CERTS=1 + QINIU_CA_BUNDLE=<cacert.pem>；"
    echo "  * 控制面负向的完整回归以 verify_file.sh（mock）为准；本脚本只跑真实模式。"
    exit 1
fi

echo "[verify-qiniu-file] ALL PASS"
# /*************图片文件功能（七牛云真实直传）：真机一键验收脚本结束***************/
