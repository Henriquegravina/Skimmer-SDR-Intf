/*
 * Settings window shared by AirspyHfIntf.dll (opened when the Skimmer starts
 * the radio) and AirspyHfIntfConfig.exe. It only reads and writes the .ini;
 * the DLL watches that file and applies the changes while receiving.
 *
 * While the Skimmer is receiving, two meters show the peak level of the I
 * and Q samples coming from the radio, so the gain can be lowered before
 * they clip.
 *
 * The interface is shown in Portuguese when Windows is in Portuguese,
 * otherwise in English.
 */
#include <windows.h>
#include <commctrl.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"
#include "settings_dialog.h"
#include "level_meter.h"

static SettingsCtx *g_ctx;
static int g_pt;
static LevelMeter g_meter[2];                 /* I, Q */
static int g_live;                            /* the level note shows the receiving text */

#define METER_TIMER 1

enum { S_TITLE, S_G_RADIO, S_L_RADIO, S_REFRESH, S_FIRST, S_NOTE, S_FOUND0, S_FOUND,
       S_G_FRONT, S_AGC, S_L_THR, S_LOW, S_HIGH, S_L_ATT, S_LNA,
       S_G_DSP, S_L_GAIN, S_L_OFFSET, S_INVQ, S_LOG, S_CANCEL, S_APPLY,
       S_FILE, S_ERRWRITE, S_SHOW, S_G_LEVEL, S_LVL_HINT, S_LVL_IDLE, S_COUNT };

static const wchar_t *STR[S_COUNT][2] = {
    { L"Airspy HF+ for Skimmer Server - Settings", L"Airspy HF+ para Skimmer Server - Configura\u00e7\u00e3o" },
    { L"Radio", L"R\u00e1dio" },
    { L"Serial number:", L"N\u00famero de s\u00e9rie:" },
    { L"Refresh", L"Atualizar" },
    { L"(first radio found)", L"(primeiro r\u00e1dio encontrado)" },
    { L"A new serial number is used the next time the Skimmer starts the radio.",
      L"Um novo n\u00famero de s\u00e9rie vale quando o Skimmer reiniciar o r\u00e1dio." },
    { L"No radio found (unplugged or in use).", L"Nenhum r\u00e1dio encontrado (desconectado ou em uso)." },
    { L"%d radio(s) found.", L"%d r\u00e1dio(s) encontrado(s)." },
    { L"Front end", L"Entrada de RF" },
    { L"HF AGC", L"AGC de HF" },
    { L"AGC threshold:", L"Limiar do AGC:" },
    { L"Low", L"Baixo" },
    { L"High", L"Alto" },
    { L"Attenuator:", L"Atenuador:" },
    { L"Preamplifier (LNA)", L"Pr\u00e9-amplificador (LNA)" },
    { L"Output to the Skimmer", L"Sa\u00edda para o Skimmer" },
    { L"Digital gain (dB):", L"Ganho digital (dB):" },
    { L"Freq. offset (Hz):", L"Desloc. freq. (Hz):" },
    { L"Invert Q (use if the spectrum is mirrored)", L"Inverter Q (use se o espectro estiver espelhado)" },
    { L"Write log file (AirspyHfIntf.log)", L"Gravar arquivo de log (AirspyHfIntf.log)" },
    { L"Cancel", L"Cancelar" },
    { L"Apply", L"Aplicar" },
    { L"File: %hs", L"Arquivo: %hs" },
    { L"Could not write the settings file:\n%hs", L"N\u00e3o foi poss\u00edvel gravar o arquivo de configura\u00e7\u00e3o:\n%hs" },
    { L"Show this window when the radio starts", L"Mostrar esta janela quando o r\u00e1dio iniciar" },
    { L"Signal level (peak)", L"N\u00edvel do sinal (pico)" },
    { L"Keep peaks below -3 dBFS. On CLIP, lower the gain.",
      L"Picos abaixo de -3 dBFS. Se acender CLIP, reduza o ganho." },
    { L"Shown while the Skimmer is receiving.", L"Aparece enquanto o Skimmer estiver recebendo." },
};
#define T(id) (STR[id][g_pt])

static void set_text(HWND d, int ctl, int sid) { SetDlgItemTextW(d, ctl, T(sid)); }

static void fill_radios(HWND d)
{
    HWND cb = GetDlgItem(d, IDC_RADIO);
    wchar_t cur[64], buf[400], found[128];
    uint64_t serials[16];
    int n, i;

    GetWindowTextW(cb, cur, 64);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)T(S_FIRST));
    n = g_ctx->list_radios ? g_ctx->list_radios(serials, 16) : 0;
    if (n < 0) n = 0;
    if (n > 16) n = 16;
    for (i = 0; i < n; i++) {
        swprintf(buf, 128, L"%08lX%08lX", (unsigned long)(serials[i] >> 32), (unsigned long)serials[i]);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (cur[0]) SetWindowTextW(cb, cur); else SendMessageW(cb, CB_SETCURSEL, 0, 0);
    if (n == 0) wcscpy(found, T(S_FOUND0)); else swprintf(found, 128, T(S_FOUND), n);
    swprintf(buf, 400, L"%ls\n%ls", found, T(S_NOTE));
    SetDlgItemTextW(d, IDC_NOTE, buf);
}

