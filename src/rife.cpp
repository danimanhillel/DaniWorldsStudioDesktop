#include "rife.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "onnxruntime_c_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

RifeInfo gInfo;
const OrtApi* gApi = nullptr;
int gApiVersion = 0;
OrtEnv* gEnv = nullptr;
OrtSession* gSession = nullptr;
OrtAllocator* gAlloc = nullptr;
#ifdef _WIN32
HMODULE gOrt = nullptr;
HMODULE gCuda = nullptr;
HMODULE gCudaShared = nullptr;
#endif
std::vector<float> gIn0, gIn1, gTimePlane;
const uint8_t* gLast0 = nullptr;
const uint8_t* gLast1 = nullptr;
int gLastW = 0, gLastH = 0;

std::string gName0, gName1, gNameT, gNameOut;
int gLayout = 0;
int gTimeRank = 1;

void ReleaseStatus(OrtStatus* st) {
    if (st && gApi) gApi->ReleaseStatus(st);
}

bool Ok(OrtStatus* st, std::string& err, const char* what) {
    if (!st) return true;
    const char* m = gApi ? gApi->GetErrorMessage(st) : nullptr;
    err = std::string(what) + ": " + (m ? m : "failed");
    ReleaseStatus(st);
    return false;
}

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
#endif

void ClearSession() {
    if (gApi && gSession) gApi->ReleaseSession(gSession);
    gSession = nullptr;
    if (gApi && gEnv) gApi->ReleaseEnv(gEnv);
    gEnv = nullptr;
    gApi = nullptr;
    gApiVersion = 0;
    gAlloc = nullptr;
    gLast0 = gLast1 = nullptr;
    gLastW = gLastH = 0;
    gLayout = 0;
    gInfo.ready = false;
    gInfo.cuda = false;
}

int Pad32(int v) { return (v + 31) & ~31; }

void ToPlanar(const uint8_t* rgba, int w, int h, int pw, int ph, float* dst, int channelOffset) {
    const float scale = 1.0f / 255.0f;
    size_t plane = (size_t)ph * (size_t)pw;
    float* r = dst + (size_t)channelOffset * plane;
    float* gch = r + plane;
    float* b = gch + plane;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = rgba + ((size_t)y * (size_t)w) * 4;
        float* rr = r + (size_t)y * (size_t)pw;
        float* gg = gch + (size_t)y * (size_t)pw;
        float* bb = b + (size_t)y * (size_t)pw;
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + (size_t)x * 4;
            rr[x] = p[0] * scale;
            gg[x] = p[1] * scale;
            bb[x] = p[2] * scale;
        }
        float er = rr[w - 1], eg = gg[w - 1], eb = bb[w - 1];
        for (int x = w; x < pw; x++) { rr[x] = er; gg[x] = eg; bb[x] = eb; }
    }
    for (int y = h; y < ph; y++) {
        memcpy(r + (size_t)y * (size_t)pw, r + (size_t)(h - 1) * (size_t)pw, (size_t)pw * sizeof(float));
        memcpy(gch + (size_t)y * (size_t)pw, gch + (size_t)(h - 1) * (size_t)pw, (size_t)pw * sizeof(float));
        memcpy(b + (size_t)y * (size_t)pw, b + (size_t)(h - 1) * (size_t)pw, (size_t)pw * sizeof(float));
    }
}

void FromPlanar(const float* src, int stride, int planeH, int w, int h, uint8_t* rgba) {
    size_t plane = (size_t)stride * (size_t)planeH;
    const float* r = src;
    const float* g = src + plane;
    const float* b = g + plane;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)stride + (size_t)x;
            uint8_t* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            auto u = [](float v) {
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return (uint8_t)(v * 255.0f + 0.5f);
            };
            p[0] = u(r[i]);
            p[1] = u(g[i]);
            p[2] = u(b[i]);
            p[3] = 255;
        }
    }
}

}  // namespace

