/*
 * AirspyHfIntf.dll - Airspy HF+ / HF+ Discovery para Skimmer Server e RTTY Skimmer Server
 *
 * Implementa a interface de DLL de radio do VE3NEA (unit SdrTypes):
 *   GetSdrInfo, StartRx, StopRx, SetRxFrequency, SetCtrlBits, ReadPort
 *
 * O radio e acessado pela libairspyhf, embutida (-DSTATIC_AIRSPYHF, veja
 * build.sh) ou carregada de uma airspyhf.dll de 32 bits. O fluxo I/Q do radio
 * (192/384/768 kHz) e decimado por 2^n ate a taxa pedida pelo Skimmer
 * (48/96/192 kHz) e entregue em blocos de taxa/93.75 amostras (512/1024/2048).
 *
 * Os ajustes ficam no .ini e sao relidos com o radio ligado. A janela de
 * configuracao (config/settings_dialog.c) abre quando o Skimmer inicia o
 * radio e grava nesse mesmo arquivo.
 *
 * Compilar (32 bits!): sh build.sh
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include "config/settings_dialog.h"
#include "common/level_meter.h"

/* ------------------------------------------------------------------ */
/* Interface do Skimmer (SdrTypes.pas)                                 */
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
/* API da airspyhf.dll (cdecl), resolvida com GetProcAddress           */
/* ------------------------------------------------------------------ */
typedef struct airspyhf_device airspyhf_device_t;
typedef struct { float re, im; } airspyhf_complex_float_t;
typedef struct {
    airspyhf_device_t *device;
    void *ctx;
    airspyhf_complex_float_t *samples;
    int sample_count;
    uint64_t dropped_samples;
} airspyhf_transfer_t;
typedef int (__cdecl *airspyhf_cb)(airspyhf_transfer_t *);

static struct {
    HMODULE h;
    int (__cdecl *open)(airspyhf_device_t **);
    int (__cdecl *open_sn)(airspyhf_device_t **, uint64_t);
    int (__cdecl *close)(airspyhf_device_t *);
    int (__cdecl *start)(airspyhf_device_t *, airspyhf_cb, void *);
    int (__cdecl *stop)(airspyhf_device_t *);
    int (__cdecl *set_freq)(airspyhf_device_t *, uint32_t);
    int (__cdecl *get_samplerates)(airspyhf_device_t *, uint32_t *, uint32_t);
    int (__cdecl *set_samplerate)(airspyhf_device_t *, uint32_t);
    int (__cdecl *set_hf_agc)(airspyhf_device_t *, uint8_t);
    int (__cdecl *set_hf_agc_threshold)(airspyhf_device_t *, uint8_t);
    int (__cdecl *set_hf_att)(airspyhf_device_t *, uint8_t);
    int (__cdecl *set_hf_lna)(airspyhf_device_t *, uint8_t);
} lib;

/* ------------------------------------------------------------------ */
/* Decimador por 2 (FIR meia-banda, janela de Kaiser)                  */
/* ------------------------------------------------------------------ */
#define NTAPS 63
#define MAX_STAGES 4

typedef struct {
    Cmplx hist[2 * NTAPS];   /* buffer duplicado: janela sempre contigua */
    int   pos;
    int   phase;
} Stage;

static float g_taps[NTAPS];

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

static void design_taps(void)
{
    const double beta = 8.0, pi = 3.14159265358979323846;
    double sum = 0.0, t[NTAPS];
    int i, m = (NTAPS - 1) / 2;
    for (i = 0; i < NTAPS; i++) {
        int n = i - m;
        double s = (n == 0) ? 0.5 : sin(pi * n / 2.0) / (pi * n);   /* corte em fs/4 */
        double r = (double)n / m;
        double w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
        t[i] = s * w;
        sum += t[i];
    }
    for (i = 0; i < NTAPS; i++) g_taps[i] = (float)(t[i] / sum);
}

