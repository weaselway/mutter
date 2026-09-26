/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * In-process RDP/VAIL output backend for mutter (WSLg).
 *
 * An in-process FreeRDP 3 server listening on vsock. It presents the
 * composited desktop through gfxredir shared memory, and bridges input, the clipboard (CLIPRDR), display control
 * (DISP), touchpad gestures (RDPEI) and audio (rdpsnd/audin, see
 * meta-rdp-audio.c).
 *
 * Originally ported from wslg/weston/libweston/backend-rdp/rdp.c (minus
 * RAIL). There is no codec path: when gfxredir cannot be used, the client gets
 * an error frame saying why instead of the desktop (meta_rdp_peer_fail()).
 * Weston's wl_event_loop fd wiring is replaced with GSources attached
 * to mutter's default GMainContext; the peer is driven from mutter's main
 * thread, while the gfxredir, disp, rdpei and audio channels run their own
 * threads and hand work back to the main loop.
 */

#include "config.h"

#include "backends/rdp/meta-rdp-server.h"
#include "backends/rdp/meta-rdp-audio.h"
#include "backends/rdp/meta-rdp-clipboard.h"

#include "backends/meta-backend-private.h"
#include "backends/meta-crtc-mode.h"
#include "backends/meta-cursor-tracker-private.h"
#include "backends/meta-logical-monitor-private.h"
#include "backends/meta-monitor-manager-private.h"
#include "backends/meta-monitor-private.h"
#include "backends/meta-renderer.h"
#include "backends/meta-renderer-view.h"
#include "backends/meta-stage-private.h"
#include "backends/meta-virtual-monitor.h"
#include "clutter/clutter.h"
#include "clutter/clutter-cursor-private.h"
#include "clutter/clutter-event-private.h"
#include "cogl/cogl.h"
#include "meta/meta-backend.h"
#include "meta/meta-keymap-description.h"
#include "core/meta-context-private.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/meta-debug.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/vm_sockets.h>
#include <linux/input.h>

#include <glib/gstdio.h>

#include <cairo.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <freerdp/freerdp.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/update.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/version.h>
#include <freerdp/input.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/channels/wtsvc.h>
#include <winpr/input.h>
#include <freerdp/channels/channels.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>

#include <freerdp/server/gfxredir.h>
#include <freerdp/server/disp.h>
#include <freerdp/channels/disp.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/rdpei.h>
#include <freerdp/channels/rdpei.h>

/* From Weston's rdp.c: an upper bound on the number of FreeRDP event handles
 * (listener or per-peer, +1 for the virtual channel manager). */
#define META_RDP_MAX_FREERDP_FDS 32

/* Watched sources per peer: the peer's own handles, +1 for the virtual channel
 * manager and +1 for the CLIPRDR event handle (which Weston does not watch
 * separately -- it runs cliprdr threaded via Start(), we drive it from the main
 * loop instead). */
#define META_RDP_MAX_PEER_FD_SOURCES (META_RDP_MAX_FREERDP_FDS + 2)

#define META_RDP_DEFAULT_TCP_PORT 3389

/* Single fullscreen desktop window (see rdp.h RDP_RAIL_DESKTOP_WINDOW_ID). */
#define META_RDP_DESKTOP_WINDOW_ID 0xFFFFFFFF
#define META_RDP_POOL_ID 1
/* Buffer ids are 1-based; buffer i uses id (i + 1). */
#define META_RDP_BUFFER_ID(i) ((uint64_t) ((i) + 1))

/* Three buffers.
 *
 * Two would be enough if the readback were synchronous -- write one while the
 * client reads the other. With the asynchronous path (see "Asynchronous
 * readback" below) a third is needed, because one buffer is tied up as the
 * destination of a readback that has been issued but not yet landed, and is
 * neither ours to rewrite nor the client's to read. At two, the steady state
 * would be one buffer mid-readback and one with the client, leaving none free,
 * and every frame would fall into the coalescing path in
 * meta_rdp_peer_present(). */
#define META_RDP_N_BUFFERS 3

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
/* One shared-memory buffer inside the pool.
 *
 * @stale is the region this buffer is missing relative to the most recently
 * written one: mutter only reads back the damaged part of each frame, so a
 * buffer that sat out a frame has a hole where that frame's damage went. It is
 * filled by copying from the up to date buffer before the next readback, which
 * keeps every buffer whole -- the client's full-refresh path blits the entire
 * surface, not just the presented rect. */
typedef struct _MetaRdpBuffer
{
  size_t offset;       /* byte offset inside the pool */
  gboolean in_flight;  /* presented, not yet acked: do not overwrite */
  uint64_t present_id; /* presentId of the outstanding present */
  MtkRegion *stale;
} MetaRdpBuffer;

typedef struct _MetaRdpPeerContext
{
  rdpContext rdp_context;

  MetaRdpServer *server;
  freerdp_peer *peer;

  HANDLE vcm;

  /* GSources bridging this peer's FreeRDP fds into the GLib main loop. */
  GSource *fd_sources[META_RDP_MAX_PEER_FD_SOURCES];
  int n_fd_sources;

  gboolean activated;

  /* input injection via clutter virtual devices. */
  ClutterVirtualInputDevice *virtual_pointer;
  ClutterVirtualInputDevice *virtual_keyboard;

  /* CLIPRDR clipboard bridge, created on first activation. */
  MetaRdpClipboard *clipboard;
  /* Aliases the fd_sources[] entry watching the cliprdr event handle, so the
   * source can be torn down together with the bridge (the fd dies with it). */
  GSource *clipboard_fd_source;

  /* PulseAudio-over-RDP bridge (rdpsnd playback / audin capture), created on
   * first activation. Each runs its own accept/read thread, so unlike
   * clipboard there is no main-loop fd source to track here. */
  MetaRdpAudioOut *audio_out;
  MetaRdpAudioIn *audio_in;

  /* Debounced pointer button state, indexed by (button - BTN_LEFT). */
  gboolean button_state[8];
  gboolean mouse_button_swap;

  /* Precise/discrete wheel accumulation (ported from Weston). */
  int vertical_accum_wheel_precise;
  int vertical_accum_wheel_discrete;
  int horizontal_accum_wheel_precise;
  int horizontal_accum_wheel_discrete;

  /* Dynamic virtual channel manager, driven to DRDYNVC_STATE_READY before
   * opening DVCs such as gfxredir. */
  DrdynvcServerContext *drdynvc;

  /* MS-RDPEDISP: the client tells us what resolution it wants. Like gfxredir,
   * the channel runs its own thread, so the layout PDU is only recorded here
   * and applied on the main loop -- resizing the monitor touches Clutter. */
  /* Serialises WTSVirtualChannelManagerCheckFileDescriptorEx(), which is the
   * only thing that actually writes queued channel data to the socket. The
   * audio thread calls it too (see meta_rdp_peer_flush_channels), so that
   * playback is not held hostage by whatever the main thread is doing.
   *
   * A mutex around the *whole* drain, not a lock inside it: the queue is
   * shared by every channel, and two threads popping from it concurrently
   * could send two chunks of one channel's PDU out of order. Serialising the
   * loop means all pops happen inside one critical section, so queue order is
   * preserved no matter which thread does the draining. */
  GMutex vcm_drain_mutex;

  DispServerContext *disp;
  GMutex disp_mutex;
  guint disp_idle_id;
  int disp_requested_width;
  int disp_requested_height;
  uint32_t disp_requested_scale_percent;

  /* Pumps the peer from the main loop until drdynvc reaches READY, after
   * which activation is completed (meta_rdp_drdynvc_wait()). */
  guint drdynvc_wait_id;
  gint64 drdynvc_wait_start_us;

  /* Set between pushing a DesktopResize and the client's re-activation. The
   * client's surface is the old size until it comes back, so nothing may be
   * presented in the meantime. */
  gboolean resize_pending;

  /* MS-RDPEI: touch input, used to synthesize touchpad-gesture ClutterEvents
   * (3+ finger swipes -> workspace switch/overview, same as a real Precision
   * Touchpad) via clutter_event_put(). rdpei_server_open() spawns its own
   * reader thread, like gfxredir, so onTouchEvent does not run on the main
   * thread; clutter_event_put() is not thread safe, so touch data is only
   * recorded here (under rdpei_mutex) and the actual events are queued for
   * the main loop to emit via rdpei_idle_id. */
  RdpeiServerContext *rdpei;
  GMutex rdpei_mutex;
  guint rdpei_idle_id;
  /* Latest known position of each active contact: contactId (GUINT_TO_POINTER)
   * -> owned graphene_point_t*, in the same session-pixel space the client's
   * touch contacts arrive in. */
  GHashTable *rdpei_contacts;
  gboolean rdpei_gesture_active;
  uint32_t rdpei_gesture_fingers;
  graphene_point_t rdpei_gesture_prev_centroid;
  /* Debouncing for contact-count noise: Precision touchpads can transiently
   * under-report a finger's contact for a frame or two mid-swipe (more
   * likely the slower/more deliberate the swipe, since per-finger pressure
   * is less uniform than in a quick flick). Without debouncing, that blip
   * looks identical to the user actually lifting/adding a finger, which
   * forces an end-and-restart of the whole gesture -- silently discarding
   * GNOME Shell's accumulated swipe progress right when it matters most
   * (near release). See meta_rdp_rdpei_evaluate_gesture_locked(). */
  uint32_t rdpei_gesture_low_streak;
  uint32_t rdpei_gesture_pending_fingers;
  uint32_t rdpei_gesture_pending_streak;
  /* Diagnostics only: total delta accumulated since the current gesture's
   * BEGIN, logged at CANCEL/END so it's possible to tell from the logs
   * whether a revert was due to too little accumulated travel or a
   * spurious restart discarding otherwise-sufficient travel. */
  float rdpei_gesture_total_dx;
  float rdpei_gesture_total_dy;
  /* Diagnostics only: running sum of the delta actually dispatched to
   * Clutter since the current gesture's BEGIN,
   * tracked in meta_rdp_rdpei_dispatch() so logs can show exactly what
   * GNOME Shell's SwipeTracker is accumulating as progress. */
  float rdpei_gesture_dispatched_dx;
  float rdpei_gesture_dispatched_dy;
  /* MetaRdpPendingGesture* queued by onTouchEvent, drained and emitted by
   * meta_rdp_rdpei_dispatch() on the main loop. */
  GQueue *rdpei_pending_gestures;
  /* Ends a gesture whose touch frames stopped arriving (client gone quiet,
   * lost focus, dropped the lifts). Armed while a gesture is active; see
   * meta_rdp_rdpei_timeout(). */
  guint rdpei_timeout_id;
  gint64 rdpei_last_frame_us;

  /* Last pointer position we told Clutter about (meta_rdp_notify_pointer_position),
   * in the same scaled Clutter coordinate space as ClutterEvent.coords. Mouse
   * events and the rdpei dispatch above both only ever run on the main
   * thread, so this needs no locking of its own. Used as the touchpad-gesture
   * event's "coords" field, which is otherwise meaningless for a device with
   * no natural on-screen position. */
  float last_pointer_x;
  float last_pointer_y;

  /* gfxredir shared-memory present, the only way the desktop is sent. */
  GfxRedirServerContext *gfxredir;
  gboolean gfxredir_activated; /* caps confirmed; g_atomic, see below */

  /* Set once gfxredir turns out to be unusable for this peer, see
   * meta_rdp_peer_fail(). From then on the client only gets the error frame,
   * which is re-sent when the framebuffer size changes. */
  char *error_reason;
  char *error_hint;
  int error_frame_width;
  int error_frame_height;

  /* Gives up on a client that never confirms gfxredir caps. */
  guint gfxredir_caps_timeout_id;

  /* gfxredir_server_open() spawns its own reader thread, so the channel
   * callbacks do NOT run on the main thread. Presenting touches Clutter/Cogl
   * and the buffer bookkeeping below, none of which is thread safe, so the
   * callbacks only record a request here and bounce the actual work to the
   * main loop via gfxredir_idle_id. Everything in this block is guarded by
   * gfxredir_mutex; the fields it protects are written from the channel thread
   * and consumed on the main thread. */
  GMutex gfxredir_mutex;
  guint gfxredir_idle_id;
  gboolean gfxredir_present_requested; /* caps confirmed: fill the screen */
  const char *gfxredir_caps_error;     /* caps rejected: why, for the error frame */
  /* presentIds the client has acked, handed over for the main thread to
   * retire. Bounded by the number of buffers, since we never have more
   * presents outstanding than that. */
  uint64_t gfxredir_acked[META_RDP_N_BUFFERS];
  int gfxredir_n_acked;

  /* One pool holding META_RDP_N_BUFFERS buffers for the whole desktop, so a
   * readback can proceed while the client is still reading the previous
   * frame. */
  gboolean buffer_created;
  int buffer_width;
  int buffer_height;
  int buffer_stride;
  size_t buffer_size; /* live bytes per buffer; buffers are spaced further
                       * apart than this, see meta_rdp_ensure_buffer() */
  /* presentIds are globally monotonic and never reused. On a pool rebuild this
   * records the highest id issued against the old pool, so a late ack for a
   * destroyed buffer cannot retire the same-numbered buffer of the new one. */
  uint64_t present_id_floor;

  MetaRdpBuffer buffers[META_RDP_N_BUFFERS];
  int next_buffer;      /* round-robin cursor */
  int last_written;     /* buffer holding fully up to date contents, -1 if none */
  int n_presents_inflight;

  /* Named shared-memory file backing the pool. */
  int shm_fd;
  void *shm_addr;
  size_t shm_size;
  char shm_name[META_RDP_SHARED_MEMORY_NAME_SIZE + 1];

  /* Asynchronous readback state. See the "Asynchronous readback" block below.
   *
   * At most one readback is outstanding at a time, which is what keeps this a
   * handful of fields rather than a queue: the fence for frame N has almost
   * always signalled by the time frame N+1 is painted, and if it has not, the
   * damage simply coalesces the way a busy buffer already makes it.
   *
   * @readback_pbo is persistent and only reallocated when the frame size
   * changes -- allocating one costs ~22ms (a committed READBACK-heap resource),
   * against ~3us to map one that already exists. */
  CoglPixelBuffer *readback_pbo;
  size_t readback_pbo_size;
  CoglGpuFence *readback_fence;
  gboolean readback_pending;
  int readback_buffer;        /* pool buffer the pixels are destined for */
  MtkRectangle readback_rect; /* what was read, in framebuffer pixels */
  int readback_polls;         /* fence polls so far, for the bounded fallback */
  guint readback_poll_id;
  int64_t readback_issued_us;

  gboolean frame_missed; /* damage arrived with no buffer free to take it */
  /* Union of the damage that accumulated while a present was in flight. Kept
   * as a region, not a bounding box: the readback still uses the extents, but
   * holding the real region lets us measure how much the bounding box
   * over-reads before deciding whether to extend the protocol with a rect
   * array (gfxredir PRESENT_BUFFER carries a single dirtyRect). */
  MtkRegion *missed_damage;
  uint64_t current_frame_id;

  GList *link; /* node in server->peers */
} MetaRdpPeerContext;

struct _MetaRdpServer
{
  GObject parent;

  MetaBackend *backend;

  gulong started_handler_id;
  gulong monitors_changed_handler_id;
  gulong cursor_changed_handler_id;
  gulong cursor_visibility_handler_id;

  gboolean views_attached;
  GList *watched_views; /* MetaRdpWatchedView* */

  uint64_t frame_counter;

  /* FreeRDP listener + its GLib fd bridge. */
  freerdp_listener *listener;
  GSource *listener_fd_sources[META_RDP_MAX_FREERDP_FDS];
  int n_listener_fd_sources;
  int owned_listen_fd; /* vsock fd we created ourselves, or -1 */

  /* Throwaway self-signed TLS material generated at startup, as PEM. */
  char *cert_pem;
  char *key_pem;

  /* virtio-fs/DAX shared-memory mount gfxredir allocates its pools on, or
   * NULL with shared_memory_error saying why. */
  char *shared_memory_mount_path;
  char *shared_memory_error;

  GList *peers; /* MetaRdpPeerContext* */
};

G_DEFINE_FINAL_TYPE (MetaRdpServer, meta_rdp_server, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ */
/* Desktop resize                                                      */
/*                                                                     */
/* The RDP client dictates the resolution: whatever size it negotiates  */
/* at activation, or later asks for over MS-RDPEDISP, becomes the size  */
/* of mutter's virtual monitor. The --virtual-monitor passed on the     */
/* command line is only the size the session runs at before anyone      */
/* connects.                                                            */
/*                                                                     */
/* Resizing is asynchronous: setting the mode makes the monitor manager */
/* rebuild the stage views, and the new size is only observable once a  */
/* frame arrives for the new view. meta_rdp_peer_present() notices the  */
/* mismatch there and pushes a DesktopResize back to the client.        */
/* ------------------------------------------------------------------ */

/* The scale the desktop is currently running at, i.e. how many framebuffer
 * pixels there are per stage (logical) pixel.
 *
 * The RDP client works in framebuffer pixels throughout -- its desktop size,
 * its pointer events and its cursor sprite are all physical -- while Clutter
 * works in logical ones, so this is the conversion factor between the two. */
static float
meta_rdp_server_get_scale (MetaRdpServer *self)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (self->backend);
  MetaLogicalMonitor *logical_monitor;

  /* Single-head: the primary logical monitor is the desktop. */
  logical_monitor =
    meta_monitor_manager_get_primary_logical_monitor (monitor_manager);
  if (!logical_monitor)
    return 1.0f;

  return meta_logical_monitor_get_scale (logical_monitor);
}

/* Turn an MS-RDPEDISP DesktopScaleFactor (a percentage: 100, 150, 200...) into
 * a scale mutter will accept for a @width x @height mode.
 *
 * Mutter only allows scales that divide the resolution into whole logical
 * pixels, so an arbitrary percentage has to be snapped to the nearest one it
 * supports; meta_get_closest_monitor_scale_factor_for_resolution() enumerates
 * exactly the set the monitor config manager would.
 *
 * Weston does the equivalent in disp_get_client_scale_from_monitor()
 * (rdpdisp.c), but has to choose up front between integer and fractional
 * scaling -- mutter's LOGICAL layout mode supports fractional natively, so we
 * can just take what the client asked for. */
static float
meta_rdp_scale_from_percent (uint32_t desktop_scale_factor,
                             int      width,
                             int      height)
{
  float requested;
  float scale;

  /* 0 means the client didn't report one. */
  if (desktop_scale_factor == 0)
    return 1.0f;

  requested = desktop_scale_factor / 100.0f;
  if (requested < 1.0f)
    requested = 1.0f;

  scale = meta_get_closest_monitor_scale_factor_for_resolution ((unsigned int) width,
                                                                (unsigned int) height,
                                                                requested);
  /* No valid scale for this resolution at all (the logical size would be too
   * small to be usable); stay unscaled rather than refuse the mode. */
  if (scale <= 0.0f)
    return 1.0f;

  if (!G_APPROX_VALUE (scale, requested, 0.001f))
    {
      g_message ("rdp: client asked for %u%% scaling at %dx%d, using %.3f "
                 "(nearest mutter supports)",
                 desktop_scale_factor, width, height, scale);
    }

  return scale;
}

/* Mutter's own resize-a-virtual-monitor path is ensure_virtual_monitor() in
 * backends/meta-screen-cast-virtual-stream-src.c; this is the same sequence.
 * Must run on the main thread -- it reconfigures Clutter.
 *
 * @width and @height are in framebuffer pixels (what the client sends); the
 * logical desktop ends up @width/@scale x @height/@scale.
 *
 * Returns TRUE if a new mode was actually applied. */