const RifeInfo& RifeProbe() {
#ifndef _WIN32
    gInfo.files = false;
    gInfo.detail = "RIFE is available on Windows";
    return gInfo;
#else
    std::wstring dir = ExeDir();
    std::wstring model;
    const wchar_t* names[] = {L"models\\rife49.onnx", L"models\\rife.onnx", L"rife49.onnx", L"rife.onnx"};
    for (const wchar_t* n : names) {
        std::wstring p = dir + n;
        if (FileExists(p)) { model = p; break; }
    }
    bool dll = FileExists(dir + L"onnxruntime.dll");
    gInfo.files = !model.empty() && dll;
    if (gInfo.ready) return gInfo;
    if (model.empty() && !dll)
        gInfo.detail = "Put models\\rife49.onnx and onnxruntime.dll next to the program";
    else if (model.empty())
        gInfo.detail = "Put models\\rife49.onnx next to the program";
    else if (!dll)
        gInfo.detail = "Put onnxruntime.dll next to the program";
    else
        gInfo.detail = "RIFE model found";
    return gInfo;
#endif
}

bool RifeStartup(std::string& err) {
    RifeShutdown();
#ifndef _WIN32
    err = "RIFE is available on Windows";
    return false;
#else
    std::wstring dir = ExeDir();
    std::wstring dll = dir + L"onnxruntime.dll";
    gOrt = LoadLibraryW(dll.c_str());
    if (!gOrt) gOrt = LoadLibraryW(L"onnxruntime.dll");
    if (!gOrt) {
        err = "onnxruntime.dll was not found next to the program";
        gInfo.detail = err;
        return false;
    }
    using GetBase = const OrtApiBase*(ORT_API_CALL*)();
    auto getBase = (GetBase)GetProcAddress(gOrt, "OrtGetApiBase");
    if (!getBase) {
        err = "onnxruntime.dll is not usable";
        RifeShutdown();
        return false;
    }
    const OrtApiBase* base = getBase();
    gApi = nullptr;
    gApiVersion = 0;
    if (base) {
        for (uint32_t v = ORT_API_VERSION; v >= 8; --v) {
            const OrtApi* api = base->GetApi(v);
            if (api) { gApi = api; gApiVersion = (int)v; break; }
        }
    }
    if (!gApi) {
        err = "onnxruntime.dll is too old. Use ONNX Runtime 1.16 or newer.";
        gInfo.detail = err;
        RifeShutdown();
        return false;
    }
    std::wstring model;
    const wchar_t* names[] = {L"models\\rife49.onnx", L"models\\rife.onnx", L"rife49.onnx", L"rife.onnx"};
    for (const wchar_t* n : names) {
        std::wstring p = dir + n;
        if (FileExists(p)) { model = p; break; }
    }
    if (model.empty()) {
        err = "Put models\\rife49.onnx next to the program";
        gInfo.detail = err;
        RifeShutdown();
        return false;
    }
    if (!Ok(gApi->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "rife", &gEnv), err, "RIFE")) {
        RifeShutdown();
        return false;
    }
    OrtSessionOptions* opt = nullptr;
    if (!Ok(gApi->CreateSessionOptions(&opt), err, "RIFE")) {
        RifeShutdown();
        return false;
    }
    gApi->SetIntraOpNumThreads(opt, 8);
    gApi->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
    std::string cudaWhy;
#ifdef _WIN32
    SetDllDirectoryW(dir.c_str());
    gCudaShared = LoadLibraryW((dir + L"onnxruntime_providers_shared.dll").c_str());
    gCuda = LoadLibraryW((dir + L"onnxruntime_providers_cuda.dll").c_str());
    if (gApiVersion >= 14) {
        const char* keys[] = {"device_id", "arena_extend_strategy", "cudnn_conv_algo_search", "do_copy_in_default_stream", "cudnn_conv_use_max_workspace", "gpu_mem_limit"};
        const char* vals[] = {"0", "1", "HEURISTIC", "1", "1", "17179869184"};
        OrtStatus* st = gApi->SessionOptionsAppendExecutionProvider(opt, "CUDA", keys, vals, 6);
        if (!st) gInfo.cuda = true;
        else {
            cudaWhy = gApi->GetErrorMessage(st);
            gApi->ReleaseStatus(st);
            const char* k2[] = {"device_id"};
            const char* v2[] = {"0"};
            st = gApi->SessionOptionsAppendExecutionProvider(opt, "CUDA", k2, v2, 1);
            if (!st) { gInfo.cuda = true; cudaWhy.clear(); }
            else {
                cudaWhy = gApi->GetErrorMessage(st);
                gApi->ReleaseStatus(st);
            }
        }
    }
