#include "overlay_fx.h"
#include <SDL2/SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

std::vector<OverlayFx> gOverlays;
int gOverlaySel = -1;

void OverlayClear() {
    gOverlays.clear();
    gOverlaySel = -1;
}

const char* OverlayKindName(int kind) {
    if (kind == 1) return "Fog";
    if (kind == 2) return "Fire";
    return "Dirt";
}

void OverlayAdd(int kind, float start) {
    if (start < 0) start = 0;
    OverlayFx o;
    o.kind = kind;
    o.start = start;
    o.duration = 4.0f;
    o.opacity = 0.85f;
    o.amount = 0.75f;
    if (kind == 0) { o.scale = 2.3f; o.y = 0.0f; o.opacity = 0.72f; }
    else if (kind == 1) { o.scale = 2.1f; o.y = 0.0f; o.opacity = 0.55f; o.amount = 0.65f; }
    else { o.scale = 0.85f; o.y = 0.42f; o.opacity = 0.92f; o.amount = 0.8f; }
    gOverlays.push_back(o);
    gOverlaySel = (int)gOverlays.size() - 1;
}

void OverlayBox(const OverlayFx& o, float x0, float y0, float x1, float y1, float& ax, float& ay, float& bx, float& by) {
    float fw = std::max(1.0f, x1 - x0);
    float fh = std::max(1.0f, y1 - y0);
    float cx = (x0 + x1) * 0.5f + o.x * fw * 0.5f;
    float cy = (y0 + y1) * 0.5f + o.y * fh * 0.5f;
    float hw = fw * 0.22f * std::max(0.15f, o.scale);
    float hh = fh * 0.28f * std::max(0.15f, o.scale);
    ax = cx - hw;
    ay = cy - hh;
    bx = cx + hw;
    by = cy + hh;
}

static unsigned OvHash(unsigned n) {
    n ^= n << 13;
    n ^= n >> 17;
    n *= 0x85ebca6bu;
    n ^= n >> 13;
    return n;
}

static float OvU(unsigned n) { return (float)(n & 0xFFFFFF) / (float)0xFFFFFF; }

struct OvBit {
    float lx, ly, ex, ey;
    float r, g, b, a;
    int mode;
};

static int BuildBits(const OverlayFx& o, float localT, OvBit* bits, int cap) {
    int n = 0;
    float amt = std::max(0.05f, std::min(1.0f, o.amount));
    float op = std::max(0.0f, std::min(1.0f, o.opacity));
    if (o.kind == 0) {
        unsigned bucket = (unsigned)(localT * 16.0f);
        int count = 5 + (int)(amt * 7.0f);
        for (int i = 0; i < count && n < cap; i++) {
            unsigned h = OvHash(bucket * 131u + (unsigned)i * 17u + 9u);
            OvBit b;
            b.lx = OvU(h) * 2.0f - 1.0f;
            b.ly = OvU(OvHash(h)) * 2.0f - 1.0f;
            bool line = (h & 3u) != 0u;
            b.mode = line ? 1 : 0;
            float len = (0.08f + OvU(OvHash(h + 3u)) * 0.45f) * amt;
            float ang = OvU(OvHash(h + 7u)) * 6.2831853f;
            b.ex = std::cos(ang) * len;
            b.ey = std::sin(ang) * len * 0.35f;
            float shade = 0.72f + OvU(OvHash(h + 11u)) * 0.28f;
            b.r = b.g = b.b = shade;
            b.a = (0.45f + OvU(OvHash(h + 13u)) * 0.55f) * op;
            if ((h & 16u) == 0u) { b.r *= 0.75f; b.g *= 0.7f; b.b *= 0.55f; }
            bits[n++] = b;
        }
    } else if (o.kind == 1) {
        int count = 4;
        for (int i = 0; i < count && n < cap; i++) {
            OvBit b;
            float drift = localT * (0.07f + i * 0.02f);
            b.lx = std::sin(drift + i * 1.7f) * 0.35f;
            b.ly = std::cos(drift * 0.8f + i) * 0.28f;
            b.ex = 0.55f + 0.15f * i;
            b.ey = b.ex;
            b.mode = 2;
            b.r = 0.78f; b.g = 0.80f; b.b = 0.82f;
            b.a = (0.10f + amt * 0.16f) * op;
            bits[n++] = b;
        }
    } else {
        int count = 8 + (int)(amt * 6.0f);
        for (int i = 0; i < count && n < cap; i++) {
            OvBit b;
            float u = (i + 0.5f) / (float)count;
            b.lx = (u * 2.0f - 1.0f) * 0.85f;
            b.ly = 0.72f;
            float flick = 0.55f + 0.45f * std::sin(localT * 18.0f + i * 1.3f);
            b.ex = (0.08f + 0.05f * std::sin(localT * 9.0f + i)) * amt;
            b.ey = (0.45f + 0.55f * flick) * (0.65f + amt * 0.5f);
            b.mode = 3;
            b.r = 1.0f;
            b.g = 0.35f + 0.4f * flick;
            b.b = 0.05f;
            b.a = (0.55f + 0.4f * flick) * op;
            bits[n++] = b;
        }
    }
    return n;
}

