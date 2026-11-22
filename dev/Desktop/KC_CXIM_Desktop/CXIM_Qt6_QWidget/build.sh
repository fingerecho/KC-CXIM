#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# CXIM Qt6 桌面客户端 一键构建脚本 (Git Bash / MSYS2 下运行)
#
#   ./build.sh                # Release 构建, 产物在 build/
#   ./build.sh -d             # Debug 构建
#   ./build.sh -c             # 清空 build/ 后重新配置构建
#   ./build.sh -j 8           # 指定并行任务数
#   ./build.sh --no-deploy    # 跳过 windeployqt (不复制 Qt DLL)
#   ./build.sh -q C:/Qt/6.11.1/mingw_64   # 手动指定 Qt 目录
#
# 产物: CXIM_Qt6_QWidget.exe(主程序) / cxim_smoke.exe(冒烟测试) / cxim_responder.exe(对端应答器)
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BUILD_TYPE="Release"
CLEAN=0
DEPLOY=1
JOBS=""
QT_DIR="${QT_DIR:-}"

# --- 参数解析 ---------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        -d|--debug)     BUILD_TYPE="Debug" ;;
        -r|--release)   BUILD_TYPE="Release" ;;
        -c|--clean)     CLEAN=1 ;;
        --no-deploy)    DEPLOY=0 ;;
        -j)             JOBS="${2:?-j 需要一个数字}"; shift ;;
        -j*)            JOBS="${1#-j}" ;;
        -q|--qt)        QT_DIR="${2:?$1 需要一个路径}"; shift ;;
        -h|--help)      sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)              echo "未知参数: $1 (用 -h 查看帮助)" >&2; exit 1 ;;
    esac
    shift
done

log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

command -v cmake >/dev/null 2>&1 || die "找不到 cmake, 请先安装 CMake 并加入 PATH"

# POSIX 路径 → Windows 路径 (Git Bash 下给 cmake 用)
winpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}

# --- 定位 Qt ----------------------------------------------------------------
if [ -z "$QT_DIR" ]; then
    # 1) PATH 上的 qmake
    if QMAKE_BIN="$(command -v qmake6 2>/dev/null || command -v qmake 2>/dev/null)"; then
        QT_DIR="$(dirname "$(dirname "$QMAKE_BIN")")"
    fi
fi
if [ -z "$QT_DIR" ]; then
    # 2) 常见安装位置里挑版本号最高的 MinGW 64 套件
    for cand in /c/Qt/6.*/mingw_64 /c/Qt/6.*/msvc2022_64 "$HOME/Qt/6.*/mingw_64"; do
        # shellcheck disable=SC2086
        for d in $cand; do [ -d "$d" ] && QT_DIR="$d"; done
    done
fi
[ -n "$QT_DIR" ] && [ -f "$QT_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] \
    || die "未能定位 Qt6 (可用 -q <Qt目录> 或设置环境变量 QT_DIR)"
QT_DIR_BIN="$QT_DIR"          # POSIX 形式, 供 windeployqt 使用
QT_DIR="$(winpath "$QT_DIR")" # Windows 形式, 供 cmake 使用
log "Qt: $QT_DIR"

# --- 定位 MinGW 工具链 ------------------------------------------------------
GENERATOR="${GENERATOR:-MinGW Makefiles}"
if ! command -v mingw32-make >/dev/null 2>&1 && ! command -v make >/dev/null 2>&1; then
    for d in /c/Qt/Tools/mingw*_64; do
        # shellcheck disable=SC2086
        [ -x "$d/bin/mingw32-make.exe" ] && export PATH="$d/bin:$PATH" && break
    done
fi

# --- 清理 -------------------------------------------------------------------
if [ "$CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
    log "清理 $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# --- 配置 -------------------------------------------------------------------
log "cmake configure ($BUILD_TYPE)"
cmake -S "$(winpath "$SCRIPT_DIR")" -B "$(winpath "$BUILD_DIR")" \
      -G "$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_PREFIX_PATH="$QT_DIR"

# --- 编译 -------------------------------------------------------------------
[ -z "$JOBS" ] && JOBS="$(nproc 2>/dev/null || echo 4)"
log "cmake --build -j$JOBS"
cmake --build "$(winpath "$BUILD_DIR")" --parallel "$JOBS"

# --- 部署 Qt 运行库 (双击即可运行) -------------------------------------------
if [ "$DEPLOY" = "1" ]; then
    WINDEPLOYQT="$(dirname "$QT_DIR_BIN")/bin/windeployqt.exe"
    GUI_EXE="$BUILD_DIR/$BUILD_TYPE/CXIM_Qt6_QWidget.exe"
    [ -f "$GUI_EXE" ] || GUI_EXE="$BUILD_DIR/CXIM_Qt6_QWidget.exe"
    if [ -f "$GUI_EXE" ] && [ -f "$WINDEPLOYQT" ]; then
        log "windeployqt (DLL 部署到 build/)"
        MODE=release; [ "$BUILD_TYPE" = "Debug" ] && MODE=debug
        "$WINDEPLOYQT" "--$MODE" --no-translations --no-system-d3d-compiler --no-opengl-sw \
            "$(winpath "$GUI_EXE")" >/dev/null
    else
        warn "未找到 windeployqt, 跳过 DLL 部署 (运行前需把 $QT_DIR/bin 加入 PATH)"
    fi
fi

# --- 汇总 -------------------------------------------------------------------
log "构建完成, 可执行文件:"
found=0
for name in CXIM_Qt6_QWidget cxim_smoke cxim_responder; do
    for exe in "$BUILD_DIR/$name.exe" "$BUILD_DIR/$BUILD_TYPE/$name.exe"; do
        # shellcheck disable=SC2086
        if [ -f "$exe" ]; then
            printf '  %-22s %s\n' "$name.exe" "$(winpath "$exe")"
            found=1
        fi
    done
done
[ "$found" = "1" ] || die "未在 build/ 下找到任何产物"
