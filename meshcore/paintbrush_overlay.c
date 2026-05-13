/*
Copyright 2026 红点画笔 C 原生模块
@用于 paintbrush overlay 功能，绕过 -b64exec 子进程中 X11 dlsym 绑定缺失的问题

直接在 C 中创建 X11 ARGB 透明覆盖层窗口，使用编译时链接的 X11 函数
（不需要 dlsym 查找符号）

通过 Duktape 注册为 "paintbrush-overlay" 模块，JS 端通过 require() 调用
*/

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include "duktape.h"
#include "microscript/ILibDuktape_Helpers.h"

/* ============================================================
   静态状态
   ============================================================ */
static Display *overlayDisplay = NULL;
static Window overlayWindow = 0;
static GC overlayGC = 0;
static GC overlayClearGC = 0;
static Colormap overlayColormap = 0;
static int screenWidth = 0;
static int screenHeight = 0;

/* Backing pixmap：所有绘图先写到这个 pixmap，flush 时再复制到窗口，
 * 避免合成器在错误的 ShapeBounding context 下处理 damage 事件 */
static Pixmap overlayBackPixmap = 0;

/* ShapeBounding 位图：KWin 合成器尊重 ShapeBounding（而非 ShapeInput），
 * 用 depth=1 位图控制窗口在合成器场景图中的"存在范围"。
 * 位图 1=窗口存在（阻挡点击），0=窗口不存在（鼠标穿透） */
static Pixmap shapeBoundingBitmap = 0;
static GC shapeBoundingGC = 0;       /* foreground=1, line_width=7 */
static GC shapeBoundingClearGC = 0;  /* foreground=0 */

/* ============================================================
   initOverlay(displayName)
   创建 ARGB 透明全屏 overlay 窗口
   JS: require('paintbrush-overlay').initOverlay(":0")
   返回: null（失败）或 { width, height }
   ============================================================ */
