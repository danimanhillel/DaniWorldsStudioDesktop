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
        look.alphaB = p;
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

std::vector<ClipEffect*>& Registry() {
    static std::vector<ClipEffect*> items;
    static bool ready = false;
    if (!ready) {
        items.push_back(new CrossDissolve());
        items.push_back(new BlendFx());
        items.push_back(new ZoomInFx());
        items.push_back(new ZoomOutFx());
        ready = true;
    }
    return items;
}

void SampleScaled(const uint8_t* img, int w, int h, float u, float v, float scale, int& r, int& g, int& b) {
    if (!img || w < 1 || h < 1 || scale < 0.05f) { r = g = b = 0; return; }
    u = (u - 0.5f) / scale + 0.5f;
    v = (v - 0.5f) / scale + 0.5f;
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) { r = g = b = 0; return; }
    int x = (int)(u * (float)(w - 1));
    int y = (int)(v * (float)(h - 1));
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    const uint8_t* p = img + ((size_t)y * (size_t)w + (size_t)x) * 4;
    r = p[0];
    g = p[1];
    b = p[2];
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
    ClipEffect* fx = FxFind(id);
    if (!fx) return;
    fx->evaluate(progress, look);
}

void FxComposite(const char* id, float progress,
                 uint8_t* dst, int dw, int dh,
                 const uint8_t* a, int aw, int ah,
                 const uint8_t* b, int bw, int bh) {
    if (!dst || dw < 1 || dh < 1) return;
    FxLook look;
    FxEvaluate(id, progress, look);
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
            uint8_t* p = row + (size_t)x * 4;
            p[0] = (uint8_t)std::min(255, (int)(ar * wa + br * wb));
            p[1] = (uint8_t)std::min(255, (int)(ag * wa + bg * wb));
            p[2] = (uint8_t)std::min(255, (int)(ab * wa + bb * wb));
            p[3] = 255;
        }
    }
}
