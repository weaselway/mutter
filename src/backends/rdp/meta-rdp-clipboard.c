/*
 * Copyright (C) 2026 WSLg mutter RDP backend
 *
 * Task 05: CLIPRDR clipboard bridge (see meta-rdp-clipboard.h).
 *
 * Adapted from wslg/weston/libweston/backend-rdp/rdpclip.c and retargeted onto
 * mutter's MetaSelection API. Two directions:
 *
 *  - client -> mutter: the client announces its clipboard via ClientFormatList.
 *    We publish a MetaRdpSelectionSource as the clipboard owner. When a mutter
 *    app pastes, read_async() issues a ClientFormatDataRequest; the reply
 *    (ClientFormatDataResponse) is converted to the requested mime type and
 *    fed back through the GTask.
 *
 *  - mutter -> client: on MetaSelection::owner-changed for a source that isn't
 *    ours, we push a ServerFormatList to the client. When the client pastes it
 *    sends ServerFormatDataRequest; we read from MetaSelection and reply with
 *    ServerFormatDataResponse.
 *
 * Only UTF-8 text and HTML are bridged; images/files are out of scope for v1.
 */

#include "config.h"

#include "backends/rdp/meta-rdp-clipboard.h"

#include <gio/gio.h>
#include <string.h>

#include "backends/meta-backend-private.h"
#include "meta/display.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/meta-selection.h"
#include "meta/meta-selection-source.h"

#include <freerdp/server/cliprdr.h>
#include <freerdp/channels/cliprdr.h>
#include <winpr/user.h>

#define MIME_TEXT_UTF8 "text/plain;charset=utf-8"
#define MIME_TEXT_PLAIN "text/plain"
#define MIME_TEXT_HTML "text/html"

/* "HTML Format" is a registered (named) clipboard format, so its numeric id is
 * assigned by the client and never appears on the wire from our side -- we only
 * ever match it by name. This is purely a local discriminator; the Microsoft
 * FreeRDP fork spelled it CB_FORMAT_HTML, which upstream FreeRDP 3 does not
 * define (Weston carries the same constant as CF_PRIVATE_HTML). */
#define META_RDP_CB_FORMAT_HTML 0xD010

/* HTML clipboard format wrapper (CF_HTML), ported from Weston's rdpclip.c. */
static const char html_header_fmt[] =
  "Version:0.9\r\n"
  "StartHTML:%010u\r\n"
  "EndHTML:%010u\r\n"
  "StartFragment:%010u\r\n"
  "EndFragment:%010u\r\n";
static const char html_fragment_start[] = "<!--StartFragment-->\r\n";
static const char html_fragment_end[] = "<!--EndFragment-->\r\n";

/* ------------------------------------------------------------------ */

/* One clipboard "format" we know how to translate. */
typedef struct
{
  UINT32 format_id;         /* RDP CF_* / CB_FORMAT_* id */
  const char *format_name;  /* registered name for HTML, else NULL */
  const char *mime_type;    /* mutter mime type */
} MetaRdpClipboardFormat;

/* Order matters: preferred formats first (HTML before plain text). */
static const MetaRdpClipboardFormat supported_formats[] = {
  { META_RDP_CB_FORMAT_HTML,   "HTML Format",  MIME_TEXT_HTML },
  { CF_UNICODETEXT,   NULL,           MIME_TEXT_UTF8 },
  { CF_UNICODETEXT,   NULL,           MIME_TEXT_PLAIN },
};

#define G_META_TYPE_RDP_SELECTION_SOURCE (meta_rdp_selection_source_get_type ())
G_DECLARE_FINAL_TYPE (MetaRdpSelectionSource, meta_rdp_selection_source,
                      META, RDP_SELECTION_SOURCE, MetaSelectionSource)

struct _MetaRdpClipboard
{
  freerdp_peer *peer;
  MetaBackend *backend;
  MetaDisplay *display;
  MetaSelection *selection;

  CliprdrServerContext *cliprdr;

  gboolean caps_received;
  gboolean client_use_long_format_names;

  gulong owner_changed_id;

  /* Source we published from the client's clipboard (client -> mutter). */
  MetaRdpSelectionSource *client_source;

  /* Pending mutter-app paste waiting on a ClientFormatDataResponse. */
  GTask *pending_read; /* read from client_source */
  const MetaRdpClipboardFormat *pending_format;

