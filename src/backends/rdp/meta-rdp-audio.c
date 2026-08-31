/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * PipeWire-over-RDP forwarding (see meta-rdp-audio.h).
 *
 * The RDP half (rdpsnd for playback, audin for capture) is ported from
 * wslg/weston/libweston/backend-rdp/rdpaudio.c and rdpaudioin.c, retargeted
 * onto GLib. The Linux half is not: Weston, and this file until recently,
 * talked to a pair of custom PulseAudio modules over a bespoke framed
 * protocol on a unix socket, with mutter as the *server*. There is no
 * PulseAudio any more. We are now a plain TCP *client* of two stock
 * libpipewire-module-protocol-simple servers, and the framing is gone with
 * it -- what crosses the socket is raw interleaved PCM and nothing else.
 *
 * The PipeWire side is configured by weaselway's
 * pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf, which must agree with
 * rdp_audio_out_format / rdp_audio_in_format below on rate, channels and
 * sample format -- protocol-simple fixes those at module load time and has no
 * way to negotiate or even announce them.
 *
 * Two consequences of the protocol having no framing, both deliberate:
 *
 *   - The old RDP_AUDIO_CMD_GET_LATENCY / RESET_LATENCY round trip is gone,
 *     and with it the rendered-latency estimate that used to be fed back to
 *     the sink. Nothing on the wire can carry it now.
 *   - We are a client, so we retry: PipeWire may not be up when a peer
 *     connects, and the server drops our stream whenever it restarts.
 */

#include "config.h"

#include "backends/rdp/meta-rdp-audio.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <freerdp/channels/channels.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/audin.h>
#include <winpr/stream.h>

/* Where the protocol-simple servers listen. Must match server.address in the
 * PipeWire drop-in. Overridable mainly so the bridge can be pointed at a
 * PipeWire running somewhere else while debugging. */
#define RDP_AUDIO_SINK_ADDR_ENV "MUTTER_RDP_AUDIO_SINK_ADDR"
#define RDP_AUDIO_SINK_ADDR_DEFAULT "127.0.0.1:4711"

#define RDP_AUDIO_SOURCE_ADDR_ENV "MUTTER_RDP_AUDIO_SOURCE_ADDR"
#define RDP_AUDIO_SOURCE_ADDR_DEFAULT "127.0.0.1:4712"

/* How long to wait between connection attempts, and how often to tick while
 * connected so a dropped capture connection is noticed. */
#define RDP_AUDIO_RECONNECT_INTERVAL_MS 1000
#define RDP_AUDIO_POLL_INTERVAL_MS 500

/* ------------------------------------------------------------------ */
/* Shared: interruptible waiting and connecting                        */
/* ------------------------------------------------------------------ */

/* Wait up to timeout_ms. Returns FALSE the moment exit_fd is signalled, so
 * every sleep in this file doubles as a teardown check -- the alternative is
 * a thread that cannot be joined until its timeout happens to expire. */
static gboolean
rdp_audio_wait_or_exit (int exit_fd,
                        int timeout_ms)
{
  struct pollfd pfd = { .fd = exit_fd, .events = POLLIN };
  int ret = poll (&pfd, 1, timeout_ms);

  if (ret < 0)
    {
      if (errno == EINTR)
        return TRUE;
      g_warning ("rdp: audio: poll failed: %s", g_strerror (errno));
      return FALSE;
    }

  /* Timed out: nothing signalled, carry on. */
  return ret == 0;
}

/* Split "host:port" on the last colon, so bare IPv6 literals are at least not
 * silently mangled into something that resolves. */
static gboolean
rdp_audio_split_addr (const char  *spec,
                      char       **host,
                      char       **service)
{
  const char *colon = strrchr (spec, ':');

  if (!colon || colon == spec || !colon[1])
    {
      g_warning ("rdp: audio: malformed address '%s', want host:port", spec);
      return FALSE;
    }

  *host = g_strndup (spec, colon - spec);
  *service = g_strdup (colon + 1);

  return TRUE;
}

