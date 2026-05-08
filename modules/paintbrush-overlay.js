/*
Copyright 2024 Intel Corporation
@author Bryan Roe / 修改: 红点画笔覆盖层

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

红点画笔覆盖层 - 在 UOS Linux 桌面上创建 X11 ARGB 透明窗口，
绘制红笔标注、支持橡皮擦和 5s 自动消失。
通过 stdin 接收 JSON 命令，每行一条。
*/

var mi = require('monitor-info');
var GM = require('_GenericMarshal');
var X11 = mi._X11;

// 常量 - 正确的 X11/Xutil.h 掩码值
var VisualScreenMask = 0x2;
var VisualDepthMask = 0x4;
var VisualClassMask = 0x8;
var TrueColor = 4;
var CWBackPixmap = 0x0002;
var CWColormap = 0x2000;
var CWBorderPixel = 0x0004;
var CWEventMask = 0x0800;
var InputOutput = 1;
var PropModeReplace = 0;
var XA_ATOM = 4;
var LineSolid = 0;
var CapRound = 2;
var JoinRound = 1;
var ARC_ANGLE_SCALE = 64;  // X11 角度单位 = 1/64 度

// 红色 ARGB 值 (不透明红色)
var COLOR_RED = 0xFFFF0000;
var COLOR_BG = 0x00000000;  // 完全透明

// 画笔线宽
var PEN_WIDTH = 3;

// 全局状态
var display = null;
var overlayWindow = null;
var gc = null;
var screenInfo = null;  // { width, height, screenId, rootWindow }
var activeStrokes = {};  // strokeId -> { tool, points, timestamp, timerId }
var strokeOrder = [];    // 笔画绘制顺序

// ============================================================
// X11 透明窗口创建
// ============================================================

function findARGBVisual(disp, screenId)
{
    // 查找 depth=32, TrueColor 的 ARGB visual
    // XVisualInfo 结构布局（64 位）：
    //   Visual* visual      offset 0,  8 bytes
    //   VisualID visualid   offset 8,  8 bytes
    //   int screen          offset 16, 4 bytes
    //   unsigned int depth  offset 20, 4 bytes
    //   int class           offset 24, 4 bytes
    // XVisualInfo 结构布局（32 位）：
    //   Visual* visual      offset 0,  4 bytes
    //   VisualID visualid   offset 4,  4 bytes
    //   int screen          offset 8,  4 bytes
    //   unsigned int depth  offset 12, 4 bytes
    //   int class           offset 16, 4 bytes

    var template = GM.CreateVariable(64);
    var screenOffset = GM.PointerSize == 8 ? 16 : 8;
    var depthOffset = GM.PointerSize == 8 ? 20 : 12;
    var classOffset = GM.PointerSize == 8 ? 24 : 16;

    template.Deref(screenOffset, 4).toBuffer().writeUInt32LE(screenId); // screen
    template.Deref(depthOffset, 4).toBuffer().writeUInt32LE(32);       // depth = 32
    template.Deref(classOffset, 4).toBuffer().writeUInt32LE(TrueColor); // class = TrueColor

    var countPtr = GM.CreateVariable(4);
    var visualList = X11.XGetVisualInfo(disp, VisualDepthMask | VisualScreenMask | VisualClassMask, template, countPtr);

    if (visualList.Val != 0 && countPtr.toBuffer().readUInt32LE() > 0)
    {
        // visual 指针在 XVisualInfo 的 offset 0
        var visualPtr = visualList.Deref(0, GM.PointerSize);
        X11.XFree(visualList);
        return visualPtr;
    }

    if (visualList.Val != 0) { X11.XFree(visualList); }
    return null;
}

