#!/usr/bin/env bash
# build_apk.sh —— 构建 HelloWorld APK 的一键脚本
# 用法:
#   ./build_apk.sh          构建 debug APK
#   ./build_apk.sh release  构建 release APK（未配置签名，产物为未签名包）
set -e
cd "$(dirname "$0")"

BUILD_TYPE="${1:-debug}"
case "$BUILD_TYPE" in
    debug)   TASK="assembleDebug"   ;;
    release) TASK="assembleRelease" ;;
    *) echo "用法: $0 [debug|release]"; exit 1 ;;
esac

# ---------- 环境检测 ----------
# JDK：优先使用已设置的 JAVA_HOME，否则回退到本机默认安装的 JDK 21
if [ -z "$JAVA_HOME" ] || [ ! -x "$JAVA_HOME/bin/java" ]; then
    if [ -x "/d/Programs/AndroidDev/JDK/jdk-21.0.13.1_windows-x64_bin/jdk-21.0.13.1/bin/java" ]; then
        export JAVA_HOME="D:/Programs/AndroidDev/JDK/jdk-21.0.13.1_windows-x64_bin/jdk-21.0.13.1"
    fi
fi
[ -n "$JAVA_HOME" ] && echo "[信息] JAVA_HOME=$JAVA_HOME"

# Android SDK：优先使用环境变量，否则回退到本机默认路径
if [ -z "$ANDROID_HOME" ] && [ -d "/d/Android/Sdk" ]; then
    export ANDROID_HOME="D:/Android/Sdk"
fi
[ -n "$ANDROID_HOME" ] && echo "[信息] ANDROID_HOME=$ANDROID_HOME"

# ---------- 选择 Gradle 命令 ----------
# 优先级: PATH 中的 gradle > 本机安装的 Gradle > 项目自带的 gradlew
GRADLE=""
if command -v gradle >/dev/null 2>&1; then
    GRADLE="gradle"
elif [ -x "/d/Android/Gradle/gradle-8.7-bin/gradle-8.7/bin/gradle" ]; then
    GRADLE="/d/Android/Gradle/gradle-8.7-bin/gradle-8.7/bin/gradle"
elif [ -f "./gradlew" ]; then
    GRADLE="./gradlew"
else
    echo "[错误] 未找到 Gradle，请安装 Gradle 或生成 gradlew 包装器" >&2
    exit 1
fi
echo "[信息] 使用 Gradle: $GRADLE"

# ---------- 构建 ----------
echo "[信息] 开始构建 ${BUILD_TYPE} APK ..."
"$GRADLE" "$TASK" --no-daemon

# ---------- 输出结果 ----------
# 取最新产出的 APK（release 目录里可能还留着上一次构建的未签名旧包）
APK=$(find "app/build/outputs/apk/$BUILD_TYPE" -name "*.apk" 2>/dev/null \
        -printf '%T@ %p\n' | sort -rn | head -1 | cut -d' ' -f2-)
if [ -z "$APK" ]; then
    echo "[错误] 构建完成但未找到 APK 文件" >&2
    exit 1
fi

echo ""
echo "[成功] APK 已生成: $APK"

# ---------- 签名自检 ----------
# 未签名的 APK 在真机上安装会报「软件包似乎无效」，这里提前查出来而不是让人去踩
APKSIGNER=""
for C in "$ANDROID_HOME"/build-tools/*/apksigner "$ANDROID_HOME"/build-tools/*/apksigner.bat; do
    [ -e "$C" ] && APKSIGNER="$C" && break
done

if [ ! -e "keystore.properties" ] && [ "$BUILD_TYPE" = "release" ]; then
    echo "[警告] 项目根缺少 keystore.properties，release 产物是未签名包，真机装不上。"
    echo "[提示] 生成本地发布库："
    echo "       keytool -genkeypair -keystore cxim-release.jks -storetype PKCS12 \\"
    echo "         -alias cxim -keyalg RSA -keysize 2048 -sigalg SHA256withRSA -validity 10000 \\"
    echo "         -dname \"CN=CXIM Android, OU=Dev, O=KC-CXIM, L=Beijing, C=CN\""
    echo "       再在项目根写 keystore.properties：storeFile/storePassword/keyAlias/keyPassword"
elif [ -n "$APKSIGNER" ]; then
    if "$APKSIGNER" verify "$APK" >/dev/null 2>&1; then
        # 直接列出实际生效的签名方案，比给个数字有用：
        # Android 11+ 只看 v2 及以上，v1（JAR 签名）在 minSdk>=24 时 AGP 会省掉
        echo "[签名] 校验通过：$( "$APKSIGNER" verify --verbose "$APK" 2>/dev/null \
            | grep -E 'Verified using v[0-9.]+ scheme' | grep true | sed 's/Verified using //;s/ (.*//' | tr '\n' ' ')"
    else
        echo "[错误] 该 APK 签名校验不通过，安装会被系统拒绝：" >&2
        "$APKSIGNER" verify "$APK" 2>&1 | head -3 >&2
        exit 1
    fi
fi

echo "[提示] 安装到设备/模拟器: adb install -r \"$APK\""
