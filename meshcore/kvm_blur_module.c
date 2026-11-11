/*
 * kvm-blur C 原生模块（2026-11-11 遮罩运行时关键字通道）
 *
 * 平台中立：纯字符串表模块，零图形 API，三平台（Linux/macOS/Windows）共用一份。
 * JS 端 require('kvm-blur').setKeywords(['a','b']) → blur_set_keywords()
 * → KVM 采集线程每帧经 blur_get_keywords() 读取当前关键字表 → 匹配 → 灰块。
 *
 * 唯一存储点说明：g_blur_override 与写侧互斥量定义在本 .c 而非 blur_config.h。
 * C 语言头文件中的 static 对象是"每编译单元一份"，blur_config.h 会被读侧
 * （mac_kvm.c/linux_blur.c）与写侧（本文件）两个 TU 同时包含，header-static
 * 方案下写侧更新对读侧 TU 永远不可见；故头文件只放类型与访问器声明，
 * 状态收敛到本文件（全工程仅此一份定义）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(WIN32)
#define KVM_BLUR_WIN32 1
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "duktape.h"
#include "microscript/ILibDuktape_Helpers.h"
#include "KVM/blur_config.h"
#include "kvm_blur_module.h"

/* ------------------------------------------------------------------
 * BLUR_DEBUG 条件日志（A6）：沿 linux_blur.c 的 /tmp/blur_debug.log 机制。
 * 差异：本文件用追加模式 "a" —— 记录的是低频 set/get 事件，不应像采集
 * 路径那样每次调用截断重建日志；Windows 无 /tmp，文件日志留待 child-1
 * 挂 sln 时按平台补（BLUR_DEBUG 下宏展开为空，不影响编译）。
 * ------------------------------------------------------------------ */
#if defined(KVM_BLUR_WIN32) || !defined(BLUR_DEBUG)
#define BLURMOD_LOG(fmt, ...)
#else
static FILE *blurmod_log = NULL;
static const void *blurmod_last_src = NULL;   /* get 路径：生效表切换检测 */
#define BLURMOD_LOG(fmt, ...) do { \
    if (!blurmod_log) blurmod_log = fopen("/tmp/blur_debug.log", "a"); \
    if (blurmod_log) { fprintf(blurmod_log, "[KVM-BLUR] " fmt, ##__VA_ARGS__); fflush(blurmod_log); } \
} while(0)
#endif

/* ------------------------------------------------------------------
 * 唯一存储点 + 写侧互斥
 * ------------------------------------------------------------------ */
static BlurKeywordSet *g_blur_override = NULL;   /* NULL = 未设置，读侧回编译期默认 */

#if defined(KVM_BLUR_WIN32)
/* CRITICAL_SECTION 无静态初始化器：在模块 PUSH（require('kvm-blur')）时惰性初始化。
 * require 必先于任何 setKeywords 调用，且互斥只用于串行化写侧（读侧按 design §1
 * 无锁），不存在其他线程的首次触达路径，无需 Interlocked 双检。 */
static CRITICAL_SECTION g_blur_mtx;
static int g_blur_mtx_ready = 0;
static void blur_mtx_init_once(void)
{
    if (!g_blur_mtx_ready) { InitializeCriticalSection(&g_blur_mtx); g_blur_mtx_ready = 1; }
}
static void blur_mtx_lock(void)   { blur_mtx_init_once(); EnterCriticalSection(&g_blur_mtx); }
static void blur_mtx_unlock(void) { LeaveCriticalSection(&g_blur_mtx); }
#else
/* linux/macOS：pthread 静态初始化器（makefile 各 POSIX 目标均链接 -lpthread，
 * macOS pthread 在 libSystem 内） */
static pthread_mutex_t g_blur_mtx = PTHREAD_MUTEX_INITIALIZER;
static void blur_mtx_init_once(void) { }
static void blur_mtx_lock(void)   { pthread_mutex_lock(&g_blur_mtx); }
static void blur_mtx_unlock(void) { pthread_mutex_unlock(&g_blur_mtx); }
#endif

/* 跨平台字符串拷贝（MSVC 无 strdup、MinGW 与 POSIX 命名不一致，自实现统一） */
static char *kvm_blur_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p != NULL) memcpy(p, s, len);
    return p;
}

