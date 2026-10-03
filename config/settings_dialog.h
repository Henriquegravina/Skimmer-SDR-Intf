#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H
#include <windows.h>
#include <stdint.h>

typedef struct {
    const char *ini;                                   /* full path of the .ini to edit */
    int (*list_radios)(uint64_t *serials, int max);    /* may be NULL */
    HWND *hwnd_out;                                    /* receives the window handle; may be NULL */
    int lang;                                          /* -1 = follow Windows, 0 = English, 1 = Portuguese */
    int (*read_peaks)(float peak[2]);                  /* I/Q peaks (0..1) since the last call; returns 0
                                                          when not receiving. NULL in the standalone program */
} SettingsCtx;

int settings_dialog_run(HINSTANCE res_module, SettingsCtx *ctx);
const wchar_t *settings_dialog_title(int lang);

#endif
