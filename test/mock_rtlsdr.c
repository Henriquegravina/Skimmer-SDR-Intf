/* Fake librtlsdr for testing RtlSdrIntf.c without hardware: one "Blog V4"
   dongle that sends a complex tone at +10 kHz in real time and writes every
   control call to mock_rtlsdr.log. Link it instead of the real driver. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "rtl-sdr.h"

static uint32_t rate = 1536000;
static volatile int run;
static void note(const char *what, int v) { FILE *f = fopen("mock_rtlsdr.log", "a"); if (f) { fprintf(f, "%s %d\n", what, v); fclose(f); } }

uint32_t rtlsdr_get_device_count(void) { return 1; }
const char *rtlsdr_get_device_name(uint32_t i) { (void)i; return "Generic RTL2832U"; }
int rtlsdr_get_device_usb_strings(uint32_t i, char *m, char *p, char *s) { (void)i; strcpy(m, "RTLSDRBlog"); strcpy(p, "Blog V4"); strcpy(s, "00000001"); return 0; }
int rtlsdr_open(rtlsdr_dev_t **d, uint32_t i) { *d = (rtlsdr_dev_t *)1; note("open", (int)i); return 0; }
int rtlsdr_close(rtlsdr_dev_t *d) { (void)d; note("close", 0); return 0; }
int rtlsdr_set_center_freq(rtlsdr_dev_t *d, uint32_t f) { (void)d; note("freq", (int)f); return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t *d, int ppm) { (void)d; note("ppm", ppm); return 0; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t *d, int *g) { static const int t[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 }; (void)d; if (g) memcpy(g, t, sizeof t); return 29; }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *d, int g) { (void)d; note("tuner_gain", g); return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *d, int m) { (void)d; note("tuner_manual", m); return 0; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t *d, uint32_t r) { (void)d; rate = r; note("rate", (int)r); return 0; }
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *d) { (void)d; return rate; }
int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *d, uint32_t bw) { (void)d; note("tuner_bw", (int)bw); return 0; }
int rtlsdr_set_agc_mode(rtlsdr_dev_t *d, int on) { (void)d; note("rtl_agc", on); return 0; }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t *d, int on) { (void)d; note("direct_sampling", on); return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t *d, int on) { (void)d; note("bias_tee", on); return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t *d) { (void)d; return 0; }
int rtlsdr_cancel_async(rtlsdr_dev_t *d) { (void)d; run = 0; return 0; }

int rtlsdr_read_async(rtlsdr_dev_t *d, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len)
{
    static unsigned char buf[65536], tone[2 * 1536];
    double sent = 0.0;
    LARGE_INTEGER f, t0, t;
    uint32_t i, k = 0, n = rate / 2000;   /* five periods of 10 kHz: a whole number of samples */
    (void)d; (void)buf_num;
    if (buf_len == 0 || buf_len > sizeof buf) buf_len = 16384;
    /* Amplitude 0.5 of full scale plus a DC offset. A table keeps the mock's
       own CPU use low. */
    if (n == 0 || n > 1536) n = 768;
    for (i = 0; i < n; i++) {
        tone[2 * i]     = (unsigned char)(127.5 + 6.0 + 63.75 * cos(2 * M_PI * 10000.0 * i / rate) + 0.5);
        tone[2 * i + 1] = (unsigned char)(127.5 - 4.0 + 63.75 * sin(2 * M_PI * 10000.0 * i / rate) + 0.5);
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    run = 1;
    while (run) {
        for (i = 0; i < buf_len; i += 2) {
            buf[i] = tone[2 * k];
            buf[i + 1] = tone[2 * k + 1];
            if (++k == n) k = 0;
        }
        cb(buf, buf_len, ctx);
        sent += buf_len / 2;
        for (;;) {
            QueryPerformanceCounter(&t);
            if ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart >= sent / rate) break;
            Sleep(1);
        }
    }
    return 0;
}