function createOverlayWindow()
{
    // 检查 DISPLAY 环境变量
    if (!process.env.DISPLAY)
    {
        console.info1('paintbrush-overlay: DISPLAY 环境变量未设置');
        return false;
    }

    // 打开 X11 显示
    display = X11.XOpenDisplay(GM.CreateVariable(process.env.DISPLAY));
    if (display.Val == 0)
    {
        console.info1('paintbrush-overlay: XOpenDisplay 失败');
        return false;
    }

    var screenId = X11.XDefaultScreen(display).Val;
    var width = X11.XDisplayWidth(display, screenId).Val;
    var height = X11.XDisplayHeight(display, screenId).Val;
    var rootWindow = X11.XRootWindow(display, screenId);

    screenInfo = { width: width, height: height, screenId: screenId, rootWindow: rootWindow };

    // 尝试查找 ARGB visual
    var visual = findARGBVisual(display, screenId);
    var depth = 32;

    if (visual == null)
    {
        // 降级方案：使用默认 visual + XShape 或纯窗口
        console.info1('paintbrush-overlay: 未找到 ARGB visual，使用默认 visual');
        visual = X11.XDefaultVisual(display, screenId);
        depth = X11.XDefaultDepth(display, screenId).Val;
    }

    // 创建 colormap
    var colormap = X11.XCreateColormap(display, rootWindow, visual, 0);

    // 构造 XSetWindowAttributes 结构
    // XSetWindowAttributes 布局（64 位）：
    //   Pixmap background_pixmap;       // offset 0,  8 bytes
    //   unsigned long background_pixel; // offset 8,  8 bytes
    //   unsigned long border_pixel;     // offset 16, 8 bytes
    //   int bit_gravity;                // offset 24, 4 bytes
    //   int win_gravity;                // offset 28, 4 bytes
    //   int backing_store;              // offset 32, 4 bytes
    //   (padding)                       // offset 36, 4 bytes
    //   unsigned long backing_planes;   // offset 40, 8 bytes
    //   unsigned long backing_pixel;    // offset 48, 8 bytes
    //   Bool save_under;                // offset 56, 4 bytes
    //   (padding)                       // offset 60, 4 bytes
    //   long event_mask;                // offset 64, 8 bytes
    //   long do_not_propagate_mask;     // offset 72, 8 bytes
    //   Bool override_redirect;         // offset 80, 4 bytes
    //   (padding)                       // offset 84, 4 bytes
    //   Colormap colormap;              // offset 88, 8 bytes
    //   Cursor cursor;                  // offset 96, 8 bytes
    // 总计: 104 bytes
    //
    // XSetWindowAttributes 布局（32 位）：
    //   Pixmap background_pixmap;       // offset 0,  4 bytes
    //   unsigned long background_pixel; // offset 4,  4 bytes
    //   unsigned long border_pixel;     // offset 8,  4 bytes
    //   int bit_gravity;                // offset 12, 4 bytes
    //   int win_gravity;                // offset 16, 4 bytes
    //   int backing_store;              // offset 20, 4 bytes
    //   unsigned long backing_planes;   // offset 24, 4 bytes
    //   unsigned long backing_pixel;    // offset 28, 4 bytes
    //   Bool save_under;                // offset 32, 4 bytes
    //   long event_mask;                // offset 36, 4 bytes
    //   long do_not_propagate_mask;     // offset 40, 4 bytes
    //   Bool override_redirect;         // offset 44, 4 bytes
    //   Colormap colormap;              // offset 48, 4 bytes
    //   Cursor cursor;                  // offset 52, 4 bytes
    // 总计: 56 bytes

    var attrSize = GM.PointerSize == 8 ? 104 : 56;
    var attrs = GM.CreateVariable(attrSize);

    var borderPixelOffset = GM.PointerSize * 2;  // 16 (64-bit) / 8 (32-bit)
    var colormapOffset = GM.PointerSize == 8 ? 88 : 48;

    // background_pixmap = None (0)
    attrs.Deref(0, GM.PointerSize).toBuffer().writeUInt32LE(0);
    // border_pixel = 0
    attrs.Deref(borderPixelOffset, 4).toBuffer().writeUInt32LE(0);
    // colormap
    colormap.pointerBuffer().copy(attrs.Deref(colormapOffset, GM.PointerSize).toBuffer());

    var valuemask = CWBackPixmap | CWColormap | CWBorderPixel;

    // 创建窗口
    overlayWindow = X11.XCreateWindow(
        display, rootWindow,
        0, 0, width, height,
        0,            // border_width
        depth,        // depth
        InputOutput,  // class
        visual,       // visual
        valuemask,    // valuemask
        attrs         // attributes
    );

    if (overlayWindow.Val == 0)
    {
        console.info1('paintbrush-overlay: XCreateWindow 失败');
        return false;
    }

    // 设置窗口属性
    X11.XStoreName(display, overlayWindow, GM.CreateVariable('meshcentral-paintbrush'));
    X11.Xutf8SetWMProperties(display, overlayWindow, GM.CreateVariable('meshcentral-paintbrush'), 0, 0, 0, 0, 0, 0);

    // 无装饰
    mi.unDecorateWindow(display, overlayWindow);

    // 不允许窗口操作
    mi.setAllowedActions(display, overlayWindow, 0);

    // 置顶
    mi.setAlwaysOnTop(display, rootWindow, overlayWindow);

    // 不在任务栏显示
    mi.hideWindowIcon(display, rootWindow, overlayWindow);

    // 设置窗口类型为 NOTIFICATION/DOCK 使窗口管理器不干扰
    var wmWindowType = X11.XInternAtom(display, GM.CreateVariable('_NET_WM_WINDOW_TYPE'), 0);
    var wmTypeNotification = X11.XInternAtom(display, GM.CreateVariable('_NET_WM_WINDOW_TYPE_NOTIFICATION'), 0);
    if (wmWindowType.Val != 0 && wmTypeNotification.Val != 0)
    {
        var atomData = GM.CreateVariable(GM.PointerSize);
        wmTypeNotification.pointerBuffer().copy(atomData.Deref(0, GM.PointerSize).toBuffer());
        X11.XChangeProperty(display, overlayWindow, wmWindowType, XA_ATOM, 32, PropModeReplace, atomData, 1);
    }

    // 设置窗口大小提示（固定大小，不允许缩放）
    mi.setWindowSizeHints(display, overlayWindow, 0, 0, width, height, width, height, width, height);

    // 创建 Graphics Context
    gc = X11.XCreateGC(display, overlayWindow, 0, 0);

    // 映射窗口
    X11.XMapWindow(display, overlayWindow);
    X11.XFlush(display);

    console.info1('paintbrush-overlay: 窗口创建成功 (' + width + 'x' + height + ')');
    return true;
}

