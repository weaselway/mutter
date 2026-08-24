/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * In-process RDP/VAIL output backend for mutter (WSLg).
 *
 * Task 03: stand up an in-process FreeRDP listener + peer, complete activation,
 * and integrate FreeRDP's file descriptors with mutter's GLib main loop. No
 * pixels are pushed yet (that lands in task 04) -- this proves out a live RDP
 * session: the WSLGd-launched client (msrdc) connects over the inherited vsock,
 * negotiates TLS, activates, and the session stays up. Screen stays black.
 *
 * Ported (with RAIL/audio/clipboard stripped) from
 * wslg/weston/libweston/backend-rdp/rdp.c on the Microsoft `working` fork
 * (FreeRDP 2.4.0). Weston's wl_event_loop fd wiring is replaced with GSources
 * attached to mutter's default GMainContext; everything runs single-threaded on
 * mutter's main thread.
 */

#include "config.h"

#include "backends/rdp/meta-rdp-server.h"

#include "backends/meta-backend-private.h"
#include "backends/meta-renderer.h"
#include "backends/meta-renderer-view.h"
#include "backends/meta-stage-private.h"
#include "clutter/clutter.h"
#include "cogl/cogl.h"
#include "core/meta-context-private.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

#include <freerdp/freerdp.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/update.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/version.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>

#ifdef HAVE_FREERDP_GFXREDIR_H
#include <freerdp/server/gfxredir.h>
#endif

/* From Weston's rdp.c: an upper bound on the number of FreeRDP event handles
 * (listener or per-peer, +1 for the virtual channel manager). */
#define META_RDP_MAX_FREERDP_FDS 32

#define META_RDP_DEFAULT_TCP_PORT 3389

/* Single fullscreen desktop window (see rdp.h RDP_RAIL_DESKTOP_WINDOW_ID). */
#define META_RDP_DESKTOP_WINDOW_ID 0xFFFFFFFF
#define META_RDP_POOL_ID 1
#define META_RDP_BUFFER_ID 1

/* GUID form: "{...}" = 32 hex + 4 dashes + 2 braces. */
#define META_RDP_SHARED_MEMORY_NAME_SIZE (32 + 4 + 2)

typedef struct _MetaRdpWatchedView
{
  MetaRdpServer *server;
  ClutterStageView *view;
  MetaStageWatch *paint_watch;
  gulong destroy_handler_id;
} MetaRdpWatchedView;

/* Per-peer context. FreeRDP allocates this inline in the peer (ContextSize),
 * so it MUST begin with rdpContext. */
typedef struct _MetaRdpPeerContext
{
  rdpContext rdp_context;

  MetaRdpServer *server;
  freerdp_peer *peer;

  HANDLE vcm;

  /* GSources bridging this peer's FreeRDP fds into the GLib main loop. */
  GSource *fd_sources[META_RDP_MAX_FREERDP_FDS];
  int n_fd_sources;

  gboolean activated;

  /* Fallback (codec) present path: NSCodec / raw over the wire. */
  NSC_CONTEXT *nsc_context;
  wStream *encode_stream;

#ifdef HAVE_FREERDP_GFXREDIR_H
  /* Fast path: gfxredir shared-memory present. */
  GfxRedirServerContext *gfxredir;
  gboolean gfxredir_activated; /* caps confirmed */
  gboolean use_gfxredir;       /* shared-memory mount available */

  /* One pool + one buffer for the whole desktop. */
  gboolean buffer_created;
  int buffer_width;
  int buffer_height;
  int buffer_stride;

  /* Named shared-memory file backing the buffer. */
  int shm_fd;
  void *shm_addr;
  size_t shm_size;
  char shm_name[META_RDP_SHARED_MEMORY_NAME_SIZE + 1];

  /* Exactly one outstanding present; coalesce to the latest frame. */
  gboolean update_pending;
  gboolean frame_missed; /* a frame arrived while a present was pending */
  uint64_t current_frame_id;
#endif /* HAVE_FREERDP_GFXREDIR_H */

  GList *link; /* node in server->peers */
} MetaRdpPeerContext;

struct _MetaRdpServer
{
  GObject parent;

  MetaBackend *backend;

  gulong started_handler_id;
  gulong monitors_changed_handler_id;

  gboolean views_attached;
  GList *watched_views; /* MetaRdpWatchedView* */

  uint64_t frame_counter;

  /* FreeRDP listener + its GLib fd bridge. */
  freerdp_listener *listener;
  GSource *listener_fd_sources[META_RDP_MAX_FREERDP_FDS];
  int n_listener_fd_sources;
  int owned_listen_fd; /* vsock fd we created ourselves, or -1 */

  /* Throwaway self-signed TLS material generated at startup. */
  char *cert_dir;
  char *cert_file;
  char *key_file;

  /* virtio-fs/DAX shared-memory mount for the gfxredir fast path, or NULL. */
  char *shared_memory_mount_path;

  GList *peers; /* MetaRdpPeerContext* */
};

