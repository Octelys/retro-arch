/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2024 - libretro team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with RetroArch. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * ws_server.c – Minimal WebSocket server for RetroArch using libwebsockets.
 *
 * The server binds exclusively to 127.0.0.1 on the caller-supplied port so
 * that it is only reachable from the local machine.  A dedicated background
 * thread owns the libwebsockets service loop, keeping it fully decoupled from
 * frame processing.
 *
 * Game-state messaging
 * --------------------
 * When a new client connects the server immediately sends it the current game
 * state JSON (see game_state.h).  When the active game changes the caller
 * invokes ws_server_notify_game_changed(); the server thread then broadcasts
 * the updated state to every connected client.
 *
 * Broadcast design
 * ----------------
 * ws_server_notify_game_changed() is safe to call from any thread.  It sets
 * a flag under the existing g_lock and wakes up the service thread via
 * lws_cancel_service().  The service thread checks the flag after each
 * lws_service() call and, if set, calls lws_callback_on_writable_all_protocol()
 * to schedule a LWS_CALLBACK_SERVER_WRITEABLE event for every connected
 * client.  The actual JSON write happens inside that callback, keeping all
 * libwebsockets I/O on the service thread.
 *
 * Platform notes:
 *   Linux  : link with -lwebsockets (or use pkg-config libwebsockets).
 *   macOS  : link with -lwebsockets (Homebrew: brew install libwebsockets).
 *   Windows: place libwebsockets headers/import-lib under
 *            deps/libwebsockets/{x64,arm64}/ and link with websockets.lib
 *            (see deps/libwebsockets/README.md).
 */

#include "ws_server.h"
#include "game_state.h"
#include "../configuration.h"
#include "../core.h"
#include "../core_info.h"
#include "../paths.h"
#include "../runloop.h"

#ifdef HAVE_CHEEVOS
#include "../cheevos/cheevos.h"
#include "../cheevos/cheevos_locals.h"
#include "../deps/rcheevos/include/rc_consoles.h"
#endif

#include <libwebsockets.h>
#include <rthreads/rthreads.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <formats/rjson.h>
#include <formats/rjson_helpers.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Some older libwebsockets versions (< 2.x / < 4.x) do not define all of the
 * option macros we use.  Provide safe no-op fall-backs so that ws_server.c
 * compiles against those versions as well. */
#ifndef LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT
#define LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT 0
#endif
#ifndef LWS_SERVER_OPTION_DISABLE_IPV6
#define LWS_SERVER_OPTION_DISABLE_IPV6 0
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* Maximum inbound frame payload the server will buffer per connection.
 * 4 KiB is sufficient for typical control messages; increase if larger
 * payloads are expected. */
#define WS_RX_BUFFER_BYTES 4096

/* Timeout (ms) passed to lws_service() each iteration.  A short value keeps
 * the thread responsive to stop requests without busy-spinning. */
#define WS_SERVICE_TIMEOUT_MS 10

/* Maximum JSON payload size (bytes) for a game-state message.
 * game_path alone can be up to 4096 chars; add room for all other fields
 * plus JSON syntax overhead. */
#define WS_MSG_MAX_BYTES 8192

/* Maximum JSON payload for the achievements message.  A game with ~400
 * achievements, each with a title (~60 chars) and badge URL (~80 chars),
 * needs roughly 400 * 250 = 100 KB.  Use 256 KB to be safe. */
#define WS_ACH_MSG_MAX_BYTES (256 * 1024)

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static struct lws_context *g_lws_ctx               = NULL;
static sthread_t          *g_thread                = NULL;
static slock_t            *g_lock                  = NULL;
static bool                g_running               = false;
static bool                g_game_broadcast_pending = false;
static bool                g_ach_broadcast_pending  = false;
static bool                g_user_broadcast_pending = false;
static bool                g_progress_broadcast_pending = false;

/* Snapshot of the most-recently-reported achievement progress.
 * Written under g_lock by ws_server_notify_achievement_progress(),
 * read (also under g_lock) by the service thread before it issues the
 * lws_callback_on_writable_all_protocol() call.                        */
#define WS_PROGRESS_STR_MAX 64
static uint32_t g_progress_id       = 0;
static char     g_progress_str[WS_PROGRESS_STR_MAX];

/* Written by the service thread before lws_callback_on_writable_all_protocol,
 * read inside the WRITEABLE callback — both on the same service thread.
 * Holds a WS_MSG_* bitmask indicating which messages the broadcast delivers. */
static int                 g_broadcast_kind         = 0;

/* Snapshot of progress data captured at broadcast time (service-thread only). */
static uint32_t g_broadcast_progress_id      = 0;
static char     g_broadcast_progress_str[WS_PROGRESS_STR_MAX];

/* -------------------------------------------------------------------------
 * Per-session data
 *
 * pending_messages: bitmask of messages yet to be sent to this client.
 *   WS_MSG_GAME_STATE   (bit 0) – game state JSON
 *   WS_MSG_ACHIEVEMENTS (bit 1) – achievements JSON
 *
 * Each WRITEABLE invocation sends exactly ONE message and clears its bit.
 * If more bits remain it calls lws_callback_on_writable() to schedule
 * the next write.  This ensures lws never has to retry a partial write,
 * which is what causes the infinite WRITEABLE loop.
 * ---------------------------------------------------------------------- */
#define WS_MSG_GAME_STATE   (1 << 0)
#define WS_MSG_ACHIEVEMENTS (1 << 1)
#define WS_MSG_USER         (1 << 2)
#define WS_MSG_PROGRESS     (1 << 3)

typedef struct {
   int      pending_messages;
   /* Per-session copy of progress data so each client gets the right snapshot
    * even if a newer broadcast arrives before this client's WRITEABLE fires. */
   uint32_t progress_id;
   char     progress_str[WS_PROGRESS_STR_MAX];
} ws_session_t;

