#ifndef MAX_AGENT_SETTINGS
#define MAX_AGENT_SETTINGS

/* ------------------------------------------------------------------ */
/* Settings-dialog: tabelle aller einstellungen. der enum-wert ist    */
/* zugleich der index; der typ eines eintrags entscheidet, was enter */
/* tut (siehe handle_settings). neue einstellung = eintrag hier +   */
/* wert in row_setting() + verhalten in handle_settings().            */
/*                                                                    */
/* der system-prompt ist bewusst KEINE einstellung mehr: er ist      */
/* fest in der codebase (prompt.c) und nicht editierbar.              */
/* ------------------------------------------------------------------ */
typedef enum {
    SET_THEME = 0,    /* submenu: theme-optionen */
    SET_CONFIRM_QUIT, /* boolean: toggle */
    SET_COUNT,
} SettingId;

static const char *const SETTING_NAMES[SET_COUNT] = {
    [SET_THEME] = "theme",
    [SET_CONFIRM_QUIT] = "confirm quit",
};

#endif