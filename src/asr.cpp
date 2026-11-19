#include "asr.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "onnxruntime_c_api.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int kSot = 50258;
const int kEot = 50257;
const int kTranscribe = 50359;
const int kNoSpeech = 50362;
const int kTimestamp = 50364;
const int kEn = 50259;
const int kHe = 50279;

const OrtApi* gApi = nullptr;
OrtEnv* gEnv = nullptr;
OrtSession* gEnc = nullptr;
OrtSession* gDec = nullptr;
OrtAllocator* gAlloc = nullptr;
#ifdef _WIN32
HMODULE gOrt = nullptr;
#endif
int gMels = 80;
int gFrames = 3000;
int gHiddenFrames = 1500;
int gHidden = 512;
std::string gEncIn, gEncOut, gDecIds, gDecEnc, gDecOut;
std::vector<float> gMelBank;
std::vector<std::string> gPieces;
int gCpByte[512];

#ifdef _WIN32
std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring s(buf, buf + n);
    size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return s.substr(0, slash + 1);
}
bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
std::wstring FindFile(const std::wstring& dir, const wchar_t* name) {
    std::wstring a = dir + name;
    if (FileExists(a)) return a;
    std::wstring b = dir + L"models\\" + name;
    if (FileExists(b)) return b;
    return L"";
}
#endif

void Log(const std::wstring& dir, const std::string& line) {
#ifdef _WIN32
    FILE* f = _wfopen((dir + L"asr.log").c_str(), L"ab");
    if (!f) return;
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
#else
    (void)dir;
    (void)line;
#endif
}

bool Ok(OrtStatus* st, std::string& err, const char* what) {
    if (!st) return true;
    const char* m = gApi ? gApi->GetErrorMessage(st) : nullptr;
    err = std::string(what) + ": " + (m ? m : "failed");
    if (st) gApi->ReleaseStatus(st);
    return false;
}

int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string B64(const std::string& in) {
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = B64Val((char)c);
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

void InitByteMap() {
    for (int i = 0; i < 512; i++) gCpByte[i] = -1;
    std::vector<int> bs;
    for (int i = '!'; i <= '~'; i++) bs.push_back(i);
    for (int i = 0xA1; i <= 0xAC; i++) bs.push_back(i);
    for (int i = 0xAE; i <= 0xFF; i++) bs.push_back(i);
    std::vector<int> cs = bs;
    bool seen[256] = {};
    for (int b : bs) seen[b] = true;
    int n = 0;
    for (int b = 0; b < 256; b++) if (!seen[b]) { bs.push_back(b); cs.push_back(256 + n); n++; }
    for (size_t i = 0; i < bs.size() && i < cs.size(); i++) {
        if (cs[i] >= 0 && cs[i] < 512) gCpByte[cs[i]] = bs[i];
    }
}

std::string PieceToText(const std::string& utf8) {
    std::string raw;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        uint32_t cp = 0;
        int need = 1;
        if (c < 0x80) { cp = c; need = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) { cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i + 1] & 0x3F); need = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[i + 1] & 0x3F) << 6) | ((unsigned char)utf8[i + 2] & 0x3F);
            need = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((c & 0x07) << 18) | (((unsigned char)utf8[i + 1] & 0x3F) << 12) | (((unsigned char)utf8[i + 2] & 0x3F) << 6) | ((unsigned char)utf8[i + 3] & 0x3F);
            need = 4;
        } else { i++; continue; }
        i += need;
        int b = (cp < 512) ? gCpByte[cp] : -1;
        if (b >= 0) raw.push_back((char)b);
    }
    return raw;
}

bool LoadPieces(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    gPieces.assign(50257, "");
    std::string line;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        int id = atoi(line.c_str() + sp + 1);
        if (id < 0 || id >= (int)gPieces.size()) continue;
        gPieces[id] = B64(line.substr(0, sp));
    }
    fclose(f);
    return !gPieces[1].empty() || !gPieces[0].empty();
}

bool LoadMel(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n != 80 * 201 * 4) { fclose(f); return false; }
    gMelBank.resize(80 * 201);
    size_t got = fread(gMelBank.data(), 1, gMelBank.size() * sizeof(float), f);
    fclose(f);
    return got == gMelBank.size() * sizeof(float);
}

