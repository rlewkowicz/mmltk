/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _nsPlacesTables_h_
#define _nsPlacesTables_h_

#define CREATE_MOZ_PLACES                                \
  nsLiteralCString(                                      \
      "CREATE TABLE moz_places ( "                       \
      "  id INTEGER PRIMARY KEY"                         \
      ", url LONGVARCHAR"                                \
      ", title LONGVARCHAR"                              \
      ", rev_host LONGVARCHAR"                           \
      ", visit_count INTEGER DEFAULT 0"                  \
      ", hidden INTEGER DEFAULT 0 NOT NULL"              \
      ", typed INTEGER DEFAULT 0 NOT NULL"               \
      ", frecency INTEGER DEFAULT -1 NOT NULL"           \
      ", last_visit_date INTEGER "                       \
      ", guid TEXT"                                      \
      ", foreign_count INTEGER DEFAULT 0 NOT NULL"       \
      ", url_hash INTEGER DEFAULT 0 NOT NULL "           \
      ", description TEXT"                               \
      ", preview_image_url TEXT"                         \
      ", site_name TEXT"                                 \
      ", origin_id INTEGER REFERENCES moz_origins(id)"   \
      ", recalc_frecency INTEGER NOT NULL DEFAULT 0"     \
      ", alt_frecency INTEGER"                           \
      ", recalc_alt_frecency INTEGER NOT NULL DEFAULT 0" \
      ")")

#define CREATE_MOZ_HISTORYVISITS            \
  nsLiteralCString(                         \
      "CREATE TABLE moz_historyvisits ("    \
      "  id INTEGER PRIMARY KEY"            \
      ", from_visit INTEGER"                \
      ", place_id INTEGER"                  \
      ", visit_date INTEGER"                \
      ", visit_type INTEGER"                \
      ", session INTEGER"                   \
      ", source INTEGER DEFAULT 0 NOT NULL" \
      ", triggeringPlaceId INTEGER"         \
      ")")

#define CREATE_MOZ_INPUTHISTORY         \
  nsLiteralCString(                     \
      "CREATE TABLE moz_inputhistory (" \
      "  place_id INTEGER NOT NULL"     \
      ", input LONGVARCHAR NOT NULL"    \
      ", use_count INTEGER"             \
      ", PRIMARY KEY (place_id, input)" \
      ")")

#define CREATE_MOZ_ANNOS                 \
  nsLiteralCString(                      \
      "CREATE TABLE moz_annos ("         \
      "  id INTEGER PRIMARY KEY"         \
      ", place_id INTEGER NOT NULL"      \
      ", anno_attribute_id INTEGER"      \
      ", content LONGVARCHAR"            \
      ", flags INTEGER DEFAULT 0"        \
      ", expiration INTEGER DEFAULT 0"   \
      ", type INTEGER DEFAULT 0"         \
      ", dateAdded INTEGER DEFAULT 0"    \
      ", lastModified INTEGER DEFAULT 0" \
      ")")

#define CREATE_MOZ_ANNO_ATTRIBUTES         \
  nsLiteralCString(                        \
      "CREATE TABLE moz_anno_attributes (" \
      "  id INTEGER PRIMARY KEY"           \
      ", name VARCHAR(32) UNIQUE NOT NULL" \
      ")")

#define CREATE_MOZ_ITEMS_ANNOS           \
  nsLiteralCString(                      \
      "CREATE TABLE moz_items_annos ("   \
      "  id INTEGER PRIMARY KEY"         \
      ", item_id INTEGER NOT NULL"       \
      ", anno_attribute_id INTEGER"      \
      ", content LONGVARCHAR"            \
      ", flags INTEGER DEFAULT 0"        \
      ", expiration INTEGER DEFAULT 0"   \
      ", type INTEGER DEFAULT 0"         \
      ", dateAdded INTEGER DEFAULT 0"    \
      ", lastModified INTEGER DEFAULT 0" \
      ")")

