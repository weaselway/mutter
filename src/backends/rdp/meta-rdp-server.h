/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * In-process RDP/VAIL output backend for mutter (WSLg).
 * Task 03 will flesh this out into a real FreeRDP listener/peer. For now this
 * is a stub that only proves the build wiring (task 01).
 */

#ifndef META_RDP_SERVER_H
#define META_RDP_SERVER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define META_TYPE_RDP_SERVER (meta_rdp_server_get_type ())
G_DECLARE_FINAL_TYPE (MetaRdpServer, meta_rdp_server,
                      META, RDP_SERVER, GObject)

MetaRdpServer * meta_rdp_server_new (void);

G_END_DECLS

#endif /* META_RDP_SERVER_H */
