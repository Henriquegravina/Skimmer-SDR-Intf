/*
 * Settings window shared by RtlSdrIntf.dll (opened when the Skimmer starts
 * the radio) and RtlSdrIntfConfig.exe. It only reads and writes the .ini;
 * the DLL watches that file and applies the changes while receiving.
 *
 * The interface is shown in Portuguese when Windows is in Portuguese,
 * otherwise in English.
 */
#include <windows.h>
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"
#include "rtl_settings_dialog.h"

static RtlSettingsCtx *g_ctx;
static int g_pt;
static RtlRadio g_list[RTL_MAX_RADIOS + 1];   /* what the device box currently shows */
static int g_nlist;

/* gains of the R820T/R828D tuners (RTL-SDR Blog V3 and V4), in tenths of a dB */
static const int TUNER_GAINS[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
                                   280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };
#define N_GAINS ((int)(sizeof TUNER_GAINS / sizeof TUNER_GAINS[0]))

/* sampling rates of the dongle, in Hz (the same list the DLL accepts) */
static const int DEVICE_RATES[] = { 1152000, 1536000, 1920000, 2304000, 3072000 };
#define N_RATES ((int)(sizeof DEVICE_RATES / sizeof DEVICE_RATES[0]))
#define DEFAULT_RATE_INDEX 1
#define RISKY_RATE 2400000          /* above this the USB link may drop samples */

/* widths of the tuner's IF filter, in Hz; 0 = let the driver choose */
static const int TUNER_BWS[] = { 0, 350000, 450000, 550000, 700000, 900000, 1200000, 1550000 };
#define N_BWS ((int)(sizeof TUNER_BWS / sizeof TUNER_BWS[0]))

enum { S_TITLE, S_G_RADIO, S_L_DEVICE, S_REFRESH, S_FIRST, S_NOTE, S_FOUND0, S_FOUND, S_ABSENT,
       S_G_FRONT, S_L_HFMODE, S_HF_AUTO, S_HF_I, S_HF_Q, S_L_RATE, S_RATE_RISKY, S_RATEINFO, S_TAGC, S_L_TGAIN, S_L_TBW, S_BW_AUTO, S_RAGC, S_BIAS, S_L_PPM,
       S_G_DSP, S_L_GAIN, S_L_OFFSET, S_INVQ, S_LOG, S_SHOW, S_CANCEL, S_APPLY,
       S_FILE, S_ERRWRITE, S_COUNT };

static const wchar_t *STR[S_COUNT][2] = {
    { L"RTL-SDR for Skimmer Server - Settings", L"RTL-SDR para Skimmer Server - Configura\u00e7\u00e3o" },
    { L"Radio", L"R\u00e1dio" },
    { L"Device:", L"Dispositivo:" },
    { L"Refresh", L"Atualizar" },
    { L"(first radio found)", L"(primeiro r\u00e1dio encontrado)" },
    { L"Device and sampling rate changes apply when the radio restarts.",
      L"Dispositivo e taxa de amostragem valem quando o r\u00e1dio reiniciar." },
    { L"No radio found (unplugged or no WinUSB driver).", L"Nenhum r\u00e1dio encontrado (desconectado ou sem driver WinUSB)." },
    { L"%d radio(s) found.", L"%d r\u00e1dio(s) encontrado(s)." },
    { L"not connected", L"n\u00e3o conectado" },
    { L"Front end", L"Entrada de RF" },
    { L"HF reception:", L"Recep\u00e7\u00e3o de HF:" },
    { L"Automatic (V3 and V4)", L"Autom\u00e1tica (V3 e V4)" },
    { L"Direct sampling, I branch", L"Amostragem direta, ramo I" },
    { L"Direct sampling, Q branch", L"Amostragem direta, ramo Q" },
    { L"Sampling rate:", L"Taxa de amostragem:" },
    { L"(may drop samples)", L"(pode perder amostras)" },
    { L"To the Skimmer at 192 / 96 / 48 kHz: decimation %d / %d / %d,\nabout %.1f / %.1f / %.1f bits (the converter has 8).",
      L"Para o Skimmer em 192 / 96 / 48 kHz: decima\u00e7\u00e3o %d / %d / %d,\ncerca de %.1f / %.1f / %.1f bits (o conversor tem 8)." },
    { L"Tuner AGC", L"AGC do sintonizador" },
    { L"Tuner gain:", L"Ganho:" },
    { L"Tuner filter width:", L"Largura do filtro do sintonizador:" },
    { L"Automatic (sampling rate)", L"Autom\u00e1tica (taxa de amostragem)" },
    { L"RTL2832 digital AGC (the gain control in direct sampling)",
      L"AGC digital do RTL2832 (o controle de ganho na amostragem direta)" },
    { L"Bias-T: DC power on the antenna connector", L"Bias-T: alimenta\u00e7\u00e3o DC no conector de antena" },
    { L"Frequency correction (ppm):", L"Corre\u00e7\u00e3o de frequ\u00eancia (ppm):" },
    { L"Output to the Skimmer", L"Sa\u00edda para o Skimmer" },
    { L"Digital gain (dB):", L"Ganho digital (dB):" },
    { L"Freq. offset (Hz):", L"Desloc. freq. (Hz):" },
    { L"Invert Q (use if the spectrum is mirrored)", L"Inverter Q (use se o espectro estiver espelhado)" },
    { L"Write log file (RtlSdrIntf.log)", L"Gravar arquivo de log (RtlSdrIntf.log)" },
    { L"Show this window when the radio starts", L"Mostrar esta janela quando o r\u00e1dio iniciar" },
    { L"Cancel", L"Cancelar" },
    { L"Apply", L"Aplicar" },
    { L"File: %hs", L"Arquivo: %hs" },
    { L"Could not write the settings file:\n%hs", L"N\u00e3o foi poss\u00edvel gravar o arquivo de configura\u00e7\u00e3o:\n%hs" },
};
#define T(id) (STR[id][g_pt])
#define SEC "RtlSdr"

