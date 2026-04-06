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

#ifndef __RARCH_WS_SERVER_H
#define __RARCH_WS_SERVER_H

#include <boolean.h>

/* Default TCP port used when no explicit port is supplied. */
#define RARCH_DEFAULT_WEBSOCKET_PORT 55437

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ws_server_init:
 * @port  : TCP port to listen on (1–65535).
 *
 * Creates a WebSocket server that listens on 127.0.0.1 at the given port
 * and spawns a dedicated background thread to service it.
 * Only one server instance is supported at a time.
 *
 * Returns: true on success, false on failure.
 */
bool ws_server_init(unsigned port);

/**
 * ws_server_destroy:
 *
 * Signals the background service thread to stop, waits for it to exit, then
 * releases all associated resources.  Safe to call even when the server is
 * not running.
 */
void ws_server_destroy(void);

/**
 * ws_server_notify_game_changed:
 *
 * Broadcasts the current game state (obtained from game_state_to_json())
 * followed immediately by the achievements list to all connected WebSocket
 * clients.  Both messages are sent asynchronously by the background service
 * thread; this function returns immediately.
 *
 * Call this whenever the active game changes (start or stop).  Safe to call
 * from any thread while the server is running; no-op when the server is not
 * initialised.
 */
void ws_server_notify_game_changed(void);

/**
 * ws_server_notify_achievements_changed:
 *
 * Broadcasts the updated achievements list to all connected WebSocket
 * clients.  Call this when an achievement is unlocked.  Safe to call
 * from any thread while the server is running.
 */
void ws_server_notify_achievements_changed(void);

/**
 * ws_server_notify_user_changed:
 *
 * Broadcasts the current RA user info (username, display name, score,
 * avatar URL) to all connected WebSocket clients.  Call this after a
 * successful RetroAchievements login.  Safe to call from any thread
 * while the server is running.
 */
void ws_server_notify_user_changed(void);


#ifdef __cplusplus
}
#endif

#endif /* __RARCH_WS_SERVER_H */
