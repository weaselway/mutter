/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * In-process RDP/VAIL output backend for mutter (WSLg).
 * Stub module (task 01): only proves that FreeRDP links into libmutter and
 * that the gfxredir header gate compiles. Real listener/peer lands in task 03.
 */

#include "config.h"

#include "backends/rdp/meta-rdp-server.h"

#include <freerdp/version.h>
#include <winpr/synch.h>

#ifdef HAVE_FREERDP_GFXREDIR_H
#include <freerdp/server/gfxredir.h>
#endif

struct _MetaRdpServer
{
  GObject parent;
};

G_DEFINE_FINAL_TYPE (MetaRdpServer, meta_rdp_server, G_TYPE_OBJECT)

static void
meta_rdp_server_class_init (MetaRdpServerClass *klass)
{
}

static void
meta_rdp_server_init (MetaRdpServer *self)
{
}

MetaRdpServer *
meta_rdp_server_new (void)
{
  /* Touch FreeRDP so the linker actually pulls it in. */
  g_debug ("MetaRdpServer stub built against FreeRDP %s (gfxredir: %s)",
           FREERDP_VERSION_FULL,
#ifdef HAVE_FREERDP_GFXREDIR_H
           "yes"
#else
           "no"
#endif
           );

  return g_object_new (META_TYPE_RDP_SERVER, NULL);
}
