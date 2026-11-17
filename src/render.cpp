// Drawing of the two title bar buttons (per-pixel alpha, any DPI).
// License: GPL-3.0-or-later.
#include "common.h"
#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

static ULONG_PTR g_gdipToken = 0;

void RenderInit()
{
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdipToken, &in, nullptr);
}

void RenderShutdown()
{
    if (g_gdipToken) GdiplusShutdown(g_gdipToken);
    g_gdipToken = 0;
}

// Glyphs are designed in a 16x16 box whose origin is (ox, oy); s = scale.
static void DrawPin(Graphics& g, float ox, float oy, float s, Color fg, bool pinned)
{
    GraphicsState st = g.Save();
    g.TranslateTransform(ox + 8 * s, oy + 8 * s);
    if (!pinned) g.RotateTransform(45.0f); // a loose pin lies on its side
    g.ScaleTransform(s, s);
    g.TranslateTransform(-8, -8);

    PointF body[] = { {5.5f, 2.0f}, {10.5f, 2.0f}, {10.0f, 6.5f}, {12.5f, 9.5f}, {3.5f, 9.5f}, {6.0f, 6.5f} };
    Pen pen(fg, 1.2f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    if (pinned) {
        SolidBrush br(fg);
        g.FillPolygon(&br, body, 6);
    }
    g.DrawPolygon(&pen, body, 6);
    g.DrawLine(&pen, 8.0f, 9.5f, 8.0f, 14.5f);
    g.Restore(st);
}

static void DrawRoll(Graphics& g, float ox, float oy, float s, Color fg, bool rolled)
{
    GraphicsState st = g.Save();
    g.TranslateTransform(ox, oy);
    g.ScaleTransform(s, s);
    Pen bar(fg, 1.8f);
    bar.SetStartCap(LineCapRound);
    bar.SetEndCap(LineCapRound);
    g.DrawLine(&bar, 3.0f, 3.5f, 13.0f, 3.5f); // the title bar that stays
    Pen pen(fg, 1.2f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    if (rolled) { // chevron down: roll the window back down
        PointF p[] = { {4.5f, 8.0f}, {8.0f, 11.5f}, {11.5f, 8.0f} };
        g.DrawLines(&pen, p, 3);
    } else {      // chevron up: roll the window up
        PointF p[] = { {4.5f, 11.5f}, {8.0f, 8.0f}, {11.5f, 11.5f} };
        g.DrawLines(&pen, p, 3);
    }
    g.Restore(st);
}

void RenderBar(HWND bar, const RECT& rc, const ButtonDraw* btns, int count, UINT dpi, bool dark)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || count <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetCompositingMode(CompositingModeSourceCopy);
        // Alpha 1: invisible, but it makes the whole bar receive mouse input.
        SolidBrush clear(Color(1, 0, 0, 0));
        g.FillRectangle(&clear, 0, 0, w, h);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        BYTE c = dark ? 255 : 0;
        Color fg(255, dark ? 255 : 27, dark ? 255 : 27, dark ? 255 : 27);
        float s = dpi / 96.0f;
        float bw = (float)w / count;
        for (int i = 0; i < count; i++) {
            float x = bw * i;
            int a = 0;
            if (btns[i].type == BTN_PIN && btns[i].active) a = dark ? 40 : 28;
            if (btns[i].hot) a = dark ? 56 : 36;
            if (btns[i].pressed) a = dark ? 84 : 60;
            if (a) {
                SolidBrush hb(Color((BYTE)a, c, c, c));
                g.FillRectangle(&hb, x, 0.0f, bw, (float)h);
            }
            float ox = x + (bw - 16 * s) / 2, oy = (h - 16 * s) / 2;
            if (btns[i].type == BTN_PIN) DrawPin(g, ox, oy, s, fg, btns[i].active);
            else DrawRoll(g, ox, oy, s, fg, btns[i].active);
        }
    }
    POINT dst = { rc.left, rc.top }, src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(bar, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

HICON CreateAppIcon(int size)
{
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        SolidBrush bg(Color(255, 0, 103, 192));
        g.FillEllipse(&bg, 0.5f, 0.5f, size - 1.0f, size - 1.0f);
        float s = size / 20.0f;
        DrawPin(g, (size - 16 * s) / 2, (size - 16 * s) / 2, s, Color(255, 255, 255, 255), true);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}
