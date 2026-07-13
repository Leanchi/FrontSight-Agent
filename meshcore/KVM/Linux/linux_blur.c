/*
 * Linux 窗口虚化 - 窗口枚举与可见区域计算
 * 使用 EWMH _NET_CLIENT_LIST_STACKING atom 获取窗口 Z-order
 */

#include "linux_blur.h"
#include "../blur_config.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef BLUR_DEBUG
static FILE *blur_log = NULL;
#define BLUR_LOG(fmt, ...) do { \
    if (!blur_log) blur_log = fopen("/tmp/blur_debug.log", "w"); \
    if (blur_log) { fprintf(blur_log, fmt, ##__VA_ARGS__); fflush(blur_log); } \
} while(0)
#else
#define BLUR_LOG(fmt, ...)
#endif

/* ============================================================================
 * X error handler: 防止 X11 默认 error handler 调用 exit() 杀死 slave 进程.
 *
 * 根因: get_blurred_regions() 遍历窗口列表时, 如果某个窗口正好被销毁,
 * XGetWindowAttributes / XTranslateCoordinates 等 X 调用会触发 X error.
 * X11 默认 error handler 会调用 exit(1), 导致 slave 进程瞬间死亡,
 * 信号 handler 都来不及执行 (因为 exit 不触发信号).
 *
 * 修复: 安装自定义 error handler, 只设置标志位, 不 exit.
 * get_blurred_regions 在每次关键 X 调用后检查标志位, 出错就提前返回.
 * ============================================================================ */
static int g_xerror_occurred = 0;

static int blur_xerror_handler(Display *disp, XErrorEvent *ev)
{
    g_xerror_occurred = 1;
    BLUR_LOG("[BLUR] X error caught! type=%d, serial=%lu, error_code=%d, request_code=%d, minor_code=%d\n",
             ev->type, ev->serial, ev->error_code, ev->request_code, ev->minor_code);
    return 0;  /* 返回 0 表示已处理, X11 不会 abort/exit */
}

/* 裁剪矩形到屏幕范围内 */
static void clip_rect(BlurRect *r, int screen_w, int screen_h)
{
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > screen_w) r->w = screen_w - r->x;
    if (r->y + r->h > screen_h) r->h = screen_h - r->y;
    if (r->w <= 0 || r->h <= 0) { r->w = 0; r->h = 0; }
}

/* 判断两个矩形是否相交 */
static int rect_intersects(BlurRect a, BlurRect b)
{
    return !(a.x >= b.x + b.w || b.x >= a.x + a.w ||
             a.y >= b.y + b.h || b.y >= a.y + a.h);
}

/* 从 src 中减去 sub 的交集，返回拆分后的子矩形列表（最多 4 个）
 * 返回子矩形数量
 */
static int subtract_rect(BlurRect src, BlurRect sub, BlurRect *out_rects, int max_out)
{
    int count = 0;

    /* 计算交集 */
    int inter_x = src.x > sub.x ? src.x : sub.x;
    int inter_y = src.y > sub.y ? src.y : sub.y;
    int inter_w = (src.x + src.w < sub.x + sub.w ? src.x + src.w : sub.x + sub.w) - inter_x;
    int inter_h = (src.y + src.h < sub.y + sub.h ? src.y + src.h : sub.y + sub.h) - inter_y;

    if (inter_w <= 0 || inter_h <= 0) {
        if (count < max_out) out_rects[count++] = src;
        return count;
    }

    /* 左边部分 */
    int left_w = inter_x - src.x;
    if (left_w > 0 && count < max_out) {
        out_rects[count++] = (BlurRect){src.x, src.y, left_w, src.h};
    }

    /* 右边部分 */
    int right_x = inter_x + inter_w;
    int right_w = (src.x + src.w) - right_x;
    if (right_w > 0 && count < max_out) {
        out_rects[count++] = (BlurRect){right_x, src.y, right_w, src.h};
    }

    /* 上边部分 */
    int top_x = src.x > inter_x ? src.x : inter_x;
    int top_w = inter_w < src.w - (top_x - src.x) ? inter_w : src.w - (top_x - src.x);
    int top_h = inter_y - src.y;
    if (top_h > 0 && top_w > 0 && count < max_out) {
        out_rects[count++] = (BlurRect){top_x, src.y, top_w, top_h};
    }

    /* 下边部分 */
    int bottom_y = inter_y + inter_h;
    int bottom_h = (src.y + src.h) - bottom_y;
    if (bottom_h > 0 && top_w > 0 && count < max_out) {
        out_rects[count++] = (BlurRect){top_x, bottom_y, top_w, bottom_h};
    }

    return count;
}

