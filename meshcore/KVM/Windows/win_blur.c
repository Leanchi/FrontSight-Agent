/*
 * Windows 窗口虚化 - 窗口枚举与可见区域计算
 *
 * 与 Linux 版（linux_blur.c，X11 _NET_CLIENT_LIST_STACKING）功能对齐，实现不同：
 * Windows 用 EnumWindows 枚举顶层窗口，回调顺序即 Z 序（最顶层最先回调，
 * 与 GetWindow(GW_HWNDNEXT) 链同序）。自顶向下单趟遍历：
 *   1) 每个可见窗口的 rect 累入"已覆盖集"（无论是否命中关键字）
 *   2) 标题命中关键字的窗口，其可见区域 = 自身 rect 减去已覆盖集
 *
 * 取舍（奥卡姆，design §1 明确）：不追求完美遮挡几何——
 *   - 所有通过过滤的窗口一律视为不透明覆盖（分层/透明窗也遮，多遮 = 安全方向）
 *   - 每窗可见区域拆分上限 MAX_VISIBLE_RECTS_PER_WINDOW（blur_config.h），
 *     溢出退化为整窗覆盖（多遮 = 安全方向）
 *   - 无标题窗口不参与覆盖（多遮 = 安全方向；被盖住的大头不遮即达标）
 *
 * DWM cloaked 窗口整体跳过（既不匹配也不覆盖）——这是刻意的泄漏方向防护：
 * Win10+ 虚拟桌面隐藏窗/挂起 UWP 会 EnumWindows 出来但不可见，若只剔除"匹配"
 * 而保留"覆盖"，matched 窗口实际可见的位置会因错误覆盖而漏遮。
 * DwmGetWindowAttribute 失败（老系统/关闭 DWM）按未 cloaked 处理，退化为基本过滤。
 *
 * 红点画笔 overlay（MeshCentralPaintbrushOverlay）同理整体跳过：全屏像素透明
 * 浮层计入覆盖集会把整屏当遮挡物，画笔会话期间遮罩将静默失效。
 *
 * 失败安全：任何 API 失败/分配失败 → 返回 0 区域（画面原样）；
 * 全函数无崩溃路径（指针判空、坐标钳制、缓冲区写前越界检查）。
 */

#if defined(_LINKVM)

#pragma warning(disable : 4996)		/* fopen 等 CRT 安全告警，沿 kvm.c 惯例 */

#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "win_blur.h"
#include "../blur_config.h"

#pragma comment(lib, "dwmapi.lib")

/* 老版 SDK 防御：DWMWA_CLOAKED = 14（Win8+ dwmapi.h 均有定义，此处仅兜底） */
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

/* 屏幕指标 / 缩放因子 / 像素字节数定义在 kvm.c（tile.cpp 同款 extern 形态） */
extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;
extern int SCREEN_X;
extern int SCREEN_Y;
extern int SCALING_FACTOR;
extern int PIXEL_SIZE;

/* 上限常量 */
#define BLUR_MAX_COVERED_RECTS	1024	/* 覆盖集容量；满后停止累计（少减 = 多遮，安全） */
#define BLUR_MAX_OUTPUT_RECTS	256		/* 输出区域容量；将满时退化整窗覆盖（多遮，安全） */
#define BLUR_TITLE_MAX_CHARS	512		/* 标题取前 512 个 UTF-16 字符参与匹配 */
#define BLUR_MIN_MATCH_SIZE		50		/* <50x50 的窗口不匹配不覆盖（沿 linux_blur.c） */
#define BLUR_MIN_PIECE_SIZE		10		/* 可见拆分碎片 <10x10 丢弃（沿 linux_blur.c） */

/* 红点画笔 overlay 窗口类名（与 tile.cpp PAINTBRUSH_WINDOW_CLASS 同字面量，
 * 该常量在 tile.cpp 内部定义无法共用，改动时须两处同步） */
#define WIN_BLUR_PAINTBRUSH_CLASS	L"MeshCentralPaintbrushOverlay"

#ifdef BLUR_DEBUG
/* BLUR_DEBUG 日志：Windows 无 /tmp，沿 %TEMP% 惯例（GetTempPathA）写 blur_debug.log。
 * 语义同 linux_blur.c——每次 get_blurred_regions 调用截断重建（"w"），落盘内容为
 * 最后一帧的枚举过程；遮罩为 50ms 帧节奏的热路径，不追加只重建，防日志无限膨胀。
 * 激活方式：vcxproj 预处理器定义加 BLUR_DEBUG（对齐 makefile CEXTRA 用法）。 */
