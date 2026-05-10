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
绘制红笔标注、支持橡皮擦和流星式自动消失（每点 5s 过期）。
通过 stdin 接收 JSON 命令，每行一条。
*/

console.log('paintbrush-overlay: 脚本开始执行...');

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
var timedPenInterval = null;  // setInterval ID for timedpen 过期裁剪

// ============================================================
// X11 透明窗口创建
// ============================================================

function findARGBVisual(disp, screenId)
{
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
        var visualPtr = visualList.Deref(0, GM.PointerSize);
        X11.XFree(visualList);
        return visualPtr;
    }

    if (visualList.Val != 0) { X11.XFree(visualList); }
    return null;
}

function createOverlayWindow()
{
    if (!process.env.DISPLAY)
    {
        console.log('paintbrush-overlay: DISPLAY 环境变量未设置');
        return false;
    }

    display = X11.XOpenDisplay(GM.CreateVariable(process.env.DISPLAY));
    if (display.Val == 0)
    {
        console.log('paintbrush-overlay: XOpenDisplay 失败');
        return false;
    }

    var screenId = X11.XDefaultScreen(display).Val;
    var width = X11.XDisplayWidth(display, screenId).Val;
    var height = X11.XDisplayHeight(display, screenId).Val;
    var rootWindow = X11.XRootWindow(display, screenId);

    screenInfo = { width: width, height: height, screenId: screenId, rootWindow: rootWindow };

    var visual = findARGBVisual(display, screenId);
    var depth = 32;

    if (visual == null)
    {
        console.log('paintbrush-overlay: 未找到 ARGB visual，使用默认 visual');
        visual = X11.XDefaultVisual(display, screenId);
        depth = X11.XDefaultDepth(display, screenId).Val;
    }

    var colormap = X11.XCreateColormap(display, rootWindow, visual, 0);

    var attrSize = GM.PointerSize == 8 ? 104 : 56;
    var attrs = GM.CreateVariable(attrSize);

    var borderPixelOffset = GM.PointerSize * 2;
    var colormapOffset = GM.PointerSize == 8 ? 88 : 48;

    attrs.Deref(0, GM.PointerSize).toBuffer().writeUInt32LE(0);
    attrs.Deref(borderPixelOffset, 4).toBuffer().writeUInt32LE(0);
    colormap.pointerBuffer().copy(attrs.Deref(colormapOffset, GM.PointerSize).toBuffer());

    var valuemask = CWBackPixmap | CWColormap | CWBorderPixel;

    overlayWindow = X11.XCreateWindow(
        display, rootWindow,
        0, 0, width, height,
        0, depth, InputOutput, visual, valuemask, attrs
    );

    if (overlayWindow.Val == 0)
    {
        console.log('paintbrush-overlay: XCreateWindow 失败');
        return false;
    }

    X11.XStoreName(display, overlayWindow, GM.CreateVariable('meshcentral-paintbrush'));
    X11.Xutf8SetWMProperties(display, overlayWindow, GM.CreateVariable('meshcentral-paintbrush'), 0, 0, 0, 0, 0, 0);

    mi.unDecorateWindow(display, overlayWindow);
    mi.setAllowedActions(display, overlayWindow, 0);
    mi.setAlwaysOnTop(display, rootWindow, overlayWindow);
    mi.hideWindowIcon(display, rootWindow, overlayWindow);

    var wmWindowType = X11.XInternAtom(display, GM.CreateVariable('_NET_WM_WINDOW_TYPE'), 0);
    var wmTypeNotification = X11.XInternAtom(display, GM.CreateVariable('_NET_WM_WINDOW_TYPE_NOTIFICATION'), 0);
    if (wmWindowType.Val != 0 && wmTypeNotification.Val != 0)
    {
        var atomData = GM.CreateVariable(GM.PointerSize);
        wmTypeNotification.pointerBuffer().copy(atomData.Deref(0, GM.PointerSize).toBuffer());
        X11.XChangeProperty(display, overlayWindow, wmWindowType, XA_ATOM, 32, PropModeReplace, atomData, 1);
    }

    mi.setWindowSizeHints(display, overlayWindow, 0, 0, width, height, width, height, width, height);

    gc = X11.XCreateGC(display, overlayWindow, 0, 0);

    X11.XMapWindow(display, overlayWindow);
    X11.XFlush(display);

    console.log('paintbrush-overlay: 窗口创建成功 (' + width + 'x' + height + ')');
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
        x - radius, y - radius,
        radius * 2, radius * 2,
        0, 360 * ARC_ANGLE_SCALE
    );
}