  /* Set while we are applying a client-driven owner change, to ignore the
   * resulting owner-changed signal (avoid echoing it back to the client). */
  gboolean setting_owner;

  /* Pending client paste: a ServerFormatDataRequest we must answer by reading
   * from the current mutter selection owner. */
  UINT32 server_requested_format_id;
  gboolean server_request_pending;
  GOutputStream *server_request_output;
};

struct _MetaRdpSelectionSource
{
  MetaSelectionSource parent;
  MetaRdpClipboard *clipboard;
  GList *mimetypes; /* char* */
};

G_DEFINE_TYPE (MetaRdpSelectionSource, meta_rdp_selection_source,
               META_TYPE_SELECTION_SOURCE)

/* ------------------------------------------------------------------ */
/* Format helpers */

static const MetaRdpClipboardFormat *
format_by_mime (const char *mime)
{
  size_t i;

  for (i = 0; i < G_N_ELEMENTS (supported_formats); i++)
    {
      if (g_strcmp0 (supported_formats[i].mime_type, mime) == 0)
        return &supported_formats[i];
    }
  return NULL;
}

static const MetaRdpClipboardFormat *
format_by_id_and_name (UINT32 format_id, const char *name)
{
  size_t i;

  for (i = 0; i < G_N_ELEMENTS (supported_formats); i++)
    {
      const MetaRdpClipboardFormat *f = &supported_formats[i];

      if (f->format_name)
        {
          if (name && g_ascii_strcasecmp (f->format_name, name) == 0)
            return f;
        }
      else if (f->format_id == format_id)
        {
          return f;
        }
    }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Payload conversion: RDP wire bytes <-> mutter mime bytes */

/* CF_UNICODETEXT (UTF-16LE, CRLF, NUL-terminated) -> UTF-8 (LF). */
static GBytes *
unicode_to_utf8 (const BYTE *data,
                 UINT32      len)
{
  g_autofree gunichar2 *u16 = NULL;
  g_autofree char *utf8 = NULL;
  GString *out;
  glong n16;
  const char *p;

  n16 = (glong) (len / sizeof (gunichar2));
  u16 = g_new0 (gunichar2, n16 + 1);
  memcpy (u16, data, n16 * sizeof (gunichar2));

  utf8 = g_utf16_to_utf8 (u16, n16, NULL, NULL, NULL);
  if (!utf8)
    return NULL;

  /* Strip trailing NUL(s) and convert CRLF -> LF. */
  out = g_string_new (NULL);
  for (p = utf8; *p; p++)
    {
      if (p[0] == '\r' && p[1] == '\n')
        continue;
      g_string_append_c (out, *p);
    }

  return g_string_free_to_bytes (out);
}

/* UTF-8 (LF) -> CF_UNICODETEXT (UTF-16LE, CRLF, NUL-terminated). */
static GBytes *
utf8_to_unicode (const char *utf8,
                 gsize       len)
{
  GString *crlf;
  g_autofree gunichar2 *u16 = NULL;
  glong n16;
  gsize i;
  gsize bytes;
  BYTE *out;

  crlf = g_string_new (NULL);
  for (i = 0; i < len; i++)
    {
      if (utf8[i] == '\n' && (i == 0 || utf8[i - 1] != '\r'))
        g_string_append_c (crlf, '\r');
      g_string_append_c (crlf, utf8[i]);
    }

  u16 = g_utf8_to_utf16 (crlf->str, crlf->len, NULL, &n16, NULL);
  g_string_free (crlf, TRUE);
  if (!u16)
    return NULL;

  /* Include a UTF-16 NUL terminator. */
  bytes = (n16 + 1) * sizeof (gunichar2);
  out = g_malloc0 (bytes);
  memcpy (out, u16, n16 * sizeof (gunichar2));

  return g_bytes_new_take (out, bytes);
}

/* CF_HTML wrapper -> plain HTML fragment (text/html). */
static GBytes *
cfhtml_to_html (const BYTE *data,
                UINT32      len)
{
  g_autofree char *buf = g_strndup ((const char *) data, len);
  const char *start, *end;

  start = strstr (buf, "<!--StartFragment-->");
  end = strstr (buf, "<!--EndFragment-->");
  if (start && end && end > start)
    {
      start += strlen ("<!--StartFragment-->");
      while (*start == '\r' || *start == '\n')
        start++;
      return g_bytes_new (start, end - start);
    }

  /* No fragment markers: hand back everything after the header block. */
  start = strstr (buf, "\r\n\r\n");
  if (start)
    return g_bytes_new (start + 4, len - (start + 4 - buf));

  return g_bytes_new (buf, len);
}

/* Plain HTML fragment (text/html) -> CF_HTML wrapper. */
static GBytes *
html_to_cfhtml (const char *html,
                gsize       len)
{
  g_autofree char *header = NULL;
  GString *out;
  guint start_html, end_html, start_frag, end_frag;
  gsize header_len;

  /* Compute offsets; header length is fixed for the given format string. */
  header = g_strdup_printf (html_header_fmt, 0u, 0u, 0u, 0u);
  header_len = strlen (header);

  start_html = header_len;
  start_frag = start_html + strlen (html_fragment_start);
  end_frag = start_frag + len;
  end_html = end_frag + strlen (html_fragment_end);

  g_free (header);
  header = g_strdup_printf (html_header_fmt, start_html, end_html,
                            start_frag, end_frag);

  out = g_string_new (header);
  g_string_append (out, html_fragment_start);
  g_string_append_len (out, html, len);
  g_string_append (out, html_fragment_end);
  g_string_append_c (out, '\0');

  return g_string_free_to_bytes (out);
}

static GBytes *
rdp_bytes_to_mime (const MetaRdpClipboardFormat *format,
                   const BYTE                   *data,
                   UINT32                        len)
{
  if (format->format_id == META_RDP_CB_FORMAT_HTML)
    return cfhtml_to_html (data, len);
  else
    return unicode_to_utf8 (data, len);
}

static GBytes *
mime_bytes_to_rdp (const MetaRdpClipboardFormat *format,
                   GBytes                       *bytes)
{
  gsize len;
  const char *data = g_bytes_get_data (bytes, &len);

  if (format->format_id == META_RDP_CB_FORMAT_HTML)
    return html_to_cfhtml (data, len);
  else
    return utf8_to_unicode (data, len);
}

/* ------------------------------------------------------------------ */
/* MetaRdpSelectionSource: exposes the client's clipboard to mutter */

static GList *
meta_rdp_selection_source_get_mimetypes (MetaSelectionSource *source)
{
  MetaRdpSelectionSource *self = META_RDP_SELECTION_SOURCE (source);
  GList *l, *out = NULL;

  for (l = self->mimetypes; l; l = l->next)
    out = g_list_prepend (out, g_strdup (l->data));

  return g_list_reverse (out);
}

static void
meta_rdp_selection_source_read_async (MetaSelectionSource *source,
                                      const char          *mimetype,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  MetaRdpSelectionSource *self = META_RDP_SELECTION_SOURCE (source);
  MetaRdpClipboard *clipboard = self->clipboard;
  const MetaRdpClipboardFormat *format;
  CLIPRDR_FORMAT_DATA_REQUEST request = { 0 };
  GTask *task;

  task = g_task_new (source, cancellable, callback, user_data);
  g_task_set_source_tag (task, meta_rdp_selection_source_read_async);

  format = format_by_mime (mimetype);
  if (!format || !clipboard->cliprdr)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "Unsupported clipboard mime type '%s'", mimetype);
      g_object_unref (task);
      return;
    }

  if (clipboard->pending_read)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_BUSY,
                               "Clipboard read already in progress");
      g_object_unref (task);
      return;
    }

  clipboard->pending_read = task;
  clipboard->pending_format = format;

  request.requestedFormatId = format->format_id;
  request.common.msgType = CB_FORMAT_DATA_REQUEST;
  request.common.dataLen = 4;
  if (clipboard->cliprdr->ServerFormatDataRequest (clipboard->cliprdr,
                                                   &request) != 0)
    {
      clipboard->pending_read = NULL;
      clipboard->pending_format = NULL;
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ServerFormatDataRequest failed");
      g_object_unref (task);
    }
}

