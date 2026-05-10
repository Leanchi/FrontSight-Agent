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
static int screenWidth = 0;
static int screenHeight = 0;

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
	Atom wmWindowType;
	Atom wmTypeNotification;
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

	/* 设置 _NET_WM_WINDOW_TYPE = _NET_WM_WINDOW_TYPE_NOTIFICATION */
	wmWindowType = XInternAtom(overlayDisplay, "_NET_WM_WINDOW_TYPE", False);
	wmTypeNotification = XInternAtom(overlayDisplay, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
	if (wmWindowType != None && wmTypeNotification != None)
	{
		XChangeProperty(overlayDisplay, overlayWindow, wmWindowType, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)&wmTypeNotification, 1);
	}

	/* 保持窗口在最上层 */
	{
		Atom netWmState = XInternAtom(overlayDisplay, "_NET_WM_STATE", False);
		Atom netWmStateAbove = XInternAtom(overlayDisplay, "_NET_WM_STATE_ABOVE", False);
		if (netWmState != None && netWmStateAbove != None)
		{
			XChangeProperty(overlayDisplay, overlayWindow, netWmState, XA_ATOM, 32,
				PropModeReplace, (unsigned char *)&netWmStateAbove, 1);
		}
	}

	/* 鼠标事件穿透：让 overlay 不拦截任何输入事件 */
	{
		XSetWindowAttributes inputAttrs;
		inputAttrs.event_mask = 0; /* 不接收任何事件 */
		inputAttrs.do_not_propagate_mask = (ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
		XChangeWindowAttributes(overlayDisplay, overlayWindow,
			CWEventMask | CWDontPropagate, &inputAttrs);
	}

	/* 使用 XShape 扩展让输入穿透（最可靠的方式） */
	{
		/* XShape 扩展：设置空的 input region，让所有鼠标事件穿透到下层 */
		int shapeEventBase, shapeErrorBase;
		if (XShapeQueryExtension(overlayDisplay, &shapeEventBase, &shapeErrorBase))
		{
			XShapeCombineMask(overlayDisplay, overlayWindow, ShapeInput, 0, 0, None, ShapeSet);
		}
		else
		{
		}
	}

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

	/* 显示窗口 */
	XMapWindow(overlayDisplay, overlayWindow);
	XFlush(overlayDisplay);


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
duk_ret_t paintbrush_drawLine(duk_context *ctx)
{
	int x1, y1, x2, y2;

	if (!overlayDisplay || !overlayGC) return 0;

	x1 = duk_require_int(ctx, 0);
	y1 = duk_require_int(ctx, 1);
	x2 = duk_require_int(ctx, 2);
	y2 = duk_require_int(ctx, 3);

	XDrawLine(overlayDisplay, overlayWindow, overlayGC, x1, y1, x2, y2);
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

	x = duk_require_int(ctx, 0);
	y = duk_require_int(ctx, 1);
	radius = duk_require_int(ctx, 2);

	XFillArc(overlayDisplay, overlayWindow, overlayGC,
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
		x1 = duk_get_int(ctx, -1);
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, "y");
		y1 = duk_get_int(ctx, -1);
		duk_pop_2(ctx);

		XFillArc(overlayDisplay, overlayWindow, overlayGC,
			x1 - 3, y1 - 3, 6, 6, 0, 360 * 64);
		return 0;
	}

	/* 多点 -> 画线段 */
	duk_get_prop_index(ctx, 0, 0);
	duk_get_prop_string(ctx, -1, "x");
	firstX = duk_get_int(ctx, -1);
	duk_pop(ctx);
	duk_get_prop_string(ctx, -1, "y");
	firstY = duk_get_int(ctx, -1);
	duk_pop_2(ctx);

	for (i = 1; i < len; i++)
	{
		duk_get_prop_index(ctx, 0, i);
		duk_get_prop_string(ctx, -1, "x");
		x2 = duk_get_int(ctx, -1);
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, "y");
		y2 = duk_get_int(ctx, -1);
		duk_pop_2(ctx);

		duk_get_prop_index(ctx, 0, i - 1);
		duk_get_prop_string(ctx, -1, "x");
		x1 = duk_get_int(ctx, -1);
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, "y");
		y1 = duk_get_int(ctx, -1);
		duk_pop_2(ctx);

		XDrawLine(overlayDisplay, overlayWindow, overlayGC, x1, y1, x2, y2);
	}

	duk_get_prop_index(ctx, 0, len - 1);
	duk_get_prop_string(ctx, -1, "x");
	lastX = duk_get_int(ctx, -1);
	duk_pop(ctx);
	duk_get_prop_string(ctx, -1, "y");
	lastY = duk_get_int(ctx, -1);
	duk_pop_2(ctx);

	/* 端点圆 */
	XFillArc(overlayDisplay, overlayWindow, overlayGC,
		firstX - 3, firstY - 3, 6, 6, 0, 360 * 64);
	XFillArc(overlayDisplay, overlayWindow, overlayGC,
		lastX - 3, lastY - 3, 6, 6, 0, 360 * 64);

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
	/* 用全透明像素填充整个窗口（XClearWindow 对 background_pixmap=None 无效） */
	XFillRectangle(overlayDisplay, overlayWindow, overlayClearGC,
		0, 0, screenWidth, screenHeight);
	return 0;
}

/* ============================================================
   flush()
   JS: overlay.flush()
   ============================================================ */
duk_ret_t paintbrush_flush(duk_context *ctx)
{
	if (!overlayDisplay) return 0;
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
		if (overlayGC) { XFreeGC(overlayDisplay, overlayGC); overlayGC = 0; }
		if (overlayClearGC) { XFreeGC(overlayDisplay, overlayClearGC); overlayClearGC = 0; }
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