G_DEFINE_FINAL_TYPE (MetaRdpServer, meta_rdp_server, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ */
/* Task 04: pixel readback + present (gfxredir fast path / codec fallback) */
/* ------------------------------------------------------------------ */

static MetaStage *
meta_rdp_server_get_stage (MetaRdpServer *self)
{
  return META_STAGE (meta_backend_get_stage (self->backend));
}

/* Read the whole framebuffer into @dest (ARGB8888, i.e. BGRA byte order in
 * memory, which is what both NSCodec's PIXEL_FORMAT_BGRA32 and gfxredir's
 * ARGB_8888 expect on little-endian). Cogl reads with a bottom-left origin
 * (OpenGL convention) whereas RDP wants a top-left origin, so we flip the
 * image vertically here. Returns FALSE on failure. */
static gboolean
meta_rdp_read_framebuffer (CoglFramebuffer *framebuffer,
                           uint8_t         *dest,
                           int              width,
                           int              height,
                           int              stride)
{
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  CoglBitmap *bitmap;
  g_autofree uint8_t *tmp = NULL;
  gboolean ok;
  int y;

  tmp = g_malloc ((size_t) stride * height);
  bitmap = cogl_bitmap_new_for_data (cogl_context,
                                     width, height,
                                     COGL_PIXEL_FORMAT_BGRA_8888_PRE,
                                     stride,
                                     tmp);
  ok = cogl_framebuffer_read_pixels_into_bitmap (framebuffer,
                                                 0, 0,
                                                 COGL_READ_PIXELS_COLOR_BUFFER,
                                                 bitmap);
  g_object_unref (bitmap);

  if (!ok)
    return FALSE;

#if 0
  /* Vertical flip: GL framebuffers are bottom-up, so historically we flipped
   * rows here. In practice the RDP output came out mirrored, so this is
   * disabled and we copy rows straight through. */
  for (y = 0; y < height; y++)
    {
      memcpy (dest + (size_t) y * stride,
              tmp + (size_t) (height - 1 - y) * stride,
              (size_t) stride);
    }
#else
  (void) y;
  memcpy (dest, tmp, (size_t) stride * height);
#endif

  return TRUE;
}

/* ---- Codec fallback path (NSCodec / raw over the wire) ---- */

static void
meta_rdp_present_codec (MetaRdpPeerContext *peer_ctx,
                        CoglFramebuffer    *framebuffer,
                        const MtkRectangle *damage)
{
  freerdp_peer *client = peer_ctx->peer;
  rdpUpdate *update = client->context->update;
  rdpSettings *settings = client->context->settings;
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);
  int stride = width * 4;
  g_autofree uint8_t *pixels = NULL;
  SURFACE_BITS_COMMAND cmd = { 0 };
  MtkRectangle rect;

  /* Clip damage to the framebuffer bounds. */
  rect = *damage;

  /* NSCodec subsamples chroma 2x2, so it requires even-aligned rectangles;
   * an odd origin/size shifts the decoded image by a pixel (seen as a 1px
   * wobble on partial updates such as focus shadows). Snap to even bounds. */
  if (rect.x & 1) { rect.x -= 1; rect.width += 1; }
  if (rect.y & 1) { rect.y -= 1; rect.height += 1; }
  if (rect.width & 1) rect.width += 1;
  if (rect.height & 1) rect.height += 1;

  if (rect.x < 0) { rect.width += rect.x; rect.x = 0; }
  if (rect.y < 0) { rect.height += rect.y; rect.y = 0; }
  if (rect.x + rect.width > width)
    rect.width = width - rect.x;
  if (rect.y + rect.height > height)
    rect.height = height - rect.y;
  if (rect.width <= 0 || rect.height <= 0)
    return;

  pixels = g_malloc ((size_t) stride * height);
  if (!meta_rdp_read_framebuffer (framebuffer, pixels, width, height, stride))
    {
      g_warning ("rdp: framebuffer readback failed (codec path)");
      return;
    }

  cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
  cmd.destLeft = rect.x;
  cmd.destTop = rect.y;
  cmd.destRight = rect.x + rect.width;
  cmd.destBottom = rect.y + rect.height;
  cmd.bmp.bpp = 32;
  cmd.bmp.width = rect.width;
  cmd.bmp.height = rect.height;

  if (settings->NSCodec && peer_ctx->nsc_context && peer_ctx->encode_stream)
    {
      const uint8_t *ptr = pixels + (size_t) rect.y * stride + rect.x * 4;

      Stream_Clear (peer_ctx->encode_stream);
      Stream_SetPosition (peer_ctx->encode_stream, 0);

      nsc_compose_message (peer_ctx->nsc_context, peer_ctx->encode_stream,
                           (BYTE *) ptr, rect.width, rect.height, stride);

      cmd.skipCompression = TRUE;
      cmd.bmp.codecID = settings->NSCodecId;
      cmd.bmp.bitmapDataLength = Stream_GetPosition (peer_ctx->encode_stream);
      cmd.bmp.bitmapData = Stream_Buffer (peer_ctx->encode_stream);

      update->SurfaceBits (update->context, &cmd);
    }
  else
    {
      /* Raw: copy the damage sub-rect tightly. */
      g_autofree uint8_t *sub = NULL;
      int y;

      sub = g_malloc ((size_t) rect.width * rect.height * 4);
      for (y = 0; y < rect.height; y++)
        {
          memcpy (sub + (size_t) y * rect.width * 4,
                  pixels + (size_t) (rect.y + y) * stride + rect.x * 4,
                  (size_t) rect.width * 4);
        }

      cmd.bmp.codecID = 0;
      cmd.bmp.bitmapDataLength = rect.width * rect.height * 4;
      cmd.bmp.bitmapData = sub;
      update->SurfaceBits (update->context, &cmd);
    }
}

#ifdef HAVE_FREERDP_GFXREDIR_H
/* ---- gfxredir shared-memory fast path ---- */

static void
meta_rdp_free_shared_memory (MetaRdpPeerContext *peer_ctx)
{
  if (peer_ctx->shm_addr && peer_ctx->shm_addr != MAP_FAILED)
    munmap (peer_ctx->shm_addr, peer_ctx->shm_size);
  peer_ctx->shm_addr = NULL;
  if (peer_ctx->shm_fd >= 0)
    {
      char path[PATH_MAX];

      close (peer_ctx->shm_fd);
      if (peer_ctx->server->shared_memory_mount_path && peer_ctx->shm_name[0])
        {
          g_snprintf (path, sizeof (path), "%s/%s",
                      peer_ctx->server->shared_memory_mount_path,
                      peer_ctx->shm_name);
          unlink (path);
        }
    }
  peer_ctx->shm_fd = -1;
  peer_ctx->shm_size = 0;
  peer_ctx->shm_name[0] = '\0';
}

/* Port of rdp_allocate_shared_memory(): GUID-named file on the shared mount. */
static gboolean
meta_rdp_allocate_shared_memory (MetaRdpPeerContext *peer_ctx,
                                 size_t              size)
{
  MetaRdpServer *self = peer_ctx->server;
  char path[PATH_MAX];
  int fd_uuid, fd = -1;
  void *addr = NULL;

  peer_ctx->shm_name[0] = '\0';

  fd_uuid = open ("/proc/sys/kernel/random/uuid", O_RDONLY);
  if (fd_uuid < 0)
    {
      g_warning ("rdp: open uuid failed: %s", g_strerror (errno));
      return FALSE;
    }
  /* 32 hex + 4 dashes into name[1..36]. */
  if (read (fd_uuid, &peer_ctx->shm_name[1], 32 + 4) != 32 + 4)
    {
      g_warning ("rdp: read uuid failed: %s", g_strerror (errno));
      close (fd_uuid);
      return FALSE;
    }
  close (fd_uuid);
  peer_ctx->shm_name[0] = '{';
  peer_ctx->shm_name[META_RDP_SHARED_MEMORY_NAME_SIZE - 1] = '}';
  peer_ctx->shm_name[META_RDP_SHARED_MEMORY_NAME_SIZE] = '\0';

  g_snprintf (path, sizeof (path), "%s/%s",
              self->shared_memory_mount_path, peer_ctx->shm_name);

  fd = open (path, O_CREAT | O_RDWR | O_EXCL, S_IWUSR | S_IRUSR);
  if (fd < 0)
    {
      g_warning ("rdp: open shm \"%s\" failed: %s", path, g_strerror (errno));
      goto error;
    }

  if (fallocate (fd, 0, 0, size) < 0)
    {
      g_warning ("rdp: fallocate shm failed: %s", g_strerror (errno));
      goto error;
    }

  addr = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
    {
      g_warning ("rdp: mmap shm failed: %s", g_strerror (errno));
      goto error;
    }

  peer_ctx->shm_fd = fd;
  peer_ctx->shm_addr = addr;
  peer_ctx->shm_size = size;
  g_message ("rdp: allocated shared memory %s (%zu bytes) at %p",
             peer_ctx->shm_name, size, addr);
  return TRUE;

error:
  if (fd >= 0)
    close (fd);
  peer_ctx->shm_name[0] = '\0';
  return FALSE;
}