static void Rot(float cs, float sn, float cx, float cy, float lx, float ly, float hw, float hh, float& ox, float& oy) {
    float px = lx * hw;
    float py = ly * hh;
    ox = cx + px * cs - py * sn;
    oy = cy + px * sn + py * cs;
}

void OverlayDrawGL(float x0, float y0, float x1, float y1, float time, int screenW, int screenH) {
    if (gOverlays.empty() || x1 - x0 < 4.0f || y1 - y0 < 4.0f || screenW < 2 || screenH < 2) return;
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, screenW, screenH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    int sx = (int)std::floor(x0);
    int sy = (int)std::floor((float)screenH - y1);
    int sw = std::max(1, (int)std::ceil(x1 - x0));
    int sh = std::max(1, (int)std::ceil(y1 - y0));
    glScissor(sx, sy, sw, sh);
    glEnable(GL_BLEND);
    for (int i = 0; i < (int)gOverlays.size(); i++) {
        const OverlayFx& o = gOverlays[i];
        if (time < o.start || time >= o.start + o.duration) continue;
        float ax, ay, bx, by;
        OverlayBox(o, x0, y0, x1, y1, ax, ay, bx, by);
        float cx = (ax + bx) * 0.5f;
        float cy = (ay + by) * 0.5f;
        float hw = (bx - ax) * 0.5f;
        float hh = (by - ay) * 0.5f;
        float rad = o.rot * 0.01745329252f;
        float cs = std::cos(rad), sn = std::sin(rad);
        OvBit bits[24];
        int n = BuildBits(o, time - o.start, bits, 24);
        if (o.kind == 2) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int k = 0; k < n; k++) {
            const OvBit& b = bits[k];
            glColor4f(b.r, b.g, b.b, b.a);
            if (b.mode == 0) {
                float px, py;
                Rot(cs, sn, cx, cy, b.lx, b.ly, hw, hh, px, py);
                float s = std::max(1.5f, hw * 0.03f);
                glBegin(GL_QUADS);
                glVertex2f(px - s, py - s); glVertex2f(px + s, py - s);
                glVertex2f(px + s, py + s); glVertex2f(px - s, py + s);
                glEnd();
            } else if (b.mode == 1) {
                float px, py, qx, qy;
                Rot(cs, sn, cx, cy, b.lx, b.ly, hw, hh, px, py);
                Rot(cs, sn, cx, cy, b.lx + b.ex, b.ly + b.ey, hw, hh, qx, qy);
                float dx = qy - py, dy = px - qx;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len < 0.001f) continue;
                float w = std::max(1.0f, hw * 0.012f);
                dx = dx / len * w; dy = dy / len * w;
                glBegin(GL_QUADS);
                glVertex2f(px - dx, py - dy); glVertex2f(px + dx, py + dy);
                glVertex2f(qx + dx, qy + dy); glVertex2f(qx - dx, qy - dy);
                glEnd();
            } else if (b.mode == 2) {
                float px, py;
                Rot(cs, sn, cx, cy, b.lx, b.ly, hw, hh, px, py);
                float rx = hw * b.ex;
                float ry = hh * b.ey;
                glBegin(GL_TRIANGLE_FAN);
                glColor4f(b.r, b.g, b.b, b.a);
                glVertex2f(px, py);
                glColor4f(b.r, b.g, b.b, 0.0f);
                for (int s = 0; s <= 16; s++) {
                    float a = s / 16.0f * 6.2831853f;
                    float vx, vy;
                    Rot(cs, sn, px, py, std::cos(a) * (rx / std::max(1.0f, hw)), std::sin(a) * (ry / std::max(1.0f, hh)), hw, hh, vx, vy);
                    glVertex2f(vx, vy);
                }
                glEnd();
            } else {
                float baseX, baseY, tipX, tipY;
                Rot(cs, sn, cx, cy, b.lx, b.ly, hw, hh, baseX, baseY);
                Rot(cs, sn, cx, cy, b.lx, b.ly - b.ey * 2.0f, hw, hh, tipX, tipY);
                float side = std::max(2.0f, hw * b.ex);
                glBegin(GL_TRIANGLES);
                glColor4f(1.0f, 0.95f, 0.6f, b.a);
                glVertex2f(tipX, tipY);
                glColor4f(b.r, b.g * 0.4f, 0.0f, 0.0f);
                glVertex2f(baseX - side, baseY);
                glVertex2f(baseX + side, baseY);
                glEnd();
            }
        }
        if (i == gOverlaySel) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(1, 1, 1, 0.95f);
            float c0x, c0y, c1x, c1y, c2x, c2y, c3x, c3y;
            Rot(cs, sn, cx, cy, -1, -1, hw, hh, c0x, c0y);
            Rot(cs, sn, cx, cy, 1, -1, hw, hh, c1x, c1y);
            Rot(cs, sn, cx, cy, 1, 1, hw, hh, c2x, c2y);
            Rot(cs, sn, cx, cy, -1, 1, hw, hh, c3x, c3y);
            glBegin(GL_LINE_LOOP);
            glVertex2f(c0x, c0y); glVertex2f(c1x, c1y); glVertex2f(c2x, c2y); glVertex2f(c3x, c3y);
            glEnd();
            float hx, hy, sxh, syh;
            Rot(cs, sn, cx, cy, 0, -1, hw, hh, hx, hy);
            hy -= 16.0f;
            glBegin(GL_LINE_LOOP);
            glVertex2f(hx - 5, hy - 5); glVertex2f(hx + 5, hy - 5); glVertex2f(hx + 5, hy + 5); glVertex2f(hx - 5, hy + 5);
            glEnd();
            Rot(cs, sn, cx, cy, 1, 1, hw, hh, sxh, syh);
            glBegin(GL_QUADS);
            glVertex2f(sxh - 5, syh - 5); glVertex2f(sxh + 5, syh - 5); glVertex2f(sxh + 5, syh + 5); glVertex2f(sxh - 5, syh + 5);
            glEnd();
        }
    }
    glColor4f(1, 1, 1, 1);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

