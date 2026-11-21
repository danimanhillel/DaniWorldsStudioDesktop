#pragma once
#include <vector>

struct OverlayFx {
    int kind = 0;
    float start = 0.0f;
    float duration = 4.0f;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    float rot = 0.0f;
    float opacity = 1.0f;
    float amount = 0.7f;
    int dir = 1;
};

extern std::vector<OverlayFx> gOverlays;
extern int gOverlaySel;

void OverlayClear();
int OverlayKindCount();
const char* OverlayKindName(int kind);
void OverlayAdd(int kind, float start);
void OverlayBox(const OverlayFx& o, float x0, float y0, float x1, float y1, float& ax, float& ay, float& bx, float& by);
void OverlayDrawGL(float x0, float y0, float x1, float y1, float time, int screenW, int screenH);
void OverlayStamp(unsigned char* canvas, int w, int h, float time);
