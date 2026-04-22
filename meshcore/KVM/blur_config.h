/*
 * 共享模糊关键字配置
 * 修改此文件即可同时影响 macOS 和 Linux 平台的窗口虚化功能
 */

#ifndef BLUR_CONFIG_H
#define BLUR_CONFIG_H

#include <string.h>
#include <ctype.h>

/* 虚化关键字配置（不区分大小写匹配）
 * 修改此数组即可同时影响 macOS 和 Linux 平台 */
#define BLUR_KEYWORD_COUNT 4
static const char *BLUR_KEYWORDS[] = { "Stash", "LM", "控制中心", "字体" };

#define MAX_VISIBLE_RECTS_PER_WINDOW 16

/* 不区分大小写子串匹配检查
 * 返回 1 表示 needle 包含在 haystack 中 */
static int should_blur_window(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;
    if (nlen == 0) return 1;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

#endif
