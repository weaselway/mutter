/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2008,2009,2010 Intel Corporation.
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
 *
 *
 *
 * Authors:
 *   Damien Lespiau <damien.lespiau@intel.com>
 *   Robert Bragg <robert@linux.intel.com>
 */

#pragma once

#if !defined(__COGL_H_INSIDE__) && !defined(COGL_COMPILATION)
#error "Only <cogl/cogl.h> can be included directly."
#endif

#include "cogl/cogl-types.h"
#include "cogl/cogl-context.h"

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CoglPixelBuffer:
 */
#define COGL_TYPE_PIXEL_BUFFER            (cogl_pixel_buffer_get_type ())
#define COGL_PIXEL_BUFFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), COGL_TYPE_PIXEL_BUFFER, CoglPixelBuffer))
#define COGL_PIXEL_BUFFER_CONST(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), COGL_TYPE_PIXEL_BUFFER, CoglPixelBuffer const))
#define COGL_PIXEL_BUFFER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  COGL_TYPE_PIXEL_BUFFER, CoglPixelBufferClass))
#define COGL_IS_PIXEL_BUFFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), COGL_TYPE_PIXEL_BUFFER))
#define COGL_IS_PIXEL_BUFFER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  COGL_TYPE_PIXEL_BUFFER))
#define COGL_PIXEL_BUFFER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  COGL_TYPE_PIXEL_BUFFER, CoglPixelBufferClass))

typedef struct _CoglPixelBufferClass CoglPixelBufferClass;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CoglPixelBuffer, g_object_unref)

COGL_EXPORT
GType               cogl_pixel_buffer_get_type       (void) G_GNUC_CONST;

/**
 * cogl_pixel_buffer_new:
 * @context: A #CoglContext
 * @size: The number of bytes to allocate for the pixel data.
 * @data: (array length=size) (element-type guint8): An optional pointer to
 *   vertex data to upload immediately
 *
 * Declares a new #CoglPixelBuffer of @size bytes to contain arrays of
 * pixels. Once declared, data can be set using cogl_buffer_set_data()
 * or by mapping it into the application's address space using
 * cogl_buffer_map().
 *
 * If @data isn't %NULL then @size bytes will be read from @data and
 * immediately copied into the new buffer.
 *
 * Return value: (transfer full): a newly allocated #CoglPixelBuffer
 */
COGL_EXPORT CoglPixelBuffer *
cogl_pixel_buffer_new (CoglContext *context,
                       size_t       size,
                       const void  *data);

/**
 * cogl_pixel_buffer_new_for_readback:
 * @context: A #CoglContext
 * @size: The number of bytes to allocate for the pixel data.
 *
 * Declares a new #CoglPixelBuffer of @size bytes intended as the destination
 * of [method@Cogl.Framebuffer.read_pixels_into_bitmap], i.e. bound to
 * `GL_PIXEL_PACK_BUFFER` and written by the GPU rather than by the
 * application.
 *
 * Unlike [func@Cogl.PixelBuffer.new] this asks the driver for a buffer the CPU
 * can read back cheaply (%COGL_BUFFER_USAGE_HINT_READ, `GL_STREAM_READ`). That
 * distinction decides which memory the buffer is placed in: a buffer created
 * for drawing may sit in memory that has to be copied through a staging
 * allocation and waited on before the application can read it, which defeats
 * the point of reading pixels into a buffer at all. See #CoglBufferUsageHint.
 *
 * Return value: (transfer full): a newly allocated #CoglPixelBuffer
 */
COGL_EXPORT CoglPixelBuffer *
cogl_pixel_buffer_new_for_readback (CoglContext *context,
                                    size_t       size);

G_END_DECLS