static FILE *blur_log = NULL;

static void blur_log_reopen(void)
{
	char path[MAX_PATH];
	char file[MAX_PATH];

	if (blur_log) { fclose(blur_log); blur_log = NULL; }
	if (GetTempPathA(MAX_PATH, path) == 0) return;
	if (sprintf_s(file, sizeof(file), "%sblur_debug.log", path) <= 0) return;
	blur_log = fopen(file, "w");
}

/* 走函数而非变参宏：规避 MSVC 传统/一致性预处理器对空 __VA_ARGS__ 逗号处理的差异 */
static void blur_log_write(const char *fmt, ...)
{
	va_list ap;

	if (!blur_log) return;
	va_start(ap, fmt);
	vfprintf(blur_log, fmt, ap);
	va_end(ap);
	fflush(blur_log);
}

#define BLUR_LOG_OPEN()		blur_log_reopen()
#define BLUR_LOG(...)		blur_log_write(__VA_ARGS__)
#else
#define BLUR_LOG_OPEN()
#define BLUR_LOG(...)
#endif

/* ------------------------------------------------------------------
 * 纯几何辅助（与 linux_blur.c 同名逻辑，Windows 侧独立一份，平台中立）
 * ------------------------------------------------------------------ */

/* 裁剪矩形到 (0,0,screen_w,screen_h) 范围内，出界部分收缩；无效置零 */
static void win_blur_clip_rect(BlurRect *r, int screen_w, int screen_h)
{
	if (r->x < 0) { r->w += r->x; r->x = 0; }
	if (r->y < 0) { r->h += r->y; r->y = 0; }
	if (r->x + r->w > screen_w) r->w = screen_w - r->x;
	if (r->y + r->h > screen_h) r->h = screen_h - r->y;
	if (r->w <= 0 || r->h <= 0) { r->w = 0; r->h = 0; }
}

/* 判断两个矩形是否相交 */
static int win_blur_rect_intersects(BlurRect a, BlurRect b)
{
	return !(a.x >= b.x + b.w || b.x >= a.x + a.w ||
		a.y >= b.y + b.h || b.y >= a.y + a.h);
}

/* 从 src 中减去 sub 的交集，拆分为最多 4 个子矩形（左/右/上/下），
 * 返回子矩形数量（无交集时原样返回 src 一件） */
static int win_blur_subtract_rect(BlurRect src, BlurRect sub, BlurRect *out_rects, int max_out)
{
	int count = 0;
	int inter_x, inter_y, inter_w, inter_h;
	int left_w, right_x, right_w, top_x, top_w, top_h, bottom_y, bottom_h;

	/* 计算交集 */
	inter_x = src.x > sub.x ? src.x : sub.x;
	inter_y = src.y > sub.y ? src.y : sub.y;
	inter_w = (src.x + src.w < sub.x + sub.w ? src.x + src.w : sub.x + sub.w) - inter_x;
	inter_h = (src.y + src.h < sub.y + sub.h ? src.y + src.h : sub.y + sub.h) - inter_y;

	if (inter_w <= 0 || inter_h <= 0) {
		if (count < max_out) out_rects[count++] = src;
		return count;
	}

	/* 左边部分 */
	left_w = inter_x - src.x;
	if (left_w > 0 && count < max_out) {
		out_rects[count].x = src.x; out_rects[count].y = src.y;
		out_rects[count].w = left_w; out_rects[count].h = src.h;
		count++;
	}

	/* 右边部分 */
	right_x = inter_x + inter_w;
	right_w = (src.x + src.w) - right_x;
	if (right_w > 0 && count < max_out) {
		out_rects[count].x = right_x; out_rects[count].y = src.y;
		out_rects[count].w = right_w; out_rects[count].h = src.h;
		count++;
	}

	/* 上边部分 */
	top_x = src.x > inter_x ? src.x : inter_x;
	top_w = inter_w < src.w - (top_x - src.x) ? inter_w : src.w - (top_x - src.x);
	top_h = inter_y - src.y;
	if (top_h > 0 && top_w > 0 && count < max_out) {
		out_rects[count].x = top_x; out_rects[count].y = src.y;
		out_rects[count].w = top_w; out_rects[count].h = top_h;
		count++;
	}

	/* 下边部分 */
	bottom_y = inter_y + inter_h;
	bottom_h = (src.y + src.h) - bottom_y;
	if (bottom_h > 0 && top_w > 0 && count < max_out) {
		out_rects[count].x = top_x; out_rects[count].y = bottom_y;
		out_rects[count].w = top_w; out_rects[count].h = bottom_h;
		count++;
	}

	return count;
}

