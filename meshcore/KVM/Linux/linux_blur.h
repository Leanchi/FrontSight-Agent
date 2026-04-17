/*
 * Linux 窗口虚化 - 头文件
 */

#ifndef LINUX_BLUR_H
#define LINUX_BLUR_H

typedef struct {
    int x, y, w, h;
} BlurRect;

int get_blurred_regions(BlurRect **regions);
void free_blurred_regions(BlurRect *regions);

#endif
