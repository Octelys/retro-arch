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
 * game_state.h – In-memory store for the game currently being played.
 *
 * Provides a thread-safe, single-instance record of the active game.
 * Call game_state_set() when a game starts and game_state_clear() when
 * it ends.  game_state_to_json() serialises the current record as a
 * JSON object suitable for sending over the WebSocket server.
 */

#ifndef __RARCH_GAME_STATE_H
#define __RARCH_GAME_STATE_H

#include <stddef.h>
#include <boolean.h>

#ifdef HAVE_CHEEVOS
/* Forward-declare rcheevos types so callers do not need to pull in the
 * full rc_client.h.  The typedefs are skipped when rc_client.h has
 * already been included (it defines RC_CLIENT_H) to avoid duplicates. */
#ifndef RC_CLIENT_H
typedef struct rc_client_t      rc_client_t;
typedef struct rc_client_game_t rc_client_game_t;
typedef struct rc_client_user_t rc_client_user_t;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum lengths for each field (including NUL terminator). */
#define GAME_STATE_GAME_ID_LEN      64
#define GAME_STATE_GAME_NAME_LEN    512
#define GAME_STATE_CONSOLE_ID_LEN   64
#define GAME_STATE_CONSOLE_NAME_LEN 256
#define GAME_STATE_COVER_URL_LEN    512

/**
 * ra_game_state_t:
 *
 * Describes the game that is currently loaded in RetroArch.
 *
 *   game_id       – Numeric RetroAchievements game ID.
 *   game_name     – RA-canonical title of the game.
 *   console_id    – Short system/platform identifier supplied by the
 *                   core info database (e.g. "snes", "megadrive").
 *   console_name  – Human-readable platform name (e.g.
 *                   "Super Nintendo Entertainment System").
 *   cover_url     – Libretro thumbnails boxart URL built from the
 *                   playlist label and db_name:
 *                   https://thumbnails.libretro.com/<db_name>/Named_Boxarts/<label>.png.
 */
typedef struct
{
   char game_id      [GAME_STATE_GAME_ID_LEN];
   char game_name    [GAME_STATE_GAME_NAME_LEN];
   char console_id   [GAME_STATE_CONSOLE_ID_LEN];
   char console_name [GAME_STATE_CONSOLE_NAME_LEN];
   char cover_url    [GAME_STATE_COVER_URL_LEN];
} ra_game_state_t;

/**
 * game_state_init:
 *
 * Initialises internal resources (mutex).  Must be called once before
 * any other game_state_* function.  Safe to call multiple times.
 */
void game_state_init(void);

/**
 * game_state_deinit:
 *
 * Releases internal resources.  After this call game_state_init() must
 * be called again before the API is used.
 */
void game_state_deinit(void);

/**
 * game_state_set:
 * @state : pointer to the state to store (copied internally).
 *
 * Records @state as the active game.  Thread-safe.
 */
void game_state_set(const ra_game_state_t *state);

/**
 * game_state_clear:
 *
 * Marks the current state as "no game running".  Thread-safe.
 */
void game_state_clear(void);

/**
 * game_state_is_running:
 *
 * Returns true when a game is currently set, false otherwise.
 * Thread-safe.
 */
bool game_state_is_running(void);

/**
 * game_state_get:
 * @out : destination struct to fill (must not be NULL).
 *
 * Copies the current state into @out.
 * Returns true when a game is active, false when no game is set (in
 * which case @out is not modified).  Thread-safe.
 */
bool game_state_get(ra_game_state_t *out);

/**
 * game_state_to_json:
 * @buf      : destination buffer.
 * @buf_size : total size of @buf in bytes.
 *
 * Serialises the current state as a JSON object into @buf.
 *
 * When a game is running the object has the shape:
 *   { "type":"game_playing",
 *     "game_id":"...", "game_name":"...",
 *     "console_id":"...", "console_name":"...",
 *     "cover_url":"..." }
 *
 * When no game is running:
 *   { "type":"no_game" }
 *
 * Returns the number of bytes written to @buf (excluding the NUL
 * terminator), or 0 on error.  Thread-safe.
 */
size_t game_state_to_json(char *buf, size_t buf_size);

#ifdef HAVE_CHEEVOS
/**
 * game_state_update_from_cheevos:
 * @game      : rc_client_game_t returned by rc_client_get_game_info().
 * @game_path : full filesystem path to the ROM.
 *
 * Sole entry-point for populating and broadcasting the WebSocket game
 * state.  Builds a fresh ra_game_state_t from the RA data and
 * game_path, then calls game_state_set() + ws_server_notify_game_changed().
 * Thread-safe.
 */
void game_state_update_from_cheevos(const rc_client_game_t *game, const char *game_path);

/**
 * game_state_set_user_from_cheevos:
 * @user : rc_client_user_t returned by rc_client_get_user_info().
 *
 * Stores the logged-in user's display name, score, and avatar URL so
 * that game_state_user_to_json() can serialize them.  Thread-safe.
 */
void game_state_set_user_from_cheevos(const rc_client_user_t *user);

/**
 * game_state_user_to_json:
 * @buf      : destination buffer.
 * @buf_size : total size of @buf in bytes.
 *
 * Serialises the logged-in RA user as a JSON object:
 *   { "type":"user",
 *     "username":"...", "display_name":"...",
 *     "score":N, "score_softcore":N,
 *     "avatar_url":"..." }
 *
 * When no user is logged in:
 *   { "type":"no_user" }
 *
 * Returns the number of bytes written (excluding NUL), or 0 on error.
 * Thread-safe.
 */
size_t game_state_user_to_json(char *buf, size_t buf_size);

/**
 * game_state_achievements_to_json:
 * @client   : the rc_client_t that owns the loaded game.
 * @buf      : destination buffer.
 * @buf_size : total size of @buf in bytes.
 *
 * Serialises all core achievements for the currently loaded game as a
 * JSON object of the shape:
 *   { "type":"achievements",
 *     "items": [
 *       { "id":1, "name":"...", "description":"...", "points":5,
 *         "status":"unlocked",
 *         "badge_url":"https://..." },
 *       ...
 *     ] }
 *
 * "status" is "unlocked" when the achievement has been earned (softcore
 * or hardcore), "locked" otherwise.
 * "badge_url" is the unlocked badge URL when available, otherwise omitted.
 * When no achievements client is available or no game is loaded, the
 * function returns an empty achievements list
 * ({"type":"achievements","items":[]}) so callers can clear stale state.
 *
 * Returns the number of bytes written (excluding NUL), or 0 on error.
 */
size_t game_state_achievements_to_json(const rc_client_t *client,
      char *buf, size_t buf_size);

#endif

#ifdef __cplusplus
}
#endif

#endif /* __RARCH_GAME_STATE_H */
