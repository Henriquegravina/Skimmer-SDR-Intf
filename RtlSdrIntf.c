/*
 * RtlSdrIntf.dll - RTL-SDR (RTL-SDR Blog V3 / V4 and compatible dongles) for
 * Skimmer Server and RTTY Skimmer Server
 *
 * Implements VE3NEA's radio DLL interface (SdrTypes unit):
 *   GetSdrInfo, StartRx, StopRx, SetRxFrequency, SetCtrlBits, ReadPort
 *
 * The dongle is driven by the RTL-SDR Blog fork of librtlsdr, linked in
 * statically (see build.sh). That driver handles HF by itself:
 *   - V3: direct sampling (Q branch) below 24 MHz
 *   - V4: built-in upconverter below 28.8 MHz
 * The dongle samples at a selectable rate (1.152 to 3.072 MS/s) and the stream
 * is decimated down to the rate the Skimmer asks for (192/96/48 kHz), which
 * adds resolution to the 8-bit converter. Samples are delivered in blocks of
 * rate/93.75 (2048/1024/512).
 *
 * Settings live in the .ini and are reread while receiving. The settings
 * window (rtlsdr/rtl_settings_dialog.c) opens when the Skimmer starts the
 * radio and writes to that same file.
 *
 * Build (32-bit!): sh build.sh
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include "rtl-sdr.h"
#include "rtlsdr/rtl_settings_dialog.h"
#include "common/level_meter.h"

/* ------------------------------------------------------------------ */
/* Skimmer interface (SdrTypes.pas)                                    */
/* ------------------------------------------------------------------ */
#define RATE_48KHZ   0
#define RATE_96KHZ   1
#define RATE_192KHZ  2
#define BLOCKS_PER_SEC 93.75
#define MAX_RX_COUNT 8

typedef struct { float Re, Im; } Cmplx;

typedef void (__stdcall *TIqProc)(int RxHandle, Cmplx **Data);
typedef void (__stdcall *TAudioProc)(int RxHandle, Cmplx *InIq, Cmplx *OutLR, int OutCount);
typedef void (__stdcall *TStatusBitsProc)(int RxHandle, unsigned char Bits);
typedef void (__stdcall *TLoadProgressProc)(int RxHandle, int Current, int Total);
typedef void (__stdcall *TErrorProc)(int RxHandle, char *ErrText);

typedef struct {
    char *DeviceName;
    int   MaxRecvCount;
    float ExactRates[3];
} TSdrInfo;

typedef struct {
    int  RxHandle;
    int  RecvCount;
    int  RateID;
    BOOL LowLatency;
    TIqProc           IqProc;
    TAudioProc        AudioProc;
    TStatusBitsProc   StatusBitsProc;
    TLoadProgressProc LoadProgressProc;
    TErrorProc        ErrorProc;
} TSdrSettings;

/* ------------------------------------------------------------------ */
/* Decimation stages (windowed-sinc FIR, Kaiser window)                */
/*                                                                     */
/* The dongle samples faster than the Skimmer needs and the excess is  */
/* filtered away. Averaging D samples into one spreads the 8-bit       */
/* quantisation noise over a band D times wider than the one kept, so  */
/* the result is worth about 8 + log2(D)/2 bits.                       */
/* ------------------------------------------------------------------ */
#define DEFAULT_RATE 1536000
#define USB_BUF_LEN  16384           /* bytes per USB transfer, a few ms */
#define MAX_TAPS     127
#define MAX_STAGES   8

/* Device rates offered: multiples of 192 kHz that are exact on the 28.8 MHz
   crystal and leave an even decimation factor at every Skimmer rate. */
static const int DEVICE_RATES[] = { 1152000, 1536000, 1920000, 2304000, 3072000 };
#define N_RATES ((int)(sizeof DEVICE_RATES / sizeof DEVICE_RATES[0]))