static void set_text(HWND d, int ctl, int sid) { SetDlgItemTextW(d, ctl, T(sid)); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ini_int(const char *key, int def) { return (int)GetPrivateProfileIntA(SEC, key, def, g_ctx->ini); }

/* Fills the device box and selects the configured dongle. Entry 0 is "first
   radio found"; g_list[k] describes entry k + 1. */
static void fill_radios(HWND d, int want_index, const char *want_serial)
{
    HWND cb = GetDlgItem(d, IDC_DEVICE);
    wchar_t buf[400], found[128];
    int n, i, sel = 0;

    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)T(S_FIRST));
    n = g_ctx->list_radios ? g_ctx->list_radios(g_list, RTL_MAX_RADIOS) : 0;
    if (n < 0) n = 0;
    g_nlist = n;
    for (i = 0; i < n; i++) {
        swprintf(buf, 400, L"%d: %hs (%hs)", g_list[i].index, g_list[i].name, g_list[i].serial);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)buf);
    }

    if (want_index >= 0) {
        for (i = 0; i < n && !sel; i++)      /* same position and same serial */
            if (g_list[i].index == want_index && !strcmp(g_list[i].serial, want_serial)) sel = i + 1;
        for (i = 0; i < n && !sel && want_serial[0]; i++)
            if (!strcmp(g_list[i].serial, want_serial)) sel = i + 1;
        if (!sel) {                          /* configured dongle is not plugged in: keep it listed */
            g_list[n].index = want_index;
            snprintf(g_list[n].serial, sizeof g_list[n].serial, "%s", want_serial);
            g_list[n].name[0] = 0;
            swprintf(buf, 400, L"%d: %hs (%ls)", want_index, want_serial, T(S_ABSENT));
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)buf);
            sel = ++g_nlist;
        }
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);

    if (n == 0) wcscpy(found, T(S_FOUND0)); else swprintf(found, 128, T(S_FOUND), n);
    swprintf(buf, 400, L"%ls\n%ls", found, T(S_NOTE));
    SetDlgItemTextW(d, IDC_NOTE, buf);
}