// ============================================================
// 绘制操作
// ============================================================

function drawLine(x1, y1, x2, y2)
{
    X11.XSetForeground(display, gc, COLOR_RED);
    X11.XSetLineAttributes(display, gc, PEN_WIDTH, LineSolid, CapRound, JoinRound);
    X11.XDrawLine(display, overlayWindow, gc, x1, y1, x2, y2);
}

function drawDot(x, y, radius)
{
    X11.XSetForeground(display, gc, COLOR_RED);
    X11.XFillArc(display, overlayWindow, gc,
        x - radius, y - radius,  // x, y (外接矩形左上角)
        radius * 2, radius * 2,  // width, height
        0, 360 * ARC_ANGLE_SCALE // angle1=0, angle2=360*64=全圆
    );
}

function drawStroke(stroke)
{
    if (stroke.points == null || stroke.points.length == 0) return;

    if (stroke.points.length == 1)
    {
        // 单点画红点
        drawDot(stroke.points[0].x, stroke.points[0].y, PEN_WIDTH);
    }
    else
    {
        // 多点画连线
        for (var i = 1; i < stroke.points.length; i++)
        {
            drawLine(
                stroke.points[i - 1].x, stroke.points[i - 1].y,
                stroke.points[i].x, stroke.points[i].y
            );
        }
        // 在端点画红点让线条更圆滑
        drawDot(stroke.points[0].x, stroke.points[0].y, PEN_WIDTH);
        drawDot(stroke.points[stroke.points.length - 1].x, stroke.points[stroke.points.length - 1].y, PEN_WIDTH);
    }
}

function redrawAll()
{
    X11.XClearWindow(display, overlayWindow);
    for (var i = 0; i < strokeOrder.length; i++)
    {
        drawStroke(activeStrokes[strokeOrder[i]]);
    }
    X11.XFlush(display);
}

// ============================================================
// 笔画管理
// ============================================================

function addStroke(strokeId, tool, points, timestamp)
{
    var entry = {
        tool: tool,
        points: points,
        timestamp: timestamp || Date.now(),
        timerId: null
    };

    activeStrokes[strokeId] = entry;
    strokeOrder.push(strokeId);

    // 立即绘制
    drawStroke(entry);
    X11.XFlush(display);

    // 5s 自动消失
    if (tool === 'timedpen')
    {
        entry.timerId = setTimeout(function ()
        {
            removeStroke(strokeId);
        }, 5000);
    }
}