static gboolean
meta_rdp_server_resize_monitor (MetaRdpServer *self,
                                int            width,
                                int            height,
                                float          scale)
{
  MetaMonitorManager *monitor_manager;
  MetaVirtualMonitor *virtual_monitor;
  MetaCrtcMode *crtc_mode;
  const MetaCrtcModeInfo *mode_info;
  MetaVirtualModeInfo *new_mode_info;
  GList *virtual_monitors;
  GList *mode_infos = NULL;

  if (width <= 0 || height <= 0)
    {
      g_warning ("rdp: refusing to resize the monitor to %dx%d", width, height);
      return FALSE;
    }

  monitor_manager = meta_backend_get_monitor_manager (self->backend);
  virtual_monitors = meta_monitor_manager_get_virtual_monitors (monitor_manager);
  if (!virtual_monitors)
    {
      g_warning ("rdp: no virtual monitor to resize; was --virtual-monitor "
                 "passed?");
      return FALSE;
    }

  /* Single-head backend: the first (and only) virtual monitor is the desktop.
   * Weston matches a whole list of heads here (rdpdisp.c), which we do not
   * need until we support more than one monitor. */
  virtual_monitor = virtual_monitors->data;

  crtc_mode = meta_virtual_monitor_get_crtc_mode (virtual_monitor);
  mode_info = meta_crtc_mode_get_info (crtc_mode);
  if (mode_info->width == width && mode_info->height == height &&
      mode_info->has_preferred_scale &&
      G_APPROX_VALUE (mode_info->preferred_scale, scale, 0.001f))
    return FALSE;

  g_message ("rdp: reconfiguring virtual monitor %dx%d@%.3f -> %dx%d@%.3f "
             "(logical %dx%d)",
             mode_info->width, mode_info->height,
             mode_info->has_preferred_scale ? mode_info->preferred_scale : 1.0f,
             width, height, scale,
             (int) floorf (width / scale), (int) floorf (height / scale));

  new_mode_info = meta_virtual_mode_info_new (width, height,
                                              mode_info->refresh_rate);
  meta_virtual_mode_info_set_preferred_scale (new_mode_info, scale);
  mode_infos = g_list_append (mode_infos, new_mode_info);

  meta_virtual_monitor_set_modes (virtual_monitor, mode_infos);
  g_list_free_full (mode_infos, (GDestroyNotify) meta_virtual_mode_info_free);

  meta_monitor_manager_reload (monitor_manager);

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* pixel readback + present (gfxredir) */
/* ------------------------------------------------------------------ */

static MetaStage *
meta_rdp_server_get_stage (MetaRdpServer *self)
{
  return META_STAGE (meta_backend_get_stage (self->backend));
}

/* Rolling meter for the framebuffer readback: exponentially weighted so it
 * tracks recent activity, reported once a second (with
 * MUTTER_DEBUG=remote-desktop), process-wide statics.
 *
 * This is the number the readback work is aimed at: glReadPixels here is a
 * synchronous GPU->CPU transfer that on d3d12 costs a blit into a staging
 * texture, a texture->buffer copy and a hard fence wait, all on the main loop.
 * Tracking mean *and* peak matters -- the mean is what steady-state damage
 * costs, the peak is what a full-frame readback (activation, resize, a
 * fullscreen repaint) costs, and they differ by two orders of magnitude.
 *
 * Bytes are counted as the pixels actually requested (width * height * 4),
 * not what the driver moved internally, so MB/s here is a lower bound. */
static void
meta_rdp_account_readback (int64_t elapsed_us,
                           int64_t fence_us,
                           size_t  bytes)
{
  /* Tracked separately from @elapsed_us because the fence is easy to assume is
   * free and profiling showed it is not: creating one can drain a
   * threaded-context driver's queue on the calling thread. Anything other than
   * a near-zero figure here means the fence is doing submission work inside the
   * paint. */
  static int64_t window_fence_us = 0;
  static double mean_fence_us = -1.0;
  /* Weight of the newest sample in the rolling means; ~5s of history. */
  static const double alpha = 0.2;
  static int64_t window_start_us = 0;
  static int64_t window_us = 0;
  static int64_t window_peak_us = 0;
  static size_t window_bytes = 0;
  static unsigned window_reads = 0;
  static double mean_us = -1.0;
  static double mean_rps = -1.0;
  static double mean_bps = -1.0;

  int64_t now_us;
  int64_t elapsed_window_us;

  if (!meta_is_topic_enabled (META_DEBUG_REMOTE_DESKTOP))
    return;

  now_us = g_get_monotonic_time ();
  window_us += elapsed_us;
  window_fence_us += fence_us;
  window_bytes += bytes;
  window_reads++;
  if (elapsed_us > window_peak_us)
    window_peak_us = elapsed_us;

  if (window_start_us == 0)
    {
      window_start_us = now_us;
      return;
    }

  elapsed_window_us = now_us - window_start_us;
  if (elapsed_window_us < G_USEC_PER_SEC)
    return;

  double us = (double) window_us / window_reads;
  double fence = (double) window_fence_us / window_reads;
  double rps = window_reads * (double) G_USEC_PER_SEC / elapsed_window_us;
  double bps = window_bytes * (double) G_USEC_PER_SEC / elapsed_window_us;

  if (mean_us < 0.0)
    {
      mean_us = us;
      mean_fence_us = fence;
      mean_rps = rps;
      mean_bps = bps;
    }
  else
    {
      mean_us = alpha * us + (1.0 - alpha) * mean_us;
      mean_fence_us = alpha * fence + (1.0 - alpha) * mean_fence_us;
      mean_rps = alpha * rps + (1.0 - alpha) * mean_rps;
      mean_bps = alpha * bps + (1.0 - alpha) * mean_bps;
    }

  /* The last figure is the share of wall-clock time the main loop spent
   * blocked in glReadPixels; at 60fps anything approaching 100% means the
   * compositor is doing nothing but readback. */
  meta_topic (META_DEBUG_REMOTE_DESKTOP,
              "rdp: readback %.0f us/read (mean %.0f, peak %.0f), "
              "%.0f us fence (mean %.0f), "
              "%.1f reads/s (mean %.1f), %.2f MB/s (mean %.2f), %.1f%% of wall",
              us, mean_us, (double) window_peak_us,
              fence, mean_fence_us,
              rps, mean_rps,
              bps / (1024.0 * 1024.0), mean_bps / (1024.0 * 1024.0),
              100.0 * (window_us + window_fence_us) / elapsed_window_us);

  window_start_us = now_us;
  window_us = 0;
  window_fence_us = 0;
  window_peak_us = 0;
  window_bytes = 0;
  window_reads = 0;
}

/* One-shot diagnostic, run when META_RDP_PROBE_FORMATS is set in the
 * environment: which pixel format can this framebuffer be read back into
 * without the driver inserting a staging blit?
 *
 * Background, because the answer is not where it looks like it should be.
 * Cogl has its own conversion path (cogl-framebuffer-gl.c:477: read into a
 * malloc'd temp, convert on the CPU), but on the GL3 driver it is unreachable
 * for our purposes: cogl_driver_gl3_get_read_pixels_format() ignores the
 * framebuffer's internal format and returns the format the *caller* asked for
 * (gl3/cogl-driver-gl3.c:445-456), so format_mismatch is always false. Cogl
 * hands whatever we ask for straight to glReadPixels.
 *
 * The expensive mismatch is one layer down, in Mesa:
 * _mesa_format_matches_format_and_type() (formats.c:1119, called from
 * readpix.c:230 and st_cb_readpixels.c:522) is a strict equality between the
 * renderbuffer's mesa_format and _mesa_format_from_format_and_type() of the
 * GL format+type pair cogl emitted. Miss it and st_ReadPixels runs a full
 * pipe->blit() into a freshly allocated staging texture before the transfer
 * even begins.
 *
 * We cannot query the renderbuffer's mesa_format from here -- this cogl has no
 * epoxy and mutter links no GL directly -- but we do not need to. Timing the
 * candidates identifies the match: the one that skips the blit is
 * substantially faster, and that is the property we actually care about.
 *
 * Only four candidates, not eight. cogl_driver_gl3_pixel_format_to_gl()
 * (gl3/cogl-driver-gl3.c:139-198) emits the same GL format+type pair for a
 * format and its X-variant -- BGRX_8888 and BGRA_8888 both give
 * GL_BGRA + GL_UNSIGNED_BYTE -- so they are indistinguishable to glReadPixels
 * and only these four GL pairs exist for 32bpp. (Corollary worth knowing: if
 * the renderbuffer turns out to be an X format such as B8G8R8X8_UNORM, *no*
 * cogl format can match it, since _mesa_format_from_format_and_type() never
 * yields an X format. The blit would then be unavoidable through this API.)
 *
 * Costs a few full-frame readbacks, so it is opt-in and runs once. */
static void
meta_rdp_probe_readback_formats (CoglFramebuffer *framebuffer)
{
  static const struct
  {
    CoglPixelFormat format;
    const char *name;
    const char *gl_pair;
    const char *bytes;
  } candidates[] = {
    { COGL_PIXEL_FORMAT_BGRA_8888_PRE, "BGRA_8888_PRE",
      "GL_BGRA + GL_UNSIGNED_BYTE       ", "B,G,R,A" },
    { COGL_PIXEL_FORMAT_RGBA_8888_PRE, "RGBA_8888_PRE",
      "GL_RGBA + GL_UNSIGNED_BYTE       ", "R,G,B,A" },
    { COGL_PIXEL_FORMAT_ARGB_8888_PRE, "ARGB_8888_PRE",
      "GL_BGRA + GL_UNSIGNED_INT_8_8_8_8", "A,R,G,B" },
    { COGL_PIXEL_FORMAT_ABGR_8888_PRE, "ABGR_8888_PRE",
      "GL_RGBA + GL_UNSIGNED_INT_8_8_8_8", "A,B,G,R" },
  };
  /* Enough to see past one-off scheduling noise; we report the minimum, which
   * is the honest figure for "what does this cost when nothing interferes". */
  const int n_runs = 5;
  static gboolean probed = FALSE;
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);
  g_autofree uint8_t *scratch = NULL;
  int64_t best_us = G_MAXINT64;
  const char *best_name = NULL;
  size_t i;

  if (probed || !g_getenv ("META_RDP_PROBE_FORMATS"))
    return;

  probed = TRUE;

  if (width <= 0 || height <= 0)
    return;

  scratch = g_malloc ((size_t) width * height * 4);

  g_message ("rdp: probing readback formats at %dx%d (%d runs each); the "
             "fastest is the one Mesa can memcpy without blit_to_staging",
             width, height, n_runs);

  for (i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      int64_t min_us = G_MAXINT64;
      int64_t total_us = 0;
      gboolean ok = TRUE;
      int run;

      for (run = 0; run < n_runs && ok; run++)
        {
          CoglBitmap *bitmap;
          int64_t started_us;

          bitmap = cogl_bitmap_new_for_data (cogl_context,
                                             width, height,
                                             candidates[i].format,
                                             width * 4,
                                             scratch);

          started_us = g_get_monotonic_time ();
          ok = cogl_framebuffer_read_pixels_into_bitmap (framebuffer, 0, 0,
                                                         COGL_READ_PIXELS_COLOR_BUFFER,
                                                         bitmap);
          if (ok)
            {
              int64_t elapsed_us = g_get_monotonic_time () - started_us;

              total_us += elapsed_us;
              if (elapsed_us < min_us)
                min_us = elapsed_us;
            }

          g_object_unref (bitmap);
        }

      if (!ok)
        {
          g_message ("rdp:   %-14s %s  bytes %-7s  FAILED",
                     candidates[i].name, candidates[i].gl_pair,
                     candidates[i].bytes);
          continue;
        }

      g_message ("rdp:   %-14s %s  bytes %-7s  min %6" G_GINT64_FORMAT " us, "
                 "mean %6" G_GINT64_FORMAT " us",
                 candidates[i].name, candidates[i].gl_pair,
                 candidates[i].bytes, min_us, total_us / n_runs);

      if (min_us < best_us)
        {
          best_us = min_us;
          best_name = candidates[i].name;
        }
    }

  if (best_name)
    {
      /* Interpreting this: only one candidate can equal the renderbuffer's
       * mesa_format, so a genuine match shows up as *one* clearly faster row.
       * Rows within noise of each other mean no candidate matches (or the blit
       * is cheap next to the fence stall) -- which is what this reported when
       * it was first run, and why the gfxredir format change was dropped. See
       * READBACK-PLAN.md step 1c. */
      g_message ("rdp: fastest readback format is %s (%" G_GINT64_FORMAT " us); "
                 "currently reading as BGRA_8888_PRE. A lone clear winner means "
                 "blit_to_staging is firing for the others and the gfxredir "
                 "buffer format should follow it; a tie means the format is not "
                 "where the time goes.", best_name, best_us);
    }
}

/* One-shot self-test for the readback PBO, run when META_RDP_PROBE_PBO is set.
 *
 * The point is to find out which memory the driver puts a readback PBO in
 * *before* the async readback is built on top of it. On d3d12 a buffer created
 * with a DRAW usage hint lands on a D3D12_HEAP_TYPE_DEFAULT heap, which
 * can_map_directly() rejects, so mapping it for reading allocates a staging
 * buffer, copies through it and blocks on a fence -- the exact stall the PBO is
 * meant to remove. That failure is invisible from here: the pixels still come
 * back correct, just slowly.
 *
 * Normal compositor traffic cannot answer this. The only buffers cogl creates
 * by itself are journal vertex buffers (GL_ARRAY_BUFFER, and only when the
 * journal's VBO pool has to grow), so watching glBufferData tells us nothing
 * about the pixel-pack path. This creates one deliberately.
 *
 * Creating the buffer is not enough on its own -- cogl defers glBufferData to
 * the first bind/map (recreate_store()), and the d3d12 resource is only
 * allocated at that point. So map it too, which is also the operation whose
 * cost we care about.
 *
 * Run with COGL_DEBUG_BUFFER_USAGE=1 to see cogl's usage enum (expect 0x88E1,
 * GL_STREAM_READ) and D3D12_DEBUG_BUFFER_USAGE=1 to see where it landed (expect
 * usage=STAGING heap=READBACK mappable=yes). */
static void
meta_rdp_probe_readback_pbo (CoglFramebuffer *framebuffer)
{
  static gboolean probed = FALSE;
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);
  size_t size = (size_t) width * height * 4;
  CoglPixelBuffer *pbo;
  int64_t started_us;
  void *data;

  if (probed || !g_getenv ("META_RDP_PROBE_PBO"))
    return;

  probed = TRUE;

  if (width <= 0 || height <= 0)
    return;

  g_message ("rdp: probing a %zu-byte readback PBO", size);

  pbo = cogl_pixel_buffer_new_for_readback (cogl_context, size);
  if (!pbo)
    {
      g_warning ("rdp: could not create a readback PBO");
      return;
    }

  /* Map repeatedly, and report the first separately from the rest.
   *
   * The two are different costs and only the second one matters. cogl defers
   * glBufferData to the first bind/map (recreate_store()), so the first map
   * also pays for allocating the resource -- on a cold pb_cache that is a real
   * CreateCommittedResource for the whole frame. Steady state is what step 4
   * pays per frame, once the ring has been allocated and is being reused.
   *
   * If the two are close, the buffer is not really being reused and step 4's
   * ring is not doing its job. If the first is large and the rest are small,
   * that is the expected shape and it confirms the ring must be persistent. */
  const int n_maps = 5;
  int64_t first_us = 0;
  int64_t rest_us = 0;

  for (int i = 0; i < n_maps; i++)
    {
      started_us = g_get_monotonic_time ();
      data = cogl_buffer_map (COGL_BUFFER (pbo), COGL_BUFFER_ACCESS_READ, 0);
      if (!data)
        {
          g_warning ("rdp: readback PBO could not be mapped for reading");
          g_object_unref (pbo);
          return;
        }

      if (i == 0)
        first_us = g_get_monotonic_time () - started_us;
      else
        rest_us += g_get_monotonic_time () - started_us;

      cogl_buffer_unmap (COGL_BUFFER (pbo));
    }

  g_message ("rdp: readback PBO first map %" G_GINT64_FORMAT " us "
             "(includes allocation), subsequent %" G_GINT64_FORMAT " us mean",
             first_us, rest_us / (n_maps - 1));

  g_object_unref (pbo);
}

/* Companion meter for the asynchronous path, reported like the one above.
 *
 * Three numbers, because they answer different questions:
 *   - copy: how long the memcpy out of the mapped PBO takes. This is the cost
 *     the asynchronous path *adds*; the direct path wrote into shared memory
 *     with no copy at all. If it approaches the synchronous readback time the
 *     whole exercise is pointless.
 *   - latency: issue to collect. How stale the frame the client sees is, and
 *     whether the fence is signalling within a frame or dragging.
 *   - blocking: how many collections had to block because the fence had not
 *     signalled by the time we ran out of patience. Should be zero. */
static void
meta_rdp_account_readback_collect (int64_t  copy_us,
                                   int64_t  latency_us,
                                   size_t   bytes,
                                   gboolean blocked)
{
  static const double alpha = 0.2;
  static int64_t window_start_us = 0;
  static int64_t window_copy_us = 0;
  static int64_t window_latency_us = 0;
  static size_t window_bytes = 0;
  static unsigned window_collects = 0;
  static unsigned window_blocked = 0;
  static double mean_copy_us = -1.0;
  static double mean_latency_us = -1.0;

  int64_t now_us;
  int64_t elapsed_us;

  if (!meta_is_topic_enabled (META_DEBUG_REMOTE_DESKTOP))
    return;

  now_us = g_get_monotonic_time ();
  window_copy_us += copy_us;
  window_latency_us += latency_us;
  window_bytes += bytes;
  window_collects++;
  if (blocked)
    window_blocked++;

  if (window_start_us == 0)
    {
      window_start_us = now_us;
      return;
    }

  elapsed_us = now_us - window_start_us;
  if (elapsed_us < G_USEC_PER_SEC)
    return;

  double copy = (double) window_copy_us / window_collects;
  double latency = (double) window_latency_us / window_collects;

  if (mean_copy_us < 0.0)
    {
      mean_copy_us = copy;
      mean_latency_us = latency;
    }
  else
    {
      mean_copy_us = alpha * copy + (1.0 - alpha) * mean_copy_us;
      mean_latency_us = alpha * latency + (1.0 - alpha) * mean_latency_us;
    }

  meta_topic (META_DEBUG_REMOTE_DESKTOP,
              "rdp: collect %.0f us copy (mean %.0f), %.0f us latency "
              "(mean %.0f), %.1f collects/s, %.2f MB/s, %u blocked, "
              "%.1f%% of wall",
              copy, mean_copy_us, latency, mean_latency_us,
              window_collects * (double) G_USEC_PER_SEC / elapsed_us,
              window_bytes * (double) G_USEC_PER_SEC / elapsed_us /
              (1024.0 * 1024.0),
              window_blocked,
              100.0 * window_copy_us / elapsed_us);

  window_start_us = now_us;
  window_copy_us = 0;
  window_latency_us = 0;
  window_bytes = 0;
  window_collects = 0;
  window_blocked = 0;
}

/* Read the @width x @height region at (@x, @y) of @framebuffer into @dest
 * (ARGB8888, i.e. BGRA byte order in memory, which is what gfxredir's
 * ARGB_8888 and an uncompressed SURFACE_BITS bitmap both expect on
 * little-endian).
 *
 * Reading only the damaged region matters: this is a synchronous GPU->CPU
 * transfer that stalls the pipeline, and a full 1920x1080 frame is ~8MB even
 * when a single button repainted. @stride lets the caller land the region
 * directly inside a larger destination buffer.
 *
 * The result is top-down. Cogl already accounts for the GL bottom-left origin
 * when reading into a bitmap, so no flip belongs here -- consumers that want
 * bottom-up data (the raw SURFACE_BITS path) flip as they pack their rows.
 * Returns FALSE on failure. */
static gboolean
meta_rdp_read_framebuffer (CoglFramebuffer *framebuffer,
                           uint8_t         *dest,
                           int              x,
                           int              y,
                           int              width,
                           int              height,
                           int              stride)
{
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  CoglBitmap *bitmap;
  int64_t started_us;
  gboolean ok;

  meta_rdp_probe_readback_formats (framebuffer);
  meta_rdp_probe_readback_pbo (framebuffer);

  bitmap = cogl_bitmap_new_for_data (cogl_context,
                                     width, height,
                                     COGL_PIXEL_FORMAT_BGRA_8888_PRE,
                                     stride,
                                     dest);

  /* Time only the transfer itself, not the bitmap wrapper around it. */
  started_us = g_get_monotonic_time ();
  ok = cogl_framebuffer_read_pixels_into_bitmap (framebuffer,
                                                 x, y,
                                                 COGL_READ_PIXELS_COLOR_BUFFER,
                                                 bitmap);
  if (ok)
    {
      meta_rdp_account_readback (g_get_monotonic_time () - started_us, 0,
                                 (size_t) width * height * 4);
    }

  g_object_unref (bitmap);

  return ok;
}

/* ------------------------------------------------------------------ */
/* Error frame                                                          */
/*                                                                      */
/* gfxredir is the only way the desktop is sent. Encoding frames into   */
/* SURFACE_BITS instead would work after a fashion, but at a fraction   */
/* of the speed, and a session that silently runs that slowly is worse  */
/* than one that plainly does not run. So when gfxredir cannot be used, */
/* the client gets a magenta frame with the reason written on it.       */
/* ------------------------------------------------------------------ */

/* What the error frame suggests, by where things went wrong. */
#define META_RDP_HINT_SYSTEM_DISTRO \
  "Is systemDistro= in .wslconfig the weaselway system image, followed by wsl --shutdown?"
#define META_RDP_HINT_CLIENT \
  "Connect with weaselway's sdl-freerdp.exe and /wslgsharedmemorypath:, as start-viewer does."

/* Sent as raw SURFACE_BITS in bands, each small enough to fit the client's
 * reassembly limit (FreeRDP_MultifragMaxRequestSize) with room for headers. */
#define META_RDP_ERROR_BAND_HEADROOM 1024

static void
meta_rdp_error_frame_draw_line (cairo_t    *cr,
                                const char *text,
                                double      size,
                                double      margin,
                                double      max_width,
                                double     *y)
{
  cairo_text_extents_t extents;

  cairo_set_font_size (cr, size);
  cairo_text_extents (cr, text, &extents);
  /* Shrink rather than clip: a narrow window still gets the whole message. */
  if (extents.x_advance > max_width)
    {
      size *= max_width / extents.x_advance;
      cairo_set_font_size (cr, size);
    }

  *y += size * 1.5;
  cairo_move_to (cr, margin, *y);
  cairo_show_text (cr, text);
}

static void
meta_rdp_present_error (MetaRdpPeerContext *peer_ctx,
                        CoglFramebuffer    *framebuffer)
{
  freerdp_peer *client = peer_ctx->peer;
  rdpUpdate *update = client->context->update;
  rdpSettings *settings = client->context->settings;
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);
  cairo_surface_t *surface;
  cairo_t *cr;
  const uint8_t *pixels;
  g_autofree uint8_t *band = NULL;
  uint32_t max_bytes;
  int stride, band_rows;
  double size, margin, max_width, y = 0;

  /* The frame is static, so once is enough until the size changes. */
  if (peer_ctx->error_frame_width == width &&
      peer_ctx->error_frame_height == height)
    return;

  surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width, height);
  cr = cairo_create (surface);

  cairo_set_source_rgb (cr, 1.0, 0.0, 1.0);
  cairo_paint (cr);

  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
  cairo_select_font_face (cr, "sans-serif",
                          CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  size = MAX (12.0, width / 40.0);
  margin = size;
  max_width = width - 2 * margin;
  meta_rdp_error_frame_draw_line (cr, "weaselway: no shared-memory graphics",
                                  size, margin, max_width, &y);
  cairo_select_font_face (cr, "sans-serif",
                          CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  y += size / 2;
  meta_rdp_error_frame_draw_line (cr, peer_ctx->error_reason,
                                  size * 0.6, margin, max_width, &y);
  if (peer_ctx->error_hint)
    meta_rdp_error_frame_draw_line (cr, peer_ctx->error_hint,
                                    size * 0.6, margin, max_width, &y);
  y += size / 2;
  meta_rdp_error_frame_draw_line (cr, "The mutter log has the details: "
                                  "journalctl --user -u 'org.gnome.Shell@*'",
                                  size * 0.5, margin, max_width, &y);

  cairo_destroy (cr);
  cairo_surface_flush (surface);

  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      g_warning ("rdp: drawing the error frame failed: %s",
                 cairo_status_to_string (cairo_surface_status (surface)));
      cairo_surface_destroy (surface);
      return;
    }

  pixels = cairo_image_surface_get_data (surface);
  stride = cairo_image_surface_get_stride (surface);

  max_bytes = freerdp_settings_get_uint32 (settings,
                                           FreeRDP_MultifragMaxRequestSize);
  band_rows = 1;
  if (max_bytes > META_RDP_ERROR_BAND_HEADROOM + (uint32_t) width * 4)
    band_rows = (max_bytes - META_RDP_ERROR_BAND_HEADROOM) / (width * 4);
  band_rows = MIN (band_rows, height);
  band = g_malloc ((size_t) width * 4 * band_rows);

  /* CAIRO_FORMAT_RGB24 is BGRX in memory on little endian, which is what a
   * 32 bpp RDP bitmap is. Raw SURFACE_BITS bitmaps are bottom-up, so each band
   * is flipped as it is packed. */
  for (int top = 0; top < height; top += band_rows)
    {
      SURFACE_BITS_COMMAND cmd = { 0 };
      int rows = MIN (band_rows, height - top);

      for (int r = 0; r < rows; r++)
        memcpy (band + (size_t) r * width * 4,
                pixels + (size_t) (top + rows - 1 - r) * stride,
                (size_t) width * 4);

      cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
      cmd.destLeft = 0;
      cmd.destTop = top;
      cmd.destRight = width;
      cmd.destBottom = top + rows;
      cmd.bmp.bpp = 32;
      cmd.bmp.codecID = 0;
      cmd.bmp.width = width;
      cmd.bmp.height = rows;
      cmd.bmp.bitmapDataLength = width * 4 * rows;
      cmd.bmp.bitmapData = band;

      if (!update->SurfaceBits (update->context, &cmd))
        {
          g_warning ("rdp: sending the error frame failed");
          break;
        }
    }

  cairo_surface_destroy (surface);

  peer_ctx->error_frame_width = width;
  peer_ctx->error_frame_height = height;
  g_message ("rdp: sent the error frame (%dx%d)", width, height);
}

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

  if (fallocate (fd, 0, 0, (off_t) size) < 0)
    {
      /* EINVAL here is most often a zero length rather than anything to do
       * with the filesystem, so say what was asked for. */
      g_warning ("rdp: fallocate shm %zu bytes failed: %s",
                 size, g_strerror (errno));
      goto error;
    }

  addr = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
    {
      g_warning ("rdp: mmap shm %zu bytes failed: %s",
                 size, g_strerror (errno));
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
    {
      close (fd);
      /* The file exists from here on; without this a failed allocation leaves
       * it behind in the shared-memory mount forever, since nothing else
       * knows its name. */
      unlink (path);
    }
  peer_ctx->shm_name[0] = '\0';
  return FALSE;
}

/* Defined with the rest of the asynchronous readback machinery below; needed
 * here because tearing the pool down has to abandon any readback aimed at it. */
static void meta_rdp_readback_cancel (MetaRdpPeerContext *peer_ctx);
static void meta_rdp_readback_free (MetaRdpPeerContext *peer_ctx);

static void
meta_rdp_destroy_buffer (MetaRdpPeerContext *peer_ctx)
{
  GfxRedirServerContext *redir = peer_ctx->gfxredir;

  if (!peer_ctx->buffer_created)
    return;

  /* Before anything else: a readback issued against this pool is about to have
   * its destination unmapped. Drop it rather than let it complete into a freed
   * mapping. */
  meta_rdp_readback_cancel (peer_ctx);

  if (redir)
    {
      GFXREDIR_CLOSE_POOL_PDU close_pool = { 0 };
      int i;

      for (i = 0; i < META_RDP_N_BUFFERS; i++)
        {
          GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer = { 0 };

          destroy_buffer.bufferId = META_RDP_BUFFER_ID (i);
          g_message ("rdp: gfxredir -> DestroyBuffer bufferId=%" G_GUINT64_FORMAT,
                     (uint64_t) destroy_buffer.bufferId);
          redir->DestroyBuffer (redir, &destroy_buffer);
        }

      close_pool.poolId = META_RDP_POOL_ID;
      g_message ("rdp: gfxredir -> ClosePool poolId=%" G_GUINT64_FORMAT,
                 (uint64_t) close_pool.poolId);
      redir->ClosePool (redir, &close_pool);
    }

  meta_rdp_free_shared_memory (peer_ctx);

  /* Every present issued so far named a buffer that no longer exists. Their
   * acks may still be in flight -- or may never arrive at all, if the client
   * discarded them along with the pool -- so refuse to retire anything at or
   * below this id; the counters are reset below regardless. */
  g_mutex_lock (&peer_ctx->gfxredir_mutex);
  peer_ctx->present_id_floor = peer_ctx->current_frame_id;
  peer_ctx->gfxredir_n_acked = 0;
  g_mutex_unlock (&peer_ctx->gfxredir_mutex);

  for (int i = 0; i < META_RDP_N_BUFFERS; i++)
    {
      g_clear_pointer (&peer_ctx->buffers[i].stale, mtk_region_unref);
      peer_ctx->buffers[i].in_flight = FALSE;
      peer_ctx->buffers[i].present_id = 0;
    }

  peer_ctx->buffer_created = FALSE;
  peer_ctx->next_buffer = 0;
  peer_ctx->last_written = -1;
  peer_ctx->n_presents_inflight = 0;
}