/* ------------------------------------------------------------------
 * 标题匹配
 * ------------------------------------------------------------------ */

/* 取窗口标题（UTF-16 → UTF-8）并逐关键字 should_blur_window 匹配。
 * 关键字来源走 blur_get_keywords() 运行时访问器（blur_config.h，存储点
 * kvm_blur_module.c）——与 mac/linux 同源，天然获得运行时可配能力。 */
static int win_blur_title_matches(HWND hwnd)
{
	int len, k, kwcount = 0;
	const char **kws;
	WCHAR wtitle[BLUR_TITLE_MAX_CHARS + 1];
	char utf8[BLUR_TITLE_MAX_CHARS * 3 + 1];

	len = GetWindowTextLengthW(hwnd);
	if (len <= 0) return 0;
	if (len > BLUR_TITLE_MAX_CHARS) len = BLUR_TITLE_MAX_CHARS;

	/* 长度与内容两次调用之间标题可能变化：取失败按不匹配（失败安全） */
	if (GetWindowTextW(hwnd, wtitle, BLUR_TITLE_MAX_CHARS + 1) <= 0) return 0;

	/* CP_UTF8 转换；返回值含结尾 0，<=1 表示空标题或转换失败 */
	len = WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, utf8, sizeof(utf8), NULL, NULL);
	if (len <= 1) return 0;

	kws = blur_get_keywords(&kwcount);
	for (k = 0; k < kwcount; k++) {
		if (should_blur_window(utf8, kws[k])) return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * EnumWindows 回调（Z 序：最顶层先回调）
 * ------------------------------------------------------------------ */

typedef struct WinBlurCtx {
	BlurRect *covered;		/* 已覆盖矩形集（更高层窗口累计） */
	int covered_count;
	int covered_cap;
	BlurRect *out;			/* 输出：matched 窗口可见区域 */
	int out_count;
	int out_cap;
	int screen_w;			/* 捕获区尺寸 = SCREEN_WIDTH/HEIGHT */
	int screen_h;
} WinBlurCtx;

static BOOL CALLBACK win_blur_enum_proc(HWND hwnd, LPARAM lparam)
{
	WinBlurCtx *ctx = (WinBlurCtx *)lparam;
	RECT rc;
	BlurRect r;
	DWORD cloaked = 0;
	WCHAR cls[64];
	int i, v;

	/* 过滤（design §1）：不可见 / 最小化 / 无标题 —— 这些窗既不匹配也不覆盖 */
	if (!IsWindowVisible(hwnd)) return TRUE;
	if (IsIconic(hwnd)) return TRUE;
	if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;

	/* 排除红点画笔 overlay：全屏像素透明浮层（WS_EX_LAYERED，仅笔画处不透明），
	 * 若计入覆盖集会把整个屏幕当作遮挡物，matched 窗口可见区域被清零 →
	 * 画笔会话期间遮罩静默失效（泄漏方向，不可接受）。类名与 tile.cpp 采集侧
	 * 特判一致（isPaintbrushActive 同款字面量）。 */
	if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, WIN_BLUR_PAINTBRUSH_CLASS) == 0) return TRUE;

	/* DWM cloaked 窗整体跳过（文件头注释：泄漏方向防护） */
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
		cloaked != 0) return TRUE;

	if (!GetWindowRect(hwnd, &rc)) return TRUE;

	/* 屏幕坐标 → 捕获区相对坐标（平移 -SCREEN_X/-SCREEN_Y）+ 裁剪屏内（R4） */
	r.x = (int)rc.left - SCREEN_X;
	r.y = (int)rc.top - SCREEN_Y;
	r.w = (int)(rc.right - rc.left);
	r.h = (int)(rc.bottom - rc.top);
	win_blur_clip_rect(&r, ctx->screen_w, ctx->screen_h);
	if (r.w <= 0 || r.h <= 0) return TRUE;

	/* 小窗不匹配不覆盖（沿 linux 阈值，防碎窗污染） */
	if (r.w < BLUR_MIN_MATCH_SIZE || r.h < BLUR_MIN_MATCH_SIZE) return TRUE;

	if (win_blur_title_matches(hwnd)) {
		BlurRect visible[MAX_VISIBLE_RECTS_PER_WINDOW];
		BlurRect new_visible[MAX_VISIBLE_RECTS_PER_WINDOW];
		int vis_count = 1;
		int new_count, pc, s;
		int overflow = 0;

		visible[0] = r;

		/* 减去更高层窗口（已进覆盖集者）的覆盖，拆分上限防碎片爆炸 */
		for (i = 0; i < ctx->covered_count && vis_count > 0 && !overflow; i++) {
			new_count = 0;
			for (v = 0; v < vis_count && !overflow; v++) {
				if (!win_blur_rect_intersects(visible[v], ctx->covered[i])) {
					/* 不相交原样保留：1:1 追加，new_count <= vis_count <= 上限，无需判满 */
					new_visible[new_count++] = visible[v];
				} else {
					BlurRect pieces[4];
					pc = win_blur_subtract_rect(visible[v], ctx->covered[i], pieces, 4);
					for (s = 0; s < pc; s++) {
						if (new_count < MAX_VISIBLE_RECTS_PER_WINDOW) {
							new_visible[new_count++] = pieces[s];
						} else { overflow = 1; break; }
					}
				}
			}
			if (!overflow) {
				vis_count = new_count;
				for (v = 0; v < vis_count; v++) visible[v] = new_visible[v];
			}
		}

		if (!overflow && ctx->out_count + vis_count <= ctx->out_cap) {
			for (v = 0; v < vis_count; v++) {
				if (visible[v].w > BLUR_MIN_PIECE_SIZE && visible[v].h > BLUR_MIN_PIECE_SIZE) {
					ctx->out[ctx->out_count++] = visible[v];
				}
			}
			BLUR_LOG("[BLUR] matched hwnd=%p rect=(%d,%d %dx%d) pieces=%d\n",
				(void *)hwnd, r.x, r.y, r.w, r.h, vis_count);
		} else {
			/* 拆分溢出或输出将满：退化整窗覆盖（多遮 = 泄漏安全方向，design 取舍） */
			if (ctx->out_count < ctx->out_cap) ctx->out[ctx->out_count++] = r;
			BLUR_LOG("[BLUR] matched hwnd=%p rect=(%d,%d %dx%d) overflow=%d fallback whole\n",
				(void *)hwnd, r.x, r.y, r.w, r.h, overflow);
		}
	}

	/* 无论如何累计覆盖集（该窗位于后续枚举窗之上） */
	if (ctx->covered_count < ctx->covered_cap) {
		ctx->covered[ctx->covered_count++] = r;
	} else {
		BLUR_LOG("[BLUR] covered set full (%d), hwnd=%p skipped from coverage\n",
			ctx->covered_cap, (void *)hwnd);
	}

	return TRUE;
}