typedef struct {
    float val[MAX_TAPS];             /* non-zero taps only: a half-band filter */
    int   idx[MAX_TAPS];             /* has zeros at every other position */
    int   nz;
    int   ntaps;
    int   factor;                    /* decimation of this stage: 2, 3 or 5 */
    int   count;
    Cmplx hist[2 * MAX_TAPS];        /* doubled buffer: the window is always contiguous */
    int   pos;
} Stage;

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 40; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

/* Low-pass with its cutoff at the new Nyquist frequency (fs_in / 2 / factor) */
static void stage_init(Stage *s, int factor, int ntaps)
{
    const double beta = 8.0, pi = 3.14159265358979323846;
    double sum = 0.0, t[MAX_TAPS];
    int i, m = (ntaps - 1) / 2;

    memset(s, 0, sizeof *s);
    s->ntaps = ntaps;
    s->factor = factor;
    for (i = 0; i < ntaps; i++) {
        int n = i - m;
        double h = (n == 0) ? 1.0 / factor : sin(pi * n / factor) / (pi * n);
        double r = (double)n / m;
        t[i] = h * bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
        if (n != 0 && n % factor == 0) t[i] = 0.0;       /* exact zeros of the sinc */
        sum += t[i];
    }
    for (i = 0; i < ntaps; i++)
        if (t[i] != 0.0) {
            s->idx[s->nz] = i;
            s->val[s->nz++] = (float)(t[i] / sum);
        }
}

/* returns 1 when *out was produced */
static int stage_push(Stage *s, Cmplx in, Cmplx *out)
{
    const Cmplx *w;
    float re = 0.0f, im = 0.0f;
    int i, n = s->ntaps;

    s->hist[s->pos] = in;
    s->hist[s->pos + n] = in;
    if (++s->pos == n) s->pos = 0;
    if (++s->count < s->factor) return 0;
    s->count = 0;

    w = &s->hist[s->pos];
    for (i = 0; i < s->nz; i++) {
        re += w[s->idx[i]].Re * s->val[i];
        im += w[s->idx[i]].Im * s->val[i];
    }
    out->Re = re;
    out->Im = im;
    return 1;
}

/* Builds the chain for a total decimation: an optional divide-by-3 or -5
   first, then divide-by-2 stages. Filters are short while the wanted band is
   a small part of the rate; only the last stage is long and sharp. Returns
   the number of stages, or 0 if the factor cannot be built. */
