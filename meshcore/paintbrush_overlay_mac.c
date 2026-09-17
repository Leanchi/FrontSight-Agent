/*
Copyright 2026 红点画笔 C 原生模块 (macOS 版本)
@macOS 端 paintbrush overlay 功能 —— objc runtime 纯 C 创建 NSWindow + CoreGraphics 绘制

与 Linux 版（paintbrush_overlay.c）/ Windows 版（paintbrush_overlay_win.cpp）
提供完全相同的 Duktape API（导出同名 paintbrush_overlay_PUSH）。
通过 Duktape 注册为 "paintbrush-overlay" 模块，JS 端通过 require() 调用

实现要点：
- objc_msgSend 纯 C 桥接（风格对齐 mac_kvm.c 的 CoreGraphics C API 用法），无 ObjC 源文件，
  编译进 make 链路（x86_64 的 NSRect 返回走 objc_msgSend_stret，arm64 走普通 objc_msgSend）
- NSWindow：borderless + 透明背景 + setIgnoresMouseEvents:YES 点击穿透 +
  setLevel:NSScreenSaverWindowLevel(1000) 置顶 + CanJoinAllSpaces/FullScreenAuxiliary
  （全屏应用/多空间下仍覆盖目标屏）
- 绘制：CGBitmapContext backing store 延迟上屏（对齐 X11 版 backing pixmap + flush 语义），
  红 3px stroke、CapRound/JoinRound（对齐 Linux line_width=3 CapRound）
- 坐标空间：KVM 流像素（SCREEN_WIDTH = CGDisplayPixelsWide，mac_kvm.c kvm_init 同源），
  backing store 建在像素分辨率，窗口 frame 为点距（Retina 时 backing = frame × backingScaleFactor，
  恰与 KVM 流像素 1:1）
- KVM 可捕获性：mac_kvm 用 CGDisplayCreateImage（显示级捕获，含所有窗口层），
  普通 on-screen NSWindow 直接入画；置顶层(1000)≠0 还使其天然避开 blur_config 的
  layer==0 虚化匹配，红笔不会被 KVM 虚化功能抹掉
*/

#if defined(__APPLE__)

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "duktape.h"
#include "microscript/ILibDuktape_Helpers.h"
#include "paintbrush_overlay.h"

/* ============================================================
   常量（NSWindow/NSApplication AppKit 头文件值的手工镜像，
   纯 C 桥接不引入 AppKit 头文件；链接经 makefile -framework AppKit）
   ============================================================ */
#define PB_WINDOW_STYLE_MASK_BORDERLESS   0u       /* NSWindowStyleMaskBorderless */
#define PB_WINDOW_BACKING_BUFFERED        2u       /* NSBackingStoreBuffered */
#define PB_ACT_POLICY_ACCESSORY           1        /* NSApplicationActivationPolicyAccessory */
#define PB_LEVEL_SCREEN_SAVER             1000L    /* NSScreenSaverWindowLevel (kCGScreenSaverWindowLevel) */
#define PB_COLLECTION_ALL_SPACES          (1L << 0)  /* NSWindowCollectionBehaviorCanJoinAllSpaces */
#define PB_COLLECTION_FULLSCREEN_AUX      (1L << 8)  /* NSWindowCollectionBehaviorFullScreenAuxiliary */
#define PB_WINDOW_NAME                    "meshcentral-paintbrush"

/* ============================================================
   静态状态
   ============================================================ */
static id g_window = NULL;        /* NSWindow */
static id g_contentView = NULL;   /* NSWindow contentView（生命周期由 window 持有，仅引用） */
static CGContextRef g_backing = NULL;   /* backing store（KVM 流像素分辨率，top-left 翻转 CTM） */
static int g_width = 0;           /* 流像素宽（draw* 输入坐标系） */
static int g_height = 0;          /* 流像素高 */
static double g_frameW = 0.0;     /* 窗口 frame 宽（点） */
static double g_frameH = 0.0;     /* 窗口 frame 高（点） */

/* ============================================================
   objc_msgSend 桥接辅助
   注意：x86_64 返回 NSRect/CGRect（32 字节结构体）必须走 objc_msgSend_stret；
   arm64 无 stret 变体（结构体返回由普通 objc_msgSend 承载）
   ============================================================ */
static id pb_send_id(id self, SEL sel)
{
	return ((id(*)(id, SEL))objc_msgSend)(self, sel);
}