static GInputStream *
meta_rdp_selection_source_read_finish (MetaSelectionSource *source,
                                       GAsyncResult        *result,
                                       GError             **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
meta_rdp_selection_source_finalize (GObject *object)
{
  MetaRdpSelectionSource *self = META_RDP_SELECTION_SOURCE (object);

  g_list_free_full (self->mimetypes, g_free);

  G_OBJECT_CLASS (meta_rdp_selection_source_parent_class)->finalize (object);
}

static void
meta_rdp_selection_source_class_init (MetaRdpSelectionSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaSelectionSourceClass *source_class = META_SELECTION_SOURCE_CLASS (klass);

  object_class->finalize = meta_rdp_selection_source_finalize;

  source_class->get_mimetypes = meta_rdp_selection_source_get_mimetypes;
  source_class->read_async = meta_rdp_selection_source_read_async;
  source_class->read_finish = meta_rdp_selection_source_read_finish;
}

static void
meta_rdp_selection_source_init (MetaRdpSelectionSource *self)
{
}

/* ------------------------------------------------------------------ */
/* client -> mutter: publish the client's clipboard */

/* CLIPRDR ClientFormatList: the client announces available formats. */
static UINT
on_client_format_list (CliprdrServerContext         *context,
                       const CLIPRDR_FORMAT_LIST    *format_list)
{
  MetaRdpClipboard *clipboard = context->custom;
  CLIPRDR_FORMAT_LIST_RESPONSE response = { 0 };
  MetaRdpSelectionSource *source;
  GHashTable *seen;
  UINT32 i;

  source = g_object_new (G_META_TYPE_RDP_SELECTION_SOURCE, NULL);
  source->clipboard = clipboard;

  seen = g_hash_table_new (g_str_hash, g_str_equal);

  for (i = 0; i < format_list->numFormats; i++)
    {
      const CLIPRDR_FORMAT *format = &format_list->formats[i];
      const MetaRdpClipboardFormat *known;

      known = format_by_id_and_name (format->formatId, format->formatName);
      if (!known)
        continue;
      if (g_hash_table_contains (seen, known->mime_type))
        continue;

      g_hash_table_add (seen, (gpointer) known->mime_type);
      source->mimetypes = g_list_append (source->mimetypes,
                                         g_strdup (known->mime_type));
    }

  g_hash_table_destroy (seen);

  response.common.msgType = CB_FORMAT_LIST_RESPONSE;
  response.common.msgFlags = CB_RESPONSE_OK;
  response.common.dataLen = 0;
  context->ServerFormatListResponse (context, &response);

  if (!source->mimetypes)
    {
      g_object_unref (source);
      return CHANNEL_RC_OK;
    }

  /* Take ownership of mutter's clipboard on the client's behalf. Guard so the
   * resulting owner-changed signal isn't echoed back as a ServerFormatList. */
  clipboard->setting_owner = TRUE;
  meta_selection_set_owner (clipboard->selection, META_SELECTION_CLIPBOARD,
                            META_SELECTION_SOURCE (source));
  clipboard->setting_owner = FALSE;

  g_set_object (&clipboard->client_source, source);
  g_object_unref (source);

  return CHANNEL_RC_OK;
}

/* CLIPRDR ClientFormatDataResponse: the reply to a ServerFormatDataRequest we
 * issued from meta_rdp_selection_source_read_async(). */
static UINT
on_client_format_data_response (CliprdrServerContext                  *context,
                                const CLIPRDR_FORMAT_DATA_RESPONSE    *response)
{
  MetaRdpClipboard *clipboard = context->custom;
  g_autoptr (GTask) task = NULL;
  const MetaRdpClipboardFormat *format;
  GBytes *mime_bytes;
  GInputStream *stream;

  task = g_steal_pointer (&clipboard->pending_read);
  format = clipboard->pending_format;
  clipboard->pending_format = NULL;

  if (!task)
    return CHANNEL_RC_OK;

  if ((response->common.msgFlags & CB_RESPONSE_FAIL) || !format ||
      !response->requestedFormatData)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Client clipboard data request failed");
      return CHANNEL_RC_OK;
    }

  mime_bytes = rdp_bytes_to_mime (format, response->requestedFormatData,
                                  response->common.dataLen);
  if (!mime_bytes)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to convert clipboard data");
      return CHANNEL_RC_OK;
    }

  stream = g_memory_input_stream_new_from_bytes (mime_bytes);
  g_bytes_unref (mime_bytes);
  g_task_return_pointer (task, stream, g_object_unref);

  return CHANNEL_RC_OK;
}