duk_ret_t paintbrush_initOverlay(duk_context *ctx)
{
	const char *displayName = NULL;
	int screen;
	Window root;
	XVisualInfo vinfo_template;
	XVisualInfo *vinfo_list = NULL;
	int nitems;
	Visual *visual;
	int depth;
	Colormap colormap;
	XSetWindowAttributes attrs;
	XGCValues gcValues;

	/* 检查 -- already initialized */
	if (overlayDisplay != NULL)
	{
		duk_push_object(ctx);
		duk_push_int(ctx, screenWidth);
		duk_put_prop_string(ctx, -2, "width");
		duk_push_int(ctx, screenHeight);
		duk_put_prop_string(ctx, -2, "height");
		return 1;
	}

	/* 参数: displayName, xauthorityPath (optional) */
	if (duk_is_string(ctx, 0))
	{
		displayName = duk_get_string(ctx, 0);
	}
	if (duk_is_string(ctx, 1))
	{
		const char *xauth = duk_get_string(ctx, 1);
		if (xauth && xauth[0])
		{
			setenv("XAUTHORITY", xauth, 1);
		}
	}
	if (displayName && displayName[0] != ':')
	{
		setenv("DISPLAY", displayName, 1);
	}

	/* 打开 X11 Display */
	overlayDisplay = XOpenDisplay(displayName);
	if (!overlayDisplay)
	{
		duk_push_null(ctx);
		return 1;
	}

	screen = XDefaultScreen(overlayDisplay);
	screenWidth = XDisplayWidth(overlayDisplay, screen);
	screenHeight = XDisplayHeight(overlayDisplay, screen);
	root = XRootWindow(overlayDisplay, screen);

	/* 查找 ARGB visual (depth=32, TrueColor) */
	vinfo_template.screen = screen;
	vinfo_template.depth = 32;
	vinfo_template.class = TrueColor;
	vinfo_list = XGetVisualInfo(overlayDisplay,
		VisualScreenMask | VisualDepthMask | VisualClassMask,
		&vinfo_template, &nitems);

	if (vinfo_list != NULL && nitems > 0)
	{
		visual = vinfo_list[0].visual;
		depth = vinfo_list[0].depth;
		XFree(vinfo_list);
	}
	else
	{
		/* 回退：使用默认 visual */
		visual = DefaultVisual(overlayDisplay, screen);
		depth = DefaultDepth(overlayDisplay, screen);
	}

	/* 创建 Colormap */
	colormap = XCreateColormap(overlayDisplay, root, visual, AllocNone);
	overlayColormap = colormap;

	/* 创建窗口 */
	/* ARGB depth=32 不能用 ParentRelative（与 root 的 depth=24 不匹配，会 BadMatch）
	 * 用 None + alpha 通道实现透明 */
	attrs.background_pixmap = None;
	attrs.border_pixel = 0;
	attrs.colormap = colormap;
	attrs.override_redirect = True; /* 跳过窗口管理器装饰和定位 */

	overlayWindow = XCreateWindow(
		overlayDisplay, root,
		0, 0, screenWidth, screenHeight,
		0, depth, InputOutput, visual,
		CWBackPixmap | CWColormap | CWBorderPixel | CWOverrideRedirect,
		&attrs
	);


	/* 设置窗口名称 */
	XStoreName(overlayDisplay, overlayWindow, "meshcentral-paintbrush");

	/* 不设置 _NET_WM_WINDOW_TYPE 和 _NET_WM_STATE —— override_redirect=True 的窗口
	 * 不需要这些属性，且某些合成器（Deepin-kwin）会对 NOTIFICATION 类型窗口做特殊处理，
	 * 可能干扰 XShape 输入穿透 */

	/* 鼠标事件穿透：不接收任何输入事件 */
	XSelectInput(overlayDisplay, overlayWindow, 0);

	/* 使用 XShape 扩展让输入穿透（最可靠的方式）
	 * 注意：XShapeCombineMask(..., None, ShapeSet) 会移除客户端 input shape，
	 * 使区域恢复为默认的全窗口矩形（仍然阻挡点击）。
	 * 正确做法：XShapeCombineRectangles + 0 个矩形 = 真正空的 input region */
	{
		int shapeEventBase, shapeErrorBase;
		if (XShapeQueryExtension(overlayDisplay, &shapeEventBase, &shapeErrorBase))
		{
			XShapeCombineRectangles(overlayDisplay, overlayWindow, ShapeInput,
				0, 0, NULL, 0, ShapeSet, Unsorted);
		}
	}

	/* 创建 backing pixmap（depth=32，与 overlay 窗口同深度）
	 * 所有绘图先写到 backing pixmap，flush 时再复制到窗口，
	 * 避免合成器在错误的 ShapeBounding context 下处理 damage 事件 */
	overlayBackPixmap = XCreatePixmap(overlayDisplay, overlayWindow,
		screenWidth, screenHeight, depth);

	/* 创建 GC */
	gcValues.foreground = 0xFFFF0000; /* ARGB 红色 */
	gcValues.line_width = 3;
	gcValues.line_style = LineSolid;
	gcValues.cap_style = CapRound;
	gcValues.join_style = JoinRound;
	overlayGC = XCreateGC(overlayDisplay, overlayWindow,
		GCForeground | GCLineWidth | GCLineStyle | GCCapStyle | GCJoinStyle,
		&gcValues);

	/* 创建清除 GC（全透明填充） */
	{
		XGCValues clearGCValues;
		clearGCValues.foreground = 0x00000000; /* 全透明 ARGB */
		clearGCValues.function = GXcopy;
		overlayClearGC = XCreateGC(overlayDisplay, overlayWindow,
			GCForeground | GCFunction, &clearGCValues);
	}

	/* 初始化 backing pixmap 为全透明 */
	XFillRectangle(overlayDisplay, overlayBackPixmap, overlayClearGC,
		0, 0, screenWidth, screenHeight);

	/* 创建 ShapeBounding 位图（depth=1），用于控制窗口在合成器场景图中的存在范围 */
	{
		XGCValues shapeGCValues;
		XGCValues shapeClearGCValues;

		shapeBoundingBitmap = XCreatePixmap(overlayDisplay, overlayWindow,
			screenWidth, screenHeight, 1);
		/* 绘制GC: foreground=1(窗口存在), line_width=7(比overlay的3宽，确保覆盖) */
		shapeGCValues.foreground = 1;
		shapeGCValues.background = 0;
		shapeGCValues.line_width = 7;
		shapeGCValues.line_style = LineSolid;
		shapeGCValues.cap_style = CapRound;
		shapeGCValues.join_style = JoinRound;
		shapeBoundingGC = XCreateGC(overlayDisplay, shapeBoundingBitmap,
			GCForeground | GCBackground | GCLineWidth | GCLineStyle | GCCapStyle | GCJoinStyle,
			&shapeGCValues);
		/* 清除GC: foreground=0(窗口不存在) */
		shapeClearGCValues.foreground = 0;
		shapeClearGCValues.function = GXcopy;
		shapeBoundingClearGC = XCreateGC(overlayDisplay, shapeBoundingBitmap,
			GCForeground | GCFunction, &shapeClearGCValues);
		/* 初始化为空 bounding shape（全零位图 = 合成器认为没有窗口 = 鼠标穿透） */
		XFillRectangle(overlayDisplay, shapeBoundingBitmap, shapeBoundingClearGC,
			0, 0, screenWidth, screenHeight);
		XShapeCombineMask(overlayDisplay, overlayWindow, ShapeBounding,
			0, 0, shapeBoundingBitmap, ShapeSet);
	}

	/* 显示窗口 */
	XMapWindow(overlayDisplay, overlayWindow);
	XFlush(overlayDisplay);

	/* MapWindow 后再次设置 ShapeBounding + ShapeInput，防止合成器在映射时重置 */
	{
		int shapeEventBase2, shapeErrorBase2;
		if (XShapeQueryExtension(overlayDisplay, &shapeEventBase2, &shapeErrorBase2))
		{
			XShapeCombineMask(overlayDisplay, overlayWindow, ShapeBounding,
				0, 0, shapeBoundingBitmap, ShapeSet);
			XShapeCombineRectangles(overlayDisplay, overlayWindow, ShapeInput,
				0, 0, NULL, 0, ShapeSet, Unsorted);
		}
	}


	/* 返回尺寸信息 */
	duk_push_object(ctx);
	duk_push_int(ctx, screenWidth);
	duk_put_prop_string(ctx, -2, "width");
	duk_push_int(ctx, screenHeight);
	duk_put_prop_string(ctx, -2, "height");
	return 1;
}