/* devolve 1 quando *out foi produzido */
static int stage_push(Stage *s, Cmplx in, Cmplx *out)
{
    const Cmplx *w;
    float re = 0.0f, im = 0.0f;
    int i;

    s->hist[s->pos] = in;
    s->hist[s->pos + NTAPS] = in;
    s->pos = (s->pos + 1) % NTAPS;
    s->phase ^= 1;
    if (s->phase) return 0;

    w = &s->hist[s->pos];
    for (i = 0; i < NTAPS; i++) {
        re += w[i].Re * g_taps[i];
        im += w[i].Im * g_taps[i];
    }
    out->Re = re;
    out->Im = im;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Estado global                                                       */
/* ------------------------------------------------------------------ */
static TSdrSettings      g_set;
static airspyhf_device_t *g_dev;
static CRITICAL_SECTION  g_cs;
static volatile LONG     g_running;
static HANDLE            g_cleanup;          /* thread que fecha o radio */

static Stage  g_stage[MAX_STAGES];
static int    g_nstages;
static Cmplx *g_buf[MAX_RX_COUNT];           /* [0] = dados, demais = zeros */
static Cmplx *g_ptrs[MAX_RX_COUNT];
static int    g_block, g_fill;
static float  g_scale, g_qsign;
static int    g_freq = 14050000;
static uint64_t g_dropped;
static volatile LONG g_peak[2];              /* pico de |I| e |Q| para os medidores da janela */

/* configuracao (AirspyHfIntf.ini) */
static char   g_dir[MAX_PATH], g_ini[MAX_PATH], g_logfile[MAX_PATH];
static int    cfg_agc, cfg_agc_thr, cfg_att, cfg_lna, cfg_invq, cfg_log, cfg_offset, cfg_show;
static double cfg_gain_db;
static char   cfg_serial[40];
static char   g_name[64] = "Airspy HF+";
static volatile LONG g_gen;                  /* geracao da thread que monitora o .ini */

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
    logf_("ERRO: %s", msg);
    if (g_set.ErrorProc) g_set.ErrorProc(g_set.RxHandle, (char *)msg);
}

static void load_config(void)
{
    char tmp[32];
    cfg_agc     = GetPrivateProfileIntA("Airspy", "HfAgc", 1, g_ini);
    cfg_agc_thr = GetPrivateProfileIntA("Airspy", "HfAgcThreshold", 0, g_ini);
    cfg_att     = GetPrivateProfileIntA("Airspy", "HfAtt", 0, g_ini);
    cfg_lna     = GetPrivateProfileIntA("Airspy", "HfLna", 0, g_ini);
    cfg_invq    = GetPrivateProfileIntA("Airspy", "InvertQ", 0, g_ini);
    cfg_offset  = GetPrivateProfileIntA("Airspy", "FreqOffsetHz", 0, g_ini);
    cfg_log     = GetPrivateProfileIntA("Airspy", "Log", 0, g_ini);
    cfg_show    = GetPrivateProfileIntA("Airspy", "ShowWindow", 1, g_ini);
    GetPrivateProfileStringA("Airspy", "GainDb", "0", tmp, sizeof tmp, g_ini);
    cfg_gain_db = atof(tmp);
    GetPrivateProfileStringA("Airspy", "Serial", "", cfg_serial, sizeof cfg_serial, g_ini);
}

#ifdef STATIC_AIRSPYHF
/* libairspyhf + libusb ligadas estaticamente: nenhuma DLL externa */
extern int airspyhf_open(airspyhf_device_t **);
extern int airspyhf_open_sn(airspyhf_device_t **, uint64_t);
extern int airspyhf_close(airspyhf_device_t *);
extern int airspyhf_start(airspyhf_device_t *, airspyhf_cb, void *);
extern int airspyhf_stop(airspyhf_device_t *);
extern int airspyhf_set_freq(airspyhf_device_t *, uint32_t);
extern int airspyhf_get_samplerates(airspyhf_device_t *, uint32_t *, uint32_t);
extern int airspyhf_set_samplerate(airspyhf_device_t *, uint32_t);
extern int airspyhf_set_hf_agc(airspyhf_device_t *, uint8_t);
extern int airspyhf_set_hf_agc_threshold(airspyhf_device_t *, uint8_t);
extern int airspyhf_set_hf_att(airspyhf_device_t *, uint8_t);
extern int airspyhf_set_hf_lna(airspyhf_device_t *, uint8_t);

static int load_lib(void)
{
    lib.open = airspyhf_open;
    lib.open_sn = airspyhf_open_sn;
    lib.close = airspyhf_close;
    lib.start = airspyhf_start;
    lib.stop = airspyhf_stop;
    lib.set_freq = airspyhf_set_freq;
    lib.get_samplerates = airspyhf_get_samplerates;
    lib.set_samplerate = airspyhf_set_samplerate;
    lib.set_hf_agc = airspyhf_set_hf_agc;
    lib.set_hf_agc_threshold = airspyhf_set_hf_agc_threshold;
    lib.set_hf_att = airspyhf_set_hf_att;
    lib.set_hf_lna = airspyhf_set_hf_lna;
    return 1;
}
#else
#define RESOLVE(field, name) \
    do { *(FARPROC *)&lib.field = GetProcAddress(lib.h, name); \
         if (!lib.field) { FreeLibrary(lib.h); lib.h = NULL; return 0; } } while (0)