/* -------------------------------------------------------------------------
 * Helper: write the current game state to a single client
 * ---------------------------------------------------------------------- */

/**
 * ws_write_game_state:
 * @wsi : the WebSocket connection to write to.
 *
 * Serialises the current game state as JSON and sends it to @wsi.
 * Must be called from within the libwebsockets service thread
 * (i.e. from a LWS_CALLBACK_SERVER_WRITEABLE handler).
 */
static void ws_write_game_state(struct lws *wsi)
{
   /* libwebsockets requires LWS_PRE bytes of padding before the payload. */
   unsigned char buf[LWS_PRE + WS_MSG_MAX_BYTES];
   size_t        len;

   len = game_state_to_json((char *)(buf + LWS_PRE), WS_MSG_MAX_BYTES);
   if (len == 0)
      return;

   lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
}

/**
 * ws_write_achievements:
 * @wsi : the WebSocket connection to write to.
 *
 * Serialises the achievements list as JSON and sends it to @wsi.
 * Must be called from within the libwebsockets service thread.
 */
static void ws_write_achievements(struct lws *wsi)
{
#ifdef HAVE_CHEEVOS
   unsigned char *buf;
   size_t         len;

   buf = (unsigned char *)malloc(LWS_PRE + WS_ACH_MSG_MAX_BYTES);

   if (!buf)
      return;

   const rcheevos_locals_t *locals = get_rcheevos_locals();
   len = game_state_achievements_to_json(
            locals ? locals->client : NULL,
            (char *)(buf + LWS_PRE),
            WS_ACH_MSG_MAX_BYTES);

   if (len > 0)
      lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);

   free(buf);
#else
   (void)wsi;
#endif
}

/**
 * ws_write_user:
 * @wsi : the WebSocket connection to write to.
 *
 * Serialises the logged-in RA user info as JSON and sends it to @wsi.
 * Must be called from within the libwebsockets service thread.
 */
static void ws_write_user(struct lws *wsi)
{
#ifdef HAVE_CHEEVOS
   unsigned char buf[LWS_PRE + WS_MSG_MAX_BYTES];
   size_t        len;

   len = game_state_user_to_json((char *)(buf + LWS_PRE), WS_MSG_MAX_BYTES);
   if (len == 0)
      return;

   lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
#else
   (void)wsi;
#endif
}

/**
 * ws_write_progress:
 * @wsi     : the WebSocket connection to write to.
 * @session : the per-session data that holds the progress snapshot.
 *
 * Sends a lightweight achievement_progress JSON message to @wsi.
 * Must be called from within the libwebsockets service thread.
 */
static void ws_write_progress(struct lws *wsi, ws_session_t *session)
{
   unsigned char buf[LWS_PRE + 256];
   int           n;

   n = snprintf((char *)(buf + LWS_PRE),
         sizeof(buf) - LWS_PRE,
         "{\"type\":\"achievement_progress\","
         "\"id\":%u,"
         "\"measured_progress\":\"%s\"}",
         (unsigned)session->progress_id,
         session->progress_str);

   if (n > 0)
      lws_write(wsi, buf + LWS_PRE, (size_t)n, LWS_WRITE_TEXT);
}

static int callback_retroarch(struct lws *wsi,
      enum lws_callback_reasons reason,
      void *user, void *in, size_t len)
{
   ws_session_t *session = (ws_session_t *)user;

   (void)in;
   (void)len;

   switch (reason)
   {
      case LWS_CALLBACK_ESTABLISHED:
         /* New client: queue user info + game state + achievements and request the first write. */
         if (session)
            session->pending_messages = WS_MSG_USER | WS_MSG_GAME_STATE | WS_MSG_ACHIEVEMENTS;
         fprintf(stderr, "[ws_server] CONNECTED\n");
         lws_callback_on_writable(wsi);
         break;

      case LWS_CALLBACK_SERVER_WRITEABLE:
         if (!session)
            break;

         /* If nothing is queued on this session yet, this WRITEABLE was
          * triggered by a broadcast — adopt the broadcast's message set
          * and clear g_broadcast_kind so subsequent unsolicited WRITEABLE
          * callbacks (fired while lws drains large payloads) don't
          * re-adopt it. */
         if (session->pending_messages == 0)
         {
            session->pending_messages = g_broadcast_kind;
            g_broadcast_kind          = 0;

            /* Copy the progress snapshot for this session. */
            session->progress_id      = g_broadcast_progress_id;
            strncpy(session->progress_str, g_broadcast_progress_str,
                  sizeof(session->progress_str));
         }

         /* Send exactly one message per WRITEABLE invocation. */
         if (session->pending_messages & WS_MSG_USER)
         {
            session->pending_messages &= ~WS_MSG_USER;
            fprintf(stderr, "[ws_server] WRITE USER\n");
            ws_write_user(wsi);
         }
         else if (session->pending_messages & WS_MSG_GAME_STATE)
         {
            session->pending_messages &= ~WS_MSG_GAME_STATE;
            fprintf(stderr, "[ws_server] WRITE GAME\n");
            ws_write_game_state(wsi);
         }
         else if (session->pending_messages & WS_MSG_ACHIEVEMENTS)
         {
            session->pending_messages &= ~WS_MSG_ACHIEVEMENTS;
            fprintf(stderr, "[ws_server] WRITE ACHIEVEMENTS\n");
            ws_write_achievements(wsi);
         }
         else if (session->pending_messages & WS_MSG_PROGRESS)
         {
            session->pending_messages &= ~WS_MSG_PROGRESS;
            fprintf(stderr, "[ws_server] WRITE ACHIEVEMENT PROGRESS\n");
            ws_write_progress(wsi, session);
         }

         /* If more messages remain, schedule the next write. */
         if (session->pending_messages != 0)
            lws_callback_on_writable(wsi);

         break;

      case LWS_CALLBACK_CLOSED:
         break;

      default:
         break;
   }

   return 0;
}