static int build_chain(Stage *st, int decim)
{
    int n = 0, odd = 1, twos = 0, i, remaining;

    while (decim % 2 == 0) { decim /= 2; twos++; }
    odd = decim;
    if ((odd != 1 && odd != 3 && odd != 5) || twos < 1 || twos + 1 > MAX_STAGES) return 0;
    if (odd != 1) stage_init(&st[n++], odd, 10 * odd + 1);
    for (i = 0; i < twos; i++) {
        remaining = twos - 1 - i;
        stage_init(&st[n++], 2, remaining == 0 ? 127 : remaining == 1 ? 31 : 15);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static TSdrSettings     g_set;
static rtlsdr_dev_t    *g_dev;
static HANDLE           g_reader;            /* thread blocked in rtlsdr_read_async */
static CRITICAL_SECTION g_cs;
static volatile LONG    g_running;
static volatile LONG    g_gen;               /* generation of the .ini watcher thread */
static HANDLE           g_cleanup;           /* thread that closes the radio */
static HINSTANCE        g_inst;
static int              g_pt;                /* 1 = messages in Portuguese */

static Stage  g_stage[MAX_STAGES];
static int    g_nstages;
static Cmplx *g_buf[2];                      /* [0] = data, [1] = zeros */
static Cmplx *g_ptrs[MAX_RX_COUNT];
static int    g_block, g_fill;
static float  g_scale, g_qsign;
static float  g_lut[256];
static float  g_dc_re, g_dc_im;
static volatile LONG g_peak[2];              /* peak |I| and |Q| at the converter, for the window's meters */
static int    g_freq = 14050000;

/* settings (RtlSdrIntf.ini) */
static char   g_dir[MAX_PATH], g_ini[MAX_PATH], g_logfile[MAX_PATH];
static int    cfg_rate, cfg_index, cfg_hfmode, cfg_tuner_agc, cfg_tuner_gain, cfg_tuner_bw, cfg_rtl_agc, cfg_bias;
static int    cfg_ppm, cfg_invq, cfg_offset, cfg_show, cfg_log;
static double cfg_gain_db;
static char   cfg_serial[64];
static char   g_name[64] = "RTL-SDR";

#define TR(en, pt) (g_pt ? (pt) : (en))

static void logf_(const char *fmt, ...)
{
    FILE *f;
    SYSTEMTIME st;
    va_list ap;
    if (!cfg_log) return;
    f = fopen(g_logfile, "a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void report_error(const char *msg)
{
    logf_("ERROR: %s", msg);
    if (g_set.ErrorProc) g_set.ErrorProc(g_set.RxHandle, (char *)msg);
}

static void load_config(void)
{
    char tmp[32];
    cfg_index      = GetPrivateProfileIntA("RtlSdr", "DeviceIndex", -1, g_ini);
    cfg_hfmode     = GetPrivateProfileIntA("RtlSdr", "HfMode", 0, g_ini);
    cfg_rate       = GetPrivateProfileIntA("RtlSdr", "SampleRate", DEFAULT_RATE, g_ini);
    cfg_tuner_agc  = GetPrivateProfileIntA("RtlSdr", "TunerAgc", 1, g_ini);
    cfg_tuner_gain = GetPrivateProfileIntA("RtlSdr", "TunerGain", 297, g_ini);
    cfg_tuner_bw   = GetPrivateProfileIntA("RtlSdr", "TunerBandwidthHz", 0, g_ini);
    cfg_rtl_agc    = GetPrivateProfileIntA("RtlSdr", "RtlAgc", 0, g_ini);
    cfg_bias       = GetPrivateProfileIntA("RtlSdr", "BiasTee", 0, g_ini);
    cfg_ppm        = GetPrivateProfileIntA("RtlSdr", "Ppm", 0, g_ini);
    cfg_invq       = GetPrivateProfileIntA("RtlSdr", "InvertQ", 0, g_ini);
    cfg_offset     = GetPrivateProfileIntA("RtlSdr", "FreqOffsetHz", 0, g_ini);
    cfg_show       = GetPrivateProfileIntA("RtlSdr", "ShowWindow", 1, g_ini);
    cfg_log        = GetPrivateProfileIntA("RtlSdr", "Log", 0, g_ini);
    GetPrivateProfileStringA("RtlSdr", "GainDb", "0", tmp, sizeof tmp, g_ini);
    cfg_gain_db = atof(tmp);
    GetPrivateProfileStringA("RtlSdr", "Serial", "", cfg_serial, sizeof cfg_serial, g_ini);
    if (cfg_hfmode < 0 || cfg_hfmode > 2) cfg_hfmode = 0;
}

/* ------------------------------------------------------------------ */
/* Radio list. A dongle that is open cannot be queried again, so the   */
/* list is kept: StartRx scans before opening, "Refresh" updates it.   */
/* ------------------------------------------------------------------ */
static RtlRadio g_radios[RTL_MAX_RADIOS];
static int      g_nradios;

static int scan_radios(RtlRadio *out, int max)
{
    char m[256], p[256], s[256];
    int n = (int)rtlsdr_get_device_count(), i;

    if (n > RTL_MAX_RADIOS) n = RTL_MAX_RADIOS;
    for (i = 0; i < n; i++) {
        m[0] = p[0] = s[0] = 0;
        if (rtlsdr_get_device_usb_strings((uint32_t)i, m, p, s) == 0) {
            g_radios[i].index = i;
            snprintf(g_radios[i].name, sizeof g_radios[i].name, "%s %s", m, p);
            snprintf(g_radios[i].serial, sizeof g_radios[i].serial, "%s", s);
        } else if (i >= g_nradios) {             /* busy and never seen before */
            g_radios[i].index = i;
            snprintf(g_radios[i].name, sizeof g_radios[i].name, "%s", rtlsdr_get_device_name((uint32_t)i));
            g_radios[i].serial[0] = 0;
        }
    }
    g_nradios = n;
    for (i = 0; out && i < n && i < max; i++) out[i] = g_radios[i];
    return n < max ? n : max;
}

/* Which dongle to open: the configured index if its serial matches (or no
   serial is set), otherwise the first dongle with that serial, otherwise 0. */
static int pick_radio(void)
{
    int i;
    if (g_nradios == 0) return -1;
    if (cfg_index >= 0 && cfg_index < g_nradios &&
        (!cfg_serial[0] || !strcmp(g_radios[cfg_index].serial, cfg_serial)))
        return cfg_index;
    if (cfg_serial[0])
        for (i = 0; i < g_nradios; i++)
            if (!strcmp(g_radios[i].serial, cfg_serial)) return i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Settings window, in its own thread so the Skimmer is never blocked  */
/* ------------------------------------------------------------------ */
static HWND          g_ui_hwnd;
static volatile LONG g_ui_running;

/* I and Q peaks at the 8-bit converter (1.0 = code 0 or 255) since the
   previous call; 0 when the radio is not receiving */
static int read_peaks(float peak[2])
{
    if (!g_running) return 0;
    peak[0] = meter_peak_take(&g_peak[0]);
    peak[1] = meter_peak_take(&g_peak[1]);
    return 1;
}

static DWORD WINAPI ui_thread(LPVOID arg)
{
    static RtlSettingsCtx ctx;
    (void)arg;
    ctx.ini = g_ini;
    ctx.list_radios = scan_radios;
    ctx.hwnd_out = &g_ui_hwnd;
    ctx.lang = -1;
    ctx.read_peaks = read_peaks;
    rtl_settings_dialog_run(g_inst, &ctx);
    InterlockedExchange(&g_ui_running, 0);
    return 0;
}

static void show_settings_window(void)
{
    HMODULE pin;
    HANDLE h;
    if (InterlockedCompareExchange(&g_ui_running, 1, 0) != 0) {
        if (g_ui_hwnd) ShowWindow(g_ui_hwnd, SW_SHOWNOACTIVATE);   /* already open */
        return;
    }
    /* keep the DLL loaded for as long as the window exists */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)ui_thread, &pin);
    h = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (h) CloseHandle(h); else InterlockedExchange(&g_ui_running, 0);
}

/* ------------------------------------------------------------------ */
/* Everything that may change while the radio is running               */
/* ------------------------------------------------------------------ */
static void apply_settings(rtlsdr_dev_t *dev)
{
    int gains[64], n, i, best = 0;

    rtlsdr_set_freq_correction(dev, cfg_ppm);
    /* 0 = automatic (driver picks direct sampling on a V3 below 24 MHz and
       the upconverter on a V4), 1 = force I branch, 2 = force Q branch */
    rtlsdr_set_direct_sampling(dev, cfg_hfmode);

    if (cfg_tuner_agc) {
        rtlsdr_set_tuner_gain_mode(dev, 0);
    } else {
        rtlsdr_set_tuner_gain_mode(dev, 1);
        n = rtlsdr_get_tuner_gains(dev, NULL);
        if (n > 0 && n <= 64) {
            rtlsdr_get_tuner_gains(dev, gains);
            for (i = 1; i < n; i++)
                if (abs(gains[i] - cfg_tuner_gain) < abs(gains[best] - cfg_tuner_gain)) best = i;
            rtlsdr_set_tuner_gain(dev, gains[best]);
        }
    }
    /* Width of the tuner's analogue IF filter, ahead of the 8-bit converter.
       0 lets the driver match it to the sampling rate (1 MHz or more); a
       narrower one keeps strong signals outside the Skimmer's band away from
       the converter. It moves the IF, so the frequency is set again below. */
    rtlsdr_set_tuner_bandwidth(dev, cfg_tuner_bw > 0 ? (uint32_t)cfg_tuner_bw : 0);
    rtlsdr_set_agc_mode(dev, cfg_rtl_agc ? 1 : 0);
    rtlsdr_set_bias_tee(dev, cfg_bias ? 1 : 0);
    rtlsdr_set_center_freq(dev, (uint32_t)(g_freq + cfg_offset));

    /* samples are scaled to +-1.0 here; the Skimmer expects a 32-bit integer scale */
    g_scale = (float)(2147483648.0 * pow(10.0, cfg_gain_db / 20.0));
    g_qsign = cfg_invq ? -1.0f : 1.0f;
}

static int ini_mtime(FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_ini, GetFileExInfoStandard, &a)) return 0;
    *ft = a.ftLastWriteTime;
    return 1;
}

/* Rereads the .ini when it changes (the settings window writes it) and
   applies the settings without restarting reception. The choice of dongle
   only takes effect on the next StartRx. */
static DWORD WINAPI watch_thread(LPVOID arg)
{
    LONG gen = (LONG)(LONG_PTR)arg;
    FILETIME last = {0, 0}, now;
    ini_mtime(&last);
    while (g_running && g_gen == gen) {
        Sleep(300);
        if (!ini_mtime(&now) || CompareFileTime(&now, &last) == 0) continue;
        last = now;
        Sleep(100);                      /* let the other program finish writing */
        EnterCriticalSection(&g_cs);
        if (g_dev && g_gen == gen) {
            load_config();
            apply_settings(g_dev);
            logf_("settings reloaded");
        }
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sample path                                                         */
/* ------------------------------------------------------------------ */
static void on_samples(unsigned char *buf, uint32_t len, void *ctx)
{
    uint32_t i;
    int k, pk_i = 0, pk_q = 0;
    (void)ctx;

    if (!g_running) return;
    for (i = 0; i + 1 < len; i += 2) {
        Cmplx s;
        /* distance from the converter's midpoint, 1 to 255: 255 means code 0 or 255 */
        int a_i = abs(2 * buf[i] - 255), a_q = abs(2 * buf[i + 1] - 255);
        if (a_i > pk_i) pk_i = a_i;
        if (a_q > pk_q) pk_q = a_q;

        s.Re = g_lut[buf[i]];
        s.Im = g_lut[buf[i + 1]];

        /* remove the ADC's DC offset (it would show as a carrier at the centre) */
        g_dc_re += (s.Re - g_dc_re) * (1.0f / 32768.0f);
        g_dc_im += (s.Im - g_dc_im) * (1.0f / 32768.0f);
        s.Re -= g_dc_re;
        s.Im -= g_dc_im;

        for (k = 0; k < g_nstages; k++)
            if (!stage_push(&g_stage[k], s, &s)) break;
        if (k < g_nstages) continue;

        g_buf[0][g_fill].Re = s.Re * g_scale;
        g_buf[0][g_fill].Im = s.Im * g_scale * g_qsign;
        if (++g_fill == g_block) {
            g_fill = 0;
            if (g_running && g_set.IqProc) g_set.IqProc(g_set.RxHandle, g_ptrs);
        }
    }
    meter_peak_merge(&g_peak[0], pk_i / 255.0f);
    meter_peak_merge(&g_peak[1], pk_q / 255.0f);
}

static DWORD WINAPI reader_thread(LPVOID arg)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)arg;
    rtlsdr_reset_buffer(dev);
    rtlsdr_read_async(dev, on_samples, NULL, 0, USB_BUF_LEN);   /* blocks until cancelled */
    if (g_running && g_dev == dev)
        report_error(TR("RTL-SDR: the radio stopped sending data (unplugged?)",
                        "RTL-SDR: o radio parou de enviar dados (desconectado?)"));
    return 0;
}

typedef struct { rtlsdr_dev_t *dev; HANDLE reader; } CleanupArg;

static DWORD WINAPI cleanup_thread(LPVOID arg)
{
    CleanupArg *c = (CleanupArg *)arg;
    rtlsdr_cancel_async(c->dev);
    if (c->reader) {
        WaitForSingleObject(c->reader, 5000);
        CloseHandle(c->reader);
    }
    rtlsdr_set_bias_tee(c->dev, 0);      /* never leave DC on the antenna connector */
    rtlsdr_close(c->dev);
    free(c);
    logf_("radio closed");
    return 0;
}

static void wait_cleanup(DWORD ms)
{
    if (g_cleanup) {
        WaitForSingleObject(g_cleanup, ms);
        CloseHandle(g_cleanup);
        g_cleanup = NULL;
    }
}

static void free_buffers(void)
{
    if (g_buf[0]) _aligned_free(g_buf[0]);
    if (g_buf[1]) _aligned_free(g_buf[1]);
    g_buf[0] = g_buf[1] = NULL;
}

/* ------------------------------------------------------------------ */
/* Exported functions                                                  */
/* ------------------------------------------------------------------ */
__declspec(dllexport) void __stdcall GetSdrInfo(TSdrInfo *info)
{
    if (!info) return;
    info->DeviceName = g_name;
    info->MaxRecvCount = 1;
    info->ExactRates[RATE_48KHZ]  = 48000.0f;
    info->ExactRates[RATE_96KHZ]  = 96000.0f;
    info->ExactRates[RATE_192KHZ] = 192000.0f;
}

__declspec(dllexport) void __stdcall StartRx(TSdrSettings *settings)
{
    rtlsdr_dev_t *dev = NULL;
    int target, i, index, rate;
    LONG gen;
    HANDLE h;

    if (!settings) return;
    if (g_running) return;
    wait_cleanup(6000);

    g_set = *settings;
    g_set.RateID &= 0xFF;   /* Skimmer Server 1.1+ leaves garbage in the high bytes */
    load_config();
    logf_("StartRx RateID=%d RecvCount=%d", g_set.RateID, g_set.RecvCount);

    switch (g_set.RateID) {
        case RATE_48KHZ:  target = 48000;  break;
        case RATE_96KHZ:  target = 96000;  break;
        case RATE_192KHZ: target = 192000; break;
        default:
            report_error(TR("RTL-SDR: unknown sampling rate", "RTL-SDR: taxa de amostragem desconhecida"));
            return;
    }
    if (g_set.RecvCount > 1) {
        report_error(TR("RTL-SDR: only 1 receiver is supported", "RTL-SDR: apenas 1 receptor e suportado"));
        return;
    }

    scan_radios(NULL, 0);                /* before opening: afterwards the dongle cannot be queried */
    if (cfg_show) show_settings_window();

    index = pick_radio();
    if (index < 0 || rtlsdr_open(&dev, (uint32_t)index) != 0 || !dev) {
        report_error(TR("RTL-SDR: radio not found (unplugged, no WinUSB driver or in use by another program)",
                        "RTL-SDR: radio nao encontrado (desconectado, sem driver WinUSB ou em uso por outro programa)"));
        return;
    }
    logf_("opened device %d: %s, serial %s", index, g_radios[index].name, g_radios[index].serial);

    rate = DEFAULT_RATE;                 /* only the rates in the list are accepted */
    for (i = 0; i < N_RATES; i++)
        if (DEVICE_RATES[i] == cfg_rate) rate = cfg_rate;
    g_nstages = build_chain(g_stage, rate / target);
    if (g_nstages == 0 || rtlsdr_set_sample_rate(dev, (uint32_t)rate) != 0) {
        rtlsdr_close(dev);
        report_error(TR("RTL-SDR: could not set the sampling rate", "RTL-SDR: nao foi possivel ajustar a taxa de amostragem"));
        return;
    }
    logf_("sampling at %u Hz, decimation %d in %d stages -> %d Hz to the Skimmer, about %.1f bits",
          rtlsdr_get_sample_rate(dev), rate / target, g_nstages, target,
          8.0 + 0.5 * log((double)(rate / target)) / log(2.0));

    g_block = (int)(target / BLOCKS_PER_SEC);
    free_buffers();
    g_buf[0] = (Cmplx *)_aligned_malloc(g_block * sizeof(Cmplx), 16);
    g_buf[1] = (Cmplx *)_aligned_malloc(g_block * sizeof(Cmplx), 16);
    if (!g_buf[0] || !g_buf[1]) {
        rtlsdr_close(dev);
        report_error(TR("RTL-SDR: out of memory", "RTL-SDR: memoria insuficiente"));
        return;
    }
    memset(g_buf[0], 0, g_block * sizeof(Cmplx));
    memset(g_buf[1], 0, g_block * sizeof(Cmplx));
    g_ptrs[0] = g_buf[0];
    for (i = 1; i < MAX_RX_COUNT; i++) g_ptrs[i] = g_buf[1];   /* unused receivers */
    g_fill = 0;
    g_dc_re = g_dc_im = 0.0f;
    g_peak[0] = g_peak[1] = 0;

    apply_settings(dev);

    EnterCriticalSection(&g_cs);
    g_dev = dev;
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_running, 1);

    g_reader = CreateThread(NULL, 0, reader_thread, dev, 0, NULL);
    if (!g_reader) {
        InterlockedExchange(&g_running, 0);
        EnterCriticalSection(&g_cs);
        g_dev = NULL;
        LeaveCriticalSection(&g_cs);
        rtlsdr_close(dev);
        report_error(TR("RTL-SDR: could not start reception", "RTL-SDR: falha ao iniciar a recepcao"));
        return;
    }
    logf_("receiving, block = %d samples", g_block);

    gen = InterlockedIncrement(&g_gen);
    h = CreateThread(NULL, 0, watch_thread, (LPVOID)(LONG_PTR)gen, 0, NULL);
    if (h) CloseHandle(h);
}