void LogMel(const float* pcm, int n, std::vector<float>& mel) {
    const int nfft = 400;
    const int hop = 160;
    const int nfreq = 201;
    const int frames = 3000;
    const int samples = 480000;
    std::vector<float> audio((size_t)samples, 0.0f);
    int copy = std::min(n, samples);
    if (pcm && copy > 0) memcpy(audio.data(), pcm, (size_t)copy * sizeof(float));
    std::vector<float> padded((size_t)samples + nfft, 0.0f);
    for (int i = 0; i < nfft / 2; i++) {
        int src = nfft / 2 - i;
        if (src >= samples) src = samples - 1;
        padded[i] = audio[src];
    }
    memcpy(padded.data() + nfft / 2, audio.data(), (size_t)samples * sizeof(float));
    for (int i = 0; i < nfft / 2; i++) {
        int src = samples - 2 - i;
        if (src < 0) src = 0;
        padded[nfft / 2 + samples + i] = audio[src];
    }
    std::vector<float> window((size_t)nfft);
    for (int i = 0; i < nfft; i++) window[i] = 0.5f * (1.0f - cosf(6.2831853f * (float)i / (float)nfft));
    std::vector<float> power((size_t)nfreq * frames, 0.0f);
    for (int f = 0; f < frames; f++) {
        const float* src = padded.data() + (size_t)f * hop;
        for (int k = 0; k < nfreq; k++) {
            float re = 0.0f, im = 0.0f;
            float ang = 6.2831853f * (float)k / (float)nfft;
            float c = 1.0f, s = 0.0f;
            float cd = cosf(ang), sd = sinf(ang);
            for (int t = 0; t < nfft; t++) {
                float w = src[t] * window[t];
                re += w * c;
                im -= w * s;
                float nc = c * cd - s * sd;
                s = c * sd + s * cd;
                c = nc;
            }
            power[(size_t)k * frames + f] = re * re + im * im;
        }
    }
    mel.assign((size_t)gMels * frames, 0.0f);
    int mels = std::min(gMels, 80);
    for (int m = 0; m < mels; m++) {
        const float* filt = gMelBank.data() + (size_t)m * nfreq;
        for (int f = 0; f < frames; f++) {
            float sum = 0.0f;
            for (int k = 0; k < nfreq; k++) sum += filt[k] * power[(size_t)k * frames + f];
            mel[(size_t)m * frames + f] = sum;
        }
    }
    float peak = -1.0e30f;
    for (float& v : mel) {
        v = log10f(std::max(v, 1.0e-10f));
        peak = std::max(peak, v);
    }
    float floor = peak - 8.0f;
    for (float& v : mel) v = (std::max(v, floor) + 4.0f) / 4.0f;
}

std::string ReadName(OrtSession* session, size_t index, bool input) {
    char* name = nullptr;
    OrtStatus* st = input ? gApi->SessionGetInputName(session, index, gAlloc, &name)
                          : gApi->SessionGetOutputName(session, index, gAlloc, &name);
    if (st) { gApi->ReleaseStatus(st); return {}; }
    std::string out = name ? name : "";
    if (name) gAlloc->Free(gAlloc, name);
    return out;
}

bool PickNames(std::string& err) {
    size_t nIn = 0, nOut = 0;
    gApi->SessionGetInputCount(gEnc, &nIn);
    gApi->SessionGetOutputCount(gEnc, &nOut);
    if (nIn < 1 || nOut < 1) { err = "Whisper encoder has no inputs"; return false; }
    gEncIn = ReadName(gEnc, 0, true);
    gEncOut = ReadName(gEnc, 0, false);
    gApi->SessionGetInputCount(gDec, &nIn);
    gApi->SessionGetOutputCount(gDec, &nOut);
    for (size_t i = 0; i < nIn; i++) {
        std::string n = ReadName(gDec, i, true);
        std::string low = n;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("input_ids") != std::string::npos || low.find("decoder_input") != std::string::npos) gDecIds = n;
        else if (low.find("encoder") != std::string::npos || low.find("hidden") != std::string::npos) gDecEnc = n;
    }
    for (size_t i = 0; i < nOut; i++) {
        std::string n = ReadName(gDec, i, false);
        std::string low = n;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("logits") != std::string::npos || gDecOut.empty()) gDecOut = n;
    }
    if (gDecIds.empty() || gDecEnc.empty() || gDecOut.empty()) {
        err = "Whisper decoder inputs were not recognized";
        return false;
    }
    return true;
}

}  // namespace