/* ------------------------------------------------------------------ */
/* mutter -> client: advertise mutter's clipboard and serve paste requests */

static void
send_server_format_list (MetaRdpClipboard *clipboard)
{
  CLIPRDR_FORMAT_LIST format_list = { 0 };
  CLIPRDR_FORMAT formats[G_N_ELEMENTS (supported_formats)] = { 0 };
  g_autoptr (GList) mimetypes = NULL;
  GHashTable *seen;
  UINT32 n = 0;
  size_t i;

  if (!clipboard->cliprdr)
    return;

  mimetypes = meta_selection_get_mimetypes (clipboard->selection,
                                            META_SELECTION_CLIPBOARD);

  seen = g_hash_table_new (NULL, NULL);

  for (i = 0; i < G_N_ELEMENTS (supported_formats); i++)
    {
      const MetaRdpClipboardFormat *f = &supported_formats[i];

      if (!g_list_find_custom (mimetypes, f->mime_type, (GCompareFunc) g_strcmp0))
        continue;
      if (g_hash_table_contains (seen, GUINT_TO_POINTER (f->format_id)) &&
          !f->format_name)
        continue;

      g_hash_table_add (seen, GUINT_TO_POINTER (f->format_id));
      formats[n].formatId = f->format_id;
      formats[n].formatName = f->format_name ? (char *) f->format_name : NULL;
      n++;
    }

  g_hash_table_destroy (seen);

  format_list.common.msgType = CB_FORMAT_LIST;
  format_list.common.msgFlags = 0;
  format_list.numFormats = n;
  format_list.formats = formats;

  clipboard->cliprdr->ServerFormatList (clipboard->cliprdr, &format_list);
}