static void
meta_rdp_destroy_buffer (MetaRdpPeerContext *peer_ctx)
{
  GfxRedirServerContext *redir = peer_ctx->gfxredir;

  if (!peer_ctx->buffer_created)
    return;

  if (redir)
    {
      GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer = { 0 };
      GFXREDIR_CLOSE_POOL_PDU close_pool = { 0 };

      destroy_buffer.bufferId = META_RDP_BUFFER_ID;
      redir->DestroyBuffer (redir, &destroy_buffer);

      close_pool.poolId = META_RDP_POOL_ID;
      redir->ClosePool (redir, &close_pool);
    }

  meta_rdp_free_shared_memory (peer_ctx);
  peer_ctx->buffer_created = FALSE;
  peer_ctx->update_pending = FALSE;
}

/* Create the single pool+buffer sized to the output (once / on resize). */
static gboolean
meta_rdp_ensure_buffer (MetaRdpPeerContext *peer_ctx,
                        int                 width,
                        int                 height)
{
  GfxRedirServerContext *redir = peer_ctx->gfxredir;
  int stride = width * 4;
  size_t size = (size_t) stride * height;
  unsigned short section_name[META_RDP_SHARED_MEMORY_NAME_SIZE + 1];
  GFXREDIR_OPEN_POOL_PDU open_pool = { 0 };
  GFXREDIR_CREATE_BUFFER_PDU create_buffer = { 0 };
  uint32_t i;

  if (peer_ctx->buffer_created &&
      peer_ctx->buffer_width == width && peer_ctx->buffer_height == height)
    return TRUE;

  if (peer_ctx->buffer_created)
    meta_rdp_destroy_buffer (peer_ctx);

  if (!meta_rdp_allocate_shared_memory (peer_ctx, size))
    return FALSE;

  /* Linux wchar_t is 4 bytes; Windows wants 2-byte wchar for sectionName. */
  for (i = 0; i < META_RDP_SHARED_MEMORY_NAME_SIZE; i++)
    section_name[i] = (unsigned short) peer_ctx->shm_name[i];
  section_name[META_RDP_SHARED_MEMORY_NAME_SIZE] = 0;

  open_pool.poolId = META_RDP_POOL_ID;
  open_pool.poolSize = size;
  open_pool.sectionNameLength = META_RDP_SHARED_MEMORY_NAME_SIZE + 1;
  open_pool.sectionName = section_name;
  if (redir->OpenPool (redir, &open_pool) != 0)
    {
      g_warning ("rdp: gfxredir OpenPool failed");
      meta_rdp_free_shared_memory (peer_ctx);
      return FALSE;
    }

  create_buffer.poolId = META_RDP_POOL_ID;
  create_buffer.bufferId = META_RDP_BUFFER_ID;
  create_buffer.offset = 0;
  create_buffer.stride = stride;
  create_buffer.width = width;
  create_buffer.height = height;
  create_buffer.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
  if (redir->CreateBuffer (redir, &create_buffer) != 0)
    {
      GFXREDIR_CLOSE_POOL_PDU close_pool = { 0 };

      g_warning ("rdp: gfxredir CreateBuffer failed");
      close_pool.poolId = META_RDP_POOL_ID;
      redir->ClosePool (redir, &close_pool);
      meta_rdp_free_shared_memory (peer_ctx);
      return FALSE;
    }

  peer_ctx->buffer_created = TRUE;
  peer_ctx->buffer_width = width;
  peer_ctx->buffer_height = height;
  peer_ctx->buffer_stride = stride;
  g_message ("rdp: gfxredir buffer created %dx%d (stride %d)",
             width, height, stride);
  return TRUE;
}

static void
meta_rdp_present_gfxredir (MetaRdpPeerContext *peer_ctx,
                           CoglFramebuffer    *framebuffer,
                           const MtkRectangle *damage)
{
  GfxRedirServerContext *redir = peer_ctx->gfxredir;
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);
  GFXREDIR_PRESENT_BUFFER_PDU present = { 0 };
  RECTANGLE_32 opaque_rect;
  MtkRectangle rect;

  if (!meta_rdp_ensure_buffer (peer_ctx, width, height))
    return;

  /* Read the whole composited frame into the shared buffer. Damage-clipping
   * the read_pixels is a task-07 optimization; for correctness we read all. */
  if (!meta_rdp_read_framebuffer (framebuffer, peer_ctx->shm_addr,
                                  width, height, peer_ctx->buffer_stride))
    {
      g_warning ("rdp: framebuffer readback failed (gfxredir path)");
      return;
    }

  /* Clip damage to bounds. */
  rect = *damage;
  if (rect.x < 0) { rect.width += rect.x; rect.x = 0; }
  if (rect.y < 0) { rect.height += rect.y; rect.y = 0; }
  if (rect.x + rect.width > width)
    rect.width = width - rect.x;
  if (rect.y + rect.height > height)
    rect.height = height - rect.y;
  if (rect.width <= 0 || rect.height <= 0)
    {
      rect.x = 0;
      rect.y = 0;
      rect.width = width;
      rect.height = height;
    }

  opaque_rect.left = rect.x;
  opaque_rect.top = rect.y;
  opaque_rect.width = rect.width;
  opaque_rect.height = rect.height;

  present.timestamp = 0; /* disable A/V sync at client side */
  present.presentId = ++peer_ctx->current_frame_id;
  present.windowId = META_RDP_DESKTOP_WINDOW_ID;
  present.bufferId = META_RDP_BUFFER_ID;
  present.orientation = 0;
  present.targetWidth = width;
  present.targetHeight = height;
  present.dirtyRect.left = rect.x;
  present.dirtyRect.top = rect.y;
  present.dirtyRect.width = rect.width;
  present.dirtyRect.height = rect.height;
  present.numOpaqueRects = 1;
  present.opaqueRects = &opaque_rect;

  if (redir->PresentBuffer (redir, &present) == 0)
    {
      peer_ctx->update_pending = TRUE;
    }
  else
    {
      g_warning ("rdp: gfxredir PresentBuffer failed");
    }
}
#endif /* HAVE_FREERDP_GFXREDIR_H */

