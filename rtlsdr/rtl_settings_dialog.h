#ifndef RTL_SETTINGS_DIALOG_H
#define RTL_SETTINGS_DIALOG_H
#include <windows.h>

#define RTL_MAX_RADIOS 16

typedef struct {
    int  index;                 /* librtlsdr device index */
    char name[128];             /* "manufacturer product" */
    char serial[64];
} RtlRadio;

typedef struct {
    const char *ini;                                  /* full path of the .ini to edit */
    int (*list_radios)(RtlRadio *out, int max);       /* may be NULL */
    HWND *hwnd_out;                                   /* receives the window handle; may be NULL */
    int lang;                                         /* -1 = follow Windows, 0 = English, 1 = Portuguese */
} RtlSettingsCtx;

int rtl_settings_dialog_run(HINSTANCE res_module, RtlSettingsCtx *ctx);
const wchar_t *rtl_settings_dialog_title(int lang);

#endif
