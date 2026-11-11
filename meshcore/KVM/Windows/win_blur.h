/*
 * Windows 窗口虚化 - 头文件
 *
 * 接口形态对齐 Linux 版 linux_blur.h（get_blurred_regions / free_blurred_regions）。
 * 消费方：tile.cpp（C++ 编译，故需 extern "C" 保护）。
 */

#ifndef WIN_BLUR_H
#define WIN_BLUR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int x, y, w, h;
} BlurRect;

/* 枚举标题命中遮罩关键字的窗口的可见区域。
 * 坐标系 = "捕获区相对坐标"：已平移 -SCREEN_X/-SCREEN_Y、裁剪到
 * (0,0,SCREEN_WIDTH,SCREEN_HEIGHT)，y 向下为正（屏幕方向，非缓冲行序）。
 * 成功返回区域数，*regions 指向新分配数组（free_blurred_regions 释放）；
 * 任何失败返回 0 且 *regions == NULL（画面原样，绝不崩溃采集线程）。 */
int get_blurred_regions(BlurRect **regions);

void free_blurred_regions(BlurRect *regions);

/* Windows 版灰色填充（tile.cpp 采集后、tile 编码前调用）。
 * 与 linux_tile.c apply_blur_to_regions 语义相同，但实现必须自写：
 * GDI 缓冲区像素格式动态（PIXEL_SIZE 3/4 字节）、行序 bottom-up（正 biHeight
 * 时缓冲第 0 行是屏幕底行）、内存字节序 BGR。
 *   buf / bufsize    : GetDIBits 产物缓冲区及其字节数
 *   bufw / bufh      : 缓冲区宽高（像素）= bmiHeader.biWidth / abs(biHeight)
 *   topdown          : 行序。0 = bottom-up（正 biHeight，屏幕行 y 翻转到缓冲行
 *                      bufh-1-y）；1 = top-down（负 biHeight，不翻转）。
 *                      由 tile.cpp 按 bmpInfo.bmiHeader.biHeight 实际符号传入。
 *   regions / count  : get_blurred_regions 的输出（捕获区相对坐标）
 * SCALING_FACTOR != 1024（远程缩放采集）时内部先做 screen→buffer 坐标缩放。 */
void apply_blur_to_regions_win(unsigned char *buf, long long bufsize,
	int bufw, int bufh, int topdown,
	const BlurRect *regions, int count);

#ifdef __cplusplus
}
#endif

#endif