/* Present the current frame to one peer, choosing fast path or fallback. */
static void
meta_rdp_peer_present (MetaRdpPeerContext *peer_ctx,
                       CoglFramebuffer    *framebuffer,
                       const MtkRectangle *damage)
{
  if (!peer_ctx->activated)
    return;

#ifdef HAVE_FREERDP_GFXREDIR_H
  if (peer_ctx->use_gfxredir)
    {
      if (!peer_ctx->gfxredir_activated)
        return;
      if (peer_ctx->update_pending)
        {
          /* Exactly one outstanding present; coalesce to the latest. */
          peer_ctx->frame_missed = TRUE;
          return;
        }
      meta_rdp_present_gfxredir (peer_ctx, framebuffer, damage);
      return;
    }
#endif

  meta_rdp_present_codec (peer_ctx, framebuffer, damage);
}

/* Present the current composited contents immediately as a full frame. Used to
 * fill the client's screen on connect/activation instead of waiting for the
 * first damage event (mutter only repaints on damage). */
static void
meta_rdp_peer_force_full_present (MetaRdpPeerContext *peer_ctx)
{
  MetaRdpServer *self = peer_ctx->server;
  GList *l;

  for (l = self->watched_views; l; l = l->next)
    {
      MetaRdpWatchedView *watched = l->data;
      CoglFramebuffer *fb;
      MtkRectangle full;

      if (!watched->view)
        continue;

      fb = clutter_stage_view_get_framebuffer (watched->view);
      full = (MtkRectangle) { 0, 0,
                              cogl_framebuffer_get_width (fb),
                              cogl_framebuffer_get_height (fb) };
      meta_rdp_peer_present (peer_ctx, fb, &full);
      break;
    }
}

static void
on_frame_ready (MetaStage        *stage,
                ClutterStageView *view,
                const MtkRegion  *redraw_clip,
                ClutterFrame     *frame,
                gpointer          user_data)
{
  MetaRdpWatchedView *watched = user_data;
  MetaRdpServer *self = watched->server;
  CoglFramebuffer *framebuffer;
  int width, height;
  MtkRectangle damage;
  GList *l;

  framebuffer = clutter_stage_view_get_framebuffer (view);
  width = cogl_framebuffer_get_width (framebuffer);
  height = cogl_framebuffer_get_height (framebuffer);

  if (redraw_clip && !mtk_region_is_empty (redraw_clip))
    damage = mtk_region_get_extents (redraw_clip);
  else
    damage = (MtkRectangle) { 0, 0, width, height };

  self->frame_counter++;

  for (l = self->peers; l; l = l->next)
    meta_rdp_peer_present (l->data, framebuffer, &damage);
}

static void meta_rdp_server_detach_views (MetaRdpServer *self);

static void
on_watched_view_destroyed (gpointer  user_data,
                           GObject  *where_the_object_was)
{
  MetaRdpWatchedView *watched = user_data;
  MetaRdpServer *self = watched->server;

  watched->view = NULL;
  watched->destroy_handler_id = 0;
  self->watched_views = g_list_remove (self->watched_views, watched);
  g_free (watched);
}

static void
meta_rdp_server_attach_views (MetaRdpServer *self)
{
  MetaRenderer *renderer = meta_backend_get_renderer (self->backend);
  MetaStage *stage = meta_rdp_server_get_stage (self);
  GList *views;
  GList *l;

  if (self->views_attached)
    meta_rdp_server_detach_views (self);

  views = meta_renderer_get_views (renderer);
  if (!views)
    {
      g_warning ("rdp: no renderer views yet; waiting for a virtual monitor");
      return;
    }

  for (l = views; l; l = l->next)
    {
      ClutterStageView *view = CLUTTER_STAGE_VIEW (l->data);
      MetaRdpWatchedView *watched;

      watched = g_new0 (MetaRdpWatchedView, 1);
      watched->server = self;
      watched->view = view;
      watched->paint_watch =
        meta_stage_watch_view (stage, view,
                               META_STAGE_WATCH_AFTER_PAINT,
                               on_frame_ready,
                               watched);
      watched->destroy_handler_id = 1;
      g_object_weak_ref (G_OBJECT (view), on_watched_view_destroyed, watched);

      self->watched_views = g_list_prepend (self->watched_views, watched);

      g_message ("rdp: attached to stage view %p", view);
    }

  self->views_attached = TRUE;
}

static void
meta_rdp_server_detach_views (MetaRdpServer *self)
{
  MetaStage *stage = meta_rdp_server_get_stage (self);
  GList *l;

  for (l = self->watched_views; l; l = l->next)
    {
      MetaRdpWatchedView *watched = l->data;

      if (watched->view && watched->paint_watch)
        meta_stage_remove_watch (stage, watched->paint_watch);
      if (watched->view && watched->destroy_handler_id)
        g_object_weak_unref (G_OBJECT (watched->view),
                             on_watched_view_destroyed, watched);
      g_free (watched);
    }

  g_clear_pointer (&self->watched_views, g_list_free);
  self->views_attached = FALSE;
}

static void
on_monitors_changed (MetaMonitorManager *monitor_manager,
                     MetaRdpServer      *self)
{
  g_message ("rdp: monitors changed, re-scanning views");
  meta_rdp_server_attach_views (self);
}

/* ------------------------------------------------------------------ */
/* Task 03: FreeRDP fd <-> GLib main loop bridge                      */
/* ------------------------------------------------------------------ */

typedef gboolean (*MetaRdpFdCheck) (gpointer data);

typedef struct _MetaRdpFdSource
{
  GSource source;
  gpointer fd_tag;
  MetaRdpFdCheck check;
  gpointer data;
} MetaRdpFdSource;