/* ============================================================
   drawLine(x1, y1, x2, y2)
   JS: overlay.drawLine(100, 200, 300, 400)
   ============================================================ */
/* 将坐标限制在屏幕范围内，防止极端值 */
#define CLAMP(val, max) ((val) < 0 ? 0 : ((val) > (max) ? (max) : (val)))

duk_ret_t paintbrush_drawLine(duk_context *ctx)
{
	int x1, y1, x2, y2;

	if (!overlayDisplay || !overlayGC) return 0;

	x1 = CLAMP(duk_require_int(ctx, 0), screenWidth);
	y1 = CLAMP(duk_require_int(ctx, 1), screenHeight);
	x2 = CLAMP(duk_require_int(ctx, 2), screenWidth);
	y2 = CLAMP(duk_require_int(ctx, 3), screenHeight);

	XDrawLine(overlayDisplay, overlayBackPixmap, overlayGC, x1, y1, x2, y2);
	if (shapeBoundingBitmap && shapeBoundingGC)
		XDrawLine(overlayDisplay, shapeBoundingBitmap, shapeBoundingGC, x1, y1, x2, y2);
	return 0;
}

/* ============================================================
   drawDot(x, y, radius)
   JS: overlay.drawDot(200, 300, 3)
   ============================================================ */
