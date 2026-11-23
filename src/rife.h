#pragma once
#include <string>

struct RifeInfo {
    bool files = false;
    bool ready = false;
    bool cuda = false;
    std::string detail;
};

const RifeInfo& RifeProbe();
bool RifeStartup(std::string& err);
void RifeShutdown();
bool RifeRun(const uint8_t* rgba0, const uint8_t* rgba1, int w, int h, float t, uint8_t* rgbaOut);