static gboolean
meta_rdp_fd_source_dispatch (GSource     *source,
                             GSourceFunc  callback,
                             gpointer     user_data)
{
  MetaRdpFdSource *fd_source = (MetaRdpFdSource *) source;
  GIOCondition revents;

  revents = g_source_query_unix_fd (source, fd_source->fd_tag);
  if (revents & (G_IO_IN | G_IO_HUP | G_IO_ERR))
    {
      if (!fd_source->check (fd_source->data))
        return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static GSourceFuncs meta_rdp_fd_source_funcs = {
  .dispatch = meta_rdp_fd_source_dispatch,
};

static GSource *
meta_rdp_add_fd_source (int             fd,
                        MetaRdpFdCheck  check,
                        gpointer        data)
{
  GSource *source;
  MetaRdpFdSource *fd_source;

  source = g_source_new (&meta_rdp_fd_source_funcs, sizeof (MetaRdpFdSource));
  fd_source = (MetaRdpFdSource *) source;
  fd_source->check = check;
  fd_source->data = data;
  fd_source->fd_tag =
    g_source_add_unix_fd (source, fd, G_IO_IN | G_IO_HUP | G_IO_ERR);

  g_source_set_name (source, "[mutter] RDP fd");
  g_source_attach (source, NULL);
  g_source_unref (source);

  return source;
}

/* ------------------------------------------------------------------ */
/* Task 03: peer context + callbacks                                  */
/* ------------------------------------------------------------------ */

static void
meta_rdp_peer_remove_fd_sources (MetaRdpPeerContext *peer_ctx)
{
  int i;

  for (i = 0; i < peer_ctx->n_fd_sources; i++)
    {
      if (peer_ctx->fd_sources[i])
        {
          g_source_destroy (peer_ctx->fd_sources[i]);
          peer_ctx->fd_sources[i] = NULL;
        }
    }
  peer_ctx->n_fd_sources = 0;
}

static void
meta_rdp_peer_destroy (MetaRdpPeerContext *peer_ctx)
{
  freerdp_peer *peer = peer_ctx->peer;
  MetaRdpServer *self = peer_ctx->server;

  g_message ("rdp: peer %p disconnected", peer);

  meta_rdp_peer_remove_fd_sources (peer_ctx);

  self->peers = g_list_remove (self->peers, peer_ctx);

  peer->Disconnect (peer);
  freerdp_peer_context_free (peer);
  freerdp_peer_free (peer);
}

#ifdef HAVE_FREERDP_GFXREDIR_H
/* ---- gfxredir caps negotiation callbacks (ported from rdprail.c) ---- */

static UINT
gfxredir_legacy_caps (GfxRedirServerContext           *context,
                      const GFXREDIR_LEGACY_CAPS_PDU   *caps)
{
  /* Legacy version 1 client is not supported: leave gfxredir_activated FALSE. */
  g_message ("rdp: gfxredir legacy caps v%d (v1 unsupported)", caps->version);
  return CHANNEL_RC_OK;
}

static UINT
gfxredir_caps_advertise (GfxRedirServerContext              *context,
                         const GFXREDIR_CAPS_ADVERTISE_PDU  *advertise)
{
  MetaRdpPeerContext *peer_ctx = context->custom;
  const GFXREDIR_CAPS_HEADER *current;
  const GFXREDIR_CAPS_HEADER *selected = NULL;
  uint32_t selected_version = 0;
  uint32_t length;

  current = (const GFXREDIR_CAPS_HEADER *) advertise->caps;
  length = advertise->length;
  while (length <= advertise->length && length >= sizeof (GFXREDIR_CAPS_HEADER))
    {
      if (current->signature != GFXREDIR_CAPS_SIGNATURE)
        return ERROR_INVALID_DATA;
      if (current->version >= selected_version)
        {
          selected = current;
          selected_version = current->version;
        }
      length -= current->length;
      current = (const GFXREDIR_CAPS_HEADER *) ((BYTE *) current + current->length);
    }

  if (selected && selected_version >= GFXREDIR_CAPS_VERSION2_0)
    {
      GFXREDIR_CAPS_CONFIRM_PDU confirm = { 0 };

      confirm.version = selected->version;
      confirm.length = selected->length;
      confirm.capsData = (const BYTE *) (selected + 1);
      context->GraphicsRedirectionCapsConfirm (context, &confirm);

      peer_ctx->gfxredir_activated = TRUE;
      g_message ("rdp: gfxredir activated (caps v0x%x)", selected->version);

      /* Fill the client's screen immediately rather than waiting for the
       * first damage event. */
      meta_rdp_peer_force_full_present (peer_ctx);
    }
  else
    {
      g_warning ("rdp: gfxredir client advertised no v2.0+ caps");
    }

  return CHANNEL_RC_OK;
}

static UINT
gfxredir_present_buffer_ack (GfxRedirServerContext                 *context,
                             const GFXREDIR_PRESENT_BUFFER_ACK_PDU *ack)
{
  MetaRdpPeerContext *peer_ctx = context->custom;

  if (ack->windowId == META_RDP_DESKTOP_WINDOW_ID)
    {
      peer_ctx->update_pending = FALSE;

      /* If a frame arrived while a present was outstanding, present the latest
       * now (coalesce -- never queue more than one). */
      if (peer_ctx->frame_missed)
        {
          MetaRdpServer *self = peer_ctx->server;
          GList *l;

          peer_ctx->frame_missed = FALSE;
          for (l = self->watched_views; l; l = l->next)
            {
              MetaRdpWatchedView *watched = l->data;
              CoglFramebuffer *fb;
              MtkRectangle full;

              if (!watched->view)
                continue;
              fb = clutter_stage_view_get_framebuffer (watched->view);
              full = (MtkRectangle) { 0, 0,
                                      cogl_framebuffer_get_width (fb),
                                      cogl_framebuffer_get_height (fb) };
              meta_rdp_present_gfxredir (peer_ctx, fb, &full);
              break;
            }
        }
    }

  return CHANNEL_RC_OK;
}

static void
meta_rdp_setup_gfxredir (MetaRdpPeerContext *peer_ctx)
{
  MetaRdpServer *self = peer_ctx->server;
  GfxRedirServerContext *redir;

  if (!self->shared_memory_mount_path)
    {
      g_message ("rdp: no shared-memory mount; using codec fallback path");
      return;
    }

  if (!peer_ctx->vcm)
    {
      g_warning ("rdp: no vcm; cannot set up gfxredir");
      return;
    }

  redir = gfxredir_server_context_new (peer_ctx->vcm);
  if (!redir)
    {
      g_warning ("rdp: gfxredir_server_context_new failed");
      return;
    }

  redir->custom = peer_ctx;
  redir->GraphicsRedirectionLegacyCaps = gfxredir_legacy_caps;
  redir->GraphicsRedirectionCapsAdvertise = gfxredir_caps_advertise;
  redir->PresentBufferAck = gfxredir_present_buffer_ack;

  if (redir->Open (redir) != CHANNEL_RC_OK)
    {
      g_warning ("rdp: gfxredir Open failed");
      gfxredir_server_context_free (redir);
      return;
    }

  peer_ctx->gfxredir = redir;
  peer_ctx->use_gfxredir = TRUE;
  g_message ("rdp: gfxredir channel opened; awaiting caps advertise");
}
#endif /* HAVE_FREERDP_GFXREDIR_H */

static BOOL
xf_peer_capabilities (freerdp_peer *client)
{
  return TRUE;
}

static BOOL
xf_peer_post_connect (freerdp_peer *client)
{
  return TRUE;
}

static BOOL
xf_peer_activate (freerdp_peer *client)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) client->context;
  rdpSettings *settings = client->context->settings;

  g_message ("rdp: peer %p activated: %ux%u, color depth %u, "
             "SurfaceCommands=%d, RemoteFxCodec=%d, NSCodec=%d, "
             "GfxPipeline=%d",
             client,
             settings->DesktopWidth, settings->DesktopHeight,
             settings->ColorDepth,
             settings->SurfaceCommandsEnabled,
             settings->RemoteFxCodec,
             settings->NSCodec,
             settings->SupportGraphicsPipeline);

  if (!settings->SurfaceCommandsEnabled)
    {
      g_warning ("rdp: client doesn't support required SurfaceCommands");
      return FALSE;
    }

  /* Task 03 deliverable: session stays up, screen stays black. We just log the
   * requested resolution here. Resizing the virtual monitor to the client's
   * DesktopWidth/Height is deferred (task 02 already created a fixed-size
   * virtual monitor via --virtual-monitor; resize is a follow-up). If the
   * sizes differ we simply keep our own and let the client scale. */
  if (peer_ctx->activated)
    return TRUE;

  peer_ctx->activated = TRUE;
  g_message ("rdp: first activation complete for peer %p", client);

#ifdef HAVE_FREERDP_GFXREDIR_H
  meta_rdp_setup_gfxredir (peer_ctx);
  if (peer_ctx->use_gfxredir)
    {
      /* gfxredir isn't ready yet; the full present is forced from
       * gfxredir_caps_advertise() once caps are confirmed. */
      return TRUE;
    }
#endif

  /* Codec fallback: fill the screen now. */
  meta_rdp_peer_force_full_present (peer_ctx);

  return TRUE;
}