/* Create the pool and its buffers, sized to the output (once / on resize). */
static gboolean
meta_rdp_ensure_buffer (MetaRdpPeerContext *peer_ctx,
                        int                 width,
                        int                 height)
{
  GfxRedirServerContext *redir = peer_ctx->gfxredir;
  int stride = width * 4;
  size_t size = (size_t) stride * height;
  /* Buffers are spaced a whole number of pages apart rather than packed back
   * to back.
   *
   * Two reasons. The pool is a mapped file, so its total length has to be page
   * aligned -- fallocate() on the virtio-fs/DAX mount rejects anything else
   * with EINVAL, and an odd height alone is enough to break that. Weston
   * rounds the same way (rdprail.c, copy_buffer_size).
   *
   * The spacing also fixes the alignment of meta_rdp_copy_between_buffers():
   * it memcpy()s row by row between two buffers at identical positions, so the
   * source and destination differ by exactly the gap between them. Packed
   * back to back that gap is stride * height, which shares alignment only when
   * the stride happens to be a multiple of 64 -- true at width 1920, false at
   * an arbitrary client width (1009 gives a stride of 4036, 4 mod 64). A page
   * multiple is aligned by construction, so both sides always agree.
   *
   * Only the spacing changes; each buffer still holds exactly stride * height
   * bytes and the slack sits unused at its end. The client is told the real
   * offsets in CREATE_BUFFER, so it needs to know nothing about this. */
  size_t page_size = (size_t) sysconf (_SC_PAGESIZE);
  size_t buffer_pitch = (size + page_size - 1) & ~(page_size - 1);
  size_t pool_size = buffer_pitch * META_RDP_N_BUFFERS;
  unsigned short section_name[META_RDP_SHARED_MEMORY_NAME_SIZE + 1];
  GFXREDIR_OPEN_POOL_PDU open_pool = { 0 };
  uint32_t i;

  if (width <= 0 || height <= 0)
    {
      /* Nothing sane to allocate. Bail here rather than let it reach
       * fallocate(), which reports a zero length as a bare EINVAL. */
      g_warning ("rdp: refusing to create a %dx%d gfxredir pool", width, height);
      return FALSE;
    }

  if (peer_ctx->buffer_created &&
      peer_ctx->buffer_width == width && peer_ctx->buffer_height == height)
    return TRUE;

  if (peer_ctx->buffer_created)
    meta_rdp_destroy_buffer (peer_ctx);

  if (!meta_rdp_allocate_shared_memory (peer_ctx, pool_size))
    return FALSE;

  /* Linux wchar_t is 4 bytes; Windows wants 2-byte wchar for sectionName. */
  for (i = 0; i < META_RDP_SHARED_MEMORY_NAME_SIZE; i++)
    section_name[i] = (unsigned short) peer_ctx->shm_name[i];
  section_name[META_RDP_SHARED_MEMORY_NAME_SIZE] = 0;

  open_pool.poolId = META_RDP_POOL_ID;
  open_pool.poolSize = pool_size;
  open_pool.sectionNameLength = META_RDP_SHARED_MEMORY_NAME_SIZE + 1;
  open_pool.sectionName = section_name;
  g_message ("rdp: gfxredir -> OpenPool poolId=%" G_GUINT64_FORMAT
             " size=%" G_GUINT64_FORMAT " section=%s",
             (uint64_t) open_pool.poolId, (uint64_t) open_pool.poolSize,
             peer_ctx->shm_name);
  if (redir->OpenPool (redir, &open_pool) != 0)
    {
      g_warning ("rdp: gfxredir OpenPool failed");
      meta_rdp_free_shared_memory (peer_ctx);
      return FALSE;
    }

  for (i = 0; i < META_RDP_N_BUFFERS; i++)
    {
      GFXREDIR_CREATE_BUFFER_PDU create_buffer = { 0 };
      size_t offset = (size_t) i * buffer_pitch;

      create_buffer.poolId = META_RDP_POOL_ID;
      create_buffer.bufferId = META_RDP_BUFFER_ID (i);
      create_buffer.offset = offset;
      create_buffer.stride = stride;
      create_buffer.width = width;
      create_buffer.height = height;
      create_buffer.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
      g_message ("rdp: gfxredir -> CreateBuffer bufferId=%" G_GUINT64_FORMAT
                 " poolId=%" G_GUINT64_FORMAT " %dx%d stride=%d offset=%"
                 G_GUINT64_FORMAT " format=ARGB_8888",
                 (uint64_t) create_buffer.bufferId, (uint64_t) create_buffer.poolId,
                 width, height, stride, (uint64_t) create_buffer.offset);
      if (redir->CreateBuffer (redir, &create_buffer) != 0)
        {
          GFXREDIR_CLOSE_POOL_PDU close_pool = { 0 };

          g_warning ("rdp: gfxredir CreateBuffer failed");
          close_pool.poolId = META_RDP_POOL_ID;
          redir->ClosePool (redir, &close_pool);
          meta_rdp_free_shared_memory (peer_ctx);
          return FALSE;
        }

      peer_ctx->buffers[i].offset = offset;
      peer_ctx->buffers[i].in_flight = FALSE;
      peer_ctx->buffers[i].present_id = 0;
      g_clear_pointer (&peer_ctx->buffers[i].stale, mtk_region_unref);
    }

  peer_ctx->buffer_created = TRUE;
  peer_ctx->buffer_width = width;
  peer_ctx->buffer_height = height;
  peer_ctx->buffer_stride = stride;
  peer_ctx->buffer_size = size;
  peer_ctx->next_buffer = 0;
  /* Nothing has been read back yet, so no buffer holds valid contents and
   * there is nothing to copy from; the first present writes a full frame. */
  peer_ctx->last_written = -1;
  peer_ctx->n_presents_inflight = 0;
  g_message ("rdp: gfxredir %d buffers created %dx%d (stride %d, pool %zu bytes)",
             META_RDP_N_BUFFERS, width, height, stride, pool_size);
  return TRUE;
}

/* Copy a region between two buffers in the pool.
 *
 * Both live in the same mapping at the same stride, so this is a row-wise
 * memcpy. Used to fill in the frames a buffer sat out; far cheaper than
 * re-reading those pixels from the GPU. */
static void
meta_rdp_copy_between_buffers (MetaRdpPeerContext *peer_ctx,
                               int                 src_index,
                               int                 dst_index,
                               const MtkRegion    *region)
{
  uint8_t *base = peer_ctx->shm_addr;
  const uint8_t *src = base + peer_ctx->buffers[src_index].offset;
  uint8_t *dst = base + peer_ctx->buffers[dst_index].offset;
  int stride = peer_ctx->buffer_stride;
  int n_rects = mtk_region_num_rectangles (region);

  for (int i = 0; i < n_rects; i++)
    {
      MtkRectangle r = mtk_region_get_rectangle (region, i);
      size_t row_bytes = (size_t) r.width * 4;

      for (int y = r.y; y < r.y + r.height; y++)
        {
          size_t row = (size_t) y * stride + (size_t) r.x * 4;

          memcpy (dst + row, src + row, row_bytes);
        }
    }
}

/* Pick a buffer nobody else is using, or -1 if there is none.
 *
 * Two ways a buffer can be unavailable, not one: the client may still be
 * reading it (@in_flight), or it may be the destination of a readback we have
 * issued but whose pixels have not landed yet. The second case has no flag of
 * its own because at most one readback is outstanding -- it is simply the
 * buffer named by @readback_buffer. Handing that one out again would let the
 * next frame's stale-fill and readback race the copy-out of the previous one. */
static int
meta_rdp_acquire_buffer (MetaRdpPeerContext *peer_ctx)
{
  for (int n = 0; n < META_RDP_N_BUFFERS; n++)
    {
      int i = (peer_ctx->next_buffer + n) % META_RDP_N_BUFFERS;

      if (peer_ctx->buffers[i].in_flight)
        continue;

      if (peer_ctx->readback_pending && peer_ctx->readback_buffer == i)
        continue;

      peer_ctx->next_buffer = (i + 1) % META_RDP_N_BUFFERS;
      return i;
    }

  return -1;
}

/* ------------------------------------------------------------------ */
/* Asynchronous readback                                               */
/*                                                                     */
/* glReadPixels straight into shared memory blocks the main loop for as */
/* long as the GPU takes to hand the pixels over -- measured at ~10ms   */
/* per frame, around 60% of wall-clock time, which is time the          */
/* compositor is not painting or servicing input.                       */
/*                                                                     */
/* Instead the readback targets a persistent pixel buffer object on a   */
/* driver readback heap, followed by a fence. Issuing costs nothing;    */
/* the pixels are collected on a later main-loop pass, once the fence   */
/* says the transfer is done, and only then copied into the shared      */
/* buffer and presented.                                                */
/*                                                                     */
/* The cost this adds is that copy: the pixels land in the PBO and have */
/* to be memcpy'd into shared memory, which the direct path did not do. */
/* Off a readback heap that is an ordinary CPU-side copy of the damage  */
/* rect, paid instead of a full pipeline stall.                          */
/*                                                                     */
/* At most one readback is outstanding at a time. A ring would allow    */
/* frame N+1 to be issued while N is still landing, but at 60fps the    */
/* fence has essentially always signalled by the next frame, and the    */
/* bookkeeping for out-of-order completion is a large amount of state   */
/* for a case that does not arise. When it does arise, the damage       */
/* coalesces exactly as it already does when every buffer is busy.       */
/* ------------------------------------------------------------------ */

/* Poll interval while waiting for the fence. GLib rounds poll timeouts up to
 * whole milliseconds, so this is the floor regardless of what we ask for. */
/* D3D12_TEXTURE_DATA_PITCH_ALIGNMENT. Not d3d12-specific in principle -- it is
 * whatever the driver reports as texture_to_buffer_copy_row_alignment -- but
 * there is no GL way to ask, and 256 is the largest value in use, so aligning
 * to it satisfies every driver that supports the copy at all. */
#define META_RDP_READBACK_ALIGN_BYTES 256

#define META_RDP_READBACK_POLL_MS 1
/* Fallback interval when there is no fence to poll: the collect then happens at
 * the next frame, and this only has to cover the case where none comes. */
#define META_RDP_READBACK_IDLE_MS 16
/* Give up polling and block after this many polls (~200ms). Only reachable if
 * a fence never signals, which should not happen -- cogl_gpu_fence_new() has
 * already flushed. Bounded so a lost fence degrades to the old synchronous
 * behaviour instead of freezing the session one frame stale. */
#define META_RDP_READBACK_MAX_POLLS 200

static void meta_rdp_gfxredir_send_present (MetaRdpPeerContext *peer_ctx,
                                            int                 index,
                                            const MtkRectangle *damage);

static void
meta_rdp_readback_disarm (MetaRdpPeerContext *peer_ctx)
{
  GSource *current;

  if (peer_ctx->readback_poll_id == 0)
    return;

  /* The collect paths below can be reached either from the frame clock or from
   * inside the poll callback itself. In the latter case g_source_remove() would
   * destroy the source while it is dispatching; clearing the id is enough,
   * because the callback returns G_SOURCE_REMOVE whenever it finds it cleared. */
  current = g_main_current_source ();
  if (current && g_source_get_id (current) == peer_ctx->readback_poll_id)
    peer_ctx->readback_poll_id = 0;
  else
    g_clear_handle_id (&peer_ctx->readback_poll_id, g_source_remove);
}

/* Abandon an outstanding readback without collecting it.
 *
 * Used when the thing it was going to be written into is going away: a resize
 * tears down the pool and unmaps the shared memory, and completing afterwards
 * would copy into a freed mapping. The pixels are simply dropped; the caller is
 * in the middle of invalidating everything anyway. */
static void
meta_rdp_readback_cancel (MetaRdpPeerContext *peer_ctx)
{
  if (!peer_ctx->readback_pending)
    return;

  g_debug ("rdp: cancelling in-flight readback into buffer %d",
           peer_ctx->readback_buffer);

  meta_rdp_readback_disarm (peer_ctx);
  g_clear_pointer (&peer_ctx->readback_fence, cogl_gpu_fence_free);
  peer_ctx->readback_pending = FALSE;
  peer_ctx->readback_buffer = -1;
  peer_ctx->readback_polls = 0;
}

static void
meta_rdp_readback_free (MetaRdpPeerContext *peer_ctx)
{
  meta_rdp_readback_cancel (peer_ctx);
  g_clear_object (&peer_ctx->readback_pbo);
  peer_ctx->readback_pbo_size = 0;
}

/* Collect a finished readback: copy the PBO into the shared buffer and present.
 *
 * @force maps regardless of the fence, which blocks until the transfer
 * completes. Only the bounded fallback and teardown use that. */
static gboolean
meta_rdp_readback_finish (MetaRdpPeerContext *peer_ctx,
                          gboolean            force)
{
  CoglBuffer *buffer;
  MtkRectangle rect;
  const uint8_t *src;
  uint8_t *dst;
  size_t src_stride;
  int index;
  int64_t started_us;
  int64_t copied_us;

  if (!peer_ctx->readback_pending)
    return FALSE;

  /* No fence means cogl had no GL_ARB_sync; such a readback is always
   * collected immediately with force=TRUE, so there is nothing to poll. */
  if (!force &&
      (!peer_ctx->readback_fence ||
       !cogl_gpu_fence_is_signalled (peer_ctx->readback_fence)))
    return FALSE;

  buffer = COGL_BUFFER (peer_ctx->readback_pbo);
  index = peer_ctx->readback_buffer;
  rect = peer_ctx->readback_rect;

  /* The pool can have been torn down while this was in flight (resize); if so
   * there is nowhere to put the pixels. */
  if (!peer_ctx->buffer_created || !peer_ctx->shm_addr ||
      index < 0 || index >= META_RDP_N_BUFFERS)
    {
      meta_rdp_readback_cancel (peer_ctx);
      return FALSE;
    }

  started_us = g_get_monotonic_time ();

  src = cogl_buffer_map (buffer, COGL_BUFFER_ACCESS_READ, 0);
  if (!src)
    {
      g_warning ("rdp: could not map the readback PBO; dropping the frame");
      meta_rdp_readback_cancel (peer_ctx);
      return FALSE;
    }

  /* The readback packed the rect tightly, so its rows are rect.width * 4 apart;
   * the destination rows are a full frame apart. Row by row either way. */
  src_stride = (size_t) rect.width * 4;
  dst = (uint8_t *) peer_ctx->shm_addr +
        peer_ctx->buffers[index].offset +
        (size_t) rect.y * peer_ctx->buffer_stride +
        (size_t) rect.x * 4;

  for (int y = 0; y < rect.height; y++)
    {
      memcpy (dst + (size_t) y * peer_ctx->buffer_stride,
              src + (size_t) y * src_stride,
              src_stride);
    }

  cogl_buffer_unmap (buffer);

  copied_us = g_get_monotonic_time ();
  meta_rdp_account_readback_collect (copied_us - started_us,
                                     copied_us - peer_ctx->readback_issued_us,
                                     (size_t) rect.width * rect.height * 4,
                                     force);

  meta_rdp_readback_disarm (peer_ctx);
  g_clear_pointer (&peer_ctx->readback_fence, cogl_gpu_fence_free);
  peer_ctx->readback_pending = FALSE;
  peer_ctx->readback_buffer = -1;
  peer_ctx->readback_polls = 0;

  meta_rdp_gfxredir_send_present (peer_ctx, index, &rect);
  return TRUE;
}

static gboolean
meta_rdp_readback_poll_cb (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;

  if (!peer_ctx->readback_pending)
    {
      peer_ctx->readback_poll_id = 0;
      return G_SOURCE_REMOVE;
    }

  peer_ctx->readback_polls++;

  /* Both branches below go through meta_rdp_readback_finish(), which disarms;
   * from in here that just clears the id (see meta_rdp_readback_disarm()), and
   * returning G_SOURCE_REMOVE is what actually tears the source down. */
  if (meta_rdp_readback_finish (peer_ctx, peer_ctx->readback_fence == NULL))
    return G_SOURCE_REMOVE;

  if (peer_ctx->readback_polls >= META_RDP_READBACK_MAX_POLLS)
    {
      g_warning ("rdp: readback fence has not signalled after %d polls; "
                 "collecting synchronously",
                 peer_ctx->readback_polls);
      meta_rdp_readback_finish (peer_ctx, TRUE);
      return G_SOURCE_REMOVE;
    }

  /* Disarmed underneath us -- the readback was cancelled, or an error path
   * dropped it. Let this source go rather than leave it running alongside the
   * timer the next readback will arm, which would poll the same fence twice. */
  if (peer_ctx->readback_poll_id == 0)
    return G_SOURCE_REMOVE;

  return G_SOURCE_CONTINUE;
}

/* Try to collect without waiting. Called at the top of each frame, which is
 * where the fence has almost always signalled already -- the poll timer is the
 * fallback for when no next frame comes, since mutter renders on damage. */
static void
meta_rdp_readback_collect_if_ready (MetaRdpPeerContext *peer_ctx)
{
  /* With no fence there is nothing to test, so this is where the deliberately
   * fenceless mode actually collects: one frame after the readback was issued,
   * by which point the frame's own flush has submitted it and the GPU has had a
   * full frame to finish. The map is unguarded, so the collect meter's
   * "blocked" count is what says whether that assumption holds. */
  if (peer_ctx->readback_pending)
    meta_rdp_readback_finish (peer_ctx, peer_ctx->readback_fence == NULL);
}

/* Make sure the PBO exists and is big enough for a full frame. */
static gboolean
meta_rdp_readback_ensure_pbo (MetaRdpPeerContext *peer_ctx,
                              CoglContext        *cogl_context,
                              size_t              size)
{
  if (peer_ctx->readback_pbo && peer_ctx->readback_pbo_size >= size)
    return TRUE;

  /* Never reallocate under an outstanding readback. */
  meta_rdp_readback_free (peer_ctx);

  peer_ctx->readback_pbo = cogl_pixel_buffer_new_for_readback (cogl_context,
                                                               size);
  if (!peer_ctx->readback_pbo)
    return FALSE;

  peer_ctx->readback_pbo_size = size;
  g_message ("rdp: allocated a %zu-byte readback PBO", size);
  return TRUE;
}

/* Issue a readback of @rect into the PBO and fence it.
 *
 * Returns FALSE if the asynchronous path is unavailable, in which case the
 * caller falls back to reading straight into shared memory. */
static gboolean
meta_rdp_readback_begin (MetaRdpPeerContext *peer_ctx,
                         CoglFramebuffer    *framebuffer,
                         int                 index,
                         const MtkRectangle *rect)
{
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  CoglGpuFence *fence;
  CoglBitmap *bitmap;
  size_t full_size;
  int64_t issued_us;
  int64_t read_us;
  int64_t fence_started_us;
  gboolean ok;
  /* Read once: this is on the per-frame path. */
  static int fence_disabled = -1;

  if (fence_disabled < 0)
    fence_disabled = g_getenv ("META_RDP_NO_FENCE") != NULL;

  g_return_val_if_fail (!peer_ctx->readback_pending, FALSE);

  /* Size for a whole frame, not for this rect: the PBO has to survive damage
   * rects of every shape without being reallocated, and reallocation is three
   * orders of magnitude more expensive than reuse. */
  full_size = (size_t) peer_ctx->buffer_width * peer_ctx->buffer_height * 4;
  if (!meta_rdp_readback_ensure_pbo (peer_ctx, cogl_context, full_size))
    return FALSE;

  /* Pack tightly at offset 0 rather than mirroring the frame layout: the
   * transfer is then exactly the damaged pixels, and rowstride == bpp * width
   * keeps cogl off its stride-mismatch path (which would read into a malloc'd
   * temporary and bypass the PBO entirely). */
  bitmap = cogl_bitmap_new_from_buffer (COGL_BUFFER (peer_ctx->readback_pbo),
                                        COGL_PIXEL_FORMAT_BGRA_8888_PRE,
                                        rect->width, rect->height,
                                        rect->width * 4,
                                        0);
  if (!bitmap)
    return FALSE;

  /* Timed with the same meter as the synchronous path, so the two are directly
   * comparable: this is what "% of wall" was 60% of before. Issuing into a PBO
   * should be near-free -- if this is still milliseconds, the transfer is not
   * actually being deferred and the fence is decorating a stall rather than
   * removing one. */
  issued_us = g_get_monotonic_time ();
  ok = cogl_framebuffer_read_pixels_into_bitmap (framebuffer,
                                                 rect->x, rect->y,
                                                 COGL_READ_PIXELS_COLOR_BUFFER,
                                                 bitmap);
  read_us = g_get_monotonic_time () - issued_us;

  g_object_unref (bitmap);

  if (!ok)
    return FALSE;

  /* After the readback, so it marks that transfer's completion.
   *
   * Timed separately and reported alongside the read: profiling caught an
   * earlier version of cogl_gpu_fence_new() flushing eagerly, which on this
   * threaded-context driver executed the frame's queued draw calls inline, on
   * the paint path. It no longer does -- the flush rides the first poll -- but
   * this is exactly the kind of cost that hides, so it is measured rather than
   * assumed. */
  fence_started_us = g_get_monotonic_time ();

  /* META_RDP_NO_FENCE trades the fence for a frame of latency.
   *
   * Creating a fence measures ~1.16ms here, several times the readback it
   * guards, and it is not our cost to remove: glFenceSync() always flushes
   * (mesa syncobj.c), and on d3d12 even a PIPE_FLUSH_DEFERRED closes and
   * submits the command list, with tc_sync blocking on the driver thread.
   *
   * Without a fence there is no completion signal, so the readback is instead
   * collected at the *next* frame's paint. By then the frame's own end-of-frame
   * flush has submitted it and the GPU has had a full frame to finish, so the
   * map should not block -- but it is a map without a guarantee, so the collect
   * meter's "blocked" count is what says whether that holds. Off by default. */
  fence = fence_disabled ? NULL : cogl_gpu_fence_new (cogl_context);

  meta_rdp_account_readback (read_us,
                             g_get_monotonic_time () - fence_started_us,
                             (size_t) rect->width * rect->height * 4);

  peer_ctx->readback_fence = fence;
  peer_ctx->readback_pending = TRUE;
  peer_ctx->readback_buffer = index;
  peer_ctx->readback_rect = *rect;
  peer_ctx->readback_polls = 0;
  peer_ctx->readback_issued_us = g_get_monotonic_time ();

  if (!fence && !fence_disabled)
    {
      /* No fence *support* (cogl built without GL_ARB_sync). Nothing will ever
       * tell us the transfer finished, so collect now and take the blocking
       * map. No better than the direct path, but correct. */
      meta_rdp_readback_finish (peer_ctx, TRUE);
      return TRUE;
    }

  /* Fenceless by choice collects at the next frame, so it only needs the timer
   * as the no-next-frame fallback -- polling it at 1ms would just burn wakeups
   * discovering there is still nothing to test. */
  meta_rdp_readback_disarm (peer_ctx);
  peer_ctx->readback_poll_id =
    g_timeout_add_full (G_PRIORITY_DEFAULT,
                        fence ? META_RDP_READBACK_POLL_MS
                              : META_RDP_READBACK_IDLE_MS,
                        meta_rdp_readback_poll_cb, peer_ctx, NULL);

  return TRUE;
}

static void meta_rdp_peer_fail (MetaRdpPeerContext *peer_ctx,
                                const char         *reason,
                                const char         *hint);

static void
meta_rdp_present_gfxredir (MetaRdpPeerContext *peer_ctx,
                           CoglFramebuffer    *framebuffer,
                           const MtkRectangle *damage)
{
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);
  MtkRectangle rect;
  MetaRdpBuffer *buffer;
  int index;

  if (!meta_rdp_ensure_buffer (peer_ctx, width, height))
    {
      meta_rdp_peer_fail (peer_ctx,
                          "The shared-memory pool could not be created.",
                          NULL);
      return;
    }

  index = meta_rdp_acquire_buffer (peer_ctx);
  if (index < 0)
    {
      /* Every buffer is still with the client; the caller records the damage
       * and retries on the next ack. */
      g_warning ("rdp: gfxredir no free buffer, deferring present");
      return;
    }

  /* Starting a frame while the client still holds another buffer is the whole
   * point of double buffering: our readback overlaps their upload.
   *
   * n_presents_inflight alone over-reports that, because acks are retired on
   * the main loop: one that has already arrived on the channel thread still
   * counts as in flight until the idle dispatch runs. Subtract those to get
   * the number the client is genuinely still holding. */
  if (peer_ctx->n_presents_inflight > 0)
    {
      int unretired;
      int really_held;

      g_mutex_lock (&peer_ctx->gfxredir_mutex);
      unretired = peer_ctx->gfxredir_n_acked;
      g_mutex_unlock (&peer_ctx->gfxredir_mutex);

      really_held = peer_ctx->n_presents_inflight - unretired;

      g_debug ("rdp: gfxredir writing buffer %d, client holds %d present(s) "
               "(%d in flight, %d acked but not yet retired)",
               index, really_held, peer_ctx->n_presents_inflight, unretired);
    }

  buffer = &peer_ctx->buffers[index];

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

  /* Widen the rect so its row stride is 256-byte aligned.
   *
   * That is what a driver needs to copy the blitted region straight into the
   * PBO on the GPU instead of mapping it and memcpying on this thread. Mesa
   * will not pad rows to reach the alignment itself -- it must not, since the
   * PBO layout is ours -- so an unaligned width silently costs us the whole
   * asynchronous path. Rounding out here is far cheaper than the fallback: the
   * extra pixels are read and sent, but the read is off the critical path
   * entirely, whereas the fallback blocks on the GPU every frame.
   *
   * If the framebuffer width itself is not a multiple of the alignment the
   * final chunk cannot be aligned, and we simply take the slow path there. */
  {
    int align_px = META_RDP_READBACK_ALIGN_BYTES / 4;
    int x1 = rect.x + rect.width;

    rect.x = rect.x - (rect.x % align_px);
    x1 = MIN (width, ((x1 + align_px - 1) / align_px) * align_px);
    rect.width = x1 - rect.x;
  }

  /* Bring this buffer up to date before writing into it. It missed every frame
   * that went to another buffer, so those regions still hold old pixels. The
   * client only uploads the presented rect, so a hole here would be invisible
   * until something triggers a full refresh -- at which point the whole
   * surface is blitted and the stale areas would show. Copying from the last
   * fully written buffer is a plain memcpy, much cheaper than re-reading from
   * the GPU. */
  if (buffer->stale && peer_ctx->last_written >= 0 &&
      peer_ctx->last_written != index)
    {
      /* Not the whole stale region: the readback below overwrites @rect, so
       * copying that part would be undone immediately. In steady state the
       * damage lands in much the same place every frame, which makes the
       * stale region and @rect nearly identical -- so without this subtraction
       * essentially the entire frame gets memcpy'd for nothing, on top of the
       * copy the GL readback already performs internally. */
      mtk_region_subtract_rectangle (buffer->stale, &rect);

      if (!mtk_region_is_empty (buffer->stale))
        meta_rdp_copy_between_buffers (peer_ctx, peer_ctx->last_written, index,
                                       buffer->stale);
    }
  g_clear_pointer (&buffer->stale, mtk_region_unref);

  /* Preferred path: read into the PBO and let the fence tell us when it has
   * landed. The present happens from meta_rdp_readback_finish(), not here. */
  if (meta_rdp_readback_begin (peer_ctx, framebuffer, index, &rect))
    return;

  /* Fallback: read straight into its place in the shared buffer, blocking
   * until the GPU hands the pixels over. Passing the full buffer_stride is
   * what lets a narrow region land at the right offset on every row. */
  if (!meta_rdp_read_framebuffer (framebuffer,
                                  (uint8_t *) peer_ctx->shm_addr +
                                  buffer->offset +
                                  (size_t) rect.y * peer_ctx->buffer_stride +
                                  (size_t) rect.x * 4,
                                  rect.x, rect.y,
                                  rect.width, rect.height,
                                  peer_ctx->buffer_stride))
    {
      g_warning ("rdp: framebuffer readback failed (gfxredir path)");
      return;
    }

  meta_rdp_gfxredir_send_present (peer_ctx, index, &rect);
}

