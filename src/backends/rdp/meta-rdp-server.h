/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * In-process RDP/VAIL output backend for mutter (WSLg).
 *
 * Task 02: attach to a single (virtual) monitor's stage view and fire a
 * per-frame hook that hands us the composited framebuffer + damage. The real
 * FreeRDP listener/peer arrives in task 03; for now MetaRdpServer just proves
 * out the headless virtual-output plumbing.
 */

#ifndef META_RDP_SERVER_H
#define META_RDP_SERVER_H

#include <glib-object.h>

#include "backends/meta-backend-types.h"

G_BEGIN_DECLS

#define META_TYPE_RDP_SERVER (meta_rdp_server_get_type ())
G_DECLARE_FINAL_TYPE (MetaRdpServer, meta_rdp_server,
                      META, RDP_SERVER, GObject)

/* Returns NULL (without erroring) when the RDP backend is not requested
 * (MUTTER_RDP unset). Otherwise starts the server bound to @backend. */
MetaRdpServer * meta_rdp_server_new (MetaBackend  *backend,
                                     GError      **error);

G_END_DECLS

#endif /* META_RDP_SERVER_H */