static void
on_selection_transfer_finished (GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  MetaRdpClipboard *clipboard = user_data;
  MetaSelection *selection = META_SELECTION (source_object);
  g_autoptr (GOutputStream) output = g_steal_pointer (&clipboard->server_request_output);
  CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
  const MetaRdpClipboardFormat *format;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) mime_bytes = NULL;
  g_autoptr (GBytes) rdp_bytes = NULL;

  clipboard->server_request_pending = FALSE;

  if (!meta_selection_transfer_finish (selection, result, &error))
    {
      g_warning ("rdp: clipboard transfer failed: %s", error->message);
      goto fail;
    }

  mime_bytes = g_memory_output_stream_steal_as_bytes (
    G_MEMORY_OUTPUT_STREAM (output));

  /* Find the format matching the id the client requested. */
  format = NULL;
  {
    size_t i;
    for (i = 0; i < G_N_ELEMENTS (supported_formats); i++)
      {
        if (supported_formats[i].format_id ==
            clipboard->server_requested_format_id)
          {
            format = &supported_formats[i];
            break;
          }
      }
  }
  if (!format)
    goto fail;

  rdp_bytes = mime_bytes_to_rdp (format, mime_bytes);
  if (!rdp_bytes)
    goto fail;

  response.common.msgType = CB_FORMAT_DATA_RESPONSE;
  response.common.msgFlags = CB_RESPONSE_OK;
  response.common.dataLen = g_bytes_get_size (rdp_bytes);
  response.requestedFormatData = g_bytes_get_data (rdp_bytes, NULL);
  clipboard->cliprdr->ServerFormatDataResponse (clipboard->cliprdr, &response);
  return;

fail:
  response.common.msgType = CB_FORMAT_DATA_RESPONSE;
  response.common.msgFlags = CB_RESPONSE_FAIL;
  response.common.dataLen = 0;
  response.requestedFormatData = NULL;
  clipboard->cliprdr->ServerFormatDataResponse (clipboard->cliprdr, &response);
}

/* CLIPRDR ClientFormatDataRequest: the client is pasting; read the current
 * mutter selection and reply. */