/* Publish a buffer whose pixels have actually landed in shared memory.
 *
 * Split out because the synchronous and asynchronous readback paths reach this
 * point at different times: synchronously right after glReadPixels returns,
 * asynchronously only once the fence has signalled and the PBO has been copied
 * out. Calling it any earlier would tell the client to upload pixels that are
 * not there yet, and -- worse -- would make @last_written name a buffer with a
 * hole in it, which meta_rdp_copy_between_buffers() would then propagate into
 * every other buffer in the pool. That corruption is invisible until something
 * triggers a full refresh. */
static void
meta_rdp_gfxredir_send_present (MetaRdpPeerContext *peer_ctx,
                                int                 index,
                                const MtkRectangle *damage)
{
  GfxRedirServerContext *redir = peer_ctx->gfxredir;
  MetaRdpBuffer *buffer = &peer_ctx->buffers[index];
  GFXREDIR_PRESENT_BUFFER_PDU present = { 0 };
  RECTANGLE_32 opaque_rect;
  MtkRectangle rect = *damage;
  int width = peer_ctx->buffer_width;
  int height = peer_ctx->buffer_height;

  /* This buffer is now current; every other buffer is missing this frame. */
  peer_ctx->last_written = index;
  for (int i = 0; i < META_RDP_N_BUFFERS; i++)
    {
      if (i == index)
        continue;

      if (!peer_ctx->buffers[i].stale)
        peer_ctx->buffers[i].stale = mtk_region_create_rectangle (&rect);
      else
        mtk_region_union_rectangle (peer_ctx->buffers[i].stale, &rect);
    }

  opaque_rect.left = rect.x;
  opaque_rect.top = rect.y;
  opaque_rect.width = rect.width;
  opaque_rect.height = rect.height;

  present.timestamp = 0; /* disable A/V sync at client side */
  present.presentId = ++peer_ctx->current_frame_id;
  present.windowId = META_RDP_DESKTOP_WINDOW_ID;
  present.bufferId = META_RDP_BUFFER_ID (index);
  present.orientation = 0;
  present.targetWidth = width;
  present.targetHeight = height;
  present.dirtyRect.left = rect.x;
  present.dirtyRect.top = rect.y;
  present.dirtyRect.width = rect.width;
  present.dirtyRect.height = rect.height;
  present.numOpaqueRects = 1;
  present.opaqueRects = &opaque_rect;

  g_debug ("rdp: gfxredir -> PresentBuffer presentId=%" G_GUINT64_FORMAT
           " bufferId=%" G_GUINT64_FORMAT " windowId=%" G_GUINT64_FORMAT
           " rect=%ux%u+%u+%u target=%dx%d",
           (uint64_t) present.presentId, (uint64_t) present.bufferId,
           (uint64_t) present.windowId,
           present.dirtyRect.width, present.dirtyRect.height,
           present.dirtyRect.left, present.dirtyRect.top,
           width, height);

  if (redir->PresentBuffer (redir, &present) == 0)
    {
      buffer->in_flight = TRUE;
      buffer->present_id = present.presentId;
      peer_ctx->n_presents_inflight++;
    }
  else
    {
      g_warning ("rdp: gfxredir PresentBuffer failed");
    }
}

/* The stage has been resized underneath us; get the client onto the new size.
 *
 * Returns TRUE if a resize is now in progress, in which case the caller must
 * not present: everything the client has -- its surface, its gfxredir buffer
 * mappings -- is still the old size until it re-activates.
 *
 * Weston does the same from rdp_output_set_mode() (rdp.c). */
static gboolean
meta_rdp_peer_sync_desktop_size (MetaRdpPeerContext *peer_ctx,
                                 CoglFramebuffer    *framebuffer)
{
  freerdp_peer *client = peer_ctx->peer;
  rdpSettings *settings = client->context->settings;
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);

  if (peer_ctx->resize_pending)
    return TRUE;

  if ((int) freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth) == width &&
      (int) freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight) == height)
    return FALSE;

  if (!freerdp_settings_get_bool (settings, FreeRDP_DesktopResize))
    {
      /* Nothing we can do: we cannot send this client a frame of a size it
       * did not agree to, and we cannot make the stage go back. */
      g_warning ("rdp: stage is %dx%d but the client cannot be resized; "
                 "closing peer %p", width, height, client);
      client->Close (client);
      return TRUE;
    }

  g_message ("rdp: desktop resized to %dx%d, notifying peer %p",
             width, height, client);

  /* Drop the pool now, while the channel is still healthy, rather than
   * leaving it to the next present.
   *
   * The client throws its gfxredir state away when it processes the
   * DesktopResize -- including any presents it had not yet acked. Those acks
   * are never coming, so buffers left in flight here would stay in flight
   * forever, and meta_rdp_peer_present() would then refuse every subsequent
   * frame on the "all buffers busy" check before ever reaching the code that
   * rebuilds the pool. Under continuous damage (glxgears) both buffers are
   * typically in flight at this point, so that deadlock is the common case,
   * not the rare one.
   *
   * This also gets DestroyBuffer/ClosePool onto the wire ahead of the resize
   * so the client releases the mapping deterministically instead of relying
   * on the two channels being ordered against each other. */
  meta_rdp_destroy_buffer (peer_ctx);

  /* Accumulated damage refers to the old framebuffer; the repaint after
   * re-activation covers the whole screen anyway. */
  peer_ctx->frame_missed = FALSE;
  g_clear_pointer (&peer_ctx->missed_damage, mtk_region_unref);

  (void) freerdp_settings_set_uint32 (settings, FreeRDP_DesktopWidth,
                                      (UINT32) width);
  (void) freerdp_settings_set_uint32 (settings, FreeRDP_DesktopHeight,
                                      (UINT32) height);

  /* Deactivate-All / re-Activate round trip. The DVCs (gfxredir, disp) survive
   * it; xf_peer_activate() clears resize_pending and forces a full repaint
   * when the client comes back. */
  peer_ctx->resize_pending = TRUE;
  client->context->update->DesktopResize (client->context);

  return TRUE;
}

/* Present the current frame to one peer. */
static void
meta_rdp_peer_present (MetaRdpPeerContext *peer_ctx,
                       CoglFramebuffer    *framebuffer,
                       const MtkRegion    *damage)
{
  MtkRectangle extents;

  if (!peer_ctx->activated)
    return;

  /* Before anything else: the client's idea of the desktop size has to match
   * the framebuffer we are about to read back. */
  if (meta_rdp_peer_sync_desktop_size (peer_ctx, framebuffer))
    return;

  if (peer_ctx->error_reason)
    {
      meta_rdp_present_error (peer_ctx, framebuffer);
      return;
    }

  if (mtk_region_is_empty (damage))
    return;

  /* Still waiting for the client to confirm gfxredir caps. */
  if (!g_atomic_int_get (&peer_ctx->gfxredir_activated))
    return;

  /* With more than one buffer we can start a frame while the client is
   * still reading the previous one; we only have to wait when every
   * buffer is in flight, or when the previous frame's readback has not
   * landed yet (only one is outstanding at a time). */
  if (peer_ctx->n_presents_inflight >= META_RDP_N_BUFFERS ||
      peer_ctx->readback_pending)
    {
      /* Coalesce by merging this damage into what we still owe the
       * client, rather than dropping it and re-sending the whole screen
       * once an ack arrives. */
      if (peer_ctx->frame_missed)
        {
          mtk_region_union (peer_ctx->missed_damage, damage);
        }
      else
        {
          g_clear_pointer (&peer_ctx->missed_damage, mtk_region_unref);
          peer_ctx->missed_damage = mtk_region_copy (damage);
          peer_ctx->frame_missed = TRUE;
        }
      return;
    }

  extents = mtk_region_get_extents (damage);
  meta_rdp_present_gfxredir (peer_ctx, framebuffer, &extents);
}

/* Present a region of the current composited contents. A NULL region means the
 * whole framebuffer, which is what connect/activation needs: mutter only
 * repaints on damage, so without this the client's screen stays empty until
 * something happens to change. */
static void
meta_rdp_peer_present_region (MetaRdpPeerContext *peer_ctx,
                              const MtkRegion    *region)
{
  MetaRdpServer *self = peer_ctx->server;
  GList *l;

  for (l = self->watched_views; l; l = l->next)
    {
      MetaRdpWatchedView *watched = l->data;
      CoglFramebuffer *fb;
      MtkRectangle bounds;
      g_autoptr (MtkRegion) clipped = NULL;

      if (!watched->view)
        continue;

      fb = clutter_stage_view_get_framebuffer (watched->view);
      bounds = (MtkRectangle) { 0, 0,
                                cogl_framebuffer_get_width (fb),
                                cogl_framebuffer_get_height (fb) };

      /* A NULL region means the whole framebuffer; otherwise clip, since
       * accumulated damage can outlive a framebuffer resize. */
      if (region)
        {
          clipped = mtk_region_copy (region);
          mtk_region_intersect_rectangle (clipped, &bounds);
        }
      else
        {
          clipped = mtk_region_create_rectangle (&bounds);
        }

      meta_rdp_peer_present (peer_ctx, fb, clipped);
      break;
    }
}

static void
meta_rdp_peer_force_full_present (MetaRdpPeerContext *peer_ctx)
{
  meta_rdp_peer_present_region (peer_ctx, NULL);
}

/* Give up on gfxredir for this peer: log why, and send the error frame in place
 * of the desktop from now on. Main thread only.
 *
 * The first reason sticks. Anything failing after it is almost always a
 * consequence, and the frame should name the cause. */
static void
meta_rdp_peer_fail (MetaRdpPeerContext *peer_ctx,
                    const char         *reason,
                    const char         *hint)
{
  if (peer_ctx->error_reason)
    return;

  g_warning ("rdp: no shared-memory graphics, sending the error frame: %s%s%s",
             reason, hint ? " " : "", hint ? hint : "");

  peer_ctx->error_reason = g_strdup (reason);
  peer_ctx->error_hint = g_strdup (hint);
  peer_ctx->error_frame_width = 0;
  peer_ctx->error_frame_height = 0;
  g_clear_handle_id (&peer_ctx->gfxredir_caps_timeout_id, g_source_remove);

  meta_rdp_peer_force_full_present (peer_ctx);
}

/* Main-loop half of the gfxredir channel callbacks.
 *
 * The callbacks themselves run on the channel's reader thread, so they only
 * flag what happened; the response -- reading back the framebuffer and writing
 * a present -- happens here, on the thread that owns Clutter and Cogl. */
static gboolean
meta_rdp_peer_gfxredir_dispatch (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;
  gboolean full_requested;
  const char *caps_error;
  uint64_t acked[META_RDP_N_BUFFERS];
  int n_acked;
  g_autoptr (MtkRegion) region = NULL;

  g_mutex_lock (&peer_ctx->gfxredir_mutex);
  peer_ctx->gfxredir_idle_id = 0;
  caps_error = g_steal_pointer (&peer_ctx->gfxredir_caps_error);
  /* A standing request (caps just confirmed) always means the whole screen:
   * the client has nothing to composite a partial update onto yet. */
  full_requested = peer_ctx->gfxredir_present_requested;
  peer_ctx->gfxredir_present_requested = FALSE;
  n_acked = peer_ctx->gfxredir_n_acked;
  memcpy (acked, peer_ctx->gfxredir_acked, sizeof (acked));
  peer_ctx->gfxredir_n_acked = 0;
  g_mutex_unlock (&peer_ctx->gfxredir_mutex);

  if (caps_error)
    {
      meta_rdp_peer_fail (peer_ctx, caps_error, META_RDP_HINT_CLIENT);
      return G_SOURCE_REMOVE;
    }

  /* Retire the acked presents: those buffers are ours to write again. */
  for (int a = 0; a < n_acked; a++)
    {
      for (int i = 0; i < META_RDP_N_BUFFERS; i++)
        {
          MetaRdpBuffer *buffer = &peer_ctx->buffers[i];

          if (buffer->in_flight && buffer->present_id == acked[a])
            {
              buffer->in_flight = FALSE;
              peer_ctx->n_presents_inflight--;
              break;
            }
        }
    }

  /* Damage that could not be sent because every buffer was busy. */
  if (n_acked > 0 && peer_ctx->frame_missed)
    {
      peer_ctx->frame_missed = FALSE;
      region = g_steal_pointer (&peer_ctx->missed_damage);
    }

  if (full_requested)
    meta_rdp_peer_present_region (peer_ctx, NULL);
  else if (region)
    meta_rdp_peer_present_region (peer_ctx, region);

  return G_SOURCE_REMOVE;
}

/* Schedule meta_rdp_peer_gfxredir_dispatch(). Safe to call from the channel
 * thread; coalesces, so concurrent requests share one main-loop pass.
 * Callers must hold gfxredir_mutex. */