static gboolean
rdp_client_activity (gpointer data)
{
  freerdp_peer *client = data;
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) client->context;

  if (!client->CheckFileDescriptor (client))
    {
      g_message ("rdp: CheckFileDescriptor failed for peer %p", client);
      goto out_clean;
    }

  if (peer_ctx->vcm)
    {
      if (!WTSVirtualChannelManagerCheckFileDescriptor (peer_ctx->vcm))
        {
          g_message ("rdp: WTS VC CheckFileDescriptor failed for peer %p",
                     client);
          goto out_clean;
        }
    }

  return TRUE;

out_clean:
  meta_rdp_peer_destroy (peer_ctx);
  return FALSE;
}

static BOOL
rdp_peer_context_new (freerdp_peer *client, rdpContext *context)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) context;

  peer_ctx->peer = client;
  peer_ctx->n_fd_sources = 0;
  peer_ctx->vcm = NULL;

  /* Codec fallback encoder. */
  peer_ctx->nsc_context = nsc_context_new ();
  if (peer_ctx->nsc_context)
    {
      nsc_context_set_parameters (peer_ctx->nsc_context, NSC_COLOR_FORMAT,
                                  PIXEL_FORMAT_BGRA32);
      peer_ctx->encode_stream = Stream_New (NULL, 65536);
    }

#ifdef HAVE_FREERDP_GFXREDIR_H
  peer_ctx->shm_fd = -1;
#endif

  return TRUE;
}

static void
rdp_peer_context_free (freerdp_peer *client, rdpContext *context)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) context;

  if (!peer_ctx)
    return;

  meta_rdp_peer_remove_fd_sources (peer_ctx);

#ifdef HAVE_FREERDP_GFXREDIR_H
  meta_rdp_destroy_buffer (peer_ctx);
  if (peer_ctx->gfxredir)
    {
      peer_ctx->gfxredir->Close (peer_ctx->gfxredir);
      gfxredir_server_context_free (peer_ctx->gfxredir);
      peer_ctx->gfxredir = NULL;
    }
#endif

  if (peer_ctx->encode_stream)
    {
      Stream_Free (peer_ctx->encode_stream, TRUE);
      peer_ctx->encode_stream = NULL;
    }
  if (peer_ctx->nsc_context)
    {
      nsc_context_free (peer_ctx->nsc_context);
      peer_ctx->nsc_context = NULL;
    }

  if (peer_ctx->vcm)
    {
      WTSCloseServer (peer_ctx->vcm);
      peer_ctx->vcm = NULL;
    }
}

static int
rdp_peer_init (freerdp_peer *client, MetaRdpServer *self)
{
  MetaRdpPeerContext *peer_ctx;
  rdpSettings *settings;
  rdpInput *input;
  HANDLE handles[META_RDP_MAX_FREERDP_FDS + 1];
  int handle_count;
  int i;

  client->ContextSize = sizeof (MetaRdpPeerContext);
  client->ContextNew = (psPeerContextNew) rdp_peer_context_new;
  client->ContextFree = (psPeerContextFree) rdp_peer_context_free;

  if (!freerdp_peer_context_new (client))
    {
      g_warning ("rdp: freerdp_peer_context_new failed");
      return -1;
    }

  peer_ctx = (MetaRdpPeerContext *) client->context;
  peer_ctx->server = self;

  settings = client->context->settings;

  /* TLS: use the throwaway self-signed pair we generated at startup. NLA is
   * disabled to match wslg_desktop.rdp (authentication level:i:0). */
  if (self->cert_file && self->key_file)
    {
      settings->CertificateFile = strdup (self->cert_file);
      settings->PrivateKeyFile = strdup (self->key_file);
      settings->TlsSecurity = TRUE;
    }
  else
    {
      settings->TlsSecurity = FALSE;
    }
  settings->RdpSecurity = TRUE;
  settings->NlaSecurity = FALSE;

  if (!client->Initialize (client))
    {
      g_warning ("rdp: peer Initialize failed");
      goto error;
    }

  settings->OsMajorType = OSMAJORTYPE_UNIX;
  settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
  settings->ColorDepth = 32;
  settings->RefreshRect = TRUE;
  settings->RemoteFxCodec = FALSE;
  settings->NSCodec = TRUE;
  settings->FrameMarkerCommandEnabled = TRUE;
  settings->SurfaceFrameMarkerEnabled = TRUE;
  /* v1: plain fullscreen desktop, not RAIL. */
  settings->RemoteApplicationMode = FALSE;
  settings->SupportGraphicsPipeline = TRUE;
  settings->SupportMonitorLayoutPdu = TRUE;
  settings->HasExtendedMouseEvent = TRUE;
  settings->HasHorizontalWheel = TRUE;

  client->Capabilities = xf_peer_capabilities;
  client->PostConnect = xf_peer_post_connect;
  client->Activate = xf_peer_activate;

  /* Input callbacks land in task 05. */
  input = client->context->input;
  (void) input;

  handle_count = client->GetEventHandles (client, handles,
                                          META_RDP_MAX_FREERDP_FDS);
  if (!handle_count)
    {
      g_warning ("rdp: unable to retrieve peer event handles");
      goto error;
    }

  {
    PWtsApiFunctionTable fn = FreeRDP_InitWtsApi ();

    WTSRegisterWtsApiFunctionTable (fn);
    peer_ctx->vcm = WTSOpenServerA ((LPSTR) peer_ctx);
    if (peer_ctx->vcm && peer_ctx->vcm != INVALID_HANDLE_VALUE)
      {
        handles[handle_count++] =
          WTSVirtualChannelManagerGetEventHandle (peer_ctx->vcm);
      }
    else
      {
        g_warning ("rdp: WTSOpenServer failed; continuing without vcm");
        peer_ctx->vcm = NULL;
      }
  }

  for (i = 0; i < handle_count && i < META_RDP_MAX_FREERDP_FDS; i++)
    {
      int fd = GetEventFileDescriptor (handles[i]);

      if (fd < 0)
        continue;

      peer_ctx->fd_sources[peer_ctx->n_fd_sources++] =
        meta_rdp_add_fd_source (fd, rdp_client_activity, client);
    }

  self->peers = g_list_prepend (self->peers, peer_ctx);

  g_message ("rdp: peer %p initialized (%d fds)",
             client, peer_ctx->n_fd_sources);
  return 0;

error:
  freerdp_peer_context_free (client);
  return -1;
}