const char **blur_get_keywords(int *count)
{
    const char **ret;
    BlurKeywordSet *snap;

    /* 读侧无锁（design §1）：一次性快照全局指针到本地，再经快照取 (count, keywords)
     * ——两字段必须同源。若直接多次读全局（本工程全部目标带 -fno-strict-aliasing，
     * 编译器在 *count 写后被迫重读全局），写侧在两次读之间换表/清表时会读到
     * "新 count 配旧表"（越界遍历）甚至 NULL->count（清表窗口崩溃）。
     * 快照后任一表都不可变且不释放（retired 策略），旧新皆自洽；
     * arm64 上解耦加载经地址依赖排序，天然安全。容忍读到旧表一帧（下一帧即新）。
     * 本函数在 50ms 帧节奏的采集热路径上，读锁开销无意义，故不加读锁。 */
    snap = g_blur_override;
    if (snap != NULL)
    {
        if (count != NULL) *count = snap->count;
        /* count=0（显式关闭）时 keywords 为 NULL，调用方循环零次不解引用 */
        ret = (const char **)(snap->keywords);
    }
    else
    {
        if (count != NULL) *count = BLUR_KEYWORD_COUNT;
        ret = BLUR_KEYWORDS;   /* 兜底：编译期默认集，C 层永不因缺配置而失败 */
    }

#if !defined(KVM_BLUR_WIN32) && defined(BLUR_DEBUG)
    /* get 路径日志（A6）仅在生效表切换时记一条：逐帧日志由采集侧 linux_blur.c
     * 自理，此处若每次调用都记会在 50ms 帧节奏下刷爆日志文件 */
    if (ret != (const char **)blurmod_last_src)
    {
        blurmod_last_src = ret;
        BLURMOD_LOG("get_keywords: source switched, ptr=%p (override=%s)\n",
                    (void *)ret, (g_blur_override != NULL) ? "yes" : "no(compiled defaults)");
    }
#endif

    return ret;
}

int blur_set_keywords(const char **kw, int count)
{
    BlurKeywordSet *fresh = NULL;
    BlurKeywordSet *old;

    if (kw == NULL)
    {
        /* kw==NULL：清 override → 回编译期默认集（防御路径；JS 契约中运行时
         * "恢复默认"传空数组=关闭遮罩，重启后删 blur-keywords.json 即本语义） */
        blur_mtx_lock();
        g_blur_override = NULL;
        blur_mtx_unlock();
        BLURMOD_LOG("set_keywords: override cleared, back to compiled defaults\n");
        return 0;
    }

    if (count <= 0)
    {
        /* count<=0：显式关闭遮罩 = 指向 count=0 的空集（区别于 NULL=编译默认） */
        fresh = (BlurKeywordSet *)malloc(sizeof(BlurKeywordSet));
        if (fresh == NULL) return -1;
        fresh->count = 0;
        fresh->keywords = NULL;
    }
    else
    {
        int i;
        fresh = (BlurKeywordSet *)malloc(sizeof(BlurKeywordSet));
        if (fresh == NULL) return -1;
        fresh->keywords = (char **)malloc((size_t)count * sizeof(char *));
        if (fresh->keywords == NULL) { free(fresh); return -1; }
        for (i = 0; i < count; i++)
        {
            fresh->keywords[i] = kvm_blur_strdup(kw[i] != NULL ? kw[i] : "");
            if (fresh->keywords[i] == NULL)
            {
                /* 分配失败：整批回滚，状态不变 */
                int j;
                for (j = 0; j < i; j++) free(fresh->keywords[j]);
                free(fresh->keywords);
                free(fresh);
                BLURMOD_LOG("set_keywords: alloc failed, state unchanged\n");
                return -1;
            }
        }
        fresh->count = count;
    }

    blur_mtx_lock();
    old = g_blur_override;
    g_blur_override = fresh;   /* 整体换引用：读侧任一时刻取到自洽旧表或自洽新表 */
    blur_mtx_unlock();

    /* retired 旧集不释放：采集线程可能正持有 get 返回的表在遍历，立即 free 有
     * use-after-free 风险。奥卡姆取舍 = 进程生命周期内不回收（不做延迟 free/
     * 双缓冲）。泄漏上界：每次变更 ≤64 项 ×(256B 关键字 + 指针) + 表头 ≈ 17KB，
     * 配置变更为人工低频操作，进程生命周期累计可忽略。 */
    (void)old;
    BLURMOD_LOG("set_keywords: applied n=%d\n", fresh->count);
    return fresh->count;
}