#define CREATE_MOZ_BOOKMARKS                                                   \
  nsLiteralCString(                                                            \
      "CREATE TABLE moz_bookmarks ("                                           \
      "  id INTEGER PRIMARY KEY"                                               \
      ", type INTEGER"                                                         \
      ", fk INTEGER DEFAULT NULL"                                \
      ", parent INTEGER"                                                       \
      ", position INTEGER"                                                     \
      ", title LONGVARCHAR"                                                    \
      ", keyword_id INTEGER"                                                   \
      ", folder_type TEXT"                                                     \
      ", dateAdded INTEGER"                                                    \
      ", lastModified INTEGER"                                                 \
      ", guid TEXT"                                                            \
      ")")

#define CREATE_MOZ_KEYWORDS                    \
  nsLiteralCString(                            \
      "CREATE TABLE moz_keywords ("            \
      "  id INTEGER PRIMARY KEY AUTOINCREMENT" \
      ", keyword TEXT UNIQUE"                  \
      ", place_id INTEGER"                     \
      ", post_data TEXT"                       \
      ")")

#define CREATE_MOZ_ORIGINS                               \
  nsLiteralCString(                                      \
      "CREATE TABLE moz_origins ( "                      \
      "id INTEGER PRIMARY KEY, "                         \
      "prefix TEXT NOT NULL, "                           \
      "host TEXT NOT NULL, "                             \
      "frecency INTEGER NOT NULL, "                      \
      "recalc_frecency INTEGER NOT NULL DEFAULT 0, "     \
      "alt_frecency INTEGER, "                           \
      "recalc_alt_frecency INTEGER NOT NULL DEFAULT 0, " \
      "block_until_ms INTEGER, "                         \
      "block_pages_until_ms INTEGER, "                   \
      "UNIQUE (prefix, host) "                           \
      ")")

#define CREATE_MOZ_OPENPAGES_TEMP                   \
  nsLiteralCString(                                 \
      "CREATE TEMP TABLE moz_openpages_temp ("      \
      "  url TEXT NOT NULL"                         \
      ", userContextId INTEGER NOT NULL"            \
      ", groupId TEXT NOT NULL"                     \
      ", open_count INTEGER"                        \
      ", PRIMARY KEY (url, userContextId, groupId)" \
      ")")

#define CREATE_UPDATEORIGINSDELETE_TEMP                   \
  nsLiteralCString(                                       \
      "CREATE TEMP TABLE moz_updateoriginsdelete_temp ( " \
      "  prefix TEXT NOT NULL, "                          \
      "  host TEXT NOT NULL, "                            \
      "  PRIMARY KEY (prefix, host) "                     \
      ") WITHOUT ROWID")

#define CREATE_MOZ_PAGES_W_ICONS          \
  nsLiteralCString(                       \
      "CREATE TABLE moz_pages_w_icons ( " \
      "id INTEGER PRIMARY KEY, "          \
      "page_url TEXT NOT NULL, "          \
      "page_url_hash INTEGER NOT NULL "   \
      ") ")

#define CREATE_MOZ_ICONS                       \
  nsLiteralCString(                            \
      "CREATE TABLE moz_icons ( "              \
      "id INTEGER PRIMARY KEY, "               \
      "icon_url TEXT NOT NULL, "               \
      "fixed_icon_url_hash INTEGER NOT NULL, " \
      "width INTEGER NOT NULL DEFAULT 0, "     \
      "root INTEGER NOT NULL DEFAULT 0, "      \
      "color INTEGER, "                        \
      "expire_ms INTEGER NOT NULL DEFAULT 0, " \
      "flags INTEGER NOT NULL DEFAULT 0, "     \
      "data BLOB "                             \
      ") ")

#define CREATE_MOZ_ICONS_TO_PAGES                                              \
  nsLiteralCString(                                                            \
      "CREATE TABLE moz_icons_to_pages ( "                                     \
      "page_id INTEGER NOT NULL, "                                             \
      "icon_id INTEGER NOT NULL, "                                             \
      "expire_ms INTEGER NOT NULL DEFAULT 0, "                                 \
      "PRIMARY KEY (page_id, icon_id), "                                       \
      "FOREIGN KEY (page_id) REFERENCES moz_pages_w_icons ON DELETE CASCADE, " \
      "FOREIGN KEY (icon_id) REFERENCES moz_icons ON DELETE CASCADE "          \
      ") WITHOUT ROWID ")