function drawStroke(stroke)
{
    if (stroke.points == null || stroke.points.length == 0) return;

    if (stroke.points.length == 1)
    {
        drawDot(stroke.points[0].x, stroke.points[0].y, PEN_WIDTH);
    }
    else
    {
        for (var i = 1; i < stroke.points.length; i++)
        {
            drawLine(
                stroke.points[i - 1].x, stroke.points[i - 1].y,
                stroke.points[i].x, stroke.points[i].y
            );
        }
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
// timedpen 流星效果：逐点过期裁剪
// ============================================================

function timedPenTick()
{
    var now = Date.now();
    var needRedraw = false;
    var anyTimedPen = false;

    for (var i = strokeOrder.length - 1; i >= 0; i--)
    {
        var id = strokeOrder[i];
        var stroke = activeStrokes[id];
        if (!stroke || stroke.tool !== 'timedpen') continue;

        anyTimedPen = true;

        // 裁剪超过 5s 的点
        while (stroke.points.length > 0 && stroke.points[0].t != null && (now - stroke.points[0].t) > 5000)
        {
            stroke.points.shift();
            needRedraw = true;
        }

        // 所有点过期则移除笔画
        if (stroke.points.length === 0)
        {
            delete activeStrokes[id];
            strokeOrder.splice(i, 1);
            needRedraw = true;
            anyTimedPen = false;
        }
    }

    if (needRedraw)
    {
        redrawAll();
    }

    // 没有 timedpen 笔画了，停止 tick
    if (!anyTimedPen)
    {
        // 再次确认
        for (var j = 0; j < strokeOrder.length; j++)
        {
            if (activeStrokes[strokeOrder[j]] && activeStrokes[strokeOrder[j]].tool === 'timedpen')
            {
                anyTimedPen = true;
                break;
            }
        }
        if (!anyTimedPen && timedPenInterval)
        {
            clearInterval(timedPenInterval);
            timedPenInterval = null;
        }
    }
}

function startTimedPenTickIfNeeded()
{
    if (!timedPenInterval)
    {
        timedPenInterval = setInterval(timedPenTick, 100);
    }
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

    // timedpen 启动逐点过期 tick
    if (tool === 'timedpen')
    {
        startTimedPenTickIfNeeded();
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

    var idx = strokeOrder.indexOf(strokeId);
    if (idx >= 0) { strokeOrder.splice(idx, 1); }

    redrawAll();
}

function eraseAt(x, y, radius)
{
    var toRemove = [];
    for (var id in activeStrokes)
    {
        // 橡皮擦只作用于 pen 笔画
        if (activeStrokes[id].tool !== 'pen') continue;
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
    if (timedPenInterval)
    {
        clearInterval(timedPenInterval);
        timedPenInterval = null;
    }
    X11.XClearWindow(display, overlayWindow);
    X11.XFlush(display);
}

function clearPenOnly()
{
    var toRemove = [];
    for (var id in activeStrokes)
    {
        if (activeStrokes[id].tool === 'pen')
        {
            if (activeStrokes[id].timerId != null)
            {
                clearTimeout(activeStrokes[id].timerId);
            }
            toRemove.push(id);
        }
    }
    for (var i = 0; i < toRemove.length; i++)
    {
        delete activeStrokes[toRemove[i]];
        var idx = strokeOrder.indexOf(toRemove[i]);
        if (idx >= 0) { strokeOrder.splice(idx, 1); }
    }
    redrawAll();
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
        case 'stroke_start':
            // timedpen 增量模式：开始新笔画
            addStroke(cmd.strokeId, cmd.tool, [cmd.firstPoint], Date.now());
            break;
        case 'stroke_append':
            // timedpen 增量模式：追加点位
            if (activeStrokes[cmd.strokeId] && cmd.points && cmd.points.length > 0)
            {
                var stroke = activeStrokes[cmd.strokeId];
                for (var i = 0; i < cmd.points.length; i++)
                {
                    stroke.points.push(cmd.points[i]);
                }
                // 增量绘制新线段
                var startIdx = Math.max(1, stroke.points.length - cmd.points.length);
                for (var j = startIdx; j < stroke.points.length; j++)
                {
                    drawLine(stroke.points[j - 1].x, stroke.points[j - 1].y, stroke.points[j].x, stroke.points[j].y);
                }
                // 画端点
                drawDot(stroke.points[stroke.points.length - 1].x, stroke.points[stroke.points.length - 1].y, PEN_WIDTH);
                X11.XFlush(display);
            }
            break;
        case 'stroke_end':
            // timedpen 笔画完成，tick 接管过期
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
        case 'clear_pen':
            clearPenOnly();
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
    console.setInfoLevel(1); // 启用 console.info1 输出（Duktape 默认 INFO_Level=0 会抑制）
    if (!createOverlayWindow())
    {
        process.exit(1);
    }

    process.stdin.resume();
    process.stdin.setEncoding('utf8');
    var buffer = '';

    process.stdin.on('data', function (chunk)
    {
        buffer += chunk;
        var lines = buffer.split('\n');
        buffer = lines.pop();

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
                console.log('paintbrush-overlay: 命令解析失败: ' + ex);
            }
        }
    });

    process.stdin.on('end', function ()
    {
        cleanup();
        process.exit(0);
    });

    process.on('exit', function ()
    {
        cleanup();
    });
}

function cleanup()
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

    if (timedPenInterval)
    {
        clearInterval(timedPenInterval);
        timedPenInterval = null;
    }

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
