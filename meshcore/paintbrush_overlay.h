#ifndef __PAINTBRUSH_OVERLAY__
#define __PAINTBRUSH_OVERLAY__

#include "../microscript/duktape.h"

#ifdef __cplusplus
extern "C" {
#endif

void paintbrush_overlay_PUSH(duk_context *ctx, void *chain);

#ifdef __cplusplus
}
#endif

#endif