static void current_choice(HWND d, int *index, char *serial, size_t size)
{
    int sel = (int)SendDlgItemMessageW(d, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    *index = -1;
    serial[0] = 0;
    if (sel >= 1 && sel <= g_nlist) {
        *index = g_list[sel - 1].index;
        snprintf(serial, size, "%s", g_list[sel - 1].serial);
    }
}

/* Shows what the chosen sampling rate means at each Skimmer rate: averaging D
   samples into one is worth about log2(D)/2 extra bits. */
static void update_rate_info(HWND d)
{
    wchar_t buf[300];
    int rate = DEVICE_RATES[clampi((int)SendDlgItemMessageW(d, IDC_RATE, CB_GETCURSEL, 0, 0), 0, N_RATES - 1)];
    int d192 = rate / 192000, d96 = rate / 96000, d48 = rate / 48000;
    swprintf(buf, 300, T(S_RATEINFO), d192, d96, d48,
             8.0 + 0.5 * log2((double)d192), 8.0 + 0.5 * log2((double)d96), 8.0 + 0.5 * log2((double)d48));
    SetDlgItemTextW(d, IDC_RATEINFO, buf);
}

static void update_enables(HWND d)
{
    BOOL agc = IsDlgButtonChecked(d, IDC_TAGC) == BST_CHECKED;
    EnableWindow(GetDlgItem(d, IDC_TGAIN), !agc);
    EnableWindow(GetDlgItem(d, IDC_L_TGAIN), !agc);
}

static void load(HWND d)
{
    char s[64];
    int i, g, best = 0;

    GetPrivateProfileStringA(SEC, "Serial", "", s, sizeof s, g_ctx->ini);
    fill_radios(d, ini_int("DeviceIndex", -1), s);

    SendDlgItemMessageW(d, IDC_HFMODE, CB_SETCURSEL, clampi(ini_int("HfMode", 0), 0, 2), 0);
    g = ini_int("SampleRate", DEVICE_RATES[DEFAULT_RATE_INDEX]);
    for (i = 0, best = DEFAULT_RATE_INDEX; i < N_RATES; i++)
        if (DEVICE_RATES[i] == g) best = i;
    SendDlgItemMessageW(d, IDC_RATE, CB_SETCURSEL, best, 0);
    update_rate_info(d);
    best = 0;
    CheckDlgButton(d, IDC_TAGC, ini_int("TunerAgc", 1) ? BST_CHECKED : BST_UNCHECKED);
    g = ini_int("TunerGain", 297);
    for (i = 1; i < N_GAINS; i++)
        if (abs(TUNER_GAINS[i] - g) < abs(TUNER_GAINS[best] - g)) best = i;
    SendDlgItemMessageW(d, IDC_TGAIN, CB_SETCURSEL, best, 0);
    g = ini_int("TunerBandwidthHz", 0);
    for (i = 1, best = 0; i < N_BWS; i++)
        if (abs(TUNER_BWS[i] - g) < abs(TUNER_BWS[best] - g)) best = i;
    SendDlgItemMessageW(d, IDC_TBW, CB_SETCURSEL, best, 0);
    CheckDlgButton(d, IDC_RAGC, ini_int("RtlAgc", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_BIAS, ini_int("BiasTee", 0) ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(d, IDC_PPM, (UINT)ini_int("Ppm", 0), TRUE);

    GetPrivateProfileStringA(SEC, "GainDb", "0", s, sizeof s, g_ctx->ini);
    i = clampi((int)(atof(s) + (atof(s) < 0 ? -0.5 : 0.5)), -60, 60);
    SetDlgItemInt(d, IDC_GAIN, (UINT)i, TRUE);
    SetDlgItemInt(d, IDC_OFFSET, (UINT)ini_int("FreqOffsetHz", 0), TRUE);
    CheckDlgButton(d, IDC_INVQ, ini_int("InvertQ", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_LOG, ini_int("Log", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_SHOW, ini_int("ShowWindow", 1) ? BST_CHECKED : BST_UNCHECKED);
    update_enables(d);
}

static BOOL put(const char *key, const char *val)
{
    return WritePrivateProfileStringA(SEC, key, val, g_ctx->ini);
}

static BOOL put_int(const char *key, int v)
{
    char s[32];
    snprintf(s, sizeof s, "%d", v);
    return put(key, s);
}

static BOOL checked(HWND d, int ctl) { return IsDlgButtonChecked(d, ctl) == BST_CHECKED; }

static BOOL save(HWND d)
{
    char serial[64];
    wchar_t msg[600];
    BOOL ok = TRUE;
    int index;

    current_choice(d, &index, serial, sizeof serial);
    ok &= put_int("DeviceIndex", index);
    ok &= put("Serial", serial);
    ok &= put_int("HfMode", clampi((int)SendDlgItemMessageW(d, IDC_HFMODE, CB_GETCURSEL, 0, 0), 0, 2));
    ok &= put_int("SampleRate", DEVICE_RATES[clampi((int)SendDlgItemMessageW(d, IDC_RATE, CB_GETCURSEL, 0, 0), 0, N_RATES - 1)]);
    ok &= put_int("TunerAgc", checked(d, IDC_TAGC));
    ok &= put_int("TunerGain", TUNER_GAINS[clampi((int)SendDlgItemMessageW(d, IDC_TGAIN, CB_GETCURSEL, 0, 0), 0, N_GAINS - 1)]);
    ok &= put_int("TunerBandwidthHz", TUNER_BWS[clampi((int)SendDlgItemMessageW(d, IDC_TBW, CB_GETCURSEL, 0, 0), 0, N_BWS - 1)]);
    ok &= put_int("RtlAgc", checked(d, IDC_RAGC));
    ok &= put_int("BiasTee", checked(d, IDC_BIAS));
    ok &= put_int("Ppm", clampi((int)GetDlgItemInt(d, IDC_PPM, NULL, TRUE), -1000, 1000));
    ok &= put_int("GainDb", clampi((int)GetDlgItemInt(d, IDC_GAIN, NULL, TRUE), -60, 60));
    ok &= put_int("FreqOffsetHz", (int)GetDlgItemInt(d, IDC_OFFSET, NULL, TRUE));
    ok &= put_int("InvertQ", checked(d, IDC_INVQ));
    ok &= put_int("Log", checked(d, IDC_LOG));
    ok &= put_int("ShowWindow", checked(d, IDC_SHOW));
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
    char serial[64];
    int i, index;
    (void)lp;

    switch (m) {
    case WM_INITDIALOG:
        if (g_ctx->hwnd_out) *g_ctx->hwnd_out = d;
        SetWindowTextW(d, T(S_TITLE));
        set_text(d, IDC_G_RADIO, S_G_RADIO);   set_text(d, IDC_L_DEVICE, S_L_DEVICE);
        set_text(d, IDC_REFRESH, S_REFRESH);   set_text(d, IDC_G_FRONT, S_G_FRONT);
        set_text(d, IDC_L_HFMODE, S_L_HFMODE); set_text(d, IDC_TAGC, S_TAGC);
        set_text(d, IDC_L_TGAIN, S_L_TGAIN);   set_text(d, IDC_RAGC, S_RAGC);
        set_text(d, IDC_L_TBW, S_L_TBW);       set_text(d, IDC_L_RATE, S_L_RATE);
        set_text(d, IDC_BIAS, S_BIAS);         set_text(d, IDC_L_PPM, S_L_PPM);
        set_text(d, IDC_G_DSP, S_G_DSP);       set_text(d, IDC_L_GAIN, S_L_GAIN);
        set_text(d, IDC_L_OFFSET, S_L_OFFSET); set_text(d, IDC_INVQ, S_INVQ);
        set_text(d, IDC_LOG, S_LOG);           set_text(d, IDC_SHOW, S_SHOW);
        set_text(d, IDCANCEL, S_CANCEL);       set_text(d, IDC_APPLY, S_APPLY);
        swprintf(buf, MAX_PATH + 32, T(S_FILE), g_ctx->ini);
        SetDlgItemTextW(d, IDC_INI, buf);

        SendDlgItemMessageW(d, IDC_HFMODE, CB_ADDSTRING, 0, (LPARAM)T(S_HF_AUTO));
        SendDlgItemMessageW(d, IDC_HFMODE, CB_ADDSTRING, 0, (LPARAM)T(S_HF_I));
        SendDlgItemMessageW(d, IDC_HFMODE, CB_ADDSTRING, 0, (LPARAM)T(S_HF_Q));
        for (i = 0; i < N_RATES; i++) {
            swprintf(buf, 64, L"%d.%03d MS/s %ls", DEVICE_RATES[i] / 1000000, DEVICE_RATES[i] / 1000 % 1000,
                     DEVICE_RATES[i] > RISKY_RATE ? T(S_RATE_RISKY) : L"");
            SendDlgItemMessageW(d, IDC_RATE, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        for (i = 0; i < N_GAINS; i++) {
            swprintf(buf, 32, L"%d.%d dB", TUNER_GAINS[i] / 10, TUNER_GAINS[i] % 10);
            SendDlgItemMessageW(d, IDC_TGAIN, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        SendDlgItemMessageW(d, IDC_TBW, CB_ADDSTRING, 0, (LPARAM)T(S_BW_AUTO));
        for (i = 1; i < N_BWS; i++) {
            swprintf(buf, 32, L"%d kHz", TUNER_BWS[i] / 1000);
            SendDlgItemMessageW(d, IDC_TBW, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        SendDlgItemMessageW(d, IDC_GAINSPIN, UDM_SETRANGE32, (WPARAM)-60, 60);
        load(d);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TAGC:    update_enables(d); return TRUE;
        case IDC_RATE:    if (HIWORD(wp) == CBN_SELCHANGE) update_rate_info(d); return TRUE;
        case IDC_REFRESH:
            current_choice(d, &index, serial, sizeof serial);
            fill_radios(d, index, serial);
            return TRUE;
        case IDC_APPLY:   save(d); return TRUE;
        case IDOK:        if (save(d)) EndDialog(d, 1); return TRUE;
        case IDCANCEL:    EndDialog(d, 0); return TRUE;
        }
        break;

    case WM_DESTROY:
        if (g_ctx->hwnd_out) *g_ctx->hwnd_out = NULL;
        break;
    }
    return FALSE;
}

/* Runs the window until it is closed. Returns 1 if closed with OK. */
int rtl_settings_dialog_run(HINSTANCE res_module, RtlSettingsCtx *ctx)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    g_ctx = ctx;
    g_pt = ctx->lang >= 0 ? ctx->lang : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
    return DialogBoxParamW(res_module, MAKEINTRESOURCEW(IDD_MAIN), NULL, dlg_proc, 0) == 1;
}

const wchar_t *rtl_settings_dialog_title(int lang)
{
    return STR[S_TITLE][lang >= 0 ? lang : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE];
}
