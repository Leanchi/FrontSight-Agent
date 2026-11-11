/* MinGW-w64 兼容性头文件 */
#ifndef MINGW_COMPAT_H
#define MINGW_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
/* 2026-11-11：struct ip_mreq 原在此自定义（2026-05 初版 MinGW 的 ws2tcpip.h 不带它），
 * 新版 MinGW（14.x）的 ws2tcpip.h 自带定义 → 无条件自定义必撞"redefinition"。
 * 修正：直接引 ws2tcpip.h 取工具链定义，删除自定义（上面已 include，下方结构体已移除）。 */

/* ---- socklen_t ---- */
typedef int socklen_t;

/* ---- in6addr_* externs ---- */
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

/* ---- struct ip_mreq：已改由 <ws2tcpip.h> 提供（见文件头注释，2026-11-11） ---- */

/* ---- 常量 ---- */
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 27
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---- MSVC SEH ---- */
#define __try
#define __except(x) if(0)

/* ---- UNREFERENCED_PARAMETER ---- */
#ifdef UNREFERENCED_PARAMETER
#undef UNREFERENCED_PARAMETER
#endif
#define UNREFERENCED_PARAMETER(P) ((void)(P))

#endif