static void update_enables(HWND d)
{
    BOOL agc = IsDlgButtonChecked(d, IDC_AGC) == BST_CHECKED;
    EnableWindow(GetDlgItem(d, IDC_THR), agc);
    EnableWindow(GetDlgItem(d, IDC_L_THR), agc);
    EnableWindow(GetDlgItem(d, IDC_ATT), !agc);
    EnableWindow(GetDlgItem(d, IDC_L_ATT), !agc);
}

static void update_meters(HWND d)
{
    int live = meter_poll(g_meter, g_ctx->read_peaks, GetDlgItem(d, IDC_METER_I), GetDlgItem(d, IDC_METER_Q));
    if (live != g_live) {
        g_live = live;
        set_text(d, IDC_LEVELNOTE, live ? S_LVL_HINT : S_LVL_IDLE);
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void load(HWND d)
{
    char s[64];
    wchar_t w[64];
    int i;

    GetPrivateProfileStringA("Airspy", "Serial", "", s, sizeof s, g_ctx->ini);
    if (s[0]) { MultiByteToWideChar(CP_ACP, 0, s, -1, w, 64); SetDlgItemTextW(d, IDC_RADIO, w); }
    else SendDlgItemMessageW(d, IDC_RADIO, CB_SETCURSEL, 0, 0);

    CheckDlgButton(d, IDC_AGC, GetPrivateProfileIntA("Airspy", "HfAgc", 1, g_ctx->ini) ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(d, IDC_THR, CB_SETCURSEL, GetPrivateProfileIntA("Airspy", "HfAgcThreshold", 0, g_ctx->ini) ? 1 : 0, 0);
    SendDlgItemMessageW(d, IDC_ATT, CB_SETCURSEL, clampi(GetPrivateProfileIntA("Airspy", "HfAtt", 0, g_ctx->ini), 0, 8), 0);
    CheckDlgButton(d, IDC_LNA, GetPrivateProfileIntA("Airspy", "HfLna", 0, g_ctx->ini) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_INVQ, GetPrivateProfileIntA("Airspy", "InvertQ", 0, g_ctx->ini) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_LOG, GetPrivateProfileIntA("Airspy", "Log", 0, g_ctx->ini) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_SHOW, GetPrivateProfileIntA("Airspy", "ShowWindow", 1, g_ctx->ini) ? BST_CHECKED : BST_UNCHECKED);

    GetPrivateProfileStringA("Airspy", "GainDb", "0", s, sizeof s, g_ctx->ini);
    i = clampi((int)(atof(s) + (atof(s) < 0 ? -0.5 : 0.5)), -60, 60);
    SetDlgItemInt(d, IDC_GAIN, (UINT)i, TRUE);
    SetDlgItemInt(d, IDC_OFFSET, (UINT)GetPrivateProfileIntA("Airspy", "FreqOffsetHz", 0, g_ctx->ini), TRUE);
    update_enables(d);
}

static BOOL put(const char *key, const char *val)
{
    return WritePrivateProfileStringA("Airspy", key, val, g_ctx->ini);
}

static BOOL put_int(const char *key, int v)
{
    char s[32];
    snprintf(s, sizeof s, "%d", v);
    return put(key, s);
}

static BOOL save(HWND d)
{
    char serial[64], clean[64];
    wchar_t msg[600];
    BOOL ok = TRUE;
    int i, n = 0;

    GetDlgItemTextA(d, IDC_RADIO, serial, sizeof serial);
    for (i = 0; serial[i] && serial[0] != '('; i++)        /* keep hex digits only */
        if (isxdigit((unsigned char)serial[i])) clean[n++] = (char)toupper((unsigned char)serial[i]);
    clean[n] = 0;

    ok &= put("Serial", clean);
    ok &= put_int("HfAgc", IsDlgButtonChecked(d, IDC_AGC) == BST_CHECKED);
    ok &= put_int("HfAgcThreshold", (int)SendDlgItemMessageW(d, IDC_THR, CB_GETCURSEL, 0, 0) == 1);
    ok &= put_int("HfAtt", clampi((int)SendDlgItemMessageW(d, IDC_ATT, CB_GETCURSEL, 0, 0), 0, 8));
    ok &= put_int("HfLna", IsDlgButtonChecked(d, IDC_LNA) == BST_CHECKED);
    ok &= put_int("GainDb", clampi((int)GetDlgItemInt(d, IDC_GAIN, NULL, TRUE), -60, 60));
    ok &= put_int("InvertQ", IsDlgButtonChecked(d, IDC_INVQ) == BST_CHECKED);
    ok &= put_int("FreqOffsetHz", (int)GetDlgItemInt(d, IDC_OFFSET, NULL, TRUE));
    ok &= put_int("Log", IsDlgButtonChecked(d, IDC_LOG) == BST_CHECKED);
    ok &= put_int("ShowWindow", IsDlgButtonChecked(d, IDC_SHOW) == BST_CHECKED);
    WritePrivateProfileStringA(NULL, NULL, NULL, g_ctx->ini);    /* flush */

    if (!ok) {
        swprintf(msg, 600, T(S_ERRWRITE), g_ctx->ini);
        MessageBoxW(d, msg, T(S_TITLE), MB_ICONERROR);
    }
    return ok;
}

static INT_PTR CALLBACK dlg_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    wchar_t buf[MAX_PATH + 32];
    int i;

    switch (m) {
    case WM_INITDIALOG:
        if (g_ctx->hwnd_out) *g_ctx->hwnd_out = d;
        SetWindowTextW(d, T(S_TITLE));
        set_text(d, IDC_G_RADIO, S_G_RADIO);   set_text(d, IDC_L_RADIO, S_L_RADIO);
        set_text(d, IDC_REFRESH, S_REFRESH);   set_text(d, IDC_G_FRONT, S_G_FRONT);
        set_text(d, IDC_AGC, S_AGC);           set_text(d, IDC_L_THR, S_L_THR);
        set_text(d, IDC_L_ATT, S_L_ATT);       set_text(d, IDC_LNA, S_LNA);
        set_text(d, IDC_G_DSP, S_G_DSP);       set_text(d, IDC_L_GAIN, S_L_GAIN);
        set_text(d, IDC_L_OFFSET, S_L_OFFSET); set_text(d, IDC_INVQ, S_INVQ);
        set_text(d, IDC_LOG, S_LOG);           set_text(d, IDCANCEL, S_CANCEL);
        set_text(d, IDC_APPLY, S_APPLY);       set_text(d, IDC_SHOW, S_SHOW);
        set_text(d, IDC_G_LEVEL, S_G_LEVEL);   set_text(d, IDC_LEVELNOTE, S_LVL_IDLE);
        swprintf(buf, MAX_PATH + 32, T(S_FILE), g_ctx->ini);
        SetDlgItemTextW(d, IDC_INI, buf);

        SendDlgItemMessageW(d, IDC_THR, CB_ADDSTRING, 0, (LPARAM)T(S_LOW));
        SendDlgItemMessageW(d, IDC_THR, CB_ADDSTRING, 0, (LPARAM)T(S_HIGH));
        for (i = 0; i <= 8; i++) {
            swprintf(buf, 32, L"%d dB", i * 6);
            SendDlgItemMessageW(d, IDC_ATT, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        SendDlgItemMessageW(d, IDC_GAINSPIN, UDM_SETRANGE32, (WPARAM)-60, 60);
        SendDlgItemMessageW(d, IDC_RADIO, CB_LIMITTEXT, 32, 0);
        fill_radios(d);
        load(d);
        meter_reset(&g_meter[0]);
        meter_reset(&g_meter[1]);
        g_live = 0;
        SetTimer(d, METER_TIMER, 100, NULL);
        return TRUE;

    case WM_TIMER:
        if (wp == METER_TIMER) update_meters(d);
        return TRUE;

    case WM_DRAWITEM:
        if (wp == IDC_METER_I || wp == IDC_METER_Q) {
            meter_draw(&g_meter[wp == IDC_METER_Q], wp == IDC_METER_Q ? L"Q" : L"I", (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_AGC:     update_enables(d); return TRUE;
        case IDC_REFRESH: fill_radios(d); return TRUE;
        case IDC_APPLY:   save(d); return TRUE;
        case IDOK:        if (save(d)) EndDialog(d, 1); return TRUE;
        case IDCANCEL:    EndDialog(d, 0); return TRUE;
        }
        break;

    case WM_DESTROY:
        KillTimer(d, METER_TIMER);
        if (g_ctx->hwnd_out) *g_ctx->hwnd_out = NULL;
        break;
    }
    return FALSE;
}

/* Runs the window until it is closed. Returns 1 if closed with OK. */
int settings_dialog_run(HINSTANCE res_module, SettingsCtx *ctx)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    g_ctx = ctx;
    g_pt = ctx->lang >= 0 ? ctx->lang : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
    return DialogBoxParamW(res_module, MAKEINTRESOURCEW(IDD_MAIN), NULL, dlg_proc, 0) == 1;
}

const wchar_t *settings_dialog_title(int lang)
{
    return STR[S_TITLE][lang >= 0 ? lang : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE];
}
