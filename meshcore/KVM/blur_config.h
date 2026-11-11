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

/* ------------------------------------------------------------------
 * 运行时关键字 override（2026-11-11 遮罩运行时关键字通道）
 *
 * 唯一存储点在 meshcore/kvm_blur_module.c（g_blur_override + 写侧互斥的
 * 定义均在那个 .c 中），本头文件只暴露类型与访问器声明。
 * 原因：C 语言头文件中的 static 对象是"每编译单元一份"——blur_config.h
 * 会被采集线程（mac_kvm.c/linux_blur.c）与写侧模块（kvm_blur_module.c）
 * 两个 TU 同时包含，header-static 方案下写侧更新对读侧 TU 永远不可见，
 * 故状态必须收敛到单一 .c 定义、经下面的访问器跨 TU 使用。
 *
 * 线程模型（design §1）：采集线程读 vs JS 引擎线程写。
 * 写侧永不原地改表，只在 mutex 内整体换引用；读侧无锁一次性读指针，
 * 对齐指针读写在 x86/arm64 上事实原子，容忍读到旧表一帧（下一帧即新）。
 * ------------------------------------------------------------------ */
typedef struct BlurKeywordSet
{
    int count;        /* 关键字条数；0 = 显式关闭遮罩（区别于 override=NULL=用编译默认） */
    char **keywords;  /* 深拷贝字符串表（UTF-8，可含中文），由写侧整体分配 */
} BlurKeywordSet;

/* 读侧：采集线程每帧调用。
 * override 已设置 → 返回该表（count=0 时调用方循环零次，即无遮罩）；
 * override 为 NULL → 返回编译期 BLUR_KEYWORDS / BLUR_KEYWORD_COUNT 兜底。 */
const char **blur_get_keywords(int *count);

/* 写侧：仅由 kvm-blur Duktape 模块（JS 引擎线程）调用。
 * kw==NULL        → 清 override，回编译期默认集（防御路径）；
 * kw!=NULL,count<=0 → 显式关闭遮罩（空集）；
 * kw!=NULL,count>0  → 深拷贝接管，mutex 内整体换引用，换下的旧表不释放
 *                      （retired，泄漏上界与理由见 kvm_blur_module.c）。
 * 返回应用条数，-1 = 分配失败（状态不变）。 */
int blur_set_keywords(const char **kw, int count);

#endif
