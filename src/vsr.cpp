#include "vsr.h"
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
#include <cstdio>
#include <cstring>
#include <string>

namespace {

VsrInfo gInfo;
VsrShape gShape;
const OrtApi* gApi = nullptr;
int gApiVersion = 0;
OrtEnv* gEnv = nullptr;
OrtSession* gSession = nullptr;
OrtAllocator* gAlloc = nullptr;
#ifdef _WIN32
HMODULE gOrt = nullptr;
#endif

enum Layout { LayNone = 0, LayNTCHW, LayNCTHW, LayNTHWC, LayNCHW, LayNHWC };

Layout gLayout = LayNone;
int gWindow = 1;
std::string gImageName;
std::string gOutName;
std::string gScaleName;
int gScaleRank = -1;
ONNXTensorElementDataType gScaleType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
bool gCudaRegistered = false;
std::vector<float> gIn;
int64_t gScaleValue = 4;

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

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void Log(const std::wstring& dir, const std::string& line) {
    if (dir.empty()) return;
    FILE* f = _wfopen((dir + L"vsr.log").c_str(), L"ab");
    if (!f) return;
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
}

std::wstring FindModel(const std::wstring& dir) {
    const wchar_t* names[] = {
        L"models\\swinir_realsr_x4.onnx",
        L"swinir_realsr_x4.onnx"
    };
    for (const wchar_t* n : names) {
        std::wstring p = dir + n;
        if (FileExists(p)) return p;
    }
    return L"";
}
#endif

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

void ClearSession() {
    if (gApi && gSession) gApi->ReleaseSession(gSession);
    gSession = nullptr;
    if (gCudaRegistered && gApi && gEnv && gApiVersion >= 22) {
        OrtStatus* st = gApi->UnregisterExecutionProviderLibrary(gEnv, "cuda_ep");
        if (st) gApi->ReleaseStatus(st);
    }
    gCudaRegistered = false;
    if (gApi && gEnv) gApi->ReleaseEnv(gEnv);
    gEnv = nullptr;
    gApi = nullptr;
    gApiVersion = 0;
    gAlloc = nullptr;
    gLayout = LayNone;
    gWindow = 1;
    gImageName.clear();
    gOutName.clear();
    gScaleName.clear();
    gScaleRank = -1;
    gInfo.ready = false;
    gInfo.cuda = false;
    gShape = VsrShape();
    gIn.clear();
}

struct Spec {
    std::string name;
    int rank = 0;
    int64_t dims[8] = {};
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
};

int Dim(const Spec& s, int i) {
    if (i < 0 || i >= s.rank) return -1;
    return (int)s.dims[i];
}

bool ReadSpec(size_t index, bool input, Spec& spec, std::string& err) {
    spec = Spec();
    char* name = nullptr;
    OrtStatus* st = input ? gApi->SessionGetInputName(gSession, index, gAlloc, &name)
                          : gApi->SessionGetOutputName(gSession, index, gAlloc, &name);
    if (!Ok(st, err, "SwinIR")) return false;
    spec.name = name ? name : "";
    if (name) gAlloc->Free(gAlloc, name);
    OrtTypeInfo* type = nullptr;
    st = input ? gApi->SessionGetInputTypeInfo(gSession, index, &type)
               : gApi->SessionGetOutputTypeInfo(gSession, index, &type);
    if (!Ok(st, err, "SwinIR")) return false;
    const OrtTensorTypeAndShapeInfo* tensor = nullptr;
    if (type) gApi->CastTypeInfoToTensorInfo(type, &tensor);
    if (!tensor) {
        if (type) gApi->ReleaseTypeInfo(type);
        err = "SwinIR model input is not a tensor";
        return false;
    }
    size_t rank = 0;
    gApi->GetDimensionsCount(tensor, &rank);
    if (rank > 8) rank = 8;
    spec.rank = (int)rank;
    if (rank) gApi->GetDimensions(tensor, spec.dims, rank);
    gApi->GetTensorElementType(tensor, &spec.type);
    gApi->ReleaseTypeInfo(type);
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

Layout Detect(const Spec& s) {
    if (s.rank == 5) {
        if (Dim(s, 2) == 3) return LayNTCHW;
        if (Dim(s, 1) == 3) return LayNCTHW;
        if (Dim(s, 4) == 3) return LayNTHWC;
        return LayNTCHW;
    }
    if (s.rank == 4) {
        if (Dim(s, 3) == 3) return LayNHWC;
        return LayNCHW;
    }
    return LayNone;
}

const char* LayoutName(Layout lay) {
    if (lay == LayNTCHW) return "N,T,C,H,W";
    if (lay == LayNCTHW) return "N,C,T,H,W";
    if (lay == LayNTHWC) return "N,T,H,W,C";
    if (lay == LayNCHW) return "N,C,H,W";
    if (lay == LayNHWC) return "N,H,W,C";
    return "unknown";
}

int WindowOf(Layout lay, const Spec& s) {
    int t = -1;
    if (lay == LayNTCHW || lay == LayNTHWC) t = Dim(s, 1);
    else if (lay == LayNCTHW) t = Dim(s, 2);
    else return 1;
    if (t <= 0) return 7;
    return t;
}

void SpatialOf(Layout lay, const Spec& s, int& H, int& W) {
    H = 0;
    W = 0;
    int h = -1, w = -1;
    if (lay == LayNTCHW || lay == LayNCTHW) { h = Dim(s, 3); w = Dim(s, 4); }
    else if (lay == LayNTHWC) { h = Dim(s, 2); w = Dim(s, 3); }
    else if (lay == LayNCHW) { h = Dim(s, 2); w = Dim(s, 3); }
    else if (lay == LayNHWC) { h = Dim(s, 1); w = Dim(s, 2); }
    if (h > 0) H = h;
    if (w > 0) W = w;
}

size_t IndexOf(Layout lay, int T, int t, int c, int y, int x, int th, int tw) {
    if (lay == LayNTCHW) return (((size_t)t * 3 + c) * (size_t)th + (size_t)y) * (size_t)tw + (size_t)x;
    if (lay == LayNCTHW) return (((size_t)c * T + t) * (size_t)th + (size_t)y) * (size_t)tw + (size_t)x;
    if (lay == LayNTHWC) return (((size_t)t * (size_t)th + (size_t)y) * (size_t)tw + (size_t)x) * 3 + (size_t)c;
    if (lay == LayNHWC) return ((size_t)y * (size_t)tw + (size_t)x) * 3 + (size_t)c;
    return ((size_t)c * (size_t)th + (size_t)y) * (size_t)tw + (size_t)x;
}

void FillShape(Layout lay, int T, int th, int tw, int64_t* shape, size_t& rank) {
    if (lay == LayNTCHW) {
        shape[0] = 1; shape[1] = T; shape[2] = 3; shape[3] = th; shape[4] = tw; rank = 5;
    } else if (lay == LayNCTHW) {
        shape[0] = 1; shape[1] = 3; shape[2] = T; shape[3] = th; shape[4] = tw; rank = 5;
    } else if (lay == LayNTHWC) {
        shape[0] = 1; shape[1] = T; shape[2] = th; shape[3] = tw; shape[4] = 3; rank = 5;
    } else if (lay == LayNHWC) {
        shape[0] = 1; shape[1] = th; shape[2] = tw; shape[3] = 3; rank = 4;
    } else {
        shape[0] = 1; shape[1] = 3; shape[2] = th; shape[3] = tw; rank = 4;
    }
}

#ifdef _WIN32
bool AttachCuda(OrtSessionOptions* opt, const std::wstring& dir, std::string& why) {
    gInfo.cuda = false;
    SetDllDirectoryW(dir.c_str());
    bool loaded = RifePrepareCuda(why);
    auto note = [&](OrtStatus* st) {
        if (!st) return;
        const char* m = gApi->GetErrorMessage(st);
        if (m && *m) { why = m; Log(dir, std::string("CUDA provider error: ") + m); }
        gApi->ReleaseStatus(st);
    };
    if (loaded && gApiVersion >= 14) {
        const char* keys[] = {"device_id"};
        const char* vals[] = {"0"};
        OrtStatus* st = gApi->SessionOptionsAppendExecutionProvider(opt, "CUDAExecutionProvider", keys, vals, 1);
        if (!st) gInfo.cuda = true;
        else note(st);
        if (!gInfo.cuda) {
            st = gApi->SessionOptionsAppendExecutionProvider(opt, "CUDA", keys, vals, 1);
            if (!st) { gInfo.cuda = true; why.clear(); }
            else note(st);
        }
    }
    if (loaded && !gInfo.cuda && gApiVersion >= 22 && FileExists(dir + L"onnxruntime_providers_cuda.dll")) {
        std::wstring dll = dir + L"onnxruntime_providers_cuda.dll";
        OrtStatus* st = gApi->RegisterExecutionProviderLibrary(gEnv, "cuda_ep", dll.c_str());
        if (st) note(st);
        else {
            gCudaRegistered = true;
            const OrtEpDevice* const* devices = nullptr;
            size_t count = 0;
            st = gApi->GetEpDevices(gEnv, &devices, &count);
            const OrtEpDevice* chosen = nullptr;
            if (!st && devices) {
                for (size_t i = 0; i < count; i++) {
                    const char* name = gApi->EpDevice_EpName(devices[i]);
                    if (name && std::strstr(name, "CUDA")) { chosen = devices[i]; break; }
                }
            } else note(st);
            if (chosen) {
                st = gApi->SessionOptionsAppendExecutionProvider_V2(opt, gEnv, &chosen, 1, nullptr, nullptr, 0);
                if (!st) { gInfo.cuda = true; why.clear(); }
                else note(st);
            } else if (why.empty()) why = "CUDA provider loaded but no GPU device was found";
        }
    }
    if (loaded && !gInfo.cuda) {
        using AppendCuda = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
        auto appendCuda = (AppendCuda)GetProcAddress(gOrt, "OrtSessionOptionsAppendExecutionProvider_CUDA");
        if (appendCuda) {
            OrtStatus* cst = appendCuda(opt, 0);
            gInfo.cuda = cst == nullptr;
            if (cst) {
                if (why.empty()) why = gApi->GetErrorMessage(cst);
                gApi->ReleaseStatus(cst);
            }
        }
    }
    SetDllDirectoryW(dir.c_str());
    return gInfo.cuda;
}
#endif

}  // namespace

const VsrInfo& VsrProbe() {
#ifndef _WIN32
    gInfo.files = false;
    gInfo.detail = "SwinIR is available on Windows";
    return gInfo;
#else
    std::wstring dir = ExeDir();
    std::wstring model = FindModel(dir);
    bool dll = FileExists(dir + L"onnxruntime.dll");
    gInfo.files = !model.empty() && dll;
    if (gInfo.ready) return gInfo;
    if (model.empty() && !dll)
        gInfo.detail = "Put models\\swinir_realsr_x4.onnx and onnxruntime.dll next to the program";
    else if (model.empty())
        gInfo.detail = "Put models\\swinir_realsr_x4.onnx next to the program";
    else if (!dll)
        gInfo.detail = "Put onnxruntime.dll next to the program";
    else
        gInfo.detail = "SwinIR model found";
    return gInfo;
#endif
}

bool VsrStartup(std::string& err) {
    VsrShutdown();
#ifndef _WIN32
    err = "SwinIR is available on Windows";
    return false;
#else
    std::wstring dir = ExeDir();
    Log(dir, "---- SwinIR load ----");
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
        VsrShutdown();
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
        VsrShutdown();
        return false;
    }
    std::wstring model = FindModel(dir);
    if (model.empty()) {
        err = "Put models\\swinir_realsr_x4.onnx next to the program";
        gInfo.detail = err;
        VsrShutdown();
        return false;
    }
    Log(dir, "model " + Narrow(model));
    if (!Ok(gApi->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "vsr", &gEnv), err, "SwinIR")) {
        VsrShutdown();
        return false;
    }
    OrtSessionOptions* opt = nullptr;
    if (!Ok(gApi->CreateSessionOptions(&opt), err, "SwinIR")) {
        VsrShutdown();
        return false;
    }
    gApi->SetIntraOpNumThreads(opt, 8);
    gApi->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
    std::string cudaWhy;
    AttachCuda(opt, dir, cudaWhy);
    OrtStatus* created = gApi->CreateSession(gEnv, model.c_str(), opt, &gSession);
    gApi->ReleaseSessionOptions(opt);
    if (!Ok(created, err, "SwinIR model")) {
        VsrShutdown();
        return false;
    }
    if (!Ok(gApi->GetAllocatorWithDefaultOptions(&gAlloc), err, "SwinIR")) {
        VsrShutdown();
        return false;
    }
    size_t nIn = 0, nOut = 0;
    gApi->SessionGetInputCount(gSession, &nIn);
    gApi->SessionGetOutputCount(gSession, &nOut);
    if (nIn < 1 || nOut < 1) {
        err = "This ONNX file is not a SwinIR model";
        VsrShutdown();
        return false;
    }
    std::vector<Spec> inputs;
    std::string listed;
    for (size_t i = 0; i < nIn; i++) {
        Spec spec;
        if (!ReadSpec(i, true, spec, err)) { VsrShutdown(); return false; }
        inputs.push_back(spec);
        if (!listed.empty()) listed += ", ";
        listed += spec.name.empty() ? "?" : spec.name;
        listed += " rank " + std::to_string(spec.rank);
        Log(dir, "input " + spec.name + " rank " + std::to_string(spec.rank));
    }
    Spec image;
    bool haveImage = false;
    int imageCount = 0;
    for (const Spec& spec : inputs) {
        std::string low = Lower(spec.name);
        bool scaleName = low.find("scale") != std::string::npos;
        if (scaleName && spec.rank <= 1) {
            gScaleName = spec.name;
            gScaleRank = spec.rank;
            gScaleType = spec.type;
            continue;
        }
        if (spec.rank >= 4) {
            imageCount++;
            if (!haveImage) { image = spec; haveImage = true; }
        }
    }
    if (!haveImage || imageCount != 1 || inputs.size() > (gScaleName.empty() ? 1u : 2u)) {
        err = "SwinIR inputs were not recognized (" + listed + ")";
        gInfo.detail = err;
        VsrShutdown();
        return false;
    }
    if (image.type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        err = "SwinIR model must use float frames";
        gInfo.detail = err;
        VsrShutdown();
        return false;
    }
    gLayout = Detect(image);
    if (gLayout == LayNone) {
        err = "SwinIR inputs were not recognized (" + listed + ")";
        VsrShutdown();
        return false;
    }
    gWindow = WindowOf(gLayout, image);
    if (gWindow > 31) {
        err = "This SwinIR model wants too many frames at once";
        gInfo.detail = err;
        VsrShutdown();
        return false;
    }
    if (gWindow < 1) gWindow = 1;
    gImageName = image.name.empty() ? "lqs" : image.name;
    int fh = 0, fw = 0;
    SpatialOf(gLayout, image, fh, fw);
    Spec outSpec;
    if (!ReadSpec(0, false, outSpec, err)) { VsrShutdown(); return false; }
    gOutName = outSpec.name.empty() ? "output" : outSpec.name;
    Log(dir, std::string("output ") + gOutName + " rank " + std::to_string(outSpec.rank));
    int oh = 0, ow = 0;
    Layout outLay = Detect(outSpec);
    SpatialOf(outLay == LayNone ? gLayout : outLay, outSpec, oh, ow);
    int hint = 0;
    if (fh > 0 && oh > 0 && (oh % fh) == 0) hint = oh / fh;
    gShape.window = gWindow;
    gShape.fixedW = fw;
    gShape.fixedH = fh;
    gShape.scaleHint = hint;
    gInfo.ready = true;
    gInfo.files = true;
    gInfo.detail = std::string(gInfo.cuda ? "SwinIR on GPU" : "SwinIR on CPU. See vsr.log");
    gInfo.detail += " window " + std::to_string(gWindow);
    Log(dir, gInfo.detail + " layout " + LayoutName(gLayout));
    if (hint) Log(dir, "graph scale " + std::to_string(hint));
    if (!cudaWhy.empty()) Log(dir, "detail: " + cudaWhy);
    return true;
