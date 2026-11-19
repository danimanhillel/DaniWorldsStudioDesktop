#pragma once
#include <string>
#include <vector>

struct AsrSeg {
    float start = 0.0f;
    float end = 0.0f;
    std::string text;
};

bool AsrOpen(std::string& err);
void AsrClose();
void AsrLogReset();
void AsrLog(const std::string& line);
bool AsrTranscribeChunk(const float* pcm, int samples, float timeOffset, int language, std::vector<AsrSeg>& out, std::string& err);
