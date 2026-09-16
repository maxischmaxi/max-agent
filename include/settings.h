#ifndef MAX_AGENT_SETTINGS
#define MAX_AGENT_SETTINGS

/* ------------------------------------------------------------------ */
/* Settings-dialog: tabelle aller einstellungen. der enum-wert ist    */
/* zugleich der index; der typ eines eintrags entscheidet, was enter */
/* tut (siehe handle_settings). neue einstellung = eintrag hier +   */
/* wert in row_setting() + verhalten in handle_settings().           */
/* ------------------------------------------------------------------ */
typedef enum {
    SET_THEME = 0,     /* submenu: theme-optionen */
    SET_CONFIRM_QUIT,  /* boolean: toggle */
    SET_SYSTEM_PROMPT, /* submenu: default / aus / selbst schreiben */
    SET_COUNT,
} SettingId;

static const char *const SETTING_NAMES[SET_COUNT] = {
    [SET_THEME] = "theme",
    [SET_CONFIRM_QUIT] = "confirm quit",
    [SET_SYSTEM_PROMPT] = "system prompt",
};

/* ------------------------------------------------------------------ */
/* Untermenue des system-prompts. die config kennt drei zustaende     */
/* (NULL = eingebaute vorlage, "" = gar kein prompt, text = eigener); */
/* hier werden sie explizit waehlbar, statt nur in der json-datei zu  */
/* existieren.                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    PROMPT_DEFAULT = 0, /* -> system_prompt = NULL  */
    PROMPT_OFF,         /* -> system_prompt = ""    */
    PROMPT_EDIT,        /* -> ins eingabefeld       */
    PROMPT_OPT_COUNT,
} PromptOpt;

static const char *const PROMPT_OPT_NAMES[PROMPT_OPT_COUNT] = {
    [PROMPT_DEFAULT] = "default",
    [PROMPT_OFF] = "off",
    [PROMPT_EDIT] = "edit",
};

#endif
