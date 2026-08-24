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

#include "config.h"

#include "cogl/cogl-gpu-fence.h"

#include "cogl/cogl-context-private.h"
#include "cogl/cogl-gl-header.h"
#include "cogl/driver/gl/cogl-driver-gl-private.h"

/* GL_ARB_sync is core in GL 3.2 and GLES 3, but cogl's GLES2 build includes
 * only <GLES2/gl2.h> and <GLES2/gl2ext.h>, where neither GLsync nor the
 * GL_SYNC_* tokens exist. cogl-all-functions.h guards the entry points the same
 * way, so without a matching guard here this would not compile there. On such a
 * build cogl_gpu_fence_new() returns NULL and callers fall back to a
 * synchronous map. */
#ifdef GL_ARB_sync

struct _CoglGpuFence
{
  CoglContext *context;
  GLsync sync;
  /* Set once glClientWaitSync has reported completion. GL_ARB_sync fences are
   * one-shot -- once signalled they stay signalled -- so there is no reason to
   * keep asking the driver after the first positive answer. */
  gboolean signalled;
};

CoglGpuFence *
cogl_gpu_fence_new (CoglContext *context)
{
  CoglDriver *driver;
  CoglGpuFence *fence;
  GLsync sync = NULL;

  g_return_val_if_fail (COGL_IS_CONTEXT (context), NULL);

  driver = cogl_context_get_driver (context);

  /* COGL_FEATURE_ID_FENCE is also set by the EGL winsys for its own
   * EGL_KHR_fence_sync, so it alone does not promise glFenceSync exists.
   * Check the entry point itself. */
  if (!GE_HAS (driver, glFenceSync) ||
      !GE_HAS (driver, glClientWaitSync) ||
      !GE_HAS (driver, glDeleteSync))
    return NULL;

  GE_RET (sync, driver, glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
  if (!sync)
    return NULL;

  /* Submit the commands the fence marks. glFenceSync only *orders* the fence
   * behind them; it does not guarantee they ever reach the GPU. A caller that
   * renders on damage has no later frame to piggyback a flush on, so without
   * this the fence can sit unsignalled forever on an idle screen. */
  GE (driver, glFlush ());

  fence = g_new0 (CoglGpuFence, 1);
  fence->context = context;
  fence->sync = sync;
  fence->signalled = FALSE;

  return fence;
}

gboolean
cogl_gpu_fence_is_signalled (CoglGpuFence *fence)
{
  CoglDriver *driver;
  GLenum result = GL_WAIT_FAILED;

  g_return_val_if_fail (fence != NULL, FALSE);

  if (fence->signalled)
    return TRUE;

  driver = cogl_context_get_driver (fence->context);

  /* Zero timeout: poll, never block. The flush already happened in
   * cogl_gpu_fence_new(), so GL_SYNC_FLUSH_COMMANDS_BIT is not needed here --
   * and passing it on every poll would issue a redundant flush each time. */
  GE_RET (result, driver, glClientWaitSync (fence->sync, 0, 0));

  if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED)
    {
      fence->signalled = TRUE;
      return TRUE;
    }

  /* GL_WAIT_FAILED means the sync object is invalid or the context was lost.
   * Report it as signalled rather than leaving the caller polling forever: the
   * data will not arrive, and a caller blocked on this would hang. */
  if (result == GL_WAIT_FAILED)
    {
      g_warning ("cogl: glClientWaitSync failed; treating fence as signalled");
      fence->signalled = TRUE;
      return TRUE;
    }

  return FALSE;
}

void
cogl_gpu_fence_free (CoglGpuFence *fence)
{
  CoglDriver *driver;

  if (!fence)
    return;

  driver = cogl_context_get_driver (fence->context);

  GE (driver, glDeleteSync (fence->sync));
  g_free (fence);
}

#else /* !GL_ARB_sync */

CoglGpuFence *
cogl_gpu_fence_new (CoglContext *context)
{
  return NULL;
}

gboolean
cogl_gpu_fence_is_signalled (CoglGpuFence *fence)
{
  g_return_val_if_reached (TRUE);
}

void
cogl_gpu_fence_free (CoglGpuFence *fence)
{
  g_return_if_fail (fence == NULL);
}

#endif /* GL_ARB_sync */
