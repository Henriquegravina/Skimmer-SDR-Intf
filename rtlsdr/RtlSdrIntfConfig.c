/*
 * RtlSdrIntfConfig.exe - opens the RtlSdrIntf settings window on its own,
 * without the Skimmer. Edits RtlSdrIntf.ini in its own folder, or the .ini
 * given on the command line. /lang:pt or /lang:en forces the language.
 */
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl-sdr.h"
#include "rtl_settings_dialog.h"

static int list_radios(RtlRadio *out, int max)
{
    char m[256], p[256], s[256];
    int n = (int)rtlsdr_get_device_count(), i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        m[0] = p[0] = s[0] = 0;
        out[i].index = i;
        if (rtlsdr_get_device_usb_strings((uint32_t)i, m, p, s) == 0)
            snprintf(out[i].name, sizeof out[i].name, "%s %s", m, p);
        else                                     /* in use by another program */
            snprintf(out[i].name, sizeof out[i].name, "%s", rtlsdr_get_device_name((uint32_t)i));
        snprintf(out[i].serial, sizeof out[i].serial, "%s", s);
    }
    return n;
}

static char g_ini[MAX_PATH];

/* Returns 1 if the .ini can be written, 0 if not (e.g. Program Files without elevation) */
static int ini_writable(void)
{
    HANDLE h = CreateFileA(g_ini, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    RtlSettingsCtx ctx = { g_ini, list_radios, NULL, -1 };
    char exe[MAX_PATH], dir[MAX_PATH], args[MAX_PATH + 16], *p;
    int i, pt;
    (void)prev; (void)cmd; (void)show;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    snprintf(dir, sizeof dir, "%s", exe);
    p = strrchr(dir, '\\');
    if (p) p[1] = 0; else dir[0] = 0;
    snprintf(g_ini, sizeof g_ini, "%sRtlSdrIntf.ini", dir);

    for (i = 1; i < __argc; i++) {
        if (!_stricmp(__argv[i], "/lang:pt")) ctx.lang = 1;
        else if (!_stricmp(__argv[i], "/lang:en")) ctx.lang = 0;
        else GetFullPathNameA(__argv[i], MAX_PATH, g_ini, NULL);
    }

    if (!ini_writable()) {
        pt = ctx.lang >= 0 ? ctx.lang : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
        if (MessageBoxW(NULL, pt
                ? L"Esta pasta exige direitos de administrador para salvar a configura\u00e7\u00e3o.\nReiniciar este programa como administrador?"
                : L"This folder needs administrator rights to save the settings.\nRestart this program as administrator?",
                rtl_settings_dialog_title(ctx.lang), MB_ICONQUESTION | MB_YESNO) == IDYES) {
            snprintf(args, sizeof args, "\"%s\"%s", g_ini,
                     ctx.lang == 1 ? " /lang:pt" : ctx.lang == 0 ? " /lang:en" : "");
            ShellExecuteA(NULL, "runas", exe, args, dir, SW_SHOWNORMAL);
        }
        return 0;
    }
    return rtl_settings_dialog_run(inst, &ctx) ? 0 : 1;
}
