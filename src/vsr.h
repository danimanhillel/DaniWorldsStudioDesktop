#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct VsrInfo {
    bool files = false;
    bool ready = false;
    bool cuda = false;
    std::string detail;
};

struct VsrShape {
    int window = 1;
    int fixedW = 0;
    int fixedH = 0;
    int scaleHint = 0;
};

const VsrInfo& VsrProbe();
bool VsrStartup(std::string& err);
void VsrShutdown();
VsrShape VsrQuery();
bool VsrRunTile(const std::vector<std::vector<uint8_t>>& frames, int w, int h,
                int x, int y, int tw, int th, std::vector<uint8_t>& hrOut, int& scaleOut, std::string& err);
