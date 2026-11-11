#ifndef __KVM_BLUR_MODULE__
#define __KVM_BLUR_MODULE__

#include "microscript/duktape.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kvm-blur 遮罩关键字模块：纯字符串表、零图形 API，三平台共用一份实现。
 * JS: require('kvm-blur').setKeywords(['a','b']) —— 运行时替换窗口虚化关键字 */
void kvm_blur_PUSH(duk_context *ctx, void *chain);

#ifdef __cplusplus
}
#endif

#endif
