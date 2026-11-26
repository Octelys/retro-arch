/* Single-source definitions: achievements group.
 * Grammar identical to settings_def_video_sync.h plus S_FLOAT and
 * the _NS no-sublabel variants; the descriptor argument span
 * matches SDESC_<kind>_ROW; row order is menu display order;
 * h2json.py parses these rows for the Crowdin source upload. */

/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_CHEEVOS) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL(cheevos_test_unofficial, CHEEVOS_TEST_UNOFFICIAL,
      "cheevos_test_unofficial",
      false, SD_FLAG_ADVANCED, 0, 0,
      "Test Unofficial Achievements",
      "Use unofficial achievements and/or beta features for testing purposes.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_CHEEVOS) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL(cheevos_richpresence_enable, CHEEVOS_RICHPRESENCE_ENABLE,
      "cheevos_richpresence_enable",
      true, SD_FLAG_ADVANCED, 0, 0,
      "Rich Presence",
      "Periodically sends contextual game information to the RetroAchievements website. Has no effect if 'Hardcore Mode' is enabled.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_CHEEVOS) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL(cheevos_badges_enable, CHEEVOS_BADGES_ENABLE,
      "cheevos_badges_enable",
      false, SD_FLAG_ADVANCED, 0, 0,
      "Achievement Badges",
      "Display badges in the Achievement List.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS #ifdef HAVE_AUDIOMIXER; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_CHEEVOS) && defined(HAVE_AUDIOMIXER) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL(cheevos_unlock_sound_enable, CHEEVOS_UNLOCK_SOUND_ENABLE,
      "cheevos_unlock_sound_enable",
      false, SD_FLAG_NONE, 0, 0,
      "Unlock Sound",
      "Play a sound when an achievement is unlocked.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_CHEEVOS) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL(cheevos_auto_screenshot, CHEEVOS_AUTO_SCREENSHOT,
      "cheevos_auto_screenshot",
      false, SD_FLAG_NONE, 0, 0,
      "Automatic Screenshot",
      "Automatically take a screenshot when an achievement is earned.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_CHEEVOS) || defined(SETTINGS_DEF_STRINGS_PASS)
/* suggestion for translators: translate as 'Play Again Mode' */
S_BOOL(cheevos_start_active, CHEEVOS_START_ACTIVE,
      "cheevos_start_active",
      false, SD_FLAG_ADVANCED, 0, 0,
      "Encore Mode",
      "Start the session with all achievements active (even the ones previously unlocked).")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_CHEEVOS; the string
 * tables always carry these rows via the strings pass. */
#if defined(HAVE_CHEEVOS) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL_EX(cheevos_debug_enable, CHEEVOS_DEBUG_ENABLE,
      "cheevos_debug_enable",
      false, SD_FLAG_ADVANCED, 0, 0, setting_bool_action_left_with_refresh, NULL, NULL, NULL, setting_bool_action_left_with_refresh, setting_bool_action_right_with_refresh, 0,
      "Debug Mode",
      "Send achievement requests to the Debug Server instead of RetroAchievements. The server takes effect when content is next loaded.")
#ifndef SETTINGS_DEF_CONFIG_PASS
S_STRING(cheevos_debug_host, CHEEVOS_DEBUG_HOST,
      "cheevos_debug_host",
      "", SD_FLAG_ALLOW_INPUT, 0, NULL, NULL, setting_generic_action_start_default, NULL, NULL, NULL, ST_UI_TYPE_STRING_LINE_EDIT,
      "Debug Server",
      "Host of the server that receives achievement requests in Debug Mode, such as localhost:8080. Without a scheme, http:// is used.")
#endif
#endif