static BOOL
rdp_incoming_peer (freerdp_listener *instance, freerdp_peer *client)
{
  MetaRdpServer *self = (MetaRdpServer *) instance->param4;

  g_message ("rdp: incoming peer %p", client);

  if (rdp_peer_init (client, self) < 0)
    {
      g_warning ("rdp: failed to init incoming peer");
      return FALSE;
    }

  return TRUE;
}

static gboolean
rdp_listener_activity (gpointer data)
{
  freerdp_listener *instance = data;

  if (!instance->CheckFileDescriptor (instance))
    {
      g_warning ("rdp: listener CheckFileDescriptor failed");
      return FALSE;
    }

  return TRUE;
}

static gboolean
rdp_implant_listener (MetaRdpServer    *self,
                      freerdp_listener *instance)
{
  HANDLE handles[META_RDP_MAX_FREERDP_FDS];
  int handle_count;
  int i;

  handle_count = instance->GetEventHandles (instance, handles,
                                            META_RDP_MAX_FREERDP_FDS);
  if (!handle_count)
    {
      g_warning ("rdp: failed to get listener event handles");
      return FALSE;
    }

  for (i = 0; i < handle_count && i < META_RDP_MAX_FREERDP_FDS; i++)
    {
      int fd = GetEventFileDescriptor (handles[i]);

      if (fd < 0)
        continue;

      self->listener_fd_sources[self->n_listener_fd_sources++] =
        meta_rdp_add_fd_source (fd, rdp_listener_activity, instance);
    }

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Task 03: TLS cert generation (winpr-makecert)                      */
/* ------------------------------------------------------------------ */

static gboolean
meta_rdp_generate_session_tls (MetaRdpServer  *self,
                               GError        **error)
{
  g_autofree char *tmpl = NULL;
  g_autofree char *stdout_buf = NULL;
  g_autofree char *stderr_buf = NULL;
  const char *argv[] = {
    "winpr-makecert",
    "-silent",
    "-format", "crt",
    "-n", "CN=mutter-rdp",
    "-path", NULL, /* filled in below */
    "mutter-rdp",
    NULL,
  };
  int exit_status = 0;

  tmpl = g_build_filename (g_get_tmp_dir (), "mutter-rdp-cert-XXXXXX", NULL);
  self->cert_dir = g_mkdtemp (tmpl);
  if (!self->cert_dir)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "failed to create temp dir for RDP cert: %s",
                   g_strerror (errno));
      return FALSE;
    }
  g_steal_pointer (&tmpl);

  argv[7] = self->cert_dir;

  if (!g_spawn_sync (NULL, (char **) argv, NULL,
                     G_SPAWN_SEARCH_PATH,
                     NULL, NULL,
                     &stdout_buf, &stderr_buf,
                     &exit_status, error))
    {
      g_prefix_error (error, "failed to run winpr-makecert: ");
      return FALSE;
    }

  if (!g_spawn_check_wait_status (exit_status, error))
    {
      g_prefix_error (error, "winpr-makecert failed (%s): ",
                      stderr_buf ? stderr_buf : "no output");
      return FALSE;
    }

  self->cert_file = g_build_filename (self->cert_dir, "mutter-rdp.crt", NULL);
  self->key_file = g_build_filename (self->cert_dir, "mutter-rdp.key", NULL);

  if (!g_file_test (self->cert_file, G_FILE_TEST_EXISTS) ||
      !g_file_test (self->key_file, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                   "winpr-makecert did not produce %s / %s",
                   self->cert_file, self->key_file);
      return FALSE;
    }

  g_message ("rdp: generated session TLS cert at %s", self->cert_file);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Task 03: listener setup (vsock / tcp)                              */
/* ------------------------------------------------------------------ */

static int
meta_rdp_create_vsock_fd (int port)
{
  struct sockaddr_vm addr;
  const int buffer_size = 65536;
  int fd;

  fd = socket (AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      g_warning ("rdp: failed to create vsock: %s", g_strerror (errno));
      return -1;
    }

  setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof (buffer_size));
  setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof (buffer_size));

  memset (&addr, 0, sizeof (addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_ANY;
  addr.svm_port = port;

  if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      g_warning ("rdp: failed to bind vsock: %s", g_strerror (errno));
      close (fd);
      return -1;
    }

  if (listen (fd, 1) != 0)
    {
      g_warning ("rdp: failed to listen on vsock: %s", g_strerror (errno));
      close (fd);
      return -1;
    }

  return fd;
}

/*
 * Determine the listening fd for the RDP server, in priority order:
 *   1. MUTTER_RDP_VSOCK_PORT set -> bind our own AF_VSOCK on that port. This is
 *      the WSLGd A1 hand-off: WSLGd publishes the reserved port and mutter (this
 *      process, launched externally in the user distro) binds it.
 *   2. USE_VSOCK set to a non-empty value -> an already-listening fd inherited
 *      from WSLGd; use it directly.
 *   3. USE_VSOCK set but empty -> create our own vsock on vsock_port.
 *   4. none of the above -> return -1 (fall back to TCP for local debugging).
 */