static int
rdp_audio_connect_once (const char *host,
                        const char *service)
{
  struct addrinfo hints = { 0 };
  struct addrinfo *result = NULL;
  struct addrinfo *ai;
  int fd = -1;
  int err;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  err = getaddrinfo (host, service, &hints, &result);
  if (err != 0)
    {
      g_warning ("rdp: audio: cannot resolve %s:%s: %s",
                host, service, gai_strerror (err));
      return -1;
    }

  for (ai = result; ai; ai = ai->ai_next)
    {
      fd = socket (ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                   ai->ai_protocol);
      if (fd < 0)
        continue;

      if (connect (fd, ai->ai_addr, ai->ai_addrlen) == 0)
        break;

      close (fd);
      fd = -1;
    }

  freeaddrinfo (result);

  if (fd >= 0)
    {
      /* Both directions move ~5ms packets. Nagle would coalesce those into
       * bursts and add latency for no benefit on a loopback connection. */
      int one = 1;

      if (setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof (one)) < 0)
        g_warning ("rdp: audio: TCP_NODELAY failed: %s", g_strerror (errno));
    }

  return fd;
}

/* Connect, retrying until it works or teardown is signalled. Returns -1 only
 * when giving up for good, so callers can treat that as "exit the thread".
 *
 * Retrying rather than failing is the point: PipeWire is a separate service
 * that may start after us, and it drops every client stream when it restarts.
 */
static int
rdp_audio_connect (const char     *env_var,
                   const char     *default_addr,
                   int             exit_fd,
                   const gboolean *exit_signal,
                   const char     *what)
{
  const char *spec = g_getenv (env_var);
  g_autofree char *host = NULL;
  g_autofree char *service = NULL;
  gboolean warned = FALSE;

  if (!spec || !spec[0])
    spec = default_addr;

  if (!rdp_audio_split_addr (spec, &host, &service))
    return -1;

  while (!*exit_signal)
    {
      int fd = rdp_audio_connect_once (host, service);

      if (fd >= 0)
        {
          if (warned)
            g_message ("rdp: audio: %s connected to %s", what, spec);
          return fd;
        }

      /* Once, not once per second: a PipeWire that is not running yet is an
       * ordinary startup race, and the retry is the handling. */
      if (!warned)
        {
          g_message ("rdp: audio: %s cannot reach %s (%s), retrying",
                    what, spec, g_strerror (errno));
          warned = TRUE;
        }

      if (!rdp_audio_wait_or_exit (exit_fd, RDP_AUDIO_RECONNECT_INTERVAL_MS))
        break;
    }

  return -1;
}

/* ------------------------------------------------------------------ */
/* Playback: PipeWire protocol-simple -> rdpsnd DVC                    */
/* ------------------------------------------------------------------ */

/* Milliseconds of audio per RDP packet. This is the dominant cost knob for
 * the whole bridge, because of where the bytes actually get written:
 * SendSamples() does not touch the socket, it formats a DVC PDU and posts it
 * to the shared vcm->queue (FreeRDP's wts_queue_send_item()). That queue is
 * drained by WTSVirtualChannelManagerCheckFileDescriptorEx(), which mutter
 * calls from rdp_client_activity() -- a GSource dispatch on the *main thread*
 * -- and the queue's event handle is itself one of the main loop's fd
 * sources. So every packet we submit wakes the main loop and is written to
 * the socket by the same thread that renders.
 *
 * Weston used 5ms, i.e. ~200 packets and ~200 main-loop wakeups per second,
 * which is enough to show up as UI stutter. 20ms cuts that to ~50/s for
 * 15ms of extra audio latency, which is well inside what is normal for RDP.
 * Tunable because the right value depends on the client and the machine. */
#define AUDIO_LATENCY_MS_DEFAULT 20
#define AUDIO_LATENCY_MS_ENV "MUTTER_RDP_AUDIO_LATENCY_MS"
#define AUDIO_LATENCY_MS_MIN 5
#define AUDIO_LATENCY_MS_MAX 200

/* Set to 1 to keep forwarding silence instead of dropping it (see
 * rdp_audio_out_forward_packet), for when a silence-related glitch needs
 * ruling in or out. */
#define AUDIO_SEND_SILENCE_ENV "MUTTER_RDP_AUDIO_SEND_SILENCE"

/* Number of RDP audio blocks that may be in flight at once, matching the
 * wraparound range of RdpsndServerContext::block_no. */
#define MAX_BLOCKS_IN_FLIGHT 256

