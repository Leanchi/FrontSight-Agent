/* MinGW-w64 兼容性头文件 */
#ifndef MINGW_COMPAT_H
#define MINGW_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2ipdef.h>

/* ---- socklen_t ---- */
typedef int socklen_t;

/* ---- in6addr_* externs ---- */
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

/* ---- struct ip_mreq（在 ws2tcpip.h 中，避免冲突） ---- */
struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

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
