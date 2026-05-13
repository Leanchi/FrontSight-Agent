/*
Copyright 2026 红点画笔 C 原生模块 (Windows 版本)
@Windows 端 paintbrush overlay 功能 —— 使用 Win32 WS_EX_LAYERED 分层窗口

与 Linux 版（paintbrush_overlay.c）提供完全相同的 Duktape API，
通过 GDI+ 在 32 位 ARGB DIB section 上绘制，UpdateLayeredWindow 推送到屏幕。

通过 Duktape 注册为 "paintbrush-overlay" 模块，JS 端通过 require() 调用
*/

#ifdef WIN32

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../microscript/duktape.h"
#include "../microscript/ILibDuktape_Helpers.h"
#include "paintbrush_overlay.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

/* ============================================================
   静态状态
   ============================================================ */
static ULONG_PTR g_gdiplusToken = 0;
static int g_gdiplusStarted = 0;

static HWND g_hWnd = NULL;
static HDC g_hMemDC = NULL;
static HBITMAP g_hBitmap = NULL;
static HBITMAP g_hOldBitmap = NULL;
static BYTE* g_pBits = NULL;
static int g_width = 0;
static int g_height = 0;
static int g_offsetX = 0;
static int g_offsetY = 0;

static Graphics* g_pGraphics = NULL;
static Pen* g_pPen = NULL;

static const WCHAR* CLASS_NAME = L"MeshCentralPaintbrushOverlay";
#define SHARED_MEM_NAME L"MeshCentral_Paintbrush_Active"
static HANDLE g_hSharedMem = NULL;
static volatile LONG* g_pActiveFlag = NULL;

/* ============================================================
   共享内存：KVM tile.cpp 读取此标志决定是否启用 CAPTUREBLT
   ============================================================ */
static void initSharedMem()
{
    g_hSharedMem = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LONG), SHARED_MEM_NAME);
    if (g_hSharedMem)
    {
        g_pActiveFlag = (LONG*)MapViewOfFile(g_hSharedMem, FILE_MAP_WRITE, 0, 0, sizeof(LONG));
        if (g_pActiveFlag) InterlockedExchange((LONG*)g_pActiveFlag, 0);
    }
}

static void markDirty()
{
    if (g_pActiveFlag) InterlockedExchange((LONG*)g_pActiveFlag, 1);
}

static void markClean()
{
    if (g_pActiveFlag) InterlockedExchange((LONG*)g_pActiveFlag, 0);
}

static void cleanupSharedMem()
{
    if (g_pActiveFlag) { UnmapViewOfFile((void*)g_pActiveFlag); g_pActiveFlag = NULL; }
    if (g_hSharedMem) { CloseHandle(g_hSharedMem); g_hSharedMem = NULL; }
}

/* ============================================================
   辅助宏
   ============================================================ */
#define CLAMP(val, max) ((val) < 0 ? 0 : ((val) > (max) ? (max) : (val)))

/* ============================================================
   WndProc —— 最小化处理，所有消息交给默认处理
   ============================================================ */
LRESULT CALLBACK PaintbrushWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

/* ============================================================
   注册窗口类 —— 首次调用时注册
   ============================================================ */
static int RegisterPaintbrushClass()
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = PaintbrushWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    return RegisterClassExW(&wc) != 0 ? 1 : -1;
}

/* ============================================================
   释放绘图资源（DIB section、GDI+ 对象），不销毁窗口
   ============================================================ */
static void cleanupDrawingResources()
{
    if (g_pPen) { delete g_pPen; g_pPen = NULL; }
    if (g_pGraphics) { delete g_pGraphics; g_pGraphics = NULL; }
    if (g_hOldBitmap) { SelectObject(g_hMemDC, g_hOldBitmap); g_hOldBitmap = NULL; }
    if (g_hBitmap) { DeleteObject(g_hBitmap); g_hBitmap = NULL; }
    if (g_hMemDC) { DeleteDC(g_hMemDC); g_hMemDC = NULL; }
    g_pBits = NULL;
}

