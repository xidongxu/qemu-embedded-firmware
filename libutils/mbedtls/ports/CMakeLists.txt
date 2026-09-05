# mbedtls 4.2.0（含 TF-PSA-Crypto）FreeRTOS 端口 -- ports/CMakeLists.txt
#
# 以 MBEDTLS_AS_SUBPROJECT 方式把上游 mbedtls 作为子项目编译，所有裁剪/覆盖
# 配置集中在 ports/：
#   - mbedtls_config.h           (MBEDTLS_CONFIG_FILE)   mbedtls 层配置
#   - tf_psa_crypto_config.h     (TF_PSA_CRYPTO_CONFIG_FILE) crypto 层配置
#   - mbedtls_port.c             裸机平台对接（ms_time / 熵源）
#
# 目标：
#   mbedtls / mbedx509 / mbedcrypto -- 上游静态库（由 mbedtls CMake 创建）
#   mbedtls_port                    -- 裸机平台对接库
#
# 注意：不要修改上游 mbedtls 源码，配置全部经由 ports 下文件注入。

set(MBEDTLS_AS_SUBPROJECT ON CACHE BOOL "Mbed TLS as subproject" FORCE)
set(MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_config.h"
    CACHE FILEPATH "Mbed TLS config (ports)" FORCE)
set(TF_PSA_CRYPTO_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tf_psa_crypto_config.h"
    CACHE FILEPATH "TF-PSA-Crypto config (ports)" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)

# 上游 mbedtls/CMakeLists.txt 声明 cmake_minimum_required(3.5.1)，低于 CMake 4
# 的 3.10 兼容门槛 → 每次 configure 报 "Compatibility with CMake < 3.10" 的
# deprecated 警告。不修改上游源码：CMake 4.4 的 CMD_DEPRECATED 诊断由
# cmake_diagnostic() 控制（旧版 CMAKE_WARN_DEPRECATED 变量已被 CMP0218 忽略），
# 在拉入 mbedtls 子树前把该诊断设为 IGNORE。子目录的诊断栈继承本设置。
cmake_diagnostic(SET CMD_DEPRECATED IGNORE)

# 上游 mbedtls 根目录（ports 的上一级）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. mbedtls_build)

# 裸机平台对接（时间 / 熵源），链接 mbedtls 库 + FreeRTOS 内核头
add_library(mbedtls_port STATIC mbedtls_port.c)
target_compile_definitions(mbedtls_port PRIVATE
    MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_config.h"
    TF_PSA_CRYPTO_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tf_psa_crypto_config.h")
# 复用 mbedtls target 的 include 目录（含 build 生成头、private 头），但【不链接】
# mbedtls：若建立依赖，CMake 会把 mbedtls_port 排到 mbedtls 之前，破坏静态库
# 顺序（mbedtls.a 引用 mbedtls_port.a 的 mbedtls_ms_time，须 mbedtls 在前）。
get_target_property(_mbedtls_port_incs mbedtls INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(mbedtls_port PRIVATE ${_mbedtls_port_incs})
# PRIVATE 链接：mbedtls_port.c 编译需要 freertos 头。
# 注意：crypto 库的 CMake target 名是 tfpsacrypto（libmbedcrypto.a 只是其
# 输出文件名 RENAME，非独立 target）。
target_link_libraries(mbedtls_port
    PRIVATE
        freertos_kernel
)