static void
meta_rdp_peer_gfxredir_queue_dispatch_locked (MetaRdpPeerContext *peer_ctx)
{
  if (peer_ctx->gfxredir_idle_id == 0)
    {
      /* G_PRIORITY_DEFAULT, not the g_idle_add() default of
       * G_PRIORITY_DEFAULT_IDLE: idle priority sits below Clutter's frame
       * clock and repaint work, and in steady state every frame comes through
       * here (a present is always in flight when damage arrives, so
       * on_frame_ready only sets frame_missed and it is the ack that actually
       * presents). At idle priority that starves under continuous damage and
       * the client only updates once the compositor goes quiet. */
      peer_ctx->gfxredir_idle_id =
        g_idle_add_full (G_PRIORITY_DEFAULT, meta_rdp_peer_gfxredir_dispatch,
                         peer_ctx, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Client-side pointer                                                  */
/*                                                                      */
/* The cursor sprite is pushed over RDP as a pointer update rather than */
/* composited into the frame -- see META_STAGE_DISABLE_CURSOR_OVERLAYS  */
/* in meta-stage.c, which keeps it out of the captured framebuffer and  */
/* stops pointer motion from costing a frame. Only the shape is sent:   */
/* the client already knows where its own mouse is, so plain motion     */
/* needs no server traffic at all.                                      */
/* ------------------------------------------------------------------ */

/* MS-RDPBCGR 2.2.9.1.2.1.11: a Large Pointer Update tops out at 384x384. */
#define META_RDP_MAX_POINTER_SIZE 384

static void
meta_rdp_peer_hide_pointer (MetaRdpPeerContext *peer_ctx)
{
  rdpUpdate *update = peer_ctx->peer->context->update;
  POINTER_SYSTEM_UPDATE pointer_system = { 0 };

  pointer_system.type = SYSPTR_NULL;

  update->BeginPaint (update->context);
  update->pointer->PointerSystem (update->context, &pointer_system);
  update->EndPaint (update->context);
}

/* Resample @src (@src_w x @src_h, BGRA premultiplied) to @dst_w x @dst_h and
 * write it into @dst bottom-up, the row order a Windows DIB wants.
 *
 * Bilinear, on premultiplied data so the filtering stays correct across the
 * transparent edges a cursor is mostly made of. Cursors are at most
 * META_RDP_MAX_POINTER_SIZE square and this only runs when the shape changes,
 * so a straightforward implementation is fine. */
static void
meta_rdp_scale_bgra_flip (const uint8_t *src,
                          int            src_w,
                          int            src_h,
                          uint8_t       *dst,
                          int            dst_w,
                          int            dst_h)
{
  int src_stride = src_w * 4;
  int dst_stride = dst_w * 4;
  /* Map destination pixel centres back into the source. */
  float x_ratio = (float) src_w / dst_w;
  float y_ratio = (float) src_h / dst_h;

  for (int dy = 0; dy < dst_h; dy++)
    {
      /* Flip: the last destination row holds the first source row. */
      uint8_t *dst_row = dst + (size_t) (dst_h - 1 - dy) * dst_stride;
      float sy = (dy + 0.5f) * y_ratio - 0.5f;
      int y0 = (int) floorf (sy);
      float fy = sy - y0;
      int y1;

      y0 = CLAMP (y0, 0, src_h - 1);
      y1 = CLAMP (y0 + 1, 0, src_h - 1);

      for (int dx = 0; dx < dst_w; dx++)
        {
          float sx = (dx + 0.5f) * x_ratio - 0.5f;
          int x0 = (int) floorf (sx);
          float fx = sx - x0;
          int x1;
          const uint8_t *p00, *p01, *p10, *p11;

          x0 = CLAMP (x0, 0, src_w - 1);
          x1 = CLAMP (x0 + 1, 0, src_w - 1);

          p00 = src + (size_t) y0 * src_stride + (size_t) x0 * 4;
          p01 = src + (size_t) y0 * src_stride + (size_t) x1 * 4;
          p10 = src + (size_t) y1 * src_stride + (size_t) x0 * 4;
          p11 = src + (size_t) y1 * src_stride + (size_t) x1 * 4;

          for (int c = 0; c < 4; c++)
            {
              float top = p00[c] + (p01[c] - p00[c]) * fx;
              float bottom = p10[c] + (p11[c] - p10[c]) * fx;
              float value = top + (bottom - top) * fy;

              dst_row[dx * 4 + c] = (uint8_t) CLAMP (value + 0.5f, 0.0f, 255.0f);
            }
        }
    }
}

/* @dst_width / @dst_height are the size the sprite should occupy in the
 * client's (framebuffer) pixels, which is not the texture's size on a scaled
 * desktop -- see meta_rdp_peer_update_pointer(). */
static void
meta_rdp_peer_send_pointer (MetaRdpPeerContext *peer_ctx,
                            CoglTexture        *texture,
                            int                 hot_x,
                            int                 hot_y,
                            int                 dst_width,
                            int                 dst_height)
{
  rdpUpdate *update = peer_ctx->peer->context->update;
  POINTER_LARGE_UPDATE pointer_update = { 0 };
  int width = cogl_texture_get_width (texture);
  int height = cogl_texture_get_height (texture);
  int stride = width * 4;
  int dst_stride;
  g_autofree uint8_t *bits = NULL;
  g_autofree uint8_t *flipped = NULL;

  if (width <= 0 || height <= 0 || dst_width <= 0 || dst_height <= 0)
    {
      g_warning ("rdp: cursor is %dx%d -> %dx%d; hiding",
                 width, height, dst_width, dst_height);
      meta_rdp_peer_hide_pointer (peer_ctx);
      return;
    }

  if (dst_width > META_RDP_MAX_POINTER_SIZE ||
      dst_height > META_RDP_MAX_POINTER_SIZE)
    {
      g_warning ("rdp: cursor is %dx%d, beyond the large pointer limit; hiding",
                 dst_width, dst_height);
      meta_rdp_peer_hide_pointer (peer_ctx);
      return;
    }

  bits = g_malloc ((size_t) stride * height);

  /* BGRA byte order on little-endian is what the wire calls ARGB, which is
   * what a 32bpp xorMask carries. */
  if (cogl_texture_get_data (texture, COGL_PIXEL_FORMAT_BGRA_8888_PRE,
                             stride, bits) == 0)
    {
      g_warning ("rdp: failed to read back cursor texture; hiding");
      meta_rdp_peer_hide_pointer (peer_ctx);
      return;
    }

  /* Pointer bitmaps are bottom-up, like a Windows DIB. */
  dst_stride = dst_width * 4;
  flipped = g_malloc ((size_t) dst_stride * dst_height);

  if (dst_width == width && dst_height == height)
    {
      for (int y = 0; y < height; y++)
        memcpy (flipped + (size_t) y * stride,
                bits + (size_t) (height - 1 - y) * stride,
                stride);
    }
  else
    {
      meta_rdp_scale_bgra_flip (bits, width, height,
                                flipped, dst_width, dst_height);

      /* The hotspot is in texture pixels; move it with the image. */
      hot_x = (int) roundf ((float) hot_x * dst_width / width);
      hot_y = (int) roundf ((float) hot_y * dst_height / height);
    }

  pointer_update.xorBpp = 32;
  pointer_update.cacheIndex = 0;
  pointer_update.hotSpotX = CLAMP (hot_x, 0, dst_width - 1);
  pointer_update.hotSpotY = CLAMP (hot_y, 0, dst_height - 1);
  pointer_update.width = dst_width;
  pointer_update.height = dst_height;
  /* A 32bpp xorMask carries its own alpha, so no separate AND mask. */
  pointer_update.lengthAndMask = 0;
  pointer_update.andMaskData = NULL;
  pointer_update.lengthXorMask = (UINT32) dst_stride * dst_height;
  pointer_update.xorMaskData = flipped;

  update->BeginPaint (update->context);
  update->pointer->PointerLarge (update->context, &pointer_update);
  update->EndPaint (update->context);
}

static void
meta_rdp_peer_update_pointer (MetaRdpPeerContext *peer_ctx)
{
  MetaCursorTracker *cursor_tracker;
  ClutterCursor *cursor;
  CoglTexture *texture;
  float scale;
  float logical_width;
  float logical_height;
  int dst_width;
  int dst_height;
  int hot_x = 0;
  int hot_y = 0;

  if (!peer_ctx->activated)
    return;

  cursor_tracker = meta_backend_get_cursor_tracker (peer_ctx->server->backend);

  if (!meta_cursor_tracker_get_pointer_visible (cursor_tracker))
    {
      meta_rdp_peer_hide_pointer (peer_ctx);
      return;
    }

  /* Go through the ClutterCursor rather than meta_cursor_tracker_get_sprite():
   * the sprite's texture size alone doesn't say how big it should appear. */
  cursor = META_CURSOR_TRACKER_GET_CLASS (cursor_tracker)->get_sprite (cursor_tracker);
  if (!cursor)
    {
      meta_rdp_peer_hide_pointer (peer_ctx);
      return;
    }

  clutter_cursor_realize_texture (cursor);
  texture = clutter_cursor_get_texture (cursor, &hot_x, &hot_y);
  if (!texture)
    {
      meta_rdp_peer_hide_pointer (peer_ctx);
      return;
    }

  /* Work out how large the sprite should be in the client's pixels.
   *
   * The texture is not authoritative: on a scaled desktop the xcursor backend
   * loads the theme at ceil(scale) and records the size it should actually be
   * drawn at as the viewport destination size, in logical pixels
   * (meta_cursor_xcursor_prepare_at). Where that isn't set -- we disable the
   * cursor overlays, so nothing may have prepared the sprite -- fall back to
   * the texture's own scale, which is 1.0 for an unprepared xcursor and
   * therefore means the texture is already logical-sized. */
  if (!clutter_cursor_get_viewport_dst_size (cursor, &dst_width, &dst_height))
    {
      float texture_scale = clutter_cursor_get_texture_scale (cursor);

      if (texture_scale <= 0.0f)
        texture_scale = 1.0f;

      logical_width = cogl_texture_get_width (texture) / texture_scale;
      logical_height = cogl_texture_get_height (texture) / texture_scale;
    }
  else
    {
      logical_width = dst_width;
      logical_height = dst_height;
    }

  scale = meta_rdp_server_get_scale (peer_ctx->server);
  dst_width = (int) roundf (logical_width * scale);
  dst_height = (int) roundf (logical_height * scale);

  meta_rdp_peer_send_pointer (peer_ctx, texture, hot_x, hot_y,
                              dst_width, dst_height);
}

static void
meta_rdp_server_update_pointer (MetaRdpServer *self)
{
  GList *l;

  for (l = self->peers; l; l = l->next)
    meta_rdp_peer_update_pointer (l->data);
}

static void
on_cursor_changed (MetaCursorTracker *cursor_tracker,
                   MetaRdpServer     *self)
{
  meta_rdp_server_update_pointer (self);
}

static void
on_cursor_visibility_changed (MetaCursorTracker *cursor_tracker,
                              MetaRdpServer     *self)
{
  meta_rdp_server_update_pointer (self);
}

/* Convert a region in stage (logical) coordinates into framebuffer pixels for
 * @view. Rounds outward: under-reporting damage leaves stale pixels on the
 * client, over-reporting only costs a slightly larger readback. */
static MtkRegion *
meta_rdp_region_to_framebuffer (const MtkRegion  *region,
                                ClutterStageView *view)
{
  float scale = clutter_stage_view_get_scale (view);
  MtkRectangle view_layout;
  MtkRegion *scaled;
  int n_rects;

  clutter_stage_view_get_layout (view, &view_layout);

  if (G_APPROX_VALUE (scale, 1.0f, 0.001f) &&
      view_layout.x == 0 && view_layout.y == 0)
    return mtk_region_copy (region);

  scaled = mtk_region_create ();
  n_rects = mtk_region_num_rectangles (region);

  for (int i = 0; i < n_rects; i++)
    {
      MtkRectangle rect = mtk_region_get_rectangle (region, i);

      /* Stage coordinates are global; make them view-relative first, since
       * the framebuffer starts at the view's origin. */
      rect.x -= view_layout.x;
      rect.y -= view_layout.y;

      mtk_rectangle_scale_double (&rect, scale, MTK_ROUNDING_STRATEGY_GROW,
                                  &rect);
      mtk_region_union_rectangle (scaled, &rect);
    }

  return scaled;
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
  g_autoptr (MtkRegion) damage = NULL;
  GList *l;

  framebuffer = clutter_stage_view_get_framebuffer (view);
  width = cogl_framebuffer_get_width (framebuffer);
  height = cogl_framebuffer_get_height (framebuffer);

  /* Keep the region rather than collapsing to its extents here: the bounding
   * box is only forced at the point the damage goes on the wire, and taking it
   * this early would also throw away detail we want to accumulate across
   * frames. */
  if (redraw_clip && !mtk_region_is_empty (redraw_clip))
    {
      /* The redraw clip is in stage coordinates, which are logical, while
       * everything downstream of here -- the readback, the shm buffers, the
       * client -- works in framebuffer pixels. On an unscaled desktop the two
       * are the same; on a scaled one, presenting the logical rectangle
       * verbatim updates only the top-left 1/scale of what actually changed.
       * paint_transformed_framebuffer() in clutter-stage-view.c performs the
       * same conversion when it composites the view. */
      damage = meta_rdp_region_to_framebuffer (redraw_clip, view);
    }
  else
    {
      MtkRectangle full = { 0, 0, width, height };

      damage = mtk_region_create_rectangle (&full);
    }

  self->frame_counter++;

  for (l = self->peers; l; l = l->next)
    {
      /* Collect last frame's readback before starting this one. By now its
       * fence has almost always signalled, so this is the path that actually
       * retires readbacks in steady state -- the 1ms poll timer exists for the
       * case where no next frame comes, since mutter renders on damage.
       *
       * Doing it here also frees the buffer it was targeting, so the present
       * below has one more to choose from. */
      meta_rdp_readback_collect_if_ready (l->data);

      meta_rdp_peer_present (l->data, framebuffer, damage);
    }
}

static void meta_rdp_server_detach_views (MetaRdpServer *self);

static void
on_watched_view_destroyed (gpointer  user_data,
                           GObject  *where_the_object_was)
{
  MetaRdpWatchedView *watched = user_data;
  MetaRdpServer *self = watched->server;

  /* The watch has to go with the view. MetaStage keeps watches in a flat
   * array and never purges them when a view is destroyed --
   * meta_stage_remove_watch() is the only removal path -- and
   * notify_watchers_for_mode() matches them by comparing watch->view against
   * the view being painted, by pointer. Leaving one behind is not merely a
   * leak: rebuilding the views (which is what a resize does) readily hands a
   * new ClutterStageView the address of one just freed, at which point the
   * stale watch starts matching and calls us back with this freed struct. */
  if (watched->paint_watch)
    {
      meta_stage_remove_watch (meta_rdp_server_get_stage (self),
                               watched->paint_watch);
      watched->paint_watch = NULL;
    }

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
      /* AFTER_ACTOR_PAINT runs before MetaStage paints the cursor overlays, so
       * the readback excludes the cursor sprite -- the RDP client draws its own
       * pointer instead. This mirrors how the screen cast backend distinguishes
       * its cursor modes (see meta_screen_cast_monitor_stream_src_sync_watches):
       * HIDDEN/METADATA watch AFTER_ACTOR_PAINT, EMBEDDED watches AFTER_PAINT.
       * Switch back to AFTER_PAINT to composite the cursor into the stream. */
      watched->paint_watch =
        meta_stage_watch_view (stage, view,
                               META_STAGE_WATCH_AFTER_ACTOR_PAINT,
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

      /* Unconditionally: the watch belongs to the stage, not the view, and
       * has to be removed even if the view is already gone. */
      if (watched->paint_watch)
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

  /* The scale may have changed with the layout, and the sprite we last sent
   * was sized for the old one. */
  meta_rdp_server_update_pointer (self);
}

/* ------------------------------------------------------------------ */
/* FreeRDP fd <-> GLib main loop bridge                               */
/* ------------------------------------------------------------------ */

typedef gboolean (*MetaRdpFdCheck) (gpointer data);

typedef struct _MetaRdpFdSource
{
  GSource source;
  gpointer fd_tag;
  MetaRdpFdCheck check;
  gpointer data;
  int fd;
  const char *label;
  guint64 dispatch_count;
} MetaRdpFdSource;

static gboolean
meta_rdp_fd_source_dispatch (GSource     *source,
                             GSourceFunc  callback,
                             gpointer     user_data)
{
  MetaRdpFdSource *fd_source = (MetaRdpFdSource *) source;
  GIOCondition revents;

  revents = g_source_query_unix_fd (source, fd_source->fd_tag);

    g_debug ("rdp: fd source %s (fd %d) dispatch #%" G_GUINT64_FORMAT
                " revents=0x%x",
                fd_source->label, fd_source->fd,
                fd_source->dispatch_count, (unsigned int) revents);

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
                        gpointer        data,
                        const char     *label)
{
  GSource *source;
  MetaRdpFdSource *fd_source;

  source = g_source_new (&meta_rdp_fd_source_funcs, sizeof (MetaRdpFdSource));
  fd_source = (MetaRdpFdSource *) source;
  fd_source->check = check;
  fd_source->data = data;
  fd_source->fd = fd;
  fd_source->label = label;
  fd_source->fd_tag =
    g_source_add_unix_fd (source, fd, G_IO_IN | G_IO_HUP | G_IO_ERR);

  g_message ("rdp: watching fd %d (%s)", fd, label);

  g_source_set_name (source, "[mutter] RDP fd");
  g_source_attach (source, NULL);
  g_source_unref (source);

  return source;
}

/* ------------------------------------------------------------------ */
/* peer context + callbacks                                           */
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

/* Tear down the CLIPRDR bridge *and* the main-loop source watching its event
 * handle. Freeing the bridge closes that fd, so leaving the GSource attached
 * would leave us polling a dead descriptor: poll() reports POLLNVAL, which our
 * dispatch mask ignores, so the source stays ready forever and the main loop
 * spins without ever sleeping (and the fd number can later be recycled by an
 * unrelated open(), giving spurious wakeups on someone else's socket). */
static void
meta_rdp_peer_clear_clipboard (MetaRdpPeerContext *peer_ctx)
{
  int i;

  if (peer_ctx->clipboard_fd_source)
    {
      for (i = 0; i < peer_ctx->n_fd_sources; i++)
        {
          if (peer_ctx->fd_sources[i] == peer_ctx->clipboard_fd_source)
            peer_ctx->fd_sources[i] = NULL;
        }

      g_source_destroy (peer_ctx->clipboard_fd_source);
      peer_ctx->clipboard_fd_source = NULL;
    }

  g_clear_pointer (&peer_ctx->clipboard, meta_rdp_clipboard_free);
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

/* ---- gfxredir caps negotiation callbacks (ported from rdprail.c) ---- */

/* Runs on the channel thread: hand the rejection to the main loop, which turns
 * it into the error frame. */
static void
gfxredir_reject_caps (MetaRdpPeerContext *peer_ctx,
                      const char         *reason)
{
  g_mutex_lock (&peer_ctx->gfxredir_mutex);
  peer_ctx->gfxredir_caps_error = reason;
  meta_rdp_peer_gfxredir_queue_dispatch_locked (peer_ctx);
  g_mutex_unlock (&peer_ctx->gfxredir_mutex);
}

static UINT
gfxredir_legacy_caps (GfxRedirServerContext           *context,
                      const GFXREDIR_LEGACY_CAPS_PDU   *caps)
{
  g_message ("rdp: gfxredir legacy caps v%d (v1 unsupported)", caps->version);
  gfxredir_reject_caps (context->custom,
                        "The client only speaks gfxredir v1; v2 is required.");
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
      /* A zero length would loop forever on the channel thread. */
      if (current->length < sizeof (GFXREDIR_CAPS_HEADER) ||
          current->length > length)
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
      g_message ("rdp: gfxredir -> CapsConfirm version=0x%x length=%u",
                 confirm.version, confirm.length);
      context->GraphicsRedirectionCapsConfirm (context, &confirm);

      /* Set atomically, not under gfxredir_mutex: the main thread's present
       * path reads it without taking the lock. */
      g_atomic_int_set (&peer_ctx->gfxredir_activated, TRUE);
      g_message ("rdp: gfxredir activated (caps v0x%x)", selected->version);

      /* Fill the client's screen immediately rather than waiting for the first
       * damage event -- but on the main thread, not here. */
      g_mutex_lock (&peer_ctx->gfxredir_mutex);
      peer_ctx->gfxredir_present_requested = TRUE;
      meta_rdp_peer_gfxredir_queue_dispatch_locked (peer_ctx);
      g_mutex_unlock (&peer_ctx->gfxredir_mutex);
    }
  else
    {
      g_warning ("rdp: gfxredir client advertised no v2.0+ caps");
      gfxredir_reject_caps (peer_ctx,
                            "The client offered no gfxredir v2 caps.");
    }

  return CHANNEL_RC_OK;
}

static UINT
gfxredir_present_buffer_ack (GfxRedirServerContext                 *context,
                             const GFXREDIR_PRESENT_BUFFER_ACK_PDU *ack)
{
  MetaRdpPeerContext *peer_ctx = context->custom;

  g_debug ("rdp: gfxredir <- PresentBufferAck presentId=%" G_GUINT64_FORMAT
           " windowId=%" G_GUINT64_FORMAT,
           (uint64_t) ack->presentId, (uint64_t) ack->windowId);

  if (ack->windowId == META_RDP_DESKTOP_WINDOW_ID)
    {
      /* Runs on the channel thread; the buffer bookkeeping belongs to the main
       * thread, so hand the presentId over rather than acting on it here. */
      g_mutex_lock (&peer_ctx->gfxredir_mutex);
      if (ack->presentId <= peer_ctx->present_id_floor)
        {
          /* Issued against a pool we have since destroyed (a resize). The
           * buffer it names no longer exists, and retiring it would free a
           * same-indexed buffer of the new pool that is still in flight. */
          g_debug ("rdp: gfxredir dropping stale ack presentId=%"
                   G_GUINT64_FORMAT " (pool rebuilt at %" G_GUINT64_FORMAT ")",
                   (uint64_t) ack->presentId,
                   (uint64_t) peer_ctx->present_id_floor);
        }
      else if (peer_ctx->gfxredir_n_acked < META_RDP_N_BUFFERS)
        {
          peer_ctx->gfxredir_acked[peer_ctx->gfxredir_n_acked++] = ack->presentId;
          meta_rdp_peer_gfxredir_queue_dispatch_locked (peer_ctx);
        }
      else
        {
          /* Cannot happen: we never have more presents outstanding than
           * buffers, so there is always room for their acks. */
          g_warning ("rdp: gfxredir ack overflow, dropping presentId=%"
                     G_GUINT64_FORMAT, (uint64_t) ack->presentId);
        }
      g_mutex_unlock (&peer_ctx->gfxredir_mutex);
    }

  return CHANNEL_RC_OK;
}

/* Create and start the drdynvc server context. Reaching READY takes a caps
 * exchange with the client; see meta_rdp_drdynvc_wait(). */
static gboolean
meta_rdp_start_drdynvc (MetaRdpPeerContext *peer_ctx)
{
  DrdynvcServerContext *drdynvc;

  if (peer_ctx->drdynvc)
    return TRUE;

  if (!peer_ctx->vcm)
    return FALSE;

  drdynvc = drdynvc_server_context_new (peer_ctx->vcm);
  if (!drdynvc)
    {
      g_warning ("rdp: drdynvc_server_context_new failed");
      return FALSE;
    }

  if (drdynvc->Start (drdynvc) != CHANNEL_RC_OK)
    {
      g_warning ("rdp: drdynvc Start failed");
      drdynvc_server_context_free (drdynvc);
      return FALSE;
    }

  peer_ctx->drdynvc = drdynvc;
  return TRUE;
}

/* ---- MS-RDPEDISP: client-requested resolution ---- */

/* Main-loop half of meta_rdp_disp_monitor_layout(). */
static gboolean
meta_rdp_disp_dispatch (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;
  int width;
  int height;
  uint32_t scale_percent;

  g_mutex_lock (&peer_ctx->disp_mutex);
  peer_ctx->disp_idle_id = 0;
  width = peer_ctx->disp_requested_width;
  height = peer_ctx->disp_requested_height;
  scale_percent = peer_ctx->disp_requested_scale_percent;
  g_mutex_unlock (&peer_ctx->disp_mutex);

  /* No DesktopResize from here: the resize is asynchronous, and
   * meta_rdp_peer_present() pushes it once a frame actually arrives at the
   * new size. */
  meta_rdp_server_resize_monitor (peer_ctx->server, width, height,
                                  meta_rdp_scale_from_percent (scale_percent,
                                                               width, height));

  return G_SOURCE_REMOVE;
}

/* Runs on the disp channel's own thread -- see the gfxredir callbacks for the
 * same constraint. Record the request and bounce it to the main loop. */
static UINT
meta_rdp_disp_monitor_layout (DispServerContext                        *context,
                              const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu)
{
  MetaRdpPeerContext *peer_ctx = context->custom;
  const DISPLAY_CONTROL_MONITOR_LAYOUT *primary = NULL;

  if (pdu->NumMonitors == 0)
    return CHANNEL_RC_OK;

  /* Single-head backend: take the primary monitor, or the first one if the
   * client didn't flag any. Weston merges the full layout into a multi-head
   * topology instead (rdpdisp.c disp_start_monitor_layout_change). */
  for (UINT32 i = 0; i < pdu->NumMonitors; i++)
    {
      if (pdu->Monitors[i].Flags & DISPLAY_CONTROL_MONITOR_PRIMARY)
        {
          primary = &pdu->Monitors[i];
          break;
        }
    }

  if (!primary)
    primary = &pdu->Monitors[0];

  g_message ("rdp: disp <- MonitorLayout %u monitor(s), primary %ux%u "
             "(scale %u%%/%u%%)",
             pdu->NumMonitors, primary->Width, primary->Height,
             primary->DesktopScaleFactor, primary->DeviceScaleFactor);

  if (pdu->NumMonitors > 1)
    g_message ("rdp: disp only the primary monitor is used (single-head)");

  g_mutex_lock (&peer_ctx->disp_mutex);
  peer_ctx->disp_requested_width = (int) primary->Width;
  peer_ctx->disp_requested_height = (int) primary->Height;
  /* DesktopScaleFactor is the DPI scaling the user picked; DeviceScaleFactor
   * describes the panel itself and is not ours to apply. */
  peer_ctx->disp_requested_scale_percent = primary->DesktopScaleFactor;
  if (peer_ctx->disp_idle_id == 0)
    {
      /* G_PRIORITY_DEFAULT for the same reason as the gfxredir dispatch. */
      peer_ctx->disp_idle_id =
        g_idle_add_full (G_PRIORITY_DEFAULT, meta_rdp_disp_dispatch,
                         peer_ctx, NULL);
    }
  g_mutex_unlock (&peer_ctx->disp_mutex);

  return CHANNEL_RC_OK;
}

/* Open the display control channel so the client can ask for a resolution.
 * Weston does the same in rdprail.c, opening disp before the graphics
 * channels; keep that order so the caps PDU is queued before the gfxredir
 * handshake starts pumping the peer. */
static void
meta_rdp_setup_disp (MetaRdpPeerContext *peer_ctx)
{
  DispServerContext *disp;

  if (peer_ctx->disp)
    return;

  disp = disp_server_context_new (peer_ctx->vcm);
  if (!disp)
    {
      g_warning ("rdp: disp_server_context_new failed; resolution is fixed");
      return;
    }

  disp->custom = peer_ctx;
  disp->MaxNumMonitors = 1;
  disp->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
  disp->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
  disp->DispMonitorLayout = meta_rdp_disp_monitor_layout;

  if (disp->Open (disp) != CHANNEL_RC_OK)
    {
      g_warning ("rdp: disp Open failed; resolution is fixed");
      disp_server_context_free (disp);
      return;
    }

  if (disp->DisplayControlCaps (disp) != CHANNEL_RC_OK)
    {
      g_warning ("rdp: disp DisplayControlCaps failed");
      disp->Close (disp);
      disp_server_context_free (disp);
      return;
    }

  peer_ctx->disp = disp;
  g_message ("rdp: disp channel opened (max %ux%u)",
             DISPLAY_CONTROL_MAX_MONITOR_WIDTH,
             DISPLAY_CONTROL_MAX_MONITOR_HEIGHT);
}

/* A client with gfxredir confirms caps right after the channel opens, well
 * inside this. One without it -- or without a share to map, which the Windows
 * client needs /wslgsharedmemorypath: for -- never answers at all. */
#define META_RDP_GFXREDIR_CAPS_TIMEOUT_S 5

static gboolean
meta_rdp_gfxredir_caps_timeout (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;

  peer_ctx->gfxredir_caps_timeout_id = 0;

  if (!g_atomic_int_get (&peer_ctx->gfxredir_activated))
    {
      meta_rdp_peer_fail (peer_ctx,
                          "The client did not open the gfxredir channel.",
                          META_RDP_HINT_CLIENT);
    }

  return G_SOURCE_REMOVE;
}

static void
meta_rdp_setup_gfxredir (MetaRdpPeerContext *peer_ctx)
{
  MetaRdpServer *self = peer_ctx->server;
  GfxRedirServerContext *redir = NULL;

  if (!self->shared_memory_mount_path)
    {
      meta_rdp_peer_fail (peer_ctx, self->shared_memory_error,
                          META_RDP_HINT_SYSTEM_DISTRO);
      return;
    }

  redir = gfxredir_server_context_new (peer_ctx->vcm);

  if (!redir)
    {
      g_warning ("rdp: gfxredir_server_context_new failed");
      meta_rdp_peer_fail (peer_ctx, "The gfxredir channel could not be created.",
                          NULL);
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
      meta_rdp_peer_fail (peer_ctx, "The gfxredir channel could not be opened.",
                          META_RDP_HINT_CLIENT);
      return;
    }

  peer_ctx->gfxredir = redir;
  peer_ctx->gfxredir_caps_timeout_id =
    g_timeout_add_seconds (META_RDP_GFXREDIR_CAPS_TIMEOUT_S,
                           meta_rdp_gfxredir_caps_timeout, peer_ctx);
  g_message ("rdp: gfxredir channel opened; awaiting caps advertise");
}

/* ------------------------------------------------------------------ */
/* MS-RDPEI: touch input -> synthesized touchpad-gesture ClutterEvents */
/* ------------------------------------------------------------------ */

/* 3 is the point at which a real Precision Touchpad's own gestures (single-
 * finger pointing, two-finger scroll) give way to multi-finger swipes; below
 * that this stays out of the way entirely, matching the FreeRDP SDL client's
 * kMinContactsToForward. */
#define META_RDP_GESTURE_MIN_FINGERS 3

/* Client-sent contact coordinates are session-pixel-space excursions around
 * whatever anchor the client chose (see the SDL client's kGestureAnchor).
 * The scale below maps that into GNOME Shell's own delta units
 * (ui/swipeTracker.js, not in this tree): confirmed from the actual
 * upstream source, touchpad-swipe progress is `progress += delta /
 * distance`, where `delta` is `event.get_gesture_motion_delta_unaccelerated()`
 * (the same units libinput reports for real touchpad hardware) and
 * `distance` is a *fixed* per-orientation constant --
 * TOUCHPAD_BASE_WIDTH = 400 (horizontal, workspace switch) or
 * TOUCHPAD_BASE_HEIGHT = 300 (vertical, overview) -- not tied to monitor or
 * window size at all. A real swipe only needs to accumulate ~300-400 total
 * delta units over its whole physical motion. Our earlier assumption that
 * "distance" scaled with screen pixels (hundreds to 1000+ px) was wrong and
 * made this value (previously 6.0) about an order of magnitude too large:
 * with session-pixel-space deltas already in the tens-to-hundreds range per
 * update, a single frame of real movement could already blow past the
 * required ~350, hence gestures committing near-instantly ("too fast") the
 * moment any movement was detected. At release, if velocity is below
 * SwipeTracker's VELOCITY_THRESHOLD_TOUCHPAD it snaps to whichever endpoint
 * accumulated *progress* is closer to; above that it projects from
 * velocity instead -- so this only needs to get the total travel roughly
 * right, not pixel-perfect.
 *
 * The client maps touchpad travel to a fraction of its window and scales it
 * by kGestureScale; the contact coordinates we get are that fraction in
 * session pixels. Normalize by the desktop size first so the result is
 * touchpad travel again, independent of resolution, then scale it as tuned
 * on a 1920x1080 session (0.5 per pixel there). Tune this, not the client's
 * kGestureScale, if gestures need too much or too little travel. */
#define META_RDP_GESTURE_DELTA_SCALE 0.5f
#define META_RDP_GESTURE_REFERENCE_WIDTH 1920.0f
#define META_RDP_GESTURE_REFERENCE_HEIGHT 1080.0f

/* How many consecutive onTouchEvent frames a finger-count change (including
 * dropping below META_RDP_GESTURE_MIN_FINGERS) must persist before it's
 * treated as real, rather than one-off HID contact-detection noise. See the
 * rdpei_gesture_low_streak/rdpei_gesture_pending_* field comments. */
#define META_RDP_GESTURE_FINGER_DEBOUNCE 3

/* A gesture that receives no touch frame for this long is ended. The
 * debounce above needs further frames to confirm a lift, and a client that
 * stops sending (focus loss, a dropped lift) never sends them. */
#define META_RDP_GESTURE_TIMEOUT_MS 200

/* Deliberately *not* done here: direction/axis locking, i.e. deciding a
 * swipe is horizontal or vertical and zeroing the other component. GNOME
 * Shell's TouchpadSwipeGesture (js/ui/swipeTracker.js) already does it, and
 * owns the decision: it accumulates motion until DRAG_THRESHOLD_DISTANCE,
 * classifies the gesture by whichever of |cdx|/|cdy| is larger, and puts
 * every SwipeTracker whose orientation doesn't match into its ignored state
 * -- so exactly one tracker ever begins, and it consumes only its own axis
 * afterwards. Locking here as well just raced that with a second, coarser
 * threshold and could pre-empt Shell's choice on a near-diagonal swipe.
 * Forward honest dx/dy and let Shell classify. */

typedef struct
{
  ClutterTouchpadGesturePhase phase;
  uint32_t fingers;
  float dx;
  float dy;
} MetaRdpPendingGesture;

static void
meta_rdp_pending_gesture_free (MetaRdpPendingGesture *pending)
{
  g_free (pending);
}

/* Runs on the main loop: pops everything onTouchEvent queued (on the rdpei
 * channel's own thread, see the peer context field comments) and turns each
 * into a real ClutterEvent. */

/* touchpad-gesture ClutterEvents need a real ClutterInputDevice as their
 * source -- ClutterVirtualInputDevice (what peer_ctx->virtual_pointer is) is
 * a plain GObject, not a ClutterInputDevice, so it cannot be used here.
 * Prefer an actual touchpad if the host has one (most natural source for a
 * touchpad-gesture event); any pointer-capable device works as a fallback,
 * since Clutter's gesture dispatch switches on event type, not device
 * type/identity. */
static ClutterInputDevice *
meta_rdp_rdpei_find_source_device (ClutterSeat *seat)
{
  g_autoptr (GList) devices = clutter_seat_list_devices (seat);
  ClutterInputDevice *fallback = NULL;
  const GList *l;

  for (l = devices; l; l = l->next)
    {
      ClutterInputDevice *device = l->data;

      switch (clutter_input_device_get_device_type (device))
        {
        case CLUTTER_TOUCHPAD_DEVICE:
          return device;
        case CLUTTER_POINTER_DEVICE:
          if (!fallback)
            fallback = device;
          break;
        default:
          break;
        }
    }

  return fallback;
}

static const char *
meta_rdp_gesture_phase_name (ClutterTouchpadGesturePhase phase)
{
  switch (phase)
    {
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN:
      return "BEGIN";
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_UPDATE:
      return "UPDATE";
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_END:
      return "END";
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_CANCEL:
      return "CANCEL";
    }
  return "?";
}

static gboolean
meta_rdp_rdpei_dispatch (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;
  ClutterSeat *seat;
  ClutterInputDevice *source_device;
  GQueue *pending;
  graphene_point_t coords;

  g_mutex_lock (&peer_ctx->rdpei_mutex);
  pending = peer_ctx->rdpei_pending_gestures;
  peer_ctx->rdpei_pending_gestures = g_queue_new ();
  peer_ctx->rdpei_idle_id = 0;
  g_mutex_unlock (&peer_ctx->rdpei_mutex);

  seat = clutter_backend_get_default_seat (clutter_get_default_backend ());
  source_device = meta_rdp_rdpei_find_source_device (seat);
  if (!source_device)
    {
      g_warning ("rdp: no pointer-capable ClutterInputDevice found; dropping touchpad gesture");
      g_queue_free_full (pending, (GDestroyNotify) meta_rdp_pending_gesture_free);
      return G_SOURCE_REMOVE;
    }
  coords = GRAPHENE_POINT_INIT (peer_ctx->last_pointer_x, peer_ctx->last_pointer_y);

  while (!g_queue_is_empty (pending))
    {
      MetaRdpPendingGesture *item = g_queue_pop_head (pending);
      graphene_point_t delta = GRAPHENE_POINT_INIT (item->dx, item->dy);
      ClutterEvent *event;

      if (item->phase == CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN)
        {
          peer_ctx->rdpei_gesture_dispatched_dx = 0.0f;
          peer_ctx->rdpei_gesture_dispatched_dy = 0.0f;
        }
      else if (item->phase == CLUTTER_TOUCHPAD_GESTURE_PHASE_UPDATE)
        {
          peer_ctx->rdpei_gesture_dispatched_dx += item->dx;
          peer_ctx->rdpei_gesture_dispatched_dy += item->dy;
        }

      /* TOUCHPAD_BASE_WIDTH/HEIGHT from GNOME Shell's swipeTracker.js: the
       * fixed (not screen-size-related) distance its progress is
       * normalized against for horizontal/vertical touchpad swipes,
       * respectively -- included here so the running sums below can be
       * read directly as an approximate progress percentage. */
      meta_topic (META_DEBUG_REMOTE_DESKTOP,
                  "rdp: rdpei -> clutter touchpad-swipe %s fingers=%u delta=(%.1f,%.1f) "
                  "running=(%.1f,%.1f) [~%.0f%% of 400 horiz, ~%.0f%% of 300 vert]",
                  meta_rdp_gesture_phase_name (item->phase), item->fingers,
                  (double) item->dx, (double) item->dy,
                  (double) peer_ctx->rdpei_gesture_dispatched_dx,
                  (double) peer_ctx->rdpei_gesture_dispatched_dy,
                  (double) (peer_ctx->rdpei_gesture_dispatched_dx / 400.0f * 100.0f),
                  (double) (peer_ctx->rdpei_gesture_dispatched_dy / 300.0f * 100.0f));

      event = clutter_event_touchpad_swipe_new (CLUTTER_EVENT_NONE,
                                                g_get_monotonic_time (),
                                                source_device,
                                                item->phase,
                                                item->fingers,
                                                coords,
                                                delta,
                                                delta);
      clutter_event_put (event);
      clutter_event_free (event);

      meta_rdp_pending_gesture_free (item);
    }

  g_queue_free (pending);

  return G_SOURCE_REMOVE;
}

/* Call with rdpei_mutex held. */
static void
meta_rdp_rdpei_queue_gesture_locked (MetaRdpPeerContext          *peer_ctx,
                                     ClutterTouchpadGesturePhase  phase,
                                     uint32_t                     fingers,
                                     float                        dx,
                                     float                        dy)
{
  MetaRdpPendingGesture *pending = g_new (MetaRdpPendingGesture, 1);

  pending->phase = phase;
  pending->fingers = fingers;
  pending->dx = dx;
  pending->dy = dy;

  g_queue_push_tail (peer_ctx->rdpei_pending_gestures, pending);

  /* G_PRIORITY_DEFAULT for the same reason as the gfxredir dispatch: at idle
   * priority, gestures starve while the compositor keeps repainting, which
   * is exactly what an animated swipe makes it do. */
  if (!peer_ctx->rdpei_idle_id)
    peer_ctx->rdpei_idle_id =
      g_idle_add_full (G_PRIORITY_DEFAULT, meta_rdp_rdpei_dispatch,
                       peer_ctx, NULL);
}

/* Call with rdpei_mutex held. */
static void
meta_rdp_rdpei_end_gesture_locked (MetaRdpPeerContext          *peer_ctx,
                                   ClutterTouchpadGesturePhase  phase)
{
  meta_topic (META_DEBUG_REMOTE_DESKTOP,
              "rdp: rdpei gesture %s (fingers=%u, total emitted delta=(%.1f,%.1f))",
              meta_rdp_gesture_phase_name (phase),
              peer_ctx->rdpei_gesture_fingers,
              (double) peer_ctx->rdpei_gesture_total_dx,
              (double) peer_ctx->rdpei_gesture_total_dy);
  meta_rdp_rdpei_queue_gesture_locked (peer_ctx, phase,
                                       peer_ctx->rdpei_gesture_fingers,
                                       0.0f, 0.0f);
  peer_ctx->rdpei_gesture_active = FALSE;
  peer_ctx->rdpei_gesture_low_streak = 0;
  peer_ctx->rdpei_gesture_pending_streak = 0;
}

/* Main loop. Ends the gesture once touch frames have stopped for
 * META_RDP_GESTURE_TIMEOUT_MS; the contacts are forgotten too, so a client
 * that resumes starts a fresh gesture. */
static gboolean
meta_rdp_rdpei_timeout (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;
  gboolean keep = G_SOURCE_CONTINUE;

  g_mutex_lock (&peer_ctx->rdpei_mutex);

  if (!peer_ctx->rdpei_gesture_active)
    {
      keep = G_SOURCE_REMOVE;
    }
  else if (g_get_monotonic_time () - peer_ctx->rdpei_last_frame_us >=
           META_RDP_GESTURE_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND)
    {
      meta_rdp_rdpei_end_gesture_locked (peer_ctx,
                                         CLUTTER_TOUCHPAD_GESTURE_PHASE_END);
      g_hash_table_remove_all (peer_ctx->rdpei_contacts);
      keep = G_SOURCE_REMOVE;
    }

  if (keep == G_SOURCE_REMOVE)
    peer_ctx->rdpei_timeout_id = 0;

  g_mutex_unlock (&peer_ctx->rdpei_mutex);

  return keep;
}

/* Call with rdpei_mutex held. Re-evaluates gesture state against the
 * currently-tracked contacts after onTouchEvent has updated them for one
 * frame, queuing whatever ClutterEvent(s) that transition implies. */
static void
meta_rdp_rdpei_evaluate_gesture_locked (MetaRdpPeerContext *peer_ctx)
{
  guint n_contacts = g_hash_table_size (peer_ctx->rdpei_contacts);

  if (n_contacts < META_RDP_GESTURE_MIN_FINGERS)
    {
      if (!peer_ctx->rdpei_gesture_active)
        return;

      /* Debounce: don't tear the gesture down over what might be a single
       * transient HID under-report (e.g. one finger of a staggered
       * release lifting slightly before the others). With no contact left
       * at all it is a real release, and the client won't send the frames
       * the debounce would wait for. */
      peer_ctx->rdpei_gesture_low_streak++;
      if (n_contacts > 0 &&
          peer_ctx->rdpei_gesture_low_streak < META_RDP_GESTURE_FINGER_DEBOUNCE)
        {
          /* Refresh prev_centroid from whatever contacts remain (if any)
           * so that if the count recovers, the next delta is computed
           * against a *current* reference instead of one that's stale
           * from before the dip -- otherwise recovery produces one large
           * (and often backwards-pointing) spurious jump. This is what
           * caused the "jumps back a bit, then animates to the target"
           * artifact on release: a staggered release's momentary dip
           * below the threshold was tolerated correctly, but the eventual
           * real END (or a same-count recovery) still diffed against a
           * centroid from before the dip. */
          if (n_contacts > 0)
            {
              GHashTableIter iter;
              gpointer key, value;
              float sum_x = 0.0f, sum_y = 0.0f;

              g_hash_table_iter_init (&iter, peer_ctx->rdpei_contacts);
              while (g_hash_table_iter_next (&iter, &key, &value))
                {
                  graphene_point_t *p = value;

                  sum_x += p->x;
                  sum_y += p->y;
                }
              peer_ctx->rdpei_gesture_prev_centroid.x = sum_x / n_contacts;
              peer_ctx->rdpei_gesture_prev_centroid.y = sum_y / n_contacts;
            }
          return;
        }

      meta_rdp_rdpei_end_gesture_locked (peer_ctx,
                                         CLUTTER_TOUCHPAD_GESTURE_PHASE_END);
      return;
    }

  peer_ctx->rdpei_gesture_low_streak = 0;

  {
    GHashTableIter iter;
    gpointer key, value;
    float sum_x = 0.0f, sum_y = 0.0f;
    graphene_point_t centroid;

    g_hash_table_iter_init (&iter, peer_ctx->rdpei_contacts);
    while (g_hash_table_iter_next (&iter, &key, &value))
      {
        graphene_point_t *p = value;

        sum_x += p->x;
        sum_y += p->y;
      }
    centroid.x = sum_x / n_contacts;
    centroid.y = sum_y / n_contacts;

    if (!peer_ctx->rdpei_gesture_active)
      {
        meta_topic (META_DEBUG_REMOTE_DESKTOP,
                    "rdp: rdpei gesture BEGIN (fingers=%u)", n_contacts);
        meta_rdp_rdpei_queue_gesture_locked (peer_ctx,
                                             CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN,
                                             n_contacts, 0.0f, 0.0f);
        peer_ctx->rdpei_gesture_active = TRUE;
        peer_ctx->rdpei_gesture_fingers = n_contacts;
        peer_ctx->rdpei_gesture_prev_centroid = centroid;
        peer_ctx->rdpei_gesture_pending_streak = 0;
        peer_ctx->rdpei_gesture_total_dx = 0.0f;
        peer_ctx->rdpei_gesture_total_dy = 0.0f;

        /* g_timeout_add() attaches to the default main context, which is
         * safe from this (the rdpei) thread. */
        if (!peer_ctx->rdpei_timeout_id)
          {
            peer_ctx->rdpei_timeout_id =
              g_timeout_add (META_RDP_GESTURE_TIMEOUT_MS / 2,
                             meta_rdp_rdpei_timeout, peer_ctx);
          }
        return;
      }

    if (n_contacts != peer_ctx->rdpei_gesture_fingers)
      {
        /* Same debounce idea as the low-finger-count case above: a
         * momentary blip from e.g. 3 to 4 and back to 3 fingers shouldn't
         * cancel+restart the gesture and reset GNOME Shell's accumulated
         * progress. While debouncing, keep tracking motion under the
         * existing (pre-blip) finger count/gesture -- refresh
         * prev_centroid without emitting a delta for this one frame, so
         * the eventual resolution (either settling back to the original
         * count, or a real sustained change) doesn't produce a spurious
         * jump. */
        if (peer_ctx->rdpei_gesture_pending_fingers != n_contacts)
          {
            peer_ctx->rdpei_gesture_pending_fingers = n_contacts;
            peer_ctx->rdpei_gesture_pending_streak = 1;
          }
        else
          {
            peer_ctx->rdpei_gesture_pending_streak++;
          }

        if (peer_ctx->rdpei_gesture_pending_streak < META_RDP_GESTURE_FINGER_DEBOUNCE)
          {
            peer_ctx->rdpei_gesture_prev_centroid = centroid;
            return;
          }

        /* Sustained for long enough: this is a real change. Real touchpad
         * drivers cancel and restart in that case too (see
         * ClutterTouchpadGesturePhase's doc comment). */
        meta_topic (META_DEBUG_REMOTE_DESKTOP,
                    "rdp: rdpei gesture CANCEL+BEGIN: finger count %u -> %u persisted %u frames "
                    "(total emitted delta before cancel=(%.1f,%.1f))",
                    peer_ctx->rdpei_gesture_fingers, n_contacts,
                    peer_ctx->rdpei_gesture_pending_streak,
                    (double) peer_ctx->rdpei_gesture_total_dx,
                    (double) peer_ctx->rdpei_gesture_total_dy);
        meta_rdp_rdpei_queue_gesture_locked (peer_ctx,
                                             CLUTTER_TOUCHPAD_GESTURE_PHASE_CANCEL,
                                             peer_ctx->rdpei_gesture_fingers,
                                             0.0f, 0.0f);
        meta_rdp_rdpei_queue_gesture_locked (peer_ctx,
                                             CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN,
                                             n_contacts, 0.0f, 0.0f);
        peer_ctx->rdpei_gesture_fingers = n_contacts;
        peer_ctx->rdpei_gesture_prev_centroid = centroid;
        peer_ctx->rdpei_gesture_pending_streak = 0;
        peer_ctx->rdpei_gesture_total_dx = 0.0f;
        peer_ctx->rdpei_gesture_total_dy = 0.0f;
        return;
      }

    peer_ctx->rdpei_gesture_pending_streak = 0;

    /* Ongoing gesture, same finger count: emit the incremental delta. */
    {
      rdpSettings *settings = peer_ctx->rdp_context.settings;
      float width = (float) freerdp_settings_get_uint32 (settings,
                                                         FreeRDP_DesktopWidth);
      float height = (float) freerdp_settings_get_uint32 (settings,
                                                          FreeRDP_DesktopHeight);
      float dx, dy;

      if (width <= 0.0f)
        width = META_RDP_GESTURE_REFERENCE_WIDTH;
      if (height <= 0.0f)
        height = META_RDP_GESTURE_REFERENCE_HEIGHT;

      dx = (centroid.x - peer_ctx->rdpei_gesture_prev_centroid.x) / width *
           META_RDP_GESTURE_REFERENCE_WIDTH * META_RDP_GESTURE_DELTA_SCALE;
      dy = (centroid.y - peer_ctx->rdpei_gesture_prev_centroid.y) / height *
           META_RDP_GESTURE_REFERENCE_HEIGHT * META_RDP_GESTURE_DELTA_SCALE;

      if (dx != 0.0f || dy != 0.0f)
        {
          meta_rdp_rdpei_queue_gesture_locked (peer_ctx,
                                               CLUTTER_TOUCHPAD_GESTURE_PHASE_UPDATE,
                                               n_contacts, dx, dy);
          peer_ctx->rdpei_gesture_prev_centroid = centroid;
          peer_ctx->rdpei_gesture_total_dx += dx;
          peer_ctx->rdpei_gesture_total_dy += dy;
        }
    }
  }
}

/* Runs on the rdpei channel's own reader thread (rdpei_server_open() spawns
 * it, like gfxredir's Open()) -- must not touch Clutter directly. */
static UINT
meta_rdp_rdpei_touch_event (RdpeiServerContext          *context,
                            const RDPINPUT_TOUCH_EVENT  *touch_event)
{
  MetaRdpPeerContext *peer_ctx = context->user_data;
  UINT16 fi;

  g_mutex_lock (&peer_ctx->rdpei_mutex);

  peer_ctx->rdpei_last_frame_us = g_get_monotonic_time ();

  for (fi = 0; fi < touch_event->frameCount; fi++)
    {
      const RDPINPUT_TOUCH_FRAME *frame = &touch_event->frames[fi];
      UINT16 ci;

      for (ci = 0; ci < frame->contactCount; ci++)
        {
          const RDPINPUT_CONTACT_DATA *contact = &frame->contacts[ci];
          gpointer key = GUINT_TO_POINTER (contact->contactId);

          if (contact->contactFlags &
              (RDPINPUT_CONTACT_FLAG_UP | RDPINPUT_CONTACT_FLAG_CANCELED))
            {
              if (g_hash_table_remove (peer_ctx->rdpei_contacts, key))
                meta_topic (META_DEBUG_REMOTE_DESKTOP,
                            "rdp: rdpei contact %u up/canceled, %u contact(s) remain",
                            contact->contactId,
                            g_hash_table_size (peer_ctx->rdpei_contacts));
              continue;
            }

          if (contact->contactFlags &
              (RDPINPUT_CONTACT_FLAG_DOWN | RDPINPUT_CONTACT_FLAG_UPDATE))
            {
              graphene_point_t *p = g_hash_table_lookup (peer_ctx->rdpei_contacts, key);

              if (!p)
                {
                  p = g_new (graphene_point_t, 1);
                  g_hash_table_insert (peer_ctx->rdpei_contacts, key, p);
                  meta_topic (META_DEBUG_REMOTE_DESKTOP,
                              "rdp: rdpei contact %u down at (%d,%d), %u contact(s) now active",
                              contact->contactId, contact->x, contact->y,
                              g_hash_table_size (peer_ctx->rdpei_contacts));
                }
              p->x = (float) contact->x;
              p->y = (float) contact->y;
            }
        }

      /* Evaluate once per frame, not once per event: a touch event can
       * bundle several frames, each a distinct instant in time. */
      meta_rdp_rdpei_evaluate_gesture_locked (peer_ctx);
    }

  g_mutex_unlock (&peer_ctx->rdpei_mutex);

  return CHANNEL_RC_OK;
}

static UINT
meta_rdp_rdpei_client_ready (RdpeiServerContext *context)
{
  g_message ("rdp: rdpei client ready (version 0x%08x, max touch points %u)",
             context->clientVersion, context->maxTouchPoints);
  return CHANNEL_RC_OK;
}

/* Fires once the background thread has actually opened the WTS channel --
 * the earliest point at which sending SC_READY (the server-initiated half of
 * the MS-RDPEI handshake) can succeed. Runs on the rdpei thread, but
 * rdpei_server_send_sc_ready() is plain channel I/O, not a Clutter call, so
 * it is fine to call directly from here. */
static BOOL
meta_rdp_rdpei_channel_id_assigned (RdpeiServerContext *context,
                                    UINT32              channel_id)
{
  UINT error;

  error = rdpei_server_send_sc_ready (context, RDPINPUT_PROTOCOL_V300, 0);
  if (error != CHANNEL_RC_OK)
    {
      g_warning ("rdp: rdpei send_sc_ready failed with error %u", error);
      return FALSE;
    }

  return TRUE;
}

static void
meta_rdp_setup_rdpei (MetaRdpPeerContext *peer_ctx)
{
  RdpeiServerContext *rdpei;

  if (peer_ctx->rdpei)
    return;

  rdpei = rdpei_server_context_new (peer_ctx->vcm);
  if (!rdpei)
    {
      g_warning ("rdp: rdpei_server_context_new failed; touch/gesture forwarding disabled");
      return;
    }

  rdpei->user_data = peer_ctx;
  rdpei->onChannelIdAssigned = meta_rdp_rdpei_channel_id_assigned;
  rdpei->onClientReady = meta_rdp_rdpei_client_ready;
  rdpei->onTouchEvent = meta_rdp_rdpei_touch_event;

  if (rdpei->Open (rdpei) != CHANNEL_RC_OK)
    {
      g_warning ("rdp: rdpei Open failed; touch/gesture forwarding disabled");
      rdpei_server_context_free (rdpei);
      return;
    }

  peer_ctx->rdpei = rdpei;
  g_message ("rdp: rdpei channel opened");
}

/* ------------------------------------------------------------------ */
/* input injection (RDP keyboard/mouse -> clutter virtual devices) */
/* ------------------------------------------------------------------ */

/* Locally define missing keyboard layout IDs in FreeRDP 2.x, as Weston does. */
#ifndef KBD_HEBREW_STANDARD
#define KBD_HEBREW_STANDARD 0x2040d
#endif
#ifndef KBD_PERSIAN
#define KBD_PERSIAN 0x50429
#endif
#ifndef KBD_FRENCH_STANDARD_BEPO
#define KBD_FRENCH_STANDARD_BEPO 0x2040c
#endif
#ifndef KBD_FRENCH_STANDARD_AZERTY
#define KBD_FRENCH_STANDARD_AZERTY 0x1040c
#endif

struct rdp_to_xkb_keyboard_layout
{
  UINT32 rdpLayoutCode;
  const char *xkbLayout;
  const char *xkbVariant;
};

/* Reversed from FreeRDP's xkb_layout_ids.c; ported verbatim from Weston's
 * rdp.c rdp_keyboards[]. */
static const struct rdp_to_xkb_keyboard_layout rdp_keyboards[] = {
  { KBD_ARABIC_101, "ara", 0 },
  { KBD_BULGARIAN, 0, 0 },
  { KBD_CHINESE_TRADITIONAL_US, 0, 0 },
  { KBD_CZECH, "cz", 0 },
  { KBD_CZECH_PROGRAMMERS, "cz", "bksl" },
  { KBD_CZECH_QWERTY, "cz", "qwerty" },
  { KBD_DANISH, "dk", 0 },
  { KBD_GERMAN, "de", 0 },
  { KBD_GERMAN_NEO, "de", "neo" },
  { KBD_GERMAN_IBM, "de", "qwerty" },
  { KBD_GREEK, "gr", 0 },
  { KBD_GREEK_220, "gr", "simple" },
  { KBD_GREEK_319, "gr", "extended" },
  { KBD_GREEK_POLYTONIC, "gr", "polytonic" },
  { KBD_US, "us", 0 },
  { KBD_UNITED_STATES_INTERNATIONAL, "us", "intl" },
  { KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L, "ara", "buckwalter" },
  { KBD_SPANISH, "es", 0 },
  { KBD_SPANISH_VARIATION, "es", "nodeadkeys" },
  { KBD_FINNISH, "fi", 0 },
  { KBD_FRENCH, "fr", 0 },
  { KBD_FRENCH_STANDARD_BEPO, "fr", "bepo" },
  { KBD_FRENCH_STANDARD_AZERTY, "fr", "afnor" },
  { KBD_HEBREW, "il", 0 },
  { KBD_HEBREW_STANDARD, "il", "basic" },
  { KBD_HUNGARIAN, "hu", 0 },
  { KBD_HUNGARIAN_101_KEY, "hu", "standard" },
  { KBD_ICELANDIC, "is", 0 },
  { KBD_ITALIAN, "it", 0 },
  { KBD_ITALIAN_142, "it", "nodeadkeys" },
  { KBD_JAPANESE, "jp", 0 },
  { KBD_JAPANESE_INPUT_SYSTEM_MS_IME2002, "jp", 0 },
  { KBD_KOREAN, "kr", 0 },
  { KBD_KOREAN_INPUT_SYSTEM_IME_2000, "kr", "kr104" },
  { KBD_DUTCH, "nl", 0 },
  { KBD_NORWEGIAN, "no", 0 },
  { KBD_POLISH_PROGRAMMERS, "pl", 0 },
  { KBD_POLISH_214, "pl", "qwertz" },
  { KBD_ROMANIAN, "ro", 0 },
  { KBD_RUSSIAN, "ru", 0 },
  { KBD_RUSSIAN_TYPEWRITER, "ru", "typewriter" },
  { KBD_CROATIAN, "hr", 0 },
  { KBD_SLOVAK, "sk", 0 },
  { KBD_SLOVAK_QWERTY, "sk", "qwerty" },
  { KBD_ALBANIAN, 0, 0 },
  { KBD_SWEDISH, "se", 0 },
  { KBD_THAI_KEDMANEE, "th", 0 },
  { KBD_THAI_KEDMANEE_NON_SHIFTLOCK, "th", "tis" },
  { KBD_TURKISH_Q, "tr", 0 },
  { KBD_TURKISH_F, "tr", "f" },
  { KBD_URDU, "in", "urd-phonetic3" },
  { KBD_UKRAINIAN, "ua", 0 },
  { KBD_BELARUSIAN, "by", 0 },
  { KBD_SLOVENIAN, "si", 0 },
  { KBD_ESTONIAN, "ee", 0 },
  { KBD_LATVIAN, "lv", 0 },
  { KBD_LITHUANIAN, "lt", 0 },
  { KBD_LITHUANIAN_IBM, "lt", "ibm" },
  { KBD_FARSI, "ir", "pes" },
  { KBD_PERSIAN, "af", "basic" },
  { KBD_VIETNAMESE, "vn", 0 },
  { KBD_ARMENIAN_EASTERN, "am", 0 },
  { KBD_AZERI_LATIN, 0, 0 },
  { KBD_FYRO_MACEDONIAN, "mk", 0 },
  { KBD_GEORGIAN, "ge", 0 },
  { KBD_FAEROESE, 0, 0 },
  { KBD_DEVANAGARI_INSCRIPT, 0, 0 },
  { KBD_MALTESE_47_KEY, 0, 0 },
  { KBD_NORWEGIAN_WITH_SAMI, "no", "smi" },
  { KBD_KAZAKH, "kz", 0 },
  { KBD_KYRGYZ_CYRILLIC, "kg", "phonetic" },
  { KBD_TATAR, "ru", "tt" },
  { KBD_BENGALI, "bd", 0 },
  { KBD_BENGALI_INSCRIPT, "bd", "probhat" },
  { KBD_PUNJABI, 0, 0 },
  { KBD_GUJARATI, "in", "guj" },
  { KBD_TAMIL, "in", "tam" },
  { KBD_TELUGU, "in", "tel" },
  { KBD_KANNADA, "in", "kan" },
  { KBD_MALAYALAM, "in", "mal" },
  { KBD_HINDI_TRADITIONAL, "in", 0 },
  { KBD_MARATHI, 0, 0 },
  { KBD_MONGOLIAN_CYRILLIC, "mn", 0 },
  { KBD_UNITED_KINGDOM_EXTENDED, "gb", "intl" },
  { KBD_SYRIAC, "syc", 0 },
  { KBD_SYRIAC_PHONETIC, "syc", "syc_phonetic" },
  { KBD_NEPALI, "np", 0 },
  { KBD_PASHTO, "af", "ps" },
  { KBD_DIVEHI_PHONETIC, 0, 0 },
  { KBD_LUXEMBOURGISH, 0, 0 },
  { KBD_MAORI, "mao", 0 },
  { KBD_CHINESE_SIMPLIFIED_US, 0, 0 },
  { KBD_SWISS_GERMAN, "ch", "de_nodeadkeys" },
  { KBD_UNITED_KINGDOM, "gb", 0 },
  { KBD_LATIN_AMERICAN, "latam", 0 },
  { KBD_BELGIAN_FRENCH, "be", 0 },
  { KBD_BELGIAN_PERIOD, "be", "oss_sundeadkeys" },
  { KBD_PORTUGUESE, "pt", 0 },
  { KBD_SERBIAN_LATIN, "rs", 0 },
  { KBD_AZERI_CYRILLIC, "az", "cyrillic" },
  { KBD_SWEDISH_WITH_SAMI, "se", "smi" },
  { KBD_UZBEK_CYRILLIC, "af", "uz" },
  { KBD_INUKTITUT_LATIN, "ca", "ike" },
  { KBD_CANADIAN_FRENCH_LEGACY, "ca", "fr-legacy" },
  { KBD_SERBIAN_CYRILLIC, "rs", 0 },
  { KBD_CANADIAN_FRENCH, "ca", 0 },
  { KBD_SWISS_FRENCH, "ch", "fr" },
  { KBD_BOSNIAN, "ba", 0 },
  { KBD_IRISH, 0, 0 },
  { KBD_BOSNIAN_CYRILLIC, "ba", "us" },
  { KBD_UNITED_STATES_DVORAK, "us", "dvorak" },
  { KBD_PORTUGUESE_BRAZILIAN_ABNT2, "br", "abnt2" },
  { KBD_CANADIAN_MULTILINGUAL_STANDARD, "ca", "multix" },
  { KBD_GAELIC, "ie", "CloGaelach" },
  { 0x00000000, 0, 0 },
};

static void
meta_rdp_apply_keymap (MetaRdpServer *self,
                       rdpSettings   *settings)
{
  const char *layout = NULL;
  const char *variant = NULL;
  g_autoptr (MetaKeymapDescription) description = NULL;
  uint32_t keyboard_layout =
    freerdp_settings_get_uint32 (settings, FreeRDP_KeyboardLayout);
  uint32_t keyboard_type =
    freerdp_settings_get_uint32 (settings, FreeRDP_KeyboardType);
  uint32_t keyboard_sub_type =
    freerdp_settings_get_uint32 (settings, FreeRDP_KeyboardSubType);
  int i;

  for (i = 0; rdp_keyboards[i].rdpLayoutCode; i++)
    {
      if (rdp_keyboards[i].rdpLayoutCode == keyboard_layout)
        {
          layout = rdp_keyboards[i].xkbLayout;
          variant = rdp_keyboards[i].xkbVariant;
          break;
        }
    }

  /* Korean keyboard support (KeyboardType 8, LangID 0x412). */
  if (keyboard_type == 8 && (keyboard_layout & 0xFFFF) == 0x412)
    {
      if (keyboard_sub_type == 0 || keyboard_sub_type == 3)
        variant = "kr104";
      else if (keyboard_sub_type == 6)
        variant = "kr106";
    }
  /* Japanese layout with non-Japanese keyboard falls back to "us". */
  else if (keyboard_type != 7 && (keyboard_layout & 0xFFFF) == 0x411)
    {
      layout = "us";
      variant = NULL;
    }

  if (!layout)
    {
      g_message ("rdp: no xkb layout for RDP layout 0x%x; keeping default",
                 keyboard_layout);
      return;
    }

  g_message ("rdp: keyboard layout 0x%x -> xkb model=pc105 layout=%s variant=%s",
             keyboard_layout, layout, variant ? variant : "(none)");

  description = meta_keymap_description_new_from_rules ("pc105", layout, variant,
                                                       NULL, NULL, NULL);
  if (!description)
    return;

  meta_backend_set_keymap_async (self->backend, description, 0, NULL,
                                 NULL, NULL);
}

static void
meta_rdp_ensure_virtual_pointer (MetaRdpPeerContext *peer_ctx)
{
  MetaBackend *backend = peer_ctx->server->backend;
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_backend);

  if (!peer_ctx->virtual_pointer)
    {
      peer_ctx->virtual_pointer =
        clutter_seat_create_virtual_device (seat, CLUTTER_POINTER_DEVICE);
    }
}

static void
meta_rdp_ensure_virtual_keyboard (MetaRdpPeerContext *peer_ctx)
{
  MetaBackend *backend = peer_ctx->server->backend;
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_backend);

  if (!peer_ctx->virtual_keyboard)
    {
      peer_ctx->virtual_keyboard =
        clutter_seat_create_virtual_device (seat, CLUTTER_KEYBOARD_DEVICE);
    }
}

/* Absolute pointer motion.
 *
 * RDP pointer coordinates are in desktop pixels -- the same space as
 * FreeRDP_DesktopWidth/Height and our framebuffer -- while Clutter wants stage
 * coordinates, which are logical. On a scaled desktop those differ by the
 * monitor scale, so divide. Weston's to_weston_coordinate() does the same
 * (rdpdisp.c), including the per-head origin offset we don't need while there
 * is only one output at (0,0). */
static void
meta_rdp_notify_pointer_position (MetaRdpPeerContext *peer_ctx,
                                  UINT16              x,
                                  UINT16              y)
{
  float scale = meta_rdp_server_get_scale (peer_ctx->server);

  peer_ctx->last_pointer_x = (float) x / scale;
  peer_ctx->last_pointer_y = (float) y / scale;

  meta_rdp_ensure_virtual_pointer (peer_ctx);
  clutter_virtual_input_device_notify_absolute_motion (peer_ctx->virtual_pointer,
                                                       CLUTTER_CURRENT_TIME,
                                                       (double) x / scale,
                                                       (double) y / scale);
}

/* Debounce redundant button state, matching Weston's rdp_validate_button_state.
 * Returns TRUE if the (evdev) button should be injected. */
static gboolean
meta_rdp_validate_button_state (MetaRdpPeerContext *peer_ctx,
                                gboolean            pressed,
                                uint32_t            button)
{
  uint32_t index = button - BTN_LEFT;

  if (index >= G_N_ELEMENTS (peer_ctx->button_state))
    return FALSE;

  if (pressed == peer_ctx->button_state[index])
    return FALSE;

  peer_ctx->button_state[index] = pressed;
  return TRUE;
}

static void
meta_rdp_notify_button (MetaRdpPeerContext *peer_ctx,
                        uint32_t            evdev_button,
                        gboolean            pressed)
{
  if (!meta_rdp_validate_button_state (peer_ctx, pressed, evdev_button))
    return;

  meta_rdp_ensure_virtual_pointer (peer_ctx);
  clutter_virtual_input_device_notify_button (peer_ctx->virtual_pointer,
                                              CLUTTER_CURRENT_TIME,
                                              meta_evdev_button_to_clutter (evdev_button),
                                              pressed ? CLUTTER_BUTTON_STATE_PRESSED
                                                      : CLUTTER_BUTTON_STATE_RELEASED);
}

/* Precise/discrete wheel accumulation ported from Weston's
 * rdp_notify_wheel_scroll. */
static void
meta_rdp_notify_wheel_scroll (MetaRdpPeerContext    *peer_ctx,
                              UINT16                 flags,
                              gboolean               horizontal)
{
  int ivalue;
  int *accum_precise;
  int *accum_discrete;
  ClutterScrollDirection direction;

  ivalue = (int) (flags & 0x000000ff);
  if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
    ivalue = (0xff - ivalue) * -1;

  if (!horizontal)
    {
      /* RDP vertical direction is inverse of Wayland. */
      ivalue *= -1;
      accum_precise = &peer_ctx->vertical_accum_wheel_precise;
      accum_discrete = &peer_ctx->vertical_accum_wheel_discrete;
    }
  else
    {
      accum_precise = &peer_ctx->horizontal_accum_wheel_precise;
      accum_discrete = &peer_ctx->horizontal_accum_wheel_discrete;
    }

  *accum_precise += ivalue;
  *accum_discrete += ivalue;

  if (abs (*accum_precise) >= 12)
    {
      int steps = *accum_discrete / 120;
      int n;

      if (steps == 0)
        steps = (*accum_precise > 0) ? 1 : -1;

      meta_rdp_ensure_virtual_pointer (peer_ctx);

      if (!horizontal)
        direction = (steps < 0) ? CLUTTER_SCROLL_UP : CLUTTER_SCROLL_DOWN;
      else
        direction = (steps < 0) ? CLUTTER_SCROLL_LEFT : CLUTTER_SCROLL_RIGHT;

      for (n = 0; n < abs (steps); n++)
        {
          clutter_virtual_input_device_notify_discrete_scroll (peer_ctx->virtual_pointer,
                                                               CLUTTER_CURRENT_TIME,
                                                               direction,
                                                               CLUTTER_SCROLL_SOURCE_WHEEL);
        }

      *accum_precise %= 12;
      *accum_discrete %= 120;
    }
}

static BOOL
meta_rdp_mouse_event (rdpInput *input,
                      UINT16    flags,
                      UINT16    x,
                      UINT16    y)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) input->context;
  uint32_t button = 0;

  if (!peer_ctx->activated)
    return TRUE;

  if (!(flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)))
    meta_rdp_notify_pointer_position (peer_ctx, x, y);

  if (flags & PTR_FLAGS_BUTTON1)
    button = peer_ctx->mouse_button_swap ? BTN_RIGHT : BTN_LEFT;
  else if (flags & PTR_FLAGS_BUTTON2)
    button = peer_ctx->mouse_button_swap ? BTN_LEFT : BTN_RIGHT;
  else if (flags & PTR_FLAGS_BUTTON3)
    button = BTN_MIDDLE;

  if (button)
    meta_rdp_notify_button (peer_ctx, button, (flags & PTR_FLAGS_DOWN) ? TRUE : FALSE);

  /* Per RDP spec, if both WHEEL and HWHEEL are set, WHEEL takes precedence. */
  if (flags & PTR_FLAGS_WHEEL)
    meta_rdp_notify_wheel_scroll (peer_ctx, flags, FALSE);
  else if (flags & PTR_FLAGS_HWHEEL)
    meta_rdp_notify_wheel_scroll (peer_ctx, flags, TRUE);

  return TRUE;
}