bool AsrOpen(std::string& err) {
    AsrClose();
#ifndef _WIN32
    err = "Auto subtitles are available on Windows";
    return false;
#else
    std::wstring dir = ExeDir();
    std::wstring enc = FindFile(dir, L"whisper-encoder.onnx");
    std::wstring dec = FindFile(dir, L"whisper-decoder.onnx");
    std::wstring mel = FindFile(dir, L"mel80.bin");
    std::wstring vocab = FindFile(dir, L"multilingual.tiktoken");
    if (enc.empty() || dec.empty()) {
        err = "Put whisper-encoder.onnx and whisper-decoder.onnx next to the program";
        return false;
    }
    if (mel.empty() || vocab.empty()) {
        err = "mel80.bin or multilingual.tiktoken is missing next to the program";
        return false;
    }
    InitByteMap();
    if (!LoadMel(mel) || !LoadPieces(vocab)) {
        err = "Could not read the Whisper tokenizer files";
        return false;
    }
    std::wstring ort = dir + L"onnxruntime.dll";
    gOrt = LoadLibraryW(ort.c_str());
    if (!gOrt) { err = "onnxruntime.dll was not found"; return false; }
    using GetApiFn = const OrtApiBase*(ORT_API_CALL*)();
    auto getApi = (GetApiFn)GetProcAddress(gOrt, "OrtGetApiBase");
    if (!getApi) { err = "OrtGetApiBase is missing"; AsrClose(); return false; }
    const OrtApiBase* base = getApi();
    for (int v = ORT_API_VERSION; v >= 8 && !gApi; v--) gApi = base->GetApi((uint32_t)v);
    if (!gApi) { err = "ONNX Runtime API was not found"; AsrClose(); return false; }
    if (!Ok(gApi->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "asr", &gEnv), err, "ASR")) { AsrClose(); return false; }
    if (!Ok(gApi->GetAllocatorWithDefaultOptions(&gAlloc), err, "ASR")) { AsrClose(); return false; }
    OrtSessionOptions* opt = nullptr;
    if (!Ok(gApi->CreateSessionOptions(&opt), err, "ASR")) { AsrClose(); return false; }
    gApi->SetIntraOpNumThreads(opt, 8);
    gApi->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
    const char* keys[] = {"device_id"};
    const char* vals[] = {"0"};
    OrtStatus* cst = gApi->SessionOptionsAppendExecutionProvider(opt, "CUDAExecutionProvider", keys, vals, 1);
    if (cst) gApi->ReleaseStatus(cst);
    auto openSession = [&](const std::wstring& model, OrtSession** dest, const char* what) {
        OrtStatus* created = gApi->CreateSession(gEnv, model.c_str(), opt, dest);
        if (!created) return true;
        gApi->ReleaseStatus(created);
        OrtSessionOptions* cpu = nullptr;
        if (!Ok(gApi->CreateSessionOptions(&cpu), err, what)) return false;
        gApi->SetIntraOpNumThreads(cpu, 8);
        gApi->SetSessionGraphOptimizationLevel(cpu, ORT_ENABLE_ALL);
        bool ok = Ok(gApi->CreateSession(gEnv, model.c_str(), cpu, dest), err, what);
        gApi->ReleaseSessionOptions(cpu);
        return ok;
    };
    if (!openSession(enc, &gEnc, "Whisper encoder") || !openSession(dec, &gDec, "Whisper decoder")) {
        gApi->ReleaseSessionOptions(opt);
        AsrClose();
        return false;
    }
    gApi->ReleaseSessionOptions(opt);
    if (!PickNames(err)) { AsrClose(); return false; }
    Log(dir, "Whisper ready");
    return true;
#endif
}

void AsrClose() {
    if (gApi && gEnc) gApi->ReleaseSession(gEnc);
    if (gApi && gDec) gApi->ReleaseSession(gDec);
    gEnc = gDec = nullptr;
    if (gApi && gEnv) gApi->ReleaseEnv(gEnv);
    gEnv = nullptr;
    gApi = nullptr;
    gAlloc = nullptr;
#ifdef _WIN32
    if (gOrt) { FreeLibrary(gOrt); gOrt = nullptr; }
#endif
}