function removeStroke(strokeId)
{
    if (activeStrokes[strokeId] == null) return;

    if (activeStrokes[strokeId].timerId != null)
    {
        clearTimeout(activeStrokes[strokeId].timerId);
    }

    delete activeStrokes[strokeId];

    // 从顺序列表中移除
    var idx = strokeOrder.indexOf(strokeId);
    if (idx >= 0) { strokeOrder.splice(idx, 1); }

    // 重绘剩余笔画
    redrawAll();
}

function eraseAt(x, y, radius)
{
    var toRemove = [];
    for (var id in activeStrokes)
    {
        if (strokeIntersects(activeStrokes[id], x, y, radius))
        {
            toRemove.push(id);
        }
    }
    for (var i = 0; i < toRemove.length; i++)
    {
        removeStroke(toRemove[i]);
    }
}

function eraseStrokes(strokeIds)
{
    for (var i = 0; i < strokeIds.length; i++)
    {
        var strokeId = strokeIds[i];
        if (activeStrokes[strokeId] == null) continue;

        if (activeStrokes[strokeId].timerId != null)
        {
            clearTimeout(activeStrokes[strokeId].timerId);
        }
        delete activeStrokes[strokeId];

        var idx = strokeOrder.indexOf(strokeId);
        if (idx >= 0) { strokeOrder.splice(idx, 1); }
    }
    redrawAll();
}

function clearAll()
{
    for (var id in activeStrokes)
    {
        if (activeStrokes[id].timerId != null)
        {
            clearTimeout(activeStrokes[id].timerId);
        }
    }
    activeStrokes = {};
    strokeOrder = [];
    X11.XClearWindow(display, overlayWindow);
    X11.XFlush(display);
}

function strokeIntersects(stroke, x, y, radius)
{
    if (stroke.points == null) return false;
    for (var i = 0; i < stroke.points.length; i++)
    {
        var dx = stroke.points[i].x - x;
        var dy = stroke.points[i].y - y;
        if (dx * dx + dy * dy <= radius * radius) return true;
    }
    return false;
}

// ============================================================
// 命令处理
// ============================================================

function processCommand(cmd)
{
    switch (cmd.sub)
    {
        case 'stroke':
            addStroke(cmd.strokeId, cmd.tool, cmd.points, cmd.timestamp);
            break;
        case 'erase':
            if (cmd.strokeIds && cmd.strokeIds.length > 0)
            {
                eraseStrokes(cmd.strokeIds);
            }
            else if (cmd.x != null && cmd.y != null)
            {
                eraseAt(cmd.x, cmd.y, cmd.radius || 20);
            }
            break;
        case 'clear':
            clearAll();
            break;
        case 'batch':
            if (cmd.commands)
            {
                for (var i = 0; i < cmd.commands.length; i++)
                {
                    processCommand(cmd.commands[i]);
                }
            }
            break;
    }
}

// ============================================================
// 初始化和主循环
// ============================================================

function main()
{
    if (!createOverlayWindow())
    {
        process.exit(1);
    }

    // stdin 命令循环
    process.stdin.resume();
    process.stdin.setEncoding('utf8');
    var buffer = '';

    process.stdin.on('data', function (chunk)
    {
        buffer += chunk;
        var lines = buffer.split('\n');
        buffer = lines.pop();  // 保留不完整的行

        for (var i = 0; i < lines.length; i++)
        {
            var line = lines[i].trim();
            if (line === '') continue;
            try
            {
                var cmd = JSON.parse(line);
                processCommand(cmd);
            }
            catch (ex)
            {
                console.info1('paintbrush-overlay: 命令解析失败: ' + ex);
            }
        }
    });

    process.stdin.on('end', function ()
    {
        // stdin 关闭，清理并退出
        cleanup();
        process.exit(0);
    });

    // 退出时清理
    process.on('exit', function ()
    {
        cleanup();
    });
}

function cleanup()
{
    // 清理所有定时器
    for (var id in activeStrokes)
    {
        if (activeStrokes[id].timerId != null)
        {
            clearTimeout(activeStrokes[id].timerId);
        }
    }
    activeStrokes = {};
    strokeOrder = [];

    if (overlayWindow != null && overlayWindow.Val != 0 && display != null && display.Val != 0)
    {
        try
        {
            X11.XDestroyWindow(display, overlayWindow);
            X11.XFlush(display);
            X11.XCloseDisplay(display);
        }
        catch (ex) { }
    }
    overlayWindow = null;
    display = null;
    gc = null;
}

// 启动
main();
