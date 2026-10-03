/*
 * Peak level meter drawn in an owner-drawn static control: a label, a bar
 * from -60 to 0 dBFS (green, yellow above -12, red above -3) with a peak-hold
 * mark, and the held peak in dBFS, or CLIP in red.
 */
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include "level_meter.h"

#define YELLOW_DB  (-12.0f)
#define RED_DB     (-3.0f)
#define FALL_DB    2.0f            /* per update (the window updates every 100 ms) */
#define HOLD_MS    2000
#define CLIP_MS    3000

void meter_reset(LevelMeter *m)
{
    m->shown = m->hold = METER_MIN_DB;
    m->hold_until = m->clip_until = 0;
    m->live = m->clip = 0;
}

void meter_feed(LevelMeter *m, float peak, DWORD now)
{
    float db;
    if (peak < 0.0f) { meter_reset(m); return; }
    db = peak > 1e-6f ? 20.0f * log10f(peak) : -120.0f;
    if (db > 0.0f) db = 0.0f;
    m->live = 1;
    m->shown = db > m->shown - FALL_DB ? db : m->shown - FALL_DB;
    if (m->shown < METER_MIN_DB) m->shown = METER_MIN_DB;
    if (db >= m->hold || (LONG)(now - m->hold_until) >= 0) {
        m->hold = db;
        m->hold_until = now + HOLD_MS;
    }
    if (peak >= METER_CLIP) m->clip_until = now + CLIP_MS;
    m->clip = (LONG)(m->clip_until - now) > 0;
}

static int x_of(float db, int x0, int x1)
{
    if (db < METER_MIN_DB) db = METER_MIN_DB;
    if (db > 0.0f) db = 0.0f;
    return x0 + (int)((db - METER_MIN_DB) / -METER_MIN_DB * (x1 - x0) + 0.5f);
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b;
    if (x1 <= x0) return;
    SetRect(&r, x0, y0, x1, y1);
    b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void meter_draw(const LevelMeter *m, const wchar_t *label, const DRAWITEMSTRUCT *di)
{
    static const float zone_db[4] = { METER_MIN_DB, YELLOW_DB, RED_DB, 0.0f };
    static const COLORREF lit[3] = { RGB(40, 190, 70), RGB(235, 200, 40), RGB(230, 50, 40) };
    static const COLORREF dim[3] = { RGB(30, 60, 36), RGB(70, 64, 28), RGB(72, 32, 30) };
    int w = di->rcItem.right - di->rcItem.left, h = di->rcItem.bottom - di->rcItem.top;
    HDC dc = CreateCompatibleDC(di->hDC);             /* drawn off screen: no flicker */
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h), old_bmp;
    RECT rc = { 0, 0, w, h }, r;
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0), old_font;
    SIZE sz;
    wchar_t txt[32];
    int lw, tw, bx0, bx1, by0, by1, i, x;

    old_bmp = (HBITMAP)SelectObject(dc, bmp);
    old_font = (HFONT)SelectObject(dc, font);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);

    GetTextExtentPoint32W(dc, L"Q  ", 3, &sz);
    lw = sz.cx;
    GetTextExtentPoint32W(dc, L"-60 dBFS ", 9, &sz);
    tw = sz.cx;
    bx0 = rc.left + lw;
    bx1 = rc.right - tw;
    by0 = rc.top + 1;
    by1 = rc.bottom - 1;

    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SetRect(&r, rc.left, rc.top, bx0, rc.bottom);
    DrawTextW(dc, label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* the three colour zones, lit up to the current level */
    fill(dc, bx0, by0, bx1, by1, RGB(24, 24, 24));
    for (i = 0; i < 3; i++) {
        int z0 = x_of(zone_db[i], bx0, bx1), z1 = x_of(zone_db[i + 1], bx0, bx1);
        int lv = m->live ? x_of(m->shown, bx0, bx1) : bx0;
        fill(dc, z0 + 1, by0 + 1, z1 - 1, by1 - 1, dim[i]);
        fill(dc, z0 + 1, by0 + 1, (lv < z1 - 1 ? lv : z1 - 1), by1 - 1, lit[i]);
    }
    if (m->live && m->hold > METER_MIN_DB) {           /* peak-hold mark */
        x = x_of(m->hold, bx0, bx1);
        fill(dc, x - 1, by0, x + 1, by1, RGB(255, 255, 255));
    }

    if (!m->live) {
        wcscpy(txt, L"--");
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    } else if (m->clip) {
        wcscpy(txt, L"CLIP");
        SetTextColor(dc, RGB(220, 0, 0));
    } else {
        swprintf(txt, 32, L"%d dBFS", (int)floorf(m->hold + 0.5f));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    }
    SetRect(&r, bx1, rc.top, rc.right, rc.bottom);
    DrawTextW(dc, txt, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    BitBlt(di->hDC, di->rcItem.left, di->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* Takes the peaks gathered since the last call and redraws both meters.
   Returns 1 while the radio is receiving. */
int meter_poll(LevelMeter m[2], int (*read_peaks)(float peak[2]), HWND meter_i, HWND meter_q)
{
    float pk[2];
    DWORD now = GetTickCount();
    int live = read_peaks && read_peaks(pk);
    meter_feed(&m[0], live ? pk[0] : -1.0f, now);
    meter_feed(&m[1], live ? pk[1] : -1.0f, now);
    InvalidateRect(meter_i, NULL, FALSE);
    InvalidateRect(meter_q, NULL, FALSE);
    return live;
}
