#ifndef RTL_SETTINGS_DIALOG_H
#define RTL_SETTINGS_DIALOG_H
#include <windows.h>

#define RTL_MAX_RADIOS 16

/* IF gain (VGA) steps of the R820T/R828D in tenths of a dB, for codes 0..15 */
#define RTL_IF_GAINS { -47, -21, 5, 35, 77, 112, 136, 149, 163, 195, 231, 265, 300, 337, 372, 408 }
#define RTL_N_IF_GAINS 16

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
    int (*read_peaks)(float peak[2]);                 /* I/Q peaks (0..1) since the last call; returns 0
                                                         when not receiving. NULL in the standalone program */
} RtlSettingsCtx;

int rtl_settings_dialog_run(HINSTANCE res_module, RtlSettingsCtx *ctx);
const wchar_t *rtl_settings_dialog_title(int lang);

#endif
