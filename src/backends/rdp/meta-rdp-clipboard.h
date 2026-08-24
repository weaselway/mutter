/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * Task 05: CLIPRDR clipboard bridge between the RDP client and mutter's
 * MetaSelection (clipboard). Text (UTF-8) and HTML are supported in both
 * directions; images/files are out of scope for v1.
 *
 * Adapted from wslg/weston/libweston/backend-rdp/rdpclip.c, but retargeted onto
 * mutter's MetaSelection/MetaSelectionSource API and simplified to run
 * single-threaded on mutter's main loop (no cross-thread task dispatch).
 */

#pragma once

#include <glib.h>

#include <freerdp/peer.h>

#include "backends/meta-backend-private.h"

G_BEGIN_DECLS

typedef struct _MetaRdpClipboard MetaRdpClipboard;

/* Create the CLIPRDR server channel for this peer and bridge it to mutter's
 * clipboard selection. Requires the peer's vcm to be valid. Returns NULL on
 * failure (clipboard simply stays disabled for the peer). */
MetaRdpClipboard * meta_rdp_clipboard_new (freerdp_peer *peer,
                                           MetaBackend  *backend,
                                           HANDLE        vcm);

/* The channel's event handle, to add to the main-loop fd set, or NULL. */
HANDLE meta_rdp_clipboard_get_event_handle (MetaRdpClipboard *clipboard);

/* Pump the channel; returns FALSE on fatal channel error. Call from the peer's
 * main-loop activity handler (same thread as everything else). */
gboolean meta_rdp_clipboard_check_event_handle (MetaRdpClipboard *clipboard);

void meta_rdp_clipboard_free (MetaRdpClipboard *clipboard);

G_END_DECLS