static BOOL
meta_rdp_extended_mouse_event (rdpInput *input,
                               UINT16    flags,
                               UINT16    x,
                               UINT16    y)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) input->context;
  uint32_t button = 0;

  if (!peer_ctx->activated)
    return TRUE;

  meta_rdp_notify_pointer_position (peer_ctx, x, y);

  if (flags & PTR_XFLAGS_BUTTON1)
    button = BTN_SIDE;
  else if (flags & PTR_XFLAGS_BUTTON2)
    button = BTN_EXTRA;

  if (button)
    meta_rdp_notify_button (peer_ctx, button, (flags & PTR_XFLAGS_DOWN) ? TRUE : FALSE);

  return TRUE;
}

static BOOL
meta_rdp_keyboard_event (rdpInput *input,
                         UINT16    flags,
                         UINT8     code)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) input->context;
  freerdp_peer *client = input->context->peer;
  rdpSettings *settings = client->context->settings;
  uint32_t keyboard_type =
    freerdp_settings_get_uint32 (settings, FreeRDP_KeyboardType);
  uint32_t scan_code, vk_code, full_code, keyboard_locale;
  ClutterKeyState key_state;
  gboolean send_release_key = FALSE;

  if (!peer_ctx->activated)
    return TRUE;

  /* KBD_FLAGS_DOWN no longer means "key press" in FreeRDP 3 -- it flags a
   * repeat of an already-down key. Absence of KBD_FLAGS_RELEASE is what
   * denotes a press. */
  if (flags & KBD_FLAGS_RELEASE)
    key_state = CLUTTER_KEY_STATE_RELEASED;
  else
    key_state = CLUTTER_KEY_STATE_PRESSED;

  full_code = code;
  /* Windows 10 reports extended bit for right shift (0x36) under certain
   * locales due to a bug; drop it. */
  keyboard_locale =
    freerdp_settings_get_uint32 (settings, FreeRDP_KeyboardLayout) & 0xFFFF;
  if (code == 0x36 &&
      (keyboard_locale == KBD_CHINESE_TRADITIONAL_US ||
       keyboard_locale == KBD_CHINESE_SIMPLIFIED_US ||
       keyboard_locale == KBD_JAPANESE))
    {
      flags &= ~KBD_FLAGS_EXTENDED;
    }
  else if (flags & KBD_FLAGS_EXTENDED)
    {
      full_code |= KBD_FLAGS_EXTENDED;
    }

  /* Korean HANJA/HANGEUL keys have no release event; synthesize one. */