/* 获取窗口标题 */
static char *get_window_title(Display *disp, Window win)
{
    char *result = NULL;

    /* 优先使用 _NET_WM_NAME (UTF-8) */
    Atom net_wm_name = XInternAtom(disp, "_NET_WM_NAME", True);
    if (net_wm_name != None) {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *prop = NULL;

        if (XGetWindowProperty(disp, win, net_wm_name, 0, 1024, False,
                               AnyPropertyType, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) == Success && prop && nitems > 0) {
            result = strdup((char *)prop);
            XFree(prop);
            if (result) return result;  /* 获取成功，直接返回 */
        }
    }

    /* 回退到 WM_NAME */
    char *wm_name = NULL;
    if (XFetchName(disp, win, &wm_name) && wm_name) {
        result = strdup(wm_name);
        XFree(wm_name);
    }

    return result;
}

/* 检查窗口标题是否匹配虚化关键字 */
static int title_matches_blur(const char *title)
{
    if (!title) return 0;
    for (int k = 0; k < BLUR_KEYWORD_COUNT; k++) {
        if (should_blur_window(title, BLUR_KEYWORDS[k])) {
            return 1;
        }
    }
    return 0;
}

int get_blurred_regions(BlurRect **regions)
{
    *regions = NULL;

#ifdef BLUR_DEBUG
    if (blur_log) { fclose(blur_log); blur_log = NULL; }
    BLUR_LOG("[BLUR] get_blurred_regions called\n");
#endif

    /* 安装自定义 X error handler, 防止 X error 触发默认 exit() */
    g_xerror_occurred = 0;
    XSetErrorHandler(blur_xerror_handler);

    Display *disp = XOpenDisplay(NULL);
    if (!disp) {
        BLUR_LOG("[BLUR] XOpenDisplay(NULL) failed, DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        return 0;
    }
    BLUR_LOG("[BLUR] XOpenDisplay OK, DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");

    int screen_num = DefaultScreen(disp);
    int screen_w = DisplayWidth(disp, screen_num);
    int screen_h = DisplayHeight(disp, screen_num);
    BLUR_LOG("[BLUR] Screen: %dx%d\n", screen_w, screen_h);

    if (screen_w == 0 || screen_h == 0) { XCloseDisplay(disp); return 0; }

    /* 获取 _NET_CLIENT_LIST_STACKING atom */
    Atom client_list_stacking = XInternAtom(disp, "_NET_CLIENT_LIST_STACKING", True);
    if (client_list_stacking == None) {
        BLUR_LOG("[BLUR] _NET_CLIENT_LIST_STACKING not supported by WM\n");
        XCloseDisplay(disp);
        return 0;
    }

    /* 查询堆叠窗口列表 */
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    Window root = RootWindow(disp, screen_num);
    if (XGetWindowProperty(disp, root, client_list_stacking, 0, 1024,
                           False, XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) != Success || !prop) {
        BLUR_LOG("[BLUR] XGetWindowProperty(_NET_CLIENT_LIST_STACKING) failed\n");
        XCloseDisplay(disp);
        return 0;
    }

    Window *windows = (Window *)prop;
    int count = (int)nitems;

    BLUR_LOG("[BLUR] _NET_CLIENT_LIST_STACKING returned %d windows\n", count);

    /* 限制窗口数量上限 */
    if (count <= 0 || count > 200) {
        BLUR_LOG("[BLUR] Window count out of range: %d\n", count);
        XFree(prop);
        XCloseDisplay(disp);
        return 0;
    }

    /* 预缓存所有窗口信息 */
    typedef struct {
        int x, y, w, h;
        int is_visible;
        int is_opaque;
    } WindowInfo;

    WindowInfo *win_info = (WindowInfo *)calloc(count, sizeof(WindowInfo));
    if (!win_info) {
        XFree(prop);
        XCloseDisplay(disp);
        return 0;
    }

    /* 拷贝窗口 ID 到本地数组, 因为 prop 之后会被释放
     * 防止窗口在遍历过程中被销毁导致访问悬空指针 */
    Window *win_ids = (Window *)malloc(count * sizeof(Window));
    if (!win_ids) {
        free(win_info);
        XFree(prop);
        XCloseDisplay(disp);
        return 0;
    }
    memcpy(win_ids, windows, count * sizeof(Window));
    XFree(prop);  /* 提前释放, 已拷贝到 win_ids */
    windows = win_ids;

    for (int i = 0; i < count; i++) {
        if (g_xerror_occurred) {
            BLUR_LOG("[BLUR] X error during window info collection at index %d, aborting\n", i);
            break;
        }
        XWindowAttributes attr;
        /* XSync 强制 flush 并等待 error handler 执行, 这样能捕获到 BadWindow 等错误 */
        if (XGetWindowAttributes(disp, windows[i], &attr)) {
            int root_x = 0, root_y = 0;
            Window child;
            /* 将窗口左上角转换为根窗口坐标（UOS/DDE 的窗口坐标相对于父窗口，不是根窗口） */
            XTranslateCoordinates(disp, windows[i], attr.root,
                                  0, 0, &root_x, &root_y, &child);
            win_info[i].x = root_x;
            win_info[i].y = root_y;
            win_info[i].w = attr.width;
            win_info[i].h = attr.height;
            win_info[i].is_visible = (attr.map_state == IsViewable) ? 1 : 0;
            win_info[i].is_opaque = 1; /* 默认不透明 */
        }
    }
    XSync(disp, False);  /* flush 所有 pending error, 确保 handler 被调用 */
    if (g_xerror_occurred) {
        BLUR_LOG("[BLUR] X error detected after window attributes collection, skipping blur this frame\n");
        free(win_info);
        free(win_ids);
        XCloseDisplay(disp);
        return 0;
    }

    /* 收集匹配窗口的索引 */
    int *match_indices = (int *)malloc(count * sizeof(int));
    BlurRect *match_bounds = (BlurRect *)malloc(count * sizeof(BlurRect));
    if (!match_indices || !match_bounds) {
        free(win_info);
        free(win_ids);
        free(match_indices);
        free(match_bounds);
        XCloseDisplay(disp);
        return 0;
    }
    int match_count = 0;

    for (int i = 0; i < count; i++) {
        if (g_xerror_occurred) break;
        if (!win_info[i].is_visible) continue;
        if (win_info[i].w < 50 || win_info[i].h < 50) continue;

        char *title = get_window_title(disp, windows[i]);
        BLUR_LOG("[BLUR] Window[%d]: pos=(%d,%d) size=%dx%d visible=1 title=[%s]\n",
                 i, win_info[i].x, win_info[i].y, win_info[i].w, win_info[i].h,
                 title ? title : "(null)");
        if (title && title_matches_blur(title)) {
            BLUR_LOG("[BLUR]   *** MATCHED blur keyword\n");
            match_indices[match_count] = i;
            match_bounds[match_count] = (BlurRect){win_info[i].x, win_info[i].y, win_info[i].w, win_info[i].h};
            match_count++;
        }
        free(title);
    }
    XSync(disp, False);
    if (g_xerror_occurred) {
        BLUR_LOG("[BLUR] X error during title collection, aborting\n");
        free(win_info);
        free(win_ids);
        free(match_indices);
        free(match_bounds);
        XCloseDisplay(disp);
        return 0;
    }

    BLUR_LOG("[BLUR] Matched %d windows\n", match_count);

    if (match_count == 0) {
        free(win_info);
        free(match_indices);
        free(match_bounds);
        XCloseDisplay(disp);
        return 0;
    }

    /* 预分配输出缓冲区 */
    BlurRect *blur_rects = (BlurRect *)malloc(count * MAX_VISIBLE_RECTS_PER_WINDOW * sizeof(BlurRect));
    if (!blur_rects) {
        free(win_info);
        free(match_indices);
        free(match_bounds);
        XCloseDisplay(disp);
        return 0;
    }

    int blur_count = 0;

    /* 对每个匹配窗口，计算可见区域 */
    for (int m = 0; m < match_count; m++) {
        BlurRect m_bounds = match_bounds[m];
        clip_rect(&m_bounds, screen_w, screen_h);
        if (m_bounds.w == 0 || m_bounds.h == 0) continue;

        BlurRect visible[MAX_VISIBLE_RECTS_PER_WINDOW];
        int vis_count = 1;
        visible[0] = m_bounds;
        int overflow = 0;

        /* _NET_CLIENT_LIST_STACKING 返回的顺序是底层→顶层（索引小=在后面）
         * 只检查索引更大（Z-order 更靠前）的窗口来遮挡计算 */
        for (int f = match_indices[m] + 1; f < count; f++) {
            if (!win_info[f].is_visible) continue;
            if (!win_info[f].is_opaque) continue;

            BlurRect f_bounds = {win_info[f].x, win_info[f].y, win_info[f].w, win_info[f].h};
            clip_rect(&f_bounds, screen_w, screen_h);
            if (f_bounds.w == 0 || f_bounds.h == 0) continue;

            /* 对当前每个可见区域，减去前方窗口的覆盖 */
            BlurRect new_visible[MAX_VISIBLE_RECTS_PER_WINDOW];
            int new_vis_count = 0;
            for (int v = 0; v < vis_count; v++) {
                if (new_vis_count >= MAX_VISIBLE_RECTS_PER_WINDOW - 4) {
                    overflow = 1;
                    break;
                }
                if (!rect_intersects(visible[v], f_bounds)) {
                    new_visible[new_vis_count++] = visible[v];
                } else {
                    BlurRect sub_rects[4];
                    int sub_count = subtract_rect(visible[v], f_bounds, sub_rects, 4);
                    for (int s = 0; s < sub_count; s++) {
                        if (new_vis_count < MAX_VISIBLE_RECTS_PER_WINDOW) {
                            new_visible[new_vis_count++] = sub_rects[s];
                        } else {
                            overflow = 1;
                            break;
                        }
                    }
                }
            }
            vis_count = new_vis_count;
            for (int i = 0; i < vis_count; i++) {
                visible[i] = new_visible[i];
            }
            if (overflow || vis_count == 0) break;
        }

        if (vis_count == 0) continue;

        if (overflow) {
            blur_rects[blur_count++] = m_bounds;
        } else {
            for (int v = 0; v < vis_count; v++) {
                if (visible[v].w > 10 && visible[v].h > 10) {
                    blur_rects[blur_count++] = visible[v];
                }
            }
        }
    }

    free(win_info);
    free(win_ids);
    free(match_indices);
    free(match_bounds);
    XCloseDisplay(disp);

    /* 恢复默认 X error handler, 避免影响主循环的其他 X 调用 */
    XSetErrorHandler(NULL);

    BLUR_LOG("[BLUR] Final: %d blur regions computed\n", blur_count);

    if (blur_count > 0) {
        *regions = blur_rects;
        return blur_count;
    } else {
        free(blur_rects);
        return 0;
    }
}

void free_blurred_regions(BlurRect *regions)
{
    if (regions) {
        free(regions);
    }
}
