#include "effects.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

class CrossDissolve : public ClipEffect {
public:
    const char* id() const override { return "cross_dissolve"; }
    const char* label() const override { return "Cross Dissolve"; }
    const char* badge() const override { return "XD"; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        float s = p * p * (3.0f - 2.0f * p);
        look.alphaB = s;
        look.scaleA = 1.0f;
        look.scaleB = 1.0f;
    }
};

class BlendFx : public ClipEffect {
public:
    const char* id() const override { return "blend"; }
    const char* label() const override { return "Blend"; }
    const char* badge() const override { return "BL"; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.alphaB = p;
        look.scaleA = 1.0f;
        look.scaleB = 1.0f;
    }
};

class DipBlackFx : public ClipEffect {
public:
    const char* id() const override { return "dip_black"; }
    const char* label() const override { return "Dip to Black"; }
    const char* badge() const override { return "DB"; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.scaleA = 1.0f;
        look.scaleB = 1.0f;
        if (p < 0.5f) {
            look.alphaB = 0.0f;
            look.dip = p * 2.0f;
        } else {
            look.alphaB = 1.0f;
            look.dip = (1.0f - p) * 2.0f;
        }
    }
};

class DipWhiteFx : public ClipEffect {
public:
    const char* id() const override { return "dip_white"; }
    const char* label() const override { return "Dip to White"; }
    const char* badge() const override { return "DW"; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.scaleA = 1.0f;
        look.scaleB = 1.0f;
        look.dipR = look.dipG = look.dipB = 1.0f;
        if (p < 0.5f) {
            look.alphaB = 0.0f;
            look.dip = p * 2.0f;
        } else {
            look.alphaB = 1.0f;
            look.dip = (1.0f - p) * 2.0f;
        }
    }
};

class WipeFx : public ClipEffect {
public:
    WipeFx(const char* i, const char* lab, const char* b, int dir) : wid(i), wlab(lab), wbadge(b), wdir(dir) {}
    const char* id() const override { return wid; }
    const char* label() const override { return wlab; }
    const char* badge() const override { return wbadge; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.alphaB = p;
        look.scaleA = 1.0f;
        look.scaleB = 1.0f;
        look.wipe = wdir;
    }
    const char* wid;
    const char* wlab;
    const char* wbadge;
    int wdir;
};

class ZoomInFx : public ClipEffect {
public:
    const char* id() const override { return "zoom_in"; }
    const char* label() const override { return "Zoom In"; }
    const char* badge() const override { return "ZI"; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.alphaB = p;
        look.scaleA = 1.0f + 0.35f * p;
        look.scaleB = 1.0f;
    }
};

class ZoomOutFx : public ClipEffect {
public:
    const char* id() const override { return "zoom_out"; }
    const char* label() const override { return "Zoom Out"; }
    const char* badge() const override { return "ZO"; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.alphaB = p;
        look.scaleA = 1.0f;
        look.scaleB = 1.35f - 0.35f * p;
    }
};

class ShapeFx : public ClipEffect {
public:
    ShapeFx(const char* i, const char* lab, const char* b, int s) : sid(i), slab(lab), sbadge(b), shape(s) {}
    const char* id() const override { return sid; }
    const char* label() const override { return slab; }
    const char* badge() const override { return sbadge; }
    void evaluate(float p, FxLook& look) const override {
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        look.alphaB = p;
        look.scaleA = 1.0f;
        look.scaleB = 1.0f;
        look.shape = shape;
    }
    const char* sid;
    const char* slab;
    const char* sbadge;
    int shape;
};