static id pb_send_id0(Class cls, SEL sel)
{
	return ((id(*)(id, SEL))objc_msgSend)((id)cls, sel);
}

static void pb_send_void(id self, SEL sel)
{
	((void(*)(id, SEL))objc_msgSend)(self, sel);
}

static void pb_send_void_bool(id self, SEL sel, BOOL value)
{
	((void(*)(id, SEL, BOOL))objc_msgSend)(self, sel, value);
}

static void pb_send_void_long(id self, SEL sel, long value)
{
	/* NSInteger == long（64 位 macOS 两架构一致） */
	((void(*)(id, SEL, long))objc_msgSend)(self, sel, value);
}

static void pb_send_void_id(id self, SEL sel, id value)
{
	((void(*)(id, SEL, id))objc_msgSend)(self, sel, value);
}

static CGRect pb_send_rect(id self, SEL sel)
{
#if defined(__x86_64__)
	/* AppKit 未包含头文件，NSRect 即 CGRect（64 位下 CGFloat==double，32 字节布局一致） */
	CGRect r;
	((void(*)(CGRect *, id, SEL))objc_msgSend_stret)(&r, self, sel);
	return r;
#else
	return ((CGRect(*)(id, SEL))objc_msgSend)(self, sel);
#endif
}

static double pb_send_double(id self, SEL sel)
{
	/* CGFloat == double（64 位 macOS） */
	return ((double(*)(id, SEL))objc_msgSend)(self, sel);
}

/* 将坐标限制在屏幕范围内，防止极端值（对齐 Linux 版 CLAMP 宏语义） */
#define CLAMP_COORD(val, max) ((val) < 0 ? 0 : ((val) > (max) ? (max) : (val)))

/* ============================================================
   NSAutoreleasePool push/pop（API 入口包裹，收集桥接过程产生的临时对象）
   ============================================================ */
static id pb_pool_new(void)
{
	id pool = pb_send_id0(objc_getClass("NSAutoreleasePool"), sel_registerName("alloc"));
	return pb_send_id(pool, sel_registerName("init"));
}

static void pb_pool_free(id pool)
{
	pb_send_void(pool, sel_registerName("drain"));
}

/* 从 JS 点对象 {x, y} 读坐标并钳制到屏幕范围（调用约定：栈顶为点对象） */
static void pb_read_point(duk_context *ctx, int *x, int *y)
{
	duk_get_prop_string(ctx, -1, "x");
	*x = CLAMP_COORD(duk_get_int(ctx, -1), g_width);
	duk_pop(ctx);
	duk_get_prop_string(ctx, -1, "y");
	*y = CLAMP_COORD(duk_get_int(ctx, -1), g_height);
	duk_pop(ctx);
	duk_pop(ctx);  /* 弹出点对象 */
}

/* ============================================================
   将 backing store 推上屏幕（flush 语义）
   lockFocus 取 view 绘图上下文 → 画 backing 镜像 → CGContextFlush
   直接推送 WindowServer（agent 无 AppKit RunLoop，靠 flush 直推）
   ============================================================ */
static void pb_push_to_screen(void)
{
	id pool;
	CGImageRef image;
	id ngc;
	CGContextRef cg;
	CGRect ptRect;

	if (!g_window || !g_backing || !g_contentView) return;

	/* 本进程（-b64exec 子进程）没有外层 autorelease pool，
	 * lockFocus 会产生自动释放的临时对象（如 NSGraphicsContext），
	 * 每次 flush 自建 pool 收集，防止长时间绘图会话累积泄漏 */
	pool = pb_pool_new();

	image = CGBitmapContextCreateImage(g_backing);
	if (!image) { pb_pool_free(pool); return; }

	pb_send_void(g_contentView, sel_registerName("lockFocus"));
	ngc = pb_send_id0(objc_getClass("NSGraphicsContext"), sel_registerName("currentContext"));
	if (ngc)
	{
		cg = ((CGContextRef(*)(id, SEL))objc_msgSend)(ngc, sel_registerName("CGContext"));
		if (cg)
		{
			/* view 上下文为标准 y-up 点距坐标系；CGImage 绘制时天然正立 */
			ptRect = CGRectMake(0, 0, (CGFloat)g_frameW, (CGFloat)g_frameH);
			CGContextClearRect(cg, ptRect);
			CGContextDrawImage(cg, ptRect, image);
			CGContextFlush(cg);
		}
	}
	pb_send_void(g_contentView, sel_registerName("unlockFocus"));

	CGImageRelease(image);

	pb_pool_free(pool);
}