static void BlendPix(unsigned char* d, float r, float g, float b, float a, bool add) {
    if (a < 0.004f) return;
    if (a > 1.0f) a = 1.0f;
    if (add) {
        int rr = (int)d[0] + (int)(r * a * 255.0f);
        int gg = (int)d[1] + (int)(g * a * 255.0f);
        int bb = (int)d[2] + (int)(b * a * 255.0f);
        d[0] = (unsigned char)(rr > 255 ? 255 : rr);
        d[1] = (unsigned char)(gg > 255 ? 255 : gg);
        d[2] = (unsigned char)(bb > 255 ? 255 : bb);
        d[3] = 255;
        return;
    }
    float ia = 1.0f - a;
    d[0] = (unsigned char)(r * a * 255.0f + d[0] * ia);
    d[1] = (unsigned char)(g * a * 255.0f + d[1] * ia);
    d[2] = (unsigned char)(b * a * 255.0f + d[2] * ia);
    d[3] = 255;
}

void OverlayStamp(unsigned char* canvas, int w, int h, float time) {
    if (!canvas || w < 2 || h < 2) return;
    for (const OverlayFx& o : gOverlays) {
        if (time < o.start || time >= o.start + o.duration) continue;
        float ax, ay, bx, by;
        OverlayBox(o, 0, 0, (float)w, (float)h, ax, ay, bx, by);
        float cx = (ax + bx) * 0.5f;
        float cy = (ay + by) * 0.5f;
        float hw = std::max(1.0f, (bx - ax) * 0.5f);
        float hh = std::max(1.0f, (by - ay) * 0.5f);
        float rad = o.rot * 0.01745329252f;
        float cs = std::cos(rad), sn = std::sin(rad);
        OvBit bits[24];
        int n = BuildBits(o, time - o.start, bits, 24);
        bool add = o.kind == 2;
        float ext = std::sqrt(hw * hw + hh * hh) + 2.0f;
        int x0 = std::max(0, (int)std::floor(cx - ext));
        int y0 = std::max(0, (int)std::floor(cy - ext));
        int x1 = std::min(w, (int)std::ceil(cx + ext));
        int y1 = std::min(h, (int)std::ceil(cy + ext));
        if (o.kind == 0) {
            for (int k = 0; k < n; k++) {
                const OvBit& b = bits[k];
                float px, py, qx, qy;
                Rot(cs, sn, cx, cy, b.lx, b.ly, hw, hh, px, py);
                if (b.mode == 0) {
                    int rads = std::max(1, (int)(hw * 0.02f));
                    for (int yy = (int)py - rads; yy <= (int)py + rads; yy++) {
                        if (yy < 0 || yy >= h) continue;
                        for (int xx = (int)px - rads; xx <= (int)px + rads; xx++) {
                            if (xx < 0 || xx >= w) continue;
                            BlendPix(canvas + ((size_t)yy * w + xx) * 4, b.r, b.g, b.b, b.a, false);
                        }
                    }
                } else {
                    Rot(cs, sn, cx, cy, b.lx + b.ex, b.ly + b.ey, hw, hh, qx, qy);
                    int steps = std::max(2, (int)std::sqrt((qx - px) * (qx - px) + (qy - py) * (qy - py)));
                    int thick = std::max(1, (int)(hw * 0.01f));
                    for (int s = 0; s <= steps; s++) {
                        float t = s / (float)steps;
                        int xx = (int)(px + (qx - px) * t);
                        int yy = (int)(py + (qy - py) * t);
                        for (int oy = -thick; oy <= thick; oy++) {
                            int yb = yy + oy;
                            if (yb < 0 || yb >= h) continue;
                            for (int ox = -thick; ox <= thick; ox++) {
                                int xb = xx + ox;
                                if (xb < 0 || xb >= w) continue;
                                BlendPix(canvas + ((size_t)yb * w + xb) * 4, b.r, b.g, b.b, b.a, false);
                            }
                        }
                    }
                }
            }
            continue;
        }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                float dx = (x + 0.5f) - cx;
                float dy = (y + 0.5f) - cy;
                float lx = (dx * cs + dy * sn) / hw;
                float ly = (-dx * sn + dy * cs) / hh;
                float r = 0, g = 0, b = 0, a = 0;
                for (int k = 0; k < n; k++) {
                    const OvBit& bit = bits[k];
                    if (bit.mode == 2) {
                        float ddx = (lx - bit.lx) / std::max(0.05f, bit.ex);
                        float ddy = (ly - bit.ly) / std::max(0.05f, bit.ey);
                        float d2 = ddx * ddx + ddy * ddy;
                        if (d2 > 1.0f) continue;
                        float fall = (1.0f - d2);
                        fall *= fall;
                        a += bit.a * fall;
                        r += bit.r * bit.a * fall;
                        g += bit.g * bit.a * fall;
                        b += bit.b * bit.a * fall;
                    } else if (bit.mode == 3) {
                        float dxn = (lx - bit.lx) / std::max(0.02f, bit.ex);
                        if (dxn < -1.0f || dxn > 1.0f) continue;
                        float top = bit.ly - bit.ey * 2.0f;
                        if (ly > bit.ly || ly < top) continue;
                        float v = (bit.ly - ly) / std::max(0.001f, bit.ly - top);
                        float side = 1.0f - std::fabs(dxn);
                        side *= side;
                        float fa = bit.a * v * side;
                        a += fa;
                        r += 1.0f * fa;
                        g += (0.45f + 0.5f * v) * fa;
                        b += 0.05f * fa;
                    }
                }
                if (a < 0.004f) continue;
                if (!add && a > 0.001f) { r /= a; g /= a; b /= a; }
                else { r = std::min(1.0f, r); g = std::min(1.0f, g); b = std::min(1.0f, b); }
                if (a > 1.0f) a = 1.0f;
                BlendPix(canvas + ((size_t)y * w + x) * 4, r, g, b, a, add);
            }
        }
    }
}
