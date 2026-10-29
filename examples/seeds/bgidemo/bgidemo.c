// bgidemo.sed - a tour of the seed BGI graphics library (graphics.h).
//
// Draws shapes, a filled pie chart, a polygon and TrueType text straight onto
// the BerryBasiC screen from native code. Load a font first from BASIC or pass
// its name; the demo tries "PHILO.TTF".
//
//   SEED h%, "BGIDEMO.SED"
//   n% = CALL(h%)
//
// Returns the number of primitives drawn, or 0 when there is no framebuffer
// (e.g. the host build, where the graphics services are all no-ops).
#include "seed.h"
#include "graphics.h"

SEED_EXPORT(bgidemo)
{
    (void)argv; (void)argc;
    if (!initgraph()) return 0;                 // no framebuffer on this build

    int W = getmaxx(), H = getmaxy();
    setrgbbkcolor(12, 12, 24);
    cleardevice();

    // Border and a title.
    setcolor(LIGHTGRAY);
    rectangle(4, 4, W - 4, H - 4);

    int drawn = 0;

    // A row of outlined + filled shapes.
    setcolor(YELLOW);        circle(140, 140, 70);                 drawn++;
    setfillstyle(SOLID_FILL, LIGHTRED);  fillellipse(340, 140, 90, 55); drawn++;
    setcolor(LIGHTCYAN);     rectangle(470, 80, 640, 200);         drawn++;
    setfillstyle(SOLID_FILL, LIGHTGREEN); bar(680, 80, 840, 200);  drawn++;
    setcolor(WHITE);         bar3d(880, 80, 1010, 200, 30, 1);     drawn++;

    // A little pie chart.
    int cx = 200, cy = 380, r = 120;
    setfillstyle(SOLID_FILL, LIGHTBLUE);    setcolor(WHITE); pieslice(cx, cy, 0, 120, r);   drawn++;
    setfillstyle(SOLID_FILL, LIGHTMAGENTA);                  pieslice(cx, cy, 120, 220, r); drawn++;
    setfillstyle(SOLID_FILL, YELLOW);                        pieslice(cx, cy, 220, 360, r); drawn++;

    // A filled star polygon.
    int star[] = { 560,300, 610,430, 745,430, 636,512, 678,640,
                   560,560, 442,640, 484,512, 375,430, 510,430 };
    setfillstyle(SOLID_FILL, LIGHTCYAN);
    setcolor(WHITE);
    fillpoly(10, star);                                            drawn++;

    // A sweep of arcs.
    setcolor(LIGHTGREEN);
    for (int a = 0; a < 360; a += 30) { arc(950, 420, a, a + 20, 150); drawn++; }

    // TrueType text, if a font is available.
    int f = loadfont("PHILO.TTF");
    if (f) {
        setrgbcolor(255, 230, 120);
        settextsize(52);
        settextjustify(CENTER_TEXT, TOP_TEXT);
        outtextxy(W / 2, 20, "BerryBasiC BGI for Seeds");
        setfontstyle(0, 1, 0);                 // italic
        setrgbcolor(180, 220, 255);
        settextsize(28);
        outtextxy(W / 2, H - 70, "line - circle - bar - pieslice - poly - text");
        setfontstyle(0, 0, 0);
        drawn += 2;
    }

    return (double)drawn;
}