/* ============================================================
   创建/重建 DIB section 和 GDI+ 绘图资源
   ============================================================ */
static int CreateDrawingResources()
{
    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) return 0;

    /* 释放旧资源 */
    cleanupDrawingResources();

    g_hMemDC = CreateCompatibleDC(hScreenDC);
    if (!g_hMemDC) { ReleaseDC(NULL, hScreenDC); return 0; }

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_width;
    bmi.bmiHeader.biHeight = -g_height;  /* top-down DIB */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hBitmap = CreateDIBSection(g_hMemDC, &bmi, DIB_RGB_COLORS,
        (void**)&g_pBits, NULL, 0);
    if (!g_hBitmap) { cleanupDrawingResources(); ReleaseDC(NULL, hScreenDC); return 0; }

    g_hOldBitmap = (HBITMAP)SelectObject(g_hMemDC, g_hBitmap);

    /* 初始化为全透明 */
    if (g_pBits)
        memset(g_pBits, 0, (size_t)g_width * g_height * 4);

    /* 创建 GDI+ Graphics */
    g_pGraphics = new Graphics(g_hMemDC);
    if (!g_pGraphics) { cleanupDrawingResources(); ReleaseDC(NULL, hScreenDC); return 0; }
    g_pGraphics->SetSmoothingMode(SmoothingModeAntiAlias);

    /* 创建红色 Pen：ARGB(255, 255, 0, 0)，线宽 3，圆头/圆角 */
    g_pPen = new Pen(Color(255, 255, 0, 0), 3.0f);
    if (!g_pPen) { cleanupDrawingResources(); ReleaseDC(NULL, hScreenDC); return 0; }
    g_pPen->SetStartCap(LineCapRound);
    g_pPen->SetEndCap(LineCapRound);
    g_pPen->SetLineJoin(LineJoinRound);

    ReleaseDC(NULL, hScreenDC);
    return 1;
}

/* ============================================================
   updateScreen() —— 调用 UpdateLayeredWindow 将 DIB 推送到屏幕
   ============================================================ */
static void updateScreen()
{
    if (!g_hWnd || !g_hMemDC) return;

    /* 刷新 GDI+ 缓冲，确保绘图操作写入 DIB */
    if (g_pGraphics) g_pGraphics->Flush(Gdiplus::FlushIntentionSync);

    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) return;

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { g_width, g_height };
    POINT ptDst = { g_offsetX, g_offsetY };

    BOOL ok = UpdateLayeredWindow(g_hWnd, hScreenDC, &ptDst, &sizeWnd,
        g_hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);
    if (!ok)
    {
        printf("paintbrush-overlay: UpdateLayeredWindow FAILED, err=%lu\n", GetLastError());
        fflush(stdout);
    }

    /* 确保窗口在最顶层 */
    SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    ReleaseDC(NULL, hScreenDC);
}

/* ============================================================
   检测分辨率/显示器变化，必要时重建 overlay
   ============================================================ */