typedef struct
{
  /* Submitted to rdpsnd and not yet given back. Confirms for a block that was
   * never submitted are the client's problem, not ours, and must not hand out
   * a slot we do not own. */
  gboolean in_flight;
  /* Whether this block's in-flight slot has been handed back already, so a
   * duplicate or late confirm can't release it twice. */
  gboolean slot_released;
} MetaRdpAudioBlockInfo;

struct _MetaRdpAudioOut
{
  RdpsndServerContext *rdpsnd;

  gboolean exit_signal;
  GThread *thread;

  /* Our client connection to the protocol-simple playback server. */
  int sink_fd;

  int bytes_per_frame;
  /* Frames per RDP packet, derived from the negotiated latency. Matches
   * rdpsnd's own out_frames so each SendSamples() emits exactly one PDU with
   * nothing left accumulated -- which is what makes dropping silent packets
   * safe, as there is never a partial buffer for the drop to strand. */
  int frames_per_packet;
  gboolean send_silence;
  guint8 *buffer;
  size_t buffer_size;

  MetaRdpAudioBlockInfo block_info[MAX_BLOCKS_IN_FLIGHT];

  /* Semaphore bounding in-flight RDP audio blocks, ported from Weston's
   * audioSem. Non-blocking: waiters poll it together with exit_fd so that
   * teardown can interrupt a wait that would otherwise never be satisfied
   * (Weston relied on pthread_cancel() for this). */
  int block_sem;
  int exit_fd;

  MetaRdpAudioFlushFunc flush;
  gpointer flush_data;
};

static AUDIO_FORMAT rdp_audio_out_format = { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0, NULL };