std::vector<ClipEffect*>& Registry() {
    static std::vector<ClipEffect*> items;
    static bool ready = false;
    if (!ready) {
        items.push_back(new CrossDissolve());
        items.push_back(new BlendFx());
        items.push_back(new DipBlackFx());
        items.push_back(new DipWhiteFx());
        items.push_back(new WipeFx("wipe_left", "Wipe Left", "WL", 2));
        items.push_back(new WipeFx("wipe_right", "Wipe Right", "WR", 1));
        items.push_back(new WipeFx("wipe_down", "Wipe Down", "WD", 3));
        items.push_back(new WipeFx("wipe_up", "Wipe Up", "WU", 4));
        items.push_back(new ZoomInFx());
        items.push_back(new ZoomOutFx());
        items.push_back(new ShapeFx("flash_cut", "Flash Cut", "FC", 1));
        items.push_back(new ShapeFx("color_pop", "Color Pop", "CP", 2));
        items.push_back(new ShapeFx("paint_in", "Paint In", "PI", 3));
        items.push_back(new ShapeFx("block_reveal", "Blocks", "BK", 4));
        items.push_back(new ShapeFx("center_hold", "Center Hold", "CH", 5));
        items.push_back(new ShapeFx("detail_hold", "Detail Hold", "DH", 6));
        items.push_back(new ShapeFx("page_peel", "Page Peel", "PP", 7));
        items.push_back(new ShapeFx("whip_up", "Whip Up", "W+", 8));
        items.push_back(new ShapeFx("whip_down", "Whip Down", "W-", 9));
        items.push_back(new ShapeFx("blob_open", "Soft Blobs", "SB", 10));
        items.push_back(new ShapeFx("iris", "Iris", "IR", 11));
        items.push_back(new ShapeFx("venetian", "Venetian", "VN", 12));
        items.push_back(new ShapeFx("shatter", "Shatter", "SH", 13));
        items.push_back(new ShapeFx("light_leak", "Light Leak", "LL", 14));
        items.push_back(new ShapeFx("glitch", "Glitch", "GL", 15));
        items.push_back(new ShapeFx("zoom_blur", "Zoom Blur", "ZB", 16));
        items.push_back(new ShapeFx("clock", "Clock", "CK", 17));
        items.push_back(new ShapeFx("film_burn", "Film Burn", "FB", 18));
        items.push_back(new ShapeFx("split_open", "Split", "SO", 19));
        items.push_back(new ShapeFx("ripple", "Ripple", "RP", 20));
        ready = true;
    }
    return items;
}

void SampleScaled(const uint8_t* img, int w, int h, float u, float v, float scale, int& r, int& g, int& b) {
    if (!img || w < 1 || h < 1 || scale < 0.05f) { r = g = b = 0; return; }
    u = (u - 0.5f) / scale + 0.5f;
    v = (v - 0.5f) / scale + 0.5f;
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) { r = g = b = 0; return; }
    int x = (int)(u * (float)w);
    int y = (int)(v * (float)h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    const uint8_t* p = img + ((size_t)y * (size_t)w + (size_t)x) * 4;
    r = p[0];
    g = p[1];
    b = p[2];
}

static float Sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float Hash2(int x, int y) {
    unsigned n = (unsigned)(x * 374761393 + y * 668265263);
    n = (n ^ (n >> 13)) * 1274126177u;
    return (n & 65535) / 65535.0f;
}

static void MixTo(int ar, int ag, int ab, int br, int bg, int bb, float w, int& rr, int& rg, int& rb) {
    w = Sat(w);
    rr = (int)(ar + (br - ar) * w);
    rg = (int)(ag + (bg - ag) * w);
    rb = (int)(ab + (bb - ab) * w);
}

static void AddTint(int& r, int& g, int& b, int tr, int tg, int tb, float k) {
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    r = (int)(r + (tr - r) * k);
    g = (int)(g + (tg - g) * k);
    b = (int)(b + (tb - b) * k);
}

static void Smear(const uint8_t* img, int w, int h, float u, float v, float du, float dv, int& r, int& g, int& b) {
    int ar = 0, ag = 0, ab = 0;
    const int taps = 4;
    for (int i = 0; i < taps; i++) {
        int sr, sg, sb;
        SampleScaled(img, w, h, u + du * (float)i, v + dv * (float)i, 1.0f, sr, sg, sb);
        ar += sr; ag += sg; ab += sb;
    }
    r = ar / taps;
    g = ag / taps;
    b = ab / taps;
}

static void Radial(const uint8_t* img, int w, int h, float u, float v, float spread, int& r, int& g, int& b) {
    int ar = 0, ag = 0, ab = 0;
    const int taps = 5;
    for (int i = 0; i < taps; i++) {
        float t = spread * ((float)i / (float)(taps - 1));
        int sr, sg, sb;
        SampleScaled(img, w, h, 0.5f + (u - 0.5f) * (1.0f + t), 0.5f + (v - 0.5f) * (1.0f + t), 1.0f, sr, sg, sb);
        ar += sr; ag += sg; ab += sb;
    }
    r = ar / taps;
    g = ag / taps;
    b = ab / taps;
}

