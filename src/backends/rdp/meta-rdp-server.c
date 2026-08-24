/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * In-process RDP/VAIL output backend for mutter (WSLg).
 *
 * Task 02: attach to the (virtual) monitor stage view(s) and log a per-frame
 * "frame ready" hook with the composited framebuffer + damage. No RDP wire
 * traffic yet -- that lands in task 03/04. The FreeRDP headers are still
 * included so task 01's link/gate stays exercised.
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

#include <freerdp/version.h>
#include <winpr/synch.h>

#ifdef HAVE_FREERDP_GFXREDIR_H
#include <freerdp/server/gfxredir.h>
#endif

typedef struct _MetaRdpWatchedView
{
  MetaRdpServer *server;
  ClutterStageView *view;
  MetaStageWatch *paint_watch;
  gulong destroy_handler_id;
} MetaRdpWatchedView;

struct _MetaRdpServer
{
  GObject parent;

  MetaBackend *backend;

  gulong started_handler_id;
  gulong monitors_changed_handler_id;

  gboolean views_attached;
  GList *watched_views; /* MetaRdpWatchedView* */

  uint64_t frame_counter;
};

G_DEFINE_FINAL_TYPE (MetaRdpServer, meta_rdp_server, G_TYPE_OBJECT)

static MetaStage *
meta_rdp_server_get_stage (MetaRdpServer *self)
{
  return META_STAGE (meta_backend_get_stage (self->backend));
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
  MtkRectangle layout;
  int width, height;
  g_autofree char *damage_str = NULL;

  framebuffer = clutter_stage_view_get_framebuffer (view);
  clutter_stage_view_get_layout (view, &layout);
  width = cogl_framebuffer_get_width (framebuffer);
  height = cogl_framebuffer_get_height (framebuffer);

  /* Damage: prefer the redraw clip; fall back to full-surface damage (Weston
   * forces full damage on the first frame anyway). */
  if (redraw_clip && !mtk_region_is_empty (redraw_clip))
    {
      MtkRectangle ext;

      ext = mtk_region_get_extents (redraw_clip);
      damage_str = g_strdup_printf ("%d,%d %dx%d (%d rects)",
                                    ext.x, ext.y, ext.width, ext.height,
                                    mtk_region_num_rectangles (redraw_clip));
    }
  else
    {
      damage_str = g_strdup_printf ("0,0 %dx%d (full)", width, height);
    }

  self->frame_counter++;

  g_debug ("rdp: frame ready #%" G_GUINT64_FORMAT
           ", fb=%p, size=%dx%d, layout=%d,%d %dx%d, damage=%s",
           self->frame_counter, framebuffer, width, height,
           layout.x, layout.y, layout.width, layout.height, damage_str);
}

static void meta_rdp_server_detach_views (MetaRdpServer *self);

static void
on_watched_view_destroyed (gpointer  user_data,
                           GObject  *where_the_object_was)
{
  MetaRdpWatchedView *watched = user_data;
  MetaRdpServer *self = watched->server;

  /* The view is gone; drop it and re-scan on the next monitors-changed. */
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

static void
on_context_started (MetaContext   *context,
                    MetaRdpServer *self)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (self->backend);

  g_message ("rdp: context started, wiring up virtual output");

  self->monitors_changed_handler_id =
    g_signal_connect_object (monitor_manager, "monitors-changed",
                             G_CALLBACK (on_monitors_changed), self,
                             G_CONNECT_DEFAULT);

  meta_rdp_server_attach_views (self);
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

  context = meta_backend_get_context (backend);

  /* By the time "started" fires, the backend, stage, display and any
   * persistent virtual monitors all exist. */
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

  meta_rdp_server_detach_views (self);

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
}