__declspec(dllexport) void __stdcall StopRx(void)
{
    CleanupArg *c;

    InterlockedExchange(&g_running, 0);
    InterlockedIncrement(&g_gen);
    c = (CleanupArg *)malloc(sizeof *c);
    EnterCriticalSection(&g_cs);
    if (c) { c->dev = g_dev; c->reader = g_reader; }
    if (!c || !g_dev) { LeaveCriticalSection(&g_cs); free(c); return; }
    g_dev = NULL;
    g_reader = NULL;
    LeaveCriticalSection(&g_cs);
    logf_("StopRx");

    /* Close in another thread: cancelling waits for the sample thread, which
       may be inside IqProc. This way the Skimmer never hangs here. */
    wait_cleanup(0);
    g_cleanup = CreateThread(NULL, 0, cleanup_thread, c, 0, NULL);
    if (!g_cleanup) cleanup_thread(c);
}

__declspec(dllexport) void __stdcall SetRxFrequency(int Frequency, int Receiver)
{
    if (Receiver != 0) return;
    g_freq = Frequency;
    EnterCriticalSection(&g_cs);
    if (g_dev) rtlsdr_set_center_freq(g_dev, (uint32_t)(Frequency + cfg_offset));
    LeaveCriticalSection(&g_cs);
    logf_("SetRxFrequency %d Hz", Frequency);
}