static void Shaped(int shape, float p, float u, float v, int dw, int dh,
                   const uint8_t* a, int aw, int ah, const uint8_t* b, int bw, int bh,
                   int& rr, int& rg, int& rb) {
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    int ar, ag, ab, br, bg, bb;
    SampleScaled(a, aw, ah, u, v, 1.0f, ar, ag, ab);
    SampleScaled(b, bw, bh, u, v, 1.0f, br, bg, bb);
    float aspect = (dw > 0) ? (float)dh / (float)dw : 1.0f;
    auto luma = [](int r, int g, int b) { return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f; };
    float wB = p;
    if (shape == 1 || shape == 2) {
        wB = p >= 0.5f ? 1.0f : 0.0f;
    } else if (shape == 3) {
        int band = (int)(v * 22.0f);
        float delay = Hash2(band, 1) * 0.42f;
        float local = (p - delay) / 0.52f;
        float t = Sat(local);
        wB = 1.0f - Sat((u - (t - 0.04f)) / 0.06f);
        if (local <= 0.0f) wB = 0.0f;
        if (local >= 1.0f) wB = 1.0f;
    } else if (shape == 4) {
        int gx = (int)(u * 8.0f);
        int gy = (int)(v * 5.0f);
        if (gx > 7) gx = 7;
        if (gy > 4) gy = 4;
        wB = p > Hash2(gx, gy) * 0.85f ? 1.0f : 0.0f;
    } else if (shape == 5) {
        float nx = (u - 0.5f) / 0.22f;
        float ny = ((v - 0.5f) * aspect) / 0.20f;
        float d = std::sqrt(nx * nx + ny * ny);
        float outside = Sat(p / 0.60f);
        wB = d > 1.0f ? outside : outside * Sat((d - 0.55f) / 0.45f);
        float late = Sat((p - 0.58f) / 0.42f);
        if (late > wB) wB = late;
    } else if (shape == 6) {
        int r1, g1, b1, r2, g2, b2;
        SampleScaled(a, aw, ah, u + 0.06f, v, 1.0f, r1, g1, b1);
        SampleScaled(a, aw, ah, u - 0.06f, v + 0.05f, 1.0f, r2, g2, b2);
        float detail = std::fabs(luma(ar, ag, ab) - luma(r1, g1, b1));
        detail += std::fabs(luma(ar, ag, ab) - luma(r2, g2, b2));
        detail = Sat((detail - 0.06f) / 0.22f);
        wB = Sat((p * 1.15f) - detail * 0.45f);
    } else if (shape == 7) {
        float line = 1.08f - p * 1.28f;
        float d = u - line;
        if (d >= 0.0f) {
            wB = 0.0f;
            if (d < 0.035f) {
                MixTo(ar, ag, ab, br, bg, bb, 0.0f, rr, rg, rb);
                AddTint(rr, rg, rb, 255, 255, 255, 1.0f - d / 0.035f);
                return;
            }
        } else {
            wB = 1.0f;
            MixTo(ar, ag, ab, br, bg, bb, 1.0f, rr, rg, rb);
            if (d > -0.07f) AddTint(rr, rg, rb, 0, 0, 0, (d + 0.07f) / 0.07f * 0.55f);
            return;
        }
    } else if (shape == 8 || shape == 9) {
        float dir = shape == 8 ? -1.0f : 1.0f;
        float spread = std::sin(p * 3.14159265f) * 0.07f;
        if (p < 0.5f) Smear(a, aw, ah, u, v, 0.0f, dir * spread, ar, ag, ab);
        else Smear(b, bw, bh, u, v, 0.0f, -dir * spread, br, bg, bb);
        wB = p < 0.46f ? 0.0f : Sat((p - 0.46f) / 0.12f);
    } else if (shape == 10) {
        const float cx[3] = {0.30f, 0.72f, 0.48f};
        const float cy[3] = {0.36f, 0.44f, 0.74f};
        float rad = 0.02f + p * 0.62f;
        wB = 0.0f;
        for (int i = 0; i < 3; i++) {
            float dx = u - cx[i];
            float dy = (v - cy[i]) * aspect;
            float d = std::sqrt(dx * dx + dy * dy);
            float k = 1.0f - Sat((d - rad * 0.72f) / std::max(0.02f, rad * 0.28f));
            if (k > wB) wB = k;
        }
    } else if (shape == 11) {
        float dx = u - 0.5f;
        float dy = (v - 0.5f) * aspect;
        float d = std::sqrt(dx * dx + dy * dy);
        float rad = p * 0.72f;
        wB = 1.0f - Sat((d - (rad - 0.02f)) / 0.035f);
    } else if (shape == 12) {
        float band = v * 11.0f;
        band = band - std::floor(band);
        float s = std::fabs(band - 0.5f) * 2.0f;
        wB = s < p ? 1.0f : 0.0f;
    } else if (shape == 13) {
        int gx = (int)(u * 10.0f);
        int gy = (int)(v * 7.0f);
        wB = p > Hash2(gx, gy) ? 1.0f : 0.0f;
    } else if (shape == 14) {
        float s = p * p * (3.0f - 2.0f * p);
        wB = s;
    } else if (shape == 15) {
        int slice = (int)(v * 16.0f);
        float j = (Hash2(slice, (int)(p * 24.0f)) - 0.5f) * 0.10f * std::sin(p * 18.0f);
        float uu = u + j;
        bool take = p > Hash2(slice, 3) * 0.55f;
        if (take) SampleScaled(b, bw, bh, uu, v, 1.0f, br, bg, bb);
        else SampleScaled(a, aw, ah, uu, v, 1.0f, ar, ag, ab);
        wB = take ? 1.0f : 0.0f;
        if (p > 0.18f && p < 0.82f) {
            int rL, gL, bL, rR, gR, bR;
            const uint8_t* img = take ? b : a;
            int iw = take ? bw : aw;
            int ih = take ? bh : ah;
            SampleScaled(img, iw, ih, uu - 0.012f, v, 1.0f, rL, gL, bL);
            SampleScaled(img, iw, ih, uu + 0.012f, v, 1.0f, rR, gR, bR);
            MixTo(ar, ag, ab, br, bg, bb, wB, rr, rg, rb);
            rr = rL;
            rb = bR;
            (void)gL; (void)bL; (void)rR; (void)gR;
            return;
        }
    } else if (shape == 16) {
        float spread = std::sin(p * 3.14159265f) * 0.07f;
        float s = p * p * (3.0f - 2.0f * p);
        Radial(a, aw, ah, u, v, spread, ar, ag, ab);
        Radial(b, bw, bh, u, v, spread, br, bg, bb);
        wB = s;
    } else if (shape == 17) {
        float ang = std::atan2(u - 0.5f, -(v - 0.5f));
        float a01 = (ang + 3.14159265f) / (2.0f * 3.14159265f);
        if (a01 < 0) a01 = 0;
        if (a01 > 1) a01 = 1;
        wB = a01 < p ? 1.0f : 0.0f;
    } else if (shape == 18) {
        wB = p * p * (3.0f - 2.0f * p);
    } else if (shape == 19) {
        float gap = p * 0.5f;
        if (u < 0.5f - gap) {
            SampleScaled(a, aw, ah, u + gap, v, 1.0f, ar, ag, ab);
            wB = 0.0f;
        } else if (u > 0.5f + gap) {
            SampleScaled(a, aw, ah, u - gap, v, 1.0f, ar, ag, ab);
            wB = 0.0f;
        } else wB = 1.0f;
    } else if (shape == 20) {
        float dx = u - 0.5f;
        float dy = (v - 0.5f) * aspect;
        float d = std::sqrt(dx * dx + dy * dy);
        float rad = p * 0.9f;
        wB = d < rad ? 1.0f : 0.0f;
        MixTo(ar, ag, ab, br, bg, bb, wB, rr, rg, rb);
        float ring = d - rad;
        float k = std::exp(-(ring * ring) / 0.00035f);
        if (p > 0.02f && p < 0.98f) AddTint(rr, rg, rb, 255, 255, 255, k * 0.7f);
        return;
    }
    MixTo(ar, ag, ab, br, bg, bb, wB, rr, rg, rb);
    if (shape == 1) {
        float flash = std::exp(-((p - 0.5f) * (p - 0.5f)) / 0.0045f);
        AddTint(rr, rg, rb, 255, 255, 255, flash);
    } else if (shape == 2) {
        float flash = std::exp(-((p - 0.5f) * (p - 0.5f)) / 0.0055f);
        if (p < 0.5f) AddTint(rr, rg, rb, 40, 230, 255, flash);
        else AddTint(rr, rg, rb, 255, 40, 190, flash);
    } else if (shape == 14) {
        float x = u - (p * 1.25f - 0.12f);
        float leak = std::exp(-x * x * 36.0f) * std::sin(p * 3.14159265f);
        AddTint(rr, rg, rb, 255, 150, 40, leak * 0.9f);
    } else if (shape == 18) {
        float hot = std::sin(p * 3.14159265f);
        rr = (int)(rr * (1.0f + hot * 1.35f) + hot * hot * 90.0f);
        rg = (int)(rg * (1.0f + hot * 0.7f) + hot * hot * 30.0f);
        rb = (int)(rb * (1.0f + hot * 0.25f));
    }
}

} // namespace