/* -------------------------------------------------------------------------
 * RAM stream ("retroarch-memory")
 *
 * The main thread copies the core's RAM regions into g_mem_ring after each
 * frame while a client is connected; the service thread drains the ring
 * into g_mem_history, from which every client is sent in order. A client
 * that falls more than WS_MEM_HISTORY records behind skips ahead, and a
 * frame that finds the ring full is dropped: both show as a gap in the
 * frame numbers. Records are a rom_info JSON text message, sent first and
 * again whenever it changes, or a binary frame:
 *
 *   0  "RAMF"
 *   4  u16 version (3)
 *   6  u16 number of regions
 *   8  u32 session, matching rom_info's
 *   12 u32 frame within the session
 *   16 u32 length of the RAM
 *   20 u16 ports the core read the RetroPad of, a bit each
 *   22 u16 x 16 RetroPad buttons per port, by RETRO_DEVICE_ID_JOYPAD_*:
 *      those the core saw held during the frame
 *   54 the RAM: each region of rom_info's "regions", in order, back to back
 *
 * all little-endian. rom_info's "console_id" is the RetroAchievements
 * console of the game, 0 until achievements have identified it. A region's
 * "start" is its RetroAchievements address
 * when the game's achievements memory map is known, as with NES
 * cartridge RAM at $6000; otherwise the regions are system RAM, then
 * save RAM, numbered on from 0.
 * ---------------------------------------------------------------------- */

#define WS_MEM_RING_BYTES   (2 * 1024 * 1024)
#define WS_MEM_FRAME_MAX    (512 * 1024)
#define WS_MEM_PORTS        16
#define WS_MEM_HEADER_BYTES (22 + 2 * WS_MEM_PORTS)
#define WS_MEM_HISTORY      64
#define WS_MEM_REGIONS_MAX  16

#define WS_MEM_REC_INFO     1
#define WS_MEM_REC_FRAME    2

typedef struct
{
   uint32_t kind;
   uint32_t len;
} ws_mem_rec_t;

typedef struct
{
   const char    *name;
   const char    *type;
   /* NULL: read through the achievements memory map. */
   const uint8_t *data;
   uint32_t       start;
   uint32_t       size;
} ws_mem_region_t;

typedef struct
{
   unsigned char *buf; /* LWS_PRE bytes, then the payload */
   size_t         cap;
   size_t         len;
   uint32_t       kind;
} ws_mem_slot_t;

typedef struct
{
   uint32_t next_seq;
   bool     established;
   bool     send_info;
} ws_mem_session_t;

static retro_spsc_t       g_mem_ring;
/* Set by the main thread once g_mem_ring exists. */
static retro_atomic_int_t g_mem_ring_ready;
static retro_atomic_int_t g_mem_clients;
static retro_atomic_int_t g_mem_info_dirty;
/* websocket_ram_stream_enable, as the main thread last saw it. */
static retro_atomic_int_t g_mem_enabled;
/* Frames the main thread found no room for; the service thread reports
 * them. */
static retro_atomic_int_t g_mem_dropped;
/* Set by the main thread when it queues a record and has woken the
 * service thread, cleared by that thread before it drains: lws_service()
 * sleeps until a socket event since libwebsockets 4, whatever its
 * timeout, so queued frames would wait for one. */
static retro_atomic_int_t g_mem_wake;
/* WS_MEM_STATE_*, as the main thread last set it. */
static retro_atomic_int_t g_mem_state;

#define WS_MEM_STATE_IDLE      0
#define WS_MEM_STATE_STREAMING 1
#define WS_MEM_STATE_HARDCORE  2
#define WS_MEM_STATE_NO_RAM    3

/* Main thread only. */
static bool               g_mem_enabled_seen = false;
static uint32_t           g_mem_session    = 1;
static uint32_t           g_mem_frame      = 0;
static bool               g_mem_streaming  = false;
static bool               g_mem_hardcore   = false;
static size_t             g_mem_ram_size   = 0;
static uint32_t           g_mem_console_id = 0;
static unsigned           g_mem_region_count = 0;
static ws_mem_region_t    g_mem_regions[WS_MEM_REGIONS_MAX];
static bool               g_mem_info_full  = false;
static uint16_t           g_mem_pad_read   = 0;
static uint16_t           g_mem_pad[WS_MEM_PORTS];

/* Service thread only. */
static ws_mem_slot_t      g_mem_history[WS_MEM_HISTORY];
static ws_mem_slot_t      g_mem_info;
static uint32_t           g_mem_seq        = 0;
static uint32_t           g_mem_logged_session = 0;
static unsigned           g_mem_sent       = 0;
static retro_time_t       g_mem_report_time = 0;

/* Service thread: how the stream went over the last few seconds, so a
 * client that receives nothing can be told why. */
#define WS_MEM_REPORT_USEC 5000000

static void ws_mem_report(void)
{
   int          clients;
   int          dropped;
   retro_time_t now = cpu_features_get_time_usec();

   if (now - g_mem_report_time < WS_MEM_REPORT_USEC)
      return;
   g_mem_report_time = now;

   clients = retro_atomic_load_relaxed_int(&g_mem_clients);
   dropped = retro_atomic_exchange_int(&g_mem_dropped, 0);
   if (!clients)
      return;

   if (g_mem_sent || dropped)
      fprintf(stderr, "[ws_server] RAM stream: %u frames sent, %d dropped "
            "in the last 5 s, %d client(s)\n", g_mem_sent, dropped, clients);
   else
   {
      static const char *const why[] = {
         "no content has run since the client connected",
         "the game is paused or in the menu",
         "achievements hardcore mode is on",
         "the core exposes no RAM"
      };
      int state = retro_atomic_load_relaxed_int(&g_mem_state);
      fprintf(stderr, "[ws_server] RAM stream: %d client(s) connected but "
            "no frames in the last 5 s: %s\n", clients,
            why[state >= 0 && state <= WS_MEM_STATE_NO_RAM ? state : 0]);
   }
   g_mem_sent = 0;
}