static int load_lib(void)
{
    char path[MAX_PATH];
    if (lib.h) return 1;
    snprintf(path, sizeof path, "%sairspyhf.dll", g_dir);
    lib.h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib.h) lib.h = LoadLibraryA("airspyhf.dll");
    if (!lib.h) return 0;
    RESOLVE(open, "airspyhf_open");
    RESOLVE(open_sn, "airspyhf_open_sn");
    RESOLVE(close, "airspyhf_close");
    RESOLVE(start, "airspyhf_start");
    RESOLVE(stop, "airspyhf_stop");
    RESOLVE(set_freq, "airspyhf_set_freq");
    RESOLVE(get_samplerates, "airspyhf_get_samplerates");
    RESOLVE(set_samplerate, "airspyhf_set_samplerate");
    RESOLVE(set_hf_agc, "airspyhf_set_hf_agc");
    RESOLVE(set_hf_agc_threshold, "airspyhf_set_hf_agc_threshold");
    RESOLVE(set_hf_att, "airspyhf_set_hf_att");
    RESOLVE(set_hf_lna, "airspyhf_set_hf_lna");
    return 1;
}

#endif

/* ------------------------------------------------------------------ */
/* Janela de configuracao, em thread propria para nao travar o Skimmer */
/* ------------------------------------------------------------------ */
static HINSTANCE g_inst;
static HWND      g_ui_hwnd;
static volatile LONG g_ui_running;

#ifdef STATIC_AIRSPYHF
extern int airspyhf_list_devices(uint64_t *, int);
static uint64_t  g_radios[16];               /* radios vistos nesta sessao */
static int       g_nradios;

/* O radio em uso nao aparece numa nova enumeracao, por isso a lista e
   acumulada: StartRx enumera antes de abrir e "Atualizar" soma os novos. */
static void scan_radios(void)
{
    uint64_t now[16];
    int n = airspyhf_list_devices(now, 16), i, k;
    for (i = 0; i < n && i < 16; i++) {
        for (k = 0; k < g_nradios; k++) if (g_radios[k] == now[i]) break;
        if (k == g_nradios && g_nradios < 16) g_radios[g_nradios++] = now[i];
    }
}

static int list_radios(uint64_t *serials, int max)
{
    int i;
    scan_radios();
    for (i = 0; i < g_nradios && i < max; i++) serials[i] = g_radios[i];
    return i;
}
#else
static void scan_radios(void) {}
static int (*const list_radios)(uint64_t *, int) = NULL;   /* airspyhf.dll externa: sem lista */
#endif

/* Picos de I e Q (1.0 = fundo de escala da saida de 16 bits do radio)
   desde a chamada anterior; 0 se o radio nao estiver recebendo */
static int read_peaks(float peak[2])
{
    if (!g_running) return 0;
    peak[0] = meter_peak_take(&g_peak[0]);
    peak[1] = meter_peak_take(&g_peak[1]);
    return 1;
}

static DWORD WINAPI ui_thread(LPVOID arg)
{
    static SettingsCtx ctx;
    (void)arg;
    ctx.ini = g_ini;
    ctx.list_radios = list_radios;
    ctx.hwnd_out = &g_ui_hwnd;
    ctx.lang = -1;
    ctx.read_peaks = read_peaks;
    settings_dialog_run(g_inst, &ctx);
    InterlockedExchange(&g_ui_running, 0);
    return 0;
}

static void show_settings_window(void)
{
    HMODULE pin;
    HANDLE h;
    if (InterlockedCompareExchange(&g_ui_running, 1, 0) != 0) {
        if (g_ui_hwnd) ShowWindow(g_ui_hwnd, SW_SHOWNOACTIVATE);   /* ja aberta */
        return;
    }
    /* mantem a DLL carregada enquanto a janela existir */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)ui_thread, &pin);
    h = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (h) CloseHandle(h); else InterlockedExchange(&g_ui_running, 0);
}