static int checkScreenChange()
{
    int newW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int newH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int newX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int newY = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (newW != g_width || newH != g_height ||
        newX != g_offsetX || newY != g_offsetY)
    {
        g_width = newW;
        g_height = newH;
        g_offsetX = newX;
        g_offsetY = newY;

        /* 重建 DIB 和窗口 */
        CreateDrawingResources();

        if (g_hWnd)
        {
            SetWindowPos(g_hWnd, NULL, g_offsetX, g_offsetY,
                g_width, g_height, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        /* 重新显示窗口 */
        if (g_hWnd)
            updateScreen();

        return 1;  /* 发生了变化 */
    }
    return 0;
}

/* ============================================================
   initOverlay(displayName, xauthPath)
   创建 Win32 WS_EX_LAYERED 透明全屏 overlay 窗口
   JS: require('paintbrush-overlay').initOverlay()
   参数在 Windows 上被忽略（不需要 display 名或 Xauthority）
   返回: null（失败）或 { width, height }
   ============================================================ */
duk_ret_t paintbrush_initOverlay(duk_context *ctx)
{
    /* 已初始化则返回当前尺寸 */
    if (g_gdiplusStarted && g_hWnd)
    {
        duk_push_object(ctx);
        duk_push_int(ctx, g_width);
        duk_put_prop_string(ctx, -2, "width");
        duk_push_int(ctx, g_height);
        duk_put_prop_string(ctx, -2, "height");
        return 1;
    }

    /* 初始化 GDI+ */
    if (!g_gdiplusStarted)
    {
        GdiplusStartupInput gdiplusInput;
        if (GdiplusStartup(&g_gdiplusToken, &gdiplusInput, NULL) != Ok)
        {
            duk_push_null(ctx);
            return 1;
        }
        g_gdiplusStarted = 1;
    }

    /* 获取虚拟屏幕尺寸（覆盖所有显示器） */
    g_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (g_width <= 0 || g_height <= 0)
    {
        duk_push_null(ctx);
        return 1;
    }

    /* 注册窗口类 */
    if (RegisterPaintbrushClass() <= 0)
    {
        duk_push_null(ctx);
        return 1;
    }

    /* 创建分层窗口：
     * WS_EX_LAYERED —— 支持逐像素 alpha
     * WS_EX_TRANSPARENT —— 所有鼠标事件穿透
     * WS_EX_TOPMOST —— 始终在最顶层
     * WS_EX_TOOLWINDOW —— 不显示在 Alt-Tab / 任务栏 */
    g_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"meshcentral-paintbrush",
        WS_POPUP,
        g_offsetX, g_offsetY, g_width, g_height,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (!g_hWnd)
    {
        duk_push_null(ctx);
        return 1;
    }

    /* 创建 DIB section 和 GDI+ 绘图资源 */
    if (!CreateDrawingResources())
    {
        DestroyWindow(g_hWnd);
        g_hWnd = NULL;
        duk_push_null(ctx);
        return 1;
    }

    /* 初始化共享内存（KVM 读取此标志决定是否启用 CAPTUREBLT） */
    initSharedMem();

    /* 显示窗口：WS_EX_LAYERED 窗口必须调用 ShowWindow 才会出现在桌面 */
    ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);

    /* 设置初始内容（全透明） */
    updateScreen();

    /* 返回尺寸 */
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
    if (!g_pGraphics || !g_pPen) return 0;

    int x1 = CLAMP(duk_require_int(ctx, 0), g_width);
    int y1 = CLAMP(duk_require_int(ctx, 1), g_height);
    int x2 = CLAMP(duk_require_int(ctx, 2), g_width);
    int y2 = CLAMP(duk_require_int(ctx, 3), g_height);

    g_pGraphics->DrawLine(g_pPen, x1, y1, x2, y2);
    markDirty();
    return 0;
}

/* ============================================================
   drawDot(x, y, radius)
   JS: overlay.drawDot(200, 300, 3)
   ============================================================ */
duk_ret_t paintbrush_drawDot(duk_context *ctx)
{
    if (!g_pGraphics || !g_pPen) return 0;

    int x = CLAMP(duk_require_int(ctx, 0), g_width);
    int y = CLAMP(duk_require_int(ctx, 1), g_height);
    int radius = duk_require_int(ctx, 2);
    if (radius < 1) radius = 1;
    if (radius > 50) radius = 50;

    SolidBrush brush(Color(255, 255, 0, 0));
    g_pGraphics->FillEllipse(&brush, x - radius, y - radius,
        radius * 2, radius * 2);
    markDirty();
    return 0;
}

/* ============================================================
   drawStroke(points)
   JS: overlay.drawStroke([{x:1,y:2},{x:3,y:4}])
   绘制一条完整笔画（多段线 + 端点圆）
   ============================================================ */
duk_ret_t paintbrush_drawStroke(duk_context *ctx)
{
    if (!g_pGraphics || !g_pPen) return 0;
    if (!duk_is_array(ctx, 0)) return 0;

    duk_size_t len = duk_get_length(ctx, 0);
    if (len == 0) return 0;

    /* 单点 -> 画圆 */
    if (len == 1)
    {
        duk_get_prop_index(ctx, 0, 0);
        duk_get_prop_string(ctx, -1, "x");
        int x1 = CLAMP(duk_get_int(ctx, -1), g_width);
        duk_pop(ctx);
        duk_get_prop_string(ctx, -1, "y");
        int y1 = CLAMP(duk_get_int(ctx, -1), g_height);
        duk_pop_2(ctx);

        SolidBrush brush(Color(255, 255, 0, 0));
        g_pGraphics->FillEllipse(&brush, x1 - 3, y1 - 3, 6, 6);
        markDirty();
        return 0;
    }

    /* 多点 -> 先收集坐标 */
    Point* pts = (Point*)malloc(len * sizeof(Point));
    if (!pts) return 0;

    for (duk_size_t i = 0; i < len; i++)
    {
        duk_get_prop_index(ctx, 0, (duk_uarridx_t)i);
        duk_get_prop_string(ctx, -1, "x");
        pts[i].X = CLAMP(duk_get_int(ctx, -1), g_width);
        duk_pop(ctx);
        duk_get_prop_string(ctx, -1, "y");
        pts[i].Y = CLAMP(duk_get_int(ctx, -1), g_height);
        duk_pop_2(ctx);
    }

    /* 画多段线 */
    g_pGraphics->DrawLines(g_pPen, pts, (INT)len);

    /* 端点圆 */
    SolidBrush brush(Color(255, 255, 0, 0));
    g_pGraphics->FillEllipse(&brush,
        pts[0].X - 3, pts[0].Y - 3, 6, 6);
    g_pGraphics->FillEllipse(&brush,
        pts[len - 1].X - 3, pts[len - 1].Y - 3, 6, 6);

    free(pts);
    markDirty();
    return 0;
}

/* ============================================================
   clearWindow()
   JS: overlay.clearWindow()
   ============================================================ */
duk_ret_t paintbrush_clearWindow(duk_context *ctx)
{
    (void)ctx;
    if (!g_pBits) return 0;

    /* 清空 DIB section（全透明） */
    memset(g_pBits, 0, (size_t)g_width * g_height * 4);
    markClean();

    /* 推送到屏幕 */
    updateScreen();
    return 0;
}

/* ============================================================
   flush()
   JS: overlay.flush()
   ============================================================ */
duk_ret_t paintbrush_flush(duk_context *ctx)
{
    (void)ctx;
    if (!g_gdiplusStarted) return 0;

    /* 检查分辨率变化 */
    checkScreenChange();

    /* 诊断：检查 DIB 是否有非零像素 */
    if (g_pBits && g_width > 0 && g_height > 0)
    {
        int dirty = 0;
        for (int i = 0; i < g_width * g_height * 4; i += 4)
        {
            if (g_pBits[i] != 0 || g_pBits[i+1] != 0 || g_pBits[i+2] != 0 || g_pBits[i+3] != 0)
            {
                dirty = 1;
                break;
            }
        }
        printf("paintbrush-overlay: flush() dirty=%d hWnd=%p IsWindow=%d visible=%d\n",
            dirty, (void*)g_hWnd, IsWindow(g_hWnd), IsWindowVisible(g_hWnd));
        fflush(stdout);
    }

    /* 推送 DIB 到屏幕 */
    updateScreen();
    return 0;
}

/* ============================================================
   destroy()
   JS: overlay.destroy()
   ============================================================ */
duk_ret_t paintbrush_destroy(duk_context *ctx)
{
    (void)ctx;

    markClean();
    cleanupDrawingResources();

    if (g_hWnd) { DestroyWindow(g_hWnd); g_hWnd = NULL; }

    if (g_gdiplusStarted) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusStarted = 0;
        g_gdiplusToken = 0;
    }

    cleanupSharedMem();

    return 0;
}

/* ============================================================
   模块注册：Duktape PUSH 函数
   当 JS 执行 require('paintbrush-overlay') 时调用
   与 Linux 版 paintbrush_overlay.c 保持完全一致的 API
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

#endif  /* WIN32 */