static UINT
on_client_format_data_request (CliprdrServerContext                 *context,
                               const CLIPRDR_FORMAT_DATA_REQUEST    *request)
{
  MetaRdpClipboard *clipboard = context->custom;
  const MetaRdpClipboardFormat *format = NULL;
  GOutputStream *output;
  size_t i;

  for (i = 0; i < G_N_ELEMENTS (supported_formats); i++)
    {
      if (supported_formats[i].format_id == request->requestedFormatId)
        {
          format = &supported_formats[i];
          break;
        }
    }

  if (!format || clipboard->server_request_pending)
    {
      CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
      response.common.msgType = CB_FORMAT_DATA_RESPONSE;
      response.common.msgFlags = CB_RESPONSE_FAIL;
      context->ServerFormatDataResponse (context, &response);
      return CHANNEL_RC_OK;
    }

  clipboard->server_requested_format_id = request->requestedFormatId;
  clipboard->server_request_pending = TRUE;

  output = g_memory_output_stream_new_resizable ();
  clipboard->server_request_output = g_object_ref (output);
  meta_selection_transfer_async (clipboard->selection, META_SELECTION_CLIPBOARD,
                                 format->mime_type, -1, output, NULL,
                                 on_selection_transfer_finished, clipboard);
  g_object_unref (output);

  return CHANNEL_RC_OK;
}

static void
on_selection_owner_changed (MetaSelection       *selection,
                            MetaSelectionType    selection_type,
                            MetaSelectionSource *new_owner,
                            gpointer             user_data)
{
  MetaRdpClipboard *clipboard = user_data;

  if (selection_type != META_SELECTION_CLIPBOARD)
    return;

  /* Ignore the change we just made on the client's behalf. */
  if (clipboard->setting_owner)
    return;

  /* A mutter app (or nothing) owns the clipboard now; the client's published
   * source is no longer current. */
  if (new_owner != META_SELECTION_SOURCE (clipboard->client_source))
    g_clear_object (&clipboard->client_source);

  send_server_format_list (clipboard);
}

/* ------------------------------------------------------------------ */
/* CLIPRDR housekeeping */

static UINT
on_client_capabilities (CliprdrServerContext              *context,
                        const CLIPRDR_CAPABILITIES        *capabilities)
{
  MetaRdpClipboard *clipboard = context->custom;
  UINT32 i;

  for (i = 0; i < capabilities->cCapabilitiesSets; i++)
    {
      CLIPRDR_CAPABILITY_SET *set = &capabilities->capabilitySets[i];

      if (set->capabilitySetType == CB_CAPSTYPE_GENERAL)
        {
          CLIPRDR_GENERAL_CAPABILITY_SET *general =
            (CLIPRDR_GENERAL_CAPABILITY_SET *) set;

          clipboard->client_use_long_format_names =
            !!(general->generalFlags & CB_USE_LONG_FORMAT_NAMES);
        }
    }

  clipboard->caps_received = TRUE;
  return CHANNEL_RC_OK;
}

static UINT
on_client_temp_directory (CliprdrServerContext             *context,
                          const CLIPRDR_TEMP_DIRECTORY     *temp_directory)
{
  return CHANNEL_RC_OK;
}

static UINT
on_client_format_list_response (CliprdrServerContext                    *context,
                                const CLIPRDR_FORMAT_LIST_RESPONSE      *response)
{
  return CHANNEL_RC_OK;
}

/* ------------------------------------------------------------------ */
/* Public API */