static void ws_mem_put_u16(unsigned char *p, unsigned v)
{
   p[0] = (unsigned char)(v & 0xff);
   p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void ws_mem_put_u32(unsigned char *p, uint32_t v)
{
   p[0] = (unsigned char)(v & 0xff);
   p[1] = (unsigned char)((v >> 8) & 0xff);
   p[2] = (unsigned char)((v >> 16) & 0xff);
   p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void ws_mem_add_key(rjsonwriter_t *writer, const char *key)
{
   rjsonwriter_add_comma(writer);
   rjsonwriter_add_string(writer, key);
   rjsonwriter_add_colon(writer);
}

/* Main thread: queues the rom_info message for g_mem_regions. Returns
 * false when it did not fit. */
static bool ws_mem_push_info(bool streaming)
{
   ws_mem_rec_t     rec;
   unsigned         i;
   bool             queued      = false;
   int              len         = 0;
   char            *json;
   rjsonwriter_t   *writer;
   core_info_t     *core_info   = NULL;
   runloop_state_t *runloop_st  = runloop_state_get_ptr();
   const char      *content     = path_get(RARCH_PATH_CONTENT);
   const char      *ra_hash     = "";
#ifdef HAVE_CHEEVOS
   const rcheevos_locals_t *locals = get_rcheevos_locals();
   const rc_client_game_t  *game   = locals
         ? rc_client_get_game_info(locals->client) : NULL;

   if (game && game->hash)
      ra_hash = game->hash;
#endif
   core_info_get_current_core(&core_info);

   if (!(writer = rjsonwriter_open_memory()))
      return false;

   rjsonwriter_raw(writer, "{\"type\":\"rom_info\"", 18);
   ws_mem_add_key(writer, "session");
   rjsonwriter_add_unsigned(writer, (unsigned)g_mem_session);
   ws_mem_add_key(writer, "content");
   rjsonwriter_add_string(writer,
         (content && *content) ? path_basename(content) : "");
   ws_mem_add_key(writer, "path");
   rjsonwriter_add_string(writer, content);
   ws_mem_add_key(writer, "core");
   rjsonwriter_add_string(writer, runloop_st->system.info.library_name);
   ws_mem_add_key(writer, "core_version");
   rjsonwriter_add_string(writer, runloop_st->system.info.library_version);
   ws_mem_add_key(writer, "system");
   rjsonwriter_add_string(writer, core_info ? core_info->system_id : NULL);
   ws_mem_add_key(writer, "console_id");
   rjsonwriter_add_unsigned(writer, (unsigned)g_mem_console_id);
   ws_mem_add_key(writer, "ra_hash");
   rjsonwriter_add_string(writer, ra_hash);
   ws_mem_add_key(writer, "hardcore");
   rjsonwriter_add_bool(writer, g_mem_hardcore);
   ws_mem_add_key(writer, "streaming");
   rjsonwriter_add_bool(writer, streaming);
   ws_mem_add_key(writer, "regions");
   rjsonwriter_add_start_array(writer);
   for (i = 0; i < g_mem_region_count; i++)
   {
      if (i)
         rjsonwriter_add_comma(writer);
      rjsonwriter_add_start_object(writer);
      rjsonwriter_add_string(writer, "id");
      rjsonwriter_add_colon(writer);
      rjsonwriter_add_unsigned(writer, i);
      ws_mem_add_key(writer, "name");
      rjsonwriter_add_string(writer, g_mem_regions[i].name);
      ws_mem_add_key(writer, "type");
      rjsonwriter_add_string(writer, g_mem_regions[i].type);
      ws_mem_add_key(writer, "start");
      rjsonwriter_add_unsigned(writer, (unsigned)g_mem_regions[i].start);
      ws_mem_add_key(writer, "size");
      rjsonwriter_add_unsigned(writer, (unsigned)g_mem_regions[i].size);
      rjsonwriter_add_end_object(writer);
   }
   rjsonwriter_add_end_array(writer);
   rjsonwriter_raw(writer, "}", 1);

   json = rjsonwriter_get_memory_buffer(writer, &len);
   rec.kind = WS_MEM_REC_INFO;
   rec.len  = (uint32_t)len;
   if (     json
         && len > 0
         && retro_spsc_write_avail(&g_mem_ring) >= sizeof(rec) + (size_t)len)
   {
      retro_spsc_write(&g_mem_ring, &rec, sizeof(rec));
      retro_spsc_write(&g_mem_ring, json, (size_t)len);
      queued = true;
   }
   rjsonwriter_free(writer);
   return queued;
}

static unsigned ws_mem_add_region(ws_mem_region_t *regions,
      unsigned count, size_t *total, const char *name, const char *type,
      const uint8_t *data, uint32_t start, uint32_t size)
{
   if (     count >= WS_MEM_REGIONS_MAX
         || !size
         || *total + size > WS_MEM_FRAME_MAX)
      return count;
   regions[count].name  = name;
   regions[count].type  = type;
   regions[count].data  = data;
   regions[count].start = start;
   regions[count].size  = size;
   *total              += size;
   return count + 1;
}

/* Main thread: the regions this frame carries. Memory such as NES
 * cartridge RAM is only reachable through the achievements memory map,
 * so that is used once rcheevos has one for the game. */
static unsigned ws_mem_layout(ws_mem_region_t *regions, size_t *total)
{
   retro_ctx_memory_info_t mem;
   unsigned                count = 0;
#ifdef HAVE_CHEEVOS
   const rcheevos_locals_t   *locals = get_rcheevos_locals();
   const rc_client_game_t    *game   = (locals && locals->memory.count)
         ? rc_client_get_game_info(locals->client) : NULL;
   const rc_memory_regions_t *map    = (game && game->console_id)
         ? rc_console_memory_regions(game->console_id) : NULL;
#endif

   *total = 0;

#ifdef HAVE_CHEEVOS
   if (map && map->num_regions)
   {
      uint32_t i;
      for (i = 0; i < map->num_regions; i++)
      {
         const rc_memory_region_t *r = &map->region[i];
         const char *type;

         if (r->type == RC_MEMORY_TYPE_SYSTEM_RAM)
            type = "system_ram";
         else if (r->type == RC_MEMORY_TYPE_SAVE_RAM)
            type = "save_ram";
         else
            continue;
         /* Absent from this core or cartridge. */
         if (!rcheevos_patch_address(r->start_address))
            continue;
         count = ws_mem_add_region(regions, count, total, r->description,
               type, NULL, r->start_address,
               r->end_address - r->start_address + 1);
      }
      return count;
   }
#endif

   mem.id   = RETRO_MEMORY_SYSTEM_RAM;
   mem.data = NULL;
   mem.size = 0;
   core_get_memory(&mem);
   if (mem.data)
      count = ws_mem_add_region(regions, count, total, "System RAM",
            "system_ram", (const uint8_t*)mem.data, 0, (uint32_t)mem.size);

   mem.id   = RETRO_MEMORY_SAVE_RAM;
   mem.data = NULL;
   mem.size = 0;
   core_get_memory(&mem);
   if (mem.data)
      count = ws_mem_add_region(regions, count, total, "Save RAM",
            "save_ram", (const uint8_t*)mem.data, (uint32_t)*total,
            (uint32_t)mem.size);

   return count;
}

/* Main thread: the RetroAchievements console of the game, 0 until
 * achievements have identified it. */
static uint32_t ws_mem_console_id(void)
{
#ifdef HAVE_CHEEVOS
   const rcheevos_locals_t *locals = get_rcheevos_locals();
   const rc_client_game_t  *game   = locals
         ? rc_client_get_game_info(locals->client) : NULL;

   if (game)
      return game->console_id;
#endif
   return 0;
}

static bool ws_mem_layout_changed(const ws_mem_region_t *regions,
      unsigned count)
{
   unsigned i;
   if (count != g_mem_region_count)
      return true;
   for (i = 0; i < count; i++)
      if (     regions[i].start != g_mem_regions[i].start
            || regions[i].size  != g_mem_regions[i].size
            || regions[i].type  != g_mem_regions[i].type
            || regions[i].name  != g_mem_regions[i].name)
         return true;
   return false;
}

/* Main thread: unmapped stretches go out as zeros. */
static void ws_mem_write_region(const ws_mem_region_t *region)
{
   static const unsigned char zeros[256];
   uint32_t done = 0;

   if (region->data)
   {
      retro_spsc_write(&g_mem_ring, region->data, region->size);
      return;
   }

#ifdef HAVE_CHEEVOS
   while (done < region->size)
   {
      unsigned       avail = 0;
      uint32_t       n     = region->size - done;
      const uint8_t *p     = rcheevos_patch_address_avail(
            region->start + done, &avail);
      if (!p || !avail)
         break;
      if (n > avail)
         n = avail;
      retro_spsc_write(&g_mem_ring, p, n);
      done += n;
   }
#endif

   while (done < region->size)
   {
      uint32_t n = region->size - done;
      if (n > sizeof(zeros))
         n = sizeof(zeros);
      retro_spsc_write(&g_mem_ring, zeros, n);
      done += n;
   }
}

/* Main thread: at most one wake-up per drain. */
static void ws_mem_wake(void)
{
   if (!retro_atomic_exchange_int(&g_mem_wake, 1))
      lws_cancel_service(g_lws_ctx);
}

void ws_server_memory_input(unsigned port, unsigned device,
      unsigned idx, unsigned id, int16_t state)
{
   if (     !g_mem_enabled_seen
         || port >= WS_MEM_PORTS
         || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD
         || idx)
      return;

   g_mem_pad_read |= (uint16_t)(1 << port);
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      g_mem_pad[port] |= (uint16_t)state;
   else if (id < 16 && state)
      g_mem_pad[port] |= (uint16_t)(1 << id);
}

void ws_server_memory_frame(void)
{
   ws_mem_region_t regions[WS_MEM_REGIONS_MAX];
   size_t        ram_size;
   unsigned      region_count;
   uint32_t      console_id;
   unsigned      i;
   unsigned      port;
   bool          hardcore = false;
   bool          streaming;
   ws_mem_rec_t  rec;
   unsigned char header[WS_MEM_HEADER_BYTES];

   /* Taken every frame, so the next one starts empty. */
   ws_mem_put_u16(header + 20, g_mem_pad_read);
   for (port = 0; port < WS_MEM_PORTS; port++)
   {
      ws_mem_put_u16(header + 22 + 2 * port, g_mem_pad[port]);
      g_mem_pad[port] = 0;
   }
   g_mem_pad_read = 0;

   if (!g_lws_ctx)
      return;

   g_mem_frame++;

   if (     !g_mem_enabled_seen
         || !retro_atomic_load_relaxed_int(&g_mem_clients))
   {
      g_mem_streaming = false;
      return;
   }

   if (!retro_atomic_load_acquire_int(&g_mem_ring_ready))
   {
      if (!retro_spsc_init(&g_mem_ring, WS_MEM_RING_BYTES))
         return;
      retro_atomic_store_release_int(&g_mem_ring_ready, 1);
   }

   region_count = ws_mem_layout(regions, &ram_size);
   console_id   = ws_mem_console_id();

#ifdef HAVE_CHEEVOS
   hardcore = rcheevos_hardcore_active();
#endif
   streaming = !hardcore && ram_size > 0;
   retro_atomic_store_relaxed_int(&g_mem_state,
           hardcore  ? WS_MEM_STATE_HARDCORE
         : streaming ? WS_MEM_STATE_STREAMING
         :             WS_MEM_STATE_NO_RAM);

   if (     retro_atomic_exchange_int(&g_mem_info_dirty, 0)
         || !g_mem_streaming
         || hardcore    != g_mem_hardcore
         || ram_size    != g_mem_ram_size
         || console_id  != g_mem_console_id
         || ws_mem_layout_changed(regions, region_count))
   {
      g_mem_hardcore     = hardcore;
      g_mem_ram_size     = ram_size;
      g_mem_console_id   = console_id;
      g_mem_region_count = region_count;
      memcpy(g_mem_regions, regions, region_count * sizeof(regions[0]));
      /* Frames go out only behind their session's rom_info. */
      g_mem_streaming = ws_mem_push_info(streaming);
      if (!g_mem_streaming)
      {
         if (!g_mem_info_full)
            fprintf(stderr, "[ws_server] RAM stream: no room for rom_info "
                  "yet, retrying every frame\n");
         g_mem_info_full = true;
         return;
      }
      g_mem_info_full = false;
      ws_mem_wake();
      fprintf(stderr, "[ws_server] RAM stream: session %u, %u bytes in %u "
            "region(s), %s\n", (unsigned)g_mem_session, (unsigned)ram_size,
            region_count,
              hardcore  ? "not streaming: achievements hardcore mode is on"
            : streaming ? "streaming"
            :             "not streaming: the core exposes no RAM");
   }

   if (!streaming)
      return;

   rec.kind = WS_MEM_REC_FRAME;
   rec.len  = (uint32_t)(WS_MEM_HEADER_BYTES + ram_size);
   if (retro_spsc_write_avail(&g_mem_ring) < sizeof(rec) + rec.len)
   {
      retro_atomic_fetch_add_int(&g_mem_dropped, 1);
      return;
   }

   memcpy(header, "RAMF", 4);
   ws_mem_put_u16(header + 4,  3);
   ws_mem_put_u16(header + 6,  region_count);
   ws_mem_put_u32(header + 8,  g_mem_session);
   ws_mem_put_u32(header + 12, g_mem_frame);
   ws_mem_put_u32(header + 16, (uint32_t)ram_size);

   retro_spsc_write(&g_mem_ring, &rec, sizeof(rec));
   retro_spsc_write(&g_mem_ring, header, sizeof(header));
   for (i = 0; i < region_count; i++)
      ws_mem_write_region(&regions[i]);
   ws_mem_wake();
}

void ws_server_memory_setting_poll(bool enabled)
{
   if (!g_lws_ctx || enabled == g_mem_enabled_seen)
      return;

   g_mem_enabled_seen = enabled;
   retro_atomic_store_release_int(&g_mem_enabled, enabled);
   fprintf(stderr, "[ws_server] RAM stream turned %s\n", enabled ? "on" : "off");
}

void ws_server_memory_content_unloaded(void)
{
   g_mem_session++;
   g_mem_frame     = 0;
   g_mem_streaming = false;
}

static bool ws_mem_slot_fill(ws_mem_slot_t *slot, uint32_t kind, size_t len)
{
   if (slot->cap < LWS_PRE + len)
   {
      unsigned char *buf = (unsigned char *)realloc(slot->buf, LWS_PRE + len);
      if (!buf)
         return false;
      slot->buf = buf;
      slot->cap = LWS_PRE + len;
   }
   slot->kind = kind;
   slot->len  = retro_spsc_read(&g_mem_ring, slot->buf + LWS_PRE, len);
   return true;
}

static void ws_mem_slot_free(ws_mem_slot_t *slot)
{
   free(slot->buf);
   slot->buf = NULL;
   slot->cap = 0;
   slot->len = 0;
}

/* Service thread: moves whole records from the ring into the history.
 * Returns true when any arrived. */
static bool ws_mem_drain(void)
{
   bool         got = false;
   ws_mem_rec_t rec;

   if (!retro_atomic_load_acquire_int(&g_mem_ring_ready))
      return false;

   while (retro_spsc_peek(&g_mem_ring, &rec, sizeof(rec)) == sizeof(rec)
         && retro_spsc_read_avail(&g_mem_ring) >= sizeof(rec) + rec.len)
   {
      ws_mem_slot_t *slot = &g_mem_history[g_mem_seq % WS_MEM_HISTORY];

      retro_spsc_read(&g_mem_ring, &rec, sizeof(rec));
      if (!ws_mem_slot_fill(slot, rec.kind, rec.len))
      {
         /* Out of memory: skip the record, keep the stream framed. */
         unsigned char skip[256];
         size_t        left = rec.len;
         while (left)
            left -= retro_spsc_read(&g_mem_ring, skip,
                  left < sizeof(skip) ? left : sizeof(skip));
         continue;
      }

      /* Kept apart from the history for clients that connect later. */
      if (rec.kind == WS_MEM_REC_INFO)
      {
         if (g_mem_info.cap < LWS_PRE + slot->len)
         {
            unsigned char *buf = (unsigned char *)realloc(g_mem_info.buf,
                  LWS_PRE + slot->len);
            if (buf)
            {
               g_mem_info.buf = buf;
               g_mem_info.cap = LWS_PRE + slot->len;
            }
         }
         if (g_mem_info.cap >= LWS_PRE + slot->len)
         {
            memcpy(g_mem_info.buf + LWS_PRE, slot->buf + LWS_PRE, slot->len);
            g_mem_info.kind = WS_MEM_REC_INFO;
            g_mem_info.len  = slot->len;
         }
         fprintf(stderr, "[ws_server] RAM stream: %.*s\n",
               (int)slot->len, (const char *)slot->buf + LWS_PRE);
      }
      else if (slot->len >= WS_MEM_HEADER_BYTES)
      {
         const unsigned char *h = slot->buf + LWS_PRE;
         uint32_t session = (uint32_t)h[8]  | ((uint32_t)h[9]  << 8)
                          | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
         if (session != g_mem_logged_session)
         {
            g_mem_logged_session = session;
            fprintf(stderr, "[ws_server] RAM stream: session %u, first "
                  "frame, %u bytes of RAM\n", (unsigned)session,
                  (unsigned)(slot->len - WS_MEM_HEADER_BYTES));
         }
      }

      g_mem_seq++;
      got = true;
   }

   return got;
}

static void ws_mem_write_slot(struct lws *wsi, const ws_mem_slot_t *slot)
{
   lws_write(wsi, slot->buf + LWS_PRE, slot->len,
         slot->kind == WS_MEM_REC_FRAME ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
}

static int callback_memory(struct lws *wsi,
      enum lws_callback_reasons reason,
      void *user, void *in, size_t len)
{
   ws_mem_session_t *session = (ws_mem_session_t *)user;

   (void)in;
   (void)len;

   if (reason == LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION)
   {
      if (retro_atomic_load_acquire_int(&g_mem_enabled))
         return 0;
      fprintf(stderr, "[ws_server] RAM stream client refused: WebSocket "
            "RAM Stream is off (Settings > Network)\n");
      return -1;
   }

   if (!session)
      return 0;

   switch (reason)
   {
      case LWS_CALLBACK_ESTABLISHED:
         session->established = true;
         session->next_seq    = g_mem_seq;
         session->send_info   = g_mem_info.len > 0;
         retro_atomic_fetch_add_int(&g_mem_clients, 1);
         fprintf(stderr, "[ws_server] RAM stream client connected (%d "
               "connected)\n", retro_atomic_load_relaxed_int(&g_mem_clients));
         if (session->send_info)
            lws_callback_on_writable(wsi);
         break;

      case LWS_CALLBACK_SERVER_WRITEABLE:
         if (!retro_atomic_load_acquire_int(&g_mem_enabled))
         {
            fprintf(stderr, "[ws_server] RAM stream client closed: the "
                  "stream was turned off\n");
            return -1;
         }
         if (session->send_info)
         {
            session->send_info = false;
            ws_mem_write_slot(wsi, &g_mem_info);
         }
         else if (session->next_seq != g_mem_seq)
         {
            if (g_mem_seq - session->next_seq > WS_MEM_HISTORY)
               session->next_seq = g_mem_seq - WS_MEM_HISTORY;
            ws_mem_write_slot(wsi,
                  &g_mem_history[session->next_seq % WS_MEM_HISTORY]);
            if (g_mem_history[session->next_seq % WS_MEM_HISTORY].kind
                  == WS_MEM_REC_FRAME)
               g_mem_sent++;
            session->next_seq++;
         }

         if (session->send_info || session->next_seq != g_mem_seq)
            lws_callback_on_writable(wsi);
         break;

      case LWS_CALLBACK_CLOSED:
         if (session->established)
         {
            session->established = false;
            retro_atomic_fetch_sub_int(&g_mem_clients, 1);
            fprintf(stderr, "[ws_server] RAM stream client disconnected "
                  "(%d connected)\n",
                  retro_atomic_load_relaxed_int(&g_mem_clients));
         }
         break;

      default:
         break;
   }

   return 0;
}

/* -------------------------------------------------------------------------
 * Protocol table
 * ---------------------------------------------------------------------- */

static struct lws_protocols g_protocols[] = {
   {
      "retroarch",
      callback_retroarch,
      sizeof(ws_session_t),  /* per-session data size */
      WS_RX_BUFFER_BYTES
   },
   /* After "retroarch": clients that ask for no protocol get the first. */
   {
      "retroarch-memory",
      callback_memory,
      sizeof(ws_mem_session_t),
      WS_RX_BUFFER_BYTES
   },
   { NULL, NULL, 0, 0 }
};

/* -------------------------------------------------------------------------
 * Background service thread
 * ---------------------------------------------------------------------- */

static void ws_server_thread(void *userdata)
{
   int mem_enabled = 0;

   (void)userdata;

   for (;;)
   {
      bool running;
      bool game_broadcast;
      bool ach_broadcast;
      bool user_broadcast;
      bool progress_broadcast;
      uint32_t progress_id      = 0;
      char     progress_str[WS_PROGRESS_STR_MAX];

      progress_str[0] = '\0';

      slock_lock(g_lock);
      running            = g_running;
      game_broadcast     = g_game_broadcast_pending;
      ach_broadcast      = g_ach_broadcast_pending;
      user_broadcast     = g_user_broadcast_pending;
      progress_broadcast = g_progress_broadcast_pending;
      if (game_broadcast)
         g_game_broadcast_pending = false;
      if (ach_broadcast)
         g_ach_broadcast_pending = false;
      if (user_broadcast)
         g_user_broadcast_pending = false;
      if (progress_broadcast)
      {
         g_progress_broadcast_pending = false;
         progress_id      = g_progress_id;
         strncpy(progress_str, g_progress_str, sizeof(progress_str));
      }
      slock_unlock(g_lock);

      if (!running)
         break;

      if (game_broadcast || ach_broadcast || user_broadcast || progress_broadcast)
      {
         /* Compose the message bitmask for this broadcast. */
         int kind = 0;
         if (game_broadcast)
            kind |= WS_MSG_GAME_STATE | WS_MSG_ACHIEVEMENTS;
         if (ach_broadcast)
            kind |= WS_MSG_ACHIEVEMENTS;
         if (user_broadcast)
            kind |= WS_MSG_USER;
         if (progress_broadcast)
            kind |= WS_MSG_PROGRESS;

         g_broadcast_kind             = kind;
         g_broadcast_progress_id      = progress_id;
         strncpy(g_broadcast_progress_str, progress_str,
               sizeof(g_broadcast_progress_str));

         lws_callback_on_writable_all_protocol(g_lws_ctx, &g_protocols[0]);
      }

      {
         int enabled = retro_atomic_load_acquire_int(&g_mem_enabled);
         /* Turning it off closes the open connections from WRITEABLE. */
         bool closing = mem_enabled && !enabled;
         mem_enabled  = enabled;
         /* Cleared first: a record queued during the drain wakes us
          * again. */
         retro_atomic_store_release_int(&g_mem_wake, 0);
         if (ws_mem_drain() || closing)
            lws_callback_on_writable_all_protocol(g_lws_ctx, &g_protocols[1]);
         ws_mem_report();
      }

      lws_service(g_lws_ctx, WS_SERVICE_TIMEOUT_MS);
   }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

bool ws_server_init(unsigned port)
{
   struct lws_context_creation_info info;

   if (g_lws_ctx)
      return true; /* already running */

   memset(&info, 0, sizeof(info));

   info.port      = (int)port;
   info.iface     = "127.0.0.1"; /* loopback only */
   info.protocols = g_protocols;
   info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT
                  | LWS_SERVER_OPTION_DISABLE_IPV6;
   info.gid       = -1;
   info.uid       = -1;

   g_lws_ctx = lws_create_context(&info);
   if (!g_lws_ctx)
   {
      fprintf(stderr, "[ws_server] Failed to create libwebsockets context "
                      "(port %u).\n", port);
      return false;
   }


   g_lock = slock_new();
   if (!g_lock)
   {
      fprintf(stderr, "[ws_server] Failed to create mutex.\n");
      lws_context_destroy(g_lws_ctx);
      g_lws_ctx = NULL;
      return false;
   }

   slock_lock(g_lock);
   g_running                    = true;
   g_game_broadcast_pending     = false;
   g_ach_broadcast_pending      = false;
   g_user_broadcast_pending     = false;
   g_progress_broadcast_pending = false;
   slock_unlock(g_lock);

   retro_atomic_int_init(&g_mem_ring_ready, 0);
   retro_atomic_int_init(&g_mem_clients, 0);
   retro_atomic_int_init(&g_mem_info_dirty, 0);
   retro_atomic_int_init(&g_mem_dropped, 0);
   retro_atomic_int_init(&g_mem_wake, 0);
   retro_atomic_int_init(&g_mem_state, WS_MEM_STATE_IDLE);
   g_mem_enabled_seen = config_get_ptr()->bools.websocket_ram_stream_enable;
   retro_atomic_int_init(&g_mem_enabled, g_mem_enabled_seen);

   g_thread = sthread_create(ws_server_thread, NULL);
   if (!g_thread)
   {
      fprintf(stderr, "[ws_server] Failed to create service thread.\n");
      slock_lock(g_lock);
      g_running = false;
      slock_unlock(g_lock);
      slock_free(g_lock);
      g_lock    = NULL;
      lws_context_destroy(g_lws_ctx);
      g_lws_ctx = NULL;
      return false;
   }

   fprintf(stderr, "[ws_server] Listening on 127.0.0.1:%u\n", port);
   fprintf(stderr, "[ws_server] RAM stream (\"retroarch-memory\") is %s\n",
         g_mem_enabled_seen ? "on" : "off");
   return true;
}

void ws_server_destroy(void)
{
   if (!g_lws_ctx)
      return;

   /* Signal the service thread to stop. */
   if (g_lock)
   {
      slock_lock(g_lock);
      g_running = false;
      slock_unlock(g_lock);
   }

   /* Wake libwebsockets so the thread exits lws_service() promptly. */
   lws_cancel_service(g_lws_ctx);

   /* Wait for the thread to finish. */
   if (g_thread)
   {
      sthread_join(g_thread);
      g_thread = NULL;
   }

   if (g_lock)
   {
      slock_free(g_lock);
      g_lock = NULL;
   }

   lws_context_destroy(g_lws_ctx);
   g_lws_ctx = NULL;

   {
      unsigned i;
      for (i = 0; i < WS_MEM_HISTORY; i++)
         ws_mem_slot_free(&g_mem_history[i]);
      ws_mem_slot_free(&g_mem_info);
      g_mem_seq = 0;
   }
   if (retro_atomic_load_acquire_int(&g_mem_ring_ready))
   {
      retro_spsc_free(&g_mem_ring);
      retro_atomic_store_release_int(&g_mem_ring_ready, 0);
   }
   g_mem_streaming = false;
}

void ws_server_notify_game_changed(void)
{
   if (!g_lws_ctx || !g_lock)
      return;

   retro_atomic_store_release_int(&g_mem_info_dirty, 1);

   slock_lock(g_lock);
   fprintf(stderr, "[ws_server] ws_server_notify_game_changed\n");
   g_game_broadcast_pending = true;
   slock_unlock(g_lock);

   lws_cancel_service(g_lws_ctx);
}

void ws_server_notify_achievements_changed(void)
{
   if (!g_lws_ctx || !g_lock)
      return;

   slock_lock(g_lock);
   g_ach_broadcast_pending = true;
   slock_unlock(g_lock);

   lws_cancel_service(g_lws_ctx);
}

void ws_server_notify_user_changed(void)
{
   if (!g_lws_ctx || !g_lock)
      return;

   slock_lock(g_lock);
   g_user_broadcast_pending = true;
   slock_unlock(g_lock);

   lws_cancel_service(g_lws_ctx);
}

void ws_server_notify_achievement_progress(uint32_t id,
      const char *measured_progress)
{
   if (!g_lws_ctx || !g_lock)
      return;

   slock_lock(g_lock);
   g_progress_id      = id;
   strncpy(g_progress_str,
         measured_progress ? measured_progress : "",
         sizeof(g_progress_str));
   g_progress_broadcast_pending = true;
   slock_unlock(g_lock);

   lws_cancel_service(g_lws_ctx);
}