#define ATKBD_RET_HANJA 0xf1
#define ATKBD_RET_HANGEUL 0xf2
  if (keyboard_type == 8 &&
      freerdp_settings_get_uint32 (settings, FreeRDP_KeyboardSubType) == 6 &&
      (full_code == (KBD_FLAGS_EXTENDED | ATKBD_RET_HANJA) ||
       full_code == (KBD_FLAGS_EXTENDED | ATKBD_RET_HANGEUL)))
    {
      if (full_code == (KBD_FLAGS_EXTENDED | ATKBD_RET_HANJA))
        vk_code = VK_HANJA;
      else
        vk_code = VK_HANGUL;
      send_release_key = TRUE;
    }
  else
    {
      vk_code = GetVirtualKeyCodeFromVirtualScanCode (full_code, keyboard_type);
    }

  if (vk_code != VK_HANGUL && vk_code != VK_HANJA)
    if (flags & KBD_FLAGS_EXTENDED)
      vk_code |= KBDEXT;

  scan_code = GetKeycodeFromVirtualKeyCode (vk_code, WINPR_KEYCODE_TYPE_XKB);

  /* 0 for keys WinPR can't map; anything below 8 would wrap around below. */
  if (scan_code < 8)
    return TRUE;

  meta_rdp_ensure_virtual_keyboard (peer_ctx);

  /* clutter/evdev keycodes are xkb keycodes minus 8. */
  clutter_virtual_input_device_notify_key (peer_ctx->virtual_keyboard,
                                           CLUTTER_CURRENT_TIME,
                                           scan_code - 8,
                                           key_state);

  if (send_release_key)
    {
      clutter_virtual_input_device_notify_key (peer_ctx->virtual_keyboard,
                                               CLUTTER_CURRENT_TIME,
                                               scan_code - 8,
                                               CLUTTER_KEY_STATE_RELEASED);
    }

#undef ATKBD_RET_HANJA
#undef ATKBD_RET_HANGEUL

  return TRUE;
}

static BOOL
meta_rdp_unicode_keyboard_event (rdpInput *input,
                                 UINT16    flags,
                                 UINT16    code)
{
  g_warning ("rdp: unhandled unicode keyboard event (flags:0x%X code:0x%X)",
             flags, code);
  return TRUE;
}

static BOOL
meta_rdp_synchronize_event (rdpInput *input,
                            UINT32    flags)
{
  /* Lock-key sync is a nice-to-have; clutter tracks its own lock state. */
  return TRUE;
}

/* Called from FreeRDP's connection state machine when the client has sent its
 * monitor list, before activation (libfreerdp/core/peer.c). This is where the
 * client's real geometry shows up; FreeRDP_DesktopWidth/Height only describes
 * the primary monitor, and a client reporting several monitors would otherwise
 * leave us guessing. Weston's equivalent is handle_adjust_monitor_layout()
 * in rdpdisp.c, which merges the whole list into its heads -- being
 * single-head, we take the primary and ignore the rest.
 *
 * Runs on the main thread: peer PDUs are pumped from rdp_client_activity(). */
static BOOL
xf_peer_adjust_monitor_layout (freerdp_peer *client)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) client->context;
  rdpSettings *settings = client->context->settings;
  UINT32 n_monitors = freerdp_settings_get_uint32 (settings, FreeRDP_MonitorCount);
  const rdpMonitor *primary = NULL;

  /* FreeRDP synthesises a primary from DesktopWidth/Height when the client
   * sent no list, but only after this callback -- so an empty list here just
   * means xf_peer_activate() will do the job instead. */
  if (n_monitors == 0)
    return TRUE;

  for (UINT32 i = 0; i < n_monitors; i++)
    {
      const rdpMonitor *monitor =
        freerdp_settings_get_pointer_array (settings, FreeRDP_MonitorDefArray, i);

      if (!monitor)
        continue;

      g_message ("rdp: client monitor[%u] %dx%d+%d+%d scale %u%%%s",
                 i, monitor->width, monitor->height, monitor->x, monitor->y,
                 monitor->attributes.desktopScaleFactor,
                 monitor->is_primary ? " (primary)" : "");

      if (monitor->is_primary && !primary)
        primary = monitor;
    }

  if (!primary)
    primary = freerdp_settings_get_pointer_array (settings,
                                                  FreeRDP_MonitorDefArray, 0);

  if (primary)
    {
      if (n_monitors > 1)
        g_message ("rdp: only the primary monitor is used (single-head)");

      meta_rdp_server_resize_monitor (peer_ctx->server,
                                      primary->width, primary->height,
                                      meta_rdp_scale_from_percent (primary->attributes.desktopScaleFactor,
                                                                   primary->width,
                                                                   primary->height));
    }

  return TRUE;
}

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

static gboolean rdp_client_activity (gpointer data);
static void meta_rdp_peer_flush_channels (gpointer user_data);

static BOOL meta_rdp_peer_finish_activation (freerdp_peer *client);

#define META_RDP_DRDYNVC_TIMEOUT_S 30

/* Main loop, every 10 ms while a peer waits for drdynvc to reach READY.
 * FreeRDP 2 only had DRDYNVC_STATE_NONE before READY; 3.x adds intermediate
 * states (INITIALIZED, ...), so wait for READY rather than keying off NONE. */
static gboolean
meta_rdp_drdynvc_wait (gpointer user_data)
{
  freerdp_peer *client = user_data;
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) client->context;
  gboolean ok;

  if (WTSVirtualChannelManagerGetDrdynvcState (peer_ctx->vcm) ==
      DRDYNVC_STATE_READY)
    {
      peer_ctx->drdynvc_wait_id = 0;
      if (!meta_rdp_peer_finish_activation (client))
        meta_rdp_peer_destroy (peer_ctx);
      return G_SOURCE_REMOVE;
    }

  if (g_get_monotonic_time () - peer_ctx->drdynvc_wait_start_us >
      META_RDP_DRDYNVC_TIMEOUT_S * G_USEC_PER_SEC)
    {
      g_warning ("rdp: drdynvc did not reach READY state");
      goto fail;
    }

  if (!client->CheckFileDescriptor (client))
    {
      g_warning ("rdp: peer died while waiting for drdynvc");
      goto fail;
    }

  /* FreeRDP 3 asserts if CheckFileDescriptor runs before the client has
   * joined drdynvc. */
  if (!WTSVirtualChannelManagerIsChannelJoined (peer_ctx->vcm,
                                                DRDYNVC_SVC_CHANNEL_NAME))
    return G_SOURCE_CONTINUE;

  g_mutex_lock (&peer_ctx->vcm_drain_mutex);
  ok = WTSVirtualChannelManagerCheckFileDescriptor (peer_ctx->vcm);
  g_mutex_unlock (&peer_ctx->vcm_drain_mutex);

  if (!ok)
    {
      g_warning ("rdp: WTS VC check failed while waiting for drdynvc");
      goto fail;
    }

  return G_SOURCE_CONTINUE;

fail:
  peer_ctx->drdynvc_wait_id = 0;
  meta_rdp_peer_destroy (peer_ctx);
  return G_SOURCE_REMOVE;
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
             freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth),
             freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight),
             freerdp_settings_get_uint32 (settings, FreeRDP_ColorDepth),
             freerdp_settings_get_bool (settings, FreeRDP_SurfaceCommandsEnabled),
             freerdp_settings_get_bool (settings, FreeRDP_RemoteFxCodec),
             freerdp_settings_get_bool (settings, FreeRDP_NSCodec),
             freerdp_settings_get_bool (settings, FreeRDP_SupportGraphicsPipeline));

  if (!freerdp_settings_get_bool (settings, FreeRDP_SurfaceCommandsEnabled))
    {
      g_warning ("rdp: client doesn't support required SurfaceCommands");
      return FALSE;
    }

  /* Weston's xf_peer_activate brings the virtual channels up first, on every
   * activation, and only then falls through to the once-per-peer setup. Keep
   * that order: the dynamic channels need drdynvc READY. */
  if (!peer_ctx->vcm)
    {
      g_warning ("rdp: virtual channel manager is required for clipboard "
                 "and gfxredir");
      return FALSE;
    }

  /* gfxredir is a dynamic virtual channel; drdynvc must be READY first. */
  if (!meta_rdp_start_drdynvc (peer_ctx))
    return FALSE;

  if (WTSVirtualChannelManagerGetDrdynvcState (peer_ctx->vcm) !=
      DRDYNVC_STATE_READY)
    {
      /* Ported from Weston's rdp_drdynvc_init, which busy-waited for READY
       * here -- up to 100 s on the compositor thread. Pump from a timer
       * instead and finish activating once it is READY. The pump only
       * auto-opens drdynvc for an activated client. */
      client->activated = TRUE;

      if (!peer_ctx->drdynvc_wait_id)
        {
          peer_ctx->drdynvc_wait_start_us = g_get_monotonic_time ();
          peer_ctx->drdynvc_wait_id =
            g_timeout_add (10, meta_rdp_drdynvc_wait, client);
        }
      return TRUE;
    }

  return meta_rdp_peer_finish_activation (client);
}

static BOOL
meta_rdp_peer_finish_activation (freerdp_peer *client)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) client->context;
  rdpSettings *settings = client->context->settings;

  /* The client dictates the resolution: adopt whatever it negotiated. This
   * replaces the --virtual-monitor size the session started at. Only on the
   * first activation: a re-activation follows our own DesktopResize, which
   * display control already applied -- including its scale, which
   * FreeRDP_DesktopScaleFactor (still the connect-time value) would undo,
   * and a newer layout request may already be on its way. */
  if (!peer_ctx->activated)
    {
      int width = (int) freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth);
      int height = (int) freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight);
      uint32_t scale_percent =
        freerdp_settings_get_uint32 (settings, FreeRDP_DesktopScaleFactor);

      meta_rdp_server_resize_monitor (peer_ctx->server, width, height,
                                      meta_rdp_scale_from_percent (scale_percent,
                                                                   width, height));
    }

  /* Everything past here is first-activation-only setup. */
  if (peer_ctx->activated)
    {
      /* A re-activation, i.e. the client came back after a DesktopResize. Its
       * surface was just recreated at the new size and holds nothing, so
       * repaint all of it -- the incremental damage path would leave most of
       * the screen blank. */
      if (peer_ctx->resize_pending)
        {
          peer_ctx->resize_pending = FALSE;
          g_message ("rdp: peer %p re-activated at %ux%u, repainting", client,
                     freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth),
                     freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight));
          peer_ctx->error_frame_width = 0;
          peer_ctx->error_frame_height = 0;
          meta_rdp_peer_force_full_present (peer_ctx);
        }

      return TRUE;
    }

  peer_ctx->activated = TRUE;
  g_message ("rdp: first activation complete for peer %p", client);

  /* Sync the xkb layout to the client's reported RDP keyboard layout. */
  meta_rdp_apply_keymap (peer_ctx->server, settings);

  /* Bridge the clipboard. CLIPRDR is a static channel, but Weston initialises
   * it last -- after drdynvc and the seat -- so do the same. */
  if (!peer_ctx->clipboard)
    {
      peer_ctx->clipboard = meta_rdp_clipboard_new (client,
                                                    peer_ctx->server->backend,
                                                    peer_ctx->vcm);

      if (peer_ctx->clipboard)
        {
          HANDLE h = meta_rdp_clipboard_get_event_handle (peer_ctx->clipboard);
          int fd = h ? GetEventFileDescriptor (h) : -1;

          if (fd >= 0 &&
              peer_ctx->n_fd_sources < META_RDP_MAX_PEER_FD_SOURCES)
            {
              GSource *source =
                meta_rdp_add_fd_source (fd, rdp_client_activity, client,
                                        "cliprdr");

              peer_ctx->fd_sources[peer_ctx->n_fd_sources++] = source;
              peer_ctx->clipboard_fd_source = source;
            }
        }
    }

  /* The client has no pointer shape until we send one. */
  meta_rdp_peer_update_pointer (peer_ctx);

  /* Before gfxredir, as Weston does. */
  meta_rdp_setup_disp (peer_ctx);

  if (!peer_ctx->audio_out)
    peer_ctx->audio_out = meta_rdp_audio_out_new (peer_ctx->vcm,
                                                  meta_rdp_peer_flush_channels,
                                                  peer_ctx);
  if (!peer_ctx->audio_in)
    peer_ctx->audio_in = meta_rdp_audio_in_new (peer_ctx->vcm);

  meta_rdp_setup_gfxredir (peer_ctx);
  meta_rdp_setup_rdpei (peer_ctx);

  /* Nothing to present yet. The first full frame is forced once gfxredir caps
   * are confirmed (gfxredir_caps_advertise()), or, if gfxredir cannot be used,
   * by meta_rdp_peer_fail(). */
  return TRUE;
}

/* Flush queued channel data to the socket from a non-main thread.
 *
 * Everything below CheckFileDescriptorEx is safe to reach from another thread:
 * the message queue has its own lock, each PDU gets a freshly allocated stream
 * (transport_send_stream_init), and transport_write serialises on
 * transport->WriteLock. The one stateful hazard -- legacy RDP encryption, with
 * its RC4 state and sequence counter -- does not apply because we only ever
 * enable TLS, never FreeRDP_RdpSecurity, so rdp->do_crypt stays FALSE.
 *
 * autoOpen is deliberately FALSE: opening drdynvc is main-loop business, gated
 * on client->activated, and has no place on an audio thread.
 *
 * Called by the audio bridge after submitting a packet, so that playback does
 * not have to wait for the next main-loop iteration to reach the wire. */