/* ============================================================
   initOverlay(displayName, xauthPath)
   创建 borderless 透明全屏 overlay 窗口（参数在 macOS 上被忽略，
   仅为与 Linux 版调用形参兼容）
   JS: require('paintbrush-overlay').initOverlay()
   返回: null（失败）或 { width, height }（流像素，与 KVM 分辨率同源）
   ============================================================ */
duk_ret_t paintbrush_initOverlay(duk_context *ctx)
{
	id pool = NULL;
	id app = NULL;
	id screen = NULL;
	CGRect frame;
	double scaleFactor = 1.0;
	CGColorSpaceRef colorSpace = NULL;
	id win = NULL;
	CFStringRef title = NULL;

	(void)ctx; /* 参数（displayName/xauthPath）在 macOS 上无意义，忽略 */

	/* 已初始化则返回当前尺寸 */
	if (g_window != NULL && g_backing != NULL)
	{
		duk_push_object(ctx);
		duk_push_int(ctx, g_width);
		duk_put_prop_string(ctx, -2, "width");
		duk_push_int(ctx, g_height);
		duk_put_prop_string(ctx, -2, "height");
		return 1;
	}

	pool = pb_pool_new();

	/* NSApplication 单例 + Accessory 策略：无 Dock 图标的 agent 也可上屏，
	 * 且窗口保持普通 on-screen window（可被 CGWindowList/显示捕获收录） */
	app = pb_send_id0(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
	if (app == NULL) { pb_pool_free(pool); duk_push_null(ctx); return 1; }
	((BOOL(*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"), PB_ACT_POLICY_ACCESSORY);

	/* 目标屏 frame（点距）与缩放系数 */
	screen = pb_send_id0(objc_getClass("NSScreen"), sel_registerName("mainScreen"));
	if (screen == NULL) { pb_pool_free(pool); duk_push_null(ctx); return 1; }
	frame = pb_send_rect(screen, sel_registerName("frame"));
	scaleFactor = pb_send_double(screen, sel_registerName("backingScaleFactor"));
	if (scaleFactor < 1.0) scaleFactor = 1.0;
	if (frame.size.width <= 0 || frame.size.height <= 0) { pb_pool_free(pool); duk_push_null(ctx); return 1; }

	/* 流像素分辨率（KVM kvm_send_resolution 报送的同源口径：CGDisplayPixelsWide/High） */
	g_width = (int)CGDisplayPixelsWide(CGMainDisplayID());
	g_height = (int)CGDisplayPixelsHigh(CGMainDisplayID());
	if (g_width <= 0 || g_height <= 0)
	{
		g_width = (int)(frame.size.width * scaleFactor);
		g_height = (int)(frame.size.height * scaleFactor);
	}
	g_frameW = frame.size.width;
	g_frameH = frame.size.height;

	/* backing store：流像素分辨率 BGRA premultiplied，CTM 翻转为 top-left（JS 坐标系） */
	colorSpace = CGColorSpaceCreateDeviceRGB();
	g_backing = CGBitmapContextCreate(NULL, (size_t)g_width, (size_t)g_height, 8, (size_t)g_width * 4,
		colorSpace, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(colorSpace);
	if (g_backing == NULL) { pb_pool_free(pool); duk_push_null(ctx); return 1; }
	CGContextTranslateCTM(g_backing, 0, (CGFloat)g_height);
	CGContextScaleCTM(g_backing, 1.0, -1.0);

	/* 红笔状态：ARGB 红、线宽 3、圆头圆角（对齐 Linux gcValues.line_width=3 CapRound） */
	CGContextSetRGBStrokeColor(g_backing, 1.0, 0.0, 0.0, 1.0);
	CGContextSetRGBFillColor(g_backing, 1.0, 0.0, 0.0, 1.0);
	CGContextSetLineWidth(g_backing, 3.0);
	CGContextSetLineCap(g_backing, kCGLineCapRound);
	CGContextSetLineJoin(g_backing, kCGLineJoinRound);

	/* 创建 borderless 透明窗口（styleMask=Borderless, backing=Buffered, defer=NO） */
	win = ((id(*)(id, SEL, CGRect, unsigned long, unsigned long, BOOL))objc_msgSend)(
		pb_send_id0(objc_getClass("NSWindow"), sel_registerName("alloc")),
		sel_registerName("initWithContentRect:styleMask:backing:defer:"),
		frame,
		(unsigned long)PB_WINDOW_STYLE_MASK_BORDERLESS,
		(unsigned long)PB_WINDOW_BACKING_BUFFERED,
		NO);
	if (win == NULL)
	{
		CGContextRelease(g_backing); g_backing = NULL;
		pb_pool_free(pool); duk_push_null(ctx); return 1;
	}
	pb_send_void_bool(win, sel_registerName("setIgnoresMouseEvents:"), YES);  /* 点击穿透 */
	pb_send_void_bool(win, sel_registerName("setOpaque:"), NO);
	pb_send_void_bool(win, sel_registerName("setHasShadow:"), NO);
	pb_send_void_bool(win, sel_registerName("setReleasedWhenClosed:"), NO);   /* 生命周期由本模块管理 */
	pb_send_void_id(win, sel_registerName("setBackgroundColor:"),
		pb_send_id0(objc_getClass("NSColor"), sel_registerName("clearColor")));
	pb_send_void_long(win, sel_registerName("setLevel:"), PB_LEVEL_SCREEN_SAVER);
	pb_send_void_long(win, sel_registerName("setCollectionBehavior:"),
		PB_COLLECTION_ALL_SPACES | PB_COLLECTION_FULLSCREEN_AUX);

	/* 窗口名（对齐 Linux XStoreName / Windows 类名；也便于调试枚举识别） */
	title = CFStringCreateWithCString(NULL, PB_WINDOW_NAME, kCFStringEncodingUTF8);
	if (title != NULL)
	{
		pb_send_void_id(win, sel_registerName("setTitle:"), (id)title);
		CFRelease(title);
	}

	g_window = win;
	g_contentView = pb_send_id(win, sel_registerName("contentView"));

	/* 上屏：orderFrontRegardless 不抢焦点（配合 ignoresMouseEvents 保持无感覆盖） */
	pb_send_void(win, sel_registerName("orderFrontRegardless"));

	/* 初始内容全透明 */
	pb_push_to_screen();

	pb_pool_free(pool);

	/* 返回尺寸信息（流像素） */
	duk_push_object(ctx);
	duk_push_int(ctx, g_width);
	duk_put_prop_string(ctx, -2, "width");
	duk_push_int(ctx, g_height);
	duk_put_prop_string(ctx, -2, "height");
	return 1;
}

/* ============================================================
   drawLine(x1, y1, x2, y2)
   JS: overlay.drawLine(100, 200, 300, 400)
   ============================================================ */
duk_ret_t paintbrush_drawLine(duk_context *ctx)
{
	int x1, y1, x2, y2;

	if (!g_backing) return 0;

	x1 = CLAMP_COORD(duk_require_int(ctx, 0), g_width);
	y1 = CLAMP_COORD(duk_require_int(ctx, 1), g_height);
	x2 = CLAMP_COORD(duk_require_int(ctx, 2), g_width);
	y2 = CLAMP_COORD(duk_require_int(ctx, 3), g_height);

	CGContextMoveToPoint(g_backing, (CGFloat)x1, (CGFloat)y1);
	CGContextAddLineToPoint(g_backing, (CGFloat)x2, (CGFloat)y2);
	CGContextStrokePath(g_backing);
	return 0;
}

/* ============================================================
   drawDot(x, y, radius)
   JS: overlay.drawDot(200, 300, 3)
   ============================================================ */
duk_ret_t paintbrush_drawDot(duk_context *ctx)
{
	int x, y, radius;

	if (!g_backing) return 0;

	x = CLAMP_COORD(duk_require_int(ctx, 0), g_width);
	y = CLAMP_COORD(duk_require_int(ctx, 1), g_height);
	radius = duk_require_int(ctx, 2);
	if (radius < 1) radius = 1;
	if (radius > 50) radius = 50;

	CGContextFillEllipseInRect(g_backing, CGRectMake((CGFloat)(x - radius), (CGFloat)(y - radius),
		(CGFloat)(radius * 2), (CGFloat)(radius * 2)));
	return 0;
}

/* 画实心端点圆（对齐 Linux 版 FillArc r=3：直径 6） */
static void pb_fill_dot(int x, int y, int radius)
{
	CGContextFillEllipseInRect(g_backing, CGRectMake((CGFloat)(x - radius), (CGFloat)(y - radius),
		(CGFloat)(radius * 2), (CGFloat)(radius * 2)));
}

/* ============================================================
   drawStroke(points)
   JS: overlay.drawStroke([{x:1,y:2},{x:3,y:4}])
   绘制一条完整笔画（多段线 + 端点圆）
   ============================================================ */
duk_ret_t paintbrush_drawStroke(duk_context *ctx)
{
	duk_size_t len, i;
	int firstX, firstY, lastX, lastY;

	if (!g_backing) return 0;
	if (!duk_is_array(ctx, 0)) return 0;

	len = duk_get_length(ctx, 0);
	if (len == 0) return 0;

	/* 单点 -> 画圆 */
	if (len == 1)
	{
		duk_get_prop_index(ctx, 0, 0);
		pb_read_point(ctx, &firstX, &firstY);
		pb_fill_dot(firstX, firstY, 3);
		return 0;
	}

	/* 多点 -> 连续路径一笔画（round cap/join 已在 init 时设置，
	 * 语义等价 Linux 版逐段 XDrawLine + JoinRound） */
	duk_get_prop_index(ctx, 0, 0);
	pb_read_point(ctx, &firstX, &firstY);

	CGContextMoveToPoint(g_backing, (CGFloat)firstX, (CGFloat)firstY);
	for (i = 1; i < len; i++)
	{
		int px, py;
		duk_get_prop_index(ctx, 0, (duk_uarridx_t)i);
		pb_read_point(ctx, &px, &py);
		CGContextAddLineToPoint(g_backing, (CGFloat)px, (CGFloat)py);
	}
	CGContextStrokePath(g_backing);

	/* 端点圆（对齐 Linux 版 first/last FillArc r=3） */
	duk_get_prop_index(ctx, 0, (duk_uarridx_t)(len - 1));
	pb_read_point(ctx, &lastX, &lastY);
	pb_fill_dot(firstX, firstY, 3);
	pb_fill_dot(lastX, lastY, 3);

	return 0;
}

/* ============================================================
   clearWindow()
   JS: overlay.clearWindow()
   ============================================================ */
duk_ret_t paintbrush_clearWindow(duk_context *ctx)
{
	(void)ctx;
	if (!g_backing) return 0;

	/* 清空 backing store（翻转 CTM 下全幅 rect 恰好映射全缓冲） */
	CGContextClearRect(g_backing, CGRectMake(0, 0, (CGFloat)g_width, (CGFloat)g_height));

	/* 直推屏幕（对齐 Windows 版 clearWindow 的 updateScreen 语义） */
	pb_push_to_screen();
	return 0;
}

/* ============================================================
   flush()
   JS: overlay.flush()
   ============================================================ */
duk_ret_t paintbrush_flush(duk_context *ctx)
{
	(void)ctx;
	if (!g_window) return 0;

	/* backing store → 窗口 → WindowServer（对齐 X11 版 XCopyArea + XFlush 语义） */
	pb_push_to_screen();
	return 0;
}

/* ============================================================
   destroy()
   JS: overlay.destroy()
   ============================================================ */
duk_ret_t paintbrush_destroy(duk_context *ctx)
{
	(void)ctx;

	if (g_window != NULL)
	{
		pb_send_void_id(g_window, sel_registerName("orderOut:"), NULL);
		pb_send_void(g_window, sel_registerName("close"));
		pb_send_void(g_window, sel_registerName("release"));  /* alloc 配对（releasedWhenClosed=NO） */
		g_window = NULL;
	}
	g_contentView = NULL;

	if (g_backing != NULL)
	{
		CGContextRelease(g_backing);
		g_backing = NULL;
	}
	return 0;
}

/* ============================================================
   模块注册：Duktape PUSH 函数
   当 JS 执行 require('paintbrush-overlay') 时调用
   与 Linux 版 paintbrush_overlay.c / Windows 版 paintbrush_overlay_win.cpp
   保持完全一致的 API
   ============================================================ */
void paintbrush_overlay_PUSH(duk_context *ctx, void *chain)
{
	(void)chain;

	duk_push_object(ctx);

	ILibDuktape_CreateInstanceMethod(ctx, "initOverlay", paintbrush_initOverlay, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "drawLine", paintbrush_drawLine, 4);
	ILibDuktape_CreateInstanceMethod(ctx, "drawDot", paintbrush_drawDot, 3);
	ILibDuktape_CreateInstanceMethod(ctx, "drawStroke", paintbrush_drawStroke, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "clearWindow", paintbrush_clearWindow, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "flush", paintbrush_flush, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "destroy", paintbrush_destroy, 0);
}

#endif  /* __APPLE__ */