bool AsrTranscribeChunk(const float* pcm, int samples, float timeOffset, int language, std::vector<AsrSeg>& out, std::string& err) {
    if (!gApi || !gEnc || !gDec) { err = "Whisper is not loaded"; return false; }
    std::vector<float> mel;
    LogMel(pcm, samples, mel);
    OrtMemoryInfo* mem = nullptr;
    if (!Ok(gApi->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeCPU, &mem), err, "ASR")) return false;
    int64_t melShape[3] = {1, gMels, gFrames};
    OrtValue* melVal = nullptr;
    OrtStatus* st = gApi->CreateTensorWithDataAsOrtValue(mem, mel.data(), mel.size() * sizeof(float), melShape, 3,
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &melVal);
    if (st) { gApi->ReleaseMemoryInfo(mem); return Ok(st, err, "Mel"); }
    const char* inName = gEncIn.c_str();
    const char* outName = gEncOut.c_str();
    OrtValue* encOut = nullptr;
    st = gApi->Run(gEnc, nullptr, &inName, &melVal, 1, &outName, 1, &encOut);
    gApi->ReleaseValue(melVal);
    if (st) { gApi->ReleaseMemoryInfo(mem); return Ok(st, err, "Encoder"); }
    OrtTensorTypeAndShapeInfo* info = nullptr;
    gApi->GetTensorTypeAndShape(encOut, &info);
    size_t rank = 0;
    int64_t dims[4] = {1, 1, 1, 1};
    if (info) {
        gApi->GetDimensionsCount(info, &rank);
        if (rank > 0 && rank <= 4) gApi->GetDimensions(info, dims, rank);
        gApi->ReleaseTensorTypeAndShapeInfo(info);
    }
    if (rank >= 3) {
        gHiddenFrames = (int)dims[rank - 2];
        gHidden = (int)dims[rank - 1];
    }
    float* hidden = nullptr;
    gApi->GetTensorMutableData(encOut, (void**)&hidden);
    if (!hidden) { gApi->ReleaseValue(encOut); gApi->ReleaseMemoryInfo(mem); err = "Encoder returned no data"; return false; }
    std::vector<float> hiddenCopy((size_t)gHiddenFrames * (size_t)gHidden);
    memcpy(hiddenCopy.data(), hidden, hiddenCopy.size() * sizeof(float));
    gApi->ReleaseValue(encOut);

    int lang = language == 1 ? kEn : kHe;
    std::vector<int64_t> ids = {kSot, lang, kTranscribe};
    std::string text;
    float segStart = -1.0f;
    int lastStamp = kTimestamp;
    auto push = [&](float endAt) {
        std::string clean;
        const unsigned char* p = (const unsigned char*)text.data();
        size_t n = text.size();
        for (size_t i = 0; i < n;) {
            unsigned char c = p[i];
            int need = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 0)));
            if (need < 1 || i + (size_t)need > n) { if (need < 1) i++; else break; continue; }
            bool ok = true;
            for (int k = 1; k < need; k++) if ((p[i + k] & 0xC0) != 0x80) ok = false;
            if (!ok) { i++; continue; }
            clean.append(text, i, (size_t)need);
            i += (size_t)need;
        }
        text.swap(clean);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
        size_t b = 0;
        while (b < text.size() && text[b] == ' ') b++;
        if (b) text.erase(0, b);
        if (!text.empty() && segStart >= 0.0f && endAt > segStart + 0.05f) {
            AsrSeg s;
            s.start = timeOffset + segStart;
            s.end = timeOffset + endAt;
            s.text = text;
            out.push_back(s);
        }
        text.clear();
    };
    for (int step = 0; step < 224; step++) {
        int64_t idShape[2] = {1, (int64_t)ids.size()};
        int64_t hShape[3] = {1, gHiddenFrames, gHidden};
        OrtValue* idVal = nullptr;
        OrtValue* hVal = nullptr;
        st = gApi->CreateTensorWithDataAsOrtValue(mem, ids.data(), ids.size() * sizeof(int64_t), idShape, 2,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &idVal);
        if (st) { gApi->ReleaseMemoryInfo(mem); return Ok(st, err, "Tokens"); }
        st = gApi->CreateTensorWithDataAsOrtValue(mem, hiddenCopy.data(), hiddenCopy.size() * sizeof(float), hShape, 3,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &hVal);
        if (st) { gApi->ReleaseValue(idVal); gApi->ReleaseMemoryInfo(mem); return Ok(st, err, "Hidden"); }
        const char* names[2] = {gDecIds.c_str(), gDecEnc.c_str()};
        const OrtValue* vals[2] = {idVal, hVal};
        if (gDecIds > gDecEnc) {
            // keep the model's input order
        }
        OrtSession* dec = gDec;
        size_t nDecIn = 0;
        gApi->SessionGetInputCount(dec, &nDecIn);
        std::vector<const char*> runNames;
        std::vector<const OrtValue*> runVals;
        std::vector<OrtValue*> extra;
        bool bad = false;
        for (size_t i = 0; i < nDecIn; i++) {
            std::string n = ReadName(dec, i, true);
            if (n == gDecIds) { runNames.push_back(gDecIds.c_str()); runVals.push_back(idVal); }
            else if (n == gDecEnc) { runNames.push_back(gDecEnc.c_str()); runVals.push_back(hVal); }
            else {
                bad = true;
                break;
            }
        }
        (void)names;
        (void)vals;
        if (bad || runNames.size() != nDecIn) {
            gApi->ReleaseValue(idVal);
            gApi->ReleaseValue(hVal);
            gApi->ReleaseMemoryInfo(mem);
            err = "This Whisper decoder needs a simple encoder/decoder pair without a cache";
            return false;
        }
        const char* oName = gDecOut.c_str();
        OrtValue* logits = nullptr;
        st = gApi->Run(dec, nullptr, runNames.data(), runVals.data(), nDecIn, &oName, 1, &logits);
        gApi->ReleaseValue(idVal);
        gApi->ReleaseValue(hVal);
        for (OrtValue* e : extra) if (e) gApi->ReleaseValue(e);
        if (st) { gApi->ReleaseMemoryInfo(mem); return Ok(st, err, "Decoder"); }
        float* logit = nullptr;
        gApi->GetTensorMutableData(logits, (void**)&logit);
        OrtTensorTypeAndShapeInfo* li = nullptr;
        gApi->GetTensorTypeAndShape(logits, &li);
        int64_t ld[3] = {1, 1, 1};
        size_t lr = 0;
        if (li) {
            gApi->GetDimensionsCount(li, &lr);
            if (lr > 0 && lr <= 3) gApi->GetDimensions(li, ld, lr);
            gApi->ReleaseTensorTypeAndShapeInfo(li);
        }
        int vocab = lr >= 1 ? (int)ld[lr - 1] : 0;
        int seq = lr >= 2 ? (int)ld[lr - 2] : 1;
        if (!logit || vocab < 100 || seq < 1) {
            gApi->ReleaseValue(logits);
            gApi->ReleaseMemoryInfo(mem);
            err = "Decoder logits were empty";
            return false;
        }
        const float* row = logit + (size_t)(seq - 1) * (size_t)vocab;
        int best = kEot;
        float bestV = -1.0e30f;
        int limit = std::min(vocab, kTimestamp + 1501);
        for (int i = 0; i < limit; i++) {
            if (i > kEot && i < kTimestamp) continue;
            if (i >= kTimestamp && i < lastStamp) continue;
            if (row[i] > bestV) { bestV = row[i]; best = i; }
        }
        gApi->ReleaseValue(logits);
        if (best == kEot || best == kNoSpeech) break;
        ids.push_back(best);
        if (best >= kTimestamp && best < kTimestamp + 1501) {
            lastStamp = best;
            float ts = (best - kTimestamp) * 0.02f;
            if (segStart < 0.0f) segStart = ts;
            else { push(ts); segStart = ts; }
        } else if (best >= 0 && best < (int)gPieces.size()) {
            if (segStart < 0.0f) segStart = 0.0f;
            text += gPieces[best];
        }
    }
    float chunkEnd = samples > 0 ? std::min(30.0f, samples / 16000.0f) : 30.0f;
    if (segStart >= 0.0f && !text.empty()) push(std::min(chunkEnd, segStart + 3.0f));
    gApi->ReleaseMemoryInfo(mem);
    return true;
}