/* Give one in-flight slot back. Safe to call from FreeRDP's channel thread. */
static gboolean
rdp_audio_block_sem_release (MetaRdpAudioOut *audio_out)
{
  uint64_t one = 1;

  if (write (audio_out->block_sem, &one, sizeof (one)) != sizeof (one))
    {
      g_warning ("rdp: audio: block_sem write failed: %s", g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

/* Take one in-flight slot, waiting if none are free. Returns FALSE if we were
 * woken by teardown instead, in which case no slot was taken. */
static gboolean
rdp_audio_block_sem_acquire (MetaRdpAudioOut *audio_out)
{
  while (TRUE)
    {
      uint64_t dummy;
      struct pollfd fds[2];
      int ret;

      if (read (audio_out->block_sem, &dummy, sizeof (dummy)) == sizeof (dummy))
        return TRUE;

      if (errno != EAGAIN && errno != EINTR)
        {
          g_warning ("rdp: audio: block_sem read failed: %s",
                    g_strerror (errno));
          return FALSE;
        }

      fds[0] = (struct pollfd) { .fd = audio_out->block_sem, .events = POLLIN };
      fds[1] = (struct pollfd) { .fd = audio_out->exit_fd, .events = POLLIN };

      ret = poll (fds, G_N_ELEMENTS (fds), 1000);
      if (ret < 0)
        {
          if (errno == EINTR)
            continue;
          g_warning ("rdp: audio: block_sem poll failed: %s",
                    g_strerror (errno));
          return FALSE;
        }

      if (ret == 0)
        {
          /* We are not draining the sink socket while parked here, so
           * PipeWire will start dropping what it cannot hand us. */
          g_warning ("rdp: audio: stalled waiting for the client to confirm "
                    "audio blocks");
          continue;
        }

      if (fds[1].revents)
        return FALSE;
    }
}

/* Blocks left unconfirmed when a connection drops would otherwise burn their
 * slots forever, so restore the full credit for each new connection. */
static void
rdp_audio_block_sem_reset (MetaRdpAudioOut *audio_out)
{
  uint64_t dummy;
  int i;

  while (read (audio_out->block_sem, &dummy, sizeof (dummy)) == sizeof (dummy))
    ;

  for (i = 0; i < MAX_BLOCKS_IN_FLIGHT; i++)
    {
      if (!rdp_audio_block_sem_release (audio_out))
        break;
    }

  memset (audio_out->block_info, 0, sizeof (audio_out->block_info));
}

static UINT
on_rdpsnd_confirm_block (RdpsndServerContext *context,
                         BYTE                 confirm_block_num,
                         UINT16                wtimestamp)
{
  MetaRdpAudioOut *audio_out = context->data;
  MetaRdpAudioBlockInfo *info = &audio_out->block_info[confirm_block_num];

  if (!info->in_flight)
    {
      g_warning ("rdp: audio: spurious confirm for block %u",
                confirm_block_num);
      return 0;
    }

  /* Release on the *first* confirm. Clients differ on how many they send --
   * mstsc sends two per block once a latency is advertised (received, then
   * rendered), others send one -- and waiting for a second that never comes
   * drains the semaphore after 256 blocks and wedges playback for good.
   * slot_released is what keeps the second confirm from double-releasing. */
  if (!info->slot_released)
    {
      info->slot_released = TRUE;

      if (!rdp_audio_block_sem_release (audio_out))
        return ERROR_INTERNAL_ERROR;
    }

  return 0;
}

/* Read exactly one RDP packet's worth of PCM off the socket and hand it to
 * rdpsnd. Reading a fixed whole number of frames keeps us frame-aligned for
 * free, which matters because the stream itself carries no framing at all.
 *
 * Returns FALSE when the connection is gone or teardown was signalled; the
 * caller reconnects or exits. */
static gboolean
rdp_audio_out_forward_packet (MetaRdpAudioOut *audio_out)
{
  size_t chunk = audio_out->buffer_size;
  size_t got = 0;
  BYTE block_no;

  while (got < chunk)
    {
      ssize_t n = read (audio_out->sink_fd, audio_out->buffer + got,
                        chunk - got);

      if (n > 0)
        {
          got += n;
          continue;
        }

      if (n == 0)
        {
          g_message ("rdp: audio: playback server closed the connection");
          return FALSE;
        }

      if (errno == EINTR)
        continue;

      /* EBADF/ECONNRESET here is the expected shape of teardown, which
       * shutdown()s this socket to break exactly this read. */
      if (!audio_out->exit_signal)
        g_warning ("rdp: audio: read from playback server failed: %s",
                  g_strerror (errno));
      return FALSE;
    }

  /* The sink monitor produces samples whenever the PipeWire graph is running,
   * so with nothing playing this is a steady stream of zeroes -- measured at
   * ~200 packets/s at 5ms, every one of which would wake the main loop for no
   * audible benefit. Drop them. The old PulseAudio path got this for free by
   * suspending the sink when idle, so clients already cope with the stream
   * simply stopping.
   *
   * memcmp against the buffer offset by one is the usual all-bytes-equal
   * trick; combined with the first byte being zero it means all-zero. */
  if (!audio_out->send_silence && got > 1 &&
      audio_out->buffer[0] == 0 &&
      memcmp (audio_out->buffer, audio_out->buffer + 1, got - 1) == 0)
    return TRUE;

  /* Bound in-flight blocks: SendSamples may accumulate rather than send every
   * call, so this blocks until a previous block has been confirmed. */
  if (!rdp_audio_block_sem_acquire (audio_out))
    return FALSE;

  block_no = audio_out->rdpsnd->block_no;
  audio_out->block_info[block_no].in_flight = TRUE;
  audio_out->block_info[block_no].slot_released = FALSE;

  /* Timestamp 0 disables A/V sync at the client, as it did in Weston. We have
   * no meaningful timestamp to give it now that the sink no longer sends one
   * and there is no latency feedback channel to correct with. */
  if (audio_out->rdpsnd->SendSamples (audio_out->rdpsnd, audio_out->buffer,
                                      audio_out->frames_per_packet, 0) != 0)
    {
      g_warning ("rdp: audio: SendSamples failed");
      return FALSE;
    }

  if (block_no == audio_out->rdpsnd->block_no)
    {
      /* Nothing was actually sent this time; give the semaphore slot back. */
      audio_out->block_info[block_no].in_flight = FALSE;
      audio_out->block_info[block_no].slot_released = TRUE;

      if (!rdp_audio_block_sem_release (audio_out))
        return FALSE;

      /* Nothing was queued, so nothing to push out. */
      return TRUE;
    }

  /* SendSamples only queues; without this the bytes sit there until the main
   * loop next runs, which is exactly the coupling to rendering we are trying
   * to avoid. Do it here, on this thread. */
  if (audio_out->flush)
    audio_out->flush (audio_out->flush_data);

  return TRUE;
}

static gpointer
rdp_audio_out_thread (gpointer data)
{
  MetaRdpAudioOut *audio_out = data;

  while (!audio_out->exit_signal)
    {
      int fd = rdp_audio_connect (RDP_AUDIO_SINK_ADDR_ENV,
                                  RDP_AUDIO_SINK_ADDR_DEFAULT,
                                  audio_out->exit_fd,
                                  &audio_out->exit_signal,
                                  "playback");

      if (fd < 0)
        break;

      audio_out->sink_fd = fd;

      /* Credit does not carry across connections: whatever was outstanding
       * when the last one dropped is never going to be confirmed. */
      rdp_audio_block_sem_reset (audio_out);

      while (!audio_out->exit_signal)
        {
          if (!rdp_audio_out_forward_packet (audio_out))
            break;
        }

      audio_out->sink_fd = -1;
      close (fd);
    }

  return NULL;
}

static void
on_rdpsnd_activated (RdpsndServerContext *context)
{
  MetaRdpAudioOut *audio_out = context->data;
  const char *latency_env = g_getenv (AUDIO_LATENCY_MS_ENV);
  int latency_ms;
  int format = -1;
  UINT32 i;

  /* Clients may activate rdpsnd more than once (reconnect, format change);
   * the thread is set up only on the first one. */
  if (audio_out->thread)
    return;

  for (i = 0; i < context->num_client_formats; i++)
    {
      if (context->client_formats[i].wFormatTag == rdp_audio_out_format.wFormatTag &&
          context->client_formats[i].nChannels == rdp_audio_out_format.nChannels &&
          context->client_formats[i].nSamplesPerSec == rdp_audio_out_format.nSamplesPerSec &&
          context->client_formats[i].wBitsPerSample == rdp_audio_out_format.wBitsPerSample)
        {
          format = i;
          break;
        }
    }

  if (format == -1)
    {
      g_warning ("rdp: audio: client and server agreed on no playback format");
      return;
    }

  audio_out->bytes_per_frame =
    (context->client_formats[format].wBitsPerSample / 8) *
    context->client_formats[format].nChannels;

  latency_ms = AUDIO_LATENCY_MS_DEFAULT;
  if (latency_env && latency_env[0])
    {
      gint64 parsed;

      if (g_ascii_string_to_signed (latency_env, 10, AUDIO_LATENCY_MS_MIN,
                                    AUDIO_LATENCY_MS_MAX, &parsed, NULL))
        latency_ms = (int) parsed;
      else
        g_warning ("rdp: audio: ignoring %s='%s', want %d-%d",
                  AUDIO_LATENCY_MS_ENV, latency_env,
                  AUDIO_LATENCY_MS_MIN, AUDIO_LATENCY_MS_MAX);
    }

  /* Tell rdpsnd the same figure we packetise at, so its out_frames matches
   * frames_per_packet and it emits one PDU per SendSamples() call. */
  context->latency = latency_ms;
  audio_out->frames_per_packet =
    context->client_formats[format].nSamplesPerSec * latency_ms / 1000;

  audio_out->send_silence =
    g_strcmp0 (g_getenv (AUDIO_SEND_SILENCE_ENV), "1") == 0;

  context->SelectFormat (context, format);
  context->SetVolume (context, 0x7FFF, 0x7FFF);

  /* One RDP packet's worth, which is all rdp_audio_out_forward_packet() ever
   * reads at a time. Sized here because bytes_per_frame is only known now. */
  audio_out->buffer_size =
    audio_out->frames_per_packet * (size_t) audio_out->bytes_per_frame;
  audio_out->buffer = g_malloc0 (audio_out->buffer_size);

  g_message ("rdp: audio: playback %dms/packet (%d frames, %.0f packets/s)%s",
            latency_ms, audio_out->frames_per_packet,
            1000.0 / latency_ms,
            audio_out->send_silence ? ", forwarding silence" : "");

  audio_out->thread = g_thread_new ("rdp-audio-out", rdp_audio_out_thread,
                                    audio_out);
}

MetaRdpAudioOut *
meta_rdp_audio_out_new (HANDLE                 vcm,
                        MetaRdpAudioFlushFunc  flush,
                        gpointer               flush_data)
{
  MetaRdpAudioOut *audio_out;
  AUDIO_FORMAT *server_formats;

  if (!vcm || vcm == INVALID_HANDLE_VALUE)
    return NULL;

  audio_out = g_new0 (MetaRdpAudioOut, 1);
  audio_out->sink_fd = -1;
  audio_out->flush = flush;
  audio_out->flush_data = flush_data;

  audio_out->rdpsnd = rdpsnd_server_context_new (vcm);
  if (!audio_out->rdpsnd)
    {
      g_warning ("rdp: audio: rdpsnd_server_context_new failed");
      g_free (audio_out);
      return NULL;
    }

  audio_out->block_sem = eventfd (MAX_BLOCKS_IN_FLIGHT,
                                  EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC);
  audio_out->exit_fd = eventfd (0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (audio_out->block_sem < 0 || audio_out->exit_fd < 0)
    {
      g_warning ("rdp: audio: eventfd failed: %s", g_strerror (errno));
      if (audio_out->block_sem >= 0)
        close (audio_out->block_sem);
      if (audio_out->exit_fd >= 0)
        close (audio_out->exit_fd);
      rdpsnd_server_context_free (audio_out->rdpsnd);
      g_free (audio_out);
      return NULL;
    }

  /* Freed by FreeRDP in rdpsnd_server_context_free(). */
  server_formats = g_new0 (AUDIO_FORMAT, 1);
  *server_formats = rdp_audio_out_format;

  audio_out->rdpsnd->data = audio_out;
  audio_out->rdpsnd->Activated = on_rdpsnd_activated;
  audio_out->rdpsnd->ConfirmBlock = on_rdpsnd_confirm_block;
  audio_out->rdpsnd->num_server_formats = 1;
  audio_out->rdpsnd->server_formats = server_formats;
  audio_out->rdpsnd->src_format = &rdp_audio_out_format;
  audio_out->rdpsnd->use_dynamic_virtual_channel =
    !g_strcmp0 (g_getenv ("MUTTER_RDP_DISABLE_AUDIO_DVC"), "1") ? FALSE : TRUE;

  /* Initialize() also Starts the channel. */
  if (audio_out->rdpsnd->Initialize (audio_out->rdpsnd, TRUE) != 0)
    {
      g_warning ("rdp: audio: rdpsnd Initialize failed");
      close (audio_out->block_sem);
      close (audio_out->exit_fd);
      rdpsnd_server_context_free (audio_out->rdpsnd);
      g_free (audio_out);
      return NULL;
    }

  g_message ("rdp: audio: playback channel ready");

  return audio_out;
}

void
meta_rdp_audio_out_free (MetaRdpAudioOut *audio_out)
{
  if (!audio_out)
    return;

  if (audio_out->thread)
    {
      uint64_t one = 1;
      int sink_fd = audio_out->sink_fd;

      audio_out->exit_signal = TRUE;

      /* Wakes a thread parked in rdp_audio_block_sem_acquire() or between
       * connection attempts; the shutdown below only covers a blocking read. */
      if (write (audio_out->exit_fd, &one, sizeof (one)) != sizeof (one))
        g_warning ("rdp: audio: exit_fd write failed: %s", g_strerror (errno));

      if (sink_fd >= 0)
        shutdown (sink_fd, SHUT_RDWR);

      g_thread_join (audio_out->thread);
    }

  if (audio_out->sink_fd >= 0)
    close (audio_out->sink_fd);

  g_free (audio_out->buffer);

  if (audio_out->rdpsnd)
    {
      audio_out->rdpsnd->Close (audio_out->rdpsnd);
      audio_out->rdpsnd->Stop (audio_out->rdpsnd);
      rdpsnd_server_context_free (audio_out->rdpsnd);
    }

  if (audio_out->block_sem >= 0)
    close (audio_out->block_sem);
  if (audio_out->exit_fd >= 0)
    close (audio_out->exit_fd);

  g_free (audio_out);
}

/* ------------------------------------------------------------------ */
/* Capture: audin DVC -> PipeWire protocol-simple                      */
/* ------------------------------------------------------------------ */

/* A local thread keeps a connection to the capture server up, and
 * on_audin_data() -- which runs on FreeRDP's own audin channel thread,
 * spawned by audin->Open() -- writes the client's microphone samples to it.
 * Hence the mutex around source_fd.
 *
 * Note the connection is held open for as long as the peer is around, not
 * just while samples are flowing: protocol-simple creates its stream per
 * connected client, so the connection *is* what makes the microphone exist in
 * PipeWire's graph. Dropping it between utterances would make the device
 * flicker in and out of every application's device list. */
struct _MetaRdpAudioIn
{
  audin_server_context *audin;

  gboolean exit_signal;
  GThread *thread;
  int exit_fd;

  GMutex source_fd_mutex;
  int source_fd;
};

static AUDIO_FORMAT rdp_audio_in_format = { WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0, NULL };

static UINT
on_audin_receive_version (audin_server_context *context,
                          const SNDIN_VERSION   *version)
{
  return CHANNEL_RC_OK;
}

/* Client announced its supported capture formats; ask it to open the one we
 * support (we only ever advertise the one format, so there's nothing to pick
 * between beyond confirming the client actually offered it). */
static UINT
on_audin_receive_formats (audin_server_context *context,
                          const SNDIN_FORMATS   *formats)
{
  SNDIN_OPEN open = { 0 };
  UINT32 i;

  for (i = 0; i < formats->NumFormats; i++)
    {
      const AUDIO_FORMAT *format = &formats->SoundFormats[i];

      if (format->wFormatTag == rdp_audio_in_format.wFormatTag &&
          format->nChannels == rdp_audio_in_format.nChannels &&
          format->nSamplesPerSec == rdp_audio_in_format.nSamplesPerSec &&
          format->wBitsPerSample == rdp_audio_in_format.wBitsPerSample)
        break;
    }

  if (i == formats->NumFormats)
    {
      g_warning ("rdp: audio: client offered no matching capture format");
      return CHANNEL_RC_OK;
    }

  open.FramesPerPacket = rdp_audio_in_format.nSamplesPerSec / 100;
  open.initialFormat = i;
  open.captureFormat = rdp_audio_in_format;

  if (context->SendOpen (context, &open) != CHANNEL_RC_OK)
    g_warning ("rdp: audio: SendOpen failed");

  return CHANNEL_RC_OK;
}

static UINT
on_audin_open_reply (audin_server_context   *context,
                     const SNDIN_OPEN_REPLY *open_reply)
{
  if (open_reply->Result != 0)
    g_warning ("rdp: audio: client rejected capture Open (0x%x)",
              open_reply->Result);

  return CHANNEL_RC_OK;
}

static UINT
on_audin_incoming_data (audin_server_context      *context,
                        const SNDIN_DATA_INCOMING *data_incoming)
{
  return CHANNEL_RC_OK;
}

static UINT
on_audin_data (audin_server_context *context,
              const SNDIN_DATA      *data)
{
  MetaRdpAudioIn *audio_in = context->userdata;
  /* Data is positioned past the PDU header FreeRDP already consumed, so the
   * payload is the remainder -- Stream_Buffer()/Stream_Length() would include
   * that header (or read back as zero on an unsealed stream). */
  const void *samples = Stream_ConstPointer (data->Data);
  size_t bytes = Stream_GetRemainingLength (data->Data);

  g_mutex_lock (&audio_in->source_fd_mutex);
  if (audio_in->source_fd >= 0 && bytes > 0)
    {
      ssize_t sent = send (audio_in->source_fd, samples, bytes, MSG_NOSIGNAL);

      if (sent != (ssize_t) bytes)
        {
          /* Drop it and let the capture thread reconnect; PipeWire restarting
           * underneath us is the common reason to land here. */
          g_warning ("rdp: audio: capture send failed: %s", g_strerror (errno));
          close (audio_in->source_fd);
          audio_in->source_fd = -1;
        }
    }
  g_mutex_unlock (&audio_in->source_fd_mutex);

  return CHANNEL_RC_OK;
}

static UINT
on_audin_receive_format_change (audin_server_context      *context,
                                const SNDIN_FORMATCHANGE  *format_change)
{
  return CHANNEL_RC_OK;
}

static gpointer
rdp_audio_in_thread (gpointer data)
{
  MetaRdpAudioIn *audio_in = data;

  while (!audio_in->exit_signal)
    {
      gboolean connected;

      g_mutex_lock (&audio_in->source_fd_mutex);
      connected = audio_in->source_fd >= 0;
      g_mutex_unlock (&audio_in->source_fd_mutex);

      if (!connected)
        {
          int fd = rdp_audio_connect (RDP_AUDIO_SOURCE_ADDR_ENV,
                                      RDP_AUDIO_SOURCE_ADDR_DEFAULT,
                                      audio_in->exit_fd,
                                      &audio_in->exit_signal,
                                      "capture");

          if (fd < 0)
            break;

          g_mutex_lock (&audio_in->source_fd_mutex);
          audio_in->source_fd = fd;
          g_mutex_unlock (&audio_in->source_fd_mutex);
        }

      /* Tick rather than poll the socket: on_audin_data() may close and clear
       * it from the channel thread at any point, and a fd being polled here
       * could by then have been closed and its number reused. */
      if (!rdp_audio_wait_or_exit (audio_in->exit_fd, RDP_AUDIO_POLL_INTERVAL_MS))
        break;
    }

  return NULL;
}

MetaRdpAudioIn *
meta_rdp_audio_in_new (HANDLE vcm)
{
  MetaRdpAudioIn *audio_in;

  if (!vcm || vcm == INVALID_HANDLE_VALUE)
    return NULL;

  audio_in = g_new0 (MetaRdpAudioIn, 1);
  audio_in->source_fd = -1;
  g_mutex_init (&audio_in->source_fd_mutex);

  audio_in->exit_fd = eventfd (0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (audio_in->exit_fd < 0)
    {
      g_warning ("rdp: audio: eventfd failed: %s", g_strerror (errno));
      g_mutex_clear (&audio_in->source_fd_mutex);
      g_free (audio_in);
      return NULL;
    }

  audio_in->audin = audin_server_context_new (vcm);
  if (!audio_in->audin)
    {
      g_warning ("rdp: audio: audin_server_context_new failed");
      close (audio_in->exit_fd);
      g_mutex_clear (&audio_in->source_fd_mutex);
      g_free (audio_in);
      return NULL;
    }

  if (!audin_server_set_formats (audio_in->audin, 1, &rdp_audio_in_format))
    {
      g_warning ("rdp: audio: audin_server_set_formats failed");
      audin_server_context_free (audio_in->audin);
      close (audio_in->exit_fd);
      g_mutex_clear (&audio_in->source_fd_mutex);
      g_free (audio_in);
      return NULL;
    }

  audio_in->audin->userdata = audio_in;
  audio_in->audin->serverVersion = SNDIN_VERSION_Version_2;
  audio_in->audin->ReceiveVersion = on_audin_receive_version;
  audio_in->audin->ReceiveFormats = on_audin_receive_formats;
  audio_in->audin->OpenReply = on_audin_open_reply;
  audio_in->audin->IncomingData = on_audin_incoming_data;
  audio_in->audin->Data = on_audin_data;
  audio_in->audin->ReceiveFormatChange = on_audin_receive_format_change;

  if (!audio_in->audin->Open (audio_in->audin))
    {
      g_warning ("rdp: audio: audin Open failed");
      audin_server_context_free (audio_in->audin);
      close (audio_in->exit_fd);
      g_mutex_clear (&audio_in->source_fd_mutex);
      g_free (audio_in);
      return NULL;
    }

  audio_in->thread = g_thread_new ("rdp-audio-in", rdp_audio_in_thread, audio_in);

  g_message ("rdp: audio: capture channel ready");

  return audio_in;
}

void
meta_rdp_audio_in_free (MetaRdpAudioIn *audio_in)
{
  if (!audio_in)
    return;

  if (audio_in->thread)
    {
      uint64_t one = 1;

      audio_in->exit_signal = TRUE;

      if (write (audio_in->exit_fd, &one, sizeof (one)) != sizeof (one))
        g_warning ("rdp: audio: exit_fd write failed: %s", g_strerror (errno));

      g_thread_join (audio_in->thread);
    }

  /* After Close() no more Data callbacks can race us for source_fd. */
  if (audio_in->audin)
    {
      audio_in->audin->Close (audio_in->audin);
      audin_server_context_free (audio_in->audin);
    }

  g_mutex_lock (&audio_in->source_fd_mutex);
  if (audio_in->source_fd >= 0)
    close (audio_in->source_fd);
  g_mutex_unlock (&audio_in->source_fd_mutex);
  g_mutex_clear (&audio_in->source_fd_mutex);

  if (audio_in->exit_fd >= 0)
    close (audio_in->exit_fd);

  g_free (audio_in);
}
