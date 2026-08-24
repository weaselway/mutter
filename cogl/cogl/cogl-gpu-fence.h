/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#if !defined(__COGL_H_INSIDE__) && !defined(COGL_COMPILATION)
#error "Only <cogl/cogl.h> can be included directly."
#endif

#include "cogl/cogl-context.h"

G_BEGIN_DECLS

/**
 * CoglGpuFence:
 *
 * A one-shot marker in the GPU command stream that can be polled, without
 * blocking, to ask whether every command issued before it has completed.
 *
 * The motivating use is asynchronous pixel readback: issue
 * [method@Cogl.Framebuffer.read_pixels_into_bitmap] into a pixel buffer created
 * with [func@Cogl.PixelBuffer.new_for_readback], create a fence, and only map
 * the buffer once the fence has signalled. Mapping it earlier is not wrong, but
 * it blocks the calling thread until the transfer finishes, which is the cost
 * this exists to avoid.
 *
 * This is deliberately poll-only: there is no blocking wait, because the
 * intended caller is a main loop that has other work to do.
 */
typedef struct _CoglGpuFence CoglGpuFence;

/**
 * cogl_gpu_fence_new: (skip)
 * @context: A #CoglContext
 *
 * Inserts a fence into the GPU command stream after all commands issued so far,
 * and flushes the stream so that it is guaranteed to be submitted.
 *
 * The flush matters: without it the commands the fence marks may sit
 * unsubmitted indefinitely, and the fence would then never signal. Callers that
 * render only in response to damage would deadlock waiting for a frame that is
 * not coming.
 *
 * Returns: (transfer full) (nullable): a new #CoglGpuFence, or %NULL if this
 *   driver has no `GL_ARB_sync`. Note %COGL_FEATURE_ID_FENCE is *not* the test:
 *   the EGL winsys sets it for `EGL_KHR_fence_sync`, which is a different
 *   mechanism, so the entry points are checked directly. Callers must handle
 *   %NULL by falling back to a synchronous map.
 */
COGL_EXPORT CoglGpuFence *
cogl_gpu_fence_new (CoglContext *context);

/**
 * cogl_gpu_fence_is_signalled: (skip)
 * @fence: A #CoglGpuFence
 *
 * Checks, without blocking, whether every command issued before @fence was
 * created has completed on the GPU.
 *
 * Returns: %TRUE if the fence has signalled
 */
COGL_EXPORT gboolean
cogl_gpu_fence_is_signalled (CoglGpuFence *fence);

/**
 * cogl_gpu_fence_free: (skip)
 * @fence: (transfer full): A #CoglGpuFence
 *
 * Destroys @fence. Safe to call whether or not it has signalled.
 */
COGL_EXPORT void
cogl_gpu_fence_free (CoglGpuFence *fence);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CoglGpuFence, cogl_gpu_fence_free)

G_END_DECLS