#define CREATE_MOZ_META         \
  nsLiteralCString(             \
      "CREATE TABLE moz_meta (" \
      "key TEXT PRIMARY KEY, "  \
      "value NOT NULL"          \
      ") WITHOUT ROWID ")

#define CREATE_MOZ_PLACES_METADATA                                           \
  nsLiteralCString(                                                          \
      "CREATE TABLE moz_places_metadata ("                                   \
      "id INTEGER PRIMARY KEY, "                                             \
      "place_id INTEGER NOT NULL, "                                          \
      "referrer_place_id INTEGER, "                                          \
      "created_at INTEGER NOT NULL DEFAULT 0, "                              \
      "updated_at INTEGER NOT NULL DEFAULT 0, "                              \
      "total_view_time INTEGER NOT NULL DEFAULT 0, "                         \
      "typing_time INTEGER NOT NULL DEFAULT 0, "                             \
      "key_presses INTEGER NOT NULL DEFAULT 0, "                             \
      "scrolling_time INTEGER NOT NULL DEFAULT 0, "                          \
      "scrolling_distance INTEGER NOT NULL DEFAULT 0, "                      \
      "document_type INTEGER NOT NULL DEFAULT 0, "                           \
      "search_query_id INTEGER, "                                            \
      "FOREIGN KEY (place_id) REFERENCES moz_places(id) ON DELETE CASCADE, " \
      "FOREIGN KEY (referrer_place_id) REFERENCES moz_places(id) ON DELETE " \
      "CASCADE, "                                                            \
      "FOREIGN KEY(search_query_id) REFERENCES "                             \
      "moz_places_metadata_search_queries(id) ON DELETE CASCADE "            \
      "CHECK(place_id != referrer_place_id) "                                \
      ")")

#define CREATE_MOZ_PLACES_METADATA_SEARCH_QUERIES                        \
  nsLiteralCString(                                                      \
      "CREATE TABLE IF NOT EXISTS moz_places_metadata_search_queries ( " \
      "id INTEGER PRIMARY KEY, "                                         \
      "terms TEXT NOT NULL UNIQUE "                                      \
      ")")

#define CREATE_MOZ_PREVIEWS_TOMBSTONES                        \
  nsLiteralCString(                                           \
      "CREATE TABLE IF NOT EXISTS moz_previews_tombstones ( " \
      "  hash TEXT PRIMARY KEY "                              \
      ") WITHOUT ROWID")

#define CREATE_MOZ_NEWTAB_STORY_CLICK            \
  nsLiteralCString(                              \
      "CREATE TABLE moz_newtab_story_click ( "   \
      "  feature TEXT NOT NULL, "                \
      "  timestamp_s INTEGER NOT NULL, "         \
      "  card_format_enum INTEGER NOT NULL, "    \
      "  position INTEGER NOT NULL, "            \
      "  section_position INTEGER NOT NULL, "    \
      "  feature_value REAL NOT NULL DEFAULT 1 " \
      ")")

#define CREATE_MOZ_NEWTAB_STORY_IMPRESSION          \
  nsLiteralCString(                                 \
      "CREATE TABLE moz_newtab_story_impression ( " \
      "  feature TEXT NOT NULL, "                   \
      "  timestamp_s INTEGER NOT NULL, "            \
      "  card_format_enum INTEGER NOT NULL, "       \
      "  position INTEGER NOT NULL, "               \
      "  section_position INTEGER NOT NULL, "       \
      "  feature_value REAL NOT NULL DEFAULT 1 "    \
      ")")

#define CREATE_MOZ_NEWTAB_SHORTCUTS_INTERACTION                          \
  nsLiteralCString(                                                      \
      "CREATE TABLE moz_newtab_shortcuts_interaction ( "                 \
      "  id INTEGER PRIMARY KEY, "                                       \
      "  place_id INTEGER NOT NULL REFERENCES moz_places(id) ON DELETE " \
      "CASCADE, "                                                        \
      "  event_type INTEGER NOT NULL, "                                  \
      "  timestamp_s INTEGER NOT NULL, "                                 \
      "  pinned INTEGER NOT NULL CHECK (pinned IN (0, 1)), "             \
      "  tile_position INTEGER NOT NULL"                                 \
      ")")

#endif  // _nsPlacesTables_h_
