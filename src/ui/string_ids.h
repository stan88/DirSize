// String table IDs of the front end. Only #defines: this header is also read
// by the resource compiler and by cmake/gen_lang.cmake. The text of each ID
// is in lang/<locale>.json (lang/README.md); the build turns those files into
// string tables.
#pragma once

#define IDS_APP_TITLE           100
#define IDS_COL_NAME            110
#define IDS_COL_SIZE            111
#define IDS_COL_ALLOCATED       112
#define IDS_COL_SHARE           113
#define IDS_COL_FILES           114
#define IDS_NO_ACCESS           120
#define IDS_EMPTY               121
#define IDS_LOADING             122
#define IDS_WALKING             123
#define IDS_SRC_SERVICE         130
#define IDS_SRC_WALK            131
#define IDS_NOTE_UPDATING       140
#define IDS_NOTE_STARTING       141
#define IDS_NOTE_NOT_RUNNING    142
#define IDS_NOTE_NOT_INSTALLED  143
#define IDS_NOTE_NOT_INDEXED    144
#define IDS_NOTE_INDEXING       145
#define IDS_NOTE_NOT_LOCAL      146
#define IDS_ERR_NOT_FOUND       150
#define IDS_ERR_ACCESS          151
#define IDS_ERR_FAILED          152
#define IDS_TOTAL               160
#define IDS_FILE_COUNT          161
#define IDS_STATUS_ALLOCATED    162
#define IDS_HINT                170
#define IDS_PICK_FOLDER         171
#define IDS_DRIVE_LABEL         180
#define IDS_VERB_TEXT           181
#define IDS_MENU_VERB           190
#define IDS_MENU_PICK           191
#define IDS_MENU_OPEN           192
#define IDS_MENU_EXPLORE        193
#define IDS_MENU_SHOW_FILE      194
#define IDS_MENU_REFRESH        195
#define IDS_ERR_VERB            196
#define IDS_MENU_LANGUAGE       197
#define IDS_MENU_LANG_AUTO      198
#define IDS_ERR_SETTING         200
#define IDS_MENU_VERB_INSTALLER 201
#define IDS_MENU_AUTOSTART      202
#define IDS_MENU_SHOW_SHARE     203