static int
meta_rdp_get_listen_fd (MetaRdpServer *self,
                        int            vsock_port)
{
  const char *vsock_port_str = g_getenv ("MUTTER_RDP_VSOCK_PORT");
  const char *fd_str;
  int fd;

  if (vsock_port_str && *vsock_port_str != '\0')
    {
      int port = atoi (vsock_port_str);

      if (port <= 0)
        {
          g_warning ("rdp: MUTTER_RDP_VSOCK_PORT=%s is not a valid port",
                     vsock_port_str);
          return -1;
        }

      fd = meta_rdp_create_vsock_fd (port);
      if (fd >= 0)
        {
          self->owned_listen_fd = fd;
          g_message ("rdp: created vsock fd %d on WSLGd-published port %d",
                     fd, port);
        }
      return fd;
    }

  fd_str = g_getenv ("USE_VSOCK");

  if (!fd_str)
    return -1;

  if (*fd_str != '\0')
    {
      fd = atoi (fd_str);
      if (fd <= 0)
        {
          g_warning ("rdp: USE_VSOCK=%s is not a valid fd", fd_str);
          return -1;
        }
      g_message ("rdp: using inherited vsock fd %d from WSLGd", fd);
      return fd;
    }

  fd = meta_rdp_create_vsock_fd (vsock_port);
  if (fd >= 0)
    {
      self->owned_listen_fd = fd;
      g_message ("rdp: created vsock fd %d on port %d", fd, vsock_port);
    }
  return fd;
}

static gboolean
meta_rdp_server_start_listener (MetaRdpServer  *self,
                                GError        **error)
{
  int listen_fd;
  int vsock_port = 0;
  int tcp_port = META_RDP_DEFAULT_TCP_PORT;
  const char *port_env;

  if (!meta_rdp_generate_session_tls (self, error))
    return FALSE;

  self->listener = freerdp_listener_new ();
  if (!self->listener)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "freerdp_listener_new failed");
      return FALSE;
    }

  self->listener->PeerAccepted = rdp_incoming_peer;
  self->listener->param4 = self;

  port_env = g_getenv ("MUTTER_RDP_PORT");
  if (port_env)
    tcp_port = atoi (port_env);

  listen_fd = meta_rdp_get_listen_fd (self, vsock_port);
  if (listen_fd > 0)
    {
      if (!self->listener->OpenFromSocket (self->listener, listen_fd))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "unable to open RDP listener from fd %d", listen_fd);
          return FALSE;
        }
      g_message ("rdp: listening on inherited/vsock fd %d", listen_fd);
    }
  else
    {
      if (!self->listener->Open (self->listener, "0.0.0.0", tcp_port))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "unable to bind RDP TCP listener on port %d", tcp_port);
          return FALSE;
        }
      g_message ("rdp: listening on TCP 0.0.0.0:%d (debug mode)", tcp_port);
    }

  if (!rdp_implant_listener (self, self->listener))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "unable to wire RDP listener into main loop");
      return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------ */

static void
on_context_started (MetaContext   *context,
                    MetaRdpServer *self)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (self->backend);
  g_autoptr (GError) error = NULL;

  g_message ("rdp: context started, wiring up virtual output");

  self->monitors_changed_handler_id =
    g_signal_connect_object (monitor_manager, "monitors-changed",
                             G_CALLBACK (on_monitors_changed), self,
                             G_CONNECT_DEFAULT);

  meta_rdp_server_attach_views (self);

  if (!meta_rdp_server_start_listener (self, &error))
    {
      g_warning ("rdp: failed to start RDP listener: %s", error->message);
      meta_context_terminate_with_error (context, g_steal_pointer (&error));
      return;
    }

  g_message ("rdp: RDP server ready, waiting for client");
}

MetaRdpServer *
meta_rdp_server_new (MetaBackend  *backend,
                     GError      **error)
{
  MetaRdpServer *self;
  MetaContext *context;

  if (g_getenv ("MUTTER_RDP") == NULL)
    return NULL;

  g_message ("rdp: starting MetaRdpServer (FreeRDP %s, gfxredir: %s)",
             FREERDP_VERSION_FULL,
#ifdef HAVE_FREERDP_GFXREDIR_H
             "yes"
#else
             "no"
#endif
             );

  self = g_object_new (META_TYPE_RDP_SERVER, NULL);
  self->backend = backend;

  self->shared_memory_mount_path =
    g_strdup (g_getenv ("WSL2_SHARED_MEMORY_MOUNT_POINT"));
  if (self->shared_memory_mount_path)
    g_message ("rdp: shared-memory mount: %s (gfxredir fast path enabled)",
               self->shared_memory_mount_path);
  else
    g_message ("rdp: WSL2_SHARED_MEMORY_MOUNT_POINT unset; codec fallback only");

  context = meta_backend_get_context (backend);

  self->started_handler_id =
    g_signal_connect_object (context, "started",
                             G_CALLBACK (on_context_started), self,
                             G_CONNECT_DEFAULT);

  return self;
}

static void
meta_rdp_server_dispose (GObject *object)
{
  MetaRdpServer *self = META_RDP_SERVER (object);
  GList *l;
  int i;

  meta_rdp_server_detach_views (self);

  for (l = self->peers; l; l = l->next)
    {
      MetaRdpPeerContext *peer_ctx = l->data;

      meta_rdp_peer_remove_fd_sources (peer_ctx);
      peer_ctx->peer->Disconnect (peer_ctx->peer);
      freerdp_peer_context_free (peer_ctx->peer);
      freerdp_peer_free (peer_ctx->peer);
    }
  g_clear_pointer (&self->peers, g_list_free);

  for (i = 0; i < self->n_listener_fd_sources; i++)
    {
      if (self->listener_fd_sources[i])
        {
          g_source_destroy (self->listener_fd_sources[i]);
          self->listener_fd_sources[i] = NULL;
        }
    }
  self->n_listener_fd_sources = 0;

  if (self->listener)
    {
      self->listener->Close (self->listener);
      freerdp_listener_free (self->listener);
      self->listener = NULL;
    }

  if (self->owned_listen_fd >= 0)
    {
      close (self->owned_listen_fd);
      self->owned_listen_fd = -1;
    }

  g_clear_pointer (&self->cert_file, g_free);
  g_clear_pointer (&self->key_file, g_free);
  g_clear_pointer (&self->cert_dir, g_free);
  g_clear_pointer (&self->shared_memory_mount_path, g_free);

  self->backend = NULL;

  G_OBJECT_CLASS (meta_rdp_server_parent_class)->dispose (object);
}

static void
meta_rdp_server_class_init (MetaRdpServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_rdp_server_dispose;
}

static void
meta_rdp_server_init (MetaRdpServer *self)
{
  self->owned_listen_fd = -1;
}
