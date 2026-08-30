/*
 * mbedtls/version.h 兼容包裹头  --  libutils/pjprojec/ports/freertos/include/mbedtls/
 *
 * mbedtls 4.2 的 mbedtls_version_get_string_full() 变为 0 参数并返回
 * const char *（旧版为 void 版本写入用户缓冲区）。pjproject 的
 * ssl_sock_mbedtls.c 按旧签名调用，这里提供兼容层。
 *
 * 本文件不改动上游 pjproject / mbedtls 源码。
 */
#ifndef PJ_PORTS_MBEDTLS_VERSION_H
#define PJ_PORTS_MBEDTLS_VERSION_H

#include_next <mbedtls/version.h>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 旧签名兼容版：把 4.2 返回的版本字符串拷入用户缓冲区。 */
static inline void mbedtls_version_get_string_full_pj(char *string)
{
    const char *v = mbedtls_version_get_string_full();
    if (v != NULL) {
        strcpy(string, v);
    } else {
        string[0] = '\0';
    }
}

#ifdef __cplusplus
}
#endif

#define mbedtls_version_get_string_full mbedtls_version_get_string_full_pj

#endif /* PJ_PORTS_MBEDTLS_VERSION_H */
