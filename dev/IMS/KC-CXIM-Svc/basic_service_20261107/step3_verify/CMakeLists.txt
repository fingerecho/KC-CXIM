# 这里不产出可执行文件，只注册一个自定义目标，方便用户知道怎么跑验证脚本。
add_custom_target(verify_tcp
    COMMAND ${CMAKE_COMMAND} -E echo "Run: bash ${CMAKE_CURRENT_SOURCE_DIR}/verify_tcp.sh"
    COMMENT "Usage: ctest or manually run verify_tcp.sh"
)