#endif
}

void VsrShutdown() {
    ClearSession();
#ifdef _WIN32
    if (gOrt) { FreeLibrary(gOrt); gOrt = nullptr; }
#endif
    gInfo.ready = false;
    gInfo.cuda = false;
}

VsrShape VsrQuery() { return gShape; }

bool VsrRunTile(const std::vector<std::vector<uint8_t>>& frames, int w, int h,
                int x0, int y0, int tw, int th, std::vector<uint8_t>& hrOut, int& scaleOut, std::string& err) {
    scaleOut = 0;
    if (!gApi || !gSession || tw < 2 || th < 2 || w < 2 || h < 2) {
        err = "SwinIR is not loaded";
        return false;
    }
    if ((gShape.fixedW > 0 && tw != gShape.fixedW) || (gShape.fixedH > 0 && th != gShape.fixedH)) {
        err = "SwinIR only accepts its fixed tile size";
        return false;
    }
    int T = gWindow;
    if ((int)frames.size() < T) {
        err = "SwinIR did not get enough frames";
        return false;
    }
    size_t need = (size_t)w * (size_t)h * 4;
    for (int i = 0; i < T; i++) {
        if (frames[(size_t)i].size() < need) {
            err = "SwinIR frame is the wrong size";
            return false;
        }
    }
    size_t count = (size_t)T * 3 * (size_t)th * (size_t)tw;
    gIn.assign(count, 0.0f);
    const float scale = 1.0f / 255.0f;
    for (int t = 0; t < T; t++) {
        const uint8_t* src = frames[(size_t)t].data();
        for (int y = 0; y < th; y++) {
            int sy = y0 + y;
            if (sy < 0) sy = 0;
            if (sy >= h) sy = h - 1;
            for (int x = 0; x < tw; x++) {
                int sx = x0 + x;
                if (sx < 0) sx = 0;
                if (sx >= w) sx = w - 1;
                const uint8_t* p = src + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
                gIn[IndexOf(gLayout, T, t, 0, y, x, th, tw)] = p[0] * scale;
                gIn[IndexOf(gLayout, T, t, 1, y, x, th, tw)] = p[1] * scale;
                gIn[IndexOf(gLayout, T, t, 2, y, x, th, tw)] = p[2] * scale;
            }
        }
    }
    OrtMemoryInfo* mem = nullptr;
    if (gApi->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeCPU, &mem)) {
        err = "SwinIR could not use memory";
        return false;
    }
    int64_t shape[5] = {};
    size_t rank = 0;
    FillShape(gLayout, T, th, tw, shape, rank);
    OrtValue* input = nullptr;
    OrtValue* scaleVal = nullptr;
    float scaleF = 4.0f;
    int32_t scaleI = 4;
    int64_t scaleL = 4;
    bool built = gApi->CreateTensorWithDataAsOrtValue(mem, gIn.data(), gIn.size() * sizeof(float), shape, rank,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input) == nullptr;
    if (built && !gScaleName.empty()) {
        scaleL = gShape.scaleHint > 0 ? gShape.scaleHint : 4;
        scaleF = (float)scaleL;
        scaleI = (int32_t)scaleL;
        gScaleValue = scaleL;
        int64_t ss[1] = {1};
        const int64_t* sp = gScaleRank == 0 ? nullptr : ss;
        size_t sr = gScaleRank == 0 ? 0 : 1;
        if (gScaleType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            built = gApi->CreateTensorWithDataAsOrtValue(mem, &scaleF, sizeof(scaleF), sp, sr, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &scaleVal) == nullptr;
        else if (gScaleType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
            built = gApi->CreateTensorWithDataAsOrtValue(mem, &scaleI, sizeof(scaleI), sp, sr, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &scaleVal) == nullptr;
        else
            built = gApi->CreateTensorWithDataAsOrtValue(mem, &scaleL, sizeof(scaleL), sp, sr, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &scaleVal) == nullptr;
    }
    if (!built || !input) {
        if (input) gApi->ReleaseValue(input);
        if (scaleVal) gApi->ReleaseValue(scaleVal);
        gApi->ReleaseMemoryInfo(mem);
        err = "SwinIR could not build the input";
        return false;
    }
    const char* names[2] = {gImageName.c_str(), gScaleName.c_str()};
    const OrtValue* vals[2] = {input, scaleVal};
    size_t nIn = gScaleName.empty() ? 1 : 2;
    const char* outName = gOutName.c_str();
    OrtValue* result = nullptr;
    OrtStatus* st = gApi->Run(gSession, nullptr, names, vals, nIn, &outName, 1, &result);
    gApi->ReleaseValue(input);
    if (scaleVal) gApi->ReleaseValue(scaleVal);
    gApi->ReleaseMemoryInfo(mem);
    if (st || !result) {
        if (st) {
            const char* m = gApi->GetErrorMessage(st);
            err = std::string("SwinIR: ") + (m ? m : "run failed");
            gApi->ReleaseStatus(st);
        } else err = "SwinIR returned nothing";
        if (result) gApi->ReleaseValue(result);
        return false;
    }
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (gApi->GetTensorTypeAndShape(result, &info) || !info) {
        gApi->ReleaseValue(result);
        err = "SwinIR output has no shape";
        return false;
    }
    size_t orank = 0;
    gApi->GetDimensionsCount(info, &orank);
    int64_t dims[8] = {};
    if (orank > 8) orank = 8;
    if (orank) gApi->GetDimensions(info, dims, orank);
    ONNXTensorElementDataType otype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    gApi->GetTensorElementType(info, &otype);
    gApi->ReleaseTensorTypeAndShapeInfo(info);

    int hAxis = -1, wAxis = -1, oh = 0, ow = 0, found = 0;
    for (int i = 0; i < (int)orank && !found; i++) {
        for (int j = 0; j < (int)orank; j++) {
            if (i == j || dims[i] <= 0 || dims[j] <= 0) continue;
            if (dims[i] % th || dims[j] % tw) continue;
            int sh = (int)(dims[i] / th);
            int sw = (int)(dims[j] / tw);
            if (sh == sw && sh >= 1 && sh <= 8) {
                hAxis = i; wAxis = j; oh = (int)dims[i]; ow = (int)dims[j]; found = sh;
                break;
            }
        }
    }
    if (!found || hAxis < 0) {
        gApi->ReleaseValue(result);
        err = "SwinIR output size was not recognized";
        return false;
    }
    int cAxis = -1, tAxis = -1;
    for (int i = 0; i < (int)orank; i++) {
        if (i == hAxis || i == wAxis) continue;
        if (cAxis < 0 && dims[i] == 3) { cAxis = i; continue; }
        if (tAxis < 0) tAxis = i;
    }
    if (cAxis < 0) {
        gApi->ReleaseValue(result);
        err = "SwinIR output is not RGB";
        return false;
    }
    int center = 0;
    if (tAxis >= 0 && dims[tAxis] > 1) center = (int)(dims[tAxis] / 2);
    size_t stride[8] = {};
    if (orank) stride[orank - 1] = 1;
    for (int i = (int)orank - 2; i >= 0; i--) stride[i] = stride[i + 1] * (size_t)std::max<int64_t>(1, dims[i + 1]);

    void* raw = nullptr;
    if (gApi->GetTensorMutableData(result, &raw) || !raw) {
        gApi->ReleaseValue(result);
        err = "SwinIR output is empty";
        return false;
    }
    auto at = [&](int y, int x, int c) -> float {
        int idx[8] = {};
        idx[hAxis] = y;
        idx[wAxis] = x;
        idx[cAxis] = c;
        if (tAxis >= 0) idx[tAxis] = center;
        size_t off = 0;
        for (int i = 0; i < (int)orank; i++) off += (size_t)idx[i] * stride[i];
        if (otype == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) return ((uint8_t*)raw)[off] / 255.0f;
        return ((float*)raw)[off];
    };
    if (otype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && otype != ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {
        gApi->ReleaseValue(result);
        err = "SwinIR output is not a float image";
        return false;
    }
    float peak = 0.0f;
    if (otype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        int step = std::max(1, ow / 32);
        for (int y = 0; y < oh; y += step) {
            for (int x = 0; x < ow; x += step) {
                peak = std::max(peak, std::max(at(y, x, 0), std::max(at(y, x, 1), at(y, x, 2))));
            }
        }
    }
    float mul = peak > 16.0f ? (1.0f / 255.0f) : 1.0f;
    hrOut.assign((size_t)ow * (size_t)oh * 4, 255);
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            uint8_t* p = hrOut.data() + ((size_t)y * (size_t)ow + (size_t)x) * 4;
            auto u = [&](float v) {
                v *= mul;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return (uint8_t)(v * 255.0f + 0.5f);
            };
            p[0] = u(at(y, x, 0));
            p[1] = u(at(y, x, 1));
            p[2] = u(at(y, x, 2));
            p[3] = 255;
        }
    }
    gApi->ReleaseValue(result);
    scaleOut = found;
    return true;
}