/* ------------------------------------------------------------------
 * Duktape 模块导出
 * ------------------------------------------------------------------ */
#define KVM_BLUR_MAX_ITEMS 64        /* 与 Java 侧 canonicalize 上限（≤64 项）对齐 */
#define KVM_BLUR_MAX_ITEM_BYTES 256  /* 单项超 256 字节：整批拒收（design §2） */

/* 空数组的非 NULL 哑表：blur_set_keywords 的 kw==NULL 保留给"清 override 回编译
 * 默认"语义，空数组必须走"显式关闭"分支（count<=0 且 kw!=NULL） */
static const char *kvm_blur_empty_table[1] = { NULL };

static duk_ret_t kvm_blur_setKeywords(duk_context *ctx)
{
    int i, n, applied;
    const char **tmp = NULL;

    /* 非数组：整批拒收（坏帧在 JS 层已拦，C 层再设一道防线） */
    if (!duk_is_array(ctx, 0)) { duk_push_int(ctx, 0); return 1; }

    n = (int)duk_get_length(ctx, 0);
    if (n > KVM_BLUR_MAX_ITEMS)
    {
        BLURMOD_LOG("setKeywords: n=%d exceeds limit, batch rejected\n", n);
        duk_push_int(ctx, 0);
        return 1;
    }

    if (n > 0)
    {
        tmp = (const char **)malloc((size_t)n * sizeof(char *));
        if (tmp == NULL) { duk_push_int(ctx, 0); return 1; }
        for (i = 0; i < n; i++)
        {
            const char *s;
            duk_get_prop_index(ctx, 0, (duk_uarridx_t)i);
            /* duk_to_string 自动接受中文（UTF-8 原样保留）；数字/布尔等一概转字符串 */
            s = duk_to_string(ctx, -1);
            if (s == NULL || strlen(s) > KVM_BLUR_MAX_ITEM_BYTES)
            {
                /* 单项超 256B：整批拒收，不应用任何一部分 */
                int j;
                duk_pop(ctx);
                for (j = 0; j < i; j++) free((void *)tmp[j]);
                free(tmp);
                BLURMOD_LOG("setKeywords: item %d too large, batch rejected\n", i);
                duk_push_int(ctx, 0);
                return 1;
            }
            /* 必须在 pop 前拷贝：duk_to_string 返回的指针归 Duktape 堆所有，
             * pop 之后该字符串可能被 GC 回收 */
            tmp[i] = kvm_blur_strdup(s);
            duk_pop(ctx);
            if (tmp[i] == NULL)
            {
                int j;
                for (j = 0; j < i; j++) free((void *)tmp[j]);
                free(tmp);
                duk_push_int(ctx, 0);
                return 1;
            }
        }
    }

    /* blur_set_keywords 内部深拷贝接管，本侧保留 tmp 的所有权（用后释放）；
     * n==0（空数组=显式关闭遮罩）时传非 NULL 哑表，避免被误判为"清 override" */
    applied = blur_set_keywords(n > 0 ? tmp : kvm_blur_empty_table, n);
    if (tmp != NULL)
    {
        for (i = 0; i < n; i++) free((void *)tmp[i]);
        free(tmp);
    }

    duk_push_int(ctx, applied < 0 ? 0 : applied);   /* 返回应用条数；分配失败按 0 上报 */
    return 1;
}

void kvm_blur_PUSH(duk_context *ctx, void *chain)
{
    (void)chain;

    blur_mtx_init_once();   /* Windows CRITICAL_SECTION 惰性初始化；POSIX 为空操作 */

    duk_push_object(ctx);
    ILibDuktape_CreateInstanceMethod(ctx, "setKeywords", kvm_blur_setKeywords, 1);
}