#endif
    if (!gInfo.cuda) {
        using AppendCuda = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
        auto appendCuda = (AppendCuda)GetProcAddress(gOrt, "OrtSessionOptionsAppendExecutionProvider_CUDA");
        if (appendCuda) {
            OrtStatus* cst = appendCuda(opt, 0);
            gInfo.cuda = cst == nullptr;
            if (cst) {
                if (cudaWhy.empty()) cudaWhy = gApi->GetErrorMessage(cst);
                gApi->ReleaseStatus(cst);
            }
        }
    }
    OrtStatus* created = gApi->CreateSession(gEnv, model.c_str(), opt, &gSession);
    gApi->ReleaseSessionOptions(opt);
    if (!Ok(created, err, "RIFE model")) {
        RifeShutdown();
        return false;
    }
    if (!Ok(gApi->GetAllocatorWithDefaultOptions(&gAlloc), err, "RIFE")) {
        RifeShutdown();
        return false;
    }

    size_t nIn = 0, nOut = 0;
    gApi->SessionGetInputCount(gSession, &nIn);
    gApi->SessionGetOutputCount(gSession, &nOut);
    if (nOut < 1 || nIn < 2) {
        err = "This ONNX file is not a RIFE model";
        RifeShutdown();
        return false;
    }
    std::vector<std::string> ins;
    for (size_t i = 0; i < nIn; i++) {
        char* name = nullptr;
        gApi->SessionGetInputName(gSession, i, gAlloc, &name);
        ins.push_back(name ? name : "");
        if (name) gAlloc->Free(gAlloc, name);
        OrtTypeInfo* type = nullptr;
        gApi->SessionGetInputTypeInfo(gSession, i, &type);
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        if (type) gApi->CastTypeInfoToTensorInfo(type, &tensor);
        size_t rank = 0;
        if (tensor) gApi->GetDimensionsCount(tensor, &rank);
        if (ins.back() == "timestep" || ins.back() == "t") gTimeRank = (int)rank;
        if (type) gApi->ReleaseTypeInfo(type);
    }
    char* on = nullptr;
    gApi->SessionGetOutputName(gSession, 0, gAlloc, &on);
    gNameOut = on ? on : "output";
    if (on) gAlloc->Free(gAlloc, on);

    bool has0 = false, has1 = false, hasT = false, packed = false;
    for (const auto& s : ins) {
        if (s == "img0") { gName0 = s; has0 = true; }
        else if (s == "img1") { gName1 = s; has1 = true; }
        else if (s == "timestep" || s == "t") { gNameT = s; hasT = true; }
        else if (s == "input") { packed = true; gName0 = s; }
    }
    if (has0 && has1 && hasT) gLayout = 1;
    else if (packed && hasT) gLayout = 2;
    else {
        err = "RIFE model inputs were not recognized";
        RifeShutdown();
        return false;
    }
    gInfo.ready = true;
    gInfo.files = true;
    gInfo.detail = std::string(gInfo.cuda ? "RIFE on GPU" : "RIFE on CPU") + " (ORT API " + std::to_string(gApiVersion) + ")";
    if (!gInfo.cuda) {
        gInfo.detail += ". CUDA was not used";
        if (!cudaWhy.empty()) gInfo.detail += ": " + cudaWhy;
    }
    return true;
#endif
}

void RifeShutdown() {
    ClearSession();
#ifdef _WIN32
    if (gOrt) { FreeLibrary(gOrt); gOrt = nullptr; }
    if (gCuda) { FreeLibrary(gCuda); gCuda = nullptr; }
    if (gCudaShared) { FreeLibrary(gCudaShared); gCudaShared = nullptr; }
#endif
    gInfo.ready = false;
    gInfo.cuda = false;
}

