/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * PipeWire-over-RDP forwarding: bridges two stock
 * libpipewire-module-protocol-simple servers to FreeRDP's rdpsnd (playback)
 * and audin (capture) dynamic virtual channels.
 *
 * The RDP half is ported from wslg/weston/libweston/backend-rdp/rdpaudio.c
 * and rdpaudioin.c. Unlike the CLIPRDR bridge, this runs its own thread per
 * direction (as Weston did) rather than driving the channel from mutter's
 * main loop: rdpsnd/audin already spawn their own FreeRDP-internal thread via
 * Initialize()/Open(), so a dedicated thread pushing samples into them keeps
 * the same threading model instead of inventing a new one.
 *
 * What crosses the socket is raw interleaved PCM with no framing, so both
 * ends have to be configured to agree on the format -- see the comment in
 * meta-rdp-audio.c and weaselway's PipeWire drop-in.
 */

#pragma once

#include <glib.h>

#include <freerdp/peer.h>

G_BEGIN_DECLS

typedef struct _MetaRdpAudioOut MetaRdpAudioOut;

/* Push whatever the RDP channels have queued out to the socket. Called from
 * the playback thread after each packet, because the queue is otherwise only
 * drained by the main loop -- which means audio would go out at the mercy of
 * the render loop and stutter whenever a frame took too long. Must be safe to
 * call from a thread that is not the main one. */
typedef void (* MetaRdpAudioFlushFunc) (gpointer user_data);
typedef struct _MetaRdpAudioIn MetaRdpAudioIn;

/* Create the rdpsnd server channel for this peer and start forwarding
 * playback audio from the PipeWire playback server to the client. Requires
 * the peer's vcm to be valid. Returns NULL on failure (playback forwarding
 * stays disabled for the peer; the RDP session itself is unaffected).
 *
 * Success here does not mean PipeWire was reachable: the connection is made
 * from a worker thread once the client negotiates a format, and is retried
 * until it succeeds.
 *
 * flush (may be NULL) is invoked from that worker thread after every packet;
 * see MetaRdpAudioFlushFunc. */
MetaRdpAudioOut * meta_rdp_audio_out_new (HANDLE                 vcm,
                                          MetaRdpAudioFlushFunc  flush,
                                          gpointer               flush_data);

void meta_rdp_audio_out_free (MetaRdpAudioOut *audio_out);

/* Create the audin server channel for this peer and start forwarding capture
 * audio from the client to the PipeWire capture server. Returns NULL on
 * failure. Connecting is likewise retried on a worker thread; note the
 * connection itself is what makes the microphone appear in PipeWire's graph,
 * so it is held for as long as the peer exists. */
MetaRdpAudioIn * meta_rdp_audio_in_new (HANDLE vcm);

void meta_rdp_audio_in_free (MetaRdpAudioIn *audio_in);

G_END_DECLS