/* ------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------ */

int get_blurred_regions(BlurRect **regions)
{
	WinBlurCtx ctx;
	BlurRect *covered = NULL;
	BlurRect *out = NULL;

	*regions = NULL;

	/* 失败安全：屏幕指标非法直接返回 0（画面原样） */
	if (SCREEN_WIDTH <= 0 || SCREEN_HEIGHT <= 0) return 0;

	BLUR_LOG_OPEN();
	BLUR_LOG("[BLUR] get_blurred_regions called, offset=(%d,%d) size=%dx%d\n",
		SCREEN_X, SCREEN_Y, SCREEN_WIDTH, SCREEN_HEIGHT);

	covered = (BlurRect *)malloc(BLUR_MAX_COVERED_RECTS * sizeof(BlurRect));
	out = (BlurRect *)malloc(BLUR_MAX_OUTPUT_RECTS * sizeof(BlurRect));
	if (!covered || !out) {
		free(covered);
		free(out);
		BLUR_LOG("[BLUR] malloc failed, returning 0 regions (gle=%lu)\n", (unsigned long)GetLastError());
		return 0;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.covered = covered;
	ctx.covered_cap = BLUR_MAX_COVERED_RECTS;
	ctx.out = out;
	ctx.out_cap = BLUR_MAX_OUTPUT_RECTS;
	ctx.screen_w = SCREEN_WIDTH;
	ctx.screen_h = SCREEN_HEIGHT;

	/* Z 序枚举顶层窗口。服务进程无自窗，无需排除自身（design §1）；
	 * 返回值不判定：失败时已收集的区域仍然有效（保守多遮，安全方向） */
	EnumWindows(win_blur_enum_proc, (LPARAM)&ctx);

	BLUR_LOG("[BLUR] Final: %d blur regions (covered=%d)\n", ctx.out_count, ctx.covered_count);

	if (ctx.out_count > 0) {
		*regions = out;
		free(covered);
		return ctx.out_count;
	}

	free(out);
	free(covered);
	return 0;
}

void free_blurred_regions(BlurRect *regions)
{
	if (regions) free(regions);
}

/* ------------------------------------------------------------------
 * Windows 版灰色填充（tile.cpp 采集后调用）
 *
 * 与 linux_tile.c apply_blur_to_regions 的差异（不可照抄的原因）：
 *   1. 像素格式动态：PIXEL_SIZE 3/4 字节按实际值分支（GDI 屏幕位图 24/32bpp）
 *   2. 行序 bottom-up：GetDIBits 正 biHeight 产物第 0 行 = 屏幕底行，
 *      屏幕行 y 须翻转到缓冲行 bufh-1-y；topdown（负 biHeight）不翻转
 *   3. 内存字节序 BGR：灰块三通道同值 RGB(128,128,128)，B/G/R 顺序无视觉
 *      差异，4 字节格式时第 4 字节（alpha/保留）保持不动
 *   4. SCALING_FACTOR != 1024 时缓冲区是缩放采集（StretchBlt），坐标先缩放
 *   5. 行距假设 = bufw*PIXEL_SIZE，无 24bpp 行尾 DWORD 对齐 padding——与
 *      tile.cpp 采集侧缓冲分配（get_desktop_buffer）及 tile 读取（get_tile_buffer/
 *      tile_crc）同款管线不变式；兼容位图（CreateCompatibleBitmap）实际格式
 *      32bpp 下精确成立。24bpp 情形全采集管线（含既有 tile 路径）本就未按
 *      padding 处理，本函数按同款不变式走而非单独对齐
 * ------------------------------------------------------------------ */
void apply_blur_to_regions_win(unsigned char *buf, long long bufsize,
	int bufw, int bufh, int topdown,
	const BlurRect *regions, int count)
{
	int i, x, y, row;
	int scaled;

	if (!buf || !regions || count <= 0) return;
	if (bufw <= 0 || bufh <= 0 || bufsize <= 0) return;

	/* 仅支持 3/4 字节像素。其他位深（16bpp 等理论值）写 3 字节灰会错位伤及
	 * 邻像素，跳过遮罩；实际路径（CreateCompatibleBitmap 屏幕位图）必为 24/32bpp，
	 * 该分支仅为防御（画面原样的失败安全语义）。 */
	if (PIXEL_SIZE != 3 && PIXEL_SIZE != 4) return;

	scaled = (SCALING_FACTOR != 1024 && SCALING_FACTOR > 0);

	for (i = 0; i < count; i++) {
		int rx = regions[i].x, ry = regions[i].y;
		int rw = regions[i].w, rh = regions[i].h;
		int bx, by, bw, bh;
		unsigned char *p;
		long long off;

		if (rw <= 0 || rh <= 0) continue;

		if (scaled) {
			/* 屏幕→缓冲坐标缩放（SCALED = SCREEN * SCALING_FACTOR/1024，kvm.c）。
			 * 右/下边缘外扩 2 像素：整数缩放截断会在矩形边缘留漏缝（少遮方向，避免） */
			int x2 = ((rx + rw) * SCALING_FACTOR) / 1024;
			int y2 = ((ry + rh) * SCALING_FACTOR) / 1024;
			bx = (rx * SCALING_FACTOR) / 1024;
			by = (ry * SCALING_FACTOR) / 1024;
			bw = x2 - bx + 2;
			bh = y2 - by + 2;
		} else {
			bx = rx; by = ry; bw = rw; bh = rh;
		}

		/* 钳制到缓冲区范围（regions 本应已裁剪，此处兜底，负宽高丢弃） */
		if (bx < 0) { bw += bx; bx = 0; }
		if (by < 0) { bh += by; by = 0; }
		if (bx + bw > bufw) bw = bufw - bx;
		if (by + bh > bufh) bh = bufh - by;
		if (bw <= 0 || bh <= 0) continue;

		for (y = by; y < by + bh; y++) {
			/* bottom-up（topdown=0）：屏幕行 y → 缓冲行 bufh-1-y */
			row = topdown ? y : (bufh - 1 - y);
			if (row < 0 || row >= bufh) continue;	/* 钳制防御，理论不可达 */

			off = ((long long)row * bufw + bx) * PIXEL_SIZE;
			if (off < 0 || off + (long long)bw * PIXEL_SIZE > bufsize) continue;	/* 越界防御 */

			p = buf + off;
			for (x = 0; x < bw; x++) {
				p[0] = 128;		/* B */
				p[1] = 128;		/* G */
				p[2] = 128;		/* R —— 三通道同值 = RGB(128,128,128)，与 mac/linux 一致 */
				/* PIXEL_SIZE==4 时 p[3]（alpha/保留）保持原值不动 */
				p += PIXEL_SIZE;
			}
		}
	}
}

#endif	/* _LINKVM */
