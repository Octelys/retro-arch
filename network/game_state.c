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
 * game_state.c – Thread-safe in-memory store for the currently active game.
 *
 * The module holds a single ra_game_state_t record plus a boolean flag
 * that indicates whether a game is running.  All public functions
 * serialise access through a mutex so they may be called from any thread.
 */

#include "game_state.h"

#include <rthreads/rthreads.h>
#include <compat/strl.h>
#include <string/stdstring.h>
#include <file/file_path.h>

#include "../playlist.h"

#include <string.h>
#include <stdio.h>


#define GAME_STATE_TAG "[game_state] "

#ifdef HAVE_CHEEVOS
#include "../deps/rcheevos/include/rc_client.h"
#include "../deps/rcheevos/include/rc_consoles.h"
#include "ws_server.h"
#endif

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static slock_t       *g_state_lock = NULL;
static ra_game_state_t g_state;
static bool            g_is_running = false;

#ifdef HAVE_CHEEVOS
/* Logged-in RetroAchievements user (set once on login). */
typedef struct
{
   char     username    [128];
   char     display_name[128];
   char     avatar_url  [512];
   uint32_t score;
   uint32_t score_softcore;
   bool     is_logged_in;
} ra_user_state_t;

static ra_user_state_t g_user_state;
#endif

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

void game_state_init(void)
{
   if (g_state_lock)
      return; /* already initialised */
   g_state_lock = slock_new();
   memset(&g_state, 0, sizeof(g_state));
   g_is_running = false;
#ifdef HAVE_CHEEVOS
   memset(&g_user_state, 0, sizeof(g_user_state));
#endif
}

