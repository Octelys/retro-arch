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

#ifdef HAVE_CHEEVOS
#include "../cheevos/cheevos_locals.h"
#endif

#include <libwebsockets.h>
#include <rthreads/rthreads.h>

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
            strlcpy(session->progress_str, g_broadcast_progress_str,
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
 * Protocol table
 * ---------------------------------------------------------------------- */

static struct lws_protocols g_protocols[] = {
   {
      "retroarch",
      callback_retroarch,
      sizeof(ws_session_t),  /* per-session data size */
      WS_RX_BUFFER_BYTES
   },
   { NULL, NULL, 0, 0 }
};

/* -------------------------------------------------------------------------
 * Background service thread
 * ---------------------------------------------------------------------- */

static void ws_server_thread(void *userdata)
{
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
         strlcpy(progress_str, g_progress_str, sizeof(progress_str));
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
         strlcpy(g_broadcast_progress_str, progress_str,
               sizeof(g_broadcast_progress_str));

         lws_callback_on_writable_all_protocol(g_lws_ctx, &g_protocols[0]);
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
}

void ws_server_notify_game_changed(void)
{
   if (!g_lws_ctx || !g_lock)
      return;

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
   strlcpy(g_progress_str,
         measured_progress ? measured_progress : "",
         sizeof(g_progress_str));
   g_progress_broadcast_pending = true;
   slock_unlock(g_lock);

   lws_cancel_service(g_lws_ctx);
}

