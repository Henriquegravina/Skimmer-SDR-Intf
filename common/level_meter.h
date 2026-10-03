#ifndef LEVEL_METER_H
#define LEVEL_METER_H
/*
 * Peak level meters for the settings windows, so the user can see how close
 * the radio's samples come to full scale and lower the gain before they clip.
 *
 * The sample thread folds the peak of each transfer into a shared value with
 * meter_peak_merge(); the window takes it (and resets it) with
 * meter_peak_take() a few times per second and feeds it to a LevelMeter.
 */
#include <windows.h>
#include <string.h>

#define METER_MIN_DB  (-60.0f)     /* left end of the bar */
#define METER_CLIP    0.995f       /* linear peak counted as clipping */

typedef struct {
    float shown;                   /* dBFS shown by the bar (falls back slowly) */
    float hold;                    /* highest peak of the last seconds, in dBFS */
    DWORD hold_until, clip_until;
    int   live;                    /* 0 = no data (radio stopped) */
    int   clip;                    /* CLIP indicator lit */
} LevelMeter;

/* Peaks are stored as the bits of a non-negative float, which order the same
   way as the integers, so a compare-and-swap keeps the largest one. */
static __inline void meter_peak_merge(volatile LONG *p, float v)
{
    LONG nv, old;
    memcpy(&nv, &v, sizeof nv);
    while ((old = *p) < nv && InterlockedCompareExchange(p, nv, old) != old)
        ;
}

static __inline float meter_peak_take(volatile LONG *p)
{
    LONG b = InterlockedExchange(p, 0);
    float v;
    memcpy(&v, &b, sizeof v);
    return v;
}

void meter_reset(LevelMeter *m);
void meter_feed(LevelMeter *m, float peak, DWORD now);    /* peak < 0: no data */
void meter_draw(const LevelMeter *m, const wchar_t *label, const DRAWITEMSTRUCT *di);
int  meter_poll(LevelMeter m[2], int (*read_peaks)(float peak[2]), HWND meter_i, HWND meter_q);

#endif