void game_state_deinit(void)
{
   if (!g_state_lock)
      return;
   slock_free(g_state_lock);
   g_state_lock = NULL;
   g_is_running = false;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/**
 * json_append_field:
 * @buf       : destination buffer.
 * @pos       : current write offset; updated on return.
 * @buf_size  : total size of @buf.
 * @key       : JSON key (must not contain characters needing escaping).
 * @value     : value string to escape and append.
 *
 * Appends   ,"key":"escaped-value"   to @buf starting at *pos.
 * Updates *pos.  Silently truncates if the buffer is too small.
 */
static void json_append_field(char *buf, size_t *pos,
      size_t buf_size, const char *key, const char *value)
{
   /* Write the key prefix */
   int n = snprintf(buf + *pos, buf_size - *pos, ",\"%s\":\"", key);
   if (n > 0)
      *pos += (size_t)n;

   /* Escape and copy the value character-by-character.
    * Reserve 2 bytes for a potential 2-byte escape sequence ('\\' + char);
    * the closing '"' is appended separately after the loop. */
   if (!string_is_empty(value))
   {
      const unsigned char *src = (const unsigned char *)value;
      while (*src && *pos + 2 < buf_size)
      {
         unsigned char c = *src++;
         if (c == '"' || c == '\\')
         {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = (char)c;
         }
         else if (c >= 0x20) /* skip bare control characters */
            buf[(*pos)++] = (char)c;
      }
   }

   /* Close the quoted value */
   if (*pos + 1 < buf_size)
      buf[(*pos)++] = '"';
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void game_state_set(const ra_game_state_t *state)
{
   if (!state || !g_state_lock)
      return;
   slock_lock(g_state_lock);
   g_state      = *state;
   g_is_running = true;
   slock_unlock(g_state_lock);
}

void game_state_clear(void)
{
   if (!g_state_lock)
      return;
   slock_lock(g_state_lock);
   memset(&g_state, 0, sizeof(g_state));
   g_is_running = false;
   slock_unlock(g_state_lock);
}

bool game_state_is_running(void)
{
   bool running;
   if (!g_state_lock)
      return false;
   slock_lock(g_state_lock);
   running = g_is_running;
   slock_unlock(g_state_lock);
   return running;
}

bool game_state_get(ra_game_state_t *out)
{
   bool running;
   if (!out || !g_state_lock)
      return false;
   slock_lock(g_state_lock);
   running = g_is_running;
   if (running)
      *out = g_state;
   slock_unlock(g_state_lock);
   return running;
}

size_t game_state_to_json(char *buf, size_t buf_size)
{
   ra_game_state_t snap;
   bool            running;
   size_t          pos = 0;
   int             n;

   if (!buf || buf_size < 2)
      return 0;

   memset(&snap, 0, sizeof(snap));

   if (g_state_lock)
   {
      slock_lock(g_state_lock);
      running = g_is_running;
      if (running)
         snap = g_state;
      slock_unlock(g_state_lock);
   }
   else
   {
      running = false;
   }

   if (!running)
   {
      n = snprintf(buf, buf_size, "{\"type\":\"no_game\"}");
      return (n > 0) ? (size_t)n : 0;
   }

   /* Open object and write the type field */
   n = snprintf(buf, buf_size, "{\"type\":\"game_playing\"");
   if (n <= 0)
      return 0;
   pos = (size_t)n;

   json_append_field(buf, &pos, buf_size, "game_id",        snap.game_id);
   json_append_field(buf, &pos, buf_size, "game_name",      snap.game_name);
   json_append_field(buf, &pos, buf_size, "console_id",     snap.console_id);
   json_append_field(buf, &pos, buf_size, "console_name",   snap.console_name);
   json_append_field(buf, &pos, buf_size, "cover_url", snap.cover_url);

   /* Close object */
   if (pos + 1 < buf_size)
   {
      buf[pos++] = '}';
      buf[pos]   = '\0';
   }

   return pos;
}

#ifdef HAVE_CHEEVOS
/**
 * game_state_update_from_cheevos:
 *
 * Sole entry-point for populating and broadcasting the WebSocket game
 * state.  Called from rcheevos_client_load_game_callback() once the
 * async RetroAchievements lookup has completed.  Builds a fresh
 * ra_game_state_t from RA data:
 *
 *   id         → game_id      (authoritative numeric RA game ID)
 *   title      → game_name    (RA-canonical title)
 *   console_id → console_name (human-readable via rc_console_name())
 *   playlist entry → cover_url (libretro thumbnails boxart URL from label + db_name)
 */
void game_state_update_from_cheevos(const rc_client_game_t *game,
      const char *game_path)
{
   ra_game_state_t               state;
   const struct playlist_entry  *entry  = NULL;
   const char                   *db_src = NULL;

   if (!game || game->id == 0)
      return;

   memset(&state, 0, sizeof(state));

   /* Authoritative numeric RA game ID */
   snprintf(state.game_id, sizeof(state.game_id), "%u", (unsigned)game->id);

   /* RA-canonical game title */
   if (!string_is_empty(game->title))
      strlcpy(state.game_name, game->title, sizeof(state.game_name));

   /* Human-readable console name */
   if (game->console_id != 0)
   {
      const char *con = rc_console_name(game->console_id);
      if (!string_is_empty(con))
         strlcpy(state.console_name, con, sizeof(state.console_name));
   }

   /* Look up the playlist entry for this ROM to get the label and
    * db_name used by the libretro thumbnail server. */
   if (!string_is_empty(game_path))
   {
      playlist_t *pl = playlist_get_cached();
      fprintf(stderr, GAME_STATE_TAG "game_path: \"%s\"\n", game_path);
      fprintf(stderr, GAME_STATE_TAG "cached playlist: %s\n", pl ? "found" : "NULL");
      if (pl)
      {
         playlist_get_index_by_path(pl, game_path, &entry);
         fprintf(stderr, GAME_STATE_TAG "playlist entry: %s\n", entry ? "found" : "not found");
         if (entry)
         {
            fprintf(stderr, GAME_STATE_TAG "  entry->label:   \"%s\"\n",
                  entry->label   ? entry->label   : "(null)");
            fprintf(stderr, GAME_STATE_TAG "  entry->db_name: \"%s\"\n",
                  entry->db_name ? entry->db_name : "(null)");
         }
      }
   }
   else
      fprintf(stderr, GAME_STATE_TAG "game_path: (empty)\n");

   /* System folder = db_name without ".lpl" extension.
    * Falls back to the RA console name when no playlist entry exists. */
   if (entry && !string_is_empty(entry->db_name))
      db_src = entry->db_name;
   else
      db_src = state.console_name;

   fprintf(stderr, GAME_STATE_TAG "db_src: \"%s\"\n", db_src ? db_src : "(null)");

   /* Build the cover URL from the playlist label and db_name:
    *   https://thumbnails.libretro.com/<db_name>/Named_Boxarts/<label>.png
    * Characters not allowed in No-Intro filenames are replaced with '_',
    * matching gfx_thumbnail_fill_content_img(). */
   if (!string_is_empty(db_src))
   {
      static const char scrub_chars[] = {
         '&', '*', '/', ':', '`', '"', '<', '>', '?', '\\', '|', '\0'
      };
      char db_name   [GAME_STATE_CONSOLE_NAME_LEN];
      char name_label[GAME_STATE_GAME_NAME_LEN];
      char *p;

      /* Scrub the system folder name (strip .lpl extension) */
      fill_pathname(db_name, db_src, "", sizeof(db_name));
      while ((p = strpbrk(db_name, scrub_chars)))
         *p = '_';

      /* Playlist label, or fall back to ROM basename */
      name_label[0] = '\0';
      if (entry && !string_is_empty(entry->label))
         strlcpy(name_label, entry->label, sizeof(name_label));
      else if (!string_is_empty(game_path))
         fill_pathname(name_label, path_basename(game_path), "", sizeof(name_label));

      while ((p = strpbrk(name_label, scrub_chars)))
         *p = '_';

      fprintf(stderr, GAME_STATE_TAG "name_label: \"%s\"\n", name_label);

      if (!string_is_empty(name_label))
         snprintf(state.cover_url, sizeof(state.cover_url),
               "https://thumbnails.libretro.com/%s/Named_Boxarts/%s.png",
               db_name, name_label);

      fprintf(stderr, GAME_STATE_TAG "cover_url: \"%s\"\n", state.cover_url);
   }

   game_state_set(&state);
   ws_server_notify_game_changed();
}

/**
 * game_state_achievements_to_json:
 *
 * Iterates all core achievements for the loaded game and serializes
 * them as a JSON object.  Each entry contains:
 *   id          – numeric achievement ID
 *   name        – achievement title
 *   description – achievement description text
 *   points      – point value
 *   status      – "unlocked" or "locked"
 *   badge_url   – unlocked badge image URL (omitted when unavailable)
 */
size_t game_state_achievements_to_json(const rc_client_t *client,
      char *buf, size_t buf_size)
{
   rc_client_achievement_list_t *list;
   rc_client_subset_list_t      *subset_list;
   uint32_t  base_subset_id = 0;
   size_t    pos            = 0;
   int       n;
   uint32_t  bi, ai;
   bool      first          = true;

   if (!client || !buf || buf_size < 2)
      return 0;

   /* Determine the core subset id deterministically.
    * rc_client_create_subset_list always places the core (base) set at
    * index 0 — it is prepended in rcheevos internals while addon subsets
    * are appended.  When num_subsets > 1 rcheevos sets bucket->subset_id
    * to the subset's own id, so we need this value to identify base-game
    * buckets.  When there is only one subset subset_id is always 0, so
    * the comparison below still works correctly. */
   subset_list = rc_client_create_subset_list((rc_client_t *)client);
   if (subset_list)
   {
      if (subset_list->num_subsets > 0)
         base_subset_id = subset_list->subsets[0]->id;
      rc_client_destroy_subset_list(subset_list);
   }

   /* Open the envelope */
   n = snprintf(buf, buf_size, "{\"type\":\"achievements\",\"items\":[");
   if (n <= 0)
      return 0;
   pos = (size_t)n;

   list = rc_client_create_achievement_list(
         (rc_client_t *)client,
         RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
         RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);

   if (list)
   {
      for (bi = 0; bi < list->num_buckets; bi++)
      {
         const rc_client_achievement_bucket_t *bucket = &list->buckets[bi];

         /* Only LOCKED and UNLOCKED buckets. */
         if (bucket->bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED &&
             bucket->bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED)
            continue;

         /* When multiple subsets are loaded each bucket carries the id of
          * the subset it belongs to.  Only keep buckets from the core set.
          * When there is only one subset, rcheevos sets subset_id=0 for all
          * buckets regardless of the real subset id, so accept both 0 and
          * the actual base subset id. */
         if (bucket->subset_id != base_subset_id)
            continue;

         for (ai = 0; ai < bucket->num_achievements; ai++)
         {
            const rc_client_achievement_t *ach = bucket->achievements[ai];
            const char *status;

            /* Belt-and-suspenders: only core achievements. */
            if (ach->category != RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE)
               continue;

            status = (ach->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE)
                  ? "unlocked" : "locked";

            if (!first)
            {
               if (pos + 1 < buf_size)
                  buf[pos++] = ',';
            }
            first = false;

            n = snprintf(buf + pos, buf_size - pos,
                  "{\"id\":%u,\"points\":%u,\"status\":\"%s\"",
                  (unsigned)ach->id, (unsigned)ach->points, status);
            if (n > 0)
               pos += (size_t)n;

            json_append_field(buf, &pos, buf_size, "name",        ach->title);
            json_append_field(buf, &pos, buf_size, "description", ach->description);

            if (!string_is_empty(ach->badge_url))
               json_append_field(buf, &pos, buf_size, "badge_url", ach->badge_url);

            if (pos + 1 < buf_size)
               buf[pos++] = '}';
         }
      }

      rc_client_destroy_achievement_list(list);
   }

   /* Close array and object */
   n = snprintf(buf + pos, buf_size - pos, "]}");
   if (n > 0)
      pos += (size_t)n;

   buf[pos < buf_size ? pos : buf_size - 1] = '\0';
   return pos;
}

/**
 * game_state_set_user_from_cheevos:
 *
 * Copies the logged-in RA user information into the internal user state
 * so that game_state_user_to_json() can serialise it at any time.
 * Called from rcheevos_client_login_callback() once the async login
 * request has succeeded.
 */
void game_state_set_user_from_cheevos(const rc_client_user_t *user)
{
   if (!user || !g_state_lock)
      return;

   slock_lock(g_state_lock);
   memset(&g_user_state, 0, sizeof(g_user_state));

   if (!string_is_empty(user->username))
      strlcpy(g_user_state.username, user->username,
            sizeof(g_user_state.username));

   if (!string_is_empty(user->display_name))
      strlcpy(g_user_state.display_name, user->display_name,
            sizeof(g_user_state.display_name));

   if (!string_is_empty(user->avatar_url))
      strlcpy(g_user_state.avatar_url, user->avatar_url,
            sizeof(g_user_state.avatar_url));

   g_user_state.score           = user->score;
   g_user_state.score_softcore  = user->score_softcore;
   g_user_state.is_logged_in    = true;

   slock_unlock(g_state_lock);
}

/**
 * game_state_user_to_json:
 *
 * Serialises the current logged-in RA user as a JSON object.  Returns
 * the number of bytes written or 0 on error.
 */
size_t game_state_user_to_json(char *buf, size_t buf_size)
{
   ra_user_state_t snap;
   size_t          pos = 0;
   int             n;

   if (!buf || buf_size < 2)
      return 0;

   if (g_state_lock)
   {
      slock_lock(g_state_lock);
      snap = g_user_state;
      slock_unlock(g_state_lock);
   }
   else
   {
      memset(&snap, 0, sizeof(snap));
   }

   if (!snap.is_logged_in)
   {
      n = snprintf(buf, buf_size, "{\"type\":\"no_user\"}");
      return (n > 0) ? (size_t)n : 0;
   }

   n = snprintf(buf, buf_size, "{\"type\":\"user\"");
   if (n <= 0)
      return 0;
   pos = (size_t)n;

   json_append_field(buf, &pos, buf_size, "username",     snap.username);
   json_append_field(buf, &pos, buf_size, "display_name", snap.display_name);
   json_append_field(buf, &pos, buf_size, "avatar_url",   snap.avatar_url);

   n = snprintf(buf + pos, buf_size - pos,
         ",\"score\":%u,\"score_softcore\":%u",
         (unsigned)snap.score, (unsigned)snap.score_softcore);
   if (n > 0)
      pos += (size_t)n;

   if (pos + 1 < buf_size)
   {
      buf[pos++] = '}';
      buf[pos]   = '\0';
   }

   return pos;
}
#endif