duk_ret_t paintbrush_drawDot(duk_context *ctx)
{
	int x, y, radius;

	if (!overlayDisplay || !overlayGC) return 0;

	x = CLAMP(duk_require_int(ctx, 0), screenWidth);
	y = CLAMP(duk_require_int(ctx, 1), screenHeight);
	radius = duk_require_int(ctx, 2);
	if (radius < 1) radius = 1;
	if (radius > 50) radius = 50;

	XFillArc(overlayDisplay, overlayBackPixmap, overlayGC,
		x - radius, y - radius,
		radius * 2, radius * 2,
		0, 360 * 64);
	if (shapeBoundingBitmap && shapeBoundingGC)
		XFillArc(overlayDisplay, shapeBoundingBitmap, shapeBoundingGC,
			x - radius, y - radius,
			radius * 2, radius * 2,
			0, 360 * 64);
	return 0;
}

/* ============================================================
   drawStroke(points)
   JS: overlay.drawStroke([{x:1,y:2},{x:3,y:4}])
   绘制一条完整笔画（多段线+端点）
   ============================================================ */
duk_ret_t paintbrush_drawStroke(duk_context *ctx)
{
	int i, len, x1, y1, x2, y2, firstX, firstY, lastX, lastY;

	if (!overlayDisplay || !overlayGC) return 0;
	if (!duk_is_array(ctx, 0)) return 0;

	len = duk_get_length(ctx, 0);
	if (len == 0) return 0;

	/* 单点 -> 画圆 */
	if (len == 1)
	{
		duk_get_prop_index(ctx, 0, 0);
		duk_get_prop_string(ctx, -1, "x");
		x1 = CLAMP(duk_get_int(ctx, -1), screenWidth);
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, "y");
		y1 = CLAMP(duk_get_int(ctx, -1), screenHeight);
		duk_pop_2(ctx);

		XFillArc(overlayDisplay, overlayBackPixmap, overlayGC,
			x1 - 3, y1 - 3, 6, 6, 0, 360 * 64);
		if (shapeBoundingBitmap && shapeBoundingGC)
			XFillArc(overlayDisplay, shapeBoundingBitmap, shapeBoundingGC,
				x1 - 5, y1 - 5, 10, 10, 0, 360 * 64);
		return 0;
	}

	/* 多点 -> 画线段 (缓存前一点避免重复读取) */
	duk_get_prop_index(ctx, 0, 0);
	duk_get_prop_string(ctx, -1, "x");
	firstX = CLAMP(duk_get_int(ctx, -1), screenWidth);
	duk_pop(ctx);
	duk_get_prop_string(ctx, -1, "y");
	firstY = CLAMP(duk_get_int(ctx, -1), screenHeight);
	duk_pop_2(ctx);

	x1 = firstX;
	y1 = firstY;
	for (i = 1; i < len; i++)
	{
		duk_get_prop_index(ctx, 0, i);
		duk_get_prop_string(ctx, -1, "x");
		x2 = CLAMP(duk_get_int(ctx, -1), screenWidth);
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, "y");
		y2 = CLAMP(duk_get_int(ctx, -1), screenHeight);
		duk_pop_2(ctx);

		XDrawLine(overlayDisplay, overlayBackPixmap, overlayGC, x1, y1, x2, y2);
		if (shapeBoundingBitmap && shapeBoundingGC)
			XDrawLine(overlayDisplay, shapeBoundingBitmap, shapeBoundingGC, x1, y1, x2, y2);
		x1 = x2;
		y1 = y2;
	}

	duk_get_prop_index(ctx, 0, len - 1);
	duk_get_prop_string(ctx, -1, "x");
	lastX = CLAMP(duk_get_int(ctx, -1), screenWidth);
	duk_pop(ctx);
	duk_get_prop_string(ctx, -1, "y");
	lastY = CLAMP(duk_get_int(ctx, -1), screenHeight);
	duk_pop_2(ctx);

	/* 端点圆 */
	XFillArc(overlayDisplay, overlayBackPixmap, overlayGC,
		firstX - 3, firstY - 3, 6, 6, 0, 360 * 64);
	XFillArc(overlayDisplay, overlayBackPixmap, overlayGC,
		lastX - 3, lastY - 3, 6, 6, 0, 360 * 64);
	if (shapeBoundingBitmap && shapeBoundingGC)
	{
		XFillArc(overlayDisplay, shapeBoundingBitmap, shapeBoundingGC,
			firstX - 5, firstY - 5, 10, 10, 0, 360 * 64);
		XFillArc(overlayDisplay, shapeBoundingBitmap, shapeBoundingGC,
			lastX - 5, lastY - 5, 10, 10, 0, 360 * 64);
	}

	return 0;
}

