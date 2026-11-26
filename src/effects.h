#pragma once
#include <cstdint>

// Clip effects live here, not in main. Add a new ClipEffect in effects.cpp
// and register it to show up in the timeline menu and in project files.

struct FxLook {
    float alphaB = 0.0f;
    float scaleA = 1.0f;
    float scaleB = 1.0f;
    float dip = 0.0f;
    float dipR = 0.0f;
    float dipG = 0.0f;
    float dipB = 0.0f;
    int wipe = 0;
};

class ClipEffect {
public:
    virtual ~ClipEffect() = default;
    virtual const char* id() const = 0;
    virtual const char* label() const = 0;
    virtual const char* badge() const = 0;
    virtual void evaluate(float progress, FxLook& look) const = 0;
};

int FxCount();
ClipEffect* FxAt(int index);
ClipEffect* FxFind(const char* id);
bool FxKnown(const char* id);
void FxEvaluate(const char* id, float progress, FxLook& look);
void FxComposite(const char* id, float progress,
                 uint8_t* dst, int dw, int dh,
                 const uint8_t* a, int aw, int ah,
                 const uint8_t* b, int bw, int bh);