bool RifeRun(const uint8_t* rgba0, const uint8_t* rgba1, int w, int h, float t, uint8_t* rgbaOut) {
    if (!gApi || !gSession || !rgba0 || !rgba1 || !rgbaOut || w < 2 || h < 2) return false;
    if (w > 3840 || h > 3840) return false;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int pw = Pad32(w);
    int ph = Pad32(h);
    size_t plane = (size_t)pw * (size_t)ph;
    bool same = rgba0 == gLast0 && rgba1 == gLast1 && w == gLastW && h == gLastH && !gIn0.empty();
    if (!same) {
        gIn0.resize(gLayout == 2 ? plane * 6 : plane * 3);
        if (gLayout == 2) {
            ToPlanar(rgba0, w, h, pw, ph, gIn0.data(), 0);
            ToPlanar(rgba1, w, h, pw, ph, gIn0.data(), 3);
        } else {
            gIn1.resize(plane * 3);
            ToPlanar(rgba0, w, h, pw, ph, gIn0.data(), 0);
            ToPlanar(rgba1, w, h, pw, ph, gIn1.data(), 0);
        }
        gLast0 = rgba0;
        gLast1 = rgba1;
        gLastW = w;
        gLastH = h;
    }
    float timeScalar = t;
    int timeRank = gTimeRank < 0 ? 1 : gTimeRank;
    if (timeRank >= 4) {
        gTimePlane.assign(plane, t);
        timeRank = 4;
    }

    OrtMemoryInfo* mem = nullptr;
    if (gApi->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeCPU, &mem)) return false;
    auto make = [&](float* data, size_t count, const int64_t* shape, size_t rank, OrtValue*& out) {
        return gApi->CreateTensorWithDataAsOrtValue(mem, data, count * sizeof(float), shape, rank,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out) == nullptr;
    };
    OrtValue* v0 = nullptr;
    OrtValue* v1 = nullptr;
    OrtValue* vt = nullptr;
    int64_t shape3[4] = {1, 3, ph, pw};
    int64_t shape6[4] = {1, 6, ph, pw};
    int64_t shapeT1[1] = {1};
    int64_t shapeT4[4] = {1, 1, ph, pw};
    bool built = false;
    if (gLayout == 2) built = make(gIn0.data(), gIn0.size(), shape6, 4, v0);
    else built = make(gIn0.data(), gIn0.size(), shape3, 4, v0) && make(gIn1.data(), gIn1.size(), shape3, 4, v1);
    if (built) {
        if (timeRank >= 4) built = make(gTimePlane.data(), gTimePlane.size(), shapeT4, 4, vt);
        else built = make(&timeScalar, 1, shapeT1, timeRank == 0 ? 0 : 1, vt);
    }
    if (!built) {
        if (v0) gApi->ReleaseValue(v0);
        if (v1) gApi->ReleaseValue(v1);
        if (vt) gApi->ReleaseValue(vt);
        gApi->ReleaseMemoryInfo(mem);
        return false;
    }

    const char* names2[3] = {gName0.c_str(), gNameT.c_str(), nullptr};
    const char* names3[3] = {gName0.c_str(), gName1.c_str(), gNameT.c_str()};
    const OrtValue* vals2[2] = {v0, vt};
    const OrtValue* vals3[3] = {v0, v1, vt};
    const char* outName = gNameOut.c_str();
    OrtValue* result = nullptr;
    OrtStatus* st = nullptr;
    if (gLayout == 2)
        st = gApi->Run(gSession, nullptr, names2, vals2, 2, &outName, 1, &result);
    else
        st = gApi->Run(gSession, nullptr, names3, vals3, 3, &outName, 1, &result);
    gApi->ReleaseValue(v0);
    if (v1) gApi->ReleaseValue(v1);
    gApi->ReleaseValue(vt);
    gApi->ReleaseMemoryInfo(mem);
    if (st || !result) {
        ReleaseStatus(st);
        if (result) gApi->ReleaseValue(result);
        return false;
    }
    float* out = nullptr;
    if (gApi->GetTensorMutableData(result, (void**)&out) || !out) {
        gApi->ReleaseValue(result);
        return false;
    }
    int ow = pw, oh = ph;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (!gApi->GetTensorTypeAndShape(result, &info) && info) {
        size_t rank = 0;
        gApi->GetDimensionsCount(info, &rank);
        int64_t dims[6] = {};
        if (rank >= 4 && !gApi->GetDimensions(info, dims, rank)) {
            if (dims[3] > 0) ow = (int)dims[3];
            if (dims[2] > 0) oh = (int)dims[2];
        }
        gApi->ReleaseTensorTypeAndShapeInfo(info);
    }
    if (ow < w || oh < h) { ow = pw; oh = ph; }
    FromPlanar(out, ow, oh, w, h, rgbaOut);
    gApi->ReleaseValue(result);
    return true;
}