/* ============================================================
   clearWindow()
   JS: overlay.clearWindow()
   ============================================================ */
duk_ret_t paintbrush_clearWindow(duk_context *ctx)
{
	if (!overlayDisplay || !overlayWindow) return 0;
	if (!overlayClearGC) return 0;
	/* 清除 backing pixmap */
	if (overlayBackPixmap)
		XFillRectangle(overlayDisplay, overlayBackPixmap, overlayClearGC,
			0, 0, screenWidth, screenHeight);
	/* 清除 overlay 窗口 */
	XFillRectangle(overlayDisplay, overlayWindow, overlayClearGC,
		0, 0, screenWidth, screenHeight);
	/* 同时清除 ShapeBounding 位图 */
	if (shapeBoundingBitmap && shapeBoundingClearGC) {
		XFillRectangle(overlayDisplay, shapeBoundingBitmap, shapeBoundingClearGC,
			0, 0, screenWidth, screenHeight);
		XShapeCombineMask(overlayDisplay, overlayWindow, ShapeBounding,
			0, 0, shapeBoundingBitmap, ShapeSet);
	}
	return 0;
}

/* ============================================================
   flush()
   JS: overlay.flush()
   ============================================================ */
duk_ret_t paintbrush_flush(duk_context *ctx)
{
	if (!overlayDisplay) return 0;
	/* 1. 先更新 ShapeBounding（让合成器知道窗口存在范围） */
	if (shapeBoundingBitmap)
		XShapeCombineMask(overlayDisplay, overlayWindow, ShapeBounding,
			0, 0, shapeBoundingBitmap, ShapeSet);
	/* 2. 从 backing pixmap 复制到 overlay 窗口
	 *    此时 bounding shape 已设置，damage 会在正确的 bounding context 下被合成器处理 */
	if (overlayBackPixmap)
		XCopyArea(overlayDisplay, overlayBackPixmap, overlayWindow, overlayGC,
			0, 0, screenWidth, screenHeight, 0, 0);
	/* 3. 刷新 */
	XFlush(overlayDisplay);
	return 0;
}

/* ============================================================
   destroy()
   JS: overlay.destroy()
   ============================================================ */
duk_ret_t paintbrush_destroy(duk_context *ctx)
{
	if (overlayDisplay != NULL)
	{
		if (shapeBoundingGC) { XFreeGC(overlayDisplay, shapeBoundingGC); shapeBoundingGC = 0; }
		if (shapeBoundingClearGC) { XFreeGC(overlayDisplay, shapeBoundingClearGC); shapeBoundingClearGC = 0; }
		if (shapeBoundingBitmap) { XFreePixmap(overlayDisplay, shapeBoundingBitmap); shapeBoundingBitmap = 0; }
			if (overlayBackPixmap) { XFreePixmap(overlayDisplay, overlayBackPixmap); overlayBackPixmap = 0; }
		if (overlayGC) { XFreeGC(overlayDisplay, overlayGC); overlayGC = 0; }
		if (overlayClearGC) { XFreeGC(overlayDisplay, overlayClearGC); overlayClearGC = 0; }
		if (overlayColormap) { XFreeColormap(overlayDisplay, overlayColormap); overlayColormap = 0; }
		if (overlayWindow) { XDestroyWindow(overlayDisplay, overlayWindow); overlayWindow = 0; }
		XCloseDisplay(overlayDisplay);
		overlayDisplay = NULL;
	}
	return 0;
}

/* ============================================================
   模块注册：Duktape PUSH 函数
   当 JS 执行 require('paintbrush-overlay') 时调用
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