MetaRdpClipboard *
meta_rdp_clipboard_new (freerdp_peer *peer,
                        MetaBackend  *backend,
                        HANDLE        vcm)
{
  MetaRdpClipboard *clipboard;
  MetaContext *context;
  CliprdrServerContext *cliprdr;

  if (!vcm || vcm == INVALID_HANDLE_VALUE)
    return NULL;

  cliprdr = cliprdr_server_context_new (vcm);
  if (!cliprdr)
    {
      g_warning ("rdp: cliprdr_server_context_new failed");
      return NULL;
    }

  clipboard = g_new0 (MetaRdpClipboard, 1);
  clipboard->peer = peer;
  clipboard->backend = backend;

  context = meta_backend_get_context (backend);
  clipboard->display = meta_context_get_display (context);
  clipboard->selection = meta_display_get_selection (clipboard->display);

  clipboard->cliprdr = cliprdr;
  cliprdr->custom = clipboard;
  cliprdr->rdpcontext = peer->context;

  cliprdr->ClientCapabilities = on_client_capabilities;
  cliprdr->TempDirectory = on_client_temp_directory;
  cliprdr->ClientFormatList = on_client_format_list;
  cliprdr->ClientFormatListResponse = on_client_format_list_response;
  cliprdr->ClientFormatDataRequest = on_client_format_data_request;
  cliprdr->ClientFormatDataResponse = on_client_format_data_response;

  cliprdr->useLongFormatNames = FALSE;
  cliprdr->streamFileClipEnabled = FALSE;
  cliprdr->fileClipNoFilePaths = TRUE;
  cliprdr->canLockClipData = FALSE;

  /* Run non-threaded: Open() the channel and drive it from mutter's main loop
   * via CheckEventHandle (see meta_rdp_clipboard_check_event_handle). We must
   * NOT call Start(), which spawns a worker thread and would invoke our
   * callbacks off-thread -- the MetaSelection APIs are main-thread only. */
  if (cliprdr->Open (cliprdr) != 0)
    {
      g_warning ("rdp: cliprdr Open failed");
      cliprdr_server_context_free (cliprdr);
      g_free (clipboard);
      return NULL;
    }

  /* Replicate FreeRDP's autoInitializationSequence (ServerCapabilities +
   * MonitorReady) that its worker thread would otherwise send. */
  {
    CLIPRDR_GENERAL_CAPABILITY_SET general = { 0 };
    CLIPRDR_CAPABILITIES caps = { 0 };
    CLIPRDR_MONITOR_READY monitor_ready = { 0 };
    UINT32 general_flags = 0;

    if (cliprdr->fileClipNoFilePaths)
      general_flags |= CB_FILECLIP_NO_FILE_PATHS;

    general.capabilitySetType = CB_CAPSTYPE_GENERAL;
    general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    general.version = CB_CAPS_VERSION_2;
    general.generalFlags = general_flags;

    caps.common.msgType = CB_CLIP_CAPS;
    caps.cCapabilitiesSets = 1;
    caps.capabilitySets = (CLIPRDR_CAPABILITY_SET *) &general;
    cliprdr->ServerCapabilities (cliprdr, &caps);

    monitor_ready.common.msgType = CB_MONITOR_READY;
    cliprdr->MonitorReady (cliprdr, &monitor_ready);
  }

  clipboard->owner_changed_id =
    g_signal_connect (clipboard->selection, "owner-changed",
                      G_CALLBACK (on_selection_owner_changed), clipboard);

  g_message ("rdp: clipboard bridge ready");

  return clipboard;
}

void
meta_rdp_clipboard_free (MetaRdpClipboard *clipboard)
{
  if (!clipboard)
    return;

  if (clipboard->owner_changed_id)
    {
      g_clear_signal_handler (&clipboard->owner_changed_id,
                              clipboard->selection);
    }

  if (clipboard->pending_read)
    {
      g_task_return_new_error (clipboard->pending_read, G_IO_ERROR,
                               G_IO_ERROR_CANCELLED, "Clipboard peer gone");
      g_clear_object (&clipboard->pending_read);
    }

  g_clear_object (&clipboard->server_request_output);

  if (clipboard->client_source)
    {
      meta_selection_unset_owner (clipboard->selection, META_SELECTION_CLIPBOARD,
                                  META_SELECTION_SOURCE (clipboard->client_source));
      g_clear_object (&clipboard->client_source);
    }

  if (clipboard->cliprdr)
    {
      clipboard->cliprdr->Close (clipboard->cliprdr);
      cliprdr_server_context_free (clipboard->cliprdr);
      clipboard->cliprdr = NULL;
    }

  g_free (clipboard);
}

HANDLE
meta_rdp_clipboard_get_event_handle (MetaRdpClipboard *clipboard)
{
  if (!clipboard || !clipboard->cliprdr)
    return NULL;

  return clipboard->cliprdr->GetEventHandle (clipboard->cliprdr);
}

gboolean
meta_rdp_clipboard_check_event_handle (MetaRdpClipboard *clipboard)
{
  UINT rc;

  if (!clipboard || !clipboard->cliprdr)
    return TRUE;

  rc = clipboard->cliprdr->CheckEventHandle (clipboard->cliprdr);
  if (rc != CHANNEL_RC_OK)
    {
      g_warning ("rdp: cliprdr CheckEventHandle returned 0x%X", rc);
      return FALSE;
    }

  return TRUE;
}