static void
meta_rdp_peer_flush_channels (gpointer user_data)
{
  MetaRdpPeerContext *peer_ctx = user_data;

  if (!peer_ctx || !peer_ctx->vcm)
    return;

  g_mutex_lock (&peer_ctx->vcm_drain_mutex);
  (void) WTSVirtualChannelManagerCheckFileDescriptorEx (peer_ctx->vcm, FALSE);
  g_mutex_unlock (&peer_ctx->vcm_drain_mutex);
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

  /* The VCM must be pumped unconditionally: its event handle stays signalled
   * until CheckFileDescriptor() drains it, so skipping the call leaves the fd
   * permanently readable and spins the main loop. It is also what demultiplexes
   * static channel data (CLIPRDR), which arrives long before drdynvc matters.
   *
   * What *is* conditional is auto-opening drdynvc. FreeRDP 2 gated that on
   * client->activated internally; FreeRDP 3 keys it only off drdynvc_state, so
   * an unconditional open would push DVC capability PDUs onto a virtual channel
   * while the client still awaits the license PDU on the global channel
   * ("unexpected message for channel 1006, expected 1003"). It also asserts if
   * drdynvc has not been joined. Re-apply both gates via the autoOpen argument.
   */
  if (peer_ctx && peer_ctx->vcm)
    {
      /* NOTE: this deliberately diverges from Weston, which guards the pump
       * with WTSVirtualChannelManagerIsChannelJoined(vcm, "drdynvc") and then
       * calls the plain CheckFileDescriptor(). That guard does not work on
       * FreeRDP 3: MCS channel join completes *before* licensing, so the guard
       * is already TRUE while the client is still waiting for the license PDU,
       * and the unconditional auto-open pushes DVC caps onto a static virtual
       * channel. Gate on activation instead. Weston needs the same fix. */
      BOOL auto_open = client->activated &&
                       WTSVirtualChannelManagerIsChannelJoined (peer_ctx->vcm,
                                                                DRDYNVC_SVC_CHANNEL_NAME);

      BOOL vcm_ok;

      g_mutex_lock (&peer_ctx->vcm_drain_mutex);
      vcm_ok = WTSVirtualChannelManagerCheckFileDescriptorEx (peer_ctx->vcm,
                                                             auto_open);
      g_mutex_unlock (&peer_ctx->vcm_drain_mutex);

      if (!vcm_ok)
        {
          g_message ("rdp: WTS VC CheckFileDescriptor failed for peer %p",
                     client);
          goto out_clean;
        }
    }

   if (peer_ctx->clipboard)
     {
       /* A clipboard protocol error is not worth dropping the whole session
        * over: tear down just the CLIPRDR bridge and keep the peer alive. */
       if (!meta_rdp_clipboard_check_event_handle (peer_ctx->clipboard))
         {
           g_warning ("rdp: disabling clipboard bridge for peer %p", client);
           meta_rdp_peer_clear_clipboard (peer_ctx);
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

  g_mutex_init (&peer_ctx->gfxredir_mutex);
  g_mutex_init (&peer_ctx->disp_mutex);
  g_mutex_init (&peer_ctx->vcm_drain_mutex);
  g_mutex_init (&peer_ctx->rdpei_mutex);
  peer_ctx->rdpei_contacts = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  peer_ctx->rdpei_pending_gestures = g_queue_new ();

  peer_ctx->shm_fd = -1;
  /* -1, not 0: 0 is a valid buffer index, and meta_rdp_acquire_buffer() checks
   * this field whenever a readback is pending. */
  peer_ctx->readback_buffer = -1;

  return TRUE;
}

static void
rdp_peer_context_free (freerdp_peer *client, rdpContext *context)
{
  MetaRdpPeerContext *peer_ctx = (MetaRdpPeerContext *) context;

  if (!peer_ctx)
    return;

  meta_rdp_peer_remove_fd_sources (peer_ctx);

  g_clear_object (&peer_ctx->virtual_pointer);
  g_clear_object (&peer_ctx->virtual_keyboard);

  /* remove_fd_sources() above already dropped the clipboard's source; clear the
   * alias so the helper does not touch a destroyed GSource. */
  peer_ctx->clipboard_fd_source = NULL;
  meta_rdp_peer_clear_clipboard (peer_ctx);

  g_clear_pointer (&peer_ctx->audio_out, meta_rdp_audio_out_free);
  g_clear_pointer (&peer_ctx->audio_in, meta_rdp_audio_in_free);

  /* destroy_buffer() cancels any outstanding readback; this additionally drops
   * the PBO itself, which outlives individual pools. */
  meta_rdp_destroy_buffer (peer_ctx);
  meta_rdp_readback_free (peer_ctx);

  if (peer_ctx->gfxredir)
    {
      /* Close() joins the channel's reader thread, so no callback can be
       * running -- or start running -- once this returns. */
      peer_ctx->gfxredir->Close (peer_ctx->gfxredir);
      gfxredir_server_context_free (peer_ctx->gfxredir);
      peer_ctx->gfxredir = NULL;
    }

  /* Only now that no callback can queue another one is it safe to drop a
   * pending dispatch; otherwise it could fire against a freed peer. */
  g_mutex_lock (&peer_ctx->gfxredir_mutex);
  g_clear_handle_id (&peer_ctx->gfxredir_idle_id, g_source_remove);
  g_mutex_unlock (&peer_ctx->gfxredir_mutex);
  g_mutex_clear (&peer_ctx->gfxredir_mutex);
  g_clear_handle_id (&peer_ctx->gfxredir_caps_timeout_id, g_source_remove);
  g_clear_pointer (&peer_ctx->error_reason, g_free);
  g_clear_pointer (&peer_ctx->error_hint, g_free);

  if (peer_ctx->disp)
    {
      /* Same ordering as gfxredir: Close() first so no layout callback can be
       * in flight, only then drop the pending dispatch. */
      peer_ctx->disp->Close (peer_ctx->disp);
      disp_server_context_free (peer_ctx->disp);
      peer_ctx->disp = NULL;
    }

  g_mutex_lock (&peer_ctx->disp_mutex);
  g_clear_handle_id (&peer_ctx->disp_idle_id, g_source_remove);
  g_mutex_unlock (&peer_ctx->disp_mutex);
  g_mutex_clear (&peer_ctx->disp_mutex);
  g_mutex_clear (&peer_ctx->vcm_drain_mutex);

  if (peer_ctx->rdpei)
    {
      /* Same ordering as gfxredir/disp: Close() first so onTouchEvent cannot
       * be running -- or start running -- once this returns. */
      peer_ctx->rdpei->Close (peer_ctx->rdpei);
      rdpei_server_context_free (peer_ctx->rdpei);
      peer_ctx->rdpei = NULL;
    }

  /* Don't leave Shell in the middle of a swipe: flush whatever is still
   * queued (possibly the END) right now, and cancel a gesture that is
   * still going. We're on the main loop, so dispatch directly. */
  g_mutex_lock (&peer_ctx->rdpei_mutex);
  g_clear_handle_id (&peer_ctx->rdpei_idle_id, g_source_remove);
  g_clear_handle_id (&peer_ctx->rdpei_timeout_id, g_source_remove);
  if (peer_ctx->rdpei_gesture_active)
    {
      meta_rdp_rdpei_end_gesture_locked (peer_ctx,
                                         CLUTTER_TOUCHPAD_GESTURE_PHASE_CANCEL);
      g_clear_handle_id (&peer_ctx->rdpei_idle_id, g_source_remove);
    }
  g_mutex_unlock (&peer_ctx->rdpei_mutex);
  if (!g_queue_is_empty (peer_ctx->rdpei_pending_gestures))
    meta_rdp_rdpei_dispatch (peer_ctx);
  g_mutex_clear (&peer_ctx->rdpei_mutex);
  g_clear_pointer (&peer_ctx->rdpei_contacts, g_hash_table_unref);
  g_queue_free_full (peer_ctx->rdpei_pending_gestures,
                     (GDestroyNotify) meta_rdp_pending_gesture_free);

  g_clear_pointer (&peer_ctx->missed_damage, mtk_region_unref);

  g_clear_handle_id (&peer_ctx->drdynvc_wait_id, g_source_remove);

  if (peer_ctx->drdynvc)
    {
      peer_ctx->drdynvc->Stop (peer_ctx->drdynvc);
      drdynvc_server_context_free (peer_ctx->drdynvc);
      peer_ctx->drdynvc = NULL;
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
   * disabled to match wslg_desktop.rdp (authentication level:i:0).
   *
   * FreeRDP 3 removed settings->{Certificate,PrivateKey}File; the cert and key
   * are now first-class objects handed to the settings as pointers. Each peer
   * gets its own, parsed from the PEM, since the settings take ownership. */
  if (self->cert_pem && self->key_pem)
    {
      rdpPrivateKey *key = freerdp_key_new_from_pem (self->key_pem);
      rdpCertificate *cert = freerdp_certificate_new_from_pem (self->cert_pem);

      if (!key || !cert)
        {
          g_warning ("rdp: failed to load the session TLS cert/key");
          freerdp_key_free (key);
          freerdp_certificate_free (cert);
          goto error;
        }

      /* Unlike most FreeRDP_* pointer keys, RdpServerRsaKey and
       * RdpServerCertificate take ownership of the pointer rather than cloning
       * it -- freeing them here would leave the settings dangling and, among
       * other things, make the RSA-2048 probe that gates standard RDP security
       * read freed memory. */
      if (!freerdp_settings_set_pointer_len (settings, FreeRDP_RdpServerRsaKey,
                                             key, 1))
        {
          g_warning ("rdp: failed to apply TLS key to peer settings");
          freerdp_key_free (key);
          freerdp_certificate_free (cert);
          goto error;
        }

      if (!freerdp_settings_set_pointer_len (settings,
                                             FreeRDP_RdpServerCertificate,
                                             cert, 1))
        {
          g_warning ("rdp: failed to apply TLS cert to peer settings");
          freerdp_certificate_free (cert);
          goto error;
        }

      (void) freerdp_settings_set_bool (settings, FreeRDP_TlsSecurity, TRUE);
    }
  else
    {
      (void) freerdp_settings_set_bool (settings, FreeRDP_TlsSecurity, FALSE);
    }
  /* Weston enables only TLS: it never turns FreeRDP_RdpSecurity on. Standard
   * RDP security puts a security header with flags on every PDU, which changes
   * what the client expects during LICENSING -- leave it at the default. */
  (void) freerdp_settings_set_bool (settings, FreeRDP_NlaSecurity, FALSE);

  if (!client->Initialize (client))
    {
      g_warning ("rdp: peer Initialize failed");
      goto error;
    }

  (void) freerdp_settings_set_uint32 (settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
  (void) freerdp_settings_set_uint32 (settings, FreeRDP_OsMinorType,
                               OSMINORTYPE_PSEUDO_XSERVER);
  (void) freerdp_settings_set_uint32 (settings, FreeRDP_ColorDepth, 32);
  (void) freerdp_settings_set_bool (settings, FreeRDP_RefreshRect, TRUE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_RemoteFxCodec, FALSE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_NSCodec, FALSE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
  /* v1: plain fullscreen desktop, not RAIL. */
  (void) freerdp_settings_set_bool (settings, FreeRDP_RemoteApplicationMode, FALSE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_SupportGraphicsPipeline, TRUE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_HasExtendedMouseEvent, TRUE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_HasHorizontalWheel, TRUE);
  /* Enable CLIPRDR so the client negotiates the clipboard channel. */
  (void) freerdp_settings_set_bool (settings, FreeRDP_RedirectClipboard, TRUE);
  /* Implicit in FreeRDP 2, must be requested explicitly in 3. */
  (void) freerdp_settings_set_bool (settings, FreeRDP_FastPathInput, TRUE);

  /* FreeRDP 3 added two connect-time steps to the server state machine that
   * 2.x did not have, and both default to on:
   *
   *   - NetworkAutoDetect inserts CONNECT_TIME_AUTO_DETECT_REQUEST/RESPONSE
   *     before LICENSING. We register no autodetect callbacks, so the server
   *     would sit waiting for a response it never services.
   *   - SupportMultitransport (with the UDPFECR flag) emits a SEC_TRANSPORT_REQ
   *     after LICENSING to bootstrap a UDP side channel. WSLg runs over a
   *     TCP-only vsock, so there is no UDP path to bootstrap.
   *
   * Turning both off collapses the 3.x state machine back to the 2.x topology.
   */
  (void) freerdp_settings_set_bool (settings, FreeRDP_NetworkAutoDetect, FALSE);
  (void) freerdp_settings_set_bool (settings, FreeRDP_SupportMultitransport, FALSE);
  (void) freerdp_settings_set_uint32 (settings, FreeRDP_MultitransportFlags, 0);

  client->Capabilities = xf_peer_capabilities;
  client->PostConnect = xf_peer_post_connect;
  client->Activate = xf_peer_activate;
  client->AdjustMonitorsLayout = xf_peer_adjust_monitor_layout;

  /* route RDP keyboard/mouse into mutter's virtual input devices. */
  input = client->context->input;
  input->SynchronizeEvent = meta_rdp_synchronize_event;
  input->MouseEvent = meta_rdp_mouse_event;
  input->ExtendedMouseEvent = meta_rdp_extended_mouse_event;
  input->KeyboardEvent = meta_rdp_keyboard_event;
  input->UnicodeKeyboardEvent = meta_rdp_unicode_keyboard_event;

  handle_count = client->GetEventHandles (client, handles,
                                          META_RDP_MAX_FREERDP_FDS);
  if (!handle_count)
    {
      g_warning ("rdp: unable to retrieve peer event handles");
      goto error;
    }

  {
    /* FreeRDP 3 hands back a const table. */
    const WtsApiFunctionTable *fn = FreeRDP_InitWtsApi ();

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

  {
    HANDLE vcm_handle = peer_ctx->vcm ?
      WTSVirtualChannelManagerGetEventHandle (peer_ctx->vcm) : NULL;

    /* handles[] is sized META_RDP_MAX_FREERDP_FDS + 1 so that the VCM handle
     * always fits; clamp against fd_sources[], not the GetEventHandles limit,
     * or a full peer handle table silently drops the VCM watch. */
    for (i = 0; i < handle_count &&
                peer_ctx->n_fd_sources < META_RDP_MAX_PEER_FD_SOURCES; i++)
      {
        int fd = GetEventFileDescriptor (handles[i]);
        g_autofree char *label = NULL;

        if (fd < 0)
          continue;

        if (vcm_handle && handles[i] == vcm_handle)
          label = g_strdup ("vcm");
        else
          label = g_strdup_printf ("peer[%d]", i);

        peer_ctx->fd_sources[peer_ctx->n_fd_sources++] =
          meta_rdp_add_fd_source (fd, rdp_client_activity, client,
                                  g_steal_pointer (&label));
      }
  }

  self->peers = g_list_prepend (self->peers, peer_ctx);

  g_message ("rdp: peer %p initialized (%d fds)",
             client, peer_ctx->n_fd_sources);
  return 0;

error:
  /* Weston's rdp_peer_init closes the peer on the error path before unwinding;
   * without this the socket is left open until the listener drops the peer. */
  meta_rdp_peer_remove_fd_sources (peer_ctx);
  if (peer_ctx->vcm)
    {
      WTSCloseServer (peer_ctx->vcm);
      peer_ctx->vcm = NULL;
    }
  client->Close (client);
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
        meta_rdp_add_fd_source (fd, rdp_listener_activity, instance,
                                "listener");
    }

  return TRUE;
}

/* The session's TLS identity: a throwaway self-signed RSA-2048 certificate,
 * made in memory at startup and never written anywhere.
 *
 * Nothing verifies it -- the client runs with /cert:ignore, and the transport is
 * a Hyper-V socket inside the machine -- but FreeRDP's server will not
 * initialize a peer without one (freerdp_peer_initialize: "Missing server
 * certificate"), whichever security protocol ends up negotiated. This used to
 * shell out to winpr-makecert, which is not on PATH everywhere (NixOS) and
 * left the private key in a temp directory for the session's lifetime. */
static char *
meta_rdp_bio_to_string (BIO *bio)
{
  char *data = NULL;
  long length = BIO_get_mem_data (bio, &data);

  return length > 0 ? g_strndup (data, length) : NULL;
}

static gboolean
meta_rdp_generate_session_tls (MetaRdpServer  *self,
                               GError        **error)
{
  EVP_PKEY *pkey = NULL;
  X509 *x509 = NULL;
  X509_NAME *name;
  BIO *cert_bio = NULL;
  BIO *key_bio = NULL;
  gboolean ok = FALSE;

  pkey = EVP_RSA_gen (2048);
  x509 = X509_new ();
  cert_bio = BIO_new (BIO_s_mem ());
  key_bio = BIO_new (BIO_s_mem ());
  if (!pkey || !x509 || !cert_bio || !key_bio)
    goto out;

  if (!X509_set_version (x509, X509_VERSION_3) ||
      !ASN1_INTEGER_set (X509_get_serialNumber (x509), g_random_int_range (1, G_MAXINT32)) ||
      !X509_gmtime_adj (X509_getm_notBefore (x509), 0) ||
      !X509_gmtime_adj (X509_getm_notAfter (x509), 365L * 24 * 60 * 60) ||
      !X509_set_pubkey (x509, pkey))
    goto out;

  name = X509_get_subject_name (x509);
  if (!X509_NAME_add_entry_by_txt (name, "CN", MBSTRING_ASC,
                                   (const unsigned char *) "mutter-rdp",
                                   -1, -1, 0) ||
      !X509_set_issuer_name (x509, name) ||
      !X509_sign (x509, pkey, EVP_sha256 ()))
    goto out;

  if (!PEM_write_bio_X509 (cert_bio, x509) ||
      !PEM_write_bio_PrivateKey (key_bio, pkey, NULL, NULL, 0, NULL, NULL))
    goto out;

  self->cert_pem = meta_rdp_bio_to_string (cert_bio);
  self->key_pem = meta_rdp_bio_to_string (key_bio);
  ok = self->cert_pem && self->key_pem;

out:
  if (!ok)
    {
      unsigned long err = ERR_get_error ();

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "generating the session TLS certificate failed: %s",
                   err ? ERR_reason_error_string (err) : "unknown error");
    }
  else
    {
      g_message ("rdp: generated the session TLS certificate");
    }

  BIO_free (key_bio);
  BIO_free (cert_bio);
  X509_free (x509);
  EVP_PKEY_free (pkey);
  return ok;
}

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

static gboolean
meta_rdp_parse_env_int (const char  *name,
                        const char  *value,
                        int          min,
                        int          max,
                        int         *out,
                        GError     **error)
{
  gint64 parsed;

  if (!g_ascii_string_to_signed (value, 10, min, max, &parsed, error))
    {
      g_prefix_error (error, "%s=%s: ", name, value);
      return FALSE;
    }

  *out = (int) parsed;
  return TRUE;
}

/*
 * Determine the listening fd for the RDP server, in priority order:
 *   1. MUTTER_RDP_VSOCK_PORT set -> bind our own AF_VSOCK on that port. This is
 *      the WSLGd A1 hand-off: WSLGd publishes the reserved port and mutter (this
 *      process, launched externally in the user distro) binds it.
 *   2. USE_VSOCK set to a non-empty value -> an already-listening fd inherited
 *      from WSLGd; use it directly.
 *   3. USE_VSOCK set but empty -> create our own vsock on vsock_port.
 *   4. none of the above -> *out_fd = -1; the caller only falls back to TCP
 *      when explicitly asked to (MUTTER_RDP_DEBUG_TCP=1).
 *
 * If a vsock was requested but can't be set up, this fails instead of falling
 * back: the TCP listener has no authentication.
 */
static gboolean
meta_rdp_get_listen_fd (MetaRdpServer  *self,
                        int             vsock_port,
                        int            *out_fd,
                        GError        **error)
{
  const char *vsock_port_str = g_getenv ("MUTTER_RDP_VSOCK_PORT");
  const char *fd_str;
  int fd;

  *out_fd = -1;

  if (vsock_port_str && *vsock_port_str != '\0')
    {
      int port;

      if (!meta_rdp_parse_env_int ("MUTTER_RDP_VSOCK_PORT", vsock_port_str,
                                   1, G_MAXINT, &port, error))
        return FALSE;

      fd = meta_rdp_create_vsock_fd (port);
      if (fd < 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "unable to listen on vsock port %d", port);
          return FALSE;
        }

      self->owned_listen_fd = fd;
      g_message ("rdp: created vsock fd %d on WSLGd-published port %d",
                 fd, port);
      *out_fd = fd;
      return TRUE;
    }

  fd_str = g_getenv ("USE_VSOCK");

  if (!fd_str)
    return TRUE;

  if (*fd_str != '\0')
    {
      if (!meta_rdp_parse_env_int ("USE_VSOCK", fd_str,
                                   3, G_MAXINT, &fd, error))
        return FALSE;

      g_message ("rdp: using inherited vsock fd %d from WSLGd", fd);
      *out_fd = fd;
      return TRUE;
    }

  fd = meta_rdp_create_vsock_fd (vsock_port);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "unable to listen on vsock port %d", vsock_port);
      return FALSE;
    }

  self->owned_listen_fd = fd;
  g_message ("rdp: created vsock fd %d on port %d", fd, vsock_port);
  *out_fd = fd;
  return TRUE;
}

static gboolean
meta_rdp_server_start_listener (MetaRdpServer  *self,
                                GError        **error)
{
  int listen_fd;
  int vsock_port = 0;
  int tcp_port = META_RDP_DEFAULT_TCP_PORT;
  const char *port_env;

  if (!meta_rdp_get_listen_fd (self, vsock_port, &listen_fd, error))
    return FALSE;

  if (listen_fd < 0 && g_strcmp0 (g_getenv ("MUTTER_RDP_DEBUG_TCP"), "1") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "no vsock configured (MUTTER_RDP_VSOCK_PORT / USE_VSOCK); "
                   "set MUTTER_RDP_DEBUG_TCP=1 for an unauthenticated "
                   "TCP listener on 127.0.0.1");
      return FALSE;
    }

  port_env = g_getenv ("MUTTER_RDP_PORT");
  if (port_env &&
      !meta_rdp_parse_env_int ("MUTTER_RDP_PORT", port_env,
                               1, 65535, &tcp_port, error))
    return FALSE;

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

  if (listen_fd >= 0)
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
      if (!self->listener->Open (self->listener, "127.0.0.1", tcp_port))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "unable to bind RDP TCP listener on port %d", tcp_port);
          return FALSE;
        }
      g_warning ("rdp: listening on unauthenticated TCP 127.0.0.1:%d "
                 "(MUTTER_RDP_DEBUG_TCP)", tcp_port);
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
  MetaCursorTracker *cursor_tracker =
    meta_backend_get_cursor_tracker (self->backend);
  g_autoptr (GError) error = NULL;

  g_message ("rdp: context started, wiring up virtual output");

  self->monitors_changed_handler_id =
    g_signal_connect_object (monitor_manager, "monitors-changed",
                             G_CALLBACK (on_monitors_changed), self,
                             G_CONNECT_DEFAULT);

  /* Push the cursor shape to connected clients whenever it changes. Position
   * is deliberately not tracked: the client draws the pointer under its own
   * mouse, so motion needs no server round trip. */
  self->cursor_changed_handler_id =
    g_signal_connect_object (cursor_tracker, "cursor-changed",
                             G_CALLBACK (on_cursor_changed), self,
                             G_CONNECT_DEFAULT);
  self->cursor_visibility_handler_id =
    g_signal_connect_object (cursor_tracker, "visibility-changed",
                             G_CALLBACK (on_cursor_visibility_changed), self,
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

static gboolean
meta_rdp_is_mount_point (const char *path)
{
  g_autofree char *parent = g_path_get_dirname (path);
  struct stat st, parent_st;

  if (stat (path, &st) != 0 || !S_ISDIR (st.st_mode) ||
      stat (parent, &parent_st) != 0)
    return FALSE;

  return st.st_dev != parent_st.st_dev;
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
             "yes"
            );

  self = g_object_new (META_TYPE_RDP_SERVER, NULL);
  self->backend = backend;

  /* Checked once, here, but only acted on per peer (meta_rdp_setup_gfxredir()):
   * without the share there is nothing to show but the error frame, and that
   * needs a connected client to show it to. */
  self->shared_memory_mount_path =
    g_strdup (g_getenv ("WSL2_SHARED_MEMORY_MOUNT_POINT"));
  if (!self->shared_memory_mount_path)
    {
      self->shared_memory_error =
        g_strdup ("WSL2_SHARED_MEMORY_MOUNT_POINT is not set.");
    }
  else if (!meta_rdp_is_mount_point (self->shared_memory_mount_path))
    {
      /* Without the share, gfxredir would create its pools in a plain
       * directory the client cannot see. */
      self->shared_memory_error =
        g_strdup_printf ("The WSLg shared-memory share is not mounted at %s.",
                         self->shared_memory_mount_path);
      g_clear_pointer (&self->shared_memory_mount_path, g_free);
    }

  if (self->shared_memory_error)
    g_warning ("rdp: %s Clients will get the error frame.",
               self->shared_memory_error);
  else
    g_message ("rdp: shared-memory mount: %s", self->shared_memory_mount_path);

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

  g_clear_pointer (&self->cert_pem, g_free);
  if (self->key_pem)
    {
      explicit_bzero (self->key_pem, strlen (self->key_pem));
      g_clear_pointer (&self->key_pem, g_free);
    }
  g_clear_pointer (&self->shared_memory_mount_path, g_free);
  g_clear_pointer (&self->shared_memory_error, g_free);

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