/* Aplica ao radio e ao DSP tudo o que pode mudar com o radio ligado */
static void apply_settings(airspyhf_device_t *dev)
{
    lib.set_hf_agc(dev, cfg_agc ? 1 : 0);
    lib.set_hf_agc_threshold(dev, cfg_agc_thr ? 1 : 0);
    if (!cfg_agc) lib.set_hf_att(dev, (uint8_t)cfg_att);
    lib.set_hf_lna(dev, cfg_lna ? 1 : 0);
    lib.set_freq(dev, (uint32_t)(g_freq + cfg_offset));
    /* airspyhf entrega float em +-1.0; o Skimmer espera escala de inteiro 32 bits */
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

/* Rele o .ini quando ele muda (ex.: gravado pelo AirspyHfIntfConfig.exe) e
   aplica os ajustes sem reiniciar a recepcao. O numero de serie so vale no
   proximo StartRx. */
static DWORD WINAPI watch_thread(LPVOID arg)
{
    LONG gen = (LONG)(LONG_PTR)arg;
    FILETIME last = {0, 0}, now;
    ini_mtime(&last);
    while (g_running && g_gen == gen) {
        Sleep(300);
        if (!ini_mtime(&now) || CompareFileTime(&now, &last) == 0) continue;
        last = now;
        Sleep(100);                      /* deixa o outro programa terminar de gravar */
        EnterCriticalSection(&g_cs);
        if (g_dev && g_gen == gen) {
            load_config();
            apply_settings(g_dev);
            logf_("configuracao recarregada");
        }
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Callback de amostras (thread da airspyhf.dll)                       */
/* ------------------------------------------------------------------ */
static int __cdecl on_samples(airspyhf_transfer_t *t)
{
    float pk_i = 0.0f, pk_q = 0.0f;
    int i, k;

    if (!g_running) return 0;
    if (t->dropped_samples) {
        g_dropped += t->dropped_samples;
        logf_("amostras perdidas: %lu (total %lu)",
              (unsigned long)t->dropped_samples, (unsigned long)g_dropped);
    }

    for (i = 0; i < t->sample_count; i++) {
        Cmplx s;
        s.Re = t->samples[i].re;
        s.Im = t->samples[i].im;
        if (fabsf(s.Re) > pk_i) pk_i = fabsf(s.Re);
        if (fabsf(s.Im) > pk_q) pk_q = fabsf(s.Im);

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
    meter_peak_merge(&g_peak[0], pk_i);
    meter_peak_merge(&g_peak[1], pk_q);
    return 0;
}

static void free_buffers(void)
{
    int i;
    if (g_buf[0]) _aligned_free(g_buf[0]);
    if (g_buf[1]) _aligned_free(g_buf[1]);
    for (i = 0; i < MAX_RX_COUNT; i++) g_buf[i] = NULL;
}

static DWORD WINAPI cleanup_thread(LPVOID arg)
{
    airspyhf_device_t *dev = (airspyhf_device_t *)arg;
    lib.stop(dev);
    lib.close(dev);
    logf_("radio fechado");
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

/* ------------------------------------------------------------------ */
/* Funcoes exportadas                                                  */
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
    uint32_t rates[32], nrates = 0, devrate = 0;
    int target, i, n, r;
    airspyhf_device_t *dev = NULL;

    if (!settings) return;
    if (g_running) return;
    wait_cleanup(3000);

    g_set = *settings;
    g_set.RateID &= 0xFF;   /* Skimmer Server 1.1+ poe lixo nos bytes altos */
    load_config();
    logf_("StartRx RateID=%d RecvCount=%d", g_set.RateID, g_set.RecvCount);

    switch (g_set.RateID) {
        case RATE_48KHZ:  target = 48000;  break;
        case RATE_96KHZ:  target = 96000;  break;
        case RATE_192KHZ: target = 192000; break;
        default: report_error("Airspy HF+: taxa de amostragem desconhecida"); return;
    }
    if (g_set.RecvCount > 1) {
        report_error("Airspy HF+: apenas 1 receptor e suportado");
        return;
    }
    if (!load_lib()) {
        report_error("Airspy HF+: airspyhf.dll (32 bits) nao encontrada ou incompativel");
        return;
    }

    scan_radios();                       /* antes de abrir: depois o radio em uso some da lista */
    if (cfg_show) show_settings_window();

    if (cfg_serial[0])
        r = lib.open_sn(&dev, _strtoui64(cfg_serial, NULL, 16));
    else
        r = lib.open(&dev);
    if (r != 0 || !dev) {
        report_error("Airspy HF+: radio nao encontrado (desconectado, sem driver WinUSB ou em uso por outro programa)");
        return;
    }

    /* menor taxa do radio que seja alvo * 2^n */
    lib.get_samplerates(dev, &nrates, 0);
    if (nrates > 32) nrates = 32;
    lib.get_samplerates(dev, rates, nrates);
    g_nstages = 0;
    for (n = 0; n <= MAX_STAGES && !devrate; n++)
        for (i = 0; i < (int)nrates; i++)
            if (rates[i] == (uint32_t)target << n) { devrate = rates[i]; g_nstages = n; break; }
    if (!devrate || lib.set_samplerate(dev, devrate) != 0) {
        lib.close(dev);
        report_error("Airspy HF+: o radio nao oferece taxa compativel (192/384/768 kHz)");
        return;
    }
    logf_("taxa do radio %u Hz, %d estagio(s) de decimacao -> %d Hz", devrate, g_nstages, target);

    apply_settings(dev);

    /* buffers: bloco de dados + bloco de zeros para os receptores nao usados */
    g_block = (int)(target / BLOCKS_PER_SEC);
    free_buffers();
    g_buf[0] = (Cmplx *)_aligned_malloc(g_block * sizeof(Cmplx), 16);
    g_buf[1] = (Cmplx *)_aligned_malloc(g_block * sizeof(Cmplx), 16);
    if (!g_buf[0] || !g_buf[1]) {
        lib.close(dev);
        report_error("Airspy HF+: memoria insuficiente");
        return;
    }
    memset(g_buf[0], 0, g_block * sizeof(Cmplx));
    memset(g_buf[1], 0, g_block * sizeof(Cmplx));
    g_ptrs[0] = g_buf[0];
    for (i = 1; i < MAX_RX_COUNT; i++) g_ptrs[i] = g_buf[1];

    memset(g_stage, 0, sizeof g_stage);
    g_fill = 0;
    g_dropped = 0;
    g_peak[0] = g_peak[1] = 0;

    EnterCriticalSection(&g_cs);
    g_dev = dev;
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_running, 1);

    if (lib.start(dev, on_samples, NULL) != 0) {
        InterlockedExchange(&g_running, 0);
        EnterCriticalSection(&g_cs);
        g_dev = NULL;
        LeaveCriticalSection(&g_cs);
        lib.close(dev);
        report_error("Airspy HF+: falha ao iniciar a recepcao");
        return;
    }
    logf_("recepcao iniciada, bloco = %d amostras", g_block);

    {   /* monitora o .ini para aplicar mudancas com o radio ligado */
        LONG gen = InterlockedIncrement(&g_gen);
        HANDLE h = CreateThread(NULL, 0, watch_thread, (LPVOID)(LONG_PTR)gen, 0, NULL);
        if (h) CloseHandle(h);
    }
}

__declspec(dllexport) void __stdcall StopRx(void)
{
    airspyhf_device_t *dev;

    InterlockedExchange(&g_running, 0);
    InterlockedIncrement(&g_gen);
    EnterCriticalSection(&g_cs);
    dev = g_dev;
    g_dev = NULL;
    LeaveCriticalSection(&g_cs);
    if (!dev) return;
    logf_("StopRx");

    /* Fecha o radio em outra thread: airspyhf_stop espera a thread de
       amostras, que pode estar dentro de IqProc. Assim o Skimmer nao trava. */
    wait_cleanup(0);
    g_cleanup = CreateThread(NULL, 0, cleanup_thread, dev, 0, NULL);
    if (!g_cleanup) cleanup_thread(dev);
}

__declspec(dllexport) void __stdcall SetRxFrequency(int Frequency, int Receiver)
{
    if (Receiver != 0) return;
    g_freq = Frequency;
    EnterCriticalSection(&g_cs);
    if (g_dev) lib.set_freq(g_dev, (uint32_t)(Frequency + cfg_offset));
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
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        p = strrchr(g_dir, '\\');
        /* .ini com o mesmo nome da DLL (permite uma copia por radio);
           se nao existir, usa AirspyHfIntf.ini */
        snprintf(base, sizeof base, "%s", p ? p + 1 : g_dir);
        if (p) p[1] = 0; else g_dir[0] = 0;
        p = strrchr(base, '.');
        if (p) *p = 0;
        snprintf(g_ini, sizeof g_ini, "%s%s.ini", g_dir, base);
        if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES)
            snprintf(g_ini, sizeof g_ini, "%sAirspyHfIntf.ini", g_dir);
        snprintf(g_logfile, sizeof g_logfile, "%sAirspyHfIntf.log", g_dir);
        design_taps();
        load_config();
        if (cfg_serial[0]) {             /* distingue os radios na lista do Skimmer */
            size_t n = strlen(cfg_serial);
            snprintf(g_name, sizeof g_name, "Airspy HF+ %s", cfg_serial + (n > 8 ? n - 8 : 0));
        }
    }
    return TRUE;
}