__declspec(dllexport) void __stdcall SetCtrlBits(unsigned char Bits)
{
    (void)Bits;
}

__declspec(dllexport) int __stdcall ReadPort(int PortNumber)
{
    (void)PortNumber;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    char *p, base[MAX_PATH];
    int i;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        g_pt = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
        for (i = 0; i < 256; i++) g_lut[i] = ((float)i - 127.5f) / 127.5f;

        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        p = strrchr(g_dir, '\\');
        /* .ini named after the DLL (one copy per dongle); falls back to RtlSdrIntf.ini */
        snprintf(base, sizeof base, "%s", p ? p + 1 : g_dir);
        if (p) p[1] = 0; else g_dir[0] = 0;
        p = strrchr(base, '.');
        if (p) *p = 0;
        snprintf(g_ini, sizeof g_ini, "%s%s.ini", g_dir, base);
        if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES)
            snprintf(g_ini, sizeof g_ini, "%sRtlSdrIntf.ini", g_dir);
        snprintf(g_logfile, sizeof g_logfile, "%sRtlSdrIntf.log", g_dir);

        load_config();
        if (cfg_serial[0])               /* tells the dongles apart in the Skimmer's list */
            snprintf(g_name, sizeof g_name, "RTL-SDR %s", cfg_serial);
    }
    return TRUE;
}