int FxCount() { return (int)Registry().size(); }

ClipEffect* FxAt(int index) {
    auto& items = Registry();
    if (index < 0 || index >= (int)items.size()) return nullptr;
    return items[index];
}

ClipEffect* FxFind(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (ClipEffect* fx : Registry()) {
        if (strcmp(fx->id(), id) == 0) return fx;
    }
    return nullptr;
}

bool FxKnown(const char* id) { return FxFind(id) != nullptr; }

void FxEvaluate(const char* id, float progress, FxLook& look) {
    look.alphaB = 0;
    look.scaleA = 1;
    look.scaleB = 1;
    look.dip = 0;
    look.dipR = look.dipG = look.dipB = 0;
    look.wipe = 0;
    look.shape = 0;
    ClipEffect* fx = FxFind(id);
    if (!fx) return;
    fx->evaluate(progress, look);
}

void FxPaint(const FxLook& look,
             uint8_t* dst, int dw, int dh,
             const uint8_t* a, int aw, int ah,
             const uint8_t* b, int bw, int bh) {
    if (!dst || dw < 1 || dh < 1) return;
    if (look.shape != 0) {
        float p = look.alphaB;
        for (int y = 0; y < dh; y++) {
            float v = ((float)y + 0.5f) / (float)dh;
            uint8_t* row = dst + (size_t)y * (size_t)dw * 4;
            for (int x = 0; x < dw; x++) {
                float u = ((float)x + 0.5f) / (float)dw;
                int rr, rg, rb;
                Shaped(look.shape, p, u, v, dw, dh, a, aw, ah, b, bw, bh, rr, rg, rb);
                if (rr < 0) rr = 0;
                if (rg < 0) rg = 0;
                if (rb < 0) rb = 0;
                if (rr > 255) rr = 255;
                if (rg > 255) rg = 255;
                if (rb > 255) rb = 255;
                uint8_t* px = row + (size_t)x * 4;
                px[0] = (uint8_t)rr;
                px[1] = (uint8_t)rg;
                px[2] = (uint8_t)rb;
                px[3] = 255;
            }
        }
        return;
    }
    float wb = look.alphaB;
    if (wb < 0) wb = 0;
    if (wb > 1) wb = 1;
    float wa = 1.0f - wb;
    for (int y = 0; y < dh; y++) {
        float v = ((float)y + 0.5f) / (float)dh;
        uint8_t* row = dst + (size_t)y * (size_t)dw * 4;
        for (int x = 0; x < dw; x++) {
            float u = ((float)x + 0.5f) / (float)dw;
            int ar, ag, ab, br, bg, bb;
            SampleScaled(a, aw, ah, u, v, look.scaleA, ar, ag, ab);
            SampleScaled(b, bw, bh, u, v, look.scaleB, br, bg, bb);
            float wA = wa;
            float wB = wb;
            if (look.wipe == 1) { bool takeB = u < wb; wA = takeB ? 0.0f : 1.0f; wB = takeB ? 1.0f : 0.0f; }
            else if (look.wipe == 2) { bool takeB = u > 1.0f - wb; wA = takeB ? 0.0f : 1.0f; wB = takeB ? 1.0f : 0.0f; }
            else if (look.wipe == 3) { bool takeB = v < wb; wA = takeB ? 0.0f : 1.0f; wB = takeB ? 1.0f : 0.0f; }
            else if (look.wipe == 4) { bool takeB = v > 1.0f - wb; wA = takeB ? 0.0f : 1.0f; wB = takeB ? 1.0f : 0.0f; }
            int rr = (int)(ar * wA + br * wB);
            int rg = (int)(ag * wA + bg * wB);
            int rb = (int)(ab * wA + bb * wB);
            if (look.dip > 0.001f) {
                float k = look.dip > 1.0f ? 1.0f : look.dip;
                rr = (int)(rr * (1.0f - k) + look.dipR * 255.0f * k);
                rg = (int)(rg * (1.0f - k) + look.dipG * 255.0f * k);
                rb = (int)(rb * (1.0f - k) + look.dipB * 255.0f * k);
            }
            if (rr < 0) rr = 0;
            if (rg < 0) rg = 0;
            if (rb < 0) rb = 0;
            if (rr > 255) rr = 255;
            if (rg > 255) rg = 255;
            if (rb > 255) rb = 255;
            uint8_t* px = row + (size_t)x * 4;
            px[0] = (uint8_t)rr;
            px[1] = (uint8_t)rg;
            px[2] = (uint8_t)rb;
            px[3] = 255;
        }
    }
}

void FxComposite(const char* id, float progress,
                 uint8_t* dst, int dw, int dh,
                 const uint8_t* a, int aw, int ah,
                 const uint8_t* b, int bw, int bh) {
    FxLook look;
    FxEvaluate(id, progress, look);
    FxPaint(look, dst, dw, dh, a, aw, ah, b, bw, bh);
}
