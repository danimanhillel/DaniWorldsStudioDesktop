#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_syswm.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <map>
#include <deque>
#include <sstream>
#include "effects.h"
#include "overlay_fx.h"
#include "fx_plate.h"
#include "trans_plates.h"
#include "rife.h"
#include "asr.h"

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}
#endif

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <wincodec.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

struct MediaItem {
    std::string path;
    std::string name;
    std::string type;
    float duration = 5.0f;
    float aspect = 16.0f / 9.0f;
    GLuint thumb = 0;
    int thumbW = 0;
    int thumbH = 0;
};

struct VolPoint {
    float norm = 0.0f;
    float gain = 1.0f;
};

struct ClipFx {
    float z1[2][8]{};
    float z2[2][8]{};
    std::vector<float> comb[2][4];
    int combPos[2][4]{};
    std::vector<float> ap[2][2];
    int apPos[2][2]{};
    std::vector<float> delay;
    int delayPos = 0;
    int delayLen = 0;
    double lastEnd = -1.0;
    void clear() {
        memset(z1, 0, sizeof(z1));
        memset(z2, 0, sizeof(z2));
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < 4; k++) {
                std::fill(comb[ch][k].begin(), comb[ch][k].end(), 0.0f);
                combPos[ch][k] = 0;
            }
            for (int k = 0; k < 2; k++) {
                std::fill(ap[ch][k].begin(), ap[ch][k].end(), 0.0f);
                apPos[ch][k] = 0;
            }
        }
        std::fill(delay.begin(), delay.end(), 0.0f);
        delayPos = 0;
        lastEnd = -1.0;
    }
};

struct Clip {
    int mediaId = -1;
    float startTime = 0.0f;
    float duration = 5.0f;
    float sourceOffset = 0.0f;
    float speed = 1.0f;
    bool fadeIn = false;
    bool fadeOut = false;
    float opIn = 0.0f;
    float opOut = 0.0f;
    float volIn = 0.0f;
    float volOut = 0.0f;
    bool audioOpen = false;
    int track = 0;
    std::vector<VolPoint> volume;
    bool eqOn = false;
    float eqGain[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool reverbOn = false;
    float reverbTime = 1.4f;
    bool delayOn = false;
    float delayTempo = 120.0f;
    std::string fxOut;
    float fxDur = 1.0f;
    float keySim = 0.35f;
    float keySoft = 0.22f;
    float keySpill = 0.80f;
    float gsX = 0.0f;
    float gsY = 0.0f;
    float gsScale = 1.0f;
    float gsAspect = 1.0f;
    float gsRot = 0.0f;
    float gsOpacity = 1.0f;
    int gsFit = 0;
    bool muted = false;
    float gradeLight = 0.0f;
    float gradeContrast = 0.0f;
    float gradeWarm = 0.0f;
    float gradeTint = 0.0f;
    float gradeSat = 0.0f;
    float gradeShadow = 0.0f;
    float gradeGrain = 0.0f;
    float gradeSharp = 0.0f;
    ClipFx fx;
};

struct ThumbBlit {
    ImVec2 a;
    ImVec2 b;
    ImVec2 c0;
    ImVec2 c1;
    GLuint tex = 0;
    int w = 0;
    int h = 0;
};

struct BusSegment {
    float start = 0.0f;
    float duration = 5.0f;
    float lightness = 0.0f;
    float contrast = 0.0f;
    float warmth = 0.0f;
    float tint = 0.0f;
    float saturation = 0.0f;
    float shadow = 0.0f;
    float grain = 0.0f;
    float sharpen = 0.0f;
};

struct Subtitle {
    float start = 0.0f;
    float duration = 3.0f;
    std::string text;
};

struct SubStyle {
    float text[3] = {1.0f, 1.0f, 1.0f};
    bool border = true;
    float borderPx = 2.2f;
    float edge[3] = {0.0f, 0.0f, 0.0f};
    bool background = false;
    float bg[3] = {0.0f, 0.0f, 0.0f};
    float bgAlpha = 0.55f;
    float fontScale = 1.0f;
};

const int kGsTrack = 6;

std::vector<MediaItem> media;
std::vector<Clip> clips;
std::vector<BusSegment> bus;
std::vector<Subtitle> subs;
SubStyle subStyle;
std::vector<ThumbBlit> thumbBlits;
float currentTime = 0.0f;
float pixelsPerSecond = 50.0f;
float masterGain = 1.0f;
float masterRawL = 0.0f;
float masterRawR = 0.0f;
bool isPlaying = false;
bool repeatOn = false;
bool snapEnabled = true;
int selectedClip = -1;
std::vector<int> selection;
int fxOther = -1;
float fxProgress = 0.0f;
std::string fxActiveId;
int activeTool = 0;
std::string previewStatus = "Ready";
bool draggingPlayhead = false;
int draggingClip = -1;
float dragOffsetX = 0.0f;
ImVec2 previewMin(0, 0), previewMax(0, 0);
ImVec2 previewFrameMin(0, 0), previewFrameMax(0, 0);
ImVec2 previewPanelMin(0, 0), previewPanelMax(0, 0);
bool previewFrameValid = false;
static int bgShow[2] = {-1, -1};
static int bgShowN = 0;
float shownLocal = -1000.0f;
int trimmingClip = -1;
int trimSide = 0;
float trimAnchorX = 0;
float trimOrigStart = 0, trimOrigDur = 0, trimOrigOffset = 0;
int contextClip = -1;
int fxEdit = -1;
bool openClipMenu = false;
int dragVolClip = -1;
int dragVolPt = -1;
int dragVolRole = 0;
int fadeDragClip = -1;
int fadeDragSide = 0;
int fadeDragKind = 0;
float frameDebt = 0.0f;
float timeOriginX = 0.0f;
int exAspect = 0;
static void SplitLongSubs();
int exRes = 1;
int exFpsIdx = 1;
bool exRife = false;
int exSubMode = 0;
int exOutKind = 0;
bool asrOn = false;
int asrPhase = 0;
int asrLang = 0;
int asrChunk = 0;
double asrCursor = 0;
double asrEnd = 0;
std::vector<float> asrPcm;
std::vector<AsrSeg> asrSegs;
std::vector<int> asrList;
int asrListPos = 0;
std::vector<std::pair<int, int>> asrSpans;
int asrSpanPos = 0;
bool asrSpansReady = false;
float asrBase = 0;
float asrSpeed = 1.0f;
int asrOff = 0;
bool requestExport = false;
int mediaMenuIndex = -1;
int mediaArm = -1;
float mediaArmTravel = 0.0f;
bool openMediaMenu = false;
bool mediaPanelOpen = false;
ImVec2 mediaMenuPos(18.0f, 18.0f);
bool audioPanelOpen = false;
ImVec4 mediaPanelRect(0, 0, 0, 0);
ImVec4 audioPanelRect(0, 0, 0, 0);
int selectedBus = -1;
int contextBus = -1;
bool openBusMenu = false;
bool busGradeOpen = false;
bool busPanelHoldsMouse = false;
ImVec4 busPanelRect(0, 0, 0, 0);
int selectedSub = -1;
std::vector<int> subSelection;
int marqueeKind = 0;
ImVec2 marqueeStart(0, 0);
bool marqueeLive = false;
bool marqueeMoved = false;
ImVec2 trackLanePos[6];
float tlTrackH = 42.0f;
float tlViewW = 640.0f;
float trackLaneH[6] = {78, 78, 78, 78, 78, 78};
float gsChildH = 78.0f;
static const float kClipBody = 52.0f;
static const float kClipWave = 36.0f;
static const float kClipGap = 2.0f;
struct DragOrig { int idx; float orig; };
std::vector<DragOrig> groupClipDrag;
std::vector<DragOrig> groupSubDrag;
std::vector<int> groupSkip;
float groupAnchor = 0;
int editingSub = -1;
char subEditBuf[512] = {0};
bool subEditorOpen = false;
bool subStyleOpen = false;
int gsPanelClip = -1;
bool gsPanelOpen = false;
int xformClip = -1;
bool xformOpen = false;
int xformDrag = 0;
bool openedPanelThisFrame = false;
bool gsPanelHolds = false;
bool xformHolds = false;
bool subPanelHolds = false;
ImVec4 gsPanelRect(0, 0, 0, 0);
ImVec4 xformRect(0, 0, 0, 0);
ImVec4 subPanelRect(0, 0, 0, 0);
ImVec4 subStyleRect(0, 0, 0, 0);
float gsOriginX = 0.0f;
float gsLaneY = 0.0f;
float subsOriginX = 0.0f;
float subsLaneY = 0.0f;
int draggingSub = -1;
float subDragOffset = 0.0f;
int trimmingSub = -1;
int trimSubSide = 0;
bool recordFit = false;
bool fitValid = false;
ImVec2 fitA(0, 0), fitB(0, 0);
int gsDrawClip = -1;
ImFont* subtitleFont = nullptr;
BusSegment previewGrade;
bool previewGradeOn = false;
int colorClip = -1;
bool colorOpen = false;
bool colorHolds = false;
ImVec4 colorRect(0, 0, 0, 0);
int draggingOv = -1;
int trimmingOv = -1;
int trimOvSide = 0;
float ovDragOffset = 0.0f;
float ovOriginX = 0.0f;
int ovDragMode = 0;
int ovDragIndex = -1;
bool ovPanelOpen = false;
int ovPanel = -1;
bool ovPanelHolds = false;
ImVec4 ovPanelRect(0, 0, 0, 0);
int fxHover = -1;
double libClock = 0.0;
std::vector<unsigned char> fontAtlas;
int fontAtlasW = 0;
int fontAtlasH = 0;
bool busHover = false;
float busHoverTime = -1.0f;
float busOriginX = 0.0f;
int draggingBus = -1;
float busDragOffset = 0.0f;
int trimmingBus = -1;
int trimBusSide = 0;
const BusSegment* activeGrade = nullptr;

typedef void (APIENTRY *PFN_glUseProgramProc)(GLuint);
PFN_glUseProgramProc pglUseProgram = nullptr;

#ifdef HAVE_FFMPEG
struct AudioSrc {
    int mediaId = -1;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int stream = -1;
    bool failed = false;
    bool noAudio = false;
    bool atEnd = false;
    double nextTime = -1;
    std::vector<int16_t> fifo;
};

std::vector<AudioSrc> audioSrcs;
SDL_AudioDeviceID audioDev = 0;

struct HwDec {
    AVBufferRef* device = nullptr;
    AVFrame* sw = nullptr;
    AVPixelFormat pix = AV_PIX_FMT_NONE;
    const char* name = "CPU";
    bool on = false;
    void Free() {
        if (sw) av_frame_free(&sw);
        if (device) av_buffer_unref(&device);
        pix = AV_PIX_FMT_NONE;
        name = "CPU";
        on = false;
    }
};

static AVPixelFormat HwGetFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    AVPixelFormat want = (ctx && ctx->opaque) ? ((HwDec*)ctx->opaque)->pix : AV_PIX_FMT_NONE;
    if (want != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == want) return *p;
    }
    return fmts[0];
}

static bool AttachHw(AVCodecContext* ctx, const AVCodec* codec, HwDec& hw) {
    const AVHWDeviceType types[2] = { AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_D3D11VA };
    const char* names[2] = { "NVDEC", "D3D11" };
    for (int t = 0; t < 2; t++) {
        AVPixelFormat pix = AV_PIX_FMT_NONE;
        bool found = false;
        for (int i = 0;; i++) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == types[t]) {
                found = true;
                pix = cfg->pix_fmt;
                break;
            }
        }
        if (!found) continue;
        AVBufferRef* dev = nullptr;
        if (av_hwdevice_ctx_create(&dev, types[t], nullptr, nullptr, 0) < 0) continue;
        hw.Free();
        hw.device = dev;
        hw.pix = pix;
        hw.name = names[t];
        hw.on = true;
        ctx->hw_device_ctx = av_buffer_ref(dev);
        ctx->opaque = &hw;
        ctx->get_format = HwGetFormat;
        ctx->thread_count = 1;
        return true;
    }
    return false;
}

static bool OpenDecoder(AVCodecContext*& ctx, const AVCodec* codec, const AVCodecParameters* par, HwDec*& hw, const char*& how) {
    how = "CPU";
    if (!codec || !par) return false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ctx) avcodec_free_context(&ctx);
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        if (avcodec_parameters_to_context(ctx, par) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        if (attempt == 0) {
            if (!hw) hw = new HwDec();
            else hw->Free();
            if (!AttachHw(ctx, codec, *hw)) continue;
        } else if (hw) {
            hw->Free();
        }
        if (avcodec_open2(ctx, codec, nullptr) >= 0) {
            if (attempt == 0 && hw && hw->on && ctx->pix_fmt == hw->pix) how = hw->name;
            else how = "CPU";
            return true;
        }
    }
    if (ctx) avcodec_free_context(&ctx);
    if (hw) hw->Free();
    return false;
}

static AVFrame* DownloadFrame(HwDec* hw, AVFrame* frame) {
    if (!frame) return nullptr;
    if (!hw || !hw->on || frame->format != hw->pix) return frame;
    if (!hw->sw) hw->sw = av_frame_alloc();
    if (!hw->sw) return nullptr;
    av_frame_unref(hw->sw);
    if (av_hwframe_transfer_data(hw->sw, frame, 0) < 0) return nullptr;
    hw->sw->best_effort_timestamp = frame->best_effort_timestamp;
    return hw->sw;
}

void BuildSolidPixels(int kind, std::vector<uint8_t>& px, int W, int H);
int SolidKindFromPath(const std::string& path);
bool IsSolidPath(const std::string& path);
bool SolidHasAlpha(const std::string& path);
void EnsureSolidThumbs();
static GLuint gSolidTex[3] = {0, 0, 0};

class VideoPlayer {
public:
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    uint8_t* rgbBuffer = nullptr;
    HwDec* hw = nullptr;
    int videoStream = -1;
    int width = 0, height = 0;
    int srcW = 0, srcH = 0;
    GLuint texture = 0;
    bool hasFrame = false;
    bool still = false;
    double lastPts = -1;
    AVPixelFormat swsFmt = AV_PIX_FMT_NONE;
    const char* decName = "CPU";
    std::string lastError;

    ~VideoPlayer() { Close(); }

    bool Open(const std::string& path) {
        Close();
        if (path.rfind("solid:", 0) == 0) {
            int kind = 0;
            if (path == "solid:bars") kind = 1;
            else if (path == "solid:green") kind = 2;
            std::vector<uint8_t> px;
            BuildSolidPixels(kind, px, 640, 360);
            still = true;
            decName = "Solid";
            lastPts = 0;
            lastError.clear();
            return ShowRGBA(px.data(), 640, 360);
        }
        if (avformat_open_input(&fmtCtx, path.c_str(), nullptr, nullptr) < 0) {
            lastError = "Cannot open file"; return false;
        }
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            lastError = "No stream info"; return false;
        }
        videoStream = -1;
        for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStream = (int)i; break;
            }
        }
        if (videoStream < 0) { lastError = "No video stream"; return false; }
        const AVCodec* codec = avcodec_find_decoder(fmtCtx->streams[videoStream]->codecpar->codec_id);
        if (!codec) { lastError = "No decoder"; return false; }
        const char* openedAs = "CPU";
        if (!OpenDecoder(codecCtx, codec, fmtCtx->streams[videoStream]->codecpar, hw, openedAs)) {
            lastError = "Cannot open codec"; return false;
        }
        decName = openedAs;
        frame = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        lastError.clear();
        return true;
    }

    bool EnsureTextureFrom(AVFrame* src) {
        if (!src || src->width <= 0 || src->height <= 0) return false;
        AVPixelFormat srcFmt = (AVPixelFormat)src->format;
        int dw = src->width;
        int dh = src->height;
        int edge = std::max(dw, dh);
        if (edge > 960) {
            double s = 960.0 / (double)edge;
            dw = std::max(2, (int)std::lround(src->width * s) & ~1);
            dh = std::max(2, (int)std::lround(src->height * s) & ~1);
        }
        if (texture && swsCtx && width == dw && height == dh && swsFmt == srcFmt && srcW == src->width && srcH == src->height) return true;
        if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
        if (rgbBuffer) { av_free(rgbBuffer); rgbBuffer = nullptr; }
        if (!rgbFrame) rgbFrame = av_frame_alloc();
        srcW = src->width;
        srcH = src->height;
        width = dw;
        height = dh;
        swsFmt = srcFmt;
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
        rgbBuffer = (uint8_t*)av_malloc(numBytes);
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer, AV_PIX_FMT_RGBA, width, height, 1);
        swsCtx = sws_getContext(srcW, srcH, srcFmt, width, height, AV_PIX_FMT_RGBA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) { lastError = "sws failed"; return false; }
        if (!texture) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        return true;
    }

    bool ShowRGBA(const uint8_t* rgba, int w, int h) {
        if (!rgba || w < 2 || h < 2) return false;
        if (!texture) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (width != w || height != h || !hasFrame) {
            width = w;
            height = h;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        hasFrame = true;
        return true;
    }

    void Seek(float seconds) {
        if (still) { lastPts = 0; return; }
        if (!fmtCtx || videoStream < 0 || !codecCtx) return;
        int64_t ts = (int64_t)(seconds / av_q2d(fmtCtx->streams[videoStream]->time_base));
        av_seek_frame(fmtCtx, videoStream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx);
        lastPts = -1;
    }

    bool GrabFrame() {
        if (still) { lastPts = 0; return hasFrame; }
        if (!fmtCtx || !codecCtx) return false;
        AVPacket* packet = av_packet_alloc();
        int reads = 0;
        while (reads < 64 && av_read_frame(fmtCtx, packet) >= 0) {
            reads++;
            if (packet->stream_index == videoStream) {
                int send = avcodec_send_packet(codecCtx, packet);
                if (send == 0 || send == AVERROR(EAGAIN)) {
                    if (avcodec_receive_frame(codecCtx, frame) == 0) {
                        if (frame->best_effort_timestamp != AV_NOPTS_VALUE && fmtCtx && videoStream >= 0)
                            lastPts = frame->best_effort_timestamp * av_q2d(fmtCtx->streams[videoStream]->time_base);
                        else {
                            double step = 1.0 / 30.0;
                            if (codecCtx->framerate.num > 0 && codecCtx->framerate.den > 0)
                                step = (double)codecCtx->framerate.den / (double)codecCtx->framerate.num;
                            lastPts = (lastPts < 0.0 ? 0.0 : lastPts) + step;
                        }
                        AVFrame* src = DownloadFrame(hw, frame);
                        if (!src) { av_packet_free(&packet); return false; }
                        if (!EnsureTextureFrom(src)) { av_packet_free(&packet); return false; }
                        sws_scale(swsCtx, src->data, src->linesize, 0, srcH, rgbFrame->data, rgbFrame->linesize);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glBindTexture(GL_TEXTURE_2D, texture);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbFrame->data[0]);
                        hasFrame = true;
                        av_packet_free(&packet);
                        return true;
                    }
                }
            }
            av_packet_unref(packet);
        }
        avcodec_send_packet(codecCtx, nullptr);
        if (avcodec_receive_frame(codecCtx, frame) == 0) {
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE && fmtCtx && videoStream >= 0)
                lastPts = frame->best_effort_timestamp * av_q2d(fmtCtx->streams[videoStream]->time_base);
            else lastPts = (lastPts < 0.0 ? 0.0 : lastPts);
            AVFrame* src = DownloadFrame(hw, frame);
            if (src && EnsureTextureFrom(src)) {
                sws_scale(swsCtx, src->data, src->linesize, 0, srcH, rgbFrame->data, rgbFrame->linesize);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbFrame->data[0]);
                hasFrame = true;
                av_packet_free(&packet);
                return true;
            }
        }
        av_packet_free(&packet);
        return false;
    }

    void Close() {
        if (texture) { glDeleteTextures(1, &texture); texture = 0; }
        if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
        if (rgbBuffer) { av_free(rgbBuffer); rgbBuffer = nullptr; }
        if (rgbFrame) { av_frame_free(&rgbFrame); }
        if (frame) { av_frame_free(&frame); }
        if (codecCtx) { avcodec_free_context(&codecCtx); }
        if (hw) { hw->Free(); delete hw; hw = nullptr; }
        if (fmtCtx) { avformat_close_input(&fmtCtx); }
        hasFrame = false;
        width = 0;
        height = 0;
        srcW = 0;
        srcH = 0;
        lastPts = -1;
        swsFmt = AV_PIX_FMT_NONE;
        decName = "CPU";
        still = false;
    }
};

struct PreviewRam {
    struct Frame { float pts = 0; std::vector<uint8_t> rgba; };
    struct Slot { int w = 0; int h = 0; uint64_t bytes = 0; std::vector<Frame> frames; };
    struct Span { float a = 0; float b = 0; };
    struct Task {
        int gen = 0;
        int media = -1;
        std::string path;
        float from = 0;
        float to = 0;
        bool image = false;
    };

    std::mutex mu;
    std::condition_variable cv;
    std::map<int, Slot> slots;
    std::map<int, std::vector<Span>> done;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> pause{false};
    std::atomic<bool> stalled{false};
    std::atomic<int> gen{1};
    std::atomic<int> pct{0};
    std::atomic<int> decoding{-1};
    std::atomic<uint64_t> usedBytes{0};
    float activeFrom = 0;
    float activeTo = 0;
    Task pending;
    bool hasTask = false;
    bool busy = false;

    ~PreviewRam() { Stop(); }

    static bool RoomFor(size_t bytes) {
        uint64_t room = 512ull << 20;
#ifdef _WIN32
        MEMORYSTATUSEX mem;
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            uint64_t reserve = mem.ullTotalPhys / 5;
            if (reserve < (2ull << 30)) reserve = 2ull << 30;
            room = mem.ullAvailPhys > reserve ? mem.ullAvailPhys - reserve : 0;
        }
#endif
        return room >= bytes;
    }

    bool MakeRoom(int keep, size_t need) {
        const uint64_t cap = 1024ull << 20;
        auto tooBig = [&]() {
            return usedBytes.load() + (uint64_t)need > cap || !RoomFor(need);
        };
        if (!tooBig()) return true;
        std::lock_guard<std::mutex> lk(mu);
        while (usedBytes.load() + (uint64_t)need > cap || !RoomFor(need)) {
            int victim = -1;
            uint64_t biggest = 0;
            for (auto& kv : slots) {
                if (kv.first == keep) continue;
                if (kv.second.bytes > biggest) { biggest = kv.second.bytes; victim = kv.first; }
            }
            if (victim < 0) return false;
            uint64_t n = slots[victim].bytes;
            uint64_t cur = usedBytes.load();
            usedBytes = cur > n ? cur - n : 0;
            slots.erase(victim);
            done.erase(victim);
        }
        return true;
    }

    void Push(int media, float pts, int w, int h, std::vector<uint8_t>& rgba, int taskGen) {
        std::lock_guard<std::mutex> lk(mu);
        if (taskGen != gen.load()) return;
        Slot& s = slots[media];
        if ((s.w && s.w != w) || (s.h && s.h != h)) {
            usedBytes -= s.bytes;
            s.frames.clear();
            s.bytes = 0;
            done.erase(media);
        }
        s.w = w;
        s.h = h;
        auto it = s.frames.begin();
        while (it != s.frames.end() && it->pts + 0.001f < pts) ++it;
        if (it != s.frames.end() && std::fabs(it->pts - pts) < 0.012f) return;
        size_t n = rgba.size();
        s.frames.insert(it, Frame{pts, std::move(rgba)});
        s.bytes += n;
        usedBytes += n;
    }

    void MarkDone(int media, float a, float b) {
        std::lock_guard<std::mutex> lk(mu);
        done[media].push_back(Span{a, b});
    }

    bool Covered(int media, float a, float b) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = done.find(media);
        if (it == done.end()) return false;
        for (const Span& s : it->second)
            if (s.a <= a + 0.08f && s.b + 0.08f >= b) return true;
        return false;
    }

    void Decode(const Task& task) {
        struct Dec {
            AVFormatContext* fmt = nullptr;
            AVCodecContext* ctx = nullptr;
            HwDec* hw = nullptr;
            AVFrame* frame = nullptr;
            AVPacket* pkt = nullptr;
            SwsContext* sws = nullptr;
            ~Dec() {
                if (sws) sws_freeContext(sws);
                if (pkt) av_packet_free(&pkt);
                if (frame) av_frame_free(&frame);
                if (ctx) avcodec_free_context(&ctx);
                if (hw) { hw->Free(); delete hw; }
                if (fmt) avformat_close_input(&fmt);
            }
        } dec;
        if (avformat_open_input(&dec.fmt, task.path.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(dec.fmt, nullptr) < 0) {
            MarkDone(task.media, task.from, task.to);
            return;
        }
        int stream = -1;
        for (unsigned i = 0; i < dec.fmt->nb_streams; i++) {
            if (dec.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { stream = (int)i; break; }
        }
        const AVCodec* codec = stream >= 0 ? avcodec_find_decoder(dec.fmt->streams[stream]->codecpar->codec_id) : nullptr;
        const char* how = "CPU";
        if (stream < 0 || !codec || !OpenDecoder(dec.ctx, codec, dec.fmt->streams[stream]->codecpar, dec.hw, how)) {
            MarkDone(task.media, task.from, task.to);
            return;
        }
        dec.frame = av_frame_alloc();
        dec.pkt = av_packet_alloc();
        if (!dec.frame || !dec.pkt) return;
        float resume = task.from;
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = slots.find(task.media);
            if (it != slots.end()) {
                for (const Frame& f : it->second.frames)
                    if (f.pts >= task.from - 0.02f && f.pts < task.to && f.pts > resume) resume = f.pts;
            }
        }
        if (!task.image && resume > 0.02f) {
            int64_t ts = (int64_t)(resume / av_q2d(dec.fmt->streams[stream]->time_base));
            av_seek_frame(dec.fmt, stream, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dec.ctx);
        }
        int dw = 0, dh = 0;
        std::vector<uint8_t> rgba;
        float lastKeep = -1;
        bool eof = false;
        int spins = 0;
        while (!stop.load() && !pause.load() && task.gen == gen.load() && spins < 200000) {
            spins++;
            if (av_read_frame(dec.fmt, dec.pkt) < 0) { eof = true; break; }
            if (dec.pkt->stream_index != stream) { av_packet_unref(dec.pkt); continue; }
            int send = avcodec_send_packet(dec.ctx, dec.pkt);
            av_packet_unref(dec.pkt);
            if (send < 0 && send != AVERROR(EAGAIN)) continue;
            while (avcodec_receive_frame(dec.ctx, dec.frame) == 0) {
                if (stop.load() || pause.load() || task.gen != gen.load()) return;
                AVFrame* src = DownloadFrame(dec.hw, dec.frame);
                if (!src || src->width <= 0 || src->height <= 0) continue;
                if (!dec.sws) {
                    int edge = std::max(src->width, src->height);
                    dw = src->width;
                    dh = src->height;
                    if (edge > 960) {
                        double s = 960.0 / (double)edge;
                        dw = std::max(2, (int)std::lround(src->width * s) & ~1);
                        dh = std::max(2, (int)std::lround(src->height * s) & ~1);
                    }
                    dec.sws = sws_getContext(src->width, src->height, (AVPixelFormat)src->format, dw, dh, AV_PIX_FMT_RGBA,
                                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    rgba.assign((size_t)dw * (size_t)dh * 4, 0);
                }
                if (!dec.sws || rgba.empty()) return;
                uint8_t* dstData[1] = { rgba.data() };
                int dstLines[1] = { dw * 4 };
                sws_scale(dec.sws, src->data, src->linesize, 0, src->height, dstData, dstLines);
                float pts = 0;
                if (!task.image && dec.frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    pts = (float)(dec.frame->best_effort_timestamp * av_q2d(dec.fmt->streams[stream]->time_base));
                else if (!task.image)
                    pts = lastKeep < 0 ? resume : lastKeep + 0.033f;
                if (!task.image && pts + 0.04f < resume) continue;
                if (!task.image && pts > task.to + 0.06f) {
                    MarkDone(task.media, task.from, task.to);
                    return;
                }
                if (!task.image && lastKeep >= 0 && pts - lastKeep < 0.028f) continue;
                if (!MakeRoom(task.media, rgba.size())) { stalled = true; return; }
                stalled = false;
                std::vector<uint8_t> owned = rgba;
                Push(task.media, pts, dw, dh, owned, task.gen);
                lastKeep = pts;
                if (task.image) {
                    MarkDone(task.media, task.from, task.to);
                    return;
                }
            }
        }
        if (eof && task.gen == gen.load() && !pause.load())
            MarkDone(task.media, task.from, task.to);
    }

    void ThreadMain() {
#ifdef _WIN32
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        while (!stop.load()) {
            Task task;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&]{ return stop.load() || hasTask; });
                if (stop.load()) break;
                task = pending;
                hasTask = false;
                busy = true;
                activeFrom = task.from;
                activeTo = task.to;
                decoding.store(task.media);
            }
            if (!pause.load() && task.gen == gen.load()) Decode(task);
            decoding.store(-1);
            std::lock_guard<std::mutex> lk(mu);
            busy = false;
        }
#ifdef _WIN32
        CoUninitialize();
#endif
    }

    void EnsureThread() {
        if (worker.joinable()) return;
        stop = false;
        worker = std::thread([this]{ ThreadMain(); });
    }

    void SetPaused(bool v) { pause.store(v); if (!v) cv.notify_all(); }

    void Clear() {
        gen.fetch_add(1);
        std::lock_guard<std::mutex> lk(mu);
        slots.clear();
        done.clear();
        usedBytes = 0;
        hasTask = false;
        pct = 0;
        stalled = false;
    }

    void Forget(int id) {
        gen.fetch_add(1);
        std::lock_guard<std::mutex> lk(mu);
        auto it = slots.find(id);
        if (it != slots.end()) {
            uint64_t n = it->second.bytes;
            uint64_t cur = usedBytes.load();
            usedBytes = cur > n ? cur - n : 0;
            std::vector<Frame>().swap(it->second.frames);
            slots.erase(it);
        }
        std::map<int, Slot> shifted;
        for (auto& kv : slots) {
            int k = kv.first > id ? kv.first - 1 : kv.first;
            shifted.emplace(k, std::move(kv.second));
        }
        slots.swap(shifted);
        std::map<int, std::vector<Span>> kept;
        for (auto& kv : done) {
            if (kv.first == id) continue;
            int k = kv.first > id ? kv.first - 1 : kv.first;
            kept.emplace(k, std::move(kv.second));
        }
        done.swap(kept);
        if (hasTask) {
            if (pending.media == id) hasTask = false;
            else if (pending.media > id) pending.media--;
        }
        stalled = false;
        uint64_t sum = 0;
        for (auto& kv : slots) {
            kv.second.bytes = 0;
            for (const Frame& f : kv.second.frames) kv.second.bytes += f.rgba.size();
            sum += kv.second.bytes;
        }
        usedBytes = sum;
    }

    void Stop() {
        stop = true;
        gen.fetch_add(1);
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    bool Present(VideoPlayer* p, int media, float mediaT) {
        if (!p || media < 0) return false;
        std::vector<uint8_t> copy;
        int w = 0, h = 0;
        float pts = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = slots.find(media);
            if (it == slots.end() || it->second.frames.empty()) return false;
            const Slot& s = it->second;
            int best = 0;
            if (s.frames.size() > 1) {
                int lo = 0, hi = (int)s.frames.size() - 1, ans = 0;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    if (s.frames[mid].pts <= mediaT + 0.02f) { ans = mid; lo = mid + 1; }
                    else hi = mid - 1;
                }
                best = ans;
            }
            if (std::fabs(s.frames[best].pts - mediaT) > 0.20f) return false;
            copy = s.frames[best].rgba;
            w = s.w;
            h = s.h;
            pts = s.frames[best].pts;
        }
        if (!p->ShowRGBA(copy.data(), w, h)) return false;
        p->lastPts = pts;
        return true;
    }

    bool Has(int media, float mediaT) {
        if (media < 0) return false;
        std::lock_guard<std::mutex> lk(mu);
        auto it = slots.find(media);
        if (it == slots.end() || it->second.frames.empty()) return false;
        const Slot& s = it->second;
        int best = 0;
        if (s.frames.size() > 1) {
            int lo = 0, hi = (int)s.frames.size() - 1, ans = 0;
            while (lo <= hi) {
                int midI = (lo + hi) / 2;
                if (s.frames[midI].pts <= mediaT + 0.02f) { ans = midI; lo = midI + 1; }
                else hi = midI - 1;
            }
            best = ans;
        }
        return std::fabs(s.frames[best].pts - mediaT) <= 0.20f;
    }

    int Percent() const { return pct.load(); }
    bool MemoryFull() const { return stalled.load() && pct.load() < 99; }

    void Tick(float time) {
        struct R { int media; std::string path; float a, b, rank; bool image; };
        std::vector<R> ranges;
        const float nearBehind = 2.0f;
        const float nearAhead = 14.0f;
        const uint64_t fullBudget = 200ull << 20;
        auto proxyBytes = [](float aspect) -> int {
            if (!(aspect > 0.05f)) aspect = 16.0f / 9.0f;
            int w, h;
            if (aspect >= 1.0f) {
                w = 960;
                h = std::max(2, ((int)std::lround(960.0 / aspect)) & ~1);
            } else {
                h = 960;
                w = std::max(2, ((int)std::lround(960.0 * aspect)) & ~1);
            }
            return w * h * 4;
        };
        int frontClip = -1;
        int frontTrack = 99;
        for (int i = 0; i < (int)clips.size(); i++) {
            const Clip& c = clips[i];
            if (c.track >= 3) continue;
            if (time < c.startTime || time >= c.startTime + c.duration) continue;
            if (c.track < frontTrack) { frontTrack = c.track; frontClip = i; }
        }
        for (int i = 0; i < (int)clips.size(); i++) {
            const Clip& c = clips[i];
            if (c.track >= 3 && c.track != kGsTrack) continue;
            if (c.mediaId < 0 || c.mediaId >= (int)media.size()) continue;
            if (media[c.mediaId].type == "audio") continue;
            if (IsSolidPath(media[c.mediaId].path)) continue;
            float clipEnd = c.startTime + c.duration;
            bool image = media[c.mediaId].type == "image";
            float speed = std::max(0.05f, c.speed);
            float srcA = std::max(0.0f, c.sourceOffset);
            float srcB = c.sourceOffset + (image ? 0.05f : c.duration * speed);
            if (!image && media[c.mediaId].duration > 0.2f && srcB > media[c.mediaId].duration + 0.05f)
                srcB = media[c.mediaId].duration + 0.05f;
            if (srcB < srcA + 0.04f) srcB = srcA + 0.04f;
            uint64_t est = (uint64_t)(std::max(0.04f, srcB - srcA) * 30.0 * proxyBytes(media[c.mediaId].aspect));
            bool full = image || est <= fullBudget;
            if (full) {
                if (clipEnd < time - nearBehind || c.startTime > time + nearAhead) continue;
            } else if (clipEnd < time - 1.0f || c.startTime > time + 9.0f) {
                continue;
            }
            R r;
            r.media = c.mediaId;
            r.path = media[c.mediaId].path;
            r.image = image;
            if (full) {
                r.a = image ? 0.0f : srcA;
                r.b = image ? 0.05f : srcB;
            } else {
                float visA = std::max(c.startTime, time - 1.0f);
                float visB = std::min(clipEnd, time + 9.0f);
                r.a = std::max(0.0f, c.sourceOffset + (visA - c.startTime) * speed);
                r.b = c.sourceOffset + (visB - c.startTime) * speed;
                if (r.b > r.a + 11.0f) r.b = r.a + 11.0f;
                if (media[c.mediaId].duration > 0.2f && r.b > media[c.mediaId].duration + 0.05f)
                    r.b = media[c.mediaId].duration + 0.05f;
                if (r.b < r.a + 0.04f) r.b = r.a + 0.04f;
            }
            if (time < c.startTime) r.rank = c.startTime - time;
            else if (time > clipEnd) r.rank = time - clipEnd;
            else r.rank = (i == frontClip) ? -1.0f : 0.0f;
            ranges.push_back(r);
        }
        std::sort(ranges.begin(), ranges.end(), [](const R& a, const R& b) {
            if (a.media != b.media) return a.media < b.media;
            return a.a < b.a;
        });
        std::vector<R> merged;
        for (const R& r : ranges) {
            if (!merged.empty() && merged.back().media == r.media && r.a <= merged.back().b + 0.20f) {
                if (r.b > merged.back().b) merged.back().b = r.b;
                if (r.rank < merged.back().rank) merged.back().rank = r.rank;
            } else merged.push_back(r);
        }
        std::sort(merged.begin(), merged.end(), [](const R& a, const R& b) {
            return a.rank < b.rank || (a.rank == b.rank && a.media < b.media);
        });
        {
            std::lock_guard<std::mutex> lk(mu);
            std::map<int, std::pair<float, float>> bounds;
            for (const R& r : merged) {
                auto it = bounds.find(r.media);
                if (it == bounds.end()) bounds[r.media] = {r.a, r.b};
                else {
                    it->second.first = std::min(it->second.first, r.a);
                    it->second.second = std::max(it->second.second, r.b);
                }
            }
            for (auto it = slots.begin(); it != slots.end();) {
                auto w = bounds.find(it->first);
                if (w == bounds.end()) {
                    it = slots.erase(it);
                    continue;
                }
                Slot& s = it->second;
                float a = w->second.first;
                float b = w->second.second;
                std::vector<Frame> keep;
                keep.reserve(s.frames.size());
                for (auto& f : s.frames) {
                    if (f.pts >= a - 0.15f && f.pts <= b + 1.0f) keep.push_back(std::move(f));
                }
                s.frames.swap(keep);
                if (s.frames.empty()) it = slots.erase(it);
                else ++it;
            }
            for (auto it = done.begin(); it != done.end();) {
                auto w = bounds.find(it->first);
                if (w == bounds.end()) { it = done.erase(it); continue; }
                float a = w->second.first;
                float b = w->second.second;
                auto sp = it->second.begin();
                while (sp != it->second.end()) {
                    if (sp->b < a - 0.2f || sp->a > b + 0.2f) sp = it->second.erase(sp);
                    else ++sp;
                }
                if (it->second.empty()) it = done.erase(it);
                else ++it;
            }
            uint64_t sum = 0;
            for (auto& kv : slots) {
                kv.second.bytes = 0;
                for (const Frame& f : kv.second.frames) kv.second.bytes += f.rgba.size();
                sum += kv.second.bytes;
            }
            usedBytes = sum;
        }
        double want = 0, got = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const R& r : merged) {
                double len = std::max(0.04f, r.b - r.a);
                want += len;
                auto it = slots.find(r.media);
                int n = 0;
                if (it != slots.end()) {
                    for (const Frame& f : it->second.frames)
                        if (f.pts + 0.02f >= r.a && f.pts <= r.b + 0.02f) n++;
                }
                got += std::min(len, n / 30.0);
            }
        }
        pct = want <= 0.01 ? 100 : (int)std::lround(got / want * 100.0);
        if (pct > 100) pct = 100;
        int best = -1;
        auto nearReady = [&](const R& r) {
            if (r.image) return Has(r.media, 0.0f);
            float head = std::min(r.b, r.a + 1.0f);
            float ahead = std::min(r.b, head + 1.5f);
            return Has(r.media, head) && Has(r.media, ahead);
        };
        for (int i = 0; i < (int)merged.size(); i++) {
            if (merged[i].rank < 0.0f && nearReady(merged[i])) continue;
            if (!Covered(merged[i].media, merged[i].a, merged[i].b)) { best = i; break; }
        }
        if (best < 0) { pct = merged.empty() ? 0 : 100; return; }
        EnsureThread();
        bool isBusy = false;
        int activeMedia = -1;
        float af = 0, at = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            isBusy = busy;
            activeMedia = decoding.load();
            af = activeFrom;
            at = activeTo;
        }
        if (isBusy) {
            bool useful = false;
            for (const R& r : merged) {
                if (r.media != activeMedia) continue;
                if (r.b >= af - 0.25f && r.a <= at + 0.25f) useful = true;
            }
            bool frontNeeds = false;
            if (!merged.empty() && merged[0].rank < 0.0f && activeMedia != merged[0].media && !nearReady(merged[0]))
                frontNeeds = true;
            if (!useful || frontNeeds) gen.fetch_add(1);
            return;
        }
        Task task;
        task.gen = gen.load();
        task.media = merged[best].media;
        task.path = merged[best].path;
        task.from = merged[best].a;
        task.to = merged[best].b;
        task.image = merged[best].image;
        {
            std::lock_guard<std::mutex> lk(mu);
            pending = task;
            hasTask = true;
        }
        cv.notify_one();
    }
};

PreviewRam gRam;

VideoPlayer* player = nullptr;
int playingMediaId = -1;
int playingClip = -1;
int playerFileId = -1;
VideoPlayer* playerB = nullptr;
int playingMediaIdB = -1;
int playingClipB = -1;
float shownLocalB = -1000.0f;
VideoPlayer* playerUnder = nullptr;
int playingMediaIdUnder = -1;
int playingClipUnder = -1;
float shownLocalUnder = -1000.0f;
VideoPlayer* playerNext = nullptr;
int nextMediaId = -1;
int nextClipIdx = -1;
VideoPlayer* playerGS = nullptr;
int playingGsClip = -1;
int playingGsMedia = -1;
VideoPlayer* playerLib = nullptr;
int libPlayId = -1;
std::vector<int> mediaSel;
int floatMedia = -1;
bool floatMediaOpen = false;
ImVec4 floatMediaRect(0, 0, 0, 0);

float ProbeDuration(const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return 10.0f;
    avformat_find_stream_info(fmt, nullptr);
    float d = 10.0f;
    if (fmt->duration > 0) d = (float)fmt->duration / (float)AV_TIME_BASE;
    avformat_close_input(&fmt);
    return (d > 0.05f) ? d : 10.0f;
}

void MakeThumbnail(MediaItem& m) {
    int solid = SolidKindFromPath(m.path);
    if (solid >= 0) {
        EnsureSolidThumbs();
        m.thumb = gSolidTex[solid];
        m.thumbW = 320;
        m.thumbH = 180;
        m.aspect = 16.0f / 9.0f;
        if (m.duration < 0.2f) m.duration = 5.0f;
        return;
    }
    if (m.type != "video" && m.type != "image") return;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, m.path.c_str(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return; }
    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    }
    if (vs < 0) { avformat_close_input(&fmt); return; }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmt); return; }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }
    if (m.duration > 0.4f) {
        int64_t ts = (int64_t)(0.4 / av_q2d(fmt->streams[vs]->time_base));
        av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    bool got = false;
    for (int n = 0; n < 60 && !got; n++) {
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index == vs && avcodec_send_packet(ctx, pkt) == 0) {
            if (avcodec_receive_frame(ctx, fr) == 0) got = true;
        }
        av_packet_unref(pkt);
    }
    if (got && fr->width > 0 && fr->height > 0) {
        m.aspect = (float)fr->width / (float)fr->height;
        int tw = fr->width, th = fr->height;
        if (tw > 360) { th = std::max(1, th * 360 / tw); tw = 360; }
        int bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, tw, th, 1);
        uint8_t* buf = (uint8_t*)av_malloc(bytes);
        AVFrame* rgb = av_frame_alloc();
        av_image_fill_arrays(rgb->data, rgb->linesize, buf, AV_PIX_FMT_RGBA, tw, th, 1);
        SwsContext* sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format,
                                         tw, th, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws && buf) {
            sws_scale(sws, fr->data, fr->linesize, 0, fr->height, rgb->data, rgb->linesize);
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgb->data[0]);
            m.thumb = tex;
            m.thumbW = tw;
            m.thumbH = th;
            sws_freeContext(sws);
        }
        av_frame_free(&rgb);
        av_free(buf);
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

void CloseAudioSrc(AudioSrc& a) {
    if (a.swr) swr_free(&a.swr);
    if (a.frame) av_frame_free(&a.frame);
    if (a.packet) av_packet_free(&a.packet);
    if (a.codec) avcodec_free_context(&a.codec);
    if (a.fmt) avformat_close_input(&a.fmt);
}

int EnsureAudio(int mediaId) {
    for (int i = 0; i < (int)audioSrcs.size(); i++)
        if (audioSrcs[i].mediaId == mediaId) return i;
    AudioSrc a;
    a.mediaId = mediaId;
    if (mediaId < 0 || mediaId >= (int)media.size()) { a.failed = true; audioSrcs.push_back(std::move(a)); return (int)audioSrcs.size() - 1; }
    if (avformat_open_input(&a.fmt, media[mediaId].path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(a.fmt, nullptr) < 0) {
        a.failed = true;
        audioSrcs.push_back(std::move(a));
        return (int)audioSrcs.size() - 1;
    }
    for (unsigned i = 0; i < a.fmt->nb_streams; i++) {
        if (a.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { a.stream = (int)i; break; }
    }
    if (a.stream < 0) {
        a.noAudio = true;
        audioSrcs.push_back(std::move(a));
        return (int)audioSrcs.size() - 1;
    }
    const AVCodec* codec = avcodec_find_decoder(a.fmt->streams[a.stream]->codecpar->codec_id);
    a.codec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!a.codec || avcodec_parameters_to_context(a.codec, a.fmt->streams[a.stream]->codecpar) < 0 ||
        avcodec_open2(a.codec, codec, nullptr) < 0 || a.codec->sample_rate <= 0) {
        a.failed = true;
        audioSrcs.push_back(std::move(a));
        return (int)audioSrcs.size() - 1;
    }
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2);
    AVChannelLayout fallback;
    const AVChannelLayout* inLay = &a.codec->ch_layout;
    bool usedFallback = false;
    if (a.codec->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&fallback, 2);
        inLay = &fallback;
        usedFallback = true;
    }
    if (swr_alloc_set_opts2(&a.swr, &outLayout, AV_SAMPLE_FMT_S16, 48000,
                            inLay, a.codec->sample_fmt, a.codec->sample_rate, 0, nullptr) < 0 ||
        !a.swr || swr_init(a.swr) < 0) {
        a.failed = true;
    }
    av_channel_layout_uninit(&outLayout);
    if (usedFallback) av_channel_layout_uninit(&fallback);
    a.frame = av_frame_alloc();
    a.packet = av_packet_alloc();
    audioSrcs.push_back(std::move(a));
    return (int)audioSrcs.size() - 1;
}

void PullSamples(AudioSrc& a, double mediaTime, int count, int16_t* dst) {
    if (count <= 0) return;
    if (a.failed || a.noAudio || !a.swr || !a.codec) {
        memset(dst, 0, (size_t)count * 2 * sizeof(int16_t));
        return;
    }
    if (mediaTime < 0) mediaTime = 0;
    bool rewind = a.nextTime < 0 || mediaTime + 0.25 < a.nextTime;
    bool drift = !a.atEnd && a.nextTime >= 0 && std::fabs(a.nextTime - mediaTime) > 0.25;
    if (rewind || drift) {
        int64_t ts = (int64_t)(mediaTime / av_q2d(a.fmt->streams[a.stream]->time_base));
        av_seek_frame(a.fmt, a.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(a.codec);
        if (a.swr) swr_close(a.swr);
        if (a.swr) swr_init(a.swr);
        a.fifo.clear();
        a.nextTime = -1;
        a.atEnd = false;
    }
    auto takeFrames = [&]() {
        while (avcodec_receive_frame(a.codec, a.frame) == 0) {
            int outCount = (int)av_rescale_rnd(swr_get_delay(a.swr, a.codec->sample_rate) + a.frame->nb_samples,
                                                48000, a.codec->sample_rate, AV_ROUND_UP);
            if (outCount < 1) outCount = 1;
            std::vector<int16_t> tmp((size_t)outCount * 2 + 8);
            uint8_t* outPlanes[1] = { (uint8_t*)tmp.data() };
            int got = swr_convert(a.swr, outPlanes, outCount, (const uint8_t**)a.frame->data, a.frame->nb_samples);
            if (got <= 0) continue;
            double pts = mediaTime;
            if (a.frame->best_effort_timestamp != AV_NOPTS_VALUE)
                pts = a.frame->best_effort_timestamp * av_q2d(a.fmt->streams[a.stream]->time_base);
            int start = 0;
            if (a.nextTime < 0) {
                int drop = (int)((mediaTime - pts) * 48000.0);
                if (drop >= got) continue;
                if (drop > 0) start = drop;
                a.nextTime = mediaTime;
            }
            a.fifo.insert(a.fifo.end(), tmp.begin() + (size_t)start * 2, tmp.begin() + (size_t)got * 2);
        }
    };
    int spins = 0;
    bool eof = false;
    while (!a.atEnd && (int)a.fifo.size() < count * 2 && spins < 1500) {
        spins++;
        takeFrames();
        if ((int)a.fifo.size() >= count * 2) break;
        if (av_read_frame(a.fmt, a.packet) < 0) { a.atEnd = true; eof = true; break; }
        if (a.packet->stream_index == a.stream) {
            int send = avcodec_send_packet(a.codec, a.packet);
            if (send == AVERROR(EAGAIN)) {
                takeFrames();
                send = avcodec_send_packet(a.codec, a.packet);
            }
            if (send == 0) takeFrames();
        }
        av_packet_unref(a.packet);
    }
    if (eof && (int)a.fifo.size() < count * 2) {
        avcodec_send_packet(a.codec, nullptr);
        takeFrames();
        int outCount = 48000;
        std::vector<int16_t> tmp((size_t)outCount * 2 + 8);
        uint8_t* outPlanes[1] = { (uint8_t*)tmp.data() };
        int got = swr_convert(a.swr, outPlanes, outCount, nullptr, 0);
        if (got > 0) a.fifo.insert(a.fifo.end(), tmp.begin(), tmp.begin() + (size_t)got * 2);
    }
    int have = (int)a.fifo.size() / 2;
    int n = std::min(count, have);
    if (n > 0) {
        memcpy(dst, a.fifo.data(), (size_t)n * 2 * sizeof(int16_t));
        a.fifo.erase(a.fifo.begin(), a.fifo.begin() + n * 2);
        if (a.nextTime < 0) a.nextTime = mediaTime;
        a.nextTime += n / 48000.0;
    }
    if (a.atEnd) {
        double step = count / 48000.0;
        if (a.nextTime < 0) a.nextTime = mediaTime;
        if (a.nextTime < mediaTime + step) a.nextTime = mediaTime + step;
    }
    if (n < count) memset(dst + n * 2, 0, (size_t)(count - n) * 2 * sizeof(int16_t));
}

void ResetAllClipFx() {
    for (auto& c : clips) c.fx.clear();
}

static void BiquadCoeffs(float freq, float gainDb, float q, float& b0, float& b1, float& b2, float& a1, float& a2) {
    const float sr = 48000.0f;
    float A = powf(10.0f, gainDb / 40.0f);
    float w = 6.2831853f * freq / sr;
    float cw = cosf(w);
    float alpha = sinf(w) / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = (-2.0f * cw) / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = (-2.0f * cw) / a0;
    a2 = (1.0f - alpha / A) / a0;
}

static float BiquadStep(float x, float b0, float b1, float b2, float a1, float a2, float& z1, float& z2) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    if (y > -1.0e-8f && y < 1.0e-8f) y = 0.0f;
    return y;
}

void ApplyClipFx(Clip& c, float* s, int n, double timelineTime) {
    if (!s || n <= 0) return;
    if (c.fx.lastEnd >= 0.0 && fabs(timelineTime - c.fx.lastEnd) > 0.06)
        c.fx.clear();
    c.fx.lastEnd = timelineTime + (double)n / 48000.0;
    if (c.eqOn) {
        const float freqs[8] = {60, 150, 400, 1000, 2500, 6000, 12000, 16000};
        for (int b = 0; b < 8; b++) {
            float g = c.eqGain[b];
            if (g > 12.0f) g = 12.0f;
            if (g < -12.0f) g = -12.0f;
            if (fabsf(g) < 0.05f) continue;
            float b0, b1, b2, a1, a2;
            BiquadCoeffs(freqs[b], g, 1.2f, b0, b1, b2, a1, a2);
            for (int i = 0; i < n; i++) {
                for (int ch = 0; ch < 2; ch++) {
                    float& x = s[i * 2 + ch];
                    x = BiquadStep(x, b0, b1, b2, a1, a2, c.fx.z1[ch][b], c.fx.z2[ch][b]);
                }
            }
        }
    }
    if (c.delayOn) {
        float tempo = c.delayTempo;
        if (tempo < 40.0f) tempo = 40.0f;
        if (tempo > 240.0f) tempo = 240.0f;
        int len = (int)(48000.0f * 60.0f / tempo + 0.5f);
        if (len < 32) len = 32;
        if (len > 96000) len = 96000;
        if (c.fx.delayLen != len) {
            c.fx.delay.assign((size_t)len * 2, 0.0f);
            c.fx.delayPos = 0;
            c.fx.delayLen = len;
        }
        const float fb = 0.38f;
        const float wet = 0.42f;
        for (int i = 0; i < n; i++) {
            int p = c.fx.delayPos;
            if (p < 0 || p >= len) p = 0;
            for (int ch = 0; ch < 2; ch++) {
                float d = c.fx.delay[(size_t)p * 2 + ch];
                float x = s[i * 2 + ch];
                c.fx.delay[(size_t)p * 2 + ch] = x + d * fb;
                s[i * 2 + ch] = x + d * wet;
            }
            p++;
            if (p >= len) p = 0;
            c.fx.delayPos = p;
        }
    }
    if (c.reverbOn) {
        static const int combLen[4] = {1216, 1296, 1408, 1472};
        static const int apLen[2] = {225, 556};
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < 4; k++) {
                if ((int)c.fx.comb[ch][k].size() != combLen[k]) {
                    c.fx.comb[ch][k].assign(combLen[k], 0.0f);
                    c.fx.combPos[ch][k] = 0;
                }
            }
            for (int k = 0; k < 2; k++) {
                if ((int)c.fx.ap[ch][k].size() != apLen[k]) {
                    c.fx.ap[ch][k].assign(apLen[k], 0.0f);
                    c.fx.apPos[ch][k] = 0;
                }
            }
        }
        float rt = c.reverbTime;
        if (rt < 0.15f) rt = 0.15f;
        if (rt > 6.0f) rt = 6.0f;
        float fb[4];
        for (int k = 0; k < 4; k++) {
            fb[k] = expf(-3.0f * ((float)combLen[k] / 48000.0f) / rt);
            if (fb[k] > 0.92f) fb[k] = 0.92f;
            if (fb[k] < 0.05f) fb[k] = 0.05f;
        }
        for (int i = 0; i < n; i++) {
            for (int ch = 0; ch < 2; ch++) {
                float x = s[i * 2 + ch];
                float acc = 0.0f;
                for (int k = 0; k < 4; k++) {
                    int len = combLen[k];
                    int p = c.fx.combPos[ch][k];
                    if (p < 0 || p >= len) p = 0;
                    float y = c.fx.comb[ch][k][p];
                    c.fx.comb[ch][k][p] = x + y * fb[k];
                    p++;
                    if (p >= len) p = 0;
                    c.fx.combPos[ch][k] = p;
                    acc += y;
                }
                acc *= 0.25f;
                for (int k = 0; k < 2; k++) {
                    int len = apLen[k];
                    int p = c.fx.apPos[ch][k];
                    if (p < 0 || p >= len) p = 0;
                    float buf = c.fx.ap[ch][k][p];
                    float y = buf - 0.5f * acc;
                    c.fx.ap[ch][k][p] = acc + 0.5f * buf;
                    p++;
                    if (p >= len) p = 0;
                    c.fx.apPos[ch][k] = p;
                    acc = y;
                }
                float y = x * 0.72f + acc * 0.45f;
                if (y > 30000.0f) y = 30000.0f;
                if (y < -30000.0f) y = -30000.0f;
                s[i * 2 + ch] = y;
            }
        }
    }
}

void ResetAudioSync() {
    for (auto& a : audioSrcs) a.nextTime = -1;
    if (audioDev) SDL_ClearQueuedAudio(audioDev);
    ResetAllClipFx();
}

void SetPlaying(bool play) {
    if (play == isPlaying) return;
    isPlaying = play;
    ResetAudioSync();
    if (audioDev) SDL_PauseAudioDevice(audioDev, play ? 0 : 1);
}

static float VolumeEdge(float local, float dur, float inSec, float outSec) {
    if (dur <= 0.0001f) return 1.0f;
    if (local < 0) local = 0;
    if (local > dur) local = dur;
    float f = 1.0f;
    if (inSec > 0.02f && local < inSec) f = local / inSec;
    if (outSec > 0.02f) {
        float tail = dur - local;
        if (tail < outSec) {
            float d = tail / outSec;
            if (d < f) f = d;
        }
    }
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

static float RampAt(float local, float dur, float inSec, float outSec);
bool TransitionForClip(int clip, float t, int& a, int& b, float& progress, std::string& id);

float GainAt(const Clip& c, float localTime) {
    if (c.duration <= 0.0001f) return 1.0f;
    if (localTime < 0) localTime = 0;
    if (localTime > c.duration) localTime = c.duration;
    float g = 1.0f;
    if (!c.volume.empty()) {
        float n = localTime / c.duration;
        if (n <= c.volume.front().norm) g = c.volume.front().gain;
        else if (n >= c.volume.back().norm) g = c.volume.back().gain;
        else {
            for (size_t i = 1; i < c.volume.size(); i++) {
                if (n <= c.volume[i].norm) {
                    float d = c.volume[i].norm - c.volume[i - 1].norm;
                    float u = d > 0.0001f ? (n - c.volume[i - 1].norm) / d : 0.0f;
                    g = c.volume[i - 1].gain + (c.volume[i].gain - c.volume[i - 1].gain) * u;
                    break;
                }
            }
        }
    }
    float fadeDur = std::min(0.75f, c.duration * 0.45f);
    if (fadeDur < 0.02f) fadeDur = 0.02f;
    if (c.volIn > 0.02f || c.volOut > 0.02f) {
        g *= VolumeEdge(localTime, c.duration, c.volIn, c.volOut);
    } else {
        if (c.fadeIn) g *= std::min(1.0f, localTime / fadeDur);
        if (c.fadeOut) g *= std::min(1.0f, (c.duration - localTime) / fadeDur);
    }
    if (g < 0) g = 0;
    if (g > 1) g = 1;
    int idx = -1;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (&clips[i] == &c) { idx = i; break; }
    }
    if (idx >= 0) {
        int a = -1, b = -1;
        float prog = 0;
        std::string tid;
        if (TransitionForClip(idx, c.startTime + localTime, a, b, prog, tid)) {
            float w = 1.0f;
            if (idx == a) w = std::cos(prog * 1.57079632679f);
            else if (idx == b) w = std::sin(prog * 1.57079632679f);
            g *= w;
        }
    }
    if (g < 0) g = 0;
    if (g > 1) g = 1;
    return g;
}

static float RampAt(float local, float dur, float inSec, float outSec) {
    if (dur <= 0.0001f) return 1.0f;
    if (local < 0) local = 0;
    if (local > dur) local = dur;
    if (inSec <= 0.02f && outSec <= 0.02f) return 1.0f;
    float o = 1.0f;
    if (inSec > 0.02f && local < inSec) o = local / inSec;
    if (outSec > 0.02f) {
        float tail = dur - local;
        if (tail < outSec) {
            float fall = tail / outSec;
            if (fall < o) o = fall;
        }
    }
    if (o < 0) o = 0;
    if (o > 1) o = 1;
    return o;
}

static float OpacityAt(const Clip& c, float localTime) {
    return RampAt(localTime, c.duration, c.opIn, c.opOut);
}

void MixTimeline(double timelineTime, int sampleCount, int16_t* dst) {
    if (!dst || sampleCount <= 0) return;
    std::vector<int32_t> mix((size_t)sampleCount * 2, 0);
    std::vector<float> shaped((size_t)sampleCount * 2);
    for (int ci = 0; ci < (int)clips.size(); ci++) {
        Clip& c = clips[ci];
        if (c.mediaId < 0 || c.mediaId >= (int)media.size()) continue;
        if (c.muted) continue;
        if (media[c.mediaId].type == "image") continue;
        float tail = 0.0f;
        if (c.reverbOn) tail = std::max(tail, std::min(5.0f, c.reverbTime));
        if (c.delayOn) tail = std::max(tail, 180.0f / std::max(40.0f, c.delayTempo));
        if (timelineTime < c.startTime || timelineTime >= c.startTime + c.duration + tail) continue;
        bool live = timelineTime < c.startTime + c.duration;
        float local = (float)(timelineTime - c.startTime);
        float blockDur = sampleCount / 48000.0f;
        float g0 = GainAt(c, std::min(local, c.duration));
        float g1 = GainAt(c, std::min(local + blockDur, c.duration));
        std::fill(shaped.begin(), shaped.end(), 0.0f);
        if (live) {
            int idx = EnsureAudio(c.mediaId);
            if (audioSrcs[idx].noAudio || audioSrcs[idx].failed) continue;
            float speed = c.speed < 0.05f ? 1.0f : c.speed;
            double mediaT = c.sourceOffset + (timelineTime - c.startTime) * speed;
            int srcN = std::max(1, (int)std::lround(sampleCount * speed));
            std::vector<int16_t> src((size_t)srcN * 2, 0);
            PullSamples(audioSrcs[idx], mediaT, srcN, src.data());
            for (int i = 0; i < sampleCount; i++) {
                float pos = (sampleCount <= 1) ? 0.0f : (float)i * (float)(srcN - 1) / (float)(sampleCount - 1);
                int i0 = (int)pos;
                int i1 = std::min(srcN - 1, i0 + 1);
                float fr = pos - (float)i0;
                for (int ch = 0; ch < 2; ch++) {
                    shaped[i * 2 + ch] = src[(size_t)i0 * 2 + ch] * (1.0f - fr) + src[(size_t)i1 * 2 + ch] * fr;
                }
            }
        }
        if (c.eqOn || c.delayOn || c.reverbOn)
            ApplyClipFx(c, shaped.data(), sampleCount, timelineTime);
        for (int i = 0; i < sampleCount; i++) {
            float g = g0 + (g1 - g0) * ((float)i / (float)sampleCount);
            mix[i * 2] += (int32_t)(shaped[i * 2] * g);
            mix[i * 2 + 1] += (int32_t)(shaped[i * 2 + 1] * g);
        }
    }
    float mg = masterGain;
    if (mg < 0.0f) mg = 0.0f;
    if (mg > 1.5f) mg = 1.5f;
    float pkL = 0.0f, pkR = 0.0f;
    for (int i = 0; i < sampleCount; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float s = (float)mix[i * 2 + ch] * mg;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            dst[i * 2 + ch] = (int16_t)s;
            float a = std::fabs(s) / 32768.0f;
            if (ch == 0) { if (a > pkL) pkL = a; }
            else if (a > pkR) pkR = a;
        }
    }
    if (pkL > masterRawL) masterRawL = pkL;
    if (pkR > masterRawR) masterRawR = pkR;
}

static double ProjectEnd() {
    double end = 0;
    for (const auto& c : clips) {
        double e = (double)c.startTime + (double)c.duration;
        if (c.reverbOn) e += std::min(5.0, (double)c.reverbTime);
        if (c.delayOn) e += 180.0 / std::max(40.0f, c.delayTempo);
        end = std::max(end, e);
    }
    for (const auto& s : subs) end = std::max(end, (double)s.start + (double)s.duration);
    return end;
}

static void MixOneClip(int ci, double timelineTime, int sampleCount, int16_t* dst) {
    if (!dst || sampleCount <= 0) return;
    memset(dst, 0, (size_t)sampleCount * 2 * sizeof(int16_t));
    if (ci < 0 || ci >= (int)clips.size()) return;
    Clip& c = clips[ci];
    if (c.mediaId < 0 || c.mediaId >= (int)media.size() || c.muted) return;
    if (media[c.mediaId].type == "image") return;
    if (timelineTime < c.startTime || timelineTime >= c.startTime + c.duration) return;
    int idx = EnsureAudio(c.mediaId);
    if (audioSrcs[idx].noAudio || audioSrcs[idx].failed) return;
    float speed = c.speed < 0.05f ? 1.0f : c.speed;
    double mediaT = c.sourceOffset + (timelineTime - c.startTime) * speed;
    int srcN = std::max(1, (int)std::lround(sampleCount * speed));
    std::vector<int16_t> src((size_t)srcN * 2, 0);
    PullSamples(audioSrcs[idx], mediaT, srcN, src.data());
    float local = (float)(timelineTime - c.startTime);
    float blockDur = sampleCount / 48000.0f;
    float g0 = GainAt(c, std::min(local, c.duration));
    float g1 = GainAt(c, std::min(local + blockDur, c.duration));
    for (int i = 0; i < sampleCount; i++) {
        float pos = (sampleCount <= 1) ? 0.0f : (float)i * (float)(srcN - 1) / (float)(sampleCount - 1);
        int i0 = (int)pos;
        int i1 = std::min(srcN - 1, i0 + 1);
        float fr = pos - (float)i0;
        float g = g0 + (g1 - g0) * ((float)i / (float)sampleCount);
        for (int ch = 0; ch < 2; ch++) {
            float s = src[(size_t)i0 * 2 + ch] * (1.0f - fr) + src[(size_t)i1 * 2 + ch] * fr;
            int v = (int)(s * g);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            dst[i * 2 + ch] = (int16_t)v;
        }
    }
}

static bool ClipCanSpeak(int ci) {
    if (ci < 0 || ci >= (int)clips.size()) return false;
    const Clip& c = clips[ci];
    if (c.muted || c.duration < 0.3f || c.mediaId < 0 || c.mediaId >= (int)media.size()) return false;
    if (media[c.mediaId].type == "image") return false;
    int idx = EnsureAudio(c.mediaId);
    return !audioSrcs[idx].noAudio && !audioSrcs[idx].failed;
}

static const char* TrackLabel(int track) {
    const char* names[] = {"V1", "V2", "V3", "A1", "A2", "A3", "GS"};
    if (track < 0 || track > 6) return "clip";
    return names[track];
}

static void BuildSpeechSpans(const std::vector<float>& pcm, std::vector<std::pair<int, int>>& spans) {
    spans.clear();
    const int hop = 1600;
    const float thresh = 0.01f;
    int speechStart = -1;
    int lastSpeech = -1;
    auto closeSpan = [&](int a, int b) {
        if (b - a < 1600 * 3) return;
        const int piece = 16000 * 4;
        int p = a;
        while (p < b) {
            int e = std::min(b, p + piece);
            if (b - e < 16000) e = b;
            spans.push_back({p, e});
            if (e <= p) break;
            p = e;
        }
    };
    for (int i = 0; i + hop <= (int)pcm.size(); i += hop) {
        double energy = 0;
        for (int k = 0; k < hop; k++) energy += (double)pcm[i + k] * pcm[i + k];
        float rms = std::sqrt((float)(energy / hop));
        if (rms >= thresh) {
            if (speechStart < 0) speechStart = i;
            lastSpeech = i + hop;
        } else if (speechStart >= 0 && i - lastSpeech > 16000 * 0.45) {
            closeSpan(speechStart, lastSpeech);
            speechStart = -1;
        }
    }
    if (speechStart >= 0) closeSpan(speechStart, std::max(lastSpeech, speechStart + hop));
}

static void SplitTimedText(float start, float end, const std::string& text, std::vector<AsrSeg>& out) {
    std::vector<std::string> bits;
    std::string cur;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = (unsigned char)text[i];
        int need = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (need < 1 || i + (size_t)need > text.size()) need = 1;
        cur.append(text, i, (size_t)need);
        i += (size_t)need;
        bool brk = c == '.' || c == '!' || c == '?' || c == '\n';
        if (brk && cur.size() > 1) {
            size_t a = 0;
            while (a < cur.size() && cur[a] == ' ') a++;
            size_t b = cur.size();
            while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\n')) b--;
            if (b > a) bits.push_back(cur.substr(a, b - a));
            cur.clear();
        }
    }
    if (!cur.empty()) {
        size_t a = 0;
        while (a < cur.size() && cur[a] == ' ') a++;
        size_t b = cur.size();
        while (b > a && cur[b - 1] == ' ') b--;
        if (b > a) bits.push_back(cur.substr(a, b - a));
    }
    if (bits.empty()) return;
    float dur = std::max(0.25f, end - start);
    int weight = 0;
    for (const auto& bit : bits) weight += std::max(1, (int)bit.size());
    float t = start;
    for (size_t i = 0; i < bits.size(); i++) {
        float d = dur * (float)std::max(1, (int)bits[i].size()) / (float)weight;
        if (i + 1 == bits.size()) d = std::max(0.25f, end - t);
        AsrSeg s;
        s.start = t;
        s.end = t + std::max(0.25f, d);
        s.text = bits[i];
        out.push_back(s);
        t = s.end;
    }
}

struct AsrRead {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int stream = -1;
    bool eof = false;
    double origin = 0;
    int want = 0;
    int gotTotal = 0;
    int packets = 0;
    int audioPackets = 0;
    double lastPts = -1;
    void Close() {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (swr) swr_free(&swr);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
        *this = AsrRead();
    }
    bool Open(const Clip& c) {
        Close();
        if (c.mediaId < 0 || c.mediaId >= (int)media.size()) return false;
        float speed = c.speed < 0.05f ? 1.0f : c.speed;
        origin = std::max(0.0, (double)c.sourceOffset);
        want = std::max(1, (int)std::lround((double)c.duration * speed * 48000.0));
        auto tryOpen = [&](bool ignoreEdit) -> bool {
            Close();
            want = std::max(1, (int)std::lround((double)c.duration * speed * 48000.0));
            origin = std::max(0.0, (double)c.sourceOffset);
            AVDictionary* opt = nullptr;
            if (ignoreEdit) av_dict_set(&opt, "ignore_editlist", "1", 0);
            if (avformat_open_input(&fmt, media[c.mediaId].path.c_str(), nullptr, &opt) < 0) {
                av_dict_free(&opt);
                return false;
            }
            av_dict_free(&opt);
            if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
            stream = -1;
            for (unsigned i = 0; i < fmt->nb_streams; i++) {
                if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { stream = (int)i; break; }
            }
            if (stream < 0) return false;
            const AVCodec* dec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
            codec = dec ? avcodec_alloc_context3(dec) : nullptr;
            if (!codec || avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar) < 0 || avcodec_open2(codec, dec, nullptr) < 0)
                return false;
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, 2);
            AVChannelLayout fallback;
            const AVChannelLayout* inLay = &codec->ch_layout;
            bool usedFallback = codec->ch_layout.nb_channels <= 0;
            if (usedFallback) {
                av_channel_layout_default(&fallback, 2);
                inLay = &fallback;
            }
            int ok = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, 48000, inLay, codec->sample_fmt, codec->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&outLayout);
            if (usedFallback) av_channel_layout_uninit(&fallback);
            if (ok < 0 || !swr || swr_init(swr) < 0) return false;
            frame = av_frame_alloc();
            pkt = av_packet_alloc();
            if (origin > 0.02) {
                av_seek_frame(fmt, -1, (int64_t)(origin * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codec);
            }
            return true;
        };
        if (!tryOpen(true)) {
            Close();
            if (!tryOpen(false)) { Close(); return false; }
        } else {
            AVStream* st = fmt->streams[stream];
            double listed = (st->duration > 0) ? st->duration * av_q2d(st->time_base) : 0;
            if (listed > 0.2 && listed + 0.3 < c.duration) {
                Close();
                if (tryOpen(false)) {
                    AVStream* st2 = fmt->streams[stream];
                    double listed2 = (st2->duration > 0) ? st2->duration * av_q2d(st2->time_base) : 0;
                    if (listed2 + 0.05 < listed) {
                        Close();
                        tryOpen(true);
                    }
                } else tryOpen(true);
            }
        }
        if (!fmt || stream < 0) return false;
        AVStream* st = fmt->streams[stream];
        double listed = (st->duration > 0) ? st->duration * av_q2d(st->time_base) : -1;
        char line[320];
        snprintf(line, sizeof(line), "decode %s %s rate=%d ch=%d stream=%.2fs from=%.2f want=%.2fs",
                 avcodec_get_name(codec->codec_id), TrackLabel(c.track), codec->sample_rate, codec->ch_layout.nb_channels,
                 listed, origin, want / 48000.0);
        AsrLog(line);
        return true;
    }
    void Take() {
        while (avcodec_receive_frame(codec, frame) == 0) {
            int outCount = (int)av_rescale_rnd(swr_get_delay(swr, codec->sample_rate) + frame->nb_samples, 48000, codec->sample_rate, AV_ROUND_UP);
            if (outCount < 1) outCount = 1;
            std::vector<int16_t> tmp((size_t)outCount * 2 + 8);
            uint8_t* planes[1] = { (uint8_t*)tmp.data() };
            int got = swr_convert(swr, planes, outCount, (const uint8_t**)frame->data, frame->nb_samples);
            if (got <= 0) continue;
            double pts = origin;
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                pts = frame->best_effort_timestamp * av_q2d(fmt->streams[stream]->time_base);
            lastPts = pts;
            int start = 0;
            if (gotTotal == 0) {
                int drop = (int)((origin - pts) * 48000.0);
                if (drop >= got) continue;
                if (drop > 0) start = drop;
            }
            fifo.insert(fifo.end(), tmp.begin() + (size_t)start * 2, tmp.begin() + (size_t)got * 2);
        }
    }
    std::vector<int16_t> fifo;
    int Pull(std::vector<int16_t>& dst, int count) {
        dst.assign((size_t)count * 2, 0);
        if (!fmt || eof || want <= 0) return 0;
        int need = std::min(count, want);
        int guard = 0;
        while ((int)fifo.size() / 2 < need && !eof && guard < 20000) {
            guard++;
            Take();
            if ((int)fifo.size() / 2 >= need) break;
            if (av_read_frame(fmt, pkt) < 0) { eof = true; break; }
            packets++;
            if (pkt->stream_index == stream) {
                audioPackets++;
                int send = avcodec_send_packet(codec, pkt);
                if (send == AVERROR(EAGAIN)) { Take(); send = avcodec_send_packet(codec, pkt); }
                if (send == 0) Take();
            }
            av_packet_unref(pkt);
        }
        if (eof) {
            avcodec_send_packet(codec, nullptr);
            Take();
            std::vector<int16_t> tmp(48000 * 2);
            uint8_t* planes[1] = { (uint8_t*)tmp.data() };
            int got = swr_convert(swr, planes, 48000, nullptr, 0);
            if (got > 0) fifo.insert(fifo.end(), tmp.begin(), tmp.begin() + (size_t)got * 2);
        }
        int have = std::min(need, (int)fifo.size() / 2);
        if (have > 0) {
            memcpy(dst.data(), fifo.data(), (size_t)have * 2 * sizeof(int16_t));
            fifo.erase(fifo.begin(), fifo.begin() + (size_t)have * 2);
            gotTotal += have;
            want -= have;
        }
        return have;
    }
};
static AsrRead asrRead;

static void AsrPump() {
    if (!asrOn) return;
    if (asrPhase == 0) {
        std::string err;
        if (!AsrOpen(err)) {
            asrOn = false;
            previewStatus = err;
            return;
        }
        asrPhase = 1;
        asrList.clear();
        AsrLog("---- timeline clips ----");
        for (int i = 0; i < (int)clips.size(); i++) {
            const Clip& c = clips[i];
            const char* why = "queued";
            if (c.muted) why = "skip muted";
            else if (c.duration < 0.3f) why = "skip short";
            else if (c.mediaId < 0 || c.mediaId >= (int)media.size()) why = "skip no media";
            else if (media[c.mediaId].type == "image") why = "skip image";
            else if (!ClipCanSpeak(i)) why = "skip no audio stream";
            char line[768];
            std::string path = (c.mediaId >= 0 && c.mediaId < (int)media.size()) ? media[c.mediaId].path : "";
            snprintf(line, sizeof(line), "clip %d %s start=%.2f dur=%.2f %s %s",
                     i, TrackLabel(c.track), c.startTime, c.duration, why, path.c_str());
            AsrLog(line);
            if (why[0] == 'q' && c.track >= 0 && c.track <= 6) asrList.push_back(i);
        }
        std::sort(asrList.begin(), asrList.end(), [](int a, int b) {
            if (clips[a].track != clips[b].track) return clips[a].track < clips[b].track;
            return clips[a].startTime < clips[b].startTime;
        });
        asrListPos = 0;
        asrPcm.clear();
        asrSegs.clear();
        asrCursor = 0;
        asrChunk = 0;
        asrSpans.clear();
        asrSpanPos = 0;
        asrSpansReady = false;
        if (asrList.empty()) {
            asrOn = false;
            asrRead.Close();
            AsrClose();
            previewStatus = "No audio clips to transcribe";
            return;
        }
        previewStatus = std::string("Reading ") + TrackLabel(clips[asrList[0]].track);
        return;
    }
    if (asrListPos < 0 || asrListPos >= (int)asrList.size()) {
        subs.clear();
        std::sort(asrSegs.begin(), asrSegs.end(), [](const AsrSeg& a, const AsrSeg& b) { return a.start < b.start; });
        for (const AsrSeg& s : asrSegs) {
            Subtitle sub;
            sub.start = std::max(0.0f, s.start);
            sub.duration = std::max(0.35f, s.end - s.start);
            sub.text = s.text;
            subs.push_back(sub);
        }
        SplitLongSubs();
        asrOn = false;
        asrPcm.clear();
        asrPcm.shrink_to_fit();
        asrRead.Close();
        AsrClose();
        previewStatus = subs.empty() ? "No speech found" : ("Subtitles " + std::to_string(subs.size()));
        AsrLog("result subtitles=" + std::to_string(subs.size()));
        for (size_t i = 0; i < subs.size(); i++) {
            std::string one = subs[i].text;
            for (char& ch : one) if (ch == '\n' || ch == '\r') ch = ' ';
            char head[80];
            snprintf(head, sizeof(head), "sub %d %.2f+%.2f ", (int)i, subs[i].start, subs[i].duration);
            AsrLog(std::string(head) + one);
        }
        return;
    }
    int ci = asrList[asrListPos];
    const Clip& c = clips[ci];
    if (asrPhase == 1) {
        if (asrPcm.empty()) {
            asrCursor = c.startTime;
            asrSpeed = c.speed < 0.05f ? 1.0f : c.speed;
            if (!asrRead.Open(c)) {
                AsrLog(std::string("decode failed ") + TrackLabel(c.track));
                asrRead.Close();
                asrListPos++;
                return;
            }
            char line[256];
            snprintf(line, sizeof(line), "read clip %d %s timeline %.2f-%.2f", ci, TrackLabel(c.track), c.startTime, c.startTime + c.duration);
            AsrLog(line);
        }
        if (asrRead.want <= 0 || asrRead.eof) {
            char line[200];
            snprintf(line, sizeof(line), "audio end %.2fs packets=%d audioPackets=%d lastPts=%.2f eof=%d",
                     asrRead.gotTotal / 48000.0, asrRead.packets, asrRead.audioPackets, asrRead.lastPts, asrRead.eof ? 1 : 0);
            AsrLog(line);
            asrRead.Close();
            asrPhase = 2;
            asrOff = 0;
            asrSpansReady = false;
            asrBase = c.startTime;
            previewStatus = std::string("Transcribing ") + TrackLabel(c.track);
            return;
        }
        const int n48 = 48000;
        std::vector<int16_t> block;
        int have = asrRead.Pull(block, n48);
        if (have <= 0) {
            asrRead.eof = true;
            return;
        }
        asrPcm.reserve(asrPcm.size() + 16000);
        double energy = 0;
        float peak = 0;
        int use = std::min(have, n48);
        for (int i = 0; i < use; i++) {
            float a = std::fabs((float)block[(size_t)i * 2]) / 32768.0f;
            float b = std::fabs((float)block[(size_t)i * 2 + 1]) / 32768.0f;
            peak = std::max(peak, std::max(a, b));
        }
        int outN = std::max(1, use / 3);
        for (int i = 0; i < outN; i++) {
            int j = i * 3;
            if (j + 2 >= use) break;
            float l = block[(size_t)j * 2] + block[(size_t)(j + 1) * 2] + block[(size_t)(j + 2) * 2];
            float r = block[(size_t)j * 2 + 1] + block[(size_t)(j + 1) * 2 + 1] + block[(size_t)(j + 2) * 2 + 1];
            float sample = (l + r) / 6.0f / 32768.0f;
            energy += (double)sample * sample;
            asrPcm.push_back(sample);
        }
        asrCursor += use / 48000.0;
        int sec = (int)((asrRead.gotTotal) / 48000.0);
        if (sec <= 8 || sec % 5 == 0) {
            char line[180];
            snprintf(line, sizeof(line), "audio %s t=%.1f rms=%.4f peak=%.3f lastPts=%.2f",
                     TrackLabel(c.track), asrRead.gotTotal / 48000.0 - use / 48000.0, std::sqrt(energy / std::max(1, outN)), peak, asrRead.lastPts);
            AsrLog(line);
        }
        int pct = c.duration > 0 ? (int)(100.0 * (asrRead.gotTotal / 48000.0) / (c.duration * asrSpeed)) : 100;
        previewStatus = std::string("Reading ") + TrackLabel(c.track) + " " + std::to_string(pct) + "%";
        return;
    }
    if (!asrSpansReady) {
        asrSpansReady = true;
        asrOff = 0;
        float peak = 0;
        double energy = 0;
        for (float s : asrPcm) {
            peak = std::max(peak, std::fabs(s));
            energy += (double)s * s;
        }
        char head[180];
        snprintf(head, sizeof(head), "pcm %.2fs rms=%.4f peak=%.3f %s",
                 asrPcm.size() / 16000.0, asrPcm.empty() ? 0.0 : std::sqrt(energy / asrPcm.size()), peak, TrackLabel(c.track));
        AsrLog(head);
        if (asrPcm.size() < 1600) {
            asrListPos++;
            asrPcm.clear();
            asrPhase = 1;
            asrSpansReady = false;
        }
        return;
    }
    if (asrOff >= (int)asrPcm.size()) {
        asrListPos++;
        asrPcm.clear();
        asrPhase = 1;
        asrSpansReady = false;
        return;
    }
    const int window = 16000 * 20;
    int off = asrOff;
    int n = std::min(window, (int)asrPcm.size() - off);
    float origin = off / 16000.0f;
    previewStatus = std::string("Transcribing ") + TrackLabel(c.track) + " " + std::to_string((int)origin) + "s";
    std::vector<AsrSeg> part;
    std::string err;
    if (!AsrTranscribeChunk(asrPcm.data() + off, n, origin, asrLang, part, err)) {
        asrOn = false;
        asrRead.Close();
        AsrClose();
        previewStatus = err;
        return;
    }
    for (const AsrSeg& s : part) {
        if (s.text.empty()) continue;
        float t0 = asrBase + s.start / asrSpeed;
        float t1 = asrBase + s.end / asrSpeed;
        SplitTimedText(t0, std::max(t0 + 0.25f, t1), s.text, asrSegs);
    }
    if (part.empty()) AsrLog("window produced no words");
    float covered = 0.0f;
    if (!part.empty()) covered = part.back().end - origin;
    float span = n / 16000.0f;
    int advance = n;
    if (!part.empty() && covered + 0.8f < span) {
        int from = off + std::max(0, (int)(covered * 16000.0f));
        double tail = 0;
        int cnt = 0;
        for (int i = from; i < off + n && i < (int)asrPcm.size(); i++) { tail += (double)asrPcm[i] * asrPcm[i]; cnt++; }
        if (cnt > 0 && tail / cnt > 0.00004) {
            advance = std::max(16000, (int)(covered * 16000.0f));
            if (advance > n) advance = n;
            AsrLog("model stopped early, continuing");
        }
    }
    if (advance < 1) advance = 1;
    asrOff += advance;
}

void QueueAudioBlock(double timelineTime, int sampleCount) {
    if (!audioDev || sampleCount <= 0) return;
    std::vector<int16_t> out((size_t)sampleCount * 2);
    MixTimeline(timelineTime, sampleCount, out.data());
    SDL_QueueAudio(audioDev, out.data(), (Uint32)(out.size() * sizeof(int16_t)));
}

void PumpAudio() {
    if (!audioDev || !isPlaying || draggingPlayhead) return;
    double queued = SDL_GetQueuedAudioSize(audioDev) / (48000.0 * 4.0);
    double t = currentTime + queued;
    int guard = 0;
    while (queued < 0.18 && guard < 5) {
        int n = 2048;
        QueueAudioBlock(t, n);
        t += n / 48000.0;
        queued += n / 48000.0;
        guard++;
    }
}
struct ExpDec {
    int mediaId = -1;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    SwsContext* fit = nullptr;
    int stream = -1;
    int rw = 0, rh = 0;
    int fitW = 0, fitH = 0;
    double lastPts = -1;
    bool failed = false;
    bool imageDone = false;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> fitted;
    double srcFps = 0;
    double holdPts[2] = {-1, -1};
    std::vector<uint8_t> hold[2];
    int holdW = 0, holdH = 0;
    int holdSlot = 0;
    HwDec* hw = nullptr;
};

struct ExportSession {
    bool active = false;
    bool headerWritten = false;
    bool videoOnly = false;
    std::string path;
    std::string status;
    int w = 0, h = 0, fps = 30;
    int frameIndex = 0;
    int total = 0;
    int aFrameSize = 1024;
    double timelineEnd = 0;
    int64_t audioWritten = 0;
    int64_t aPts = 0;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* aframe = nullptr;
    SwsContext* toYuv = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    std::vector<ExpDec> decs;
    std::vector<uint8_t> canvas;
    std::vector<int16_t> afifo;
    bool rifeOn = false;
    bool audioOnly = false;
    int64_t audioTotal = 0;
    bool burnSubs = true;
    int rifeFails = 0;
    int phase = 0;
    int outFps = 0;
    int baseFps = 0;
    std::string finalPath;
    std::string tempPath;
    AVFormatContext* rifeIn = nullptr;
    AVFormatContext* rifeAudio = nullptr;
    AVCodecContext* rifeDec = nullptr;
    AVFrame* rifeFrame = nullptr;
    AVPacket* rifePkt = nullptr;
    SwsContext* rifeSws = nullptr;
    int rifeStream = -1;
    std::vector<uint8_t> prevRGBA, nextRGBA, rifeMid;
    bool haveNext = false;
    bool emitMid = false;
    int rifeFactor = 2;
    int rifeSlot = 0;

    void CloseAll();
    bool Begin(const std::string& outPath, int width, int height, int framesPerSec, bool useRife, bool burn);
    bool BeginMp3(const std::string& outPath);
    void Step();
    void StepMp3();
    void StepRife();
    bool BeginRifePass();
    void SealPass();
    bool ReadRifeFrame(std::vector<uint8_t>& dst);
    void ReleasePassDecoder();
    void Cancel();
    void Fail(const char* msg);
    void Finish(bool ok);
    bool WriteEncoded(AVCodecContext* ctx, AVStream* st);
    bool DrainAudio(bool padFinal);
    int OpenDec(int mediaId);
    bool ReadOne(ExpDec& d);
    bool EnsureFrame(int mediaId, double mediaTime);
    bool HoldFrame(ExpDec& d, double want, std::vector<uint8_t>& dst, int& rw, int& rh);
    bool HoldFrameView(ExpDec& d, double want, const uint8_t*& data, int& rw, int& rh);
    bool RifePair(int mediaId, double mediaT, float speed, std::vector<uint8_t>& out, int& rw, int& rh);
    void BlitToCanvas(int idx, const Clip* place = nullptr, float cover = 1.0f);
};

void FitInside(int sw, int sh, int dw, int dh, int& ox, int& oy, int& fw, int& fh) {
    if (sw < 1 || sh < 1) { fw = dw; fh = dh; ox = oy = 0; return; }
    double sa = (double)sw / (double)sh;
    double da = (double)dw / (double)dh;
    if (sa > da) { fw = dw; fh = (int)(dw / sa); }
    else { fh = dh; fw = (int)(dh * sa); }
    if (fw < 2) fw = 2;
    if (fh < 2) fh = 2;
    fw &= ~1;
    fh &= ~1;
    ox = ((dw - fw) / 2) & ~1;
    oy = ((dh - fh) / 2) & ~1;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
}

void ExportSession::CloseAll() {
    active = false;
    for (auto& d : decs) {
        if (d.fit) sws_freeContext(d.fit);
        if (d.sws) sws_freeContext(d.sws);
        if (d.frame) av_frame_free(&d.frame);
        if (d.pkt) av_packet_free(&d.pkt);
        if (d.ctx) avcodec_free_context(&d.ctx);
        if (d.hw) { d.hw->Free(); delete d.hw; d.hw = nullptr; }
        if (d.fmt) avformat_close_input(&d.fmt);
        d.fit = nullptr;
        d.sws = nullptr;
    }
    decs.clear();
    if (swr) swr_free(&swr);
    if (toYuv) { sws_freeContext(toYuv); toYuv = nullptr; }
    if (vframe) av_frame_free(&vframe);
    if (aframe) av_frame_free(&aframe);
    if (pkt) av_packet_free(&pkt);
    if (vctx) avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (fmt) {
        if (fmt->pb) avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        fmt = nullptr;
    }
    vst = nullptr;
    ast = nullptr;
    canvas.clear();
    afifo.clear();
    headerWritten = false;
    audioOnly = false;
    audioTotal = 0;
    ReleasePassDecoder();
    if (!tempPath.empty()) { ::remove(tempPath.c_str()); tempPath.clear(); }
    if (rifeOn) RifeShutdown();
    rifeOn = false;
    rifeFails = 0;
    phase = 0;
    outFps = 0;
    baseFps = 0;
    rifeFactor = 2;
    rifeSlot = 0;
    finalPath.clear();
}

void ExportSession::Fail(const char* msg) {
    status = msg ? msg : "Export failed";
    previewStatus = status;
    CloseAll();
}

bool ExportSession::WriteEncoded(AVCodecContext* ctx, AVStream* st) {
    if (!ctx || !st || !fmt) return false;
    if (!pkt) pkt = av_packet_alloc();
    while (pkt) {
        av_packet_unref(pkt);
        int r = avcodec_receive_packet(ctx, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
        if (r < 0) return false;
        av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
        pkt->stream_index = st->index;
        r = av_interleaved_write_frame(fmt, pkt);
        if (r < 0) return false;
    }
    return false;
}

bool ExportSession::DrainAudio(bool padFinal) {
    if (!actx || !aframe || !swr) return true;
    if (padFinal && !afifo.empty() && (int)afifo.size() / 2 < aFrameSize)
        afifo.resize((size_t)aFrameSize * 2, 0);
    while ((int)afifo.size() / 2 >= aFrameSize) {
        if (av_frame_make_writable(aframe) < 0) return false;
        const uint8_t* inData[1] = { (const uint8_t*)afifo.data() };
        int got = swr_convert(swr, aframe->data, aFrameSize, inData, aFrameSize);
        if (got <= 0) return false;
        aframe->nb_samples = got;
        aframe->pts = aPts;
        aPts += got;
        afifo.erase(afifo.begin(), afifo.begin() + (size_t)aFrameSize * 2);
        if (avcodec_send_frame(actx, aframe) < 0) return false;
        if (!WriteEncoded(actx, ast)) return false;
    }
    return true;
}

int ExportSession::OpenDec(int mediaId) {
    for (int i = 0; i < (int)decs.size(); i++)
        if (decs[i].mediaId == mediaId) return i;
    ExpDec d;
    d.mediaId = mediaId;
    if (mediaId >= 0 && mediaId < (int)media.size() && IsSolidPath(media[mediaId].path)) {
        BuildSolidPixels(SolidKindFromPath(media[mediaId].path), d.rgba, 640, 360);
        d.rw = 640;
        d.rh = 360;
        d.lastPts = 0;
        d.imageDone = true;
        decs.push_back(std::move(d));
        return (int)decs.size() - 1;
    }
    if (mediaId < 0 || mediaId >= (int)media.size() ||
        avformat_open_input(&d.fmt, media[mediaId].path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(d.fmt, nullptr) < 0) {
        d.failed = true;
        decs.push_back(std::move(d));
        return (int)decs.size() - 1;
    }
    for (unsigned i = 0; i < d.fmt->nb_streams; i++) {
        if (d.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { d.stream = (int)i; break; }
    }
    if (d.stream >= 0) {
        AVRational rate = d.fmt->streams[d.stream]->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = d.fmt->streams[d.stream]->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) d.srcFps = (double)rate.num / (double)rate.den;
        if (d.srcFps < 1.0 || d.srcFps > 240.0) d.srcFps = 0;
    }
    const AVCodec* codec = (d.stream >= 0) ? avcodec_find_decoder(d.fmt->streams[d.stream]->codecpar->codec_id) : nullptr;
    const char* how = "CPU";
    if (d.stream < 0 || !codec ||
        !OpenDecoder(d.ctx, codec, d.fmt->streams[d.stream]->codecpar, d.hw, how)) {
        d.failed = true;
        decs.push_back(std::move(d));
        return (int)decs.size() - 1;
    }
    d.frame = av_frame_alloc();
    d.pkt = av_packet_alloc();
    decs.push_back(std::move(d));
    return (int)decs.size() - 1;
}

bool ExportSession::ReadOne(ExpDec& d) {
    if (!d.fmt || !d.ctx || !d.pkt || !d.frame) return false;
    while (av_read_frame(d.fmt, d.pkt) >= 0) {
        if (d.pkt->stream_index != d.stream) { av_packet_unref(d.pkt); continue; }
        int send = avcodec_send_packet(d.ctx, d.pkt);
        av_packet_unref(d.pkt);
        if (send < 0 && send != AVERROR(EAGAIN)) continue;
        if (avcodec_receive_frame(d.ctx, d.frame) != 0) continue;
        AVFrame* src = DownloadFrame(d.hw, d.frame);
        if (!src || src->width <= 0 || src->height <= 0) continue;
        AVPixelFormat srcFmt = (AVPixelFormat)src->format;
        if (srcFmt == AV_PIX_FMT_NONE) continue;
        if (!d.sws || d.rw != src->width || d.rh != src->height) {
            if (d.sws) sws_freeContext(d.sws);
            d.rw = src->width;
            d.rh = src->height;
            d.sws = sws_getContext(d.rw, d.rh, srcFmt, d.rw, d.rh, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
            d.rgba.assign((size_t)d.rw * (size_t)d.rh * 4, 0);
            if (d.fit) { sws_freeContext(d.fit); d.fit = nullptr; d.fitW = d.fitH = 0; }
        }
        if (!d.sws || d.rgba.empty()) return false;
        uint8_t* dstData[1] = { d.rgba.data() };
        int dstLines[1] = { d.rw * 4 };
        sws_scale(d.sws, src->data, src->linesize, 0, d.rh, dstData, dstLines);
        if (d.frame->best_effort_timestamp != AV_NOPTS_VALUE)
            d.lastPts = d.frame->best_effort_timestamp * av_q2d(d.fmt->streams[d.stream]->time_base);
        else {
            double step = 1.0 / 30.0;
            if (d.ctx->framerate.num > 0 && d.ctx->framerate.den > 0)
                step = (double)d.ctx->framerate.den / (double)d.ctx->framerate.num;
            d.lastPts = (d.lastPts < 0 ? 0 : d.lastPts) + step;
        }
        return true;
    }
    return false;
}

bool ExportSession::EnsureFrame(int mediaId, double mediaTime) {
    int idx = OpenDec(mediaId);
    if (idx < 0 || idx >= (int)decs.size()) return false;
    ExpDec& d = decs[idx];
    if (mediaId >= 0 && mediaId < (int)media.size() && IsSolidPath(media[mediaId].path))
        return d.rw > 0 && !d.rgba.empty();
    if (d.failed || !d.fmt) return false;
    bool isImage = media[mediaId].type == "image";
    if (isImage && d.imageDone) return d.rw > 0;
    if (mediaTime < 0) mediaTime = 0;
    bool needSeek = !isImage && (d.lastPts < 0 || mediaTime + 0.08 < d.lastPts || mediaTime - d.lastPts > 1.25);
    if (needSeek) {
        int64_t ts = (int64_t)(mediaTime / av_q2d(d.fmt->streams[d.stream]->time_base));
        av_seek_frame(d.fmt, d.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d.ctx);
        d.lastPts = -1;
    }
    int guard = 0;
    while (guard++ < 160) {
        if (d.rw > 0 && d.lastPts >= 0 && d.lastPts + 0.0001 >= mediaTime) {
            if (isImage) d.imageDone = true;
            return true;
        }
        if (!ReadOne(d)) break;
        if (isImage) { d.imageDone = true; return d.rw > 0; }
    }
    return d.rw > 0;
}

bool ExportSession::HoldFrame(ExpDec& d, double want, std::vector<uint8_t>& dst, int& rw, int& rh) {
    double tol = 0.45 / std::max(1.0, d.srcFps);
    int best = -1;
    double bestd = 1e9;
    for (int i = 0; i < 2; i++) {
        if (d.holdPts[i] < 0 || d.hold[i].empty()) continue;
        double dd = std::fabs(d.holdPts[i] - want);
        if (dd < bestd) { bestd = dd; best = i; }
    }
    if (best >= 0 && bestd <= tol) {
        dst = d.hold[best];
        rw = d.holdW;
        rh = d.holdH;
        return rw > 0;
    }
    if (d.lastPts >= 0 && want + 0.012 < d.lastPts && d.fmt && d.ctx && d.stream >= 0) {
        int64_t ts = (int64_t)(want / av_q2d(d.fmt->streams[d.stream]->time_base));
        av_seek_frame(d.fmt, d.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d.ctx);
        d.lastPts = -1;
    }
    if (!EnsureFrame(d.mediaId, want) || d.rgba.empty()) return false;
    d.holdPts[d.holdSlot] = d.lastPts;
    d.hold[d.holdSlot] = d.rgba;
    d.holdW = d.rw;
    d.holdH = d.rh;
    d.holdSlot ^= 1;
    dst = d.rgba;
    rw = d.rw;
    rh = d.rh;
    return true;
}

bool ExportSession::HoldFrameView(ExpDec& d, double want, const uint8_t*& data, int& rw, int& rh) {
    double tol = 0.45 / std::max(1.0, d.srcFps);
    int best = -1;
    double bestd = 1e9;
    for (int i = 0; i < 2; i++) {
        if (d.holdPts[i] < 0 || d.hold[i].empty()) continue;
        double dd = std::fabs(d.holdPts[i] - want);
        if (dd < bestd) { bestd = dd; best = i; }
    }
    if (best >= 0 && bestd <= tol) {
        data = d.hold[best].data();
        rw = d.holdW;
        rh = d.holdH;
        return data && rw > 0;
    }
    if (d.lastPts >= 0 && want + 0.012 < d.lastPts && d.fmt && d.ctx && d.stream >= 0) {
        int64_t ts = (int64_t)(want / av_q2d(d.fmt->streams[d.stream]->time_base));
        av_seek_frame(d.fmt, d.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d.ctx);
        d.lastPts = -1;
    }
    if (!EnsureFrame(d.mediaId, want) || d.rgba.empty()) return false;
    d.holdPts[d.holdSlot] = d.lastPts;
    d.hold[d.holdSlot] = d.rgba;
    d.holdW = d.rw;
    d.holdH = d.rh;
    int slot = d.holdSlot;
    d.holdSlot ^= 1;
    data = d.hold[slot].data();
    rw = d.holdW;
    rh = d.holdH;
    return data != nullptr;
}

bool ExportSession::RifePair(int mediaId, double mediaT, float speed, std::vector<uint8_t>& out, int& rw, int& rh) {
    if (phase != 1 || !rifeOn || mediaId < 0 || mediaId >= (int)media.size()) return false;
    if (media[mediaId].type == "image") return false;
    int idx = OpenDec(mediaId);
    if (idx < 0 || idx >= (int)decs.size()) return false;
    ExpDec& d = decs[idx];
    if (d.failed || d.srcFps < 1.0) return false;
    double mediaStep = (double)std::max(0.05f, speed) / (double)std::max(1, fps);
    if (mediaStep >= (1.0 / d.srcFps) * 0.92) return false;
    double pos = std::max(0.0, mediaT) * d.srcFps;
    double base = std::floor(pos);
    double frac = pos - base;
    if (frac < 0.04 || frac > 0.96) return false;
    double t0 = base / d.srcFps;
    double t1 = (base + 1.0) / d.srcFps;
    const uint8_t* ap = nullptr;
    const uint8_t* bp = nullptr;
    int aw = 0, ah = 0, bw = 0, bh = 0;
    if (!HoldFrameView(d, t0, ap, aw, ah) || !ap) return false;
    bool haveB = false;
    double tol = 0.45 / d.srcFps;
    for (int i = 0; i < 2; i++) {
        if (d.holdPts[i] >= 0 && std::fabs(d.holdPts[i] - t1) <= tol && !d.hold[i].empty()) haveB = true;
    }
    std::vector<uint8_t> keepA;
    const uint8_t* useA = ap;
    if (!haveB) {
        keepA.assign(ap, ap + (size_t)aw * (size_t)ah * 4);
        useA = keepA.data();
    }
    if (!HoldFrameView(d, t1, bp, bw, bh) || !bp) return false;
    if (aw != bw || ah != bh || aw < 2) return false;
    out.assign((size_t)aw * (size_t)ah * 4, 255);
    if (!RifeRun(useA, bp, aw, ah, (float)frac, out.data())) {
        if (++rifeFails > 2) {
            rifeOn = false;
            previewStatus = "RIFE stopped. Export continues with the original frames.";
        }
        return false;
    }
    rifeFails = 0;
    rw = aw;
    rh = ah;
    return true;
}

void ExportSession::BlitToCanvas(int idx, const Clip* place, float cover) {
    if (cover < 0.0f) cover = 0.0f;
    if (cover > 1.0f) cover = 1.0f;
    if (cover <= 0.001f) return;
    if (idx < 0 || idx >= (int)decs.size()) return;
    ExpDec& d = decs[idx];
    if (d.rw < 1 || d.rh < 1 || d.rgba.empty()) return;
    int ox, oy, fw, fh;
    FitInside(d.rw, d.rh, w, h, ox, oy, fw, fh);
    if (fw < 2 || fh < 2) return;
    if (!d.fit || d.fitW != fw || d.fitH != fh) {
        if (d.fit) sws_freeContext(d.fit);
        d.fit = sws_getContext(d.rw, d.rh, AV_PIX_FMT_RGBA, fw, fh, AV_PIX_FMT_RGBA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        d.fitW = fw;
        d.fitH = fh;
        d.fitted.assign((size_t)fw * (size_t)fh * 4, 0);
    }
    if (!d.fit || d.fitted.empty()) return;
    uint8_t* sData[1] = { d.rgba.data() };
    int sLines[1] = { d.rw * 4 };
    uint8_t* dData[1] = { d.fitted.data() };
    int dLines[1] = { fw * 4 };
    sws_scale(d.fit, sData, sLines, 0, d.rh, dData, dLines);
    bool plain = !place
        || (std::fabs(place->gsX) < 0.0005f && std::fabs(place->gsY) < 0.0005f
            && std::fabs(place->gsScale - 1.0f) < 0.0005f && std::fabs(place->gsAspect - 1.0f) < 0.0005f
            && std::fabs(place->gsRot) < 0.05f);
    if (plain) {
        for (int y = 0; y < fh; y++) {
            size_t dstOff = ((size_t)(oy + y) * (size_t)w + (size_t)ox) * 4;
            const uint8_t* src = d.fitted.data() + (size_t)y * (size_t)fw * 4;
            if (dstOff + (size_t)fw * 4 > canvas.size()) break;
            for (int x = 0; x < fw; x++) {
                const uint8_t* s = src + (size_t)x * 4;
                uint8_t* dst = canvas.data() + dstOff + (size_t)x * 4;
                unsigned a = s[3];
                if (cover < 0.999f) a = (unsigned)((float)a * cover + 0.5f);
                if (a > 255) a = 255;
                if (a >= 250 && cover >= 0.999f) {
                    dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255;
                } else if (a > 0) {
                    dst[0] = (uint8_t)((s[0] * a + dst[0] * (255 - a)) / 255);
                    dst[1] = (uint8_t)((s[1] * a + dst[1] * (255 - a)) / 255);
                    dst[2] = (uint8_t)((s[2] * a + dst[2] * (255 - a)) / 255);
                    dst[3] = 255;
                }
            }
        }
        return;
    }
    float aspect = place->gsAspect < 0.05f ? 1.0f : place->gsAspect;
    float cx = ox + fw * 0.5f + place->gsX * w * 0.5f;
    float cy = oy + fh * 0.5f + place->gsY * h * 0.5f;
    float hw = fw * 0.5f * place->gsScale * aspect;
    float hh = fh * 0.5f * place->gsScale;
    float rad = place->gsRot * 0.01745329252f;
    float cs = std::cos(rad), sn = std::sin(rad);
    float ext = std::sqrt(hw * hw + hh * hh) + 2.0f;
    int x0 = std::max(0, (int)std::floor(cx - ext));
    int y0 = std::max(0, (int)std::floor(cy - ext));
    int x1 = std::min(w, (int)std::ceil(cx + ext));
    int y1 = std::min(h, (int)std::ceil(cy + ext));
    const uint8_t* src = d.fitted.data();
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (x + 0.5f) - cx;
            float dy = (y + 0.5f) - cy;
            float lx = dx * cs + dy * sn;
            float ly = -dx * sn + dy * cs;
            if (lx < -hw || lx > hw || ly < -hh || ly > hh) continue;
            int sx = (int)((lx / (hw * 2.0f) + 0.5f) * fw);
            int sy = (int)((ly / (hh * 2.0f) + 0.5f) * fh);
            if (sx < 0 || sy < 0 || sx >= fw || sy >= fh) continue;
            const uint8_t* s = src + ((size_t)sy * fw + sx) * 4;
            uint8_t* dst = canvas.data() + ((size_t)y * w + x) * 4;
            unsigned a = s[3];
            if (cover < 0.999f) a = (unsigned)((float)a * cover + 0.5f);
            if (a > 255) a = 255;
            if (a >= 250 && cover >= 0.999f) {
                dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255;
            } else if (a > 0) {
                dst[0] = (uint8_t)((s[0] * a + dst[0] * (255 - a)) / 255);
                dst[1] = (uint8_t)((s[1] * a + dst[1] * (255 - a)) / 255);
                dst[2] = (uint8_t)((s[2] * a + dst[2] * (255 - a)) / 255);
                dst[3] = 255;
            }
        }
    }
}

bool ExportSession::Begin(const std::string& outPath, int width, int height, int framesPerSec, bool useRife, bool burn) {
    CloseAll();
    path = outPath;
    w = width & ~1;
    h = height & ~1;
    fps = framesPerSec;
    if (w < 2 || h < 2 || fps < 1) { status = "Bad export settings"; previewStatus = status; return false; }
    timelineEnd = 0;
    for (const auto& c : clips) {
        double end = (double)c.startTime + (double)c.duration;
        if (c.reverbOn) end += std::min(5.0, (double)c.reverbTime);
        if (c.delayOn) end += 180.0 / std::max(40.0f, c.delayTempo);
        timelineEnd = std::max(timelineEnd, end);
    }
    for (const auto& s : subs) timelineEnd = std::max(timelineEnd, (double)s.start + (double)s.duration);
    if (timelineEnd < 0.05 || (clips.empty() && subs.empty())) { status = "Nothing to export"; previewStatus = status; return false; }
    path = outPath;
    fps = framesPerSec;
    phase = 0;
    outFps = 0;
    baseFps = framesPerSec;
    rifeFactor = 2;
    rifeSlot = 0;
    finalPath.clear();
    tempPath.clear();
    bool highFps = framesPerSec == 120 || framesPerSec == 240;
    bool triple = framesPerSec == 72;
    if (highFps || triple) useRife = true;
    if (useRife && (highFps || triple || (framesPerSec >= 48 && (framesPerSec % 2) == 0))) {
        outFps = framesPerSec;
        if (triple) {
            baseFps = 24;
            rifeFactor = 3;
        } else if (highFps) {
            baseFps = 60;
            rifeFactor = framesPerSec / 60;
        } else {
            baseFps = framesPerSec / 2;
            rifeFactor = 2;
        }
        if (rifeFactor < 2) rifeFactor = 2;
        finalPath = outPath;
        tempPath = outPath + ".rife-tmp.mp4";
        path = tempPath;
        fps = baseFps;
        rifeOn = true;
    }
    burnSubs = burn;
    total = (int)std::ceil(timelineEnd * (double)fps);
    if (total < 1) total = 1;
    frameIndex = 0;
    audioWritten = 0;
    aPts = 0;
    videoOnly = false;
    SetPlaying(false);
    ResetAudioSync();

    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.c_str()) < 0 || !fmt) {
        status = "Cannot create mp4"; previewStatus = status; CloseAll(); return false;
    }
    const bool large = (w > 4096 || h > 4096);
    const char* encLabel = "x264";
    const AVCodec* vcodec = nullptr;
    bool useNv = false;
    if (large) {
        vcodec = avcodec_find_encoder_by_name("hevc_nvenc");
        if (vcodec) useNv = true;
        if (!vcodec) vcodec = avcodec_find_encoder_by_name("libx265");
    }
    if (!vcodec) {
        vcodec = avcodec_find_encoder_by_name("h264_nvenc");
        if (vcodec) useNv = true;
    }
    if (!vcodec) {
        useNv = false;
        vcodec = avcodec_find_encoder_by_name("libx264");
    }
    if (!vcodec) vcodec = avcodec_find_encoder_by_name("h264_mf");
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) { status = "H.264 encoder not found"; previewStatus = status; CloseAll(); return false; }
    int64_t br = (int64_t)w * (int64_t)h;
    if (br < 4000000) br = 4000000;
    if (br > 80000000) br = 80000000;
    auto openEnc = [&](const AVCodec* c, bool nv) -> bool {
        if (!c) return false;
        if (vctx) avcodec_free_context(&vctx);
        vctx = avcodec_alloc_context3(c);
        if (!vctx) return false;
        vctx->width = w;
        vctx->height = h;
        vctx->time_base = AVRational{1, fps};
        vctx->framerate = AVRational{fps, 1};
        vctx->gop_size = fps;
        vctx->max_b_frames = 0;
        vctx->pix_fmt = AV_PIX_FMT_YUV420P;
        vctx->bit_rate = br;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
            vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (nv && vctx->priv_data) {
            av_opt_set(vctx->priv_data, "preset", "p4", 0);
            av_opt_set(vctx->priv_data, "rc", "vbr", 0);
            av_opt_set(vctx->priv_data, "cq", "19", 0);
        } else if (c->name && strcmp(c->name, "libx264") == 0 && vctx->priv_data) {
            av_opt_set(vctx->priv_data, "preset", (w >= 3840 ? "ultrafast" : "veryfast"), 0);
            av_opt_set(vctx->priv_data, "crf", "20", 0);
        } else if (c->name && strcmp(c->name, "libx265") == 0 && vctx->priv_data) {
            av_opt_set(vctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(vctx->priv_data, "crf", "22", 0);
        }
        return avcodec_open2(vctx, c, nullptr) >= 0;
    };
    if (!openEnc(vcodec, useNv)) {
        useNv = false;
        const AVCodec* fb = avcodec_find_encoder_by_name(large ? "libx265" : "libx264");
        if (!fb) fb = avcodec_find_encoder_by_name("libx264");
        if (!fb) fb = avcodec_find_encoder_by_name("h264_mf");
        if (!fb) fb = avcodec_find_encoder(AV_CODEC_ID_H264);
        vcodec = fb;
        if (!openEnc(vcodec, false)) {
            status = "Cannot open video encoder"; previewStatus = status; CloseAll(); return false;
        }
    }
    if (useNv) encLabel = large ? "HEVC NVENC" : "NVENC";
    else if (vcodec && vcodec->name && strstr(vcodec->name, "265")) encLabel = "x265";
    else if (vcodec && vcodec->name && strstr(vcodec->name, "mf")) encLabel = "MediaFoundation";
    else encLabel = "x264";
    vst = avformat_new_stream(fmt, nullptr);
    if (!vst) { status = "Cannot add video stream"; previewStatus = status; CloseAll(); return false; }
    avcodec_parameters_from_context(vst->codecpar, vctx);
    vst->time_base = vctx->time_base;

    const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (acodec) {
        AVCodecContext* tryA = avcodec_alloc_context3(acodec);
        tryA->sample_rate = 48000;
        tryA->bit_rate = 192000;
        tryA->time_base = AVRational{1, 48000};
        tryA->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&tryA->ch_layout, 2);
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
            tryA->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVChannelLayout inLay;
        av_channel_layout_default(&inLay, 2);
        SwrContext* trySwr = nullptr;
        bool audioOk = avcodec_open2(tryA, acodec, nullptr) >= 0;
        if (audioOk) {
            audioOk = swr_alloc_set_opts2(&trySwr, &tryA->ch_layout, tryA->sample_fmt, 48000,
                                          &inLay, AV_SAMPLE_FMT_S16, 48000, 0, nullptr) >= 0
                      && trySwr && swr_init(trySwr) >= 0;
        }
        av_channel_layout_uninit(&inLay);
        if (!audioOk) {
            if (trySwr) swr_free(&trySwr);
            avcodec_free_context(&tryA);
            videoOnly = true;
        } else {
            actx = tryA;
            swr = trySwr;
            aFrameSize = actx->frame_size > 0 ? actx->frame_size : 1024;
            ast = avformat_new_stream(fmt, nullptr);
            if (!ast) { status = "Cannot add audio stream"; previewStatus = status; CloseAll(); return false; }
            avcodec_parameters_from_context(ast->codecpar, actx);
            ast->time_base = actx->time_base;
            aframe = av_frame_alloc();
            aframe->format = actx->sample_fmt;
            aframe->nb_samples = aFrameSize;
            aframe->sample_rate = 48000;
            av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
            if (av_frame_get_buffer(aframe, 0) < 0) {
                status = "Audio buffer failed"; previewStatus = status; CloseAll(); return false;
            }
        }
    } else {
        videoOnly = true;
    }

    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        status = "Cannot open output file"; previewStatus = status; CloseAll(); return false;
    }
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "movflags", "+faststart", 0);
    if (avformat_write_header(fmt, &opt) < 0) {
        av_dict_free(&opt);
        status = "Cannot write mp4 header"; previewStatus = status; CloseAll(); return false;
    }
    av_dict_free(&opt);
    headerWritten = true;

    vframe = av_frame_alloc();
    vframe->format = vctx->pix_fmt;
    vframe->width = w;
    vframe->height = h;
    if (av_frame_get_buffer(vframe, 32) < 0) {
        status = "Out of memory"; previewStatus = status; CloseAll(); return false;
    }
    toYuv = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, vctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!toYuv) { status = "Scaler failed"; previewStatus = status; CloseAll(); return false; }
    canvas.assign((size_t)w * (size_t)h * 4, 0);
    pkt = av_packet_alloc();
    active = true;
    char buf[192];
    int showW = w;
    int showH = h;
    int showFps = rifeOn ? outFps : framesPerSec;
    const char* later = rifeOn ? " then RIFE" : "";
    snprintf(buf, sizeof(buf), "Exporting %dx%d %d fps %s%s%s", showW, showH, showFps, encLabel, videoOnly ? " (no audio)" : "", later);
    status = buf;
    previewStatus = status;
    return true;
}

bool ExportSession::BeginMp3(const std::string& outPath) {
    CloseAll();
    path = outPath;
    timelineEnd = ProjectEnd();
    if (timelineEnd < 0.05 || clips.empty()) {
        status = "Nothing to export";
        previewStatus = status;
        return false;
    }
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp3", path.c_str()) < 0 || !fmt) {
        status = "Cannot create mp3";
        previewStatus = status;
        CloseAll();
        return false;
    }
    const char* names[] = {"libmp3lame", "mp3_mf", "libshine", nullptr};
    const AVSampleFormat tries[] = {AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S16};
    bool saw = false;
    for (int n = 0; n < 4 && !actx; n++) {
        const AVCodec* c = names[n] ? avcodec_find_encoder_by_name(names[n]) : avcodec_find_encoder(AV_CODEC_ID_MP3);
        if (!c) continue;
        saw = true;
        for (int i = 0; i < 3 && !actx; i++) {
            AVCodecContext* tryA = avcodec_alloc_context3(c);
            if (!tryA) continue;
            tryA->sample_rate = 48000;
            tryA->bit_rate = 192000;
            tryA->sample_fmt = tries[i];
            tryA->time_base = AVRational{1, 48000};
            av_channel_layout_default(&tryA->ch_layout, 2);
            if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
                tryA->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(tryA, c, nullptr) >= 0) actx = tryA;
            else avcodec_free_context(&tryA);
        }
    }
    if (!actx) {
        status = saw ? "Cannot open MP3 encoder" : "MP3 encoder not found";
        previewStatus = status;
        CloseAll();
        return false;
    }
    AVChannelLayout inLay;
    av_channel_layout_default(&inLay, 2);
    int swrOk = swr_alloc_set_opts2(&swr, &actx->ch_layout, actx->sample_fmt, 48000,
                                    &inLay, AV_SAMPLE_FMT_S16, 48000, 0, nullptr);
    av_channel_layout_uninit(&inLay);
    if (swrOk < 0 || !swr || swr_init(swr) < 0) {
        status = "Cannot open MP3 encoder";
        previewStatus = status;
        CloseAll();
        return false;
    }
    aFrameSize = actx->frame_size > 0 ? actx->frame_size : 1152;
    ast = avformat_new_stream(fmt, nullptr);
    if (!ast) {
        status = "Cannot add audio stream";
        previewStatus = status;
        CloseAll();
        return false;
    }
    avcodec_parameters_from_context(ast->codecpar, actx);
    ast->time_base = actx->time_base;
    aframe = av_frame_alloc();
    if (!aframe) {
        status = "Audio buffer failed";
        previewStatus = status;
        CloseAll();
        return false;
    }
    aframe->format = actx->sample_fmt;
    aframe->nb_samples = aFrameSize;
    aframe->sample_rate = 48000;
    av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
    if (av_frame_get_buffer(aframe, 0) < 0) {
        status = "Audio buffer failed";
        previewStatus = status;
        CloseAll();
        return false;
    }
    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        status = "Cannot open output file";
        previewStatus = status;
        CloseAll();
        return false;
    }
    if (avformat_write_header(fmt, nullptr) < 0) {
        status = "Cannot write mp3 header";
        previewStatus = status;
        CloseAll();
        return false;
    }
    headerWritten = true;
    pkt = av_packet_alloc();
    audioOnly = true;
    rifeOn = false;
    phase = 0;
    fps = 0;
    w = 0;
    h = 0;
    audioWritten = 0;
    aPts = 0;
    audioTotal = (int64_t)std::ceil(timelineEnd * 48000.0);
    if (audioTotal < 1) audioTotal = 1;
    total = (int)((audioTotal + aFrameSize - 1) / aFrameSize);
    if (total < 1) total = 1;
    frameIndex = 0;
    active = true;
    SetPlaying(false);
    ResetAudioSync();
    status = "Exporting audio MP3";
    previewStatus = status;
    return true;
}

bool TransitionForClip(int clip, float t, int& a, int& b, float& progress, std::string& id);
float ClipLocalTime(int clipIdx, float timelineT);
int ClipBelow(int above, float t);

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float BusHash(int x, int y, int frame) {
    unsigned n = (unsigned)(x * 374761393 + y * 668265263 + frame * 1440662683);
    n = (n ^ (n >> 13)) * 1274126177u;
    return (float)(n & 0xFFFFFF) / (float)0xFFFFFF;
}

bool BusActive(const BusSegment& s) {
    return std::fabs(s.lightness) > 0.001f || std::fabs(s.contrast) > 0.001f || std::fabs(s.warmth) > 0.001f
        || std::fabs(s.tint) > 0.001f || std::fabs(s.saturation) > 0.001f || std::fabs(s.shadow) > 0.001f
        || s.grain > 0.001f || s.sharpen > 0.001f;
}

static float Lim11(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

static BusSegment ClipGradeOf(const Clip& c) {
    BusSegment s;
    s.lightness = c.gradeLight;
    s.contrast = c.gradeContrast;
    s.warmth = c.gradeWarm;
    s.tint = c.gradeTint;
    s.saturation = c.gradeSat;
    s.shadow = c.gradeShadow;
    s.grain = c.gradeGrain;
    s.sharpen = c.gradeSharp;
    return s;
}

static void AddGrade(BusSegment& d, const BusSegment& s) {
    d.lightness = Lim11(d.lightness + s.lightness);
    d.contrast = Lim11(d.contrast + s.contrast);
    d.warmth = Lim11(d.warmth + s.warmth);
    d.tint = Lim11(d.tint + s.tint);
    d.saturation = Lim11(d.saturation + s.saturation);
    d.shadow = Lim11(d.shadow + s.shadow);
    d.grain = Clamp01(d.grain + s.grain);
    d.sharpen = Clamp01(d.sharpen + s.sharpen);
}

static bool SliderTyped(const char* label, float* v, float mn, float mx, const char* fmt);

static void DrawGradeSliders(float& light, float& contrast, float& warm, float& tint, float& sat, float& shadow, float& grain, float& sharp) {
    SliderTyped("Lightness", &light, -1.0f, 1.0f, "%.2f");
    SliderTyped("Contrast", &contrast, -1.0f, 1.0f, "%.2f");
    SliderTyped("Warmth", &warm, -1.0f, 1.0f, "%.2f");
    SliderTyped("Tint", &tint, -1.0f, 1.0f, "%.2f");
    SliderTyped("Saturation", &sat, -1.0f, 1.0f, "%.2f");
    SliderTyped("Shadow", &shadow, -1.0f, 1.0f, "%.2f");
    SliderTyped("Film grain", &grain, 0.0f, 1.0f, "%.2f");
    SliderTyped("Sharpen", &sharp, 0.0f, 1.0f, "%.2f");
}

const BusSegment* BusAt(float t) {
    const BusSegment* hit = nullptr;
    for (const auto& s : bus) {
        if (t >= s.start && t < s.start + s.duration) hit = &s;
    }
    return hit;
}

static void GradeRGB(float& r, float& g, float& b, const BusSegment& s, float noise) {
    float l = 0.299f * r + 0.587f * g + 0.114f * b;
    float sh = 1.0f - Clamp01(l / 0.5f);
    float lift = s.shadow * 0.35f * sh;
    r += lift; g += lift; b += lift;
    float ct = 1.0f + s.contrast * 1.35f;
    r = (r - 0.5f) * ct + 0.5f;
    g = (g - 0.5f) * ct + 0.5f;
    b = (b - 0.5f) * ct + 0.5f;
    r += s.lightness * 0.45f;
    g += s.lightness * 0.45f;
    b += s.lightness * 0.45f;
    l = 0.299f * r + 0.587f * g + 0.114f * b;
    float sat = 1.0f + s.saturation;
    r = l + (r - l) * sat;
    g = l + (g - l) * sat;
    b = l + (b - l) * sat;
    r += s.warmth * 0.18f;
    g += s.warmth * 0.05f;
    b -= s.warmth * 0.18f;
    g += s.tint * 0.16f;
    r -= s.tint * 0.08f;
    b -= s.tint * 0.08f;
    if (s.grain > 0.001f) {
        l = Clamp01(0.299f * r + 0.587f * g + 0.114f * b);
        float mid = 1.0f - std::fabs(l - 0.45f) * 1.7f;
        if (mid < 0.15f) mid = 0.15f;
        if (mid > 1.0f) mid = 1.0f;
        float n = (noise - 0.5f) * s.grain * 0.28f * mid;
        r += n; g += n; b += n;
    }
    r = Clamp01(r); g = Clamp01(g); b = Clamp01(b);
}

void GradeCanvas(uint8_t* px, int w, int h, const BusSegment& s, int frame) {
    if (!px || w <= 0 || h <= 0 || !BusActive(s)) return;
    std::vector<uint8_t> src;
    const uint8_t* in = px;
    if (s.sharpen > 0.01f) {
        src.assign(px, px + (size_t)w * (size_t)h * 4);
        in = src.data();
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = in + ((size_t)y * (size_t)w + (size_t)x) * 4;
            float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            if (s.sharpen > 0.01f && w > 2 && h > 2) {
                int xm = x > 0 ? x - 1 : x;
                int xp = x + 1 < w ? x + 1 : x;
                int ym = y > 0 ? y - 1 : y;
                int yp = y + 1 < h ? y + 1 : y;
                auto at = [&](int xx, int yy, int c) {
                    return in[((size_t)yy * (size_t)w + (size_t)xx) * 4 + c] / 255.0f;
                };
                float br = (at(xm, y, 0) + at(xp, y, 0) + at(x, ym, 0) + at(x, yp, 0)) * 0.25f;
                float bg = (at(xm, y, 1) + at(xp, y, 1) + at(x, ym, 1) + at(x, yp, 1)) * 0.25f;
                float bb = (at(xm, y, 2) + at(xp, y, 2) + at(x, ym, 2) + at(x, yp, 2)) * 0.25f;
                float k = s.sharpen * 1.6f;
                r += (r - br) * k;
                g += (g - bg) * k;
                b += (b - bb) * k;
            }
            GradeRGB(r, g, b, s, BusHash(x, y, frame));
            uint8_t* o = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            o[0] = (uint8_t)std::lround(r * 255.0f);
            o[1] = (uint8_t)std::lround(g * 255.0f);
            o[2] = (uint8_t)std::lround(b * 255.0f);
        }
    }
}

static void ChromaKey(float r, float g, float b, float sim, float soft, float spill, float& orr, float& og, float& ob, float& a) {
    float rb = std::max(r, b);
    float dom = g - rb;
    float t0 = 0.04f + sim * 0.50f;
    float t1 = t0 + 0.03f + soft * 0.40f;
    float k = 0.0f;
    if (dom >= t1) k = 1.0f;
    else if (dom > t0) k = (dom - t0) / (t1 - t0);
    float peak = std::max(r, std::max(g, b));
    if (peak > 0.82f && dom < 0.18f) k = 0.0f;
    orr = r;
    og = g;
    ob = b;
    float excess = g - rb;
    if (excess > 0.0f) og = g - excess * spill;
    if (orr < 0) orr = 0; if (orr > 1) orr = 1;
    if (og < 0) og = 0; if (og > 1) og = 1;
    if (ob < 0) ob = 0; if (ob > 1) ob = 1;
    a = 1.0f - k;
}

static void BlendChromaPlaced(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh,
                              float sim, float soft, float spill, float ox, float oy, float scale, float opacity,
                              float aspect, float rotDeg) {
    if (!dst || !src || sw < 1 || sh < 1 || scale < 0.05f || dw < 1 || dh < 1) return;
    if (aspect < 0.05f) aspect = 0.05f;
    float cx = dw * 0.5f + ox * dw * 0.5f;
    float cy = dh * 0.5f + oy * dh * 0.5f;
    float hw = dw * 0.5f * scale * aspect;
    float hh = dh * 0.5f * scale;
    if (hw < 1.0f || hh < 1.0f) return;
    float rad = rotDeg * 0.01745329252f;
    float cs = std::cos(rad);
    float sn = std::sin(rad);
    float ext = std::sqrt(hw * hw + hh * hh) + 2.0f;
    int x0 = std::max(0, (int)std::floor(cx - ext));
    int y0 = std::max(0, (int)std::floor(cy - ext));
    int x1 = std::min(dw, (int)std::ceil(cx + ext));
    int y1 = std::min(dh, (int)std::ceil(cy + ext));
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (x + 0.5f) - cx;
            float dy = (y + 0.5f) - cy;
            float lx = dx * cs + dy * sn;
            float ly = -dx * sn + dy * cs;
            if (lx < -hw || lx > hw || ly < -hh || ly > hh) continue;
            int sx = (int)((lx / (hw * 2.0f) + 0.5f) * sw);
            int sy = (int)((ly / (hh * 2.0f) + 0.5f) * sh);
            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
            const uint8_t* s = src + ((size_t)sy * sw + sx) * 4;
            float r, g, b, a;
            ChromaKey(s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, sim, soft, spill, r, g, b, a);
            a *= opacity;
            if (a < 0.004f) continue;
            uint8_t* d = dst + ((size_t)y * dw + x) * 4;
            float ia = 1.0f - a;
            d[0] = (uint8_t)std::lround(r * a * 255.0f + d[0] * ia);
            d[1] = (uint8_t)std::lround(g * a * 255.0f + d[1] * ia);
            d[2] = (uint8_t)std::lround(b * a * 255.0f + d[2] * ia);
            d[3] = 255;
        }
    }
}

static int FindGsClip(float t) {
    int hit = -1;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (clips[i].track != kGsTrack) continue;
        if (t < clips[i].startTime || t >= clips[i].startTime + clips[i].duration) continue;
        hit = i;
    }
    return hit;
}

static int FindSub(float t) {
    int hit = -1;
    for (int i = 0; i < (int)subs.size(); i++) {
        if (t < subs[i].start || t >= subs[i].start + subs[i].duration) continue;
        hit = i;
    }
    return hit;
}

static unsigned DecodeUtf8(const char*& p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x80) { p++; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { unsigned u = ((c & 31) << 6) | (p[1] & 63); p += 2; return u; }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { unsigned u = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; return u; }
    p++;
    return c;
}

static bool HasHebrew(const std::string& s) {
    const char* p = s.c_str();
    while (*p) {
        unsigned cp = DecodeUtf8(p);
        if (cp >= 0x0590 && cp <= 0x05FF) return true;
    }
    return false;
}

static void AppendUtf8(std::string& o, unsigned c) {
    if (c < 0x80) o.push_back((char)c);
    else if (c < 0x800) { o.push_back((char)(0xC0 | (c >> 6))); o.push_back((char)(0x80 | (c & 63))); }
    else { o.push_back((char)(0xE0 | (c >> 12))); o.push_back((char)(0x80 | ((c >> 6) & 63))); o.push_back((char)(0x80 | (c & 63))); }
}

static std::string ReverseUtf8(const std::string& s) {
    std::vector<unsigned> cps;
    const char* p = s.c_str();
    while (*p) cps.push_back(DecodeUtf8(p));
    std::string o;
    for (int i = (int)cps.size() - 1; i >= 0; --i) AppendUtf8(o, cps[i]);
    return o;
}

static int subBufChars = 0;
static bool subArmCursor = false;

static int Utf8CharLen(const char* p) {
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return p[1] ? 2 : 1;
    if ((c & 0xF0) == 0xE0) return (p[1] && p[2]) ? 3 : 1;
    return 1;
}

static void LoadSubEditor(const std::string& logical) {
    std::string shown = HasHebrew(logical) ? ReverseUtf8(logical) : logical;
    snprintf(subEditBuf, sizeof(subEditBuf), "%s", shown.c_str());
    subBufChars = (int)strlen(subEditBuf);
    subArmCursor = HasHebrew(shown);
}

static int SubInputCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) return 0;
    std::string s(data->Buf, data->BufTextLen);
    bool heb = HasHebrew(s);
    if (heb && subArmCursor) {
        data->CursorPos = 0;
        data->SelectionStart = 0;
        data->SelectionEnd = 0;
        subArmCursor = false;
    }
    if (heb && data->BufTextLen > subBufChars) {
        data->CursorPos = 0;
        data->SelectionStart = 0;
        data->SelectionEnd = 0;
    }
    if (heb && ImGui::IsKeyPressed(ImGuiKey_Backspace) && data->CursorPos == 0 && data->BufTextLen > 0) {
        int n = Utf8CharLen(data->Buf);
        if (n > data->BufTextLen) n = data->BufTextLen;
        data->DeleteChars(0, n);
    }
    subBufChars = data->BufTextLen;
    return 0;
}

static bool RectHit(const ImVec4& r, const ImVec2& m) {
    return r.z > r.x && m.x >= r.x && m.y >= r.y && m.x <= r.z && m.y <= r.w;
}

static bool ClickOutsidePanels() {
    if (openedPanelThisFrame) return false;
    if (!ImGui::IsMouseClicked(0) && !ImGui::IsMouseClicked(1)) return false;
    ImVec2 m = ImGui::GetMousePos();
    if (busGradeOpen && RectHit(busPanelRect, m)) return false;
    if (gsPanelOpen && RectHit(gsPanelRect, m)) return false;
    if (xformOpen && RectHit(xformRect, m)) return false;
    if (xformOpen && m.x >= previewPanelMin.x && m.y >= previewPanelMin.y && m.x <= previewPanelMax.x && m.y <= previewPanelMax.y) return false;
    if (subEditorOpen && RectHit(subPanelRect, m)) return false;
    if (subStyleOpen && RectHit(subStyleRect, m)) return false;
    if (mediaPanelOpen && RectHit(mediaPanelRect, m)) return false;
    if (audioPanelOpen && RectHit(audioPanelRect, m)) return false;
    if (floatMediaOpen && RectHit(floatMediaRect, m)) return false;
    if (colorOpen && RectHit(colorRect, m)) return false;
    if (ovPanelOpen && RectHit(ovPanelRect, m)) return false;
    return true;
}

static bool SliderTyped(const char* label, float* v, float mn, float mx, const char* fmt) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(118.0f);
    ImGui::SetNextItemWidth(150.0f);
    bool changed = ImGui::SliderFloat("##s", v, mn, mx, fmt);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    changed |= ImGui::InputFloat("##n", v, 0.0f, 0.0f, fmt);
    if (*v < mn) *v = mn;
    if (*v > mx) *v = mx;
    ImGui::PopID();
    return changed;
}

static bool FxSlider(OverlayFx& o, const char* label, float* v, int channel, float mn, float mx, const char* fmt) {
    float before = *v;
    bool changed = SliderTyped(label, v, mn, mx, fmt);
    if (changed && o.rec && isPlaying) OverlayWrite(o, channel, before, *v, currentTime);
    return changed;
}

static void DrawPictureControls(Clip& c) {
    SliderTyped("Position X", &c.gsX, -1.0f, 1.0f, "%.2f");
    SliderTyped("Position Y", &c.gsY, -1.0f, 1.0f, "%.2f");
    if (SliderTyped("Size", &c.gsScale, 0.05f, 4.0f, "%.2f")) c.gsFit = 2;
    if (SliderTyped("Aspect", &c.gsAspect, 0.25f, 4.0f, "%.2f")) c.gsFit = 2;
    SliderTyped("Rotate", &c.gsRot, -180.0f, 180.0f, "%.1f");
}

static void DrawBlurGraph(float* k) {
    ImGui::TextUnformatted("Change");
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size(240.0f, 78.0f);
    ImGui::InvisibleButton("##blurgraph", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = origin;
    ImVec2 b(origin.x + size.x, origin.y + size.y);
    dl->AddRectFilled(a, b, IM_COL32(14, 16, 22, 255), 3.0f);
    dl->AddRect(a, b, IM_COL32(70, 78, 96, 255), 3.0f);
    ImVec2 pts[4];
    for (int i = 0; i < 4; i++) {
        float u = (float)i / 3.0f;
        float v = k[i] < 0 ? 0 : (k[i] > 1 ? 1 : k[i]);
        pts[i] = ImVec2(a.x + 8.0f + u * (size.x - 16.0f), b.y - 8.0f - v * (size.y - 16.0f));
    }
    dl->AddPolyline(pts, 4, IM_COL32(120, 186, 255, 255), 0, 2.0f);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    static int drag = -1;
    if (ImGui::IsItemActivated()) {
        drag = 0;
        float best = 1.0e9f;
        for (int i = 0; i < 4; i++) {
            float dx = mouse.x - pts[i].x;
            float dy = mouse.y - pts[i].y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; drag = i; }
        }
    }
    if (drag >= 0 && ImGui::IsItemActive()) {
        float v = (b.y - 8.0f - mouse.y) / (size.y - 16.0f);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        k[drag] = v;
    }
    if (!ImGui::IsItemActive()) drag = -1;
    for (int i = 0; i < 4; i++) dl->AddCircleFilled(pts[i], 4.5f, IM_COL32(230, 236, 245, 255));
    const char* labs[4] = {"Start", "Early", "Late", "End"};
    for (int i = 0; i < 4; i++) SliderTyped(labs[i], &k[i], 0.0f, 1.0f, "%.2f");
}

static void PlaceAtCursor() {
    ImVec2 m = ImGui::GetMousePos();
    ImGuiIO& pio = ImGui::GetIO();
    if (m.x > pio.DisplaySize.x - 360.0f) m.x = pio.DisplaySize.x - 360.0f;
    if (m.y > pio.DisplaySize.y - 280.0f) m.y = pio.DisplaySize.y - 280.0f;
    if (m.x < 8.0f) m.x = 8.0f;
    if (m.y < 8.0f) m.y = 8.0f;
    ImGui::SetNextWindowPos(m, ImGuiCond_Appearing);
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowBgAlpha(0.98f);
}

static void PlacePanel(float xShift) {
    float x = previewPanelMax.x - 16.0f - xShift;
    float y = previewPanelMin.y + 36.0f;
    if (x < 280.0f) x = 280.0f;
    if (y < 36.0f) y = 36.0f;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.98f);
}

static bool TransportButton(const char* id, int kind) {
    ImGui::PushID(id);
    bool pressed = ImGui::Button("##t", ImVec2(40.0f, 36.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    float cx = (a.x + b.x) * 0.5f;
    float cy = (a.y + b.y) * 0.5f;
    ImU32 col = IM_COL32(245, 245, 245, 255);
    if (kind == 0) {
        dl->AddTriangleFilled(ImVec2(cx - 6.0f, cy - 8.0f), ImVec2(cx - 6.0f, cy + 8.0f), ImVec2(cx + 9.0f, cy), col);
    } else if (kind == 1) {
        dl->AddRectFilled(ImVec2(cx - 7.0f, cy - 8.0f), ImVec2(cx - 2.5f, cy + 8.0f), col);
        dl->AddRectFilled(ImVec2(cx + 2.5f, cy - 8.0f), ImVec2(cx + 7.0f, cy + 8.0f), col);
    } else if (kind == 2) {
        dl->AddRectFilled(ImVec2(cx - 7.0f, cy - 7.0f), ImVec2(cx + 7.0f, cy + 7.0f), col);
    } else if (kind == 3) {
        dl->AddRectFilled(ImVec2(cx - 12.0f, cy - 8.0f), ImVec2(cx - 8.2f, cy + 8.0f), col);
        dl->AddTriangleFilled(ImVec2(cx - 7.2f, cy), ImVec2(cx + 0.4f, cy - 7.4f), ImVec2(cx + 0.4f, cy + 7.4f), col);
        dl->AddTriangleFilled(ImVec2(cx + 1.6f, cy), ImVec2(cx + 11.0f, cy - 7.4f), ImVec2(cx + 11.0f, cy + 7.4f), col);
    } else {
        ImVec2 c(cx + 0.4f, cy + 1.2f);
        const float rad = 7.4f;
        dl->PathArcTo(c, rad, 0.85f, 5.45f, 24);
        dl->PathStroke(col, 0, 2.7f);
        float tipx = c.x + std::cos(0.85f) * rad;
        float tipy = c.y + std::sin(0.85f) * rad;
        dl->AddTriangleFilled(ImVec2(tipx - 7.2f, tipy + 0.4f), ImVec2(tipx + 1.4f, tipy - 5.2f), ImVec2(tipx + 2.2f, tipy + 4.6f), col);
    }
    ImGui::PopID();
    return pressed;
}

static int WordsPerCue() {
    if (exAspect == 1) return 6;
    if (exAspect == 2) return 8;
    if (exAspect == 4) return 7;
    if (exAspect == 3) return 10;
    return 14;
}

static void SplitLongSubs() {
    int budget = WordsPerCue();
    if (budget < 1) budget = 1;
    std::vector<Subtitle> out;
    out.reserve(subs.size());
    for (const Subtitle& s : subs) {
        std::vector<std::string> words;
        std::string word;
        for (char ch : s.text) {
            if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
                if (!word.empty()) { words.push_back(word); word.clear(); }
            } else word.push_back(ch);
        }
        if (!word.empty()) words.push_back(word);
        if ((int)words.size() <= budget) { out.push_back(s); continue; }
        float t = s.start;
        float total = std::max(0.25f, s.duration);
        int used = 0;
        while (used < (int)words.size()) {
            int n = std::min(budget, (int)words.size() - used);
            std::string text;
            for (int i = 0; i < n; i++) {
                if (!text.empty()) text += " ";
                text += words[used + i];
            }
            float d = total * (float)n / (float)words.size();
            if (used + n >= (int)words.size()) d = std::max(0.2f, s.start + s.duration - t);
            Subtitle bit = s;
            bit.start = t;
            bit.duration = std::max(0.2f, d);
            bit.text = text;
            out.push_back(bit);
            t += bit.duration;
            used += n;
        }
    }
    subs.swap(out);
}

static float AspectValue(int aspect) {
    if (aspect == 1) return 9.0f / 16.0f;
    if (aspect == 2) return 1.0f;
    if (aspect == 3) return 4.0f / 3.0f;
    if (aspect == 4) return 3.0f / 4.0f;
    return 16.0f / 9.0f;
}

static bool IsImportedStill(const Clip& c) {
    if (c.mediaId < 0 || c.mediaId >= (int)media.size()) return false;
    const MediaItem& m = media[c.mediaId];
    return m.type == "image" && !IsSolidPath(m.path);
}

static float StillAspect(const Clip& c) {
    const MediaItem& m = media[c.mediaId];
    if (m.aspect > 0.05f) return m.aspect;
    if (m.thumbW > 0 && m.thumbH > 0) return (float)m.thumbW / (float)m.thumbH;
    return 0.0f;
}

static void GsContainPair(float imageAspect, float& aspect, float& scale) {
    float frame = AspectValue(exAspect);
    if (!(frame > 0.05f)) frame = 16.0f / 9.0f;
    float rel = imageAspect / frame;
    if (rel < 0.25f) rel = 0.25f;
    if (rel > 4.0f) rel = 4.0f;
    aspect = rel;
    scale = (rel > 1.0f) ? (1.0f / rel) : 1.0f;
}

static void ContainGsImage(Clip& c) {
    if (!IsImportedStill(c)) return;
    float image = StillAspect(c);
    if (!(image > 0.05f)) return;
    GsContainPair(image, c.gsAspect, c.gsScale);
    c.gsFit = 1;
}

static bool GsScaleIdentity(const Clip& c) {
    return std::fabs(c.gsScale - 1.0f) < 0.02f && std::fabs(c.gsAspect - 1.0f) < 0.02f;
}

static bool GsStillAutoContain(const Clip& c) {
    if (!IsImportedStill(c)) return false;
    float image = StillAspect(c);
    if (!(image > 0.05f)) return false;
    float aspect = 1.0f, scale = 1.0f;
    GsContainPair(image, aspect, scale);
    return std::fabs(c.gsAspect - aspect) < 0.02f && std::fabs(c.gsScale - scale) < 0.02f;
}

static void ReleaseGsImageContain(Clip& c) {
    if (!GsStillAutoContain(c)) return;
    c.gsScale = 1.0f;
    c.gsAspect = 1.0f;
    c.gsFit = 0;
}

static void RefitAutoGsImages() {
    for (Clip& c : clips)
        if (c.track == kGsTrack && c.gsFit == 1) ContainGsImage(c);
}

static void BakeLoadedGsImages() {
    for (Clip& c : clips) {
        if (c.track != kGsTrack || !IsImportedStill(c) || c.gsFit == 2) continue;
        if (c.gsFit == 1 || GsScaleIdentity(c)) ContainGsImage(c);
    }
}

static int PictureSlot(int track) {
    if (track == kGsTrack) return 0;
    if (track >= 0 && track < 3) return track + 1;
    return -1;
}

static int SlotToPicture(int slot) {
    if (slot == 0) return kGsTrack;
    if (slot >= 1 && slot <= 3) return slot - 1;
    return -1;
}

static int AudioSlot(int track) {
    if (track >= 3 && track <= 5) return track - 3;
    return -1;
}

static int SlotToAudio(int slot) {
    if (slot >= 0 && slot <= 2) return slot + 3;
    return -1;
}

static void MoveClipTrack(int idx, int dest) {
    if (idx < 0 || idx >= (int)clips.size() || dest < 0) return;
    Clip& c = clips[idx];
    if (c.track == dest) return;
    int prev = c.track;
    c.track = dest;
    if (dest == kGsTrack && prev != kGsTrack) {
        if (c.gsFit != 2 && GsScaleIdentity(c)) ContainGsImage(c);
    } else if (prev == kGsTrack && dest != kGsTrack) {
        ReleaseGsImageContain(c);
    }
}

static int ShiftedTrack(int track, int delta) {
    int pic = PictureSlot(track);
    if (pic >= 0) return SlotToPicture(pic + delta);
    int aud = AudioSlot(track);
    if (aud >= 0) return SlotToAudio(aud + delta);
    return -1;
}

static void NudgeSelectionChannel(int dir) {
    if (dir == 0) return;
    struct Move { int idx; int dest; };
    std::vector<Move> moves;
    for (int i : selection) {
        if (i < 0 || i >= (int)clips.size()) continue;
        int dest = ShiftedTrack(clips[i].track, dir);
        if (dest >= 0 && dest != clips[i].track) moves.push_back({i, dest});
    }
    for (const auto& m : moves) MoveClipTrack(m.idx, m.dest);
}

static void FollowLeaderChannel(int leader, int prevTrack) {
    if (leader < 0 || leader >= (int)clips.size()) return;
    int now = clips[leader].track;
    if (now == prevTrack) return;
    int delta = 0;
    int ps = PictureSlot(prevTrack), ns = PictureSlot(now);
    if (ps >= 0 && ns >= 0) delta = ns - ps;
    else if (AudioSlot(prevTrack) >= 0 && AudioSlot(now) >= 0) delta = now - prevTrack;
    else return;
    if (delta == 0) return;
    struct Move { int idx; int dest; };
    std::vector<Move> moves;
    for (int i : selection) {
        if (i == leader || i < 0 || i >= (int)clips.size()) continue;
        int dest = ShiftedTrack(clips[i].track, delta);
        if (dest >= 0 && dest != clips[i].track) moves.push_back({i, dest});
    }
    for (const auto& m : moves) MoveClipTrack(m.idx, m.dest);
}

static bool ClipCoversFrame(const Clip& c, float picAspect, float effectScale) {
    if (std::fabs(c.gsX) > 0.002f || std::fabs(c.gsY) > 0.002f || std::fabs(c.gsRot) > 0.4f) return false;
    float frameAspect = AspectValue(exAspect);
    if (!(picAspect > 0.05f)) picAspect = frameAspect;
    float lw = 1.0f, lh = 1.0f;
    if (picAspect > frameAspect) lh = frameAspect / picAspect;
    else lw = picAspect / frameAspect;
    float sc = c.gsScale * (effectScale < 0.05f ? 1.0f : effectScale);
    float asp = c.gsAspect < 0.05f ? 1.0f : c.gsAspect;
    lw *= sc * asp;
    lh *= sc;
    return lw >= 0.995f && lh >= 0.995f;
}

static float ClipPixelAspect(int clip) {
    if (clip < 0 || clip >= (int)clips.size()) return 16.0f / 9.0f;
    if (playingClip == clip && player && player->width > 0 && player->height > 0)
        return (float)player->width / (float)player->height;
    if (playingClipB == clip && playerB && playerB->width > 0 && playerB->height > 0)
        return (float)playerB->width / (float)playerB->height;
    if (playingClipUnder == clip && playerUnder && playerUnder->width > 0 && playerUnder->height > 0)
        return (float)playerUnder->width / (float)playerUnder->height;
    int mid = clips[clip].mediaId;
    if (mid >= 0 && mid < (int)media.size() && media[mid].aspect > 0.05f) return media[mid].aspect;
    return 16.0f / 9.0f;
}

static bool ClipLeavesGap(int clip, float effectScale) {
    if (clip < 0 || clip >= (int)clips.size()) return true;
    return !ClipCoversFrame(clips[clip], ClipPixelAspect(clip), effectScale);
}

static bool FrontLeavesGaps(int top, int partner, int plate, int drive, float t, int* bg, int& nb) {
    nb = 0;
    if (top < 0 || top >= (int)clips.size()) return false;
    for (int tr = 2; tr >= 0; --tr) {
        int c = -1;
        for (int i = 0; i < (int)clips.size(); i++) {
            if (clips[i].track != tr) continue;
            if (t < clips[i].startTime || t >= clips[i].startTime + clips[i].duration) continue;
            c = i;
        }
        if (c < 0 || c == top || c == partner || c == plate || c == drive) continue;
        if (nb < 2) bg[nb++] = c;
    }
    if (nb == 0) return false;
    float topLocal = t - clips[top].startTime;
    if (OpacityAt(clips[top], topLocal) < 0.98f) return true;
    if (partner >= 0) {
        int a = -1, b = -1;
        float prog = 0;
        std::string id;
        if (TransitionForClip(top, t, a, b, prog, id)) {
            FxLook look;
            FxEvaluate(id.c_str(), prog, look);
            return ClipLeavesGap(a, look.scaleA) || ClipLeavesGap(b, look.scaleB)
                || ClipLeavesGap(a, 1.0f) || ClipLeavesGap(b, 1.0f);
        }
    }
    return ClipLeavesGap(top, 1.0f);
}

static void FitAspectBox(ImVec2 p0, ImVec2 p1, float aspect, ImVec2& a, ImVec2& b) {
    float areaW = std::max(1.0f, p1.x - p0.x);
    float areaH = std::max(1.0f, p1.y - p0.y);
    float areaAspect = areaW / areaH;
    a = p0;
    b = p1;
    if (aspect > areaAspect) {
        float h = areaW / aspect;
        float off = (areaH - h) * 0.5f;
        a.y += off;
        b.y -= off;
    } else {
        float w = areaH * aspect;
        float off = (areaW - w) * 0.5f;
        a.x += off;
        b.x -= off;
    }
}

static void DrawThumb(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 c0, ImVec2 c1, GLuint tex, int tw, int th) {
    if (!dl || !tex || tw < 1 || th < 1) return;
    float aw = p1.x - p0.x;
    float ah = p1.y - p0.y;
    if (aw < 2.0f || ah < 2.0f) return;
    float aspect = (float)tw / (float)th;
    ImVec2 a = p0, b = p1;
    if (aspect > aw / ah) {
        float h = aw / aspect;
        float off = (ah - h) * 0.5f;
        a.y += off;
        b.y -= off;
    } else {
        float w = ah * aspect;
        float off = (aw - w) * 0.5f;
        a.x += off;
        b.x -= off;
    }
    dl->PushClipRect(c0, c1, true);
    dl->AddImage((ImTextureID)(intptr_t)tex, a, b);
    dl->PopClipRect();
}

static ImU32 FxLaneColor(int kind) {
    switch (kind) {
    case 0: return IM_COL32(176, 122, 48, 235);
    case 1: return IM_COL32(70, 110, 180, 235);
    case 2: return IM_COL32(110, 70, 160, 235);
    case 3:
    case 4: return IM_COL32(46, 130, 96, 235);
    case 5:
    case 6:
    case 7: return IM_COL32(70, 90, 120, 235);
    case 8: return IM_COL32(140, 70, 110, 235);
    case 9: return IM_COL32(180, 60, 150, 235);
    case 10: return IM_COL32(40, 140, 160, 235);
    case 11: return IM_COL32(70, 100, 150, 235);
    case 12: return IM_COL32(90, 90, 96, 235);
    case 13: return IM_COL32(150, 110, 60, 235);
    case 14: return IM_COL32(50, 40, 70, 235);
    case 15: return IM_COL32(40, 130, 120, 235);
    case 16: return IM_COL32(110, 110, 110, 235);
    case 17: return IM_COL32(60, 150, 90, 235);
    case 18: return IM_COL32(190, 100, 40, 235);
    case 19: return IM_COL32(150, 150, 150, 235);
    case 20: return IM_COL32(140, 90, 50, 235);
    case 21: return IM_COL32(36, 36, 42, 235);
    case 22: return IM_COL32(170, 160, 140, 235);
    case 23: return IM_COL32(176, 110, 70, 235);
    case 24: return IM_COL32(150, 110, 60, 235);
    case 25: return IM_COL32(40, 32, 28, 235);
    case 26: return IM_COL32(64, 112, 168, 235);
    case 27: return IM_COL32(72, 146, 126, 235);
    case 28: return IM_COL32(214, 220, 232, 235);
    default: return IM_COL32(140, 70, 110, 235);
    }
}

static void DrawFxIcon(ImDrawList* dl, ImVec2 a, ImVec2 b, int kind) {
    dl->AddRectFilled(a, b, IM_COL32(16, 16, 20, 255), 4.0f);
    dl->AddRect(a, b, IM_COL32(70, 70, 80, 255), 4.0f);
    float t = (float)ImGui::GetTime();
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float w = (b.x - a.x) * 0.62f;
    float h = (b.y - a.y) * 0.56f;
    auto quad = [&](ImVec2 c0, float rw, float rh, float rot, ImU32 col) {
        float hw = rw * 0.5f, hh = rh * 0.5f;
        float cs = std::cos(rot), sn = std::sin(rot);
        ImVec2 q[4];
        float xs[4] = {-hw, hw, hw, -hw};
        float ys[4] = {-hh, -hh, hh, hh};
        for (int i = 0; i < 4; i++) {
            q[i].x = c0.x + xs[i] * cs - ys[i] * sn;
            q[i].y = c0.y + xs[i] * sn + ys[i] * cs;
        }
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], col);
        dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(235, 235, 235, 200), 1.4f);
    };
    if (kind == 0) {
        float bucket = std::floor(t * 8.0f);
        float jx = (std::sin(bucket * 1.7f) * 0.5f) * (b.x - a.x) * 0.12f;
        float jy = (std::cos(bucket * 2.1f) * 0.5f) * (b.y - a.y) * 0.10f;
        quad(ImVec2(c.x + jx, c.y + jy), w, h, std::sin(bucket) * 0.08f, IM_COL32(196, 146, 64, 255));
    } else if (kind == 1) {
        quad(c, w * 0.72f, h * 0.72f, t * 1.3f, IM_COL32(86, 132, 210, 255));
    } else if (kind == 2) {
        float cw = w / 3.0f, ch = h / 3.0f;
        ImVec2 o(c.x - w * 0.5f, c.y - h * 0.5f);
        for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) {
            ImVec2 p(o.x + x * cw + 1.5f, o.y + y * ch + 1.5f);
            dl->AddRectFilled(p, ImVec2(p.x + cw - 3.0f, p.y + ch - 3.0f), IM_COL32(120, 86, 176, 255), 1.0f);
        }
    } else if (kind == 3 || kind == 4) {
        float k = 0.5f + 0.5f * std::sin(t * 2.0f);
        float z = kind == 3 ? (0.42f + 0.58f * k) : (1.0f - 0.55f * k);
        quad(c, w * z, h * z, 0, IM_COL32(64, 156, 114, 255));
    } else if (kind >= 5 && kind <= 7) {
        float k = 0.5f + 0.5f * std::sin(t * 1.6f);
        float blur = kind == 6 ? k : (kind == 7 ? 1.0f - k : 0.75f);
        for (int i = 4; i >= 0; i--) {
            float grow = i * 3.2f * blur;
            int alpha = 28 + i * 10;
            dl->AddRectFilled(ImVec2(c.x - w * 0.42f - grow, c.y - h * 0.42f - grow),
                              ImVec2(c.x + w * 0.42f + grow, c.y + h * 0.42f + grow),
                              IM_COL32(150, 170, 190, alpha), 3.0f);
        }
        dl->AddRectFilled(ImVec2(c.x - w * 0.36f, c.y - h * 0.36f), ImVec2(c.x + w * 0.36f, c.y + h * 0.36f), IM_COL32(96, 118, 146, 230), 2.0f);
    } else if (kind == 8) {
        ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.5f);
        ImVec2 p1(c.x + w * 0.5f, c.y + h * 0.5f);
        dl->AddRectFilled(p0, p1, IM_COL32(42, 24, 48, 255), 2.0f);
        for (int i = 0; i < 7; i++) {
            float y = p0.y + (i + 0.5f) * ((p1.y - p0.y) / 7.0f);
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 255, 255, 36));
        }
        float n = std::fmod(std::fabs(std::sin(std::floor(t * 6.0f) * 12.9f)) * 17.0f, 1.0f);
        dl->AddCircleFilled(ImVec2(p0.x + 6.0f + n * (w - 12.0f), c.y - h * 0.12f), 2.2f, IM_COL32(245, 245, 240, 230));
        float bar = p0.y + std::fmod(t * 0.35f, 1.0f) * (p1.y - p0.y);
        dl->AddRectFilled(ImVec2(p0.x, bar), ImVec2(p1.x, bar + 2.0f), IM_COL32(255, 230, 210, 120));
    } else if (kind == 9) {
        ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.5f);
        for (int i = 0; i < 5; i++) {
            float y = p0.y + i * (h / 5.0f);
            float shift = std::sin(std::floor(t * 8.0f) + i * 1.7f) * w * 0.12f;
            dl->AddRectFilled(ImVec2(p0.x + shift, y + 1), ImVec2(p0.x + w + shift, y + h / 5.0f - 1), IM_COL32(200, 70, 160, 230));
        }
    } else if (kind == 10) {
        ImVec2 o(c.x - w * 0.5f, c.y - h * 0.5f);
        int n = 5;
        float cw = w / n, ch = h / n;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            int v = 70 + ((x * 3 + y * 5) % 4) * 40;
            dl->AddRectFilled(ImVec2(o.x + x * cw, o.y + y * ch), ImVec2(o.x + (x + 1) * cw - 1.5f, o.y + (y + 1) * ch - 1.5f), IM_COL32(v, v + 20, v + 30, 255));
        }
    } else if (kind == 11) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x, c.y + h * 0.45f), IM_COL32(90, 130, 190, 255));
        dl->AddRectFilled(ImVec2(c.x, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(70, 100, 160, 255));
        dl->AddLine(ImVec2(c.x, c.y - h * 0.45f), ImVec2(c.x, c.y + h * 0.45f), IM_COL32(255, 255, 255, 180), 1.5f);
    } else if (kind == 12) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x, c.y + h * 0.45f), IM_COL32(230, 230, 230, 255));
        dl->AddRectFilled(ImVec2(c.x, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(24, 24, 28, 255));
    } else if (kind == 13) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(168, 122, 64, 255), 2.0f);
    } else if (kind == 14) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(180, 180, 190, 255), 2.0f);
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x - w * 0.18f, c.y + h * 0.45f), IM_COL32(10, 8, 16, 180));
        dl->AddRectFilled(ImVec2(c.x + w * 0.18f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(10, 8, 16, 180));
    } else if (kind == 15) {
        ImVec2 prev(c.x - w * 0.5f, c.y);
        for (int i = 1; i <= 16; i++) {
            float u = i / 16.0f;
            ImVec2 p(c.x - w * 0.5f + u * w, c.y + std::sin(u * 8.0f + t * 3.0f) * h * 0.28f);
            dl->AddLine(prev, p, IM_COL32(80, 200, 180, 255), 2.0f);
            prev = p;
        }
    } else if (kind == 16) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(150, 150, 150, 255), 2.0f);
        dl->AddCircleFilled(ImVec2(c.x - w * 0.12f, c.y), h * 0.16f, IM_COL32(40, 40, 40, 255));
    } else if (kind == 17) {
        dl->AddCircleFilled(c, h * 0.42f, IM_COL32(70, 170, 110, 255), 24);
        dl->AddCircle(c, h * 0.28f, IM_COL32(230, 255, 230, 180), 16, 1.5f);
    } else if (kind == 18) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(40, 24, 18, 255), 2.0f);
        float u = std::fmod(t * 0.25f, 1.0f);
        dl->AddCircleFilled(ImVec2(c.x - w * 0.4f + u * w * 0.8f, c.y), h * 0.28f, IM_COL32(255, 140, 50, 180), 16);
    } else if (kind == 19) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), IM_COL32(28, 28, 32, 255), 2.0f);
        for (int i = 0; i < 8; i++) {
            float u = std::fmod(t * 0.15f + i * 0.17f, 1.0f);
            float v = std::fmod(i * 0.37f + t * 0.05f, 1.0f);
            dl->AddCircleFilled(ImVec2(c.x - w * 0.45f + u * w * 0.9f, c.y - h * 0.4f + v * h * 0.8f), 1.4f + (i % 3), IM_COL32(230, 226, 210, 200));
        }
    } else if (kind == 20) {
        ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.28f);
        ImVec2 p1(c.x + w * 0.5f, c.y + h * 0.28f);
        dl->AddRectFilled(p0, p1, IM_COL32(92, 58, 32, 255), 3.0f);
        dl->AddCircleFilled(ImVec2(c.x - w * 0.24f, c.y), h * 0.22f, IM_COL32(210, 170, 90, 255), 16);
        dl->AddCircleFilled(ImVec2(c.x + w * 0.24f, c.y), h * 0.22f, IM_COL32(210, 170, 90, 255), 16);
        dl->AddCircleFilled(ImVec2(c.x - w * 0.24f, c.y), h * 0.07f, IM_COL32(40, 28, 18, 255), 10);
        dl->AddCircleFilled(ImVec2(c.x + w * 0.24f, c.y), h * 0.07f, IM_COL32(40, 28, 18, 255), 10);
    } else if (kind == 21) {
        ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.45f);
        ImVec2 p1(c.x + w * 0.5f, c.y + h * 0.45f);
        dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 20, 255), 2.0f);
        dl->AddRectFilled(ImVec2(p0.x + w * 0.16f, p0.y + 3), ImVec2(p1.x - w * 0.16f, p1.y - 3), IM_COL32(70, 78, 90, 255), 1.0f);
        for (int i = 0; i < 4; i++) {
            float y = p0.y + 4.0f + i * ((p1.y - p0.y - 8.0f) / 4.0f);
            dl->AddRectFilled(ImVec2(p0.x + 3, y), ImVec2(p0.x + w * 0.10f, y + 4.0f), IM_COL32(210, 206, 190, 230));
            dl->AddRectFilled(ImVec2(p1.x - w * 0.10f, y), ImVec2(p1.x - 3, y + 4.0f), IM_COL32(210, 206, 190, 230));
        }
    } else if (kind == 22 || kind == 23 || kind == 24) {
        ImU32 base = kind == 23 ? IM_COL32(150, 90, 55, 255) : (kind == 24 ? IM_COL32(130, 96, 52, 255) : IM_COL32(50, 48, 46, 255));
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.45f), ImVec2(c.x + w * 0.5f, c.y + h * 0.45f), base, 2.0f);
        float s = std::fmod(t * 0.4f, 1.0f);
        dl->AddLine(ImVec2(c.x - w * 0.35f + s * w * 0.15f, c.y - h * 0.45f), ImVec2(c.x - w * 0.32f + s * w * 0.15f, c.y + h * 0.45f), IM_COL32(240, 236, 220, 180), 1.2f);
        for (int i = 0; i < 6; i++) {
            float u = std::fmod(i * 0.19f + t * 0.08f, 1.0f);
            float v = std::fmod(i * 0.41f, 1.0f);
            dl->AddCircleFilled(ImVec2(c.x - w * 0.42f + u * w * 0.84f, c.y - h * 0.38f + v * h * 0.76f), 1.3f, IM_COL32(245, 240, 230, 210));
        }
    } else if (kind == 25) {
        dl->AddCircleFilled(c, h * 0.42f, IM_COL32(28, 22, 18, 255), 24);
        dl->AddCircle(c, h * 0.30f, IM_COL32(180, 140, 70, 200), 20, 1.2f);
        dl->AddCircle(c, h * 0.18f, IM_COL32(180, 140, 70, 160), 16, 1.0f);
        dl->AddCircleFilled(c, h * 0.07f, IM_COL32(210, 180, 110, 255), 12);
    } else if (kind == 26) {
        ImVec2 p0(c.x - w * 0.46f, c.y);
        ImVec2 p1(c.x + w * 0.46f, c.y);
        for (int i = -2; i <= 2; i++) {
            int a = 40 + (2 - std::abs(i)) * 50;
            dl->AddLine(ImVec2(p0.x, p0.y + i * 3.2f), ImVec2(p1.x, p1.y + i * 3.2f), IM_COL32(170, 200, 230, a), 2.0f);
        }
    } else if (kind == 27) {
        ImVec2 p0(c.x - w * 0.46f, c.y - h * 0.42f);
        ImVec2 p1(c.x + w * 0.46f, c.y + h * 0.42f);
        for (int i = 0; i < 6; i++) {
            float u = (float)i / 5.0f;
            int a = 40 + (int)(u * 200.0f);
            float x0 = p0.x + u * (p1.x - p0.x);
            float x1 = x0 + (p1.x - p0.x) / 6.0f - 1.0f;
            dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y), IM_COL32(90, 170, 150, a));
        }
    } else if (kind == 28) {
        dl->AddRectFilled(ImVec2(c.x - w * 0.46f, c.y - h * 0.40f), ImVec2(c.x + w * 0.46f, c.y + h * 0.40f), IM_COL32(36, 40, 48, 255), 2.0f);
        dl->AddLine(ImVec2(c.x - w * 0.34f, c.y), ImVec2(c.x + w * 0.34f, c.y), IM_COL32(236, 240, 246, 255), 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - h * 0.30f), ImVec2(c.x, c.y + h * 0.30f), IM_COL32(236, 240, 246, 255), 1.6f);
        dl->AddCircle(c, h * 0.16f, IM_COL32(236, 240, 246, 255), 16, 1.4f);
    } else {
        quad(c, w * 0.7f, h * 0.7f, 0, IM_COL32(120, 120, 120, 255));
    }
}

static void EnsureFxPlate() {
#ifdef _WIN32
    static int done = 0;
    if (done) return;
    done = 1;
    int nbytes = 0;
    const unsigned char* bytes = FxPlatePng(&nbytes);
    if (!bytes || nbytes < 32) return;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ownCo = (co == S_OK);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)nbytes);
    if (!mem) {
        if (ownCo) CoUninitialize();
        return;
    }
    void* locked = GlobalLock(mem);
    if (locked) memcpy(locked, bytes, (size_t)nbytes);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
        GlobalFree(mem);
        if (ownCo) CoUninitialize();
        return;
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) {
        bool bgra = false;
        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            bgra = true;
            hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        UINT pw = 0, ph = 0;
        if (SUCCEEDED(hr)) hr = conv->GetSize(&pw, &ph);
        if (SUCCEEDED(hr) && pw >= 2 && ph >= 2 && pw <= 4096 && ph <= 4096) {
            std::vector<unsigned char> px((size_t)pw * ph * 4);
            hr = conv->CopyPixels(nullptr, pw * 4, (UINT)px.size(), px.data());
            if (SUCCEEDED(hr)) {
                if (bgra) {
                    for (size_t i = 0; i + 3 < px.size(); i += 4) {
                        unsigned char tmp = px[i];
                        px[i] = px[i + 2];
                        px[i + 2] = tmp;
                    }
                }
                OverlaySetPreviewPlate(px.data(), (int)pw, (int)ph);
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    stream->Release();
    if (ownCo) CoUninitialize();
#else
    (void)0;
#endif
}

static void RenderFloatingOnTop() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    ImDrawData* src = ImGui::GetDrawData();
    if (!ctx || !src) return;
    ImDrawData data;
    data.Valid = true;
    data.DisplayPos = src->DisplayPos;
    data.DisplaySize = src->DisplaySize;
    data.FramebufferScale = src->FramebufferScale;
    data.OwnerViewport = src->OwnerViewport;
    for (int i = 0; i < ctx->Windows.Size; i++) {
        ImGuiWindow* w = ctx->Windows[i];
        if (!w || !w->Active || w->Hidden || w->DrawList->VtxBuffer.Size <= 0) continue;
        if (w->Flags & ImGuiWindowFlags_ChildWindow) continue;
        if (strcmp(w->Name, "Editor") == 0) continue;
        data.CmdLists.push_back(w->DrawList);
        data.TotalVtxCount += w->DrawList->VtxBuffer.Size;
        data.TotalIdxCount += w->DrawList->IdxBuffer.Size;
    }
    data.CmdListsCount = data.CmdLists.Size;
    if (data.CmdListsCount > 0) ImGui_ImplOpenGL3_RenderDrawData(&data);
}

static float GlyphWidth(ImFont* font, float size, unsigned cp) {
    const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
    if (!g) return size * 0.5f;
    return g->AdvanceX * (size / font->FontSize);
}

static float TextWidth(ImFont* font, float size, const std::string& s) {
    const char* p = s.c_str();
    float w = 0;
    while (*p) w += GlyphWidth(font, size, DecodeUtf8(p));
    return w;
}

static void WrapBalanced(ImFont* font, float size, const std::string& text, float maxW, std::vector<std::string>& lines) {
    lines.clear();
    std::string src = text;
    for (char& ch : src) if (ch == '\n' || ch == '\r') ch = ' ';
    if (src.empty() || !font || maxW < 8.0f) return;
    if (TextWidth(font, size, src) <= maxW) { lines.push_back(src); return; }
    std::vector<std::string> words;
    std::string word;
    for (char ch : src) {
        if (ch == ' ') {
            if (!word.empty()) { words.push_back(word); word.clear(); }
        } else word.push_back(ch);
    }
    if (!word.empty()) words.push_back(word);
    if (words.empty()) return;
    std::string line;
    for (const auto& w : words) {
        std::string trial = line.empty() ? w : line + " " + w;
        if (!line.empty() && TextWidth(font, size, trial) > maxW) {
            lines.push_back(line);
            line = w;
        } else line = trial;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.size() == 2) {
        float a = TextWidth(font, size, lines[0]);
        float b = TextWidth(font, size, lines[1]);
        if (a > b * 1.65f) {
            std::vector<std::string> again;
            std::string all = lines[0] + " " + lines[1];
            float half = TextWidth(font, size, all) * 0.5f;
            std::string left, bestL, bestR;
            float best = 1e9f;
            std::vector<std::string> ww;
            std::string wd;
            for (char ch : all) {
                if (ch == ' ') { if (!wd.empty()) { ww.push_back(wd); wd.clear(); } }
                else wd.push_back(ch);
            }
            if (!wd.empty()) ww.push_back(wd);
            for (size_t i = 0; i + 1 < ww.size(); i++) {
                if (!left.empty()) left += " ";
                left += ww[i];
                std::string right;
                for (size_t j = i + 1; j < ww.size(); j++) {
                    if (!right.empty()) right += " ";
                    right += ww[j];
                }
                float d = std::fabs(TextWidth(font, size, left) - half);
                if (d < best && TextWidth(font, size, left) <= maxW && TextWidth(font, size, right) <= maxW) {
                    best = d; bestL = left; bestR = right;
                }
            }
            if (!bestL.empty()) { lines.clear(); lines.push_back(bestL); lines.push_back(bestR); }
        }
    }
}

static float AtlasAlpha(float ax, float ay) {
    if (fontAtlas.empty() || fontAtlasW < 2 || fontAtlasH < 2) return 0.0f;
    ax -= 0.5f;
    ay -= 0.5f;
    int x0 = (int)std::floor(ax);
    int y0 = (int)std::floor(ay);
    float tx = ax - (float)x0;
    float ty = ay - (float)y0;
    auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= fontAtlasW || y >= fontAtlasH) return 0.0f;
        return fontAtlas[(size_t)y * (size_t)fontAtlasW + (size_t)x] * (1.0f / 255.0f);
    };
    float a0 = at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx;
    float a1 = at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx;
    return a0 * (1.0f - ty) + a1 * ty;
}

static void BlendPixel(uint8_t* d, unsigned char cr, unsigned char cg, unsigned char cb, float a) {
    if (a < 0.004f) return;
    if (a > 1.0f) a = 1.0f;
    float inv = 1.0f - a;
    d[0] = (uint8_t)std::lround(cr * a + d[0] * inv);
    d[1] = (uint8_t)std::lround(cg * a + d[1] * inv);
    d[2] = (uint8_t)std::lround(cb * a + d[2] * inv);
    d[3] = 255;
}

static void StampGlyph(uint8_t* canvas, int cw, int ch, const ImFontGlyph* g, float x0, float y0, float x1, float y1,
                       unsigned char cr, unsigned char cg, unsigned char cb,
                       unsigned char br, unsigned char bgc, unsigned char bb, float edge) {
    if (!g || fontAtlas.empty() || x1 <= x0 || y1 <= y0) return;
    float gw = x1 - x0;
    float gh = y1 - y0;
    float pad = edge > 0.2f ? edge : 0.0f;
    int ix0 = (int)std::floor(x0 - pad);
    int iy0 = (int)std::floor(y0 - pad);
    int ix1 = (int)std::ceil(x1 + pad);
    int iy1 = (int)std::ceil(y1 + pad);
    auto sampleAt = [&](float px, float py) -> float {
        float u = (px - x0) / gw;
        float v = (py - y0) / gh;
        if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) return 0.0f;
        float ax = (g->U0 + u * (g->U1 - g->U0)) * (float)fontAtlasW;
        float ay = (g->V0 + v * (g->V1 - g->V0)) * (float)fontAtlasH;
        float minX = g->U0 * (float)fontAtlasW + 0.5f;
        float maxX = g->U1 * (float)fontAtlasW - 0.5f;
        float minY = g->V0 * (float)fontAtlasH + 0.5f;
        float maxY = g->V1 * (float)fontAtlasH - 0.5f;
        if (maxX < minX) { minX = maxX = (minX + maxX) * 0.5f; }
        if (maxY < minY) { minY = maxY = (minY + maxY) * 0.5f; }
        if (ax < minX) ax = minX;
        if (ax > maxX) ax = maxX;
        if (ay < minY) ay = minY;
        if (ay > maxY) ay = maxY;
        return AtlasAlpha(ax, ay);
    };
    const int rings = pad > 0.2f ? 1 : 0;
    const int spokes = 16;
    for (int y = iy0; y < iy1; y++) {
        if (y < 0 || y >= ch) continue;
        for (int x = ix0; x < ix1; x++) {
            if (x < 0 || x >= cw) continue;
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float fill = sampleAt(px, py);
            float stroke = fill;
            for (int ring = 1; ring <= rings; ring++) {
                float rad = pad * ((float)ring / (float)rings);
                for (int k = 0; k < spokes; k++) {
                    float ang = (float)k * 6.2831853f / (float)spokes + (ring == 2 ? 0.26f : 0.0f);
                    stroke = std::max(stroke, sampleAt(px - std::cos(ang) * rad, py - std::sin(ang) * rad));
                }
            }
            uint8_t* d = canvas + ((size_t)y * (size_t)cw + (size_t)x) * 4;
            if (rings) BlendPixel(d, br, bgc, bb, stroke);
            BlendPixel(d, cr, cg, cb, fill);
        }
    }
}

static void DrawTextLine(uint8_t* canvas, int cw, int ch, ImFont* font, float size, float x, float y, const std::string& text,
                         unsigned char cr, unsigned char cg, unsigned char cb,
                         unsigned char br, unsigned char bgc, unsigned char bb, float edge) {
    if (!font) return;
    float scale = size / font->FontSize;
    const char* p = text.c_str();
    while (*p) {
        unsigned cp = DecodeUtf8(p);
        const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
        if (!g) { x += size * 0.5f; continue; }
        StampGlyph(canvas, cw, ch, g, x + g->X0 * scale, y + g->Y0 * scale, x + g->X1 * scale, y + g->Y1 * scale,
                   cr, cg, cb, br, bgc, bb, edge);
        x += g->AdvanceX * scale;
    }
}

static void ActiveSubs(float t, std::vector<int>& ids) {
    ids.clear();
    for (int i = 0; i < (int)subs.size(); i++) {
        if (subs[i].text.empty()) continue;
        if (t < subs[i].start || t >= subs[i].start + subs[i].duration) continue;
        ids.push_back(i);
    }
}

static void BurnSubtitles(uint8_t* canvas, int cw, int ch, float t) {
    std::vector<int> ids;
    ActiveSubs(t, ids);
    if (ids.empty() || fontAtlas.empty()) return;
    ImFont* font = subtitleFont ? subtitleFont : ImGui::GetFont();
    if (!font) return;
    float size = std::max(18.0f, ch * 0.048f) * std::max(0.3f, subStyle.fontScale);
    float maxW = cw * 0.82f;
    float lineH = size * 1.18f;
    float yBottom = ch - ch * 0.07f;
    unsigned char tr = (unsigned char)(Clamp01(subStyle.text[0]) * 255);
    unsigned char tg = (unsigned char)(Clamp01(subStyle.text[1]) * 255);
    unsigned char tb = (unsigned char)(Clamp01(subStyle.text[2]) * 255);
    unsigned char br = (unsigned char)(Clamp01(subStyle.edge[0]) * 255);
    unsigned char bgc = (unsigned char)(Clamp01(subStyle.edge[1]) * 255);
    unsigned char bb = (unsigned char)(Clamp01(subStyle.edge[2]) * 255);
    float edge = subStyle.border ? std::max(1.0f, subStyle.borderPx * (size / 42.0f)) : 0.0f;
    for (int n = (int)ids.size() - 1; n >= 0; n--) {
        std::vector<std::string> lines;
        WrapBalanced(font, size, subs[ids[n]].text, maxW, lines);
        if (lines.empty()) continue;
        float blockH = lineH * (float)lines.size();
        float y = yBottom - blockH;
        if (subStyle.background) {
            float widest = 0;
            for (const auto& ln : lines) widest = std::max(widest, TextWidth(font, size, ln));
            int x0 = (int)((cw - widest) * 0.5f - size * 0.35f);
            int y0 = (int)(y - size * 0.15f);
            int x1 = (int)((cw + widest) * 0.5f + size * 0.35f);
            int y1 = (int)(y + blockH + size * 0.2f);
            unsigned char bgr = (unsigned char)(Clamp01(subStyle.bg[0]) * 255);
            unsigned char bgg = (unsigned char)(Clamp01(subStyle.bg[1]) * 255);
            unsigned char bgb = (unsigned char)(Clamp01(subStyle.bg[2]) * 255);
            float ba = Clamp01(subStyle.bgAlpha);
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
                if (xx < 0 || yy < 0 || xx >= cw || yy >= ch) continue;
                uint8_t* d = canvas + ((size_t)yy * cw + xx) * 4;
                d[0] = (uint8_t)std::lround(bgr * ba + d[0] * (1.0f - ba));
                d[1] = (uint8_t)std::lround(bgg * ba + d[1] * (1.0f - ba));
                d[2] = (uint8_t)std::lround(bgb * ba + d[2] * (1.0f - ba));
            }
        }
        for (const auto& raw : lines) {
            std::string ln = HasHebrew(raw) ? ReverseUtf8(raw) : raw;
            float x = (cw - TextWidth(font, size, ln)) * 0.5f;
            DrawTextLine(canvas, cw, ch, font, size, x, y, ln, tr, tg, tb, br, bgc, bb, edge);
            y += lineH;
        }
        yBottom -= blockH + size * 0.35f;
    }
}

#ifdef _MSC_VER
#pragma optimize("", off)
#endif
void ExportSession::StepMp3() {
    if (!active) return;
    if (audioWritten >= audioTotal) {
        Finish(true);
        return;
    }
    int chunk = aFrameSize > 0 ? aFrameSize : 1152;
    int64_t left = audioTotal - audioWritten;
    int n = left < chunk ? (int)left : chunk;
    if (n < 1) {
        Finish(true);
        return;
    }
    double t = (double)audioWritten / 48000.0;
    std::vector<int16_t> tmp((size_t)n * 2);
    MixTimeline(t, n, tmp.data());
    afifo.insert(afifo.end(), tmp.begin(), tmp.end());
    audioWritten += n;
    if (!DrainAudio(false)) {
        Fail("Audio encode failed");
        return;
    }
    frameIndex = (int)(audioWritten / chunk);
    if (frameIndex > total) frameIndex = total;
    int pct = total ? (frameIndex * 100) / total : 0;
    if (pct > 100) pct = 100;
    char buf[128];
    snprintf(buf, sizeof(buf), "Audio MP3 %d%%", pct);
    status = buf;
    previewStatus = status;
}

void ExportSession::Step() {
    if (!active) return;
    if (audioOnly) { StepMp3(); return; }
    if (phase == 1) { StepRife(); return; }
    if (frameIndex >= total) {
        if (rifeOn && outFps > baseFps) { BeginRifePass(); return; }
        Finish(true);
        return;
    }
    double t = (double)frameIndex / (double)fps;
    std::fill(canvas.begin(), canvas.end(), 0);
    int top = -1, bestTrack = 99;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (clips[i].track >= 3) continue;
        if (t < clips[i].startTime || t >= clips[i].startTime + clips[i].duration) continue;
        if (clips[i].track <= bestTrack) { bestTrack = clips[i].track; top = i; }
    }
    int plate = -1;
    if (top >= 0 && clips[top].mediaId >= 0 && clips[top].mediaId < (int)media.size()
        && SolidHasAlpha(media[clips[top].mediaId].path)) {
        plate = top;
        top = ClipBelow(plate, (float)t);
    }
    if (top >= 0) {
        int ta = -1, tb = -1;
        float tp = 0;
        std::string tid;
        bool useFx = TransitionForClip(top, (float)t, ta, tb, tp, tid);
        if (useFx) {
            double nextT = (double)(frameIndex + 1) / (double)fps;
            int na = -1, nb2 = -1;
            float np = 0;
            std::string nid;
            if (!TransitionForClip(top, (float)nextT, na, nb2, np, nid)) {
                auto alive = [&](int c) {
                    if (c < 0 || c >= (int)clips.size()) return false;
                    float end = clips[c].startTime + clips[c].duration;
                    return nextT >= (double)clips[c].startTime && nextT < (double)end;
                };
                bool aLive = alive(ta);
                bool bLive = alive(tb);
                if (bLive && !aLive) tp = 1.0f;
                else if (aLive && !bLive) tp = 0.0f;
                else if (bLive && aLive) tp = 1.0f;
            }
        }
        auto blitClip = [&](int ci, double when) {
            if (ci < 0 || ci >= (int)clips.size()) return false;
            const Clip& c = clips[ci];
            float speed = std::max(0.05f, c.speed);
            double local = ClipLocalTime(ci, (float)when);
            double mediaT = c.sourceOffset + local * (double)speed;
            float cover = OpacityAt(c, (float)local);
            std::vector<uint8_t> mid;
            int mw = 0, mh = 0;
            if (RifePair(c.mediaId, mediaT, speed, mid, mw, mh)) {
                int idx = OpenDec(c.mediaId);
                if (idx < 0 || idx >= (int)decs.size()) return false;
                ExpDec& d = decs[idx];
                std::vector<uint8_t> saved = std::move(d.rgba);
                int srw = d.rw, srh = d.rh;
                d.rgba = std::move(mid);
                d.rw = mw;
                d.rh = mh;
                BlitToCanvas(idx, &c, cover);
                d.rgba = std::move(saved);
                d.rw = srw;
                d.rh = srh;
                return true;
            }
            int idx = OpenDec(c.mediaId);
            if (!EnsureFrame(c.mediaId, mediaT)) return false;
            BlitToCanvas(idx, &c, cover);
            return true;
        };
        int bg[2] = {-1, -1};
        int nb = 0;
        int partner = useFx ? ta : -1;
        int drive = useFx ? tb : top;
        bool gaps = FrontLeavesGaps(top, partner, plate, drive, (float)t, bg, nb);
        if (gaps) {
            for (int i = 0; i < nb; i++) blitClip(bg[i], t);
        }
        if (useFx) {
            static std::vector<uint8_t> frameA, frameB, bgKeep;
            if (gaps) bgKeep = canvas;
            std::fill(canvas.begin(), canvas.end(), 0);
            if (blitClip(ta, t)) frameA = canvas;
            else frameA.assign(canvas.size(), 0);
            std::fill(canvas.begin(), canvas.end(), 0);
            if (blitClip(tb, t)) frameB = canvas;
            else frameB.assign(canvas.size(), 0);
            FxLook early;
            FxEvaluate(tid.c_str(), tp, early);
            if (!gaps || early.shape != 0) {
                FxComposite(tid.c_str(), tp, canvas.data(), w, h,
                            frameA.data(), w, h, frameB.data(), w, h);
            } else {
                FxLook look;
                FxEvaluate(tid.c_str(), tp, look);
                float wb0 = look.alphaB;
                if (wb0 < 0) wb0 = 0;
                if (wb0 > 1) wb0 = 1;
                float wa0 = 1.0f - wb0;
                canvas = bgKeep;
                auto sample = [&](const std::vector<uint8_t>& img, int x, int y, float scale, int& r, int& g, int& b, int& a) {
                    r = g = b = a = 0;
                    if ((int)img.size() < w * h * 4 || scale < 0.05f) return;
                    if (std::fabs(scale - 1.0f) <= 0.0008f) {
                        const uint8_t* p = img.data() + ((size_t)y * (size_t)w + (size_t)x) * 4;
                        r = p[0]; g = p[1]; b = p[2]; a = p[3];
                        return;
                    }
                    float u = ((x + 0.5f) / (float)w - 0.5f) / scale + 0.5f;
                    float v = ((y + 0.5f) / (float)h - 0.5f) / scale + 0.5f;
                    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) return;
                    int ix = (int)(u * (float)w);
                    int iy = (int)(v * (float)h);
                    if (ix < 0) ix = 0;
                    if (iy < 0) iy = 0;
                    if (ix >= w) ix = w - 1;
                    if (iy >= h) iy = h - 1;
                    const uint8_t* p = img.data() + ((size_t)iy * (size_t)w + ix) * 4;
                    r = p[0]; g = p[1]; b = p[2]; a = p[3];
                };
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        int ar, ag, ab, aa, br, bgg, bb, ba;
                        sample(frameA, x, y, look.scaleA, ar, ag, ab, aa);
                        sample(frameB, x, y, look.scaleB, br, bgg, bb, ba);
                        float u = ((float)x + 0.5f) / (float)w;
                        float v = ((float)y + 0.5f) / (float)h;
                        float wa = aa ? wa0 : 0.0f;
                        float wb = ba ? wb0 : 0.0f;
                        if (look.wipe == 1) { bool takeB = u < wb0; wa = (takeB || !aa) ? 0.0f : 1.0f; wb = (takeB && ba) ? 1.0f : 0.0f; }
                        else if (look.wipe == 2) { bool takeB = u > 1.0f - wb0; wa = (takeB || !aa) ? 0.0f : 1.0f; wb = (takeB && ba) ? 1.0f : 0.0f; }
                        else if (look.wipe == 3) { bool takeB = v < wb0; wa = (takeB || !aa) ? 0.0f : 1.0f; wb = (takeB && ba) ? 1.0f : 0.0f; }
                        else if (look.wipe == 4) { bool takeB = v > 1.0f - wb0; wa = (takeB || !aa) ? 0.0f : 1.0f; wb = (takeB && ba) ? 1.0f : 0.0f; }
                        float sum = wa + wb;
                        size_t i = ((size_t)y * (size_t)w + x) * 4;
                        const uint8_t* s = canvas.data() + i;
                        float rr, rg, rbv;
                        if (sum <= 0.001f) {
                            rr = s[0];
                            rg = s[1];
                            rbv = s[2];
                        } else {
                            float keep = 1.0f - sum;
                            if (keep < 0) keep = 0;
                            rr = ar * wa + br * wb + s[0] * keep;
                            rg = ag * wa + bgg * wb + s[1] * keep;
                            rbv = ab * wa + bb * wb + s[2] * keep;
                        }
                        if (look.dip > 0.001f) {
                            float k = look.dip > 1.0f ? 1.0f : look.dip;
                            rr = rr * (1.0f - k) + look.dipR * 255.0f * k;
                            rg = rg * (1.0f - k) + look.dipG * 255.0f * k;
                            rbv = rbv * (1.0f - k) + look.dipB * 255.0f * k;
                        }
                        canvas[i] = (uint8_t)std::min(255, (int)rr);
                        canvas[i + 1] = (uint8_t)std::min(255, (int)rg);
                        canvas[i + 2] = (uint8_t)std::min(255, (int)rbv);
                        canvas[i + 3] = 255;
                    }
                }
            }
        } else {
            blitClip(top, t);
        }
        if (top >= 0 && top < (int)clips.size()) {
            BusSegment cg = ClipGradeOf(clips[top]);
            if (BusActive(cg)) GradeCanvas(canvas.data(), w, h, cg, frameIndex);
        }
    }
    if (plate >= 0) {
        const Clip& c = clips[plate];
        int idx = OpenDec(c.mediaId);
        if (EnsureFrame(c.mediaId, 0)) BlitToCanvas(idx, &c, OpacityAt(c, (float)t - c.startTime));
    }
    int gs = FindGsClip((float)t);
    if (gs >= 0) {
        const Clip& c = clips[gs];
        float speed = std::max(0.05f, c.speed);
        double mediaT = c.sourceOffset + ClipLocalTime(gs, (float)t) * (double)speed;
        std::vector<uint8_t> base = canvas;
        std::vector<uint8_t> mid;
        int mw = 0, mh = 0;
        float gsCover = c.gsOpacity * OpacityAt(c, (float)t - c.startTime);
        if (RifePair(c.mediaId, mediaT, speed, mid, mw, mh)) {
            BlendChromaPlaced(base.data(), w, h, mid.data(), mw, mh,
                              c.keySim, c.keySoft, c.keySpill, c.gsX, c.gsY, c.gsScale, gsCover, c.gsAspect, c.gsRot);
            canvas.swap(base);
        } else {
            int idx = OpenDec(c.mediaId);
            if (idx >= 0 && EnsureFrame(c.mediaId, mediaT) && idx < (int)decs.size() && !decs[idx].rgba.empty()) {
                ExpDec& d = decs[idx];
                BlendChromaPlaced(base.data(), w, h, d.rgba.data(), d.rw, d.rh,
                                  c.keySim, c.keySoft, c.keySpill, c.gsX, c.gsY, c.gsScale, gsCover, c.gsAspect, c.gsRot);
                canvas.swap(base);
            }
        }
    }
    OverlayStamp(canvas.data(), w, h, (float)t);
    if (const BusSegment* bs = BusAt((float)t)) GradeCanvas(canvas.data(), w, h, *bs, frameIndex);
    if (burnSubs && !rifeOn) BurnSubtitles(canvas.data(), w, h, (float)t);
    if (!vframe || av_frame_make_writable(vframe) < 0) { Fail("Video frame failed"); return; }
    uint8_t* srcData[1] = { canvas.data() };
    int srcLines[1] = { w * 4 };
    sws_scale(toYuv, srcData, srcLines, 0, h, vframe->data, vframe->linesize);
    vframe->pts = frameIndex;
    if (avcodec_send_frame(vctx, vframe) < 0) { Fail("Video encode failed"); return; }
    if (!WriteEncoded(vctx, vst)) { Fail("Video write failed"); return; }
    if (actx && swr) {
        int64_t want = av_rescale((int64_t)frameIndex + 1, 48000, fps);
        int n = (int)(want - audioWritten);
        if (n > 0) {
            std::vector<int16_t> tmp((size_t)n * 2);
            MixTimeline(t, n, tmp.data());
            afifo.insert(afifo.end(), tmp.begin(), tmp.end());
            audioWritten += n;
        }
        if (!DrainAudio(false)) { Fail("Audio encode failed"); return; }
    }
    frameIndex++;
    int pct = total ? (frameIndex * 100) / total : 0;
    char buf[192];
    if (rifeOn)
        snprintf(buf, sizeof(buf), "Rendering %d%%  (%d/%d)  then RIFE", pct, frameIndex, total);
    else
        snprintf(buf, sizeof(buf), "Export %d%%  (%d/%d)  %dx%d", pct, frameIndex, total, w, h);
    status = buf;
    previewStatus = status;
}
#ifdef _MSC_VER
#pragma optimize("", on)
#endif

void ExportSession::SealPass() {
    if (actx && swr) DrainAudio(true);
    if (vctx) { avcodec_send_frame(vctx, nullptr); WriteEncoded(vctx, vst); }
    if (actx) { avcodec_send_frame(actx, nullptr); WriteEncoded(actx, ast); }
    if (fmt && headerWritten) av_write_trailer(fmt);
    headerWritten = false;
    for (auto& d : decs) {
        if (d.fit) sws_freeContext(d.fit);
        if (d.sws) sws_freeContext(d.sws);
        if (d.frame) av_frame_free(&d.frame);
        if (d.pkt) av_packet_free(&d.pkt);
        if (d.ctx) avcodec_free_context(&d.ctx);
        if (d.hw) { d.hw->Free(); delete d.hw; d.hw = nullptr; }
        if (d.fmt) avformat_close_input(&d.fmt);
        d.fit = nullptr;
        d.sws = nullptr;
    }
    decs.clear();
    if (swr) swr_free(&swr);
    if (toYuv) { sws_freeContext(toYuv); toYuv = nullptr; }
    if (vframe) av_frame_free(&vframe);
    if (aframe) av_frame_free(&aframe);
    if (pkt) av_packet_free(&pkt);
    if (vctx) avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (fmt) {
        if (fmt->pb) avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        fmt = nullptr;
    }
    vst = nullptr;
    ast = nullptr;
    afifo.clear();
}

bool ExportSession::ReadRifeFrame(std::vector<uint8_t>& dst) {
    if (!rifeIn || !rifeDec || !rifePkt || !rifeFrame) return false;
    while (av_read_frame(rifeIn, rifePkt) >= 0) {
        if (rifePkt->stream_index != rifeStream) { av_packet_unref(rifePkt); continue; }
        int send = avcodec_send_packet(rifeDec, rifePkt);
        av_packet_unref(rifePkt);
        if (send < 0 && send != AVERROR(EAGAIN)) continue;
        if (avcodec_receive_frame(rifeDec, rifeFrame) != 0) continue;
        if (rifeFrame->width <= 0 || rifeFrame->height <= 0) continue;
        if (!rifeSws) {
            rifeSws = sws_getContext(rifeFrame->width, rifeFrame->height, (AVPixelFormat)rifeFrame->format,
                                     w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            prevRGBA.assign((size_t)w * (size_t)h * 4, 0);
        }
        if (!rifeSws) return false;
        dst.assign((size_t)w * (size_t)h * 4, 0);
        uint8_t* data[1] = { dst.data() };
        int lines[1] = { w * 4 };
        sws_scale(rifeSws, rifeFrame->data, rifeFrame->linesize, 0, rifeFrame->height, data, lines);
        return true;
    }
    return false;
}

void ExportSession::ReleasePassDecoder() {
    if (rifeSws) { sws_freeContext(rifeSws); rifeSws = nullptr; }
    if (rifeFrame) av_frame_free(&rifeFrame);
    if (rifePkt) av_packet_free(&rifePkt);
    if (rifeDec) avcodec_free_context(&rifeDec);
    if (rifeIn) avformat_close_input(&rifeIn);
    if (rifeAudio) avformat_close_input(&rifeAudio);
    rifeStream = -1;
}

bool ExportSession::BeginRifePass() {
    std::string rendered = tempPath;
    SealPass();
    std::string err;
    if (!RifeStartup(err)) { Fail(err.c_str()); return false; }
    if (avformat_open_input(&rifeIn, rendered.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(rifeIn, nullptr) < 0) {
        Fail("Cannot open the rendered video");
        return false;
    }
    for (unsigned i = 0; i < rifeIn->nb_streams; i++) {
        if (rifeIn->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { rifeStream = (int)i; break; }
    }
    if (rifeStream < 0) { Fail("Rendered video has no picture"); return false; }
    const AVCodec* codec = avcodec_find_decoder(rifeIn->streams[rifeStream]->codecpar->codec_id);
    rifeDec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!rifeDec || avcodec_parameters_to_context(rifeDec, rifeIn->streams[rifeStream]->codecpar) < 0 || avcodec_open2(rifeDec, codec, nullptr) < 0) {
        Fail("Cannot decode the rendered video");
        return false;
    }
    rifeFrame = av_frame_alloc();
    rifePkt = av_packet_alloc();
    if (!ReadRifeFrame(prevRGBA)) { Fail("Rendered video is empty"); return false; }
    haveNext = ReadRifeFrame(nextRGBA);
    emitMid = false;
    rifeSlot = 0;

    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", finalPath.c_str()) < 0 || !fmt) {
        Fail("Cannot create the final mp4");
        return false;
    }
    const AVCodec* vcodec = avcodec_find_encoder_by_name("h264_nvenc");
    bool nv = vcodec != nullptr;
    if (!vcodec) vcodec = avcodec_find_encoder_by_name("libx264");
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) { Fail("Encoder not found"); return false; }
    vctx = avcodec_alloc_context3(vcodec);
    vctx->width = w;
    vctx->height = h;
    vctx->time_base = AVRational{1, outFps};
    vctx->framerate = AVRational{outFps, 1};
    vctx->gop_size = outFps;
    vctx->max_b_frames = 0;
    vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    int64_t br = (int64_t)w * (int64_t)h;
    if (br < 4000000) br = 4000000;
    if (br > 80000000) br = 80000000;
    vctx->bit_rate = br;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (nv && vctx->priv_data) {
        av_opt_set(vctx->priv_data, "preset", "p1", 0);
        av_opt_set(vctx->priv_data, "rc", "vbr", 0);
        av_opt_set(vctx->priv_data, "cq", "19", 0);
    } else if (vctx->priv_data) {
        av_opt_set(vctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(vctx->priv_data, "crf", "20", 0);
    }
    if (avcodec_open2(vctx, vcodec, nullptr) < 0) { Fail("Cannot open the final encoder"); return false; }
    vst = avformat_new_stream(fmt, nullptr);
    if (!vst) { Fail("Cannot add the final video"); return false; }
    avcodec_parameters_from_context(vst->codecpar, vctx);
    vst->time_base = vctx->time_base;

    int astream = -1;
    if (!videoOnly && avformat_open_input(&rifeAudio, rendered.c_str(), nullptr, nullptr) == 0 && avformat_find_stream_info(rifeAudio, nullptr) == 0) {
        for (unsigned i = 0; i < rifeAudio->nb_streams; i++) {
            if (rifeAudio->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { astream = (int)i; break; }
        }
        if (astream >= 0) {
            ast = avformat_new_stream(fmt, nullptr);
            if (ast) {
                avcodec_parameters_copy(ast->codecpar, rifeAudio->streams[astream]->codecpar);
                ast->codecpar->codec_tag = 0;
                ast->time_base = rifeAudio->streams[astream]->time_base;
            }
        }
    }
    if (avio_open(&fmt->pb, finalPath.c_str(), AVIO_FLAG_WRITE) < 0) { Fail("Cannot open the final file"); return false; }
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "movflags", "+faststart", 0);
    if (avformat_write_header(fmt, &opt) < 0) { av_dict_free(&opt); Fail("Cannot write the final header"); return false; }
    av_dict_free(&opt);
    headerWritten = true;
    if (rifeAudio && ast && astream >= 0) {
        AVPacket* ap = av_packet_alloc();
        while (ap && av_read_frame(rifeAudio, ap) >= 0) {
            if (ap->stream_index == astream) {
                av_packet_rescale_ts(ap, rifeAudio->streams[astream]->time_base, ast->time_base);
                ap->stream_index = ast->index;
                av_interleaved_write_frame(fmt, ap);
            }
            av_packet_unref(ap);
        }
        av_packet_free(&ap);
        avformat_close_input(&rifeAudio);
    }
    vframe = av_frame_alloc();
    vframe->format = vctx->pix_fmt;
    vframe->width = w;
    vframe->height = h;
    if (av_frame_get_buffer(vframe, 32) < 0) { Fail("Out of memory"); return false; }
    toYuv = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, vctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!toYuv) { Fail("Scaler failed"); return false; }
    pkt = av_packet_alloc();
    canvas.assign((size_t)w * (size_t)h * 4, 0);
    frameIndex = 0;
    total = (int)std::llround(timelineEnd * (double)outFps);
    if (total < 1) total = 1;
    phase = 1;
    fps = outFps;
    status = RifeProbe().detail;
    previewStatus = status;
    return true;
}

void ExportSession::StepRife() {
    if (frameIndex >= total) { Finish(true); return; }
    double t = (double)frameIndex / (double)std::max(1, outFps);
    const uint8_t* src = prevRGBA.empty() ? nullptr : prevRGBA.data();
    int factor = rifeFactor < 2 ? 2 : rifeFactor;
    if (rifeSlot > 0 && haveNext && !nextRGBA.empty() && nextRGBA.size() == prevRGBA.size()) {
        float ts = (float)rifeSlot / (float)factor;
        rifeMid.assign(prevRGBA.size(), 255);
        if (!RifeRun(prevRGBA.data(), nextRGBA.data(), w, h, ts, rifeMid.data())) rifeMid = prevRGBA;
        src = rifeMid.data();
    }
    if (src && canvas.size() == prevRGBA.size()) memcpy(canvas.data(), src, canvas.size());
    if (burnSubs) BurnSubtitles(canvas.data(), w, h, (float)t);
    if (!vframe || av_frame_make_writable(vframe) < 0) { Fail("Video frame failed"); return; }
    uint8_t* data[1] = { canvas.data() };
    int lines[1] = { w * 4 };
    sws_scale(toYuv, data, lines, 0, h, vframe->data, vframe->linesize);
    vframe->pts = frameIndex;
    if (avcodec_send_frame(vctx, vframe) < 0) { Fail("Video encode failed"); return; }
    if (!WriteEncoded(vctx, vst)) { Fail("Video write failed"); return; }
    if (haveNext) {
        rifeSlot++;
        if (rifeSlot >= factor) {
            prevRGBA.swap(nextRGBA);
            haveNext = ReadRifeFrame(nextRGBA);
            rifeSlot = 0;
        }
    }
    frameIndex++;
    int pct = total ? (frameIndex * 100) / total : 0;
    char buf[160];
    if (RifeProbe().cuda)
        snprintf(buf, sizeof(buf), "RIFE GPU %d%%  (%d/%d)", pct, frameIndex, total);
    else
        snprintf(buf, sizeof(buf), "RIFE CPU %d%%  (%d/%d)  %s", pct, frameIndex, total, RifeProbe().detail.c_str());
    status = buf;
    previewStatus = status;
}

void ExportSession::Finish(bool ok) {
    if (actx && swr) DrainAudio(true);
    if (vctx) {
        avcodec_send_frame(vctx, nullptr);
        WriteEncoded(vctx, vst);
    }
    if (actx) {
        avcodec_send_frame(actx, nullptr);
        WriteEncoded(actx, ast);
    }
    if (fmt && headerWritten) av_write_trailer(fmt);
    headerWritten = false;
    std::string saved = finalPath.empty() ? path : finalPath;
    CloseAll();
    if (ok) {
        status = "Exported " + saved;
        previewStatus = status;
    }
}

void ExportSession::Cancel() {
    if (fmt && headerWritten) av_write_trailer(fmt);
    headerWritten = false;
    CloseAll();
    status = "Export cancelled";
    previewStatus = status;
}

ExportSession gExport;

void EnsurePlayerForClip(int clipIdx);

void SyncPreview(int topClip, float dt) {
    bool playing = isPlaying && !draggingPlayhead;
    if (topClip < 0 || !player) {
        if (playing) currentTime += dt;
        PumpAudio();
        return;
    }
    int mid = clips[topClip].mediaId;
    bool image = mid >= 0 && mid < (int)media.size() && media[mid].type == "image";
    if (playing) currentTime += dt;
    float speed = std::max(0.05f, clips[topClip].speed);
    float mediaT = image ? 0.0f : clips[topClip].sourceOffset + ClipLocalTime(topClip, currentTime) * speed;
    if (gRam.Present(player, mid, mediaT)) {
        shownLocal = mediaT;
        char buf[128];
        int cachePct = gRam.Percent();
        int cacheMb = (int)(gRam.usedBytes.load() >> 20);
        const char* tag = gRam.MemoryFull() ? "mem full" : (cachePct >= 99 ? "cached" : "loading");
        snprintf(buf, sizeof(buf), "RAM %dMB %dx%d  %s %d%%", cacheMb, player->width, player->height, tag, cachePct);
        previewStatus = buf;
        PumpAudio();
        return;
    }
    if (playerFileId != mid) {
        EnsurePlayerForClip(topClip);
        if (player && gRam.Present(player, mid, mediaT)) {
            shownLocal = mediaT;
            PumpAudio();
            return;
        }
        if (playerFileId != mid || !player || (!player->fmtCtx && !player->still)) {
            PumpAudio();
            return;
        }
    }
    if (!player->fmtCtx) {
        if (player->still && player->hasFrame) previewStatus = "Solid";
        PumpAudio();
        return;
    }
    if (image) {
        if (!player->hasFrame) {
            player->Seek(0);
            player->GrabFrame();
        }
        previewStatus = "Image";
        PumpAudio();
        return;
    }
    if (!playing && player->hasFrame && std::fabs(mediaT - shownLocal) <= 0.04f) {
        PumpAudio();
        return;
    }
    bool ahead = player->lastPts >= 0.0 && mediaT + 0.12f < (float)player->lastPts;
    if (player->lastPts < 0.0 || ahead || (!playing && player->hasFrame && std::fabs(mediaT - (float)player->lastPts) > 0.05f)) {
        player->Seek(std::max(0.0f, mediaT));
        player->GrabFrame();
    }
    int limit = 2;
    if (player->lastPts < 0.0 || (float)player->lastPts + 0.08f < mediaT)
        limit = playing ? 4 : 400;
    int guard = 0;
    while (player->lastPts >= 0.0 && (float)player->lastPts + 0.0005f < mediaT && guard < limit) {
        if (!player->GrabFrame()) break;
        guard++;
    }
    shownLocal = mediaT;
    if (player->hasFrame) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %dx%d  x%.2g  cache %dMB", player->decName, player->width, player->height, speed, (int)(gRam.usedBytes.load() >> 20));
        previewStatus = buf;
    }
    PumpAudio();
}

static void SyncHeld(VideoPlayer*& slot, int& slotClip, int& slotMedia, float& slotShown, int clipIdx, float timelineT) {
    if (clipIdx < 0 || clipIdx >= (int)clips.size()) return;
    int mid = clips[clipIdx].mediaId;
    if (mid < 0 || mid >= (int)media.size()) return;
    if (!slot) slot = new VideoPlayer();
    bool image = media[mid].type == "image";
    float mediaT = image ? 0.0f : clips[clipIdx].sourceOffset + ClipLocalTime(clipIdx, timelineT) * std::max(0.05f, clips[clipIdx].speed);
    if (gRam.Present(slot, mid, mediaT)) {
        slotClip = clipIdx;
        slotMedia = mid;
        slotShown = mediaT;
        bool close = image || std::fabs((float)slot->lastPts - mediaT) <= 0.045f;
        if (close) return;
    }
    bool sideReady = slot->fmtCtx || (slot->still && slot->hasFrame);
    if (slotClip != clipIdx || slotMedia != mid || !sideReady) {
        if (playerNext && nextClipIdx == clipIdx && nextMediaId == mid && playerNext->fmtCtx && playerNext->hasFrame) {
            VideoPlayer* old = slot;
            slot = playerNext;
            playerNext = nullptr;
            slotMedia = mid;
            nextMediaId = -1;
            nextClipIdx = -1;
            slotShown = -1000.0f;
            if (old && old != slot) { old->Close(); delete old; }
        } else if (!slot->Open(media[mid].path)) {
            return;
        } else {
            slotMedia = mid;
            slotShown = -1000.0f;
        }
        slotClip = clipIdx;
    }
    if (media[mid].type == "image") {
        if (!slot->hasFrame) {
            slot->Seek(0);
            slot->GrabFrame();
        }
        return;
    }
    bool ahead = slot->lastPts >= 0.0 && mediaT + 0.08f < (float)slot->lastPts;
    bool behind = slot->lastPts < 0.0 || (float)slot->lastPts + 0.08f < mediaT;
    if (isPlaying) {
        if (slot->lastPts < 0.0 || ahead || (float)slot->lastPts + 1.0f < mediaT) {
            slot->Seek(std::max(0.0f, mediaT));
            slot->GrabFrame();
        }
    } else if (ahead || behind) {
        if (slot->lastPts < 0.0 || ahead || (float)slot->lastPts + 0.20f < mediaT)
            slot->Seek(std::max(0.0f, mediaT));
        slot->GrabFrame();
    }
    int guard = 0;
    int cap = isPlaying ? 4 : 80;
    while (slot->lastPts >= 0.0 && (float)slot->lastPts + 0.0005f < mediaT && guard < cap) {
        if (!slot->GrabFrame()) break;
        guard++;
    }
    slotShown = mediaT;
}

void SyncSide(int clipIdx, float timelineT) {
    SyncHeld(playerB, playingClipB, playingMediaIdB, shownLocalB, clipIdx, timelineT);
}

void SyncUnder(int clipIdx, float timelineT) {
    SyncHeld(playerUnder, playingClipUnder, playingMediaIdUnder, shownLocalUnder, clipIdx, timelineT);
}


#endif

static GLuint gradeProgram = 0;
static int gradeReady = 0;
static int uTex = -1, uTexel = -1, uLight = -1, uContrast = -1, uWarm = -1, uTint = -1, uSat = -1, uShadow = -1, uGrain = -1, uSharp = -1, uTime = -1, uAlpha = -1;

typedef unsigned int (APIENTRY *PFN_glCreateShader_t)(unsigned int);
typedef void (APIENTRY *PFN_glShaderSource_t)(unsigned int, int, const char* const*, const int*);
typedef void (APIENTRY *PFN_glCompileShader_t)(unsigned int);
typedef unsigned int (APIENTRY *PFN_glCreateProgram_t)(void);
typedef void (APIENTRY *PFN_glAttachShader_t)(unsigned int, unsigned int);
typedef void (APIENTRY *PFN_glLinkProgram_t)(unsigned int);
typedef void (APIENTRY *PFN_glDeleteShader_t)(unsigned int);
typedef void (APIENTRY *PFN_glGetShaderiv_t)(unsigned int, unsigned int, int*);
typedef void (APIENTRY *PFN_glGetProgramiv_t)(unsigned int, unsigned int, int*);
typedef int (APIENTRY *PFN_glGetUniformLocation_t)(unsigned int, const char*);
typedef void (APIENTRY *PFN_glUniform1f_t)(int, float);
typedef void (APIENTRY *PFN_glUniform1i_t)(int, int);
typedef void (APIENTRY *PFN_glUniform2f_t)(int, float, float);

static PFN_glCreateShader_t pCreateShader = nullptr;
static PFN_glShaderSource_t pShaderSource = nullptr;
static PFN_glCompileShader_t pCompileShader = nullptr;
static PFN_glCreateProgram_t pCreateProgram = nullptr;
static PFN_glAttachShader_t pAttachShader = nullptr;
static PFN_glLinkProgram_t pLinkProgram = nullptr;
static PFN_glDeleteShader_t pDeleteShader = nullptr;
static PFN_glGetShaderiv_t pGetShaderiv = nullptr;
static PFN_glGetProgramiv_t pGetProgramiv = nullptr;
static PFN_glGetUniformLocation_t pGetUniform = nullptr;
static PFN_glUniform1f_t pUniform1f = nullptr;
static PFN_glUniform1i_t pUniform1i = nullptr;
static PFN_glUniform2f_t pUniform2f = nullptr;

static bool CompileStage(unsigned int type, const char* src, unsigned int& out) {
    out = pCreateShader(type);
    pShaderSource(out, 1, &src, nullptr);
    pCompileShader(out);
    int ok = 0;
    pGetShaderiv(out, 0x8B81, &ok);
    return ok == 1;
}

static bool EnsureGrade() {
    if (gradeReady == 1) return gradeProgram != 0;
    if (gradeReady == -1) return false;
    gradeReady = -1;
    if (!pglUseProgram) return false;
    pCreateShader = (PFN_glCreateShader_t)SDL_GL_GetProcAddress("glCreateShader");
    pShaderSource = (PFN_glShaderSource_t)SDL_GL_GetProcAddress("glShaderSource");
    pCompileShader = (PFN_glCompileShader_t)SDL_GL_GetProcAddress("glCompileShader");
    pCreateProgram = (PFN_glCreateProgram_t)SDL_GL_GetProcAddress("glCreateProgram");
    pAttachShader = (PFN_glAttachShader_t)SDL_GL_GetProcAddress("glAttachShader");
    pLinkProgram = (PFN_glLinkProgram_t)SDL_GL_GetProcAddress("glLinkProgram");
    pDeleteShader = (PFN_glDeleteShader_t)SDL_GL_GetProcAddress("glDeleteShader");
    pGetShaderiv = (PFN_glGetShaderiv_t)SDL_GL_GetProcAddress("glGetShaderiv");
    pGetProgramiv = (PFN_glGetProgramiv_t)SDL_GL_GetProcAddress("glGetProgramiv");
    pGetUniform = (PFN_glGetUniformLocation_t)SDL_GL_GetProcAddress("glGetUniformLocation");
    pUniform1f = (PFN_glUniform1f_t)SDL_GL_GetProcAddress("glUniform1f");
    pUniform1i = (PFN_glUniform1i_t)SDL_GL_GetProcAddress("glUniform1i");
    pUniform2f = (PFN_glUniform2f_t)SDL_GL_GetProcAddress("glUniform2f");
    if (!pCreateShader || !pShaderSource || !pCompileShader || !pCreateProgram || !pAttachShader || !pLinkProgram || !pGetShaderiv || !pGetProgramiv || !pGetUniform || !pUniform1f || !pUniform1i || !pUniform2f)
        return false;
    const char* vs =
        "#version 130\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    gl_Position = ftransform();\n"
        "    vUV = gl_MultiTexCoord0.xy;\n"
        "}\n";
    const char* fs =
        "#version 130\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D tex;\n"
        "uniform vec2 texel;\n"
        "uniform float light, contrast, warmth, tint, sat, shadow, grain, sharp, time, alpha;\n"
        "void main() {\n"
        "    vec4 c = texture2D(tex, vUV);\n"
        "    if (sharp > 0.001) {\n"
        "        vec3 blur = (texture2D(tex, vUV + vec2(texel.x, 0.0)).rgb +\n"
        "                     texture2D(tex, vUV - vec2(texel.x, 0.0)).rgb +\n"
        "                     texture2D(tex, vUV + vec2(0.0, texel.y)).rgb +\n"
        "                     texture2D(tex, vUV - vec2(0.0, texel.y)).rgb) * 0.25;\n"
        "        c.rgb += (c.rgb - blur) * sharp * 1.6;\n"
        "    }\n"
        "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
        "    float shw = 1.0 - smoothstep(0.0, 0.5, l);\n"
        "    c.rgb += shadow * 0.35 * shw;\n"
        "    float ct = 1.0 + contrast * 1.35;\n"
        "    c.rgb = (c.rgb - 0.5) * ct + 0.5;\n"
        "    c.rgb += light * 0.45;\n"
        "    l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
        "    c.rgb = mix(vec3(l), c.rgb, 1.0 + sat);\n"
        "    c.r += warmth * 0.18; c.g += warmth * 0.05; c.b -= warmth * 0.18;\n"
        "    c.g += tint * 0.16; c.r -= tint * 0.08; c.b -= tint * 0.08;\n"
        "    if (grain > 0.001) {\n"
        "        float n = fract(sin(dot(gl_FragCoord.xy + vec2(time * 13.1, time * 7.7), vec2(12.9898, 78.233))) * 43758.5453);\n"
        "        l = clamp(dot(c.rgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);\n"
        "        float mid = clamp(1.0 - abs(l - 0.45) * 1.7, 0.15, 1.0);\n"
        "        c.rgb += (n - 0.5) * grain * 0.28 * mid;\n"
        "    }\n"
        "    c.rgb = clamp(c.rgb, 0.0, 1.0);\n"
        "    gl_FragColor = vec4(c.rgb, c.a * alpha);\n"
        "}\n";
    unsigned int vert = 0, frag = 0;
    if (!CompileStage(0x8B31, vs, vert) || !CompileStage(0x8B30, fs, frag)) {
        if (pDeleteShader) {
            if (vert) pDeleteShader(vert);
            if (frag) pDeleteShader(frag);
        }
        return false;
    }
    gradeProgram = pCreateProgram();
    pAttachShader(gradeProgram, vert);
    pAttachShader(gradeProgram, frag);
    pLinkProgram(gradeProgram);
    int ok = 0;
    pGetProgramiv(gradeProgram, 0x8B82, &ok);
    if (pDeleteShader) { pDeleteShader(vert); pDeleteShader(frag); }
    if (!ok) { gradeProgram = 0; return false; }
    uTex = pGetUniform(gradeProgram, "tex");
    uTexel = pGetUniform(gradeProgram, "texel");
    uLight = pGetUniform(gradeProgram, "light");
    uContrast = pGetUniform(gradeProgram, "contrast");
    uWarm = pGetUniform(gradeProgram, "warmth");
    uTint = pGetUniform(gradeProgram, "tint");
    uSat = pGetUniform(gradeProgram, "sat");
    uShadow = pGetUniform(gradeProgram, "shadow");
    uGrain = pGetUniform(gradeProgram, "grain");
    uSharp = pGetUniform(gradeProgram, "sharp");
    uTime = pGetUniform(gradeProgram, "time");
    uAlpha = pGetUniform(gradeProgram, "alpha");
    gradeReady = 1;
    return true;
}

static bool BindGrade(const BusSegment& s, int tw, int th, float alpha) {
    if (!BusActive(s) || !EnsureGrade()) return false;
    pglUseProgram(gradeProgram);
    if (uTex >= 0) pUniform1i(uTex, 0);
    if (uTexel >= 0) pUniform2f(uTexel, tw > 0 ? 1.0f / (float)tw : 0.0f, th > 0 ? 1.0f / (float)th : 0.0f);
    if (uLight >= 0) pUniform1f(uLight, s.lightness);
    if (uContrast >= 0) pUniform1f(uContrast, s.contrast);
    if (uWarm >= 0) pUniform1f(uWarm, s.warmth);
    if (uTint >= 0) pUniform1f(uTint, s.tint);
    if (uSat >= 0) pUniform1f(uSat, s.saturation);
    if (uShadow >= 0) pUniform1f(uShadow, s.shadow);
    if (uGrain >= 0) pUniform1f(uGrain, s.grain);
    if (uSharp >= 0) pUniform1f(uSharp, s.sharpen);
    if (uTime >= 0) pUniform1f(uTime, currentTime);
    if (uAlpha >= 0) pUniform1f(uAlpha, alpha);
    return true;
}

static GLuint keyProgram = 0;
static int keyReady = 0;
static int kTex = -1, kSim = -1, kSoft = -1, kSpill = -1, kOpacity = -1;

static bool BindKey(float sim, float soft, float spill, float opacity) {
    if (keyReady == -1) return false;
    if (!pglUseProgram) return false;
    if (keyReady == 0) {
        if (!EnsureGrade() && !pCreateShader) return false;
        if (!pCreateShader) { keyReady = -1; return false; }
        const char* vs =
            "#version 130\n"
            "varying vec2 vUV;\n"
            "void main(){ gl_Position = ftransform(); vUV = gl_MultiTexCoord0.xy; }\n";
        const char* fs =
            "#version 130\n"
            "varying vec2 vUV;\n"
            "uniform sampler2D tex;\n"
            "uniform float sim, soft, spill, opacity;\n"
            "void main(){\n"
            "  vec4 c = texture2D(tex, vUV);\n"
            "  float rb = max(c.r, c.b);\n"
            "  float dom = c.g - rb;\n"
            "  float t0 = 0.04 + sim * 0.50;\n"
            "  float t1 = t0 + 0.03 + soft * 0.40;\n"
            "  float k = smoothstep(t0, max(t0 + 0.001, t1), dom);\n"
            "  float peak = max(c.r, max(c.g, c.b));\n"
            "  if (peak > 0.82 && dom < 0.18) k = 0.0;\n"
            "  vec3 rgb = c.rgb;\n"
            "  rgb.g -= max(0.0, rgb.g - rb) * spill;\n"
            "  gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), (1.0 - k) * c.a * opacity);\n"
            "}\n";
        unsigned int vert = 0, frag = 0;
        if (!CompileStage(0x8B31, vs, vert) || !CompileStage(0x8B30, fs, frag)) { keyReady = -1; return false; }
        keyProgram = pCreateProgram();
        pAttachShader(keyProgram, vert);
        pAttachShader(keyProgram, frag);
        pLinkProgram(keyProgram);
        int ok = 0;
        pGetProgramiv(keyProgram, 0x8B82, &ok);
        if (pDeleteShader) { pDeleteShader(vert); pDeleteShader(frag); }
        if (!ok) { keyReady = -1; keyProgram = 0; return false; }
        kTex = pGetUniform(keyProgram, "tex");
        kSim = pGetUniform(keyProgram, "sim");
        kSoft = pGetUniform(keyProgram, "soft");
        kSpill = pGetUniform(keyProgram, "spill");
        kOpacity = pGetUniform(keyProgram, "opacity");
        keyReady = 1;
    }
    if (!keyProgram) return false;
    pglUseProgram(keyProgram);
    if (kTex >= 0) pUniform1i(kTex, 0);
    if (kSim >= 0) pUniform1f(kSim, sim);
    if (kSoft >= 0) pUniform1f(kSoft, soft);
    if (kSpill >= 0) pUniform1f(kSpill, spill);
    if (kOpacity >= 0) pUniform1f(kOpacity, opacity);
    return true;
}

static void PictureCorners(ImVec2 a, ImVec2 b, float ox, float oy, float scale, float aspect, float rot, ImVec2 q[4]) {
    float pw = std::max(1.0f, b.x - a.x);
    float ph = std::max(1.0f, b.y - a.y);
    if (aspect < 0.05f) aspect = 1.0f;
    if (scale < 0.05f) scale = 0.05f;
    float cx = (a.x + b.x) * 0.5f + ox * pw * 0.5f;
    float cy = (a.y + b.y) * 0.5f + oy * ph * 0.5f;
    float hw = pw * 0.5f * scale * aspect;
    float hh = ph * 0.5f * scale;
    float rad = rot * 0.01745329252f;
    float cs = std::cos(rad);
    float sn = std::sin(rad);
    float xs[4] = {-hw, hw, hw, -hw};
    float ys[4] = {-hh, -hh, hh, hh};
    for (int i = 0; i < 4; i++) {
        q[i].x = cx + xs[i] * cs - ys[i] * sn;
        q[i].y = cy + xs[i] * sn + ys[i] * cs;
    }
}

#ifdef HAVE_FFMPEG
static float ShownAspect(int clip) {
    if (clip >= 0) {
        if (playingClip == clip && player && player->width > 0 && player->height > 0)
            return (float)player->width / (float)player->height;
        if (playingClipB == clip && playerB && playerB->width > 0 && playerB->height > 0)
            return (float)playerB->width / (float)playerB->height;
        if (playingClipUnder == clip && playerUnder && playerUnder->width > 0 && playerUnder->height > 0)
            return (float)playerUnder->width / (float)playerUnder->height;
    }
    if (clip >= 0 && clip < (int)clips.size()) {
        int mid = clips[clip].mediaId;
        if (mid >= 0 && mid < (int)media.size()) {
            if (media[mid].thumbW > 0 && media[mid].thumbH > 0)
                return (float)media[mid].thumbW / (float)media[mid].thumbH;
            if (media[mid].aspect > 0.01f) return media[mid].aspect;
        }
    }
    return 16.0f / 9.0f;
}

static float XformEffectScale(int clip, int top) {
    if (clip < 0 || fxOther < 0 || fxActiveId.empty() || top < 0) return 1.0f;
    int a = -1, b = -1;
    float prog = 0;
    std::string id;
    if (!TransitionForClip(top, currentTime, a, b, prog, id)) return 1.0f;
    FxLook look;
    FxEvaluate(id.c_str(), prog, look);
    if (clip == a) return look.scaleA;
    if (clip == b) return look.scaleB;
    return 1.0f;
}

static bool XformOnScreen(int clip, int top, int plate) {
    if (clip < 0 || clip >= (int)clips.size()) return false;
    if (clip == top || clip == plate || clip == gsDrawClip || clip == playingClip) return true;
    if (fxOther >= 0 && clip == playingClipB) return true;
    for (int i = 0; i < bgShowN; i++) if (bgShow[i] == clip) return true;
    return false;
}

struct XformGeom {
    ImVec2 q[4];
    ImVec2 box0, box1;
    ImVec2 center, top, right, rot;
    float hw = 0, hh = 0;
    bool ok = false;
};

static XformGeom MakeXformGeom(int clip, int top) {
    XformGeom g;
    if (!previewFrameValid || clip < 0 || clip >= (int)clips.size()) return g;
    if (clip == gsDrawClip) {
        g.box0 = fitValid ? fitA : previewFrameMin;
        g.box1 = fitValid ? fitB : previewFrameMax;
    } else {
        FitAspectBox(previewFrameMin, previewFrameMax, ShownAspect(clip), g.box0, g.box1);
    }
    const Clip& c = clips[clip];
    PictureCorners(g.box0, g.box1, c.gsX, c.gsY, c.gsScale * XformEffectScale(clip, top), c.gsAspect, c.gsRot, g.q);
    g.center = ImVec2((g.q[0].x + g.q[2].x) * 0.5f, (g.q[0].y + g.q[2].y) * 0.5f);
    g.top = ImVec2((g.q[0].x + g.q[1].x) * 0.5f, (g.q[0].y + g.q[1].y) * 0.5f);
    g.right = ImVec2((g.q[1].x + g.q[2].x) * 0.5f, (g.q[1].y + g.q[2].y) * 0.5f);
    float dx = g.top.x - g.center.x;
    float dy = g.top.y - g.center.y;
    g.hh = std::sqrt(dx * dx + dy * dy);
    float len = g.hh < 1.0f ? 1.0f : g.hh;
    g.rot = ImVec2(g.top.x + dx / len * 18.0f, g.top.y + dy / len * 18.0f);
    float rx = g.right.x - g.center.x;
    float ry = g.right.y - g.center.y;
    g.hw = std::sqrt(rx * rx + ry * ry);
    g.ok = true;
    return g;
}

static int HitXform(int clip, int top, ImVec2 m) {
    XformGeom g = MakeXformGeom(clip, top);
    if (!g.ok) return 0;
    auto nearP = [&](ImVec2 p, float r) {
        float x = m.x - p.x, y = m.y - p.y;
        return x * x + y * y <= r * r;
    };
    if (nearP(g.rot, 14.0f)) return 3;
    if (nearP(g.q[2], 14.0f)) return 2;
    if (nearP(g.right, 12.0f)) return 4;
    float rad = clips[clip].gsRot * 0.01745329252f;
    float cs = std::cos(rad), sn = std::sin(rad);
    float dx = m.x - g.center.x, dy = m.y - g.center.y;
    float lx = dx * cs + dy * sn;
    float ly = -dx * sn + dy * cs;
    if (std::fabs(lx) <= g.hw + 4.0f && std::fabs(ly) <= g.hh + 4.0f) return 1;
    return 0;
}

static void DragXform(int clip, int top) {
    if (xformDrag == 0 || clip < 0 || clip >= (int)clips.size()) return;
    XformGeom g = MakeXformGeom(clip, top);
    if (!g.ok) return;
    Clip& c = clips[clip];
    ImGuiIO& pio = ImGui::GetIO();
    float pw = std::max(1.0f, g.box1.x - g.box0.x);
    float ph = std::max(1.0f, g.box1.y - g.box0.y);
    float rad = c.gsRot * 0.01745329252f;
    float cs = std::cos(rad), sn = std::sin(rad);
    if (xformDrag == 1) {
        c.gsX += pio.MouseDelta.x / (pw * 0.5f);
        c.gsY += pio.MouseDelta.y / (ph * 0.5f);
    } else if (xformDrag == 2) {
        c.gsScale += (pio.MouseDelta.x * (cs - sn) + pio.MouseDelta.y * (sn + cs)) / std::max(40.0f, pw * 0.25f);
        c.gsFit = 2;
    } else if (xformDrag == 3) {
        float a0 = std::atan2((pio.MousePos.y - pio.MouseDelta.y) - g.center.y, (pio.MousePos.x - pio.MouseDelta.x) - g.center.x);
        float a1 = std::atan2(pio.MousePos.y - g.center.y, pio.MousePos.x - g.center.x);
        c.gsRot += (a1 - a0) * 57.2957795f;
    } else if (xformDrag == 4) {
        float along = pio.MouseDelta.x * cs + pio.MouseDelta.y * sn;
        c.gsAspect += along / (pw * 0.5f * std::max(0.05f, c.gsScale));
        c.gsFit = 2;
    }
    if (c.gsX < -1.0f) c.gsX = -1.0f;
    if (c.gsX > 1.0f) c.gsX = 1.0f;
    if (c.gsY < -1.0f) c.gsY = -1.0f;
    if (c.gsY > 1.0f) c.gsY = 1.0f;
    if (c.gsScale < 0.05f) c.gsScale = 0.05f;
    if (c.gsScale > 4.0f) c.gsScale = 4.0f;
    if (c.gsAspect < 0.25f) c.gsAspect = 0.25f;
    if (c.gsAspect > 4.0f) c.gsAspect = 4.0f;
    while (c.gsRot > 180.0f) c.gsRot -= 360.0f;
    while (c.gsRot < -180.0f) c.gsRot += 360.0f;
}

static void DrawXformChrome(int top, int plate) {
    if (!xformOpen || !XformOnScreen(xformClip, top, plate)) return;
    XformGeom g = MakeXformGeom(xformClip, top);
    if (!g.ok) return;
    ImGuiIO& io = ImGui::GetIO();
    float sx0 = std::max(0.0f, previewFrameMin.x);
    float sy0 = std::max(0.0f, previewFrameMin.y);
    float sx1 = std::min(io.DisplaySize.x, previewFrameMax.x);
    float sy1 = std::min(io.DisplaySize.y, previewFrameMax.y);
    if (sx1 - sx0 < 1.0f || sy1 - sy0 < 1.0f) return;
    if (pglUseProgram) pglUseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glLineWidth(1.5f);
    glColor3f(0.95f, 0.82f, 0.25f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(g.q[0].x, g.q[0].y);
    glVertex2f(g.q[1].x, g.q[1].y);
    glVertex2f(g.q[2].x, g.q[2].y);
    glVertex2f(g.q[3].x, g.q[3].y);
    glEnd();
    glBegin(GL_LINES);
    glVertex2f(g.top.x, g.top.y);
    glVertex2f(g.rot.x, g.rot.y);
    glEnd();
    auto knob = [](ImVec2 p, float r) {
        glBegin(GL_QUADS);
        glVertex2f(p.x - r, p.y - r);
        glVertex2f(p.x + r, p.y - r);
        glVertex2f(p.x + r, p.y + r);
        glVertex2f(p.x - r, p.y + r);
        glEnd();
    };
    glColor3f(1.0f, 1.0f, 1.0f);
    knob(g.q[2], 5.0f);
    knob(g.right, 4.5f);
    glColor3f(0.35f, 0.82f, 1.0f);
    knob(g.rot, 5.0f);
    glLineWidth(1.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_SCISSOR_TEST);
}
#endif

void BlitVideo(ImVec2 p0, ImVec2 p1, GLuint tex, int vw, int vh, ImVec2 clip0, ImVec2 clip1,
               float ox = 0.0f, float oy = 0.0f, float scale = 1.0f, float aspect = 1.0f, float rot = 0.0f, bool blendAlpha = false, float cover = 1.0f) {
    if (!tex || vw <= 0 || vh <= 0) return;
    if (cover < 0.0f) cover = 0.0f;
    if (cover > 1.0f) cover = 1.0f;
    if (cover <= 0.001f) return;
    float areaW = p1.x - p0.x;
    float areaH = p1.y - p0.y;
    if (areaW < 4 || areaH < 4) return;
    float picAspect = (float)vw / (float)vh;
    float areaAspect = areaW / areaH;
    ImVec2 a = p0, b = p1;
    if (picAspect > areaAspect) {
        float h = areaW / picAspect;
        float off = (areaH - h) * 0.5f;
        a.y += off; b.y -= off;
    } else {
        float w = areaH * picAspect;
        float off = (areaW - w) * 0.5f;
        a.x += off; b.x -= off;
    }
    if (recordFit) { fitA = a; fitB = b; fitValid = true; }
    bool moved = std::fabs(ox) > 0.0005f || std::fabs(oy) > 0.0005f || std::fabs(scale - 1.0f) > 0.0005f
        || std::fabs(aspect - 1.0f) > 0.0005f || std::fabs(rot) > 0.05f;
    ImVec2 quad[4];
    if (moved) PictureCorners(a, b, ox, oy, scale, aspect, rot, quad);
    ImGuiIO& io = ImGui::GetIO();
    float sx0 = std::max(moved ? clip0.x : a.x, std::max(0.0f, clip0.x));
    float sy0 = std::max(moved ? clip0.y : a.y, std::max(0.0f, clip0.y));
    float sx1 = std::min(moved ? clip1.x : b.x, std::min(io.DisplaySize.x, clip1.x));
    float sy1 = std::min(moved ? clip1.y : b.y, std::min(io.DisplaySize.y, clip1.y));
    if (previewFrameValid) {
        sx0 = std::max(sx0, previewFrameMin.x);
        sy0 = std::max(sy0, previewFrameMin.y);
        sx1 = std::min(sx1, previewFrameMax.x);
        sy1 = std::min(sy1, previewFrameMax.y);
    }
    if (sx1 - sx0 < 1.0f || sy1 - sy0 < 1.0f) return;

    bool graded = activeGrade && BindGrade(*activeGrade, vw, vh, cover);
    if (!graded && pglUseProgram) pglUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    bool useBlend = blendAlpha || cover < 0.999f;
    if (useBlend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (!graded) glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1.f, 1.f, 1.f, cover);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glBegin(GL_QUADS);
    if (moved) {
        glTexCoord2f(0, 0); glVertex2f(quad[0].x, quad[0].y);
        glTexCoord2f(1, 0); glVertex2f(quad[1].x, quad[1].y);
        glTexCoord2f(1, 1); glVertex2f(quad[2].x, quad[2].y);
        glTexCoord2f(0, 1); glVertex2f(quad[3].x, quad[3].y);
    } else {
        glTexCoord2f(0, 0); glVertex2f(a.x, a.y);
        glTexCoord2f(1, 0); glVertex2f(b.x, a.y);
        glTexCoord2f(1, 1); glVertex2f(b.x, b.y);
        glTexCoord2f(0, 1); glVertex2f(a.x, b.y);
    }
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_TEXTURE_2D);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    if (graded && pglUseProgram) pglUseProgram(0);
}

static GLuint gTransTexA = 0;
static GLuint gTransTexB = 0;
static std::vector<unsigned char> gPlateA, gPlateB;
static int gPlateAw = 0, gPlateAh = 0, gPlateBw = 0, gPlateBh = 0;

#ifdef _WIN32
static bool DecodePlateRgba(const unsigned char* bytes, int nbytes, std::vector<unsigned char>& px, int& w, int& h) {
    px.clear();
    w = 0;
    h = 0;
    if (!bytes || nbytes < 32) return false;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ownCo = (co == S_OK);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)nbytes);
    if (!mem) {
        if (ownCo) CoUninitialize();
        return false;
    }
    void* locked = GlobalLock(mem);
    if (locked) memcpy(locked, bytes, (size_t)nbytes);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
        GlobalFree(mem);
        if (ownCo) CoUninitialize();
        return false;
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) {
        bool bgra = false;
        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            bgra = true;
            hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        UINT pw = 0, ph = 0;
        if (SUCCEEDED(hr)) hr = conv->GetSize(&pw, &ph);
        if (SUCCEEDED(hr) && pw >= 2 && ph >= 2 && pw <= 4096 && ph <= 4096) {
            px.resize((size_t)pw * ph * 4);
            hr = conv->CopyPixels(nullptr, pw * 4, (UINT)px.size(), px.data());
            if (SUCCEEDED(hr)) {
                if (bgra) {
                    for (size_t i = 0; i + 3 < px.size(); i += 4) {
                        unsigned char tmp = px[i];
                        px[i] = px[i + 2];
                        px[i + 2] = tmp;
                    }
                }
                w = (int)pw;
                h = (int)ph;
                ok = true;
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    stream->Release();
    if (ownCo) CoUninitialize();
    if (!ok) px.clear();
    return ok;
}
#endif

static void UploadTransTex(GLuint& id, const unsigned char* rgba, int w, int h) {
    if (!rgba || w < 2 || h < 2) return;
    if (!id) glGenTextures(1, &id);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint unpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
}

static void EnsureTransPlates() {
#ifdef _WIN32
    static int done = 0;
    if (done) return;
    done = 1;
    int na = 0, nb = 0;
    const unsigned char* ba = TransPlateA(&na);
    const unsigned char* bb = TransPlateB(&nb);
    std::vector<unsigned char> pa, pb;
    int wa = 0, ha = 0, wb = 0, hb = 0;
    if (!DecodePlateRgba(ba, na, pa, wa, ha)) return;
    if (!DecodePlateRgba(bb, nb, pb, wb, hb)) return;
    UploadTransTex(gTransTexA, pa.data(), wa, ha);
    UploadTransTex(gTransTexB, pb.data(), wb, hb);
    gPlateA.swap(pa);
    gPlateB.swap(pb);
    gPlateAw = wa;
    gPlateAh = ha;
    gPlateBw = wb;
    gPlateBh = hb;
#else
    (void)0;
#endif
}

static void DrawTransitionCard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* id) {
    if (!dl || !id) return;
    float span = 3.0f;
    float shown = std::fmod((float)ImGui::GetTime(), span * 2.0f);
    if (shown < 0.0f) shown += span * 2.0f;
    if (shown > span) shown = span * 2.0f - shown;
    float progress = shown / span;
    FxLook look;
    FxEvaluate(id, progress, look);
    if (look.shape != 0 && !gPlateA.empty() && !gPlateB.empty()) {
        const int tw = 192, th = 108;
        static std::vector<unsigned char> pix;
        pix.resize((size_t)tw * (size_t)th * 4);
        FxPaint(look, pix.data(), tw, th, gPlateA.data(), gPlateAw, gPlateAh, gPlateB.data(), gPlateBw, gPlateBh);
        struct Slot { const char* id; GLuint tex; };
        static Slot slots[64];
        GLuint tex = 0;
        for (int i = 0; i < 64; i++) {
            if (slots[i].id == id) { tex = slots[i].tex; break; }
            if (!slots[i].id) {
                slots[i].id = id;
                glGenTextures(1, &slots[i].tex);
                tex = slots[i].tex;
                break;
            }
        }
        if (tex) {
            GLint prev = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
            dl->PushClipRect(p0, p1, true);
            dl->AddImage((ImTextureID)(intptr_t)tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
            dl->AddRect(p0, p1, IM_COL32(90, 96, 110, 255), 2.0f);
            dl->PopClipRect();
            return;
        }
    }
    float wb = look.alphaB;
    if (wb < 0.0f) wb = 0.0f;
    if (wb > 1.0f) wb = 1.0f;
    dl->PushClipRect(p0, p1, true);
    if (gTransTexA && gTransTexB) {
        auto zoomUv = [](float scale, ImVec2& u0, ImVec2& u1) {
            if (scale < 1.0f) scale = 1.0f;
            float half = 0.5f / scale;
            u0 = ImVec2(0.5f - half, 0.5f - half);
            u1 = ImVec2(0.5f + half, 0.5f + half);
        };
        ImVec2 ua0, ua1, ub0, ub1;
        zoomUv(look.scaleA, ua0, ua1);
        zoomUv(look.scaleB, ub0, ub1);
        ImTextureID texA = (ImTextureID)(intptr_t)gTransTexA;
        ImTextureID texB = (ImTextureID)(intptr_t)gTransTexB;
        dl->AddImage(texA, p0, p1, ua0, ua1, IM_COL32_WHITE);
        if (look.wipe == 0) {
            int ba = (int)(wb * 255.0f);
            if (ba > 0) dl->AddImage(texB, p0, p1, ub0, ub1, IM_COL32(255, 255, 255, ba));
        } else if (wb > 0.001f) {
            ImVec2 c0 = p0, c1 = p1;
            if (look.wipe == 1) c1.x = p0.x + (p1.x - p0.x) * wb;
            else if (look.wipe == 2) c0.x = p1.x - (p1.x - p0.x) * wb;
            else if (look.wipe == 3) c1.y = p0.y + (p1.y - p0.y) * wb;
            else c0.y = p1.y - (p1.y - p0.y) * wb;
            if (c1.x - c0.x >= 1.0f && c1.y - c0.y >= 1.0f) {
                dl->PushClipRect(c0, c1, true);
                dl->AddImage(texB, p0, p1, ub0, ub1, IM_COL32_WHITE);
                dl->PopClipRect();
            }
        }
    } else {
        dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, 255), 2.0f);
        auto box = [&](float sc, ImVec2& a, ImVec2& b) {
            float cx = (p0.x + p1.x) * 0.5f;
            float cy = (p0.y + p1.y) * 0.5f;
            float hw = (p1.x - p0.x) * 0.5f * sc;
            float hh = (p1.y - p0.y) * 0.5f * sc;
            a = ImVec2(cx - hw, cy - hh);
            b = ImVec2(cx + hw, cy + hh);
        };
        ImVec2 a0, a1, b0, b1;
        box(look.scaleA, a0, a1);
        box(look.scaleB, b0, b1);
        if (look.wipe == 0) {
            int aa = (int)((1.0f - wb) * 255.0f);
            if (aa < 0) aa = 0;
            if (aa > 255) aa = 255;
            dl->AddRectFilled(a0, a1, IM_COL32(196, 122, 64, aa));
            int ba = (int)(wb * 255.0f);
            if (ba > 0) dl->AddRectFilled(b0, b1, IM_COL32(70, 148, 214, ba));
        } else {
            dl->AddRectFilled(a0, a1, IM_COL32(196, 122, 64, 255));
            ImVec2 c0 = p0, c1 = p1;
            if (look.wipe == 1) c1.x = p0.x + (p1.x - p0.x) * wb;
            else if (look.wipe == 2) c0.x = p1.x - (p1.x - p0.x) * wb;
            else if (look.wipe == 3) c1.y = p0.y + (p1.y - p0.y) * wb;
            else c0.y = p1.y - (p1.y - p0.y) * wb;
            dl->PushClipRect(c0, c1, true);
            dl->AddRectFilled(b0, b1, IM_COL32(70, 148, 214, 255));
            dl->PopClipRect();
        }
    }
    if (look.dip > 0.001f) {
        float k = look.dip > 1.0f ? 1.0f : look.dip;
        int a = (int)(k * 255.0f);
        int r = (int)(look.dipR * 255.0f);
        int g = (int)(look.dipG * 255.0f);
        int b = (int)(look.dipB * 255.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(r, g, b, a));
    }
    dl->AddRect(p0, p1, IM_COL32(90, 96, 110, 255), 2.0f);
    dl->PopClipRect();
}

static bool DrawShapePreview(ImVec2 p0, ImVec2 p1, GLuint texA, int aw, int ah, GLuint texB, int bw, int bh, const FxLook& look) {
    if (look.shape == 0 || !texA || !texB || aw < 1 || ah < 1 || bw < 1 || bh < 1) return false;
    if ((long long)aw * ah > 3840LL * 2160LL || (long long)bw * bh > 3840LL * 2160LL) return false;
    static std::vector<unsigned char> pixA, pixB, out;
    pixA.resize((size_t)aw * (size_t)ah * 4);
    pixB.resize((size_t)bw * (size_t)bh * 4);
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLint pack = 4, unpack = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int n = 0; n < 8 && glGetError() != GL_NO_ERROR; n++) {}
    glBindTexture(GL_TEXTURE_2D, texA);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixA.data());
    glBindTexture(GL_TEXTURE_2D, texB);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixB.data());
    bool readOk = glGetError() == GL_NO_ERROR;
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    if (!readOk) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        return false;
    }
    int dw = aw;
    int dh = ah;
    if (dw > 960) {
        dh = (int)((double)ah * 960.0 / (double)aw + 0.5);
        dw = 960;
    }
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    dw &= ~1;
    dh &= ~1;
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    out.resize((size_t)dw * (size_t)dh * 4);
    FxPaint(look, out.data(), dw, dh, pixA.data(), aw, ah, pixB.data(), bw, bh);
    static GLuint mixTex = 0;
    static int mixW = 0, mixH = 0;
    if (!mixTex) glGenTextures(1, &mixTex);
    glBindTexture(GL_TEXTURE_2D, mixTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (mixW != dw || mixH != dh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        mixW = dw;
        mixH = dh;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    ImGuiIO& io = ImGui::GetIO();
    float areaW = p1.x - p0.x;
    float areaH = p1.y - p0.y;
    if (areaW < 4.0f || areaH < 4.0f) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        return true;
    }
    ImVec2 box0 = p0, box1 = p1;
    float aspect = (float)dw / (float)dh;
    float areaAspect = areaW / areaH;
    if (aspect > areaAspect) {
        float hh = areaW / aspect;
        float off = (areaH - hh) * 0.5f;
        box0.y += off;
        box1.y -= off;
    } else {
        float ww = areaH * aspect;
        float off = (areaW - ww) * 0.5f;
        box0.x += off;
        box1.x -= off;
    }
    if (recordFit) { fitA = box0; fitB = box1; fitValid = true; }
    float sx0 = std::max(p0.x, 0.0f);
    float sy0 = std::max(p0.y, 0.0f);
    float sx1 = std::min(p1.x, io.DisplaySize.x);
    float sy1 = std::min(p1.y, io.DisplaySize.y);
    if (previewFrameValid) {
        sx0 = std::max(sx0, previewFrameMin.x);
        sy0 = std::max(sy0, previewFrameMin.y);
        sx1 = std::min(sx1, previewFrameMax.x);
        sy1 = std::min(sy1, previewFrameMax.y);
    }
    if (pglUseProgram) pglUseProgram(0);
    GLboolean blendOn = glIsEnabled(GL_BLEND);
    GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean texOn = glIsEnabled(GL_TEXTURE_2D);
    GLboolean depthOn = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    if (sx1 - sx0 >= 1.0f && sy1 - sy0 >= 1.0f) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glBindTexture(GL_TEXTURE_2D, mixTex);
        glColor4f(1.f, 1.f, 1.f, 1.f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(box0.x, box0.y);
        glTexCoord2f(1, 0); glVertex2f(box1.x, box0.y);
        glTexCoord2f(1, 1); glVertex2f(box1.x, box1.y);
        glTexCoord2f(0, 1); glVertex2f(box0.x, box1.y);
        glEnd();
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
    }
    if (blendOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (scissorOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (texOn) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    if (depthOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    if (pglUseProgram) pglUseProgram(0);
    return true;
}

void BlitFxPair(ImVec2 p0, ImVec2 p1, GLuint texA, int aw, int ah, GLuint texB, int bw, int bh, const FxLook& look, const Clip* ca = nullptr, const Clip* cb = nullptr) {
    if (look.shape != 0 && DrawShapePreview(p0, p1, texA, aw, ah, texB, bw, bh, look)) return;
    if (!texA || aw <= 0 || ah <= 0) return;
    ImGuiIO& io = ImGui::GetIO();
    float areaW = p1.x - p0.x;
    float areaH = p1.y - p0.y;
    if (areaW < 4 || areaH < 4) return;
    auto letterbox = [&](int tw, int th, ImVec2& a, ImVec2& b) {
        a = p0;
        b = p1;
        if (tw <= 0 || th <= 0) return;
        float aspect = (float)tw / (float)th;
        float areaAspect = areaW / areaH;
        if (aspect > areaAspect) {
            float h = areaW / aspect;
            float off = (areaH - h) * 0.5f;
            a.y += off; b.y -= off;
        } else {
            float w = areaH * aspect;
            float off = (areaW - w) * 0.5f;
            a.x += off; b.x -= off;
        }
    };
    ImVec2 boxA0, boxA1, boxB0, boxB1;
    letterbox(aw, ah, boxA0, boxA1);
    bool haveB = texB && bw > 0 && bh > 0 && look.alphaB > 0.001f;
    if (haveB) letterbox(bw, bh, boxB0, boxB1);
    if (recordFit) { fitA = boxA0; fitB = boxA1; fitValid = true; }
    float sx0 = std::max(p0.x, 0.0f);
    float sy0 = std::max(p0.y, 0.0f);
    float sx1 = std::min(p1.x, io.DisplaySize.x);
    float sy1 = std::min(p1.y, io.DisplaySize.y);
    if (previewFrameValid) {
        sx0 = std::max(sx0, previewFrameMin.x);
        sy0 = std::max(sy0, previewFrameMin.y);
        sx1 = std::min(sx1, previewFrameMax.x);
        sy1 = std::min(sy1, previewFrameMax.y);
    }
    if (sx1 - sx0 < 1.0f || sy1 - sy0 < 1.0f) return;
    auto quad = [&](GLuint tex, int tw, int th, ImVec2 a, ImVec2 b, float alpha, float effectScale, const Clip* clip) {
        float sc = effectScale * (clip ? clip->gsScale : 1.0f);
        float asp = clip ? clip->gsAspect : 1.0f;
        float ox = clip ? clip->gsX : 0.0f;
        float oy = clip ? clip->gsY : 0.0f;
        float rot = clip ? clip->gsRot : 0.0f;
        ImVec2 q[4];
        PictureCorners(a, b, ox, oy, sc, asp, rot, q);
        bool graded = activeGrade && BindGrade(*activeGrade, tw, th, alpha);
        if (!graded && pglUseProgram) pglUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glColor4f(1.f, 1.f, 1.f, alpha);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(q[0].x, q[0].y);
        glTexCoord2f(1, 0); glVertex2f(q[1].x, q[1].y);
        glTexCoord2f(1, 1); glVertex2f(q[2].x, q[2].y);
        glTexCoord2f(0, 1); glVertex2f(q[3].x, q[3].y);
        glEnd();
        if (graded && pglUseProgram) pglUseProgram(0);
    };
    if (pglUseProgram) pglUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
    glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_BLEND);
    bool revealed = false;
    if (haveB && look.wipe == 0 && bgShowN > 0 && previewFrameValid) {
        static int stencilBits = -1;
        if (stencilBits < 0) {
            stencilBits = 0;
            glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
        }
        int fbW = (int)io.DisplaySize.x;
        int fbH = (int)io.DisplaySize.y;
        int cx = (int)std::floor(std::max(0.0f, previewFrameMin.x));
        int top = (int)std::floor(std::max(0.0f, previewFrameMin.y));
        int right = (int)std::ceil(std::min((float)fbW, previewFrameMax.x));
        int bot = (int)std::ceil(std::min((float)fbH, previewFrameMax.y));
        int cy = fbH - bot;
        int cw = right - cx;
        int ch = bot - top;
        static GLuint revealTex = 0;
        static int revealW = 0, revealH = 0;
        if (stencilBits > 0 && cw > 1 && ch > 1 && cy >= 0 && cx + cw <= fbW && cy + ch <= fbH) {
            if (!revealTex) glGenTextures(1, &revealTex);
            glBindTexture(GL_TEXTURE_2D, revealTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (revealW != cw || revealH != ch) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                revealW = cw;
                revealH = ch;
            }
            for (int n = 0; n < 8 && glGetError() != GL_NO_ERROR; n++) {}
            glReadBuffer(GL_BACK);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cx, cy, cw, ch);
            if (glGetError() == GL_NO_ERROR) revealed = true;
        }
        if (revealed) {
            float wa = 1.0f - look.alphaB;
            if (wa < 0.0f) wa = 0.0f;
            if (wa > 1.0f) wa = 1.0f;
            if (wa > 0.001f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                quad(texA, aw, ah, boxA0, boxA1, wa, look.scaleA, ca);
            }
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
            if (pglUseProgram) pglUseProgram(0);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            float sc = look.scaleB * (cb ? cb->gsScale : 1.0f);
            float asp = cb ? cb->gsAspect : 1.0f;
            float ox = cb ? cb->gsX : 0.0f;
            float oy = cb ? cb->gsY : 0.0f;
            float rot = cb ? cb->gsRot : 0.0f;
            ImVec2 bq[4];
            PictureCorners(boxB0, boxB1, ox, oy, sc, asp, rot, bq);
            glColor4f(1.f, 1.f, 1.f, 1.f);
            glBegin(GL_QUADS);
            glVertex2f(bq[0].x, bq[0].y);
            glVertex2f(bq[1].x, bq[1].y);
            glVertex2f(bq[2].x, bq[2].y);
            glVertex2f(bq[3].x, bq[3].y);
            glEnd();
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, revealTex);
            if (pglUseProgram) pglUseProgram(0);
            glDisable(GL_BLEND);
            float rx0 = (float)cx;
            float rx1 = (float)(cx + cw);
            float ryTop = (float)(fbH - (cy + ch));
            float ryBot = (float)(fbH - cy);
            glColor4f(1.f, 1.f, 1.f, 1.f);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 1); glVertex2f(rx0, ryTop);
            glTexCoord2f(1, 1); glVertex2f(rx1, ryTop);
            glTexCoord2f(1, 0); glVertex2f(rx1, ryBot);
            glTexCoord2f(0, 0); glVertex2f(rx0, ryBot);
            glEnd();
            glDisable(GL_BLEND);
            quad(texA, aw, ah, boxA0, boxA1, 1.0f, look.scaleA, ca);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            quad(texB, bw, bh, boxB0, boxB1, look.alphaB, look.scaleB, cb);
            glDisable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
        }
    }
    if (!revealed) {
        quad(texA, aw, ah, boxA0, boxA1, 1.0f, look.scaleA, ca);
        if (haveB) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (look.wipe != 0) {
                float edge = look.alphaB;
                if (edge < 0) edge = 0;
                if (edge > 1) edge = 1;
                float x0 = sx0, y0 = sy0, x1 = sx1, y1 = sy1;
                float spanX = p1.x - p0.x;
                float spanY = p1.y - p0.y;
                if (look.wipe == 1) x1 = p0.x + spanX * edge;
                else if (look.wipe == 2) x0 = p1.x - spanX * edge;
                else if (look.wipe == 3) y1 = p0.y + spanY * edge;
                else if (look.wipe == 4) y0 = p1.y - spanY * edge;
                x0 = std::max(x0, sx0);
                y0 = std::max(y0, sy0);
                x1 = std::min(x1, sx1);
                y1 = std::min(y1, sy1);
                if (x1 - x0 >= 1.0f && y1 - y0 >= 1.0f) {
                    glScissor((int)x0, (int)(io.DisplaySize.y - y1), std::max(1, (int)(x1 - x0)), std::max(1, (int)(y1 - y0)));
                    quad(texB, bw, bh, boxB0, boxB1, 1.0f, look.scaleB, cb);
                }
            } else {
                quad(texB, bw, bh, boxB0, boxB1, look.alphaB, look.scaleB, cb);
            }
        }
    }
    if (look.dip > 0.001f) {
        float k = look.dip > 1.0f ? 1.0f : look.dip;
        if (pglUseProgram) pglUseProgram(0);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
        glColor4f(look.dipR, look.dipG, look.dipB, k);
        glBegin(GL_QUADS);
        glVertex2f(sx0, sy0);
        glVertex2f(sx1, sy0);
        glVertex2f(sx1, sy1);
        glVertex2f(sx0, sy1);
        glEnd();
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glColor3f(1.f, 1.f, 1.f);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
}

static void DrawBackgroundClip(int clip, ImVec2 view0, ImVec2 view1) {
    if (clip < 0 || clip >= (int)clips.size()) return;
    GLuint tex = 0;
    int tw = 0, th = 0;
#ifdef HAVE_FFMPEG
    auto take = [&](VideoPlayer* p, int id) -> bool {
        if (id == clip && p && p->hasFrame && p->texture && p->width > 0 && p->height > 0) {
            tex = p->texture;
            tw = p->width;
            th = p->height;
            return true;
        }
        return false;
    };
    if (!take(playerUnder, playingClipUnder) && !take(playerB, playingClipB) && !take(player, playingClip)) {
        int mid = clips[clip].mediaId;
        if (mid >= 0 && mid < (int)media.size() && media[mid].thumb) {
            tex = media[mid].thumb;
            tw = media[mid].thumbW;
            th = media[mid].thumbH;
        }
    }
#else
    int mid = clips[clip].mediaId;
    if (mid >= 0 && mid < (int)media.size() && media[mid].thumb) {
        tex = media[mid].thumb;
        tw = media[mid].thumbW;
        th = media[mid].thumbH;
    }
#endif
    if (!tex || tw < 1 || th < 1) return;
    const Clip& c = clips[clip];
    BusSegment grade = ClipGradeOf(c);
    if (const BusSegment* bs = BusAt(currentTime)) AddGrade(grade, *bs);
    static BusSegment layerGrade;
    layerGrade = grade;
    const BusSegment* saved = activeGrade;
    activeGrade = BusActive(layerGrade) ? &layerGrade : nullptr;
    bool rec = recordFit;
    recordFit = false;
    bool alpha = false;
    int mid = c.mediaId;
    if (mid >= 0 && mid < (int)media.size()) alpha = SolidHasAlpha(media[mid].path);
    float cover = OpacityAt(c, currentTime - c.startTime);
    BlitVideo(view0, view1, tex, tw, th, view0, view1, c.gsX, c.gsY, c.gsScale, c.gsAspect, c.gsRot, alpha, cover);
    recordFit = rec;
    activeGrade = saved;
}

void BlitKeyed(ImVec2 a, ImVec2 b, GLuint tex, float sim, float soft, float spill, float ox, float oy, float scale, float opacity, float aspect, float rot) {
    if (!tex || b.x - a.x < 2 || b.y - a.y < 2 || scale < 0.05f) return;
    ImVec2 q[4];
    PictureCorners(a, b, ox, oy, scale, aspect, rot, q);
    bool keyed = BindKey(sim, soft, spill, opacity);
    if (!keyed && pglUseProgram) pglUseProgram(0);
    ImGuiIO& io = ImGui::GetIO();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (previewFrameValid) {
        float sx0 = std::max(0.0f, previewFrameMin.x);
        float sy0 = std::max(0.0f, previewFrameMin.y);
        float sx1 = std::min(io.DisplaySize.x, previewFrameMax.x);
        float sy1 = std::min(io.DisplaySize.y, previewFrameMax.y);
        if (sx1 > sx0 && sy1 > sy0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
        }
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1, 1, 1, opacity);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(q[0].x, q[0].y);
    glTexCoord2f(1, 0); glVertex2f(q[1].x, q[1].y);
    glTexCoord2f(1, 1); glVertex2f(q[2].x, q[2].y);
    glTexCoord2f(0, 1); glVertex2f(q[3].x, q[3].y);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    if (pglUseProgram) pglUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
}

static void DrawGlyphQuad(const ImFontGlyph* g, float x0, float y0, float x1, float y1) {
    float du = 0.0f, dv = 0.0f;
    if (fontAtlasW > 1 && fontAtlasH > 1) {
        du = 0.5f / (float)fontAtlasW;
        dv = 0.5f / (float)fontAtlasH;
        if (g->U1 - g->U0 < du * 3.0f) du = 0.0f;
        if (g->V1 - g->V0 < dv * 3.0f) dv = 0.0f;
    }
    float u0 = g->U0 + du, v0 = g->V0 + dv, u1 = g->U1 - du, v1 = g->V1 - dv;
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
}

static void DrawLineGL(ImFont* font, float size, float x, float y, const std::string& text) {
    float scale = size / font->FontSize;
    const char* p = text.c_str();
    while (*p) {
        unsigned cp = DecodeUtf8(p);
        const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
        if (!g) { x += size * 0.5f; continue; }
        DrawGlyphQuad(g, x + g->X0 * scale, y + g->Y0 * scale, x + g->X1 * scale, y + g->Y1 * scale);
        x += g->AdvanceX * scale;
    }
}

void DrawPreviewSubs(ImVec2 pic0, ImVec2 pic1) {
    std::vector<int> ids;
    ActiveSubs(currentTime, ids);
    if (ids.empty()) return;
    ImFont* font = subtitleFont ? subtitleFont : ImGui::GetFont();
    if (!font || pic1.x - pic0.x < 20) return;
    float size = std::max(16.0f, (pic1.y - pic0.y) * 0.055f) * std::max(0.3f, subStyle.fontScale);
    float maxW = (pic1.x - pic0.x) * 0.82f;
    float lineH = size * 1.18f;
    ImGuiIO& io = ImGui::GetIO();
    if (pglUseProgram) pglUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)io.Fonts->TexID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (previewFrameValid) {
        glEnable(GL_SCISSOR_TEST);
        float sx0 = std::max(0.0f, previewFrameMin.x);
        float sy0 = std::max(0.0f, previewFrameMin.y);
        float sx1 = std::min(io.DisplaySize.x, previewFrameMax.x);
        float sy1 = std::min(io.DisplaySize.y, previewFrameMax.y);
        glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
    }
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    float edge = subStyle.border ? std::max(1.0f, subStyle.borderPx * (size / 48.0f)) : 0.0f;
    float yBottom = pic1.y - (pic1.y - pic0.y) * 0.06f;
    for (int n = (int)ids.size() - 1; n >= 0; n--) {
        std::vector<std::string> lines;
        WrapBalanced(font, size, subs[ids[n]].text, maxW, lines);
        if (lines.empty()) continue;
        float blockH = lineH * (float)lines.size();
        float y = yBottom - blockH;
        float widest = 0;
        for (const auto& ln : lines) widest = std::max(widest, TextWidth(font, size, ln));
        if (subStyle.background) {
            glDisable(GL_TEXTURE_2D);
            glColor4f(subStyle.bg[0], subStyle.bg[1], subStyle.bg[2], Clamp01(subStyle.bgAlpha));
            float x0 = (pic0.x + pic1.x - widest) * 0.5f - size * 0.35f;
            float x1 = (pic0.x + pic1.x + widest) * 0.5f + size * 0.35f;
            float y0 = y - size * 0.12f;
            float y1 = y + blockH + size * 0.15f;
            glBegin(GL_QUADS);
            glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
            glEnd();
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)io.Fonts->TexID);
        }
        float yy = y;
        for (const auto& raw : lines) {
            std::string ln = HasHebrew(raw) ? ReverseUtf8(raw) : raw;
            float x = (pic0.x + pic1.x - TextWidth(font, size, ln)) * 0.5f;
            if (edge > 0.2f) {
                glColor4f(subStyle.edge[0], subStyle.edge[1], subStyle.edge[2], 1);
                glBegin(GL_QUADS);
                for (int k = 0; k < 16; k++) {
                    float ang = (float)k * 6.2831853f / 16.0f;
                    DrawLineGL(font, size, x + std::cos(ang) * edge, yy + std::sin(ang) * edge, ln);
                }
                glEnd();
            }
            glColor4f(subStyle.text[0], subStyle.text[1], subStyle.text[2], 1);
            glBegin(GL_QUADS);
            DrawLineGL(font, size, x, yy, ln);
            glEnd();
            yy += lineH;
        }
        yBottom = y - size * 0.35f;
    }
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glColor3f(1, 1, 1);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
}

float SnapTime(int ignore, float proposed) {
    if (proposed < 0) proposed = 0;
    if (!snapEnabled) return proposed;
    float thresh = 10.0f / pixelsPerSecond;
    float best = proposed;
    float bestD = thresh;
    auto test = [&](float target) {
        if (target < -0.001f) return;
        float d = std::fabs(proposed - target);
        if (d < bestD) { bestD = d; best = std::max(0.0f, target); }
    };
    test(0.0f);
    test(currentTime);
    for (int i = 0; i < (int)clips.size(); i++) {
        if (i == ignore) continue;
        if (std::find(groupSkip.begin(), groupSkip.end(), i) != groupSkip.end()) continue;
        test(clips[i].startTime);
        test(clips[i].startTime + clips[i].duration);
    }
    return best;
}

float SnapStart(int ignore, float dur, float proposed) {
    if (proposed < 0) proposed = 0;
    if (!snapEnabled) return proposed;
    float thresh = 10.0f / pixelsPerSecond;
    float best = proposed;
    float bestD = thresh;
    auto test = [&](float targetStart) {
        if (targetStart < -0.001f) return;
        float d = std::fabs(proposed - targetStart);
        if (d < bestD) { bestD = d; best = std::max(0.0f, targetStart); }
    };
    test(0.0f);
    test(currentTime);
    test(currentTime - dur);
    for (int i = 0; i < (int)clips.size(); i++) {
        if (i == ignore) continue;
        if (std::find(groupSkip.begin(), groupSkip.end(), i) != groupSkip.end()) continue;
        float s = clips[i].startTime;
        float e = s + clips[i].duration;
        test(e);
        test(s);
        test(s - dur);
        test(e - dur);
    }
    return best;
}

static void ClearGroupDrag() {
    groupClipDrag.clear();
    groupSubDrag.clear();
    groupSkip.clear();
    groupAnchor = 0;
}

static void CaptureGroupDrag(int clipLeader, int subLeader) {
    ClearGroupDrag();
    if (clipLeader >= 0 && clipLeader < (int)clips.size())
        groupAnchor = clips[clipLeader].startTime;
    else if (subLeader >= 0 && subLeader < (int)subs.size())
        groupAnchor = subs[subLeader].start;
    for (int i : selection) {
        if (i < 0 || i >= (int)clips.size()) continue;
        groupClipDrag.push_back({i, clips[i].startTime});
        groupSkip.push_back(i);
    }
    if (clipLeader >= 0 && std::find(groupSkip.begin(), groupSkip.end(), clipLeader) == groupSkip.end()
        && clipLeader < (int)clips.size()) {
        groupClipDrag.push_back({clipLeader, clips[clipLeader].startTime});
        groupSkip.push_back(clipLeader);
    }
    for (int i : subSelection) {
        if (i < 0 || i >= (int)subs.size()) continue;
        groupSubDrag.push_back({i, subs[i].start});
    }
    if (subLeader >= 0 && subLeader < (int)subs.size()) {
        bool have = false;
        for (const auto& g : groupSubDrag) if (g.idx == subLeader) have = true;
        if (!have) groupSubDrag.push_back({subLeader, subs[subLeader].start});
    }
}

static void ApplyGroupDelta(float delta) {
    float earliest = 1e9f;
    for (const auto& g : groupClipDrag) earliest = std::min(earliest, g.orig);
    for (const auto& g : groupSubDrag) earliest = std::min(earliest, g.orig);
    if (earliest < 1e8f && earliest + delta < 0) delta = -earliest;
    for (const auto& g : groupClipDrag) {
        if (g.idx >= 0 && g.idx < (int)clips.size()) clips[g.idx].startTime = g.orig + delta;
    }
    for (const auto& g : groupSubDrag) {
        if (g.idx >= 0 && g.idx < (int)subs.size()) subs[g.idx].start = g.orig + delta;
    }
}

static void PushAfter(float at, float delta, float floorT) {
    if (delta > -0.001f && delta < 0.001f) return;
    auto shift = [&](float& t) {
        if (t + 0.001f < at) return;
        float n = t + delta;
        if (n < floorT) n = floorT;
        if (n < 0) n = 0;
        t = n;
    };
    for (Clip& c : clips) shift(c.startTime);
    for (Subtitle& s : subs) shift(s.start);
    for (BusSegment& b : bus) shift(b.start);
    for (OverlayFx& o : gOverlays) shift(o.start);
}

static bool FindInsertPoint(int track, float dropT, float& at, float& rightStart) {
    std::vector<int> ids;
    for (int i = 0; i < (int)clips.size(); i++)
        if (clips[i].track == track) ids.push_back(i);
    if ((int)ids.size() < 2) return false;
    std::sort(ids.begin(), ids.end(), [](int a, int b) {
        if (clips[a].startTime != clips[b].startTime) return clips[a].startTime < clips[b].startTime;
        return a < b;
    });
    float tol = std::max(0.2f, 16.0f / std::max(1.0f, pixelsPerSecond));
    for (size_t k = 0; k + 1 < ids.size(); k++) {
        const Clip& L = clips[ids[k]];
        const Clip& R = clips[ids[k + 1]];
        float endL = L.startTime + L.duration;
        float startR = R.startTime;
        float zone0 = endL - tol;
        float zone1 = std::max(endL, startR) + tol;
        if (startR > endL + 0.03f) zone1 = startR + tol;
        if (zone0 < L.startTime + 0.05f) zone0 = L.startTime + 0.05f;
        if (dropT < zone0 || dropT > zone1) continue;
        at = std::max(0.0f, endL);
        rightStart = startR;
        return true;
    }
    return false;
}

static void AddClipOnTrack(Clip c, float dropT) {
    if (dropT < 0) dropT = 0;
    float at = 0, rightStart = 0;
    if (FindInsertPoint(c.track, dropT, at, rightStart)) {
        float dur = std::max(0.05f, c.duration);
        float delta = (at + dur) - rightStart;
        PushAfter(std::min(at, rightStart), delta, at);
        c.startTime = at;
    } else {
        c.startTime = SnapStart(-1, c.duration, dropT);
    }
    clips.push_back(c);
}

void EnsureEnvelope(Clip& c) {
    if (c.volume.empty()) {
        c.volume.push_back({0.0f, 1.0f});
        c.volume.push_back({1.0f, 1.0f});
    }
}

float EnvAtNorm(const Clip& c, float n) {
    if (c.volume.empty()) return 1.0f;
    if (n <= c.volume.front().norm) return c.volume.front().gain;
    if (n >= c.volume.back().norm) return c.volume.back().gain;
    for (size_t i = 1; i < c.volume.size(); i++) {
        if (n <= c.volume[i].norm) {
            float d = c.volume[i].norm - c.volume[i - 1].norm;
            float u = d > 0.0001f ? (n - c.volume[i - 1].norm) / d : 0.0f;
            return c.volume[i - 1].gain + (c.volume[i].gain - c.volume[i - 1].gain) * u;
        }
    }
    return c.volume.back().gain;
}

bool IsSelected(int i) {
    return std::find(selection.begin(), selection.end(), i) != selection.end();
}

void AddBusSegment(float start, float dur) {
    if (start < 0) start = 0;
    if (dur < 0.2f) dur = 0.2f;
    BusSegment s;
    s.start = start;
    s.duration = dur;
    bus.push_back(s);
    selectedBus = (int)bus.size() - 1;
    selectedClip = -1;
    selection.clear();
    contextClip = -1;
    selectedSub = -1;
    subSelection.clear();
    mediaSel.clear();
    gOverlaySel = -1;
}

void CopyBusGrade() {
    if (selectedBus < 0 || selectedBus >= (int)bus.size()) {
        previewStatus = "Select a bus segment";
        return;
    }
    const BusSegment src = bus[selectedBus];
    int n = 0;
    for (int i = 0; i < (int)bus.size(); i++) {
        if (i == selectedBus) continue;
        bus[i].lightness = src.lightness;
        bus[i].contrast = src.contrast;
        bus[i].warmth = src.warmth;
        bus[i].tint = src.tint;
        bus[i].saturation = src.saturation;
        bus[i].shadow = src.shadow;
        bus[i].grain = src.grain;
        bus[i].sharpen = src.sharpen;
        n++;
    }
    previewStatus = n > 0 ? "Bus grade copied" : "No other bus segments";
}

bool panelPointerLock = false;

static bool ForeignName(const char* n) {
    return n && strncmp(n, "Editor", 6) != 0 && strcmp(n, "Debug##Default") != 0;
}

bool TimelinePointerBusy() {
    if (panelPointerLock) return true;
    ImVec2 m = ImGui::GetMousePos();
    auto inside = [&](const ImVec4& r) {
        return r.z > r.x + 1.0f && m.x >= r.x - 8.0f && m.y >= r.y - 8.0f && m.x <= r.z + 8.0f && m.y <= r.w + 8.0f;
    };
    if (busPanelHoldsMouse || (busGradeOpen && inside(busPanelRect))) return true;
    if (gsPanelHolds || (gsPanelOpen && inside(gsPanelRect))) return true;
    if (xformHolds || (xformOpen && inside(xformRect))) return true;
    if (subPanelHolds || (subEditorOpen && inside(subPanelRect)) || (subStyleOpen && inside(subStyleRect))) return true;
    if (mediaPanelOpen && inside(mediaPanelRect)) return true;
    if (audioPanelOpen && inside(audioPanelRect)) return true;
    if (floatMediaOpen && inside(floatMediaRect)) return true;
    if (colorHolds || (colorOpen && inside(colorRect))) return true;
    if (ovPanelHolds || (ovPanelOpen && inside(ovPanelRect))) return true;
    ImGuiContext& g = *GImGui;
    for (int wi = 0; wi < g.Windows.Size; wi++) {
        ImGuiWindow* w = g.Windows[wi];
        if (!w || !w->WasActive) continue;
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) continue;
        if (!ForeignName(w->Name)) continue;
        if (m.x < w->Pos.x - 6.0f || m.y < w->Pos.y - 6.0f) continue;
        if (m.x > w->Pos.x + w->Size.x + 6.0f || m.y > w->Pos.y + w->Size.y + 6.0f) continue;
        return true;
    }
    if (g.ActiveIdWindow && ForeignName(g.ActiveIdWindow->Name)) return true;
    if (g.HoveredWindow && ForeignName(g.HoveredWindow->Name)) return true;
    return false;
}

static void ClearTimelineDrags() {
    draggingClip = -1;
    draggingBus = -1;
    trimmingClip = -1;
    trimmingBus = -1;
    dragVolClip = -1;
    dragVolPt = -1;
    dragVolRole = 0;
    fadeDragClip = -1;
    draggingOv = -1;
    trimmingOv = -1;
    draggingSub = -1;
    trimmingSub = -1;
    draggingPlayhead = false;
    ovDragMode = 0;
    ovDragIndex = -1;
    xformDrag = 0;
    marqueeLive = false;
    marqueeKind = 0;
    marqueeMoved = false;
    groupClipDrag.clear();
    groupSubDrag.clear();
    groupSkip.clear();
}

struct UiShield {
    bool on = false;
    struct C {
        float start, dur, off, gsX, gsY, gsSc, gsAsp, gsRot;
        int track;
        std::vector<VolPoint> vol;
    };
    std::vector<C> clips;
    std::vector<float> bS, bD, sS, sD, oS, oD, oX, oY, oSc, oRot;
};

static UiShield uiShield;

static void CaptureUiShield() {
    uiShield.on = true;
    uiShield.clips.clear();
    uiShield.clips.reserve(clips.size());
    for (const Clip& c : clips) {
        UiShield::C s;
        s.start = c.startTime;
        s.dur = c.duration;
        s.off = c.sourceOffset;
        s.track = c.track;
        s.gsX = c.gsX;
        s.gsY = c.gsY;
        s.gsSc = c.gsScale;
        s.gsAsp = c.gsAspect;
        s.gsRot = c.gsRot;
        s.vol = c.volume;
        uiShield.clips.push_back(std::move(s));
    }
    uiShield.bS.clear();
    uiShield.bD.clear();
    for (const auto& b : bus) { uiShield.bS.push_back(b.start); uiShield.bD.push_back(b.duration); }
    uiShield.sS.clear();
    uiShield.sD.clear();
    for (const auto& s : subs) { uiShield.sS.push_back(s.start); uiShield.sD.push_back(s.duration); }
    uiShield.oS.clear();
    uiShield.oD.clear();
    uiShield.oX.clear();
    uiShield.oY.clear();
    uiShield.oSc.clear();
    uiShield.oRot.clear();
    for (const auto& o : gOverlays) {
        uiShield.oS.push_back(o.start);
        uiShield.oD.push_back(o.duration);
        uiShield.oX.push_back(o.x);
        uiShield.oY.push_back(o.y);
        uiShield.oSc.push_back(o.scale);
        uiShield.oRot.push_back(o.rot);
    }
}

static bool PanelUnderPointer(bool& keepOv, bool& keepGs) {
    keepOv = false;
    keepGs = false;
    ImGuiContext& g = *GImGui;
    if (!g.ActiveId || !g.ActiveIdWindow || !ForeignName(g.ActiveIdWindow->Name)) return false;
    const char* n = g.ActiveIdWindow->Name;
    if (!strcmp(n, "Overlay")) { keepOv = true; return true; }
    if (!strcmp(n, "Transform") || !strcmp(n, "Green screen")) { keepGs = true; return true; }
    if (!strcmp(n, "Bus grade") || !strcmp(n, "Subtitle") || !strcmp(n, "Subtitle style")
        || !strcmp(n, "Audio Effects") || !strcmp(n, "Clip color") || !strcmp(n, "Library clip"))
        return true;
    return false;
}

static void RestoreUiShield() {
    bool keepOv = false, keepGs = false;
    bool panel = PanelUnderPointer(keepOv, keepGs);
    if (panel && ImGui::IsMouseDown(0)) panelPointerLock = true;
    if (!panel || !uiShield.on) return;
    if (uiShield.clips.size() == clips.size()) {
        for (size_t i = 0; i < clips.size(); i++) {
            const auto& s = uiShield.clips[i];
            clips[i].startTime = s.start;
            clips[i].duration = s.dur;
            clips[i].sourceOffset = s.off;
            clips[i].track = s.track;
            clips[i].volume = s.vol;
            if (!keepGs) {
                clips[i].gsX = s.gsX;
                clips[i].gsY = s.gsY;
                clips[i].gsScale = s.gsSc;
                clips[i].gsAspect = s.gsAsp;
                clips[i].gsRot = s.gsRot;
            }
        }
    }
    if (uiShield.bS.size() == bus.size()) {
        for (size_t i = 0; i < bus.size(); i++) {
            bus[i].start = uiShield.bS[i];
            bus[i].duration = uiShield.bD[i];
        }
    }
    if (uiShield.sS.size() == subs.size()) {
        for (size_t i = 0; i < subs.size(); i++) {
            subs[i].start = uiShield.sS[i];
            subs[i].duration = uiShield.sD[i];
        }
    }
    if (uiShield.oS.size() == gOverlays.size()) {
        for (size_t i = 0; i < gOverlays.size(); i++) {
            gOverlays[i].start = uiShield.oS[i];
            gOverlays[i].duration = uiShield.oD[i];
            if (!keepOv) {
                gOverlays[i].x = uiShield.oX[i];
                gOverlays[i].y = uiShield.oY[i];
                gOverlays[i].scale = uiShield.oSc[i];
                gOverlays[i].rot = uiShield.oRot[i];
            }
        }
    }
    ClearTimelineDrags();
}

static void UnselectAll() {
    selection.clear();
    selectedClip = -1;
    contextClip = -1;
    subSelection.clear();
    selectedSub = -1;
    selectedBus = -1;
    mediaSel.clear();
    gOverlaySel = -1;
}

static int SelectedObjectCount() {
    int n = (int)selection.size() + (int)subSelection.size() + (int)mediaSel.size();
    if (selectedBus >= 0) n++;
    if (gOverlaySel >= 0) n++;
    return n;
}

static bool UnselectIfMany() {
    if (SelectedObjectCount() <= 1) return false;
    UnselectAll();
    return true;
}

static void ClearLaneRivals() {
    selectedBus = -1;
    mediaSel.clear();
    gOverlaySel = -1;
}

static void FocusOverlay(int i) {
    gOverlaySel = i;
    selectedClip = -1;
    selection.clear();
    contextClip = -1;
    selectedBus = -1;
    selectedSub = -1;
    subSelection.clear();
    mediaSel.clear();
}

static void DeleteOverlayIndex(int id) {
    if (id < 0 || id >= (int)gOverlays.size()) return;
    gOverlays.erase(gOverlays.begin() + id);
    if (gOverlaySel == id) gOverlaySel = -1;
    else if (gOverlaySel > id) gOverlaySel--;
    if (ovPanel == id) { ovPanel = -1; ovPanelOpen = false; }
    else if (ovPanel > id) ovPanel--;
    if (draggingOv == id) draggingOv = -1;
    else if (draggingOv > id) draggingOv--;
    if (trimmingOv == id) trimmingOv = -1;
    else if (trimmingOv > id) trimmingOv--;
    if (ovDragIndex == id) { ovDragIndex = -1; ovDragMode = 0; }
    else if (ovDragIndex > id) ovDragIndex--;
    fxHover = -1;
}

void SelectOnly(int i) {
    selection.clear();
    mediaSel.clear();
    if (i >= 0) selection.push_back(i);
    selectedClip = i;
    selectedBus = -1;
    selectedSub = -1;
    subSelection.clear();
    gOverlaySel = -1;
}

void ToggleSelect(int i) {
    auto it = std::find(selection.begin(), selection.end(), i);
    if (it == selection.end()) selection.push_back(i);
    else selection.erase(it);
    selectedClip = i;
    selectedBus = -1;
    selectedSub = -1;
    subSelection.clear();
    mediaSel.clear();
    gOverlaySel = -1;
}

static bool ScreenHit(ImVec2 a, ImVec2 b, ImVec2 c0, ImVec2 c1) {
    float x0 = a.x < b.x ? a.x : b.x;
    float x1 = a.x < b.x ? b.x : a.x;
    float y0 = a.y < b.y ? a.y : b.y;
    float y1 = a.y < b.y ? b.y : a.y;
    return x0 < c1.x && x1 > c0.x && y0 < c1.y && y1 > c0.y;
}

static void BeginMarquee(int kind, ImVec2 p) {
    marqueeKind = kind;
    marqueeStart = p;
    marqueeLive = true;
    marqueeMoved = false;
}

static void ApplyMarquee(ImVec2 m) {
    int prev = selectedSub;
    selection.clear();
    selectedClip = -1;
    ClearLaneRivals();
    subSelection.clear();
    selectedSub = -1;
    auto consider = [&](int i, float originX, float originY) {
        float x = originX + clips[i].startTime * pixelsPerSecond;
        float w = std::max(24.0f, clips[i].duration * pixelsPerSecond);
        ImVec2 c0(x, originY);
        float vis = kClipBody;
        if (clips[i].audioOpen) vis += kClipGap + kClipWave;
        ImVec2 c1(x + w, originY + vis);
        if (!ScreenHit(marqueeStart, m, c0, c1)) return;
        selection.push_back(i);
    };
    for (int i = 0; i < (int)clips.size(); i++) {
        int tr = clips[i].track;
        if (tr >= 0 && tr < 6) consider(i, trackLanePos[tr].x, trackLanePos[tr].y);
        else if (tr == kGsTrack) consider(i, gsOriginX, gsLaneY + 4.0f);
    }
    for (int i = 0; i < (int)subs.size(); i++) {
        float x = subsOriginX + subs[i].start * pixelsPerSecond;
        float w = std::max(28.0f, subs[i].duration * pixelsPerSecond);
        ImVec2 c0(x, subsLaneY + 4.0f);
        ImVec2 c1(x + w, subsLaneY + 4.0f + 34.0f);
        if (!ScreenHit(marqueeStart, m, c0, c1)) continue;
        subSelection.push_back(i);
    }
    if (!selection.empty()) selectedClip = selection.back();
    if (!subSelection.empty()) {
        selectedSub = subSelection.back();
        if (subEditorOpen && selectedSub != prev) {
            editingSub = selectedSub;
            LoadSubEditor(subs[selectedSub].text);
        }
    }
}

int ClipBelow(int above, float t) {
    if (above < 0 || above >= (int)clips.size()) return -1;
    int track = clips[above].track;
    int best = -1;
    int bestTrack = 99;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (i == above || clips[i].track >= 3 || clips[i].track <= track) continue;
        if (t < clips[i].startTime || t >= clips[i].startTime + clips[i].duration) continue;
        if (clips[i].track < bestTrack) { bestTrack = clips[i].track; best = i; }
    }
    return best;
}

static int TopPictureAt(float t, int& plate) {
    plate = -1;
    int top = -1;
    int bestTrack = 99;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (clips[i].track >= 3) continue;
        if (t < clips[i].startTime || t >= clips[i].startTime + clips[i].duration) continue;
        if (clips[i].track <= bestTrack) {
            bestTrack = clips[i].track;
            top = i;
        }
    }
    if (top >= 0) {
        int mid = clips[top].mediaId;
        if (mid >= 0 && mid < (int)media.size() && SolidHasAlpha(media[mid].path)) {
            plate = top;
            top = ClipBelow(plate, t);
        }
    }
    return top;
}

static void RoutePreviewClip(float t, int& top, int& plate, int& drive) {
    top = TopPictureAt(t, plate);
    drive = top;
    fxOther = -1;
    fxActiveId.clear();
    fxProgress = 0;
    if (top < 0) return;
    int ta = -1, tb = -1;
    float prog = 0;
    std::string tid;
    if (!TransitionForClip(top, t, ta, tb, prog, tid)) return;
    drive = tb;
    fxOther = ta;
    fxProgress = prog;
    fxActiveId = tid;
}

int OverlapPartner(int i) {
    if (i < 0 || i >= (int)clips.size()) return -1;
    const Clip& a = clips[i];
    if (a.track >= 3) return -1;
    float a0 = a.startTime;
    float a1 = a0 + a.duration;
    int best = -1;
    int bestDist = 99;
    float bestOv = 0.05f;
    for (int j = 0; j < (int)clips.size(); j++) {
        if (j == i || clips[j].track >= 3 || clips[j].track == a.track) continue;
        float b0 = clips[j].startTime;
        float b1 = b0 + clips[j].duration;
        float ov = std::min(a1, b1) - std::max(a0, b0);
        if (ov < 0.05f) continue;
        int dist = std::abs(clips[j].track - a.track);
        if (dist < bestDist || (dist == bestDist && ov > bestOv)) {
            bestDist = dist;
            bestOv = ov;
            best = j;
        }
    }
    return best;
}

bool TransitionForClip(int clip, float t, int& a, int& b, float& progress, std::string& id) {
    a = b = -1;
    progress = 0;
    id.clear();
    if (clip < 0 || clip >= (int)clips.size()) return false;
    int other = OverlapPartner(clip);
    if (other < 0) return false;
    auto known = [&](int i) {
        return !clips[i].fxOut.empty() && FxKnown(clips[i].fxOut.c_str());
    };
    int owner = (clips[clip].track <= clips[other].track) ? clip : other;
    int carrier = -1;
    if (known(owner)) carrier = owner;
    else if (known(clip)) carrier = clip;
    else if (known(other)) carrier = other;
    if (carrier < 0) return false;
    float t0 = std::max(clips[clip].startTime, clips[other].startTime);
    float t1 = std::min(clips[clip].startTime + clips[clip].duration, clips[other].startTime + clips[other].duration);
    if (!(t1 > t0 + 0.02f) || t < t0 || t >= t1) return false;
    int outgoing = clip;
    int incoming = other;
    if (clips[other].startTime < clips[clip].startTime - 0.001f) {
        outgoing = other;
        incoming = clip;
    }
    a = outgoing;
    b = incoming;
    progress = (t - t0) / (t1 - t0);
    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;
    id = clips[carrier].fxOut;
    return true;
}

float ClipLocalTime(int clipIdx, float timelineT) {
    const Clip& c = clips[clipIdx];
    float local = timelineT - c.startTime;
    if (local < 0) local = 0;
    if (local > c.duration) local = std::max(0.0f, c.duration - 0.001f);
    return local;
}

int CountSelectedJoins() {
    int n = 0;
    for (int i : selection) {
        int j = OverlapPartner(i);
        if (j > i && IsSelected(j)) n++;
    }
    return n;
}

int ApplyFxToSelection(const char* id, float dur) {
    (void)dur;
    int n = 0;
    std::vector<int> used;
    for (int i : selection) {
        if (i < 0 || i >= (int)clips.size()) continue;
        int j = OverlapPartner(i);
        if (j < 0 || !IsSelected(j)) continue;
        int owner = (clips[i].track <= clips[j].track) ? i : j;
        int other = (owner == i) ? j : i;
        if (std::find(used.begin(), used.end(), owner) != used.end()) continue;
        used.push_back(owner);
        if (id && id[0] && FxKnown(id)) {
            float t0 = std::max(clips[i].startTime, clips[j].startTime);
            float t1 = std::min(clips[i].startTime + clips[i].duration, clips[j].startTime + clips[j].duration);
            clips[owner].fxOut = id;
            clips[owner].fxDur = std::max(0.05f, t1 - t0);
            clips[other].fxOut.clear();
        } else {
            clips[i].fxOut.clear();
            clips[j].fxOut.clear();
        }
        n++;
    }
    return n;
}

void SetClipSpeed(Clip& c, float sp) {
    if (sp < 0.1f) sp = 0.1f;
    float span = c.duration * std::max(0.1f, c.speed);
    c.speed = sp;
    c.duration = std::max(0.1f, span / sp);
    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
    if (player) player->lastPts = -1;
    ResetAudioSync();
#endif
}

void SplitSelected() {
    if (selectedClip < 0 || selectedClip >= (int)clips.size()) return;
    Clip& c = clips[selectedClip];
    float cut = currentTime;
    if (cut <= c.startTime + 0.05f || cut >= c.startTime + c.duration - 0.05f)
        cut = c.startTime + c.duration * 0.5f;
    float leftDur = cut - c.startTime;
    float rightDur = c.duration - leftDur;
    if (leftDur < 0.05f || rightDur < 0.05f) return;
    EnsureEnvelope(c);
    float cutN = leftDur / c.duration;
    float gCut = EnvAtNorm(c, cutN);
    Clip right = c;
    right.startTime = cut;
    right.duration = rightDur;
    right.sourceOffset = c.sourceOffset + leftDur * std::max(0.05f, c.speed);
    right.fadeIn = false;
    right.fadeOut = c.fadeOut;
    c.fadeOut = false;
    right.opIn = 0;
    right.opOut = c.opOut;
    c.opOut = 0;
    right.volIn = 0;
    right.volOut = c.volOut;
    c.volOut = 0;
    c.duration = leftDur;
    if (c.opIn > c.duration) c.opIn = c.duration;
    if (c.volIn > c.duration) c.volIn = c.duration;
    if (right.opOut > right.duration) right.opOut = right.duration;
    if (right.volOut > right.duration) right.volOut = right.duration;
    std::vector<VolPoint> leftPts, rightPts;
    leftPts.push_back({0.0f, c.volume.front().gain});
    for (const auto& p : c.volume) {
        if (p.norm > 0.001f && p.norm < cutN - 0.001f)
            leftPts.push_back({p.norm / cutN, p.gain});
        else if (p.norm > cutN + 0.001f && p.norm < 0.999f)
            rightPts.push_back({(p.norm - cutN) / (1.0f - cutN), p.gain});
    }
    leftPts.push_back({1.0f, gCut});
    rightPts.insert(rightPts.begin(), {0.0f, gCut});
    rightPts.push_back({1.0f, c.volume.back().gain});
    c.volume = leftPts;
    right.volume = rightPts;
    c.fxOut.clear();
    c.fx.clear();
    right.fx.clear();
    clips.insert(clips.begin() + selectedClip + 1, right);
    for (int& s : selection) if (s > selectedClip) s++;
    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
    ResetAudioSync();
#endif
}

void SplitBusAt(float cut) {
    int idx = -1;
    if (selectedBus >= 0 && selectedBus < (int)bus.size()) {
        const BusSegment& s = bus[selectedBus];
        if (cut > s.start + 0.05f && cut < s.start + s.duration - 0.05f) idx = selectedBus;
    }
    if (idx < 0) {
        for (int i = 0; i < (int)bus.size(); i++) {
            if (cut > bus[i].start + 0.05f && cut < bus[i].start + bus[i].duration - 0.05f) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) return;
    float end = bus[idx].start + bus[idx].duration;
    float left = cut - bus[idx].start;
    float rightDur = end - cut;
    if (left < 0.05f || rightDur < 0.05f) return;
    BusSegment right = bus[idx];
    bus[idx].duration = left;
    right.start = cut;
    right.duration = rightDur;
    bus.insert(bus.begin() + idx + 1, right);
    selectedBus = idx + 1;
    selectedClip = -1;
    previewStatus = "Sub segment split";
}

HWND GetSDLWindowHandle(SDL_Window* window) {
#ifdef _WIN32
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) return wmInfo.info.win.window;
#endif
    return NULL;
}

void AddMedia(const std::string& path);

void ImportFiles(HWND owner) {
#ifdef _WIN32
    std::vector<char> buf(65536, 0);
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Media\0*.mp4;*.mov;*.avi;*.mkv;*.webm;*.mp3;*.wav;*.aac;*.flac;*.m4a;*.ogg;*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.gif\0All Files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = "Import Media";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameA(&ofn)) return;
    char* p = buf.data();
    std::string first = p;
    p += first.size() + 1;
    if (*p == 0) {
        AddMedia(first);
    } else {
        while (*p) {
            std::string name = p;
            AddMedia(first + "\\" + name);
            p += name.size() + 1;
        }
    }
#else
    (void)owner;
#endif
}

void BuildSolidPixels(int kind, std::vector<uint8_t>& px, int W, int H) {
    if (W < 2) W = 2;
    if (H < 2) H = 2;
    px.assign((size_t)W * (size_t)H * 4, 0);
    auto put = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        size_t i = ((size_t)y * (size_t)W + (size_t)x) * 4;
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    };
    if (kind == 1) {
        int bar = std::max(2, H * 12 / 100);
        for (int y = 0; y < H; y++) {
            bool stripe = y < bar || y >= H - bar;
            for (int x = 0; x < W; x++)
                put(x, y, 0, 0, 0, stripe ? 255 : 0);
        }
    } else if (kind == 2) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) put(x, y, 0, 255, 0, 255);
    } else {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) put(x, y, 0, 0, 0, 255);
    }
}

int SolidKindFromPath(const std::string& path) {
    if (path == "solid:bars") return 1;
    if (path == "solid:green") return 2;
    if (path == "solid:black") return 0;
    return -1;
}

bool IsSolidPath(const std::string& path) { return SolidKindFromPath(path) >= 0; }
bool SolidHasAlpha(const std::string& path) { return path == "solid:bars"; }

static const char* kSolidId[] = {"solid:black", "solid:bars", "solid:green"};
static const char* kSolidName[] = {"Black", "Letterbox", "Green"};

void EnsureSolidThumbs() {
    for (int k = 0; k < 3; k++) {
        if (gSolidTex[k]) continue;
        std::vector<uint8_t> px;
        BuildSolidPixels(k, px, 320, 180);
        glGenTextures(1, &gSolidTex[k]);
        glBindTexture(GL_TEXTURE_2D, gSolidTex[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 320, 180, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
}

static int EnsureSolidMedia(int kind) {
    if (kind < 0 || kind > 2) kind = 0;
    for (int i = 0; i < (int)media.size(); i++)
        if (media[i].path == kSolidId[kind]) return i;
    EnsureSolidThumbs();
    MediaItem m;
    m.path = kSolidId[kind];
    m.name = kSolidName[kind];
    m.type = "image";
    m.duration = 5.0f;
    m.aspect = 16.0f / 9.0f;
    m.thumb = gSolidTex[kind];
    m.thumbW = 320;
    m.thumbH = 180;
    media.push_back(m);
    return (int)media.size() - 1;
}

void AddMedia(const std::string& path) {
    if (path.empty()) return;
    MediaItem m;
    m.path = path;
    size_t pos = path.find_last_of("/\\");
    m.name = (pos == std::string::npos) ? path : path.substr(pos + 1);
    std::string lower = m.name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find(".mp4") != std::string::npos || lower.find(".mov") != std::string::npos ||
        lower.find(".avi") != std::string::npos || lower.find(".mkv") != std::string::npos ||
        lower.find(".webm") != std::string::npos)
        m.type = "video";
    else if (lower.find(".mp3") != std::string::npos || lower.find(".wav") != std::string::npos ||
             lower.find(".aac") != std::string::npos || lower.find(".flac") != std::string::npos ||
             lower.find(".m4a") != std::string::npos || lower.find(".ogg") != std::string::npos)
        m.type = "audio";
    else
        m.type = "image";
#ifdef HAVE_FFMPEG
    m.duration = ProbeDuration(path);
    MakeThumbnail(m);
#else
    m.duration = (m.type == "image") ? 5.0f : 10.0f;
#endif
    media.push_back(m);
}

void DrawPlaceholder(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const MediaItem& m) {
    ImU32 bg = (m.type == "video") ? IM_COL32(50, 90, 160, 255) :
               (m.type == "audio") ? IM_COL32(50, 130, 70, 255) : IM_COL32(120, 70, 140, 255);
    dl->AddRectFilled(p0, p1, bg, 4.0f);
    dl->AddRect(p0, p1, IM_COL32(180, 180, 180, 120), 4.0f);
    const char* icon = (m.type == "video") ? "VID" : (m.type == "audio") ? "AUD" : "IMG";
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p0.x + (p1.x - p0.x - ts.x) * 0.5f, p0.y + (p1.y - p0.y - ts.y) * 0.35f), IM_COL32(255, 255, 255, 230), icon);
}

void EnsurePlayerForClip(int clipIdx) {
#ifdef HAVE_FFMPEG
    if (clipIdx < 0 || clipIdx >= (int)clips.size()) return;
    int mid = clips[clipIdx].mediaId;
    if (mid < 0 || mid >= (int)media.size()) return;
    auto ready = [](VideoPlayer* p) {
        return p && p->hasFrame && (p->fmtCtx || p->still);
    };
    bool image = media[mid].type == "image";
    float speed = std::max(0.05f, clips[clipIdx].speed);
    float mediaT = image ? 0.0f : clips[clipIdx].sourceOffset + ClipLocalTime(clipIdx, currentTime) * speed;
    if (mediaT < 0) mediaT = 0;
    if (clipIdx == playingClip && mid == playingMediaId && playerFileId == mid && ready(player)) return;
    auto takeSlot = [&](VideoPlayer*& slot, int& slotClip, int& slotMedia, float& slotShown) -> bool {
        if (!slot || slot == player || slotClip != clipIdx || slotMedia != mid) return false;
        if (!ready(slot)) return false;
        VideoPlayer* previous = player;
        player = slot;
        slot = previous;
        playingMediaId = mid;
        playingClip = clipIdx;
        playerFileId = mid;
        slotMedia = -1;
        slotClip = -1;
        slotShown = -1000.0f;
        shownLocal = (player->lastPts >= 0.0) ? (float)player->lastPts : mediaT;
        return true;
    };
    auto catchUp = [&](VideoPlayer* p) {
        if (!p || p->still || image) {
            shownLocal = mediaT;
            return;
        }
        bool ahead = p->lastPts >= 0.0 && mediaT + 0.12f < (float)p->lastPts;
        bool behind = p->lastPts < 0.0 || (float)p->lastPts + 0.45f < mediaT;
        if (!p->hasFrame || ahead || behind) {
            if (p->lastPts < 0.0 || ahead || behind) p->Seek(mediaT);
            p->GrabFrame();
        }
        int guard = 0;
        int limit = behind ? 100 : 8;
        if (isPlaying) limit = 4;
        while (p->lastPts >= 0.0 && (float)p->lastPts + 0.0005f < mediaT && guard < limit) {
            if (!p->GrabFrame()) break;
            guard++;
        }
        shownLocal = p->lastPts >= 0.0 ? (float)p->lastPts : mediaT;
    };
    if (takeSlot(playerB, playingClipB, playingMediaIdB, shownLocalB)) {
        if (std::fabs(shownLocal - mediaT) > 0.20f) catchUp(player);
        return;
    }
    if (takeSlot(playerUnder, playingClipUnder, playingMediaIdUnder, shownLocalUnder)) {
        if (std::fabs(shownLocal - mediaT) > 0.20f) catchUp(player);
        return;
    }
    bool nextClose = playerNext && nextClipIdx == clipIdx && nextMediaId == mid && playerNext->fmtCtx && playerNext->hasFrame
        && playerNext->lastPts >= 0.0 && std::fabs((float)playerNext->lastPts - mediaT) <= 0.20f;
    auto adoptNext = [&]() {
        if (player) { player->Close(); delete player; }
        player = playerNext;
        playerNext = nullptr;
        nextMediaId = -1;
        nextClipIdx = -1;
        playingClip = clipIdx;
        playingMediaId = mid;
        playerFileId = mid;
        shownLocal = player->lastPts >= 0.0 ? (float)player->lastPts : mediaT;
    };
    if (nextClose) {
        adoptNext();
        return;
    }
    if (!player) player = new VideoPlayer();
    if (!(playerFileId == mid && player->fmtCtx)) {
        if (gRam.Present(player, mid, mediaT)) {
            playingClip = clipIdx;
            playingMediaId = mid;
            shownLocal = mediaT;
            return;
        }
    }
    if (playerNext && nextClipIdx == clipIdx && nextMediaId == mid && playerNext->hasFrame && (playerNext->fmtCtx || playerNext->still)) {
        adoptNext();
        if (std::fabs(shownLocal - mediaT) > 0.20f) catchUp(player);
        return;
    }
    if (mid == playingMediaId && playerFileId == mid && player->fmtCtx) {
        catchUp(player);
        playingClip = clipIdx;
        playingMediaId = mid;
        return;
    }
    previewStatus = "Opening...";
    shownLocal = -1000.0f;
    if (player->Open(media[mid].path)) {
        playingMediaId = mid;
        playingClip = clipIdx;
        playerFileId = mid;
        if (!player->still) player->Seek(mediaT);
        if (player->GrabFrame()) {
            int guard = 0;
            int walk = isPlaying ? 2 : 100;
            while (!image && !player->still && player->lastPts >= 0.0 && (float)player->lastPts + 0.0005f < mediaT && guard < walk) {
                if (!player->GrabFrame()) break;
                guard++;
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%s %dx%d", player->decName, player->width, player->height);
            previewStatus = buf;
            shownLocal = player->lastPts >= 0.0 ? (float)player->lastPts : mediaT;
        } else {
            previewStatus = player->lastError.empty() ? "Grab failed" : player->lastError;
        }
    } else {
        previewStatus = player->lastError.empty() ? "Open failed" : player->lastError;
    }
#else
    (void)clipIdx;
    previewStatus = "FFmpeg not linked";
#endif
}

static int NextVisibleClip(int currentTop, float from, float horizon, float& changeAt) {
    changeAt = -1.0f;
    int best = -1;
    float bestT = from + horizon + 1.0f;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (clips[i].track >= 3) continue;
        float marks[2] = {clips[i].startTime, clips[i].startTime + clips[i].duration};
        for (int m = 0; m < 2; m++) {
            float t = marks[m];
            if (t < from + 0.03f || t > from + horizon || t > bestT + 0.0005f) continue;
            int plate = -1;
            int vis = TopPictureAt(t + 0.002f, plate);
            if (vis < 0 || vis == currentTop) continue;
            bestT = t;
            best = vis;
        }
    }
    if (best < 0) return -1;
    changeAt = bestT;
    return best;
}

void PrefetchUpcoming(int top) {
#ifdef HAVE_FFMPEG
    if (!isPlaying || draggingPlayhead) return;
    for (int i = 0; i < (int)clips.size(); i++) {
        int am = clips[i].mediaId;
        if (am < 0 || am >= (int)media.size()) continue;
        if (media[am].type == "image" || media[am].type == "audio") continue;
        float start = clips[i].startTime;
        if (start < currentTime - 0.2f || start > currentTime + 5.0f) continue;
        int ai = EnsureAudio(am);
        if (ai < 0 || ai >= (int)audioSrcs.size()) continue;
        if (audioSrcs[ai].nextTime >= 0 || audioSrcs[ai].failed || audioSrcs[ai].noAudio || audioSrcs[ai].atEnd) continue;
        int16_t junk[64];
        PullSamples(audioSrcs[ai], std::max(0.0f, clips[i].sourceOffset), 32, junk);
    }
    int ignored = -1;
    int nowTop = (top >= 0 && top < (int)clips.size()) ? top : TopPictureAt(currentTime, ignored);
    float changeAt = -1.0f;
    int best = NextVisibleClip(nowTop, currentTime, 5.0f, changeAt);
    if (best < 0) {
        if (playerNext && nextClipIdx >= 0) {
            playerNext->Close();
            nextMediaId = -1;
            nextClipIdx = -1;
        }
        return;
    }
    int mid = clips[best].mediaId;
    if (mid < 0 || mid >= (int)media.size()) return;
    if (media[mid].type == "audio" || IsSolidPath(media[mid].path)) return;
    if (playingClip == best || playingClipUnder == best || playingClipB == best) return;
    float end = clips[best].startTime + clips[best].duration;
    bool image = media[mid].type == "image";
    float speed = std::max(0.05f, clips[best].speed);
    float cutWhen = changeAt;
    if (cutWhen < clips[best].startTime) cutWhen = clips[best].startTime;
    if (cutWhen > end) cutWhen = clips[best].startTime;
    float cutMedia = image ? 0.0f : clips[best].sourceOffset + ClipLocalTime(best, cutWhen) * speed;
    if (cutMedia < 0) cutMedia = 0;
    if (image ? gRam.Has(mid, 0.0f) : gRam.Has(mid, cutMedia)) return;
    if (mid == playingMediaId && playerFileId == mid && player && player->fmtCtx && nowTop >= 0
        && nowTop < (int)clips.size() && clips[nowTop].mediaId >= 0 && clips[nowTop].mediaId < (int)media.size()) {
        float curSpeed = std::max(0.05f, clips[nowTop].speed);
        bool curImage = media[clips[nowTop].mediaId].type == "image";
        float curMedia = curImage ? 0.0f : clips[nowTop].sourceOffset + ClipLocalTime(nowTop, currentTime) * curSpeed;
        float lead = std::max(0.0f, changeAt - currentTime);
        if (std::fabs((curMedia + lead * curSpeed) - cutMedia) < 0.20f) return;
    }
    if (!playerNext) playerNext = new VideoPlayer();
    if (nextMediaId != mid || !(playerNext->fmtCtx || playerNext->still)) {
        if (!playerNext->Open(media[mid].path)) return;
        nextMediaId = mid;
        nextClipIdx = -1;
    }
    if (image || playerNext->still) {
        if (!playerNext->hasFrame) playerNext->GrabFrame();
        if (playerNext->hasFrame) nextClipIdx = best;
        return;
    }
    float park = cutMedia;
    bool ahead = playerNext->lastPts >= 0.0 && park + 0.12f < (float)playerNext->lastPts;
    if (playerNext->lastPts < 0.0 || ahead) {
        playerNext->Seek(park);
        playerNext->GrabFrame();
    }
    int guard = 0;
    while (playerNext->lastPts >= 0.0 && (float)playerNext->lastPts + 0.03f < park && guard < 3) {
        if (!playerNext->GrabFrame()) break;
        guard++;
    }
    if (playerNext->hasFrame) nextClipIdx = best;
#else
    (void)top;
#endif
}

void ReleaseSidePlayer() {
#ifdef HAVE_FFMPEG
    if (playerB && (playerB->fmtCtx || playerB->hasFrame)) playerB->Close();
    playingMediaIdB = -1;
    playingClipB = -1;
    shownLocalB = -1000.0f;
#endif
}

void ReleaseUnder() {
#ifdef HAVE_FFMPEG
    if (playerUnder && (playerUnder->fmtCtx || playerUnder->hasFrame)) playerUnder->Close();
    playingMediaIdUnder = -1;
    playingClipUnder = -1;
    shownLocalUnder = -1000.0f;
#endif
}

static void SyncBackground(int top, int plate, int drive) {
#ifdef HAVE_FFMPEG
    int bg[2] = {-1, -1};
    int nb = 0;
    int partner = fxOther;
    bool gaps = FrontLeavesGaps(top, partner, plate, drive, currentTime, bg, nb);
    bgShowN = 0;
    bgShow[0] = bgShow[1] = -1;
    if (!gaps) {
        if (fxOther < 0) ReleaseSidePlayer();
        ReleaseUnder();
        return;
    }
    bgShowN = nb;
    bgShow[0] = bg[0];
    bgShow[1] = nb > 1 ? bg[1] : -1;
    if (fxOther < 0) {
        SyncSide(bg[nb - 1], currentTime);
        if (nb >= 2) SyncUnder(bg[0], currentTime);
        else ReleaseUnder();
    } else {
        SyncUnder(bg[nb - 1], currentTime);
    }
#else
    (void)top; (void)plate; (void)drive;
    bgShowN = 0;
#endif
}

void SyncGreen() {
#ifdef HAVE_FFMPEG
    gsDrawClip = FindGsClip(currentTime);
    if (gsDrawClip < 0) return;
    int mid = clips[gsDrawClip].mediaId;
    if (mid < 0 || mid >= (int)media.size()) { gsDrawClip = -1; return; }
    if (!playerGS) playerGS = new VideoPlayer();
    bool image = media[mid].type == "image";
    float mediaT = image ? 0.0f : clips[gsDrawClip].sourceOffset + ClipLocalTime(gsDrawClip, currentTime) * std::max(0.05f, clips[gsDrawClip].speed);
    if (gRam.Present(playerGS, mid, mediaT)) {
        playingGsMedia = mid;
        playingGsClip = gsDrawClip;
        return;
    }
    if (playingGsClip != gsDrawClip || playingGsMedia != mid || !playerGS->fmtCtx) {
        if (!playerGS->Open(media[mid].path)) { gsDrawClip = -1; return; }
        playingGsMedia = mid;
        playingGsClip = gsDrawClip;
    }
    if (media[mid].type == "image") {
        if (!playerGS->hasFrame) {
            if (!playerGS->GrabFrame()) {
                playerGS->Seek(0);
                playerGS->GrabFrame();
            }
        }
        return;
    }
    bool ahead = playerGS->lastPts >= 0.0 && mediaT + 0.12f < (float)playerGS->lastPts;
    if (playerGS->lastPts < 0.0 || ahead) {
        playerGS->Seek(std::max(0.0f, mediaT));
        playerGS->GrabFrame();
    }
    int limit = ((float)playerGS->lastPts + 0.08f < mediaT) ? 120 : 2;
    int guard = 0;
    while (playerGS->lastPts >= 0.0 && (float)playerGS->lastPts + 0.0005f < mediaT && guard < limit) {
        if (!playerGS->GrabFrame()) break;
        guard++;
    }
#else
    gsDrawClip = -1;
#endif
}

void ApplyTrim(int idx, float mouseX) {
    if (idx < 0 || idx >= (int)clips.size()) return;
    Clip& c = clips[idx];
    bool image = c.mediaId >= 0 && c.mediaId < (int)media.size() && media[c.mediaId].type == "image";
    float srcDur = image ? 100000.0f : ((c.mediaId >= 0 && c.mediaId < (int)media.size()) ? media[c.mediaId].duration : (c.duration * c.speed));
    float speed = c.speed < 0.05f ? 1.0f : c.speed;
    float dt = (mouseX - trimAnchorX) / pixelsPerSecond;
    if (trimSide == 1) {
        float nd = trimOrigDur + dt;
        float maxD = image ? 100000.0f : (srcDur - trimOrigOffset) / speed;
        if (nd < 0.1f) nd = 0.1f;
        if (nd > maxD) nd = maxD;
        float end = SnapTime(idx, trimOrigStart + nd);
        nd = end - trimOrigStart;
        if (nd < 0.1f) nd = 0.1f;
        if (nd > maxD) nd = maxD;
        c.duration = nd;
        c.sourceOffset = image ? 0.0f : trimOrigOffset;
        c.startTime = trimOrigStart;
    } else {
        float nstart = trimOrigStart + dt;
        float noff = image ? 0.0f : trimOrigOffset + dt * speed;
        float ndur = trimOrigDur - dt;
        if (nstart < 0) {
            float back = -nstart;
            nstart = 0;
            if (!image) noff -= back * speed;
            ndur -= back;
        }
        if (!image && noff < 0) {
            float back = -noff / speed;
            noff = 0;
            nstart -= back;
            ndur += back;
            if (nstart < 0) nstart = 0;
        }
        if (ndur < 0.1f) ndur = 0.1f;
        if (!image && noff + ndur * speed > srcDur) ndur = std::max(0.1f, (srcDur - noff) / speed);
        float snapped = SnapTime(idx, nstart);
        float applied = snapped - trimOrigStart;
        nstart = snapped;
        if (!image) noff = trimOrigOffset + applied * speed;
        ndur = trimOrigDur - applied;
        if (!image && noff < 0) { ndur += (-noff) / speed; noff = 0; }
        if (nstart < 0) nstart = 0;
        if (ndur < 0.1f) ndur = 0.1f;
        if (!image && noff + ndur * speed > srcDur) ndur = std::max(0.1f, (srcDur - noff) / speed);
        c.startTime = nstart;
        c.sourceOffset = image ? 0.0f : noff;
        c.duration = ndur;
    }
    auto clampFade = [&](float& v) {
        if (v < 0) v = 0;
        if (v > c.duration) v = c.duration;
    };
    clampFade(c.opIn);
    clampFade(c.opOut);
    clampFade(c.volIn);
    clampFade(c.volOut);
}

void ExportDims(int aspect, int res, int& w, int& h) {
    if (res == 5) {
        if (aspect == 1 || aspect == 4) { w = 768; h = 1376; }
        else if (aspect == 2) { w = 768; h = 768; }
        else { w = 1376; h = 768; }
        return;
    }
    const int heights[] = {720, 1080, 1440, 2160, 4320};
    const int widths169[] = {1280, 1920, 2560, 3840, 7680};
    if (res < 0) res = 0;
    if (res > 4) res = 4;
    if (aspect < 0) aspect = 0;
    if (aspect > 4) aspect = 4;
    int H = heights[res];
    int W = widths169[res];
    if (aspect == 0) { w = W; h = H; }
    else if (aspect == 1) { w = H; h = W; }
    else if (aspect == 2) { w = H; h = H; }
    else if (aspect == 4) { h = W; w = (W * 3 / 4) & ~1; }
    else { h = H; w = (H * 4 / 3) & ~1; }
    w &= ~1;
    h &= ~1;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
}

static std::string SrtClock(float t) {
    if (t < 0) t = 0;
    int ms = (int)std::lround(t * 1000.0f);
    int s = ms / 1000;
    ms %= 1000;
    int m = s / 60;
    s %= 60;
    int h = m / 60;
    m %= 60;
    char b[32];
    snprintf(b, sizeof(b), "%02d:%02d:%02d,%03d", h, m, s, ms);
    return b;
}

static std::string SwapExt(const std::string& path, const char* ext) {
    size_t slash = path.find_last_of("\\/");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + ext;
    return path.substr(0, dot) + ext;
}

static bool WriteSrt(const std::string& path) {
    std::vector<int> order(subs.size());
    for (int i = 0; i < (int)subs.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [](int a, int b) { return subs[a].start < subs[b].start; });
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite("\xEF\xBB\xBF", 1, 3, f);
    int n = 1;
    for (int idx : order) {
        const Subtitle& s = subs[idx];
        if (s.text.empty()) continue;
        float a = std::max(0.0f, s.start);
        float b = a + std::max(0.2f, s.duration);
        fprintf(f, "%d\n%s --> %s\n%s\n\n", n++, SrtClock(a).c_str(), SrtClock(b).c_str(), s.text.c_str());
    }
    fclose(f);
    return n > 1;
}

std::string SaveDialog(HWND owner, const char* filter, const char* defExt, const char* title) {
#ifdef _WIN32
    char buf[4096] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = defExt;
    ofn.lpstrTitle = title;
    if (GetSaveFileNameA(&ofn)) return buf;
#else
    (void)owner; (void)filter; (void)defExt; (void)title;
#endif
    return {};
}

std::string OpenDialog(HWND owner, const char* filter, const char* title) {
#ifdef _WIN32
    char buf[4096] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = title;
    if (GetOpenFileNameA(&ofn)) return buf;
#else
    (void)owner; (void)filter; (void)title;
#endif
    return {};
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n < 1) return L"";
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !w[0]) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n < 2) return {};
    std::string s((size_t)n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static const std::wstring& LookRootW() {
    static std::wstring root;
    static int ready = 0;
    if (!ready) {
        ready = 1;
        wchar_t home[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        root = (n > 0 && n < MAX_PATH) ? std::wstring(home) : L".";
        root += L"\\DaniWorldsStudio";
        CreateDirectoryW(root.c_str(), nullptr);
        root += L"\\Looks";
        CreateDirectoryW(root.c_str(), nullptr);
    }
    return root;
}

static std::wstring LookDirW(const char* kind) {
    std::wstring d = LookRootW() + L"\\" + Utf8ToWide(kind ? kind : "look");
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::string CleanLookName(const char* in) {
    std::string o;
    if (!in) return o;
    for (const unsigned char* p = (const unsigned char*)in; *p && o.size() < 48; ++p) {
        if (*p < 32) continue;
        if (*p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') continue;
        o.push_back((char)*p);
    }
    size_t a = 0;
    while (a < o.size() && (o[a] == ' ' || o[a] == '.')) a++;
    size_t b = o.size();
    while (b > a && (o[b - 1] == ' ' || o[b - 1] == '.')) b--;
    return o.substr(a, b - a);
}

struct LookBin {
    const char* kind;
    std::vector<std::string> names;
    char typed[64];
    int chars;
    bool arm;
};

static LookBin gLooks[] = {
    {"grade", {}, {}, 0, false},
    {"xform", {}, {}, 0, false},
    {"gs", {}, {}, 0, false},
    {"audio", {}, {}, 0, false},
    {"style", {}, {}, 0, false},
    {"overlay", {}, {}, 0, false},
};

static LookBin* LookBinFor(const char* kind) {
    for (auto& b : gLooks) if (strcmp(b.kind, kind) == 0) return &b;
    return &gLooks[0];
}

static void RefreshLookBin(LookBin& bin) {
    bin.names.clear();
    std::wstring pat = LookDirW(bin.kind) + L"\\*.dlook";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = WideToUtf8(fd.cFileName);
        if (name.size() > 6 && name.compare(name.size() - 6, 6, ".dlook") == 0)
            name.resize(name.size() - 6);
        if (!name.empty()) bin.names.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(bin.names.begin(), bin.names.end());
}

static void RefreshAllLooks() {
    for (auto& b : gLooks) RefreshLookBin(b);
}

static std::string LookFilePath(const char* kind, const std::string& name) {
    std::wstring w = LookDirW(kind) + L"\\" + Utf8ToWide(name) + L".dlook";
    return WideToUtf8(w.c_str());
}

static FILE* LookCreate(const std::string& path, const char* kind) {
    FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"wb");
    if (!f) return nullptr;
    fprintf(f, "DWSLOOK %s\n", kind);
    return f;
}

static bool LookOpen(const std::string& path, const char* kind, FILE*& f) {
    f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
    if (!f) return false;
    char tag[32] = {};
    char got[32] = {};
    if (fscanf(f, "%31s %31s", tag, got) != 2 || strcmp(tag, "DWSLOOK") != 0 || strcmp(got, kind) != 0) {
        fclose(f);
        f = nullptr;
        return false;
    }
    return true;
}

static bool TakeF(FILE* f, const char* key, float& v) {
    char k[32];
    float x = 0;
    if (fscanf(f, "%31s %f", k, &x) != 2) return false;
    if (strcmp(k, key) != 0) return false;
    v = x;
    return true;
}

static bool TakeI(FILE* f, const char* key, int& v) {
    char k[32];
    int x = 0;
    if (fscanf(f, "%31s %d", k, &x) != 2) return false;
    if (strcmp(k, key) != 0) return false;
    v = x;
    return true;
}

static float LimRange(float v, float a, float b) {
    if (v < a) return a;
    if (v > b) return b;
    return v;
}

static bool WriteGradeLook(const std::string& path, const BusSegment& s) {
    FILE* f = LookCreate(path, "grade");
    if (!f) return false;
    fprintf(f, "light %.5f\ncontrast %.5f\nwarm %.5f\ntint %.5f\nsat %.5f\nshadow %.5f\ngrain %.5f\nsharp %.5f\n",
            s.lightness, s.contrast, s.warmth, s.tint, s.saturation, s.shadow, s.grain, s.sharpen);
    fclose(f);
    return true;
}

static bool ReadGradeLook(const std::string& path, BusSegment& s) {
    FILE* f = nullptr;
    if (!LookOpen(path, "grade", f)) return false;
    float light, contrast, warm, tint, sat, shadow, grain, sharp;
    bool ok = TakeF(f, "light", light) && TakeF(f, "contrast", contrast) && TakeF(f, "warm", warm)
        && TakeF(f, "tint", tint) && TakeF(f, "sat", sat) && TakeF(f, "shadow", shadow)
        && TakeF(f, "grain", grain) && TakeF(f, "sharp", sharp);
    fclose(f);
    if (!ok) return false;
    s.lightness = Lim11(light);
    s.contrast = Lim11(contrast);
    s.warmth = Lim11(warm);
    s.tint = Lim11(tint);
    s.saturation = Lim11(sat);
    s.shadow = Lim11(shadow);
    s.grain = Clamp01(grain);
    s.sharpen = Clamp01(sharp);
    return true;
}

static void StoreClipGrade(Clip& c, const BusSegment& s) {
    c.gradeLight = s.lightness;
    c.gradeContrast = s.contrast;
    c.gradeWarm = s.warmth;
    c.gradeTint = s.tint;
    c.gradeSat = s.saturation;
    c.gradeShadow = s.shadow;
    c.gradeGrain = s.grain;
    c.gradeSharp = s.sharpen;
}

static bool WriteXformLook(const std::string& path, const Clip& c) {
    FILE* f = LookCreate(path, "xform");
    if (!f) return false;
    fprintf(f, "x %.5f\ny %.5f\nsize %.5f\naspect %.5f\nrot %.5f\n", c.gsX, c.gsY, c.gsScale, c.gsAspect, c.gsRot);
    fclose(f);
    return true;
}

static bool ReadXformLook(const std::string& path, Clip& c) {
    FILE* f = nullptr;
    if (!LookOpen(path, "xform", f)) return false;
    float x, y, size, aspect, rot;
    bool ok = TakeF(f, "x", x) && TakeF(f, "y", y) && TakeF(f, "size", size) && TakeF(f, "aspect", aspect) && TakeF(f, "rot", rot);
    fclose(f);
    if (!ok) return false;
    c.gsX = LimRange(x, -1.0f, 1.0f);
    c.gsY = LimRange(y, -1.0f, 1.0f);
    c.gsScale = LimRange(size, 0.05f, 4.0f);
    c.gsAspect = LimRange(aspect, 0.25f, 4.0f);
    c.gsRot = LimRange(rot, -180.0f, 180.0f);
    c.gsFit = 2;
    return true;
}

static bool WriteGsLook(const std::string& path, const Clip& c) {
    FILE* f = LookCreate(path, "gs");
    if (!f) return false;
    fprintf(f, "x %.5f\ny %.5f\nsize %.5f\naspect %.5f\nrot %.5f\nsim %.5f\nsoft %.5f\nspill %.5f\nopacity %.5f\n",
            c.gsX, c.gsY, c.gsScale, c.gsAspect, c.gsRot, c.keySim, c.keySoft, c.keySpill, c.gsOpacity);
    fclose(f);
    return true;
}

static bool ReadGsLook(const std::string& path, Clip& c) {
    FILE* f = nullptr;
    if (!LookOpen(path, "gs", f)) return false;
    float x, y, size, aspect, rot, sim, soft, spill, opacity;
    bool ok = TakeF(f, "x", x) && TakeF(f, "y", y) && TakeF(f, "size", size) && TakeF(f, "aspect", aspect)
        && TakeF(f, "rot", rot) && TakeF(f, "sim", sim) && TakeF(f, "soft", soft) && TakeF(f, "spill", spill)
        && TakeF(f, "opacity", opacity);
    fclose(f);
    if (!ok) return false;
    c.gsX = LimRange(x, -1.0f, 1.0f);
    c.gsY = LimRange(y, -1.0f, 1.0f);
    c.gsScale = LimRange(size, 0.05f, 4.0f);
    c.gsAspect = LimRange(aspect, 0.25f, 4.0f);
    c.gsRot = LimRange(rot, -180.0f, 180.0f);
    c.keySim = Clamp01(sim);
    c.keySoft = Clamp01(soft);
    c.keySpill = Clamp01(spill);
    c.gsOpacity = Clamp01(opacity);
    c.gsFit = 2;
    return true;
}

static bool WriteAudioLook(const std::string& path, const Clip& c) {
    FILE* f = LookCreate(path, "audio");
    if (!f) return false;
    fprintf(f, "eq %d\n", c.eqOn ? 1 : 0);
    for (int i = 0; i < 8; i++) fprintf(f, "g%d %.5f\n", i, c.eqGain[i]);
    fprintf(f, "reverb %d\ntime %.5f\ndelay %d\ntempo %.5f\n", c.reverbOn ? 1 : 0, c.reverbTime, c.delayOn ? 1 : 0, c.delayTempo);
    fclose(f);
    return true;
}

static bool ReadAudioLook(const std::string& path, Clip& c) {
    FILE* f = nullptr;
    if (!LookOpen(path, "audio", f)) return false;
    int eq = 0, reverb = 0, delay = 0;
    float g[8], time = 0, tempo = 0;
    bool ok = TakeI(f, "eq", eq);
    for (int i = 0; ok && i < 8; i++) {
        char key[8];
        snprintf(key, sizeof(key), "g%d", i);
        ok = TakeF(f, key, g[i]);
    }
    ok = ok && TakeI(f, "reverb", reverb) && TakeF(f, "time", time) && TakeI(f, "delay", delay) && TakeF(f, "tempo", tempo);
    fclose(f);
    if (!ok) return false;
    c.eqOn = eq != 0;
    for (int i = 0; i < 8; i++) c.eqGain[i] = LimRange(g[i], -12.0f, 12.0f);
    c.reverbOn = reverb != 0;
    c.reverbTime = LimRange(time, 0.2f, 5.0f);
    c.delayOn = delay != 0;
    c.delayTempo = LimRange(tempo, 40.0f, 240.0f);
    c.fx.clear();
    return true;
}

static bool WriteStyleLook(const std::string& path, const SubStyle& s) {
    FILE* f = LookCreate(path, "style");
    if (!f) return false;
    fprintf(f, "tr %.5f\ntg %.5f\ntb %.5f\nborder %d\npx %.5f\ner %.5f\neg %.5f\neb %.5f\nbgon %d\nbr %.5f\nbg %.5f\nbb %.5f\nalpha %.5f\nscale %.5f\n",
            s.text[0], s.text[1], s.text[2], s.border ? 1 : 0, s.borderPx,
            s.edge[0], s.edge[1], s.edge[2], s.background ? 1 : 0,
            s.bg[0], s.bg[1], s.bg[2], s.bgAlpha, s.fontScale);
    fclose(f);
    return true;
}

static bool ReadStyleLook(const std::string& path, SubStyle& s) {
    FILE* f = nullptr;
    if (!LookOpen(path, "style", f)) return false;
    float tr, tg, tb, px, er, eg, eb, br, bg, bb, alpha, scale;
    int border = 0, bgon = 0;
    bool ok = TakeF(f, "tr", tr) && TakeF(f, "tg", tg) && TakeF(f, "tb", tb) && TakeI(f, "border", border)
        && TakeF(f, "px", px) && TakeF(f, "er", er) && TakeF(f, "eg", eg) && TakeF(f, "eb", eb)
        && TakeI(f, "bgon", bgon) && TakeF(f, "br", br) && TakeF(f, "bg", bg) && TakeF(f, "bb", bb)
        && TakeF(f, "alpha", alpha) && TakeF(f, "scale", scale);
    fclose(f);
    if (!ok) return false;
    s.text[0] = Clamp01(tr); s.text[1] = Clamp01(tg); s.text[2] = Clamp01(tb);
    s.border = border != 0;
    s.borderPx = LimRange(px, 0.0f, 6.0f);
    s.edge[0] = Clamp01(er); s.edge[1] = Clamp01(eg); s.edge[2] = Clamp01(eb);
    s.background = bgon != 0;
    s.bg[0] = Clamp01(br); s.bg[1] = Clamp01(bg); s.bg[2] = Clamp01(bb);
    s.bgAlpha = Clamp01(alpha);
    s.fontScale = LimRange(scale, 0.4f, 2.5f);
    return true;
}

static bool WriteFxLook(const std::string& path, const OverlayFx& o) {
    FILE* f = LookCreate(path, "overlay");
    if (!f) return false;
    fprintf(f, "kind %d\nx %.5f\ny %.5f\nsize %.5f\nrot %.5f\nopacity %.5f\namount %.5f\ndir %d\nk0 %.5f\nk1 %.5f\nk2 %.5f\nk3 %.5f\n",
            o.kind, o.x, o.y, o.scale, o.rot, o.opacity, o.amount, o.dir < 0 ? -1 : 1,
            o.knob[0], o.knob[1], o.knob[2], o.knob[3]);
    fclose(f);
    return true;
}

static bool ReadFxLook(const std::string& path, OverlayFx& o) {
    FILE* f = nullptr;
    if (!LookOpen(path, "overlay", f)) return false;
    int kind = 0, dir = 1;
    float x, y, size, rot, opacity, amount;
    bool ok = TakeI(f, "kind", kind) && TakeF(f, "x", x) && TakeF(f, "y", y) && TakeF(f, "size", size)
        && TakeF(f, "rot", rot) && TakeF(f, "opacity", opacity) && TakeF(f, "amount", amount) && TakeI(f, "dir", dir);
    float k0 = 0, k1 = 0, k2 = 0, k3 = 0;
    bool gotK = ok && TakeF(f, "k0", k0) && TakeF(f, "k1", k1) && TakeF(f, "k2", k2) && TakeF(f, "k3", k3);
    fclose(f);
    if (!ok || kind < 0 || kind >= OverlayKindCount()) return false;
    o.kind = kind;
    o.x = LimRange(x, -1.0f, 1.0f);
    o.y = LimRange(y, -1.0f, 1.0f);
    o.scale = LimRange(size, 0.15f, 4.0f);
    o.rot = LimRange(rot, -180.0f, 180.0f);
    o.opacity = Clamp01(opacity);
    o.amount = Clamp01(amount);
    o.dir = dir < 0 ? -1 : 1;
    if (gotK) {
        o.knob[0] = Clamp01(k0);
        o.knob[1] = Clamp01(k1);
        o.knob[2] = Clamp01(k2);
        o.knob[3] = Clamp01(k3);
    } else if (kind == 26) {
        o.knob[0] = 0.55f;
        o.knob[1] = 0.35f;
        o.knob[2] = o.knob[3] = 0;
    } else if (kind == 27) {
        o.knob[0] = 0.0f;
        o.knob[1] = 0.20f;
        o.knob[2] = 0.65f;
        o.knob[3] = 1.0f;
    } else if (kind == 28) {
        o.knob[0] = 0.45f;
        o.knob[1] = o.knob[2] = o.knob[3] = 0;
    }
    return true;
}

static int LookNameCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) return 0;
    LookBin* bin = (LookBin*)data->UserData;
    if (!bin) return 0;
    std::string s(data->Buf, (size_t)data->BufTextLen);
    bool heb = HasHebrew(s);
    if (heb && bin->arm) {
        data->CursorPos = 0;
        data->SelectionStart = 0;
        data->SelectionEnd = 0;
        bin->arm = false;
    }
    if (heb && data->BufTextLen > bin->chars) {
        data->CursorPos = 0;
        data->SelectionStart = 0;
        data->SelectionEnd = 0;
    }
    if (heb && ImGui::IsKeyPressed(ImGuiKey_Backspace) && data->CursorPos == 0 && data->BufTextLen > 0) {
        int n = Utf8CharLen(data->Buf);
        if (n > data->BufTextLen) n = data->BufTextLen;
        data->DeleteChars(0, n);
    }
    bin->chars = data->BufTextLen;
    return 0;
}

template <typename SaveFn, typename LoadFn>
static void RunLook(const char* kind, SaveFn save, LoadFn load) {
    LookBin* bin = LookBinFor(kind);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(128.0f);
    ImGui::InputText("##lookname", bin->typed, sizeof(bin->typed), ImGuiInputTextFlags_CallbackAlways, LookNameCallback, bin);
    ImGui::SameLine();
    if (ImGui::Button("SAVE")) {
        std::string visual = bin->typed;
        std::string logical = HasHebrew(visual) ? ReverseUtf8(visual) : visual;
        std::string name = CleanLookName(logical.c_str());
        if (name.empty()) previewStatus = "Type a name";
        else {
            std::string path = LookFilePath(kind, name);
            previewStatus = save(path) ? "Look saved" : "Could not save look";
            RefreshLookBin(*bin);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves under your Windows profile");
    ImGui::TextUnformatted("LOAD");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.26f, 0.34f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.50f, 0.74f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.42f, 0.64f, 1.0f));
    if (bin->names.empty()) {
        ImGui::TextUnformatted("Nothing saved yet");
    } else {
        int idx = 0;
        int shownN = (int)bin->names.size();
        bool scroll = shownN > 6;
        if (scroll) ImGui::BeginChild("##savedlooks", ImVec2(260.0f, 6.0f * ImGui::GetFrameHeightWithSpacing() + 4.0f), false);
        for (const std::string& name : bin->names) {
            std::string shown = HasHebrew(name) ? ReverseUtf8(name) : name;
            ImGui::PushID(idx++);
            if (ImGui::Button(shown.c_str(), ImVec2(248.0f, 0.0f))) {
                snprintf(bin->typed, sizeof(bin->typed), "%s", shown.c_str());
                bin->chars = (int)strlen(bin->typed);
                bin->arm = HasHebrew(shown);
                previewStatus = load(LookFilePath(kind, name)) ? "Look loaded" : "Could not load look";
            }
            ImGui::PopID();
        }
        if (scroll) ImGui::EndChild();
    }
    ImGui::PopStyleColor(4);
}

void ClearProject() {
    for (auto& m : media) {
        if (m.thumb) { glDeleteTextures(1, &m.thumb); m.thumb = 0; }
    }
    media.clear();
    clips.clear();
    bus.clear();
    subs.clear();
    asrOn = false;
    AsrClose();
    selectedClip = -1;
    selectedBus = -1;
    selectedSub = -1;
    subSelection.clear();
    marqueeLive = false;
    marqueeKind = 0;
    editingSub = -1;
    subEditorOpen = false;
    subStyleOpen = false;
    gsPanelOpen = false;
    gsPanelClip = -1;
    selection.clear();
    draggingClip = -1;
    trimmingClip = -1;
    dragVolClip = -1;
    contextClip = -1;
    fxEdit = -1;
    mediaMenuIndex = -1;
    mediaSel.clear();
    floatMedia = -1;
    floatMediaOpen = false;
    libPlayId = -1;
    OverlayClear();
    colorOpen = false;
    ovPanelOpen = false;
    draggingOv = -1;
    trimmingOv = -1;
    currentTime = 0;
    shownLocal = -1000.0f;
    masterGain = 1.0f;
    masterRawL = 0.0f;
    masterRawR = 0.0f;
    repeatOn = false;
#ifdef HAVE_FFMPEG
    SetPlaying(false);
    for (auto& a : audioSrcs) CloseAudioSrc(a);
    audioSrcs.clear();
    if (player) player->Close();
    if (playerB) playerB->Close();
    if (playerUnder) playerUnder->Close();
    if (playerNext) playerNext->Close();
    if (playerGS) playerGS->Close();
    if (playerLib) playerLib->Close();
    gRam.Clear();
    libPlayId = -1;
    playingMediaId = -1;
    playingClip = -1;
    playerFileId = -1;
    playingMediaIdB = -1;
    playingClipB = -1;
    playingMediaIdUnder = -1;
    playingClipUnder = -1;
    playingGsClip = -1;
    playingGsMedia = -1;
    nextMediaId = -1;
    nextClipIdx = -1;
#else
    isPlaying = false;
#endif
}

void RemoveMedia(int id) {
    if (id < 0 || id >= (int)media.size()) return;
    if (media[id].thumb && !IsSolidPath(media[id].path)) {
        glDeleteTextures(1, &media[id].thumb);
    }
    media[id].thumb = 0;
    for (int i = (int)clips.size() - 1; i >= 0; --i) {
        if (clips[i].mediaId == id) clips.erase(clips.begin() + i);
        else if (clips[i].mediaId > id) clips[i].mediaId--;
    }
    media.erase(media.begin() + id);
    selectedClip = -1;
    selection.clear();
    draggingClip = -1;
    trimmingClip = -1;
    dragVolClip = -1;
    contextClip = -1;
    fxEdit = -1;
    mediaMenuIndex = -1;
    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
    for (auto& a : audioSrcs) CloseAudioSrc(a);
    audioSrcs.clear();
    if (player) player->Close();
    if (playerB) playerB->Close();
    if (playerUnder) playerUnder->Close();
    if (playerNext) playerNext->Close();
    if (playerGS) playerGS->Close();
    if (playerLib) playerLib->Close();
    gRam.Forget(id);
    libPlayId = -1;
    playingMediaId = -1;
    playingClip = -1;
    playerFileId = -1;
    playingMediaIdB = -1;
    playingClipB = -1;
    playingMediaIdUnder = -1;
    playingClipUnder = -1;
    playingGsClip = -1;
    playingGsMedia = -1;
    nextMediaId = -1;
    nextClipIdx = -1;
    ResetAudioSync();
#endif
}

void DeleteSelectedMedia() {
    std::vector<int> ids = mediaSel;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (int k = (int)ids.size() - 1; k >= 0; --k) {
        int id = ids[k];
        if (id < 0 || id >= (int)media.size()) continue;
        if (floatMedia == id) { floatMediaOpen = false; floatMedia = -1; libPlayId = -1; }
        else if (floatMedia > id) floatMedia--;
        RemoveMedia(id);
    }
    mediaSel.clear();
}

float TimelineEnd() {
    float end = 0.0f;
    for (const auto& c : clips) {
        float e = c.startTime + c.duration;
        if (c.reverbOn) e += std::min(5.0f, c.reverbTime);
        if (c.delayOn) e += 180.0f / std::max(40.0f, c.delayTempo);
        end = std::max(end, e);
    }
    return end;
}

void StopIfFinished() {
    if (!isPlaying || draggingPlayhead) return;
    float end = TimelineEnd();
    if (clips.empty() || currentTime < end - 0.02f) return;
    if (repeatOn) {
        currentTime = 0;
        shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
        ResetAudioSync();
        if (player) player->lastPts = -1;
        if (playerB) playerB->lastPts = -1;
        if (playerGS) playerGS->lastPts = -1;
        if (playerNext) playerNext->lastPts = -1;
#endif
        return;
    }
#ifdef HAVE_FFMPEG
    SetPlaying(false);
#else
    isPlaying = false;
#endif
    if (currentTime > end) currentTime = end;
    shownLocal = -1000.0f;
}

bool SaveProject(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "DWP1\n");
    fprintf(f, "time %.6f\n", currentTime);
    fprintf(f, "snap %d\n", snapEnabled ? 1 : 0);
    fprintf(f, "repeat %d\n", repeatOn ? 1 : 0);
    fprintf(f, "pps %.3f\n", pixelsPerSecond);
    fprintf(f, "master %.4f\n", masterGain);
    fprintf(f, "aspect %d\n", exAspect);
    for (const auto& m : media) {
        std::string p = m.path;
        for (char& ch : p) if (ch == '\n' || ch == '\r') ch = ' ';
        fprintf(f, "M %s %.6f %.6f\n%s\n", m.type.c_str(), m.duration, m.aspect, p.c_str());
    }
    for (const auto& c : clips) {
        fprintf(f, "C %d %.6f %.6f %.6f %.6f %d %d %d %d\n",
                c.mediaId, c.startTime, c.duration, c.sourceOffset, c.speed,
                c.fadeIn ? 1 : 0, c.fadeOut ? 1 : 0, c.track, (int)c.volume.size());
        for (const auto& v : c.volume)
            fprintf(f, "V %.6f %.6f\n", v.norm, v.gain);
        fprintf(f, "R %.4f %.4f %.4f %.4f\n", c.opIn, c.opOut, c.volIn, c.volOut);
        fprintf(f, "F %d %d %d %.4f %.4f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
                c.eqOn ? 1 : 0, c.reverbOn ? 1 : 0, c.delayOn ? 1 : 0, c.reverbTime, c.delayTempo,
                c.eqGain[0], c.eqGain[1], c.eqGain[2], c.eqGain[3],
                c.eqGain[4], c.eqGain[5], c.eqGain[6], c.eqGain[7]);
        fprintf(f, "E %s %.4f\n", c.fxOut.empty() ? "-" : c.fxOut.c_str(), c.fxDur);
        fprintf(f, "K %.3f %.3f %.3f %.3f %.3f %.3f %d %.3f %.3f %.2f %d\n",
                c.keySim, c.keySoft, c.keySpill, c.gsX, c.gsY, c.gsScale, c.muted ? 1 : 0, c.gsOpacity, c.gsAspect, c.gsRot, c.gsFit);
        fprintf(f, "G %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
                c.gradeLight, c.gradeContrast, c.gradeWarm, c.gradeTint, c.gradeSat, c.gradeShadow, c.gradeGrain, c.gradeSharp);
    }
    for (const auto& b : bus) {
        fprintf(f, "B %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                b.start, b.duration, b.lightness, b.contrast, b.warmth, b.tint,
                b.saturation, b.shadow, b.grain, b.sharpen);
    }
    for (const auto& o : gOverlays) {
        fprintf(f, "O %d %.4f %.4f %.3f %.3f %.3f %.2f %.3f %.3f %d\n",
                o.kind, o.start, o.duration, o.x, o.y, o.scale, o.rot, o.opacity, o.amount, o.dir < 0 ? -1 : 1);
        if (o.kind == 26 || o.kind == 27 || o.kind == 28)
            fprintf(f, "N %.4f %.4f %.4f %.4f\n", o.knob[0], o.knob[1], o.knob[2], o.knob[3]);
        for (int ch = 0; ch < 7; ch++) {
            if (o.holdOn[ch]) fprintf(f, "H %d %.5f\n", ch, o.hold[ch]);
            for (const auto& k : o.anim[ch]) fprintf(f, "P %d %.4f %.5f\n", ch, k.t, k.v);
        }
    }
    fprintf(f, "Y %.3f %.3f %.3f %d %.2f %.3f %.3f %.3f %.3f %.3f %.3f %d\n",
            subStyle.text[0], subStyle.text[1], subStyle.text[2], subStyle.border ? 1 : 0, subStyle.borderPx,
            subStyle.edge[0], subStyle.edge[1], subStyle.edge[2],
            subStyle.bg[0], subStyle.bg[1], subStyle.bg[2], subStyle.background ? 1 : 0);
    fprintf(f, "Z %.3f %.3f\n", subStyle.bgAlpha, subStyle.fontScale);
    for (const auto& s : subs) {
        std::string t = s.text;
        for (char& ch : t) if (ch == '\n' || ch == '\r') ch = ' ';
        fprintf(f, "U %.4f %.4f %s\n", s.start, s.duration, t.c_str());
    }
    fclose(f);
    return true;
}

struct EditSnap {
    struct M {
        std::string path, name, type;
        float duration = 5.0f;
        float aspect = 16.0f / 9.0f;
    };
    std::vector<M> media;
    std::vector<Clip> clips;
    std::vector<BusSegment> bus;
    std::vector<Subtitle> subs;
    SubStyle style;
    std::vector<OverlayFx> overlays;
    int aspect = 0;
};

static Clip ClipWithoutFx(const Clip& s) {
    Clip d;
    d.mediaId = s.mediaId;
    d.startTime = s.startTime;
    d.duration = s.duration;
    d.sourceOffset = s.sourceOffset;
    d.speed = s.speed;
    d.fadeIn = s.fadeIn;
    d.fadeOut = s.fadeOut;
    d.opIn = s.opIn;
    d.opOut = s.opOut;
    d.volIn = s.volIn;
    d.volOut = s.volOut;
    d.audioOpen = s.audioOpen;
    d.track = s.track;
    d.volume = s.volume;
    d.eqOn = s.eqOn;
    memcpy(d.eqGain, s.eqGain, sizeof(d.eqGain));
    d.reverbOn = s.reverbOn;
    d.reverbTime = s.reverbTime;
    d.delayOn = s.delayOn;
    d.delayTempo = s.delayTempo;
    d.fxOut = s.fxOut;
    d.fxDur = s.fxDur;
    d.keySim = s.keySim;
    d.keySoft = s.keySoft;
    d.keySpill = s.keySpill;
    d.gsX = s.gsX;
    d.gsY = s.gsY;
    d.gsScale = s.gsScale;
    d.gsAspect = s.gsAspect;
    d.gsRot = s.gsRot;
    d.gsOpacity = s.gsOpacity;
    d.gsFit = s.gsFit;
    d.muted = s.muted;
    d.gradeLight = s.gradeLight;
    d.gradeContrast = s.gradeContrast;
    d.gradeWarm = s.gradeWarm;
    d.gradeTint = s.gradeTint;
    d.gradeSat = s.gradeSat;
    d.gradeShadow = s.gradeShadow;
    d.gradeGrain = s.gradeGrain;
    d.gradeSharp = s.gradeSharp;
    return d;
}

static EditSnap CaptureEdit() {
    EditSnap e;
    e.aspect = exAspect;
    e.style = subStyle;
    e.bus = bus;
    e.subs = subs;
    e.overlays = gOverlays;
    for (const auto& m : media) {
        EditSnap::M x;
        x.path = m.path;
        x.name = m.name;
        x.type = m.type;
        x.duration = m.duration;
        x.aspect = m.aspect;
        e.media.push_back(x);
    }
    for (const auto& c : clips) e.clips.push_back(ClipWithoutFx(c));
    return e;
}

static std::string EditSignature(const EditSnap& e) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << e.aspect << '\n' << e.media.size() << '\n';
    for (const auto& m : e.media)
        o << m.type << '\t' << m.duration << '\t' << m.aspect << '\t' << m.path << '\n';
    o << e.clips.size() << '\n';
    for (const auto& c : e.clips) {
        o << c.mediaId << '\t' << c.startTime << '\t' << c.duration << '\t' << c.sourceOffset << '\t'
          << c.speed << '\t' << c.fadeIn << c.fadeOut << '\t' << c.opIn << '\t' << c.opOut << '\t'
          << c.volIn << '\t' << c.volOut << '\t' << c.track << '\t' << c.muted << '\t'
          << c.fxOut << '\t' << c.fxDur << '\t'
          << c.keySim << '\t' << c.keySoft << '\t' << c.keySpill << '\t'
          << c.gsX << '\t' << c.gsY << '\t' << c.gsScale << '\t' << c.gsAspect << '\t' << c.gsRot << '\t' << c.gsOpacity << '\t' << c.gsFit << '\t'
          << c.gradeLight << '\t' << c.gradeContrast << '\t' << c.gradeWarm << '\t' << c.gradeTint << '\t'
          << c.gradeSat << '\t' << c.gradeShadow << '\t' << c.gradeGrain << '\t' << c.gradeSharp << '\t'
          << c.eqOn << c.reverbOn << c.delayOn << '\t' << c.reverbTime << '\t' << c.delayTempo;
        for (float g : c.eqGain) o << '\t' << g;
        o << '\t' << c.volume.size();
        for (const auto& v : c.volume) o << '\t' << v.norm << '\t' << v.gain;
        o << '\n';
    }
    o << e.bus.size() << '\n';
    for (const auto& b : e.bus)
        o << b.start << '\t' << b.duration << '\t' << b.lightness << '\t' << b.contrast << '\t' << b.warmth << '\t'
          << b.tint << '\t' << b.saturation << '\t' << b.shadow << '\t' << b.grain << '\t' << b.sharpen << '\n';
    o << e.style.text[0] << '\t' << e.style.text[1] << '\t' << e.style.text[2] << '\t' << e.style.border << '\t'
      << e.style.borderPx << '\t' << e.style.edge[0] << '\t' << e.style.edge[1] << '\t' << e.style.edge[2] << '\t'
      << e.style.background << '\t' << e.style.bg[0] << '\t' << e.style.bg[1] << '\t' << e.style.bg[2] << '\t'
      << e.style.bgAlpha << '\t' << e.style.fontScale << '\n';
    o << e.subs.size() << '\n';
    for (const auto& s : e.subs) o << s.start << '\t' << s.duration << '\t' << s.text << '\n';
    o << e.overlays.size() << '\n';
    for (const auto& fx : e.overlays) {
        o << fx.kind << '\t' << fx.start << '\t' << fx.duration << '\t' << fx.x << '\t' << fx.y << '\t'
          << fx.scale << '\t' << fx.rot << '\t' << fx.opacity << '\t' << fx.amount << '\t' << fx.dir << '\t' << fx.rec;
        for (int ch = 0; ch < 7; ch++) {
            o << '\t' << fx.holdOn[ch] << '\t' << fx.hold[ch] << '\t' << fx.anim[ch].size();
            for (const auto& k : fx.anim[ch]) o << '\t' << k.t << '\t' << k.v;
        }
        o << '\n';
    }
    return o.str();
}

static std::deque<EditSnap> undoStack;
static EditSnap undoBaseline;
static std::string undoBaselineSig;
static bool undoReady = false;
static bool undoGesture = false;
static int undoRequest = 0;

static void ApplyEdit(const EditSnap& e) {
    bool sameMedia = e.media.size() == media.size();
    if (sameMedia) {
        for (size_t i = 0; i < e.media.size(); i++) {
            if (e.media[i].path != media[i].path || e.media[i].type != media[i].type) sameMedia = false;
        }
    }
    if (!sameMedia) {
        std::vector<int> steal(e.media.size(), -1);
        std::vector<char> used(media.size(), 0);
        for (size_t i = 0; i < e.media.size(); i++) {
            for (size_t j = 0; j < media.size(); j++) {
                if (!used[j] && media[j].path == e.media[i].path) {
                    steal[i] = (int)j;
                    used[j] = 1;
                    break;
                }
            }
        }
        std::vector<MediaItem> next;
        next.reserve(e.media.size());
        for (size_t i = 0; i < e.media.size(); i++) {
            MediaItem m;
            if (steal[i] >= 0) {
                m = media[steal[i]];
                media[steal[i]].thumb = 0;
            }
            m.path = e.media[i].path;
            m.name = e.media[i].name;
            m.type = e.media[i].type;
            m.duration = e.media[i].duration;
            m.aspect = e.media[i].aspect;
            if (!m.thumb) MakeThumbnail(m);
            next.push_back(m);
        }
        for (auto& m : media) {
            if (m.thumb) glDeleteTextures(1, &m.thumb);
        }
        media.swap(next);
        mediaSel.clear();
        floatMedia = -1;
        floatMediaOpen = false;
#ifdef HAVE_FFMPEG
        for (auto& a : audioSrcs) CloseAudioSrc(a);
        audioSrcs.clear();
        if (player) player->Close();
        if (playerB) playerB->Close();
        if (playerUnder) playerUnder->Close();
        if (playerNext) playerNext->Close();
        if (playerGS) playerGS->Close();
        if (playerLib) playerLib->Close();
        gRam.Clear();
        libPlayId = -1;
        playingMediaId = -1;
        playingClip = -1;
        playerFileId = -1;
        playingMediaIdB = -1;
        playingClipB = -1;
        playingMediaIdUnder = -1;
        playingClipUnder = -1;
        playingGsClip = -1;
        playingGsMedia = -1;
        nextMediaId = -1;
        nextClipIdx = -1;
        ResetAudioSync();
#endif
    }
    clips.clear();
    for (const auto& c : e.clips) clips.push_back(ClipWithoutFx(c));
    bus = e.bus;
    subs = e.subs;
    subStyle = e.style;
    gOverlays = e.overlays;
    exAspect = e.aspect;
    if (selectedClip >= (int)clips.size()) selectedClip = -1;
    selection.erase(std::remove_if(selection.begin(), selection.end(), [](int i) { return i < 0 || i >= (int)clips.size(); }), selection.end());
    if (selectedBus >= (int)bus.size()) selectedBus = -1;
    if (contextBus >= (int)bus.size()) contextBus = -1;
    if (selectedSub >= (int)subs.size()) selectedSub = -1;
    subSelection.erase(std::remove_if(subSelection.begin(), subSelection.end(), [](int i) { return i < 0 || i >= (int)subs.size(); }), subSelection.end());
    if (editingSub < 0 || editingSub >= (int)subs.size()) {
        editingSub = -1;
        subEditorOpen = false;
    } else if (subEditorOpen) {
        LoadSubEditor(subs[editingSub].text);
    }
    if (gOverlaySel >= (int)gOverlays.size()) gOverlaySel = -1;
    if (ovPanel < 0 || ovPanel >= (int)gOverlays.size()) {
        ovPanel = -1;
        ovPanelOpen = false;
    }
    draggingClip = -1;
    trimmingClip = -1;
    dragVolClip = -1;
    draggingOv = -1;
    trimmingOv = -1;
    contextClip = -1;
    shownLocal = -1000.0f;
    ImGui::ClearActiveID();
}

static void NoteUndoBaseline() {
    undoBaseline = CaptureEdit();
    undoBaselineSig = EditSignature(undoBaseline);
    undoReady = true;
    undoGesture = false;
}

static void UndoTick() {
    if (asrOn || gExport.active) return;
    EditSnap now = CaptureEdit();
    std::string sig = EditSignature(now);
    if (!undoReady) {
        undoBaseline = std::move(now);
        undoBaselineSig = std::move(sig);
        undoReady = true;
        return;
    }
    bool hot = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1);
    if (hot) {
        if (!undoGesture && sig != undoBaselineSig) undoGesture = true;
        return;
    }
    if (sig != undoBaselineSig) {
        undoStack.push_back(std::move(undoBaseline));
        while (undoStack.size() > 20) undoStack.pop_front();
        undoBaseline = std::move(now);
        undoBaselineSig = std::move(sig);
    }
    undoGesture = false;
}

static void PerformUndo() {
    if (undoGesture) {
        EditSnap now = CaptureEdit();
        if (EditSignature(now) != undoBaselineSig) {
            ApplyEdit(undoBaseline);
            NoteUndoBaseline();
            previewStatus = "Undo";
            if (undoRequest > 0) undoRequest--;
            if (undoRequest <= 0) return;
        }
    }
    if (undoStack.empty()) {
        undoRequest = 0;
        return;
    }
    while (undoRequest > 0 && !undoStack.empty()) {
        EditSnap snap = std::move(undoStack.back());
        undoStack.pop_back();
        ApplyEdit(snap);
        undoRequest--;
    }
    undoRequest = 0;
    NoteUndoBaseline();
    previewStatus = "Undo";
}

static void TrimLine(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

bool LoadProject(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[8192];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "DWP1", 4) != 0) {
        fclose(f);
        return false;
    }
    ClearProject();
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "time ", 5) == 0) {
            currentTime = (float)atof(line + 5);
        } else if (strncmp(line, "snap ", 5) == 0) {
            snapEnabled = atoi(line + 5) != 0;
        } else if (strncmp(line, "repeat ", 7) == 0) {
            repeatOn = atoi(line + 7) != 0;
        } else if (strncmp(line, "pps ", 4) == 0) {
            float p = (float)atof(line + 4);
            if (p >= 1.0f && p <= 400.0f) pixelsPerSecond = p;
        } else if (strncmp(line, "master ", 7) == 0) {
            float g = (float)atof(line + 7);
            if (g < 0.0f) g = 0.0f;
            if (g > 1.5f) g = 1.5f;
            masterGain = g;
        } else if (strncmp(line, "aspect ", 7) == 0) {
            int a = atoi(line + 7);
            if (a >= 0 && a <= 4) exAspect = a;
        } else if (line[0] == 'M') {
            char type[32] = {0};
            float dur = 5.0f, aspect = 16.0f / 9.0f;
            if (sscanf(line, "M %31s %f %f", type, &dur, &aspect) == 3 && fgets(line, sizeof(line), f)) {
                std::string p = line;
                TrimLine(p);
                MediaItem m;
                m.path = p;
                m.type = type;
                m.duration = dur > 0.05f ? dur : 5.0f;
                m.aspect = aspect > 0.01f ? aspect : (16.0f / 9.0f);
                size_t slash = p.find_last_of("/\\");
                m.name = (slash == std::string::npos) ? p : p.substr(slash + 1);
                media.push_back(m);
            }
        } else if (line[0] == 'C') {
            Clip c;
            int fi = 0, fo = 0, nvol = 0;
            if (sscanf(line, "C %d %f %f %f %f %d %d %d %d",
                       &c.mediaId, &c.startTime, &c.duration, &c.sourceOffset, &c.speed,
                       &fi, &fo, &c.track, &nvol) == 9) {
                c.fadeIn = fi != 0;
                c.fadeOut = fo != 0;
                if (c.speed < 0.05f) c.speed = 1.0f;
                if (nvol < 0) nvol = 0;
                if (nvol > 64) nvol = 64;
                for (int i = 0; i < nvol; i++) {
                    if (!fgets(line, sizeof(line), f)) break;
                    VolPoint vp;
                    if (sscanf(line, "V %f %f", &vp.norm, &vp.gain) == 2) c.volume.push_back(vp);
                }
                clips.push_back(c);
            }
        } else if (line[0] == 'R' && line[1] == ' ' && !clips.empty()) {
            float a = 0, b = 0, vi = 0, vo = 0;
            if (sscanf(line, "R %f %f %f %f", &a, &b, &vi, &vo) == 4) {
                Clip& cl = clips.back();
                auto lim = [&](float v) {
                    if (v < 0) v = 0;
                    if (v > cl.duration) v = cl.duration;
                    return v;
                };
                cl.opIn = lim(a);
                cl.opOut = lim(b);
                cl.volIn = lim(vi);
                cl.volOut = lim(vo);
            }
        } else if (line[0] == 'F' && line[1] == ' ' && !clips.empty()) {
            Clip& c = clips.back();
            int eq = 0, rv = 0, dl = 0;
            float rt = 1.4f, tp = 120.0f;
            float e[8] = {};
            if (sscanf(line, "F %d %d %d %f %f %f %f %f %f %f %f %f %f",
                       &eq, &rv, &dl, &rt, &tp,
                       &e[0], &e[1], &e[2], &e[3], &e[4], &e[5], &e[6], &e[7]) == 13) {
                c.eqOn = eq != 0;
                c.reverbOn = rv != 0;
                c.delayOn = dl != 0;
                c.reverbTime = rt;
                c.delayTempo = tp;
                for (int i = 0; i < 8; i++) {
                    if (e[i] > 12.0f) e[i] = 12.0f;
                    if (e[i] < -12.0f) e[i] = -12.0f;
                    c.eqGain[i] = e[i];
                }
            }
        } else if (line[0] == 'E' && line[1] == ' ' && !clips.empty()) {
            char id[64] = {0};
            float dur = 1.0f;
            if (sscanf(line, "E %63s %f", id, &dur) >= 1) {
                Clip& c = clips.back();
                if (id[0] && strcmp(id, "-") != 0 && FxKnown(id)) c.fxOut = id;
                else c.fxOut.clear();
                if (dur >= 0.1f && dur <= 5.0f) c.fxDur = dur;
            }
        } else if (line[0] == 'B' && line[1] == ' ') {
            BusSegment b;
            if (sscanf(line, "B %f %f %f %f %f %f %f %f %f %f",
                       &b.start, &b.duration, &b.lightness, &b.contrast, &b.warmth, &b.tint,
                       &b.saturation, &b.shadow, &b.grain, &b.sharpen) == 10 && b.duration > 0.05f) {
                auto lim = [](float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); };
                b.lightness = lim(b.lightness);
                b.contrast = lim(b.contrast);
                b.warmth = lim(b.warmth);
                b.tint = lim(b.tint);
                b.saturation = lim(b.saturation);
                b.shadow = lim(b.shadow);
                b.grain = Clamp01(b.grain);
                b.sharpen = Clamp01(b.sharpen);
                if (b.start < 0) b.start = 0;
                bus.push_back(b);
            }
        } else if (line[0] == 'O' && line[1] == ' ') {
            OverlayFx o;
            int dir = 1;
            int n = sscanf(line, "O %d %f %f %f %f %f %f %f %f %d", &o.kind, &o.start, &o.duration, &o.x, &o.y, &o.scale, &o.rot, &o.opacity, &o.amount, &dir);
            if ((n == 9 || n == 10) && o.duration > 0.05f) {
                if (o.kind < 0 || o.kind >= OverlayKindCount()) o.kind = 0;
                if (o.start < 0) o.start = 0;
                o.scale = o.scale < 0.15f ? 0.15f : (o.scale > 4.0f ? 4.0f : o.scale);
                o.opacity = Clamp01(o.opacity);
                o.amount = Clamp01(o.amount);
                o.dir = (n == 10 && dir < 0) ? -1 : 1;
                gOverlays.push_back(o);
            }
        } else if (line[0] == 'N' && line[1] == ' ' && !gOverlays.empty()) {
            float a = 0, b = 0, c = 0, d = 0;
            if (sscanf(line, "N %f %f %f %f", &a, &b, &c, &d) == 4) {
                auto u01 = [](float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
                OverlayFx& o = gOverlays.back();
                o.knob[0] = u01(a);
                o.knob[1] = u01(b);
                o.knob[2] = u01(c);
                o.knob[3] = u01(d);
            }
        } else if (line[0] == 'H' && line[1] == ' ' && !gOverlays.empty()) {
            int ch = 0;
            float v = 0;
            if (sscanf(line, "H %d %f", &ch, &v) == 2 && ch >= 0 && ch < 7) {
                gOverlays.back().holdOn[ch] = true;
                gOverlays.back().hold[ch] = v;
            }
        } else if (line[0] == 'P' && line[1] == ' ' && !gOverlays.empty()) {
            int ch = 0;
            float t = 0, v = 0;
            if (sscanf(line, "P %d %f %f", &ch, &t, &v) == 3 && ch >= 0 && ch < 7 && t >= 0) {
                OverlayRecord(gOverlays.back().anim[ch], t, v);
            }
        } else if (line[0] == 'G' && line[1] == ' ' && !clips.empty()) {
            float a, b, c, d, e, f0, g0, h;
            if (sscanf(line, "G %f %f %f %f %f %f %f %f", &a, &b, &c, &d, &e, &f0, &g0, &h) == 8) {
                Clip& cl = clips.back();
                cl.gradeLight = Lim11(a);
                cl.gradeContrast = Lim11(b);
                cl.gradeWarm = Lim11(c);
                cl.gradeTint = Lim11(d);
                cl.gradeSat = Lim11(e);
                cl.gradeShadow = Lim11(f0);
                cl.gradeGrain = Clamp01(g0);
                cl.gradeSharp = Clamp01(h);
            }
        } else if (line[0] == 'K' && line[1] == ' ' && !clips.empty()) {
            float a = 0.35f, b = 0.22f, ck = 0.8f, x = 0, y = 0, sc = 1, op = 1, aspect = 1, rot = 0;
            int mute = 0, fit = 0;
            int n = sscanf(line, "K %f %f %f %f %f %f %d %f %f %f %d", &a, &b, &ck, &x, &y, &sc, &mute, &op, &aspect, &rot, &fit);
            if (n >= 3) {
                clips.back().keySim = Clamp01(a);
                clips.back().keySoft = Clamp01(b);
                clips.back().keySpill = Clamp01(ck);
            }
            if (n >= 6) {
                clips.back().gsX = x < -1 ? -1 : (x > 1 ? 1 : x);
                clips.back().gsY = y < -1 ? -1 : (y > 1 ? 1 : y);
                clips.back().gsScale = sc < 0.05f ? 0.05f : (sc > 4.0f ? 4.0f : sc);
            }
            if (n >= 7) clips.back().muted = mute != 0;
            if (n >= 8) clips.back().gsOpacity = Clamp01(op);
            if (n >= 9) clips.back().gsAspect = aspect < 0.2f ? 0.2f : (aspect > 4.0f ? 4.0f : aspect);
            if (n >= 10) clips.back().gsRot = rot < -360.0f ? -360.0f : (rot > 360.0f ? 360.0f : rot);
            if (n >= 11) clips.back().gsFit = (fit == 1 || fit == 2) ? fit : 0;
        } else if (line[0] == 'Y' && line[1] == ' ') {
            float tr, tg, tb, bp, br, bg, bb, gr, gg, gb;
            int border = 1, back = 0;
            if (sscanf(line, "Y %f %f %f %d %f %f %f %f %f %f %f %d",
                       &tr, &tg, &tb, &border, &bp, &br, &bg, &bb, &gr, &gg, &gb, &back) == 12) {
                subStyle.text[0] = Clamp01(tr); subStyle.text[1] = Clamp01(tg); subStyle.text[2] = Clamp01(tb);
                subStyle.border = border != 0;
                subStyle.borderPx = bp < 0 ? 0 : (bp > 8 ? 8 : bp);
                subStyle.edge[0] = Clamp01(br); subStyle.edge[1] = Clamp01(bg); subStyle.edge[2] = Clamp01(bb);
                subStyle.bg[0] = Clamp01(gr); subStyle.bg[1] = Clamp01(gg); subStyle.bg[2] = Clamp01(gb);
                subStyle.background = back != 0;
            }
        } else if (line[0] == 'Z' && line[1] == ' ') {
            float z = 0.55f, sc = 1.0f;
            int n = sscanf(line + 2, "%f %f", &z, &sc);
            if (n >= 1) subStyle.bgAlpha = Clamp01(z);
            if (n >= 2) subStyle.fontScale = sc < 0.3f ? 0.3f : (sc > 3.0f ? 3.0f : sc);
        } else if (line[0] == 'U' && line[1] == ' ') {
            float st = 0, dur = 3;
            char* rest = line + 2;
            if (sscanf(rest, "%f %f", &st, &dur) == 2) {
                while (*rest && *rest != ' ') rest++;
                if (*rest) rest++;
                while (*rest && *rest != ' ') rest++;
                if (*rest) rest++;
                Subtitle s;
                s.start = st < 0 ? 0 : st;
                s.duration = dur < 0.2f ? 0.2f : dur;
                s.text = rest;
                TrimLine(s.text);
                subs.push_back(s);
            }
        }
    }
    fclose(f);
    for (auto& m : media) {
#ifdef HAVE_FFMPEG
        float saved = m.duration;
        if (m.type != "image") {
            float probed = ProbeDuration(m.path);
            if (probed > 0.05f) m.duration = std::max(saved, probed);
        }
        MakeThumbnail(m);
#else
        (void)m;
#endif
    }
    BakeLoadedGsImages();
    shownLocal = -1000.0f;
    return true;
}

static std::mutex gWaveMu;
static std::map<std::string, int> gWaveState;
static std::map<std::string, std::vector<unsigned char>> gWavePeaks;

#ifdef HAVE_FFMPEG
static std::vector<unsigned char> DecodeWavePeaks(const std::string& path, float durationHint) {
    std::vector<unsigned char> peaks;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return peaks;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return peaks; }
    int as = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { as = (int)i; break; }
    }
    if (as < 0) { avformat_close_input(&fmt); return peaks; }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[as]->codecpar) < 0
        || avcodec_open2(ctx, codec, nullptr) < 0 || ctx->sample_rate <= 0) {
        if (ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return peaks;
    }
    float dur = durationHint;
    if (fmt->duration > 0) dur = (float)fmt->duration / (float)AV_TIME_BASE;
    if (dur < 0.05f) dur = durationHint > 0.05f ? durationHint : 1.0f;
    int buckets = (int)(dur * 2.0f);
    if (buckets < 32) buckets = 32;
    if (buckets > 2000) buckets = 2000;
    std::vector<float> acc((size_t)buckets, 0.0f);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 1);
    AVChannelLayout fallback;
    const AVChannelLayout* inLay = &ctx->ch_layout;
    bool usedFallback = false;
    if (ctx->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&fallback, 2);
        inLay = &fallback;
        usedFallback = true;
    }
    SwrContext* swr = nullptr;
    const int rate = 8000;
    bool swrOk = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, rate,
                                     inLay, ctx->sample_fmt, ctx->sample_rate, 0, nullptr) >= 0
        && swr && swr_init(swr) >= 0;
    av_channel_layout_uninit(&outLayout);
    if (usedFallback) av_channel_layout_uninit(&fallback);
    if (!swrOk) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return peaks;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    if (!pkt || !fr) {
        if (pkt) av_packet_free(&pkt);
        if (fr) av_frame_free(&fr);
        swr_free(&swr);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return peaks;
    }
    int audioPackets = 0;
    while (audioPackets < 8000 && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != as) { av_packet_unref(pkt); continue; }
        audioPackets++;
        if (avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, fr) == 0) {
                int outCount = (int)av_rescale_rnd(swr_get_delay(swr, ctx->sample_rate) + fr->nb_samples,
                                                    rate, ctx->sample_rate, AV_ROUND_UP);
                if (outCount < 1) outCount = 1;
                std::vector<int16_t> tmp((size_t)outCount + 8);
                uint8_t* planes[1] = { (uint8_t*)tmp.data() };
                int got = swr_convert(swr, planes, outCount, (const uint8_t**)fr->data, fr->nb_samples);
                if (got <= 0) continue;
                double pts = 0;
                if (fr->best_effort_timestamp != AV_NOPTS_VALUE)
                    pts = fr->best_effort_timestamp * av_q2d(fmt->streams[as]->time_base);
                else if (fr->pts != AV_NOPTS_VALUE)
                    pts = fr->pts * av_q2d(fmt->streams[as]->time_base);
                if (pts < 0) pts = 0;
                for (int s = 0; s < got; s++) {
                    double sampleT = pts + (double)s / (double)rate;
                    int bucket = (int)(sampleT / (double)dur * (double)buckets);
                    if (bucket < 0) bucket = 0;
                    if (bucket >= buckets) bucket = buckets - 1;
                    float amp = std::fabs((float)tmp[(size_t)s]) / 32768.0f;
                    if (amp > acc[(size_t)bucket]) acc[(size_t)bucket] = amp;
                }
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    float peak = 0;
    for (float v : acc) if (v > peak) peak = v;
    peaks.assign((size_t)buckets, 0);
    if (peak > 0.0008f) {
        for (int i = 0; i < buckets; i++) {
            float n = acc[(size_t)i] / peak;
            if (n > 1) n = 1;
            peaks[(size_t)i] = (unsigned char)(n * 255.0f + 0.5f);
        }
    }
    return peaks;
}
#endif

static void RequestWave(int mediaId) {
#ifdef HAVE_FFMPEG
    if (mediaId < 0 || mediaId >= (int)media.size()) return;
    if (media[mediaId].type == "image") return;
    if (media[mediaId].path.rfind("solid:", 0) == 0) return;
    std::string path = media[mediaId].path;
    float dur = media[mediaId].duration;
    {
        std::lock_guard<std::mutex> lock(gWaveMu);
        int& st = gWaveState[path];
        if (st != 0) return;
        st = 1;
    }
    std::thread([path, dur]() {
        std::vector<unsigned char> peaks = DecodeWavePeaks(path, dur);
        std::lock_guard<std::mutex> lock(gWaveMu);
        gWavePeaks[path] = std::move(peaks);
        gWaveState[path] = 2;
    }).detach();
#else
    (void)mediaId;
#endif
}

static bool CopyWave(const std::string& path, std::vector<unsigned char>& out) {
    std::lock_guard<std::mutex> lock(gWaveMu);
    auto it = gWaveState.find(path);
    if (it == gWaveState.end() || it->second != 2) return false;
    out = gWavePeaks[path];
    return true;
}

static void DrawFadeGraph(ImDrawList* dl, ImVec2 a, ImVec2 b, float dur, float inSec, float outSec, ImU32 col) {
    if (!dl || dur < 0.05f) return;
    if (inSec <= 0.02f && outSec <= 0.02f) return;
    float w = b.x - a.x;
    float h = b.y - a.y;
    if (w < 6.0f || h < 6.0f) return;
    const int steps = 24;
    ImVec2 prev(0, 0);
    for (int i = 0; i <= steps; i++) {
        float u = (float)i / (float)steps;
        float o = RampAt(dur * u, dur, inSec, outSec);
        ImVec2 p(a.x + w * u, b.y - 1.0f - o * (h - 2.0f));
        if (i) dl->AddLine(prev, p, col, 1.6f);
        prev = p;
    }
    auto mark = [&](float local) {
        float x = a.x + w * (local / dur);
        float o = RampAt(local, dur, inSec, outSec);
        dl->AddCircleFilled(ImVec2(x, b.y - 1.0f - o * (h - 2.0f)), 3.4f, col);
    };
    if (inSec > 0.02f) mark(std::min(dur, inSec));
    if (outSec > 0.02f) mark(std::max(0.0f, dur - outSec));
}

static float VolPointY(float top, float bot, float g) {
    float h = bot - top;
    if (g < 0) g = 0;
    if (g > 1) g = 1;
    return bot - 1.0f - g * (h - 2.0f);
}

static bool VolPointHidden(const Clip& c, float norm) {
    float dur = c.duration;
    if (dur <= 0.05f) return false;
    float local = norm * dur;
    if (c.volIn > 0.02f && local < c.volIn - 0.02f) return true;
    if (c.volOut > 0.02f && dur - local < c.volOut - 0.02f) return true;
    return false;
}

static int FindVolPoint(const Clip& c, float norm, float tol) {
    int best = -1;
    float bd = tol;
    for (int i = 0; i < (int)c.volume.size(); i++) {
        float d = std::fabs(c.volume[i].norm - norm);
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}

static int EnsureFadeAnchor(Clip& c, int side) {
    EnsureEnvelope(c);
    float dur = c.duration;
    if (dur <= 0.05f) return -1;
    float len = side < 0 ? c.volIn : c.volOut;
    if (len <= 0.02f) return -1;
    float n = side < 0 ? len / dur : 1.0f - len / dur;
    if (n < 0.012f) n = 0.012f;
    if (n > 0.988f) n = 0.988f;
    int idx = FindVolPoint(c, n, 0.035f);
    if (idx == 0 || idx == (int)c.volume.size() - 1) idx = -1;
    if (idx >= 0) return idx;
    float g = EnvAtNorm(c, n);
    size_t at = 1;
    while (at < c.volume.size() && c.volume[at].norm < n) at++;
    if (at >= c.volume.size()) at = c.volume.size() - 1;
    c.volume.insert(c.volume.begin() + (std::ptrdiff_t)at, {n, g});
    return (int)at;
}

static void MoveFadeAnchor(Clip& c, int side, float oldLen) {
    EnsureEnvelope(c);
    float dur = c.duration;
    if (dur <= 0.05f) return;
    float len = side < 0 ? c.volIn : c.volOut;
    float oldN = side < 0 ? oldLen / dur : 1.0f - oldLen / dur;
    if (len <= 0.02f) {
        int idx = FindVolPoint(c, oldN, 0.045f);
        if (idx > 0 && idx < (int)c.volume.size() - 1) c.volume.erase(c.volume.begin() + idx);
        return;
    }
    float n = side < 0 ? len / dur : 1.0f - len / dur;
    if (n < 0.012f) n = 0.012f;
    if (n > 0.988f) n = 0.988f;
    int idx = FindVolPoint(c, oldN, 0.045f);
    if (idx <= 0 || idx >= (int)c.volume.size() - 1) idx = FindVolPoint(c, n, 0.045f);
    if (idx <= 0 || idx >= (int)c.volume.size() - 1) {
        EnsureFadeAnchor(c, side);
        return;
    }
    float lo = c.volume[idx - 1].norm + 0.01f;
    float hi = c.volume[idx + 1].norm - 0.01f;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    c.volume[idx].norm = n;
    if (side < 0) c.volIn = n * dur;
    else c.volOut = (1.0f - n) * dur;
}

static int FadeHandleRole(const Clip& c, ImVec2 wp0, ImVec2 wp1, float w, ImVec2 mouse) {
    float dur = c.duration;
    if (dur <= 0.05f) return 0;
    auto hit = [&](float local) {
        if (local <= 0.02f || local >= dur - 0.02f) return false;
        float u = local / dur;
        float g = EnvAtNorm(c, u);
        float px = wp0.x + u * w;
        float py = VolPointY(wp0.y, wp1.y, g);
        float dx = mouse.x - px;
        float dy = mouse.y - py;
        return dx * dx + dy * dy <= 196.0f;
    };
    if (c.volIn > 0.02f && hit(c.volIn)) return 1;
    if (c.volOut > 0.02f && hit(dur - c.volOut)) return 2;
    return 0;
}

static void DrawVolumeLine(ImDrawList* dl, ImVec2 a, ImVec2 b, const Clip& c) {
    if (!dl) return;
    float dur = c.duration;
    if (dur < 0.05f) return;
    float w = b.x - a.x;
    float h = b.y - a.y;
    if (w < 6.0f || h < 6.0f) return;
    const int steps = 48;
    ImVec2 prev(0, 0);
    ImU32 col = IM_COL32(255, 210, 70, 240);
    for (int i = 0; i <= steps; i++) {
        float u = (float)i / (float)steps;
        float local = dur * u;
        float g = EnvAtNorm(c, u) * VolumeEdge(local, dur, c.volIn, c.volOut);
        if (g < 0) g = 0;
        if (g > 1) g = 1;
        ImVec2 p(a.x + w * u, VolPointY(a.y, b.y, g));
        if (i) dl->AddLine(prev, p, col, 1.8f);
        prev = p;
    }
    auto mark = [&](float local) {
        if (local < 0) local = 0;
        if (local > dur) local = dur;
        float u = local / dur;
        float g = EnvAtNorm(c, u);
        dl->AddCircleFilled(ImVec2(a.x + w * u, VolPointY(a.y, b.y, g)), 5.2f, IM_COL32(255, 255, 255, 255));
    };
    if (c.volIn > 0.02f) mark(std::min(dur, c.volIn));
    if (c.volOut > 0.02f) mark(std::max(0.0f, dur - c.volOut));
}

static bool AudioArrowHit(ImVec2 a, ImVec2 b, ImVec2 m) {
    float cx = (a.x + b.x) * 0.5f;
    float cy = (a.y + b.y) * 0.5f;
    float dx = m.x - cx;
    float dy = m.y - cy;
    return dx * dx + dy * dy <= 121.0f;
}

static void DrawAudioArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, bool open) {
    if (!dl) return;
    if (b.x - a.x < 18.0f) return;
    float cx = (a.x + b.x) * 0.5f;
    float cy = (a.y + b.y) * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, cy), 8.0f, IM_COL32(0, 0, 0, 150));
    ImU32 col = IM_COL32(255, 214, 70, 245);
    if (open)
        dl->AddTriangleFilled(ImVec2(cx - 5.5f, cy + 3.0f), ImVec2(cx + 5.5f, cy + 3.0f), ImVec2(cx, cy - 4.5f), col);
    else
        dl->AddTriangleFilled(ImVec2(cx - 5.5f, cy - 3.0f), ImVec2(cx + 5.5f, cy - 3.0f), ImVec2(cx, cy + 4.5f), col);
}

static bool LaneHasAudioOpen(int track) {
    for (int i = 0; i < (int)clips.size(); i++)
        if (clips[i].track == track && clips[i].audioOpen) return true;
    return false;
}

static void DrawClipBody(ImDrawList* dl, ImVec2 cp0, ImVec2 cp1, const Clip& c) {
    if (!dl || c.mediaId < 0 || c.mediaId >= (int)media.size()) return;
    const MediaItem& m = media[c.mediaId];
    if (m.type != "image" && m.path.rfind("solid:", 0) != 0) {
        RequestWave(c.mediaId);
        std::vector<unsigned char> peaks;
        if (CopyWave(m.path, peaks) && !peaks.empty()) {
            float w = cp1.x - cp0.x;
            float h = cp1.y - cp0.y;
            if (w >= 8.0f && h >= 8.0f) {
                float dur = m.duration > 0.05f ? m.duration : 1.0f;
                float speed = c.speed < 0.05f ? 1.0f : c.speed;
                float src0 = c.sourceOffset;
                float src1 = src0 + c.duration * speed;
                int n = (int)peaks.size();
                float midY = (cp0.y + cp1.y) * 0.5f;
                float maxH = h * 0.42f;
                int bars = (int)(w / 3.0f);
                if (bars < 1) bars = 1;
                if (bars > 2000) bars = 2000;
                ImU32 col = IM_COL32(255, 255, 255, 210);
                for (int i = 0; i < bars; i++) {
                    float u0 = src0 + (src1 - src0) * ((float)i / (float)bars);
                    float u1 = src0 + (src1 - src0) * ((float)(i + 1) / (float)bars);
                    int i0 = (int)(u0 / dur * (float)n);
                    int i1 = (int)(u1 / dur * (float)n);
                    if (i0 < 0) i0 = 0;
                    if (i1 < i0) i1 = i0;
                    if (i0 >= n) i0 = n - 1;
                    if (i1 >= n) i1 = n - 1;
                    unsigned char pk = 0;
                    for (int k = i0; k <= i1; k++) if (peaks[(size_t)k] > pk) pk = peaks[(size_t)k];
                    float bh = maxH * ((float)pk / 255.0f);
                    if (bh < 0.6f) continue;
                    float x = cp0.x + (float)i * 3.0f + 1.0f;
                    dl->AddLine(ImVec2(x, midY - bh), ImVec2(x, midY + bh), col, 1.35f);
                }
            }
        }
    }
}

#ifdef _MSC_VER
#pragma optimize("", off)
#endif
int main(int argc, char* argv[]) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) return 1;

#ifdef HAVE_FFMPEG
    {
        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (audioDev) SDL_PauseAudioDevice(audioDev, 1);
    }
#endif

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("DaniWorldsStudio FFmpeg 56",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { SDL_Quit(); return 1; }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);
    pglUseProgram = (PFN_glUseProgramProc)SDL_GL_GetProcAddress("glUseProgram");
    HWND hwnd = GetSDLWindowHandle(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsDark();
    {
        static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0590, 0x05FF, 0 };
        ImFontConfig uiCfg;
        uiCfg.OversampleH = 2;
        uiCfg.OversampleV = 2;
        uiCfg.PixelSnapH = false;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 22.0f, &uiCfg, ranges))
            io.Fonts->AddFontDefault();
        ImFontConfig subCfg;
        subCfg.OversampleH = 3;
        subCfg.OversampleV = 3;
        subCfg.PixelSnapH = false;
        subtitleFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 160.0f, &subCfg, ranges);
        if (!subtitleFont)
            subtitleFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 160.0f, &subCfg, ranges);
    }
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    RefreshAllLooks();

    bool running = true;
    SDL_Event event;
    int topClip = -1;
    int driveClip = -1;
    int previewPlate = -1;

    while (running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (event.key.keysym.sym == SDLK_SPACE && !event.key.repeat && !io.WantTextInput && !subEditorOpen) {
#ifdef HAVE_FFMPEG
                    if (!gExport.active) SetPlaying(!isPlaying);
#else
                    isPlaying = !isPlaying;
#endif
                }
                if (event.key.keysym.sym == SDLK_c && !event.key.repeat && !io.WantTextInput) {
                    if (selectedBus >= 0) SplitBusAt(currentTime);
                    else SplitSelected();
                }
                if (event.key.keysym.sym == SDLK_s && !event.key.repeat && !io.WantTextInput && busHover) {
                    float st = SnapTime(-1, busHoverTime);
                    AddBusSegment(st, 5.0f);
                }
                if ((event.key.keysym.sym == SDLK_i) && !event.key.repeat && !io.WantTextInput && !subEditorOpen && !(SDL_GetModState() & KMOD_CTRL))
                    ImportFiles(hwnd);
                if (event.key.keysym.sym == SDLK_z && (event.key.keysym.mod & KMOD_CTRL) && !(event.key.keysym.mod & KMOD_SHIFT) && !io.WantTextInput)
                    undoRequest++;
                if ((event.key.keysym.sym == SDLK_DELETE || event.key.keysym.sym == SDLK_BACKSPACE) && !io.WantTextInput) {
                    bool clipsOn = selectedClip >= 0 || !selection.empty();
                    bool subsOn = selectedSub >= 0 || !subSelection.empty();
                    if (selectedBus >= 0 && selectedBus < (int)bus.size()) {
                        bus.erase(bus.begin() + selectedBus);
                        if (contextBus == selectedBus) contextBus = -1;
                        else if (contextBus > selectedBus) contextBus--;
                        selectedBus = -1;
                        draggingBus = -1;
                        trimmingBus = -1;
                    } else if (!mediaSel.empty()) {
                        DeleteSelectedMedia();
                    } else if (!clipsOn && !subsOn && gOverlaySel >= 0 && gOverlaySel < (int)gOverlays.size()) {
                        DeleteOverlayIndex(gOverlaySel);
                    } else if (clipsOn || subsOn) {
                        if (clipsOn) {
                        std::vector<int> kill = selection;
                        if (selectedClip >= 0) kill.push_back(selectedClip);
                        std::sort(kill.begin(), kill.end());
                        kill.erase(std::unique(kill.begin(), kill.end()), kill.end());
                        for (int n = (int)kill.size() - 1; n >= 0; --n) {
                            int id = kill[n];
                            if (id < 0 || id >= (int)clips.size()) continue;
                            if (fxEdit == id) fxEdit = -1;
                            else if (fxEdit > id) fxEdit--;
                            if (contextClip == id) contextClip = -1;
                            else if (contextClip > id) contextClip--;
                            clips.erase(clips.begin() + id);
                        }
                        selectedClip = -1;
                        selection.clear();
#ifdef HAVE_FFMPEG
                        playingClip = -1;
                        playingClipB = -1;
                        nextClipIdx = -1;
#endif
                        draggingClip = -1;
                        trimmingClip = -1;
                        dragVolClip = -1;
                        }
                        if (subsOn) {
                        std::vector<int> kill = subSelection;
                        if (selectedSub >= 0) kill.push_back(selectedSub);
                        std::sort(kill.begin(), kill.end());
                        kill.erase(std::unique(kill.begin(), kill.end()), kill.end());
                        for (int n = (int)kill.size() - 1; n >= 0; --n) {
                            int id = kill[n];
                            if (id < 0 || id >= (int)subs.size()) continue;
                            subs.erase(subs.begin() + id);
                            if (editingSub == id) { editingSub = -1; subEditorOpen = false; }
                            else if (editingSub > id) editingSub--;
                            if (draggingSub == id) draggingSub = -1;
                            else if (draggingSub > id) draggingSub--;
                            if (trimmingSub == id) trimmingSub = -1;
                            else if (trimmingSub > id) trimmingSub--;
                        }
                        selectedSub = -1;
                        subSelection.clear();
                        if (editingSub < 0 || editingSub >= (int)subs.size()) {
                            editingSub = -1;
                            subEditorOpen = false;
                        }
                        }
                        ClearGroupDrag();
                    }
                }
            }
        }

        thumbBlits.clear();
        fxOther = -1;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        if (!ImGui::IsMouseDown(0)) panelPointerLock = false;
        uiShield.on = false;
        openedPanelThisFrame = false;
        if (fontAtlas.empty() && ImGui::GetIO().Fonts->TexPixelsAlpha8 && ImGui::GetIO().Fonts->TexWidth > 1) {
            fontAtlasW = ImGui::GetIO().Fonts->TexWidth;
            fontAtlasH = ImGui::GetIO().Fonts->TexHeight;
            const unsigned char* px = ImGui::GetIO().Fonts->TexPixelsAlpha8;
            fontAtlas.assign(px, px + (size_t)fontAtlasW * fontAtlasH);
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("New")) {
                ClearProject();
                previewStatus = "New project";
            }
            if (ImGui::MenuItem("Save")) {
                std::string p = SaveDialog(hwnd, "DaniWorlds Project (*.dwp)\0*.dwp\0", "dwp", "Save Project");
                if (!p.empty()) previewStatus = SaveProject(p) ? "Project saved" : "Save failed";
            }
            if (ImGui::MenuItem("Load")) {
                std::string p = OpenDialog(hwnd, "DaniWorlds Project (*.dwp)\0*.dwp\0", "Load Project");
                if (!p.empty()) previewStatus = LoadProject(p) ? "Project loaded" : "Load failed";
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export")) requestExport = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty() || undoGesture)) undoRequest++;
            if (ImGui::MenuItem("Import")) ImportFiles(hwnd);
            if (ImGui::MenuItem(asrOn ? "Stop subs" : "Auto subs")) {
                if (asrOn) {
                    asrOn = false;
                    AsrClose();
                    previewStatus = "Subtitles cancelled";
                } else {
                    asrEnd = ProjectEnd();
                    if (asrEnd < 0.3) previewStatus = "Nothing to transcribe";
                    else {
                        SetPlaying(false);
                        asrPcm.clear();
                        asrSegs.clear();
                        asrCursor = 0;
                        asrChunk = 0;
                        asrPhase = 0;
                        asrOn = true;
                        AsrLogReset();
                        previewStatus = "Preparing subtitles. Log: asr.log";
                    }
                }
            }
            ImGui::SetNextItemWidth(120);
            ImGui::Combo("Language", &asrLang, "Hebrew\0English\0");
            if (ImGui::MenuItem("Export SRT")) {
                if (subs.empty()) previewStatus = "No subtitles";
                else {
                    std::string p = SaveDialog(hwnd, "Subtitles (*.srt)\0*.srt\0", "srt", "Export Subtitles");
                    previewStatus = (!p.empty() && WriteSrt(p)) ? "SRT saved" : (p.empty() ? "SRT cancelled" : "SRT failed");
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(snapEnabled ? "Snap ON" : "Snap OFF")) snapEnabled = !snapEnabled;
            if (ImGui::MenuItem("RIFE", nullptr, &exRife)) {}
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
        }

        float availY = ImGui::GetContentRegionAvail().y;
        float tlH = 26.0f + 58.0f + 42.0f + 120.0f + 42.0f + 120.0f * 6.0f + 72.0f;
        if (tlH > availY * 0.50f) tlH = availY * 0.50f;
        float topH = availY - tlH - 4.0f;
        if (topH < 220.0f) topH = 220.0f;

        ImGui::BeginChild("Media", ImVec2(io.DisplaySize.x * 0.26f, topH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("LibraryTabs")) {
        if (ImGui::BeginTabItem("Media")) {
        ImGui::BeginChild("libmedia", ImVec2(0, 0), false);
        ImVec2 mediaClip0 = ImGui::GetWindowPos();
        ImVec2 mediaClip1(mediaClip0.x + ImGui::GetWindowSize().x, mediaClip0.y + ImGui::GetWindowSize().y);
        float gap = 6.0f;
        float cellW = std::max(70.0f, (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f);
        float thumbH = cellW * 9.0f / 16.0f;
        int shown = 0;
        for (int i = 0; i < (int)media.size(); i++) {
            const auto& m = media[i];
            if (IsSolidPath(m.path)) continue;
            if (shown % 3 != 0) ImGui::SameLine(0, gap);
            shown++;
            ImGui::PushID(i);
            ImGui::BeginGroup();
            ImGui::InvisibleButton("##dragitem", ImVec2(cellW, thumbH));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            bool vis = p1.y > mediaClip0.y && p0.y < mediaClip1.y && p1.x > mediaClip0.x && p0.x < mediaClip1.x;
            if (m.thumb && vis)
                DrawThumb(dl, p0, p1, mediaClip0, mediaClip1, m.thumb, m.thumbW, m.thumbH);
            else if (vis)
                DrawPlaceholder(dl, p0, p1, m);
            if (std::find(mediaSel.begin(), mediaSel.end(), i) != mediaSel.end())
                dl->AddRect(p0, p1, IM_COL32(255, 210, 60, 255), 2.0f, 0, 2.0f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cellW);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::Text("%.1fs %s", m.duration, m.type.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImVec2 g0 = ImGui::GetItemRectMin();
            ImVec2 g1 = ImGui::GetItemRectMax();
            if (ImGui::IsMouseHoveringRect(g0, g1) && ImGui::IsMouseClicked(0) && !ImGui::IsMouseDragging(0)) {
                selectedClip = -1;
                selection.clear();
                contextClip = -1;
                selectedBus = -1;
                selectedSub = -1;
                subSelection.clear();
                gOverlaySel = -1;
                if (ImGui::GetIO().KeyShift) {
                    auto it = std::find(mediaSel.begin(), mediaSel.end(), i);
                    if (it == mediaSel.end()) mediaSel.push_back(i);
                    else mediaSel.erase(it);
                } else {
                    mediaSel.clear();
                    mediaSel.push_back(i);
                }
            }
            if (ImGui::IsMouseHoveringRect(g0, g1) && ImGui::IsMouseClicked(1)) {
                selectedClip = -1;
                selection.clear();
                contextClip = -1;
                selectedBus = -1;
                selectedSub = -1;
                subSelection.clear();
                gOverlaySel = -1;
                mediaSel.clear();
                mediaSel.push_back(i);
                floatMedia = i;
                floatMediaOpen = true;
                libPlayId = -1;
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload("MEDIA", &i, sizeof(int));
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects")) {
            ImGui::TextDisabled("Drag onto the FX lane");
            EnsureFxPlate();
            OverlayPreviewTick((float)ImGui::GetTime());
            ImGui::BeginChild("libfx", ImVec2(0, 0), false);
            ImVec2 mediaClip0 = ImGui::GetWindowPos();
            ImVec2 mediaClip1(mediaClip0.x + ImGui::GetWindowSize().x, mediaClip0.y + ImGui::GetWindowSize().y);
            float gap = 6.0f;
            float cellW = std::max(70.0f, (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f);
            float thumbH = cellW * 9.0f / 16.0f;
            static const int fxOrder[] = {
                28, 26, 27,
                19, 20, 21, 22, 23, 24, 25, 3, 4,
                0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18
            };
            int nOrder = (int)(sizeof(fxOrder) / sizeof(fxOrder[0]));
            int nKind = OverlayKindCount();
            for (int slot = 0; slot < nKind; slot++) {
                int i = slot < nOrder ? fxOrder[slot] : slot;
                if (slot % 3 != 0) ImGui::SameLine(0, gap);
                ImGui::PushID(1000 + i);
                ImGui::BeginGroup();
                ImGui::InvisibleButton("##fxcard", ImVec2(cellW, thumbH));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetItemRectMin();
                ImVec2 p1 = ImGui::GetItemRectMax();
                bool vis = p1.y > mediaClip0.y && p0.y < mediaClip1.y && p1.x > mediaClip0.x && p0.x < mediaClip1.x;
                unsigned int pv = OverlayPreviewTex(i);
                if (vis && pv) {
                    dl->AddRectFilled(p0, p1, IM_COL32(8, 8, 10, 255), 2.0f);
                    DrawThumb(dl, p0, p1, mediaClip0, mediaClip1, (GLuint)pv, OverlayPreviewW(), OverlayPreviewH());
                    dl->AddRect(p0, p1, IM_COL32(80, 80, 90, 255), 2.0f);
                } else if (vis) DrawFxIcon(dl, p0, p1, i);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cellW);
                ImGui::TextUnformatted(OverlayKindName(i));
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    int kind = i;
                    ImGui::SetDragDropPayload("FXKIND", &kind, sizeof(int));
                    ImGui::TextUnformatted(OverlayKindName(kind));
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Solids")) {
            EnsureSolidThumbs();
            ImGui::TextDisabled("Drag onto a video track");
            ImGui::BeginChild("libsolids", ImVec2(0, 0), false);
            float gap = 6.0f;
            float cellW = std::max(70.0f, (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f);
            float thumbH = cellW * 9.0f / 16.0f;
            for (int k = 0; k < 3; k++) {
                if (k % 3 != 0) ImGui::SameLine(0, gap);
                ImGui::PushID(5000 + k);
                ImGui::BeginGroup();
                ImGui::InvisibleButton("##solid", ImVec2(cellW, thumbH));
                ImDrawList* sdl = ImGui::GetWindowDrawList();
                ImVec2 sp0 = ImGui::GetItemRectMin();
                ImVec2 sp1 = ImGui::GetItemRectMax();
                if (gSolidTex[k]) DrawThumb(sdl, sp0, sp1, sp0, sp1, gSolidTex[k], 320, 180);
                ImGui::TextUnformatted(kSolidName[k]);
                ImGui::TextUnformatted("Solid");
                ImGui::EndGroup();
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                    int kind = k;
                    ImGui::SetDragDropPayload("SOLID", &kind, sizeof(int));
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transitions")) {
            ImGui::TextDisabled("Select two overlapping clips, then drag");
            EnsureTransPlates();
            ImGui::BeginChild("libtrans", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImVec2 transClip0 = ImGui::GetWindowPos();
            ImVec2 transClip1(transClip0.x + ImGui::GetWindowSize().x, transClip0.y + ImGui::GetWindowSize().y);
            float gap = 6.0f;
            float cellW = std::max(70.0f, (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f);
            float thumbH = cellW * 9.0f / 16.0f;
            for (int i = 0; i < FxCount(); i++) {
                ClipEffect* fx = FxAt(i);
                if (!fx) continue;
                if (i % 3 != 0) ImGui::SameLine(0, gap);
                ImGui::PushID(7000 + i);
                ImGui::BeginGroup();
                ImGui::InvisibleButton("##tr", ImVec2(cellW, thumbH));
                ImDrawList* xdl = ImGui::GetWindowDrawList();
                ImVec2 tp0 = ImGui::GetItemRectMin();
                ImVec2 tp1 = ImGui::GetItemRectMax();
                bool vis = tp1.y > transClip0.y && tp0.y < transClip1.y && tp1.x > transClip0.x && tp0.x < transClip1.x;
                if (vis) DrawTransitionCard(xdl, tp0, tp1, fx->id());
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cellW);
                ImGui::TextUnformatted(fx->label());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("TRANS", &i, sizeof(int));
                    ImGui::TextUnformatted(fx->label());
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::BeginChild("Preview", ImVec2(0, topH - 46), true);
        previewPanelMin = ImGui::GetWindowPos();
        previewPanelMax = ImVec2(previewPanelMin.x + ImGui::GetWindowSize().x, previewPanelMin.y + ImGui::GetWindowSize().y);
        ImGui::Text("Preview | %s", previewStatus.c_str());
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("prev", ImVec2(avail.x, std::max(10.0f, avail.y - 8)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
        previewMin = p0;
        previewMax = p1;
        FitAspectBox(p0, p1, AspectValue(exAspect), previewFrameMin, previewFrameMax);
        previewFrameValid = true;
        dl->AddRect(previewFrameMin, previewFrameMax, IM_COL32(230, 230, 230, 255), 0.0f, 0, 2.0f);
        if (ImGui::IsMouseDown(0)) CaptureUiShield();
        if (TimelinePointerBusy()) {
            ovDragMode = 0;
            ovDragIndex = -1;
            xformDrag = 0;
        } else if (ovDragMode == 0 || !ImGui::IsMouseDown(0)) {
            if (!ImGui::IsMouseDown(0)) { ovDragMode = 0; ovDragIndex = -1; xformDrag = 0; }
        }
        if (!xformOpen) xformDrag = 0;
        if (!TimelinePointerBusy() && ImGui::IsMouseClicked(0) && ovDragMode == 0 && xformDrag == 0) {
            ImVec2 m = ImGui::GetMousePos();
            if (m.x >= previewFrameMin.x && m.y >= previewFrameMin.y && m.x <= previewFrameMax.x && m.y <= previewFrameMax.y) {
                for (int i = (int)gOverlays.size() - 1; i >= 0; --i) {
                    const OverlayFx& o = gOverlays[i];
                    if (currentTime < o.start || currentTime >= o.start + o.duration) continue;
                    float ax, ay, bx, by;
                    OverlayBox(o, previewFrameMin.x, previewFrameMin.y, previewFrameMax.x, previewFrameMax.y, ax, ay, bx, by, currentTime);
                    float hx = (ax + bx) * 0.5f;
                    float hy = ay + 16.0f;
                    if (std::fabs(m.x - hx) < 10.0f && std::fabs(m.y - hy) < 10.0f) { FocusOverlay(i); ovDragMode = 3; ovDragIndex = i; break; }
                    if (std::fabs(m.x - bx) < 12.0f && std::fabs(m.y - by) < 12.0f) { FocusOverlay(i); ovDragMode = 2; ovDragIndex = i; break; }
                    if (m.x >= ax && m.x <= bx && m.y >= ay && m.y <= by) { FocusOverlay(i); ovDragMode = 1; ovDragIndex = i; break; }
                }
#ifdef HAVE_FFMPEG
                if (ovDragMode == 0 && xformOpen && XformOnScreen(xformClip, topClip, previewPlate)) {
                    int hit = HitXform(xformClip, topClip, m);
                    if (hit) xformDrag = hit;
                }
#endif
            }
        } else if (!TimelinePointerBusy() && ovDragMode && ImGui::IsMouseDown(0) && ovDragIndex >= 0 && ovDragIndex < (int)gOverlays.size()) {
            OverlayFx& o = gOverlays[ovDragIndex];
            float fw = std::max(1.0f, previewFrameMax.x - previewFrameMin.x);
            float fh = std::max(1.0f, previewFrameMax.y - previewFrameMin.y);
            ImGuiIO& pio = ImGui::GetIO();
            bool capture = o.rec && isPlaying;
            float local = currentTime - o.start;
            float x = o.x, y = o.y, sc = o.scale, rot = o.rot;
            if (capture) {
                x = OverlaySample(o.anim[0], local, o.holdOn[0] ? o.hold[0] : o.x);
                y = OverlaySample(o.anim[1], local, o.holdOn[1] ? o.hold[1] : o.y);
                sc = OverlaySample(o.anim[2], local, o.holdOn[2] ? o.hold[2] : o.scale);
                rot = OverlaySample(o.anim[3], local, o.holdOn[3] ? o.hold[3] : o.rot);
            }
            float bx = x, by = y, bs = sc, br = rot;
            if (ovDragMode == 1) {
                x += pio.MouseDelta.x / (fw * 0.5f);
                y += pio.MouseDelta.y / (fh * 0.5f);
            } else if (ovDragMode == 2) {
                sc += (pio.MouseDelta.x + pio.MouseDelta.y) / std::max(40.0f, fw * 0.25f);
            } else if (ovDragMode == 3) {
                float cx = (previewFrameMin.x + previewFrameMax.x) * 0.5f + x * fw * 0.5f;
                float cy = (previewFrameMin.y + previewFrameMax.y) * 0.5f + y * fh * 0.5f;
                float a0 = std::atan2((pio.MousePos.y - pio.MouseDelta.y) - cy, (pio.MousePos.x - pio.MouseDelta.x) - cx);
                float a1 = std::atan2(pio.MousePos.y - cy, pio.MousePos.x - cx);
                rot += (a1 - a0) * 57.2957795f;
            }
            if (x < -1.2f) x = -1.2f;
            if (x > 1.2f) x = 1.2f;
            if (y < -1.2f) y = -1.2f;
            if (y > 1.2f) y = 1.2f;
            if (sc < 0.15f) sc = 0.15f;
            if (sc > 4.0f) sc = 4.0f;
            o.x = x;
            o.y = y;
            o.scale = sc;
            o.rot = rot;
            bool moved = std::fabs(pio.MouseDelta.x) + std::fabs(pio.MouseDelta.y) > 0.01f;
            if (capture && moved) {
                if (ovDragMode == 1) {
                    OverlayWrite(o, 0, bx, o.x, currentTime);
                    OverlayWrite(o, 1, by, o.y, currentTime);
                } else if (ovDragMode == 2) OverlayWrite(o, 2, bs, o.scale, currentTime);
                else if (ovDragMode == 3) OverlayWrite(o, 3, br, o.rot, currentTime);
            }
        }
#ifdef HAVE_FFMPEG
        else if (!TimelinePointerBusy() && xformOpen && xformDrag && ImGui::IsMouseDown(0)) {
            DragXform(xformClip, topClip);
        }
#endif

        RoutePreviewClip(currentTime, topClip, previewPlate, driveClip);
        if (driveClip >= 0) EnsurePlayerForClip(driveClip);
        else {
            dl->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(140, 140, 140, 255), "No video on timeline");
            previewStatus = "No video";
        }
        ImGui::EndChild();

        {
            float buttonsW = 40.0f * 5.0f + ImGui::GetStyle().ItemSpacing.x * 4.0f;
            float comboW = 92.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            float origin = ImGui::GetCursorPosX();
            float left = std::max(0.0f, (avail - comboW - 12.0f - buttonsW) * 0.5f);
            ImGui::SetCursorPosX(origin + left);
        }
        if (TransportButton("tostart", 3)) {
#ifdef HAVE_FFMPEG
            SetPlaying(false);
            ResetAudioSync();
            if (player) player->lastPts = -1;
#else
            isPlaying = false;
#endif
            currentTime = 0;
            shownLocal = -1000.0f;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("To start");
        ImGui::SameLine();
        if (TransportButton("play", 0)) {
#ifdef HAVE_FFMPEG
            SetPlaying(true);
#else
            isPlaying = true;
#endif
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play");
        ImGui::SameLine();
        if (TransportButton("pause", 1)) {
#ifdef HAVE_FFMPEG
            SetPlaying(false);
#else
            isPlaying = false;
#endif
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause");
        ImGui::SameLine();
        if (TransportButton("stop", 2)) {
#ifdef HAVE_FFMPEG
            SetPlaying(false);
#else
            isPlaying = false;
#endif
            currentTime = 0;
            shownLocal = -1000.0f;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop");
        ImGui::SameLine();
        bool repeatLit = repeatOn;
        if (repeatLit) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.32f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.40f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.22f, 0.06f, 1.0f));
        }
        if (TransportButton("repeat", 4)) repeatOn = !repeatOn;
        if (repeatLit) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(repeatOn ? "Repeat on" : "Repeat");
        ImGui::SameLine();
        {
            int total = (int)currentTime;
            int ms = (int)((currentTime - (float)total) * 100.0f);
            if (ms > 99) ms = 99;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%02d:%02d.%02d", total / 60, total % 60, ms);
        }
        ImGui::SameLine();
        {
            float zoomW = 130.0f;
            float comboW = 92.0f;
            float need = zoomW + comboW + 8.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            if (avail > need) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - need);
            float contentEnd = 8.0f;
            for (const auto& c : clips) contentEnd = std::max(contentEnd, c.startTime + c.duration);
            for (const auto& b : bus) contentEnd = std::max(contentEnd, b.start + b.duration);
            for (const auto& s : subs) contentEnd = std::max(contentEnd, s.start + s.duration);
            for (const auto& o : gOverlays) contentEnd = std::max(contentEnd, o.start + o.duration);
            float span = contentEnd * 1.20f;
            float viewW = tlViewW - 62.0f;
            if (viewW < 160.0f) viewW = 160.0f;
            float fit = viewW / std::max(1.0f, span);
            if (fit < 1.0f) fit = 1.0f;
            if (fit > 80.0f) fit = 80.0f;
            float zmin = std::min(fit, pixelsPerSecond);
            float zmax = std::max(240.0f, pixelsPerSecond);
            ImGui::SetNextItemWidth(zoomW);
            ImGui::SliderFloat("##tlzoom", &pixelsPerSecond, zmin, zmax, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(comboW);
            const char* aspectLabel = "16:9";
            if (exAspect == 1) aspectLabel = "9:16";
            else if (exAspect == 2) aspectLabel = "1:1";
            else if (exAspect == 3) aspectLabel = "4:3";
            else if (exAspect == 4) aspectLabel = "3:4";
            if (ImGui::BeginCombo("##previewaspect", aspectLabel)) {
                const char* names[] = {"16:9", "9:16", "1:1", "4:3", "3:4"};
                for (int i = 0; i < 5; i++) {
                    if (ImGui::Selectable(names[i], exAspect == i) && exAspect != i) {
                        exAspect = i;
                        RefitAutoGsImages();
                        SplitLongSubs();
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview and export aspect");
        }
        ImGui::EndGroup();

        float tlSeconds = 600.0f;
        for (const auto& c : clips)
            tlSeconds = std::max(tlSeconds, c.startTime + c.duration + 90.0f);
        for (const auto& b : bus)
            tlSeconds = std::max(tlSeconds, b.start + b.duration + 30.0f);
        for (const auto& s : subs)
            tlSeconds = std::max(tlSeconds, s.start + s.duration + 30.0f);
        for (const auto& o : gOverlays)
            tlSeconds = std::max(tlSeconds, o.start + o.duration + 30.0f);
        {
            float contentEnd = 0.0f;
            for (const auto& c : clips) contentEnd = std::max(contentEnd, c.startTime + c.duration);
            for (const auto& b : bus) contentEnd = std::max(contentEnd, b.start + b.duration);
            for (const auto& s : subs) contentEnd = std::max(contentEnd, s.start + s.duration);
            for (const auto& o : gOverlays) contentEnd = std::max(contentEnd, o.start + o.duration);
            if (contentEnd > 0.5f) tlSeconds = std::max(tlSeconds, contentEnd * 1.20f);
        }
        float timeW = tlSeconds * pixelsPerSecond;
        ImGui::BeginChild("Timeline", ImVec2(0, tlH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float meterW = 78.0f;
        tlViewW = ImGui::GetWindowWidth() - meterW;
        const char* trackNames[] = {"V1", "V2", "V3", "A1", "A2", "A3"};
        const float kBodyH = kClipBody;
        const float kWaveH = kClipWave;
        const float kWaveGap = kClipGap;
        float laneH = 42.0f;
        float closedH = kBodyH + 26.0f;
        float openH = kBodyH + kWaveGap + kWaveH + 26.0f;
        tlTrackH = kBodyH + 8.0f;
        ImVec2 trackPositions[6];
        const float rulerH = 26.0f;
        ImVec2 rulerCursor = ImGui::GetCursorPos();
        ImGui::Dummy(ImVec2(1.0f, rulerH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("tlbody", ImVec2(-meterW, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PopStyleVar();
        float bodyScrollX = ImGui::GetScrollX();

        busHover = false;
        ImGui::BeginChild("buslane", ImVec2(timeW + 90.0f, 58.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImVec2 lane0 = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 0));
        ImGui::BeginGroup();
        ImGui::TextUnformatted("BUS");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click the empty lane for the whole project");
        ImGui::EndGroup();
        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(lane0.x + 46.0f, lane0.y));
        ImVec2 busStart = ImGui::GetCursorScreenPos();
        busOriginX = busStart.x;
        float busW = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("buszone", ImVec2(std::max(8.0f, busW), 34.0f));
        bool busEmptyClick = ImGui::IsItemClicked(0) && !TimelinePointerBusy();
        if (ImGui::IsItemHovered()) {
            busHover = true;
            busHoverTime = (ImGui::GetMousePos().x - busStart.x) / pixelsPerSecond;
            if (busHoverTime < 0) busHoverTime = 0;
        }
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)bus.size(); i++) {
            float x = busStart.x + bus[i].start * pixelsPerSecond;
            float w = std::max(18.0f, bus[i].duration * pixelsPerSecond);
            ImVec2 cp0(x, busStart.y);
            ImVec2 cp1(x + w, busStart.y + 32.0f);
            bdl->AddRectFilled(cp0, cp1, IM_COL32(168, 112, 42, 235), 3.0f);
            if (i == selectedBus) bdl->AddRect(cp0, cp1, IM_COL32(255, 220, 120, 255), 3.0f, 0, 2.0f);
            char lab[64];
            snprintf(lab, sizeof(lab), "BUS %.1fs%s", bus[i].duration, BusActive(bus[i]) ? " FX" : "");
            bdl->AddText(ImVec2(cp0.x + 6, cp0.y + 4), IM_COL32(255, 245, 220, 240), lab);
            if (!ImGui::IsMouseHoveringRect(cp0, cp1) || TimelinePointerBusy()) continue;
            busEmptyClick = false;
            busHover = true;
            busHoverTime = (ImGui::GetMousePos().x - busStart.x) / pixelsPerSecond;
            ImVec2 mouse = ImGui::GetMousePos();
            if (mouse.x < cp0.x + 8 || mouse.x > cp1.x - 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseClicked(1)) {
                contextBus = i;
                selectedBus = i;
                selectedClip = -1;
                selection.clear();
                contextClip = -1;
                selectedSub = -1;
                subSelection.clear();
                mediaSel.clear();
                gOverlaySel = -1;
                openBusMenu = true;
                busGradeOpen = true;
                openedPanelThisFrame = true;
            }
            if (ImGui::IsMouseClicked(0)) {
                selectedBus = i;
                selectedClip = -1;
                selection.clear();
                contextClip = -1;
                selectedSub = -1;
                subSelection.clear();
                mediaSel.clear();
                gOverlaySel = -1;
                if (mouse.x < cp0.x + 8) { trimmingBus = i; trimBusSide = 0; }
                else if (mouse.x > cp1.x - 8) { trimmingBus = i; trimBusSide = 1; }
                else { draggingBus = i; busDragOffset = mouse.x - cp0.x; trimmingBus = -1; }
            }
        }
        if (busEmptyClick && !UnselectIfMany()) {
            float end = 0.0f;
            for (const auto& c : clips) end = std::max(end, c.startTime + c.duration);
            if (end < 0.5f) end = 5.0f;
            AddBusSegment(0.0f, end);
        }
        ImGui::EndChild();

        ImGui::BeginChild("fxlane", ImVec2(timeW + 90.0f, laneH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted("FX");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Effects");
        ImGui::SameLine(46.0f);
        ImVec2 fxStart = ImGui::GetCursorScreenPos();
        ovOriginX = fxStart.x;
        ImGui::InvisibleButton("fxzone", ImVec2(std::max(8.0f, ImGui::GetContentRegionAvail().x), laneH - 4));
        bool fxZoneClick = ImGui::IsItemClicked(0) && !TimelinePointerBusy();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FXKIND")) {
                int kind = *(const int*)payload->Data;
                float localX = ImGui::GetMousePos().x - fxStart.x;
                float st = localX / pixelsPerSecond;
                if (st < 0) st = 0;
                OverlayAdd(kind, SnapTime(-1, st));
                FocusOverlay(gOverlaySel);
                fxZoneClick = false;
            }
            ImGui::EndDragDropTarget();
        }
        ImDrawList* fxl = ImGui::GetWindowDrawList();
        fxHover = -1;
        bool ateFx = false;
        for (int i = 0; i < (int)gOverlays.size(); i++) {
            const OverlayFx& o = gOverlays[i];
            float x = fxStart.x + o.start * pixelsPerSecond;
            float w = std::max(24.0f, o.duration * pixelsPerSecond);
            ImVec2 cp0(x, fxStart.y);
            ImVec2 cp1(x + w, fxStart.y + laneH - 8);
            fxl->AddRectFilled(cp0, cp1, FxLaneColor(o.kind), 3.0f);
            if (i == gOverlaySel) fxl->AddRect(cp0, cp1, IM_COL32(255, 230, 160, 255), 3.0f, 0, 2.0f);
            fxl->AddText(ImVec2(cp0.x + 6, cp0.y + 4), IM_COL32(255, 245, 230, 240), OverlayKindName(o.kind));
            int recorded = 0;
            for (int k = 0; k < 7; k++) recorded += (int)o.anim[k].size();
            if (recorded > 0) {
                fxl->AddCircleFilled(ImVec2(cp0.x + 8.0f, cp1.y - 7.0f), 3.2f, IM_COL32(220, 48, 48, 255));
                ImGui::SetCursorScreenPos(ImVec2(cp1.x + 4.0f, cp0.y + 2.0f));
                ImGui::PushID(3000 + i);
                if (ImGui::SmallButton("Rst")) OverlayResetAnim(gOverlays[i]);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset recorded changes");
                ImGui::PopID();
            }
            if (ImGui::IsMouseHoveringRect(cp0, cp1)) fxHover = i;
            if (!ImGui::IsMouseHoveringRect(cp0, cp1) || TimelinePointerBusy()) continue;
            ImVec2 mouse = ImGui::GetMousePos();
            if (mouse.x < cp0.x + 8 || mouse.x > cp1.x - 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseClicked(1)) {
                ateFx = true;
                FocusOverlay(i);
                ovPanel = i;
                ovPanelOpen = true;
                openedPanelThisFrame = true;
            }
            if (ImGui::IsMouseClicked(0)) {
                ateFx = true;
                FocusOverlay(i);
                if (mouse.x < cp0.x + 8) { trimmingOv = i; trimOvSide = 0; draggingOv = -1; }
                else if (mouse.x > cp1.x - 8) { trimmingOv = i; trimOvSide = 1; draggingOv = -1; }
                else { draggingOv = i; ovDragOffset = mouse.x - cp0.x; trimmingOv = -1; }
            }
        }
        if (fxZoneClick && !ateFx) UnselectIfMany();
        ImGui::EndChild();

        float gsH = LaneHasAudioOpen(kGsTrack) ? openH : closedH;
        gsChildH = gsH;
        ImGui::BeginChild("gslane", ImVec2(timeW + 90.0f, gsH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted("GS");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Green screen");
        ImGui::SameLine(46.0f);
        ImVec2 gsStart = ImGui::GetCursorScreenPos();
        gsOriginX = gsStart.x;
        gsLaneY = gsStart.y - 4.0f;
        ImGui::InvisibleButton("gszone", ImVec2(ImGui::GetContentRegionAvail().x, gsH - 4));
        bool gsZoneClick = ImGui::IsItemClicked(0) && !TimelinePointerBusy();
        bool ateGs = false;
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA");
            const ImGuiPayload* solid = payload ? nullptr : ImGui::AcceptDragDropPayload("SOLID");
            int mid = -1;
            if (payload) mid = *(const int*)payload->Data;
            else if (solid) mid = EnsureSolidMedia(*(const int*)solid->Data);
            if (mid >= 0 && mid < (int)media.size() && (media[mid].type == "video" || media[mid].type == "image")) {
                Clip c;
                c.mediaId = mid;
                c.duration = media[mid].duration;
                c.track = kGsTrack;
                if (media[mid].type == "image") ContainGsImage(c);
                float localX = ImGui::GetMousePos().x - gsStart.x;
                AddClipOnTrack(c, localX / pixelsPerSecond);
                SelectOnly((int)clips.size() - 1);
            }
            ImGui::EndDragDropTarget();
        }
        {
            ImDrawList* gdl = ImGui::GetWindowDrawList();
            for (int i = 0; i < (int)clips.size(); i++) {
                if (clips[i].track != kGsTrack) continue;
                float x = gsStart.x + clips[i].startTime * pixelsPerSecond;
                float w = std::max(24.0f, clips[i].duration * pixelsPerSecond);
                bool open = clips[i].audioOpen;
                ImVec2 cp0(x, gsStart.y);
                ImVec2 cp1(x + w, gsStart.y + kBodyH);
                ImVec2 wp0(x, cp1.y + kWaveGap);
                ImVec2 wp1(x + w, wp0.y + kWaveH);
                gdl->AddRectFilled(cp0, cp1, IM_COL32(40, 140, 70, 230), 3.0f);
                if (open) {
                    gdl->AddRectFilled(wp0, wp1, IM_COL32(16, 28, 22, 230), 2.0f);
                    DrawClipBody(gdl, wp0, wp1, clips[i]);
                    DrawVolumeLine(gdl, wp0, wp1, clips[i]);
                }
                if (i == selectedClip || IsSelected(i)) gdl->AddRect(cp0, cp1, IM_COL32(180, 255, 180, 255), 3.0f, 0, i == selectedClip ? 2.0f : 1.2f);
                DrawFadeGraph(gdl, cp0, cp1, clips[i].duration, clips[i].opIn, clips[i].opOut, IM_COL32(255, 255, 255, 235));
                std::string label = "GS";
                if (clips[i].mediaId >= 0 && clips[i].mediaId < (int)media.size())
                    label = media[clips[i].mediaId].name.substr(0, 16);
                gdl->AddText(ImVec2(cp0.x + 6, cp0.y + 4), IM_COL32(240, 255, 240, 240), label.c_str());
                DrawAudioArrow(gdl, cp0, cp1, open);
                ImVec2 hit1 = open ? wp1 : cp1;
                if (!ImGui::IsMouseHoveringRect(cp0, hit1) || TimelinePointerBusy()) continue;
                if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) ateGs = true;
                ImVec2 mouse = ImGui::GetMousePos();
                bool onArrow = AudioArrowHit(cp0, cp1, mouse);
                bool onWave = open && mouse.y >= wp0.y;
                bool atStart = mouse.x < cp0.x + 10.0f;
                bool atEnd = mouse.x > cp1.x - 10.0f;
                if (onArrow) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                else if ((onWave || (!onWave && (atStart || atEnd))) && (atStart || atEnd))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (onArrow) ImGui::SetTooltip(open ? "Close audio" : "Audio");
                if (!onArrow && !onWave && (atStart || atEnd) && ImGui::IsKeyDown(ImGuiKey_LeftShift)) ImGui::SetTooltip("Opacity");
                if (onWave && (atStart || atEnd) && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)))
                    ImGui::SetTooltip("Volume");
                if (open && FadeHandleRole(clips[i], wp0, wp1, w, mouse)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Move volume point");
                }
                if (ImGui::IsMouseClicked(1)) {
                    if (!IsSelected(i)) SelectOnly(i);
                    else { selectedClip = i; ClearLaneRivals(); }
                    contextClip = i;
                    gsPanelClip = i;
                    gsPanelOpen = true;
                    openClipMenu = true;
                    openedPanelThisFrame = true;
                }
                if (ImGui::IsMouseClicked(0)) {
                    bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift);
                    bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                    if (onArrow && !shift && !ctrl) {
                        clips[i].audioOpen = !clips[i].audioOpen;
                        if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                        else SelectOnly(i);
                        continue;
                    }
                    bool opacityDrag = shift && !onWave && (atStart || atEnd);
                    bool volumeDrag = ctrl && onWave && (atStart || atEnd);
                    int fadePt = open ? FadeHandleRole(clips[i], wp0, wp1, w, mouse) : 0;
                    if (fadePt && !shift) {
                        if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                        else SelectOnly(i);
                        EnsureEnvelope(clips[i]);
                        int idx = EnsureFadeAnchor(clips[i], fadePt == 1 ? -1 : 1);
                        if (idx >= 0) {
                            dragVolClip = i;
                            dragVolPt = idx;
                            dragVolRole = fadePt;
                            trimmingClip = -1;
                            draggingClip = -1;
                            fadeDragClip = -1;
                        }
                        continue;
                    }
                    if (opacityDrag || volumeDrag) {
                        if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                        else SelectOnly(i);
                        fadeDragClip = i;
                        fadeDragSide = atStart ? -1 : 1;
                        fadeDragKind = opacityDrag ? 1 : 2;
                        trimmingClip = -1;
                        draggingClip = -1;
                    } else if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                    else SelectOnly(i);
                    if (fadeDragClip == i) {
                    } else if (!onWave && mouse.x < cp0.x + 8) { trimmingClip = i; trimSide = 0; trimAnchorX = mouse.x; trimOrigStart = clips[i].startTime; trimOrigDur = clips[i].duration; trimOrigOffset = clips[i].sourceOffset; }
                    else if (!onWave && mouse.x > cp1.x - 8) { trimmingClip = i; trimSide = 1; trimAnchorX = mouse.x; trimOrigStart = clips[i].startTime; trimOrigDur = clips[i].duration; trimOrigOffset = clips[i].sourceOffset; }
                    else { draggingClip = i; dragOffsetX = mouse.x - cp0.x; trimmingClip = -1; CaptureGroupDrag(i, -1); }
                }
            }
        }
        if (gsZoneClick && !ateGs && !marqueeLive && draggingClip < 0 && trimmingClip < 0) {
            if (!UnselectIfMany()) BeginMarquee(1, ImGui::GetMousePos());
        }
        ImGui::EndChild();

        ImGui::BeginChild("subslane", ImVec2(timeW + 90.0f, laneH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted("SUBS");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtitles");
        ImGui::SameLine(46.0f);
        ImVec2 subStart = ImGui::GetCursorScreenPos();
        subsOriginX = subStart.x;
        subsLaneY = subStart.y - 4.0f;
        ImGui::InvisibleButton("subszone", ImVec2(std::max(10.0f, ImGui::GetContentRegionAvail().x), laneH - 4));
        bool subZoneClick = ImGui::IsItemClicked(0) && !TimelinePointerBusy();
        bool ateSub = false;
        bool subStyleClick = ImGui::IsItemClicked(1) && !TimelinePointerBusy();
        {
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            for (int i = 0; i < (int)subs.size(); i++) {
                float x = subStart.x + subs[i].start * pixelsPerSecond;
                float w = std::max(28.0f, subs[i].duration * pixelsPerSecond);
                ImVec2 cp0(x, subStart.y);
                ImVec2 cp1(x + w, subStart.y + laneH - 8);
                sdl->AddRectFilled(cp0, cp1, IM_COL32(150, 90, 40, 230), 3.0f);
                if (i == selectedSub || std::find(subSelection.begin(), subSelection.end(), i) != subSelection.end())
                    sdl->AddRect(cp0, cp1, IM_COL32(255, 220, 160, 255), 3.0f, 0, i == selectedSub ? 2.0f : 1.2f);
                std::string label = subs[i].text.empty() ? "Text" : subs[i].text.substr(0, 22);
                if (HasHebrew(label)) label = ReverseUtf8(label);
                sdl->AddText(ImVec2(cp0.x + 6, cp0.y + 4), IM_COL32(255, 245, 230, 240), label.c_str());
                if (!ImGui::IsMouseHoveringRect(cp0, cp1) || TimelinePointerBusy()) continue;
                ateSub = true;
                ImVec2 mouse = ImGui::GetMousePos();
                if (mouse.x < cp0.x + 8 || mouse.x > cp1.x - 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsMouseClicked(1)) {
                    selectedSub = i;
                    subSelection.clear();
                    subSelection.push_back(i);
                    selectedClip = -1;
                    selection.clear();
                    contextClip = -1;
                    ClearLaneRivals();
                    subStyleOpen = true;
                    openedPanelThisFrame = true;
                    subStyleClick = false;
                }
                if (ImGui::IsMouseClicked(0)) {
                    bool keep = std::find(subSelection.begin(), subSelection.end(), i) != subSelection.end();
                    ClearLaneRivals();
                    if (keep) selectedSub = i;
                    else {
                        selectedSub = i;
                        subSelection.clear();
                        subSelection.push_back(i);
                        selectedClip = -1;
                        selection.clear();
                    }
                    if (mouse.x < cp0.x + 8) { trimmingSub = i; trimSubSide = 0; }
                    else if (mouse.x > cp1.x - 8) { trimmingSub = i; trimSubSide = 1; }
                    else { draggingSub = i; subDragOffset = mouse.x - cp0.x; trimmingSub = -1; CaptureGroupDrag(-1, i); }
                    LoadSubEditor(subs[i].text);
                    editingSub = i;
                    subEditorOpen = true;
                    openedPanelThisFrame = true;
                }
            }
        }
        if (subStyleClick) { subStyleOpen = true; openedPanelThisFrame = true; }
        if (subZoneClick && !ateSub && !marqueeLive) {
            if (!UnselectIfMany()) BeginMarquee(3, ImGui::GetMousePos());
        }
        ImGui::EndChild();

        for (int t = 0; t < 6; t++) {
            ImGui::PushID(t);
            float thisH = (t < 3 && LaneHasAudioOpen(t)) ? openH : closedH;
            trackLaneH[t] = thisH;
            ImGui::BeginChild("track", ImVec2(timeW + 90.0f, thisH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::Text("%s", trackNames[t]);
            ImGui::SameLine(46.0f);
            ImVec2 trackStart = ImGui::GetCursorScreenPos();
            trackPositions[t] = trackStart;
            trackLanePos[t] = trackStart;
            float trackW = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("dropzone", ImVec2(trackW, thisH - 4));

            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA");
                const ImGuiPayload* solid = payload ? nullptr : ImGui::AcceptDragDropPayload("SOLID");
                const ImGuiPayload* trans = (payload || solid || t >= 3) ? nullptr : ImGui::AcceptDragDropPayload("TRANS");
                int mid = -1;
                if (payload) mid = *(const int*)payload->Data;
                else if (solid) mid = EnsureSolidMedia(*(const int*)solid->Data);
                else if (trans && trans->DataSize == (int)sizeof(int)) {
                    int fxIdx = *(const int*)trans->Data;
                    ClipEffect* fx = FxAt(fxIdx);
                    if (fx) {
                        int n = ApplyFxToSelection(fx->id(), 1.0f);
                        previewStatus = n ? std::string(fx->label()) + " on the overlap" : "Select two overlapping clips";
                    }
                }
                if (mid >= 0 && mid < (int)media.size()) {
                    bool isVideo = (media[mid].type == "video" || media[mid].type == "image");
                    bool isAudio = (media[mid].type == "audio");
                    if ((t < 3 && isVideo) || (t >= 3 && isAudio)) {
                            Clip c;
                            c.mediaId = mid;
                            c.duration = media[mid].duration;
                            c.sourceOffset = 0;
                            c.track = t;
                            float localX = ImGui::GetMousePos().x - trackStart.x;
                            AddClipOnTrack(c, localX / pixelsPerSecond);
                            SelectOnly((int)clips.size() - 1);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImDrawList* tdl = ImGui::GetWindowDrawList();
            ImVec2 trackClip0 = ImGui::GetWindowPos();
            ImVec2 trackClip1(trackClip0.x + ImGui::GetWindowSize().x, trackClip0.y + ImGui::GetWindowSize().y);
            for (int i = 0; i < (int)clips.size(); i++) {
                if (clips[i].track != t) continue;
                float x = trackStart.x + clips[i].startTime * pixelsPerSecond;
                float w = std::max(24.0f, clips[i].duration * pixelsPerSecond);
                bool audioLane = t >= 3;
                bool open = !audioLane && clips[i].audioOpen;
                ImVec2 cp0(x, trackStart.y);
                ImVec2 cp1(x + w, trackStart.y + kBodyH);
                ImVec2 wp0 = audioLane ? cp0 : ImVec2(x, cp1.y + kWaveGap);
                ImVec2 wp1 = audioLane ? cp1 : ImVec2(x + w, wp0.y + kWaveH);
                ImU32 ccol = (t < 3) ? IM_COL32(60, 120, 200, 220) : IM_COL32(70, 160, 90, 220);
                tdl->AddRectFilled(cp0, cp1, ccol, 3.0f);
                int mid = clips[i].mediaId;
                if (mid >= 0 && mid < (int)media.size() && media[mid].thumb) {
                    float tw = std::min(40.0f, w - 8.0f);
                    if (tw > 10.0f)
                        DrawThumb(tdl, cp0, ImVec2(cp0.x + tw, cp1.y), trackClip0, trackClip1, media[mid].thumb, media[mid].thumbW, media[mid].thumbH);
                }
                if (audioLane) {
                    DrawClipBody(tdl, cp0, cp1, clips[i]);
                    DrawVolumeLine(tdl, cp0, cp1, clips[i]);
                } else if (open) {
                    tdl->AddRectFilled(wp0, wp1, IM_COL32(12, 18, 28, 235), 2.0f);
                    DrawClipBody(tdl, wp0, wp1, clips[i]);
                    DrawVolumeLine(tdl, wp0, wp1, clips[i]);
                }
                if (i == selectedClip || IsSelected(i)) {
                    if (i == selectedClip) EnsureEnvelope(clips[i]);
                    tdl->AddRect(cp0, cp1, IM_COL32(255, 220, 60, 255), 3.0f, 0, i == selectedClip ? 2.0f : 1.2f);
                    if (i == selectedClip) {
                        tdl->AddLine(cp0, ImVec2(cp0.x, cp1.y), IM_COL32(255, 255, 255, 255), 4.0f);
                        tdl->AddLine(ImVec2(cp1.x, cp0.y), cp1, IM_COL32(255, 255, 255, 255), 4.0f);
                        if (audioLane || open) {
                            for (int p = 0; p < (int)clips[i].volume.size(); p++) {
                                if (VolPointHidden(clips[i], clips[i].volume[p].norm)) continue;
                                float px = wp0.x + clips[i].volume[p].norm * w;
                                float py = VolPointY(wp0.y, wp1.y, clips[i].volume[p].gain);
                                tdl->AddCircleFilled(ImVec2(px, py), 4.5f, IM_COL32(255, 255, 255, 255));
                            }
                        }
                    }
                }
                if (!audioLane) DrawFadeGraph(tdl, cp0, cp1, clips[i].duration, clips[i].opIn, clips[i].opOut, IM_COL32(255, 255, 255, 235));
                if (t < 3) DrawAudioArrow(tdl, cp0, cp1, clips[i].audioOpen);
                std::string label = media[clips[i].mediaId].name.substr(0, 14);
                char extra[48];
                snprintf(extra, sizeof(extra), " %.1fs", clips[i].duration);
                label += extra;
                if (clips[i].speed > 1.01f) {
                    char sp[16];
                    snprintf(sp, sizeof(sp), " x%.2g", clips[i].speed);
                    label += sp;
                }
                if (clips[i].fadeIn) label += " FI";
                if (clips[i].fadeOut) label += " FO";
                if (!clips[i].fxOut.empty()) {
                    ClipEffect* fx = FxFind(clips[i].fxOut.c_str());
                    if (fx) {
                        label += " ";
                        label += fx->badge();
                    }
                }
                tdl->AddText(ImVec2(cp0.x + 44, cp0.y + 4), IM_COL32(255, 255, 255, 230), label.c_str());

                ImVec2 hover1 = (!audioLane && open) ? wp1 : cp1;
                if (ImGui::IsMouseHoveringRect(cp0, hover1)) {
                    float mx = ImGui::GetMousePos().x;
                    float my = ImGui::GetMousePos().y;
                    bool edge = mx < cp0.x + 10.0f || mx > cp1.x - 10.0f;
                    bool onArrow = t < 3 && AudioArrowHit(cp0, cp1, ImGui::GetMousePos());
                    if (onArrow) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip(open ? "Close audio" : "Audio");
                    } else if (edge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    if (!audioLane && !onArrow && edge && my < wp0.y && ImGui::IsKeyDown(ImGuiKey_LeftShift)) ImGui::SetTooltip("Opacity");
                    bool volEdge = edge && (audioLane || (open && my >= wp0.y));
                    if (volEdge && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)))
                        ImGui::SetTooltip("Volume");
                    if ((audioLane || open) && FadeHandleRole(clips[i], wp0, wp1, w, ImGui::GetMousePos())) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("Move volume point");
                    }
                }
            }

            bool ateClip = false;
            if (draggingClip < 0 && trimmingClip < 0 && dragVolClip < 0 && fadeDragClip < 0) {
                for (int i = (int)clips.size() - 1; i >= 0; --i) {
                    if (clips[i].track != t) continue;
                    float x = trackStart.x + clips[i].startTime * pixelsPerSecond;
                    float w = std::max(24.0f, clips[i].duration * pixelsPerSecond);
                    bool audioLane = t >= 3;
                    bool open = !audioLane && clips[i].audioOpen;
                    ImVec2 cp0(x, trackStart.y);
                    ImVec2 cp1(x + w, trackStart.y + kBodyH);
                    ImVec2 wp0 = audioLane ? cp0 : ImVec2(x, cp1.y + kWaveGap);
                    ImVec2 wp1 = audioLane ? cp1 : ImVec2(x + w, wp0.y + kWaveH);
                    ImVec2 hit1 = (!audioLane && open) ? wp1 : cp1;
                    if (!ImGui::IsMouseHoveringRect(cp0, hit1) || TimelinePointerBusy()) continue;
                    if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseDoubleClicked(0)) ateClip = true;
                    ImVec2 mouse = ImGui::GetMousePos();
                    bool onArrow = t < 3 && AudioArrowHit(cp0, cp1, mouse);
                    bool onStrip = !audioLane && open && mouse.y >= wp0.y;
                    bool volHere = audioLane || onStrip;
                    bool atStart = mouse.x < cp0.x + 10.0f;
                    bool atEnd = mouse.x > cp1.x - 10.0f;
                    if (ImGui::IsMouseClicked(0)) {
                        bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift);
                        bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                        if (onArrow && !shift && !ctrl) {
                            clips[i].audioOpen = !clips[i].audioOpen;
                            if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                            else SelectOnly(i);
                            break;
                        }
                        bool opacityDrag = !audioLane && shift && !onStrip && (atStart || atEnd);
                        bool volumeDrag = ctrl && volHere && (atStart || atEnd);
                        int fadePt = volHere ? FadeHandleRole(clips[i], wp0, wp1, w, mouse) : 0;
                        if (fadePt && !shift) {
                            if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                            else SelectOnly(i);
                            EnsureEnvelope(clips[i]);
                            int idx = EnsureFadeAnchor(clips[i], fadePt == 1 ? -1 : 1);
                            if (idx >= 0) {
                                dragVolClip = i;
                                dragVolPt = idx;
                                dragVolRole = fadePt;
                                trimmingClip = -1;
                                draggingClip = -1;
                                fadeDragClip = -1;
                            }
                            break;
                        }
                        if (opacityDrag || volumeDrag) {
                            if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                            else SelectOnly(i);
                            fadeDragClip = i;
                            fadeDragSide = atStart ? -1 : 1;
                            fadeDragKind = opacityDrag ? 1 : 2;
                            trimmingClip = -1;
                            draggingClip = -1;
                            break;
                        }
                    }
                    if (ImGui::IsMouseClicked(0) && std::fabs(mouse.x - (trackStart.x + currentTime * pixelsPerSecond)) <= 6.0f) {
                        draggingPlayhead = true;
                        shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                        ResetAudioSync();
                        if (player) player->lastPts = -1;
#endif
                        break;
                    }
                    if (ImGui::IsMouseClicked(1)) {
                        if (!IsSelected(i)) SelectOnly(i);
                        else { selectedClip = i; ClearLaneRivals(); }
                        contextClip = i;
                        EnsureEnvelope(clips[i]);
                        openClipMenu = true;
                        break;
                    }
                    if (ImGui::IsMouseDoubleClicked(0) && volHere) {
                        if (FadeHandleRole(clips[i], wp0, wp1, w, mouse)) break;
                        SelectOnly(i);
                        EnsureEnvelope(clips[i]);
                        float n = (mouse.x - wp0.x) / w;
                        float g = (wp1.y - mouse.y) / std::max(1.0f, wp1.y - wp0.y);
                        if (n < 0) n = 0;
                        if (n > 1) n = 1;
                        if (g < 0) g = 0;
                        if (g > 1) g = 1;
                        if (!VolPointHidden(clips[i], n)) {
                            auto& pts = clips[i].volume;
                            size_t at = 0;
                            while (at < pts.size() && pts[at].norm < n) at++;
                            pts.insert(pts.begin() + at, {n, g});
                        }
                        break;
                    }
                    if (!ImGui::IsMouseClicked(0)) continue;
                    if (ImGui::GetIO().KeyShift && !onStrip && !audioLane) {
                        ToggleSelect(i);
                        break;
                    }
                    if (IsSelected(i)) { selectedClip = i; ClearLaneRivals(); }
                    else SelectOnly(i);
                    EnsureEnvelope(clips[i]);
                    int hit = -1;
                    if (volHere) {
                        for (int p = 0; p < (int)clips[i].volume.size(); p++) {
                            if (VolPointHidden(clips[i], clips[i].volume[p].norm)) continue;
                            float px = wp0.x + clips[i].volume[p].norm * w;
                            float py = VolPointY(wp0.y, wp1.y, clips[i].volume[p].gain);
                            float dx = mouse.x - px, dy = mouse.y - py;
                            if (dx * dx + dy * dy <= 64.0f) { hit = p; break; }
                        }
                    }
                    if (hit >= 0) {
                        dragVolClip = i;
                        dragVolPt = hit;
                        float dur = clips[i].duration;
                        float local = clips[i].volume[hit].norm * dur;
                        if (clips[i].volIn > 0.02f && std::fabs(local - clips[i].volIn) <= 0.06f) dragVolRole = 1;
                        else if (clips[i].volOut > 0.02f && std::fabs((dur - local) - clips[i].volOut) <= 0.06f) dragVolRole = 2;
                        else dragVolRole = 0;
                    } else if (!onStrip) {
                        float mx = mouse.x;
                        bool edge = (mx < cp0.x + 8.0f) || (mx > cp1.x - 8.0f);
                        if (edge || activeTool == 1) {
                            trimmingClip = i;
                            trimSide = (mx < cp0.x + 8.0f) ? -1 : 1;
                            if (mx > cp1.x - 8.0f) trimSide = 1;
                            trimAnchorX = mx;
                            trimOrigStart = clips[i].startTime;
                            trimOrigDur = clips[i].duration;
                            trimOrigOffset = clips[i].sourceOffset;
                        } else {
                            draggingClip = i;
                            dragOffsetX = mx - cp0.x;
                            CaptureGroupDrag(i, -1);
                        }
                    } else {
                        draggingClip = i;
                        dragOffsetX = mouse.x - cp0.x;
                        CaptureGroupDrag(i, -1);
                    }
                    break;
                }
            }
            if (!ateClip && ImGui::IsItemClicked(0) && !TimelinePointerBusy()
                && draggingClip < 0 && trimmingClip < 0 && dragVolClip < 0 && fadeDragClip < 0 && !marqueeLive) {
                ImVec2 mouse = ImGui::GetMousePos();
                float phx = trackStart.x + currentTime * pixelsPerSecond;
                if (std::fabs(mouse.x - phx) <= 6.0f) {
                    draggingPlayhead = true;
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
                    if (player) player->lastPts = -1;
#endif
                } else if (!UnselectIfMany()) {
                    BeginMarquee(t < 3 ? 1 : 2, mouse);
                }
            }
            ImGui::EndChild();
            ImGui::PopID();
        }

        if (TimelinePointerBusy()) ClearTimelineDrags();

        if (marqueeLive) {
            if (ImGui::IsMouseDown(0)) {
                ImVec2 m = ImGui::GetMousePos();
                float dx = m.x - marqueeStart.x;
                float dy = m.y - marqueeStart.y;
                if (dx * dx + dy * dy > 16.0f) marqueeMoved = true;
                if (marqueeMoved) {
                    ApplyMarquee(m);
                    ImDrawList* fg = ImGui::GetForegroundDrawList();
                    ImVec2 a(std::min(marqueeStart.x, m.x), std::min(marqueeStart.y, m.y));
                    ImVec2 b(std::max(marqueeStart.x, m.x), std::max(marqueeStart.y, m.y));
                    fg->AddRectFilled(a, b, IM_COL32(90, 160, 255, 45));
                    fg->AddRect(a, b, IM_COL32(190, 220, 255, 230), 0.0f, 0, 1.5f);
                }
            } else {
                if (marqueeKind == 3 && !marqueeMoved) {
                    Subtitle s;
                    float st = (marqueeStart.x - subsOriginX) / pixelsPerSecond;
                    if (st < 0) st = 0;
                    s.start = SnapTime(-1, st);
                    s.duration = 3.0f;
                    subs.push_back(s);
                    selectedSub = (int)subs.size() - 1;
                    subSelection.clear();
                    subSelection.push_back(selectedSub);
                    selectedClip = -1;
                    selection.clear();
                    selectedBus = -1;
                    editingSub = selectedSub;
                    LoadSubEditor("");
                    subEditorOpen = true;
                    openedPanelThisFrame = true;
                }
                marqueeLive = false;
                marqueeKind = 0;
                marqueeMoved = false;
            }
        }

        if (dragVolClip >= 0 && dragVolClip < (int)clips.size()) {
            int tr = clips[dragVolClip].track;
            bool gsLane = tr == kGsTrack;
            bool avLane = tr >= 0 && tr < 6;
            if (ImGui::IsMouseDown(0) && dragVolPt >= 0 && dragVolPt < (int)clips[dragVolClip].volume.size() && (gsLane || avLane)) {
                ImVec2 mouse = ImGui::GetMousePos();
                bool audioBox = tr >= 3 && tr <= 5;
                float top = audioBox ? trackPositions[tr].y : ((gsLane ? gsLaneY + 4.0f : trackPositions[tr].y) + kBodyH + kWaveGap);
                float bot = audioBox ? top + kBodyH : top + kWaveH;
                float x0 = (gsLane ? gsOriginX : trackPositions[tr].x) + clips[dragVolClip].startTime * pixelsPerSecond;
                float w = std::max(24.0f, clips[dragVolClip].duration * pixelsPerSecond);
                float n = (mouse.x - x0) / w;
                float g = (bot - mouse.y) / std::max(1.0f, bot - top);
                if (n < 0) n = 0;
                if (n > 1) n = 1;
                if (g < 0) g = 0;
                if (g > 1) g = 1;
                Clip& c = clips[dragVolClip];
                auto& pts = c.volume;
                if (dragVolRole == 1 || dragVolRole == 2) {
                    float dur = c.duration;
                    float other = dragVolRole == 1 ? c.volOut : c.volIn;
                    float cap = dur - std::max(0.0f, other) - 0.05f;
                    if (cap < 0.05f) cap = dur * 0.45f;
                    float len = dragVolRole == 1 ? n * dur : (1.0f - n) * dur;
                    if (len < 0.03f) len = 0.0f;
                    if (len > cap) len = cap;
                    if (len <= 0.02f) {
                        if (dragVolPt > 0 && dragVolPt < (int)pts.size() - 1)
                            pts.erase(pts.begin() + dragVolPt);
                        if (dragVolRole == 1) c.volIn = 0.0f;
                        else c.volOut = 0.0f;
                        dragVolPt = -1;
                        dragVolRole = 0;
                        dragVolClip = -1;
                    } else if (dragVolPt > 0 && dragVolPt < (int)pts.size() - 1) {
                        float nn = dragVolRole == 1 ? len / dur : 1.0f - len / dur;
                        float lo = pts[dragVolPt - 1].norm + 0.01f;
                        float hi = pts[dragVolPt + 1].norm - 0.01f;
                        if (nn < lo) nn = lo;
                        if (nn > hi) nn = hi;
                        pts[dragVolPt].norm = nn;
                        pts[dragVolPt].gain = g;
                        if (dragVolRole == 1) c.volIn = nn * dur;
                        else c.volOut = (1.0f - nn) * dur;
                    }
                } else {
                    if (dragVolPt == 0) n = 0;
                    if (dragVolPt == (int)pts.size() - 1) n = 1;
                    if (dragVolPt > 0 && dragVolPt < (int)pts.size() - 1) {
                        float lo = pts[dragVolPt - 1].norm + 0.01f;
                        float hi = pts[dragVolPt + 1].norm - 0.01f;
                        if (n < lo) n = lo;
                        if (n > hi) n = hi;
                    }
                    pts[dragVolPt].norm = n;
                    pts[dragVolPt].gain = g;
                }
                ImGui::SetTooltip("%d", (int)std::lround(g * 128.0f));
                shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                ResetAudioSync();
#endif
            } else {
                dragVolClip = -1;
                dragVolPt = -1;
                dragVolRole = 0;
            }
        }

        if (trimmingClip >= 0 && trimmingClip < (int)clips.size()) {
            if (ImGui::IsMouseDown(0)) {
                ApplyTrim(trimmingClip, ImGui::GetMousePos().x);
                shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                ResetAudioSync();
#endif
            } else trimmingClip = -1;
        }

        if (fadeDragClip >= 0 && fadeDragClip < (int)clips.size()) {
            if (ImGui::IsMouseDown(0)) {
                Clip& c = clips[fadeDragClip];
                int tr = c.track;
                float origin = timeOriginX;
                if (tr == kGsTrack) origin = gsOriginX;
                else if (tr >= 0 && tr < 6) origin = trackPositions[tr].x;
                float x0 = origin + c.startTime * pixelsPerSecond;
                float span = std::max(24.0f, c.duration * pixelsPerSecond);
                float mx = ImGui::GetMousePos().x;
                float len = (fadeDragSide < 0) ? (mx - x0) / pixelsPerSecond : (x0 + span - mx) / pixelsPerSecond;
                if (len < 0.03f) len = 0.0f;
                float cap = c.duration - 0.05f;
                if (cap < 0.0f) cap = 0.0f;
                if (len > cap) len = cap;
                if (fadeDragKind == 2) {
                    float oldLen = fadeDragSide < 0 ? c.volIn : c.volOut;
                    if (fadeDragSide < 0) c.volIn = len;
                    else c.volOut = len;
                    MoveFadeAnchor(c, fadeDragSide, oldLen);
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                } else {
                    if (fadeDragSide < 0) c.opIn = len;
                    else c.opOut = len;
                }
                shownLocal = -1000.0f;
            } else fadeDragClip = -1;
        }

        if (draggingClip >= 0 && draggingClip < (int)clips.size() && trimmingClip < 0 && fadeDragClip < 0) {
            if (ImGui::IsMouseDown(0)) {
                ImVec2 mouse = ImGui::GetMousePos();
                int tr = clips[draggingClip].track;
                float origin = (tr == kGsTrack) ? gsOriginX : trackPositions[tr].x;
                float newStart = (mouse.x - dragOffsetX - origin) / pixelsPerSecond;
                newStart = SnapStart(draggingClip, clips[draggingClip].duration, newStart);
                if (groupClipDrag.empty() && groupSubDrag.empty()) clips[draggingClip].startTime = newStart;
                else ApplyGroupDelta(newStart - groupAnchor);
                int mid = clips[draggingClip].mediaId;
                bool isVideo = mid >= 0 && mid < (int)media.size() && (media[mid].type == "video" || media[mid].type == "image");
                bool isAudio = mid >= 0 && mid < (int)media.size() && media[mid].type == "audio";
                int prevTrack = clips[draggingClip].track;
                if (isVideo && mouse.y >= gsLaneY && mouse.y < gsLaneY + gsChildH) {
                    clips[draggingClip].track = kGsTrack;
                } else {
                    for (int t = 0; t < 6; t++) {
                        float ty = trackPositions[t].y;
                        if (mouse.y >= ty && mouse.y < ty + trackLaneH[t]) {
                            if ((t < 3 && isVideo) || (t >= 3 && isAudio))
                                clips[draggingClip].track = t;
                            break;
                        }
                    }
                }
                if (clips[draggingClip].track != prevTrack) {
                    if (clips[draggingClip].track == kGsTrack) {
                        if (clips[draggingClip].gsFit != 2 && GsScaleIdentity(clips[draggingClip]))
                            ContainGsImage(clips[draggingClip]);
                    } else if (prevTrack == kGsTrack) {
                        ReleaseGsImageContain(clips[draggingClip]);
                    }
                    FollowLeaderChannel(draggingClip, prevTrack);
                }
            } else { draggingClip = -1; ClearGroupDrag(); }
        }

        if (trimmingBus >= 0 && trimmingBus < (int)bus.size()) {
            if (ImGui::IsMouseDown(0)) {
                float mx = (ImGui::GetMousePos().x - busOriginX) / pixelsPerSecond;
                BusSegment& s = bus[trimmingBus];
                if (trimBusSide == 1) {
                    float end = SnapTime(-1, mx);
                    float nd = end - s.start;
                    if (nd < 0.2f) nd = 0.2f;
                    s.duration = nd;
                } else {
                    float end = s.start + s.duration;
                    float ns = SnapTime(-1, mx);
                    if (ns < 0) ns = 0;
                    if (ns > end - 0.2f) ns = end - 0.2f;
                    s.duration = end - ns;
                    s.start = ns;
                }
            } else trimmingBus = -1;
        }

        if (draggingBus >= 0 && draggingBus < (int)bus.size() && trimmingBus < 0) {
            if (ImGui::IsMouseDown(0)) {
                float ns = (ImGui::GetMousePos().x - busDragOffset - busOriginX) / pixelsPerSecond;
                bus[draggingBus].start = SnapTime(-1, ns);
            } else draggingBus = -1;
        }

        if (trimmingOv >= 0 && trimmingOv < (int)gOverlays.size()) {
            if (ImGui::IsMouseDown(0)) {
                float mx = (ImGui::GetMousePos().x - ovOriginX) / pixelsPerSecond;
                OverlayFx& o = gOverlays[trimmingOv];
                if (trimOvSide == 1) {
                    float nd = SnapTime(-1, mx) - o.start;
                    if (nd < 0.2f) nd = 0.2f;
                    o.duration = nd;
                } else {
                    float end = o.start + o.duration;
                    float ns = SnapTime(-1, mx);
                    if (ns < 0) ns = 0;
                    if (ns > end - 0.2f) ns = end - 0.2f;
                    o.duration = end - ns;
                    o.start = ns;
                }
            } else trimmingOv = -1;
        }
        if (draggingOv >= 0 && draggingOv < (int)gOverlays.size() && trimmingOv < 0) {
            if (ImGui::IsMouseDown(0)) {
                float ns = (ImGui::GetMousePos().x - ovDragOffset - ovOriginX) / pixelsPerSecond;
                if (ns < 0) ns = 0;
                gOverlays[draggingOv].start = SnapTime(-1, ns);
            } else draggingOv = -1;
        }

        if (trimmingSub >= 0 && trimmingSub < (int)subs.size()) {
            if (ImGui::IsMouseDown(0) && !TimelinePointerBusy()) {
                float mx = (ImGui::GetMousePos().x - subsOriginX) / pixelsPerSecond;
                Subtitle& s = subs[trimmingSub];
                if (trimSubSide == 1) {
                    float nd = SnapTime(-1, mx) - s.start;
                    if (nd < 0.3f) nd = 0.3f;
                    s.duration = nd;
                } else {
                    float end = s.start + s.duration;
                    float ns = SnapTime(-1, mx);
                    if (ns < 0) ns = 0;
                    if (ns > end - 0.3f) ns = end - 0.3f;
                    s.duration = end - ns;
                    s.start = ns;
                }
            } else trimmingSub = -1;
        }
        if (draggingSub >= 0 && draggingSub < (int)subs.size() && trimmingSub < 0) {
            if (ImGui::IsMouseDown(0) && !TimelinePointerBusy()) {
                float ns = (ImGui::GetMousePos().x - subDragOffset - subsOriginX) / pixelsPerSecond;
                ns = SnapTime(-1, ns);
                if (groupClipDrag.empty() && groupSubDrag.empty()) subs[draggingSub].start = ns;
                else ApplyGroupDelta(ns - groupAnchor);
            } else { draggingSub = -1; ClearGroupDrag(); }
        }

        float phX = trackPositions[0].x + currentTime * pixelsPerSecond;
        ImVec2 tlMin = ImGui::GetWindowPos();
        float tlBottom = tlMin.y + ImGui::GetWindowSize().y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(phX, tlMin.y), ImVec2(phX, tlBottom), IM_COL32(255, 50, 50, 255), 2.0f);

        if (draggingPlayhead) {
            if (ImGui::IsMouseDown(0)) {
                float newTime = (ImGui::GetMousePos().x - trackPositions[0].x) / pixelsPerSecond;
                if (newTime < 0) newTime = 0;
                if (std::fabs(newTime - currentTime) > 0.0001f) {
                    currentTime = newTime;
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
                    if (player) player->lastPts = -1;
#endif
                }
            } else draggingPlayhead = false;
        }

        if (isPlaying || draggingClip >= 0 || draggingPlayhead || trimmingClip >= 0 || fadeDragClip >= 0) {
            float mx = ImGui::GetMousePos().x;
            float viewL = tlMin.x;
            float viewR = tlMin.x + ImGui::GetWindowSize().x;
            if (draggingClip >= 0 || draggingPlayhead || trimmingClip >= 0 || fadeDragClip >= 0) {
                if (mx > viewR - 28.0f) ImGui::SetScrollX(ImGui::GetScrollX() + 30.0f);
                else if (mx < viewL + 16.0f) ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() - 30.0f));
            }
            if (isPlaying && !draggingPlayhead) {
                float phContent = ImGui::GetScrollX() + (phX - tlMin.x);
                float viewRight = ImGui::GetScrollX() + ImGui::GetWindowSize().x - 80.0f;
                if (phContent > viewRight)
                    ImGui::SetScrollX(std::max(0.0f, phContent - ImGui::GetWindowSize().x * 0.35f));
            }
        }

        ImGui::Dummy(ImVec2(timeW + 90.0f, 1.0f));
        ImGui::EndChild();

        ImGui::SetCursorPos(rulerCursor);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowScroll(ImVec2(bodyScrollX, 0.0f));
        ImGui::BeginChild("ruler", ImVec2(-meterW, rulerH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        float gutter = 46.0f;
        ImGui::SetCursorPos(ImVec2(gutter, 4.0f));
        float rulerOrigin = ImGui::GetCursorScreenPos().x;
        ImGui::InvisibleButton("rulerhit", ImVec2(std::max(10.0f, timeW), 16.0f));
        {
            ImDrawList* rdl = ImGui::GetWindowDrawList();
            ImVec2 rr0 = ImGui::GetItemRectMin();
            ImVec2 rr1 = ImGui::GetItemRectMax();
            for (int s = 0; s <= (int)tlSeconds; s++) {
                float x = rulerOrigin + s * pixelsPerSecond;
                bool major = (s % 5) == 0;
                rdl->AddLine(ImVec2(x, rr0.y + (major ? 0.0f : 8.0f)), ImVec2(x, rr1.y), IM_COL32(180, 180, 180, major ? 230 : 110));
                if (major) {
                    char lab[16];
                    snprintf(lab, sizeof(lab), "%d:%02d", s / 60, s % 60);
                    rdl->AddText(ImVec2(x + 2.0f, rr0.y - 2.0f), IM_COL32(210, 210, 210, 255), lab);
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                float nt = (ImGui::GetMousePos().x - rulerOrigin) / pixelsPerSecond;
                if (nt < 0) nt = 0;
                currentTime = nt;
                shownLocal = -1000.0f;
                draggingPlayhead = true;
#ifdef HAVE_FFMPEG
                ResetAudioSync();
                if (player) player->lastPts = -1;
#endif
            }
            ImVec2 wp = ImGui::GetWindowPos();
            float ph = rulerOrigin + currentTime * pixelsPerSecond;
            rdl->AddLine(ImVec2(ph, wp.y), ImVec2(ph, wp.y + ImGui::GetWindowSize().y), IM_COL32(255, 50, 50, 255), 2.0f);
        }
        ImGui::SetCursorPos(ImVec2(timeW + 90.0f + ImGui::GetStyle().ScrollbarSize, 0.0f));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ImGui::EndChild();
        {
            static float showL = 0.0f, showR = 0.0f, hold = 0.0f;
            float fall = std::exp(-std::max(0.0f, io.DeltaTime) * 8.0f);
            float holdFall = std::exp(-std::max(0.0f, io.DeltaTime) * 1.4f);
            if (masterRawL > showL) showL = masterRawL; else showL *= fall;
            if (masterRawR > showR) showR = masterRawR; else showR *= fall;
            float loud = showL > showR ? showL : showR;
            if (loud >= hold) hold = loud; else hold *= holdFall;
            masterRawL = 0.0f;
            masterRawR = 0.0f;
            ImVec2 host = ImGui::GetWindowPos();
            ImVec2 hostSz = ImGui::GetWindowSize();
            float pad = ImGui::GetStyle().WindowPadding.x;
            ImGui::SetCursorScreenPos(ImVec2(host.x + hostSz.x - meterW - pad * 0.5f, host.y + rulerH + 2.0f));
            float colH = hostSz.y - rulerH - 10.0f;
            if (colH < 48.0f) colH = 48.0f;
            ImGui::BeginChild("masterout", ImVec2(meterW - 4.0f, colH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
            float meterH = colH * 0.46f;
            if (meterH < 36.0f) meterH = 36.0f;
            if (meterH > colH - 48.0f) meterH = colH - 48.0f;
            ImVec2 m0 = ImGui::GetCursorScreenPos();
            float mW = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("meterhit", ImVec2(mW, meterH));
            ImDrawList* mdl = ImGui::GetWindowDrawList();
            ImVec2 m1(m0.x + mW, m0.y + meterH);
            mdl->AddRectFilled(m0, m1, IM_COL32(22, 22, 22, 255), 3.0f);
            auto segColor = [](float t) -> ImU32 {
                if (t >= 0.86f) return IM_COL32(220, 36, 32, 255);
                if (t >= 0.62f) return IM_COL32(230, 196, 32, 255);
                return IM_COL32(36, 196, 64, 255);
            };
            float levels[2] = {showL, showR};
            float gap = 3.0f;
            float barW = (mW - 16.0f - gap) * 0.5f;
            if (barW < 6.0f) barW = 6.0f;
            float x0 = m0.x + (mW - (barW * 2.0f + gap)) * 0.5f;
            float innerTop = m0.y + 5.0f;
            float innerBot = m1.y - 5.0f;
            float innerH = innerBot - innerTop;
            int segs = (int)(innerH / 4.0f);
            if (segs < 8) segs = 8;
            if (segs > 48) segs = 48;
            for (int ch = 0; ch < 2; ch++) {
                float bx = x0 + ch * (barW + gap);
                for (int s = 0; s < segs; s++) {
                    float t0 = (float)s / (float)segs;
                    float t1 = (float)(s + 1) / (float)segs;
                    bool on = levels[ch] >= t0 + 0.5f / (float)segs;
                    float y1 = innerBot - t0 * innerH;
                    float y0 = innerBot - t1 * innerH;
                    mdl->AddRectFilled(ImVec2(bx, y0 + 0.6f), ImVec2(bx + barW, y1 - 0.4f), on ? segColor(t0) : IM_COL32(40, 40, 40, 255));
                }
            }
            if (hold > 0.02f && innerH > 4.0f) {
                float hy = innerBot - std::min(1.0f, hold) * innerH;
                mdl->AddLine(ImVec2(x0, hy), ImVec2(x0 + barW * 2.0f + gap, hy), IM_COL32(245, 245, 245, 230), 1.4f);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output");
            float faderH = ImGui::GetContentRegionAvail().y;
            if (faderH < 36.0f) faderH = 36.0f;
            float sliderW = 18.0f;
            ImGui::SetCursorPosX((mW - sliderW) * 0.5f);
            ImVec2 f0 = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.78f, 0.78f, 0.80f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.92f, 0.92f, 0.94f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14.0f);
            bool moved = ImGui::VSliderFloat("##master", ImVec2(sliderW, faderH), &masterGain, 0.0f, 1.5f, "");
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (moved) {
                if (masterGain < 0.0f) masterGain = 0.0f;
                if (masterGain > 1.5f) masterGain = 1.5f;
#ifdef HAVE_FFMPEG
                ResetAudioSync();
#endif
            }
            ImVec2 f1 = ImGui::GetItemRectMax();
            float unity = 1.0f / 1.5f;
            float uy = f1.y - unity * (f1.y - f0.y);
            mdl->AddLine(ImVec2(f0.x - 7.0f, uy), ImVec2(f0.x - 2.0f, uy), IM_COL32(220, 220, 220, 220), 1.2f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master %d%%", (int)std::lround(masterGain * 100.0f));
            ImGui::EndChild();
        }
        ImGui::EndChild();

        if (openClipMenu) {
            ImGui::OpenPopup("clipctx");
            openClipMenu = false;
        }
        if (ImGui::BeginPopup("clipctx")) {
            if (contextClip >= 0 && contextClip < (int)clips.size()) {
                Clip& c = clips[contextClip];
                ImGui::TextUnformatted("Clip");
                ImGui::Separator();
                if (ImGui::MenuItem("Fade In", nullptr, &c.fadeIn)) {
                    bool v = c.fadeIn;
                    for (int i : selection)
                        if (i >= 0 && i < (int)clips.size()) clips[i].fadeIn = v;
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
                if (ImGui::MenuItem("Fade Out", nullptr, &c.fadeOut)) {
                    bool v = c.fadeOut;
                    for (int i : selection)
                        if (i >= 0 && i < (int)clips.size()) clips[i].fadeOut = v;
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
                if (ImGui::MenuItem("Mute", nullptr, &c.muted)) {
                    bool v = c.muted;
                    for (int i : selection)
                        if (i >= 0 && i < (int)clips.size()) clips[i].muted = v;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
                if (ImGui::MenuItem("Channel up")) NudgeSelectionChannel(-1);
                if (ImGui::MenuItem("Channel down")) NudgeSelectionChannel(1);
                if (c.mediaId >= 0 && c.mediaId < (int)media.size() && media[c.mediaId].type != "audio") {
                    if (ImGui::MenuItem("Transform")) {
                        xformClip = contextClip;
                        xformOpen = true;
                        openedPanelThisFrame = true;
                    }
                    if (ImGui::MenuItem("Color")) {
                        colorClip = contextClip;
                        colorOpen = true;
                        openedPanelThisFrame = true;
                    }
                }
                if (ImGui::BeginMenu("Speed")) {
                    const float speeds[] = {1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
                    const char* labels[] = {"1.0x", "1.1x", "1.25x", "1.5x", "1.75x", "2x"};
                    for (int s = 0; s < 6; s++) {
                        bool on = std::fabs(c.speed - speeds[s]) < 0.02f;
                        if (ImGui::MenuItem(labels[s], nullptr, on)) {
                            for (int i : selection)
                                if (i >= 0 && i < (int)clips.size()) SetClipSpeed(clips[i], speeds[s]);
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Audio Effects")) {
                    fxEdit = contextClip;
                    audioPanelOpen = true;
                    openedPanelThisFrame = true;
                }
            }
            ImGui::EndPopup();
        }

        busPanelHoldsMouse = false;
        if (busGradeOpen && contextBus >= 0 && contextBus < (int)bus.size()) {
            PlacePanel(0.0f);
            bool open = true;
            if (ClickOutsidePanels()) open = false;
            if (ImGui::Begin("Bus grade", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                BusSegment& s = bus[contextBus];
                ImGui::TextUnformatted("Bus grade");
                ImGui::Separator();
                ImGui::SliderFloat("Lightness", &s.lightness, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Contrast", &s.contrast, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Warmth", &s.warmth, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Tint", &s.tint, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Saturation", &s.saturation, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Shadow", &s.shadow, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Film grain", &s.grain, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Sharpen", &s.sharpen, 0.0f, 1.0f, "%.2f");
                ImGui::Separator();
                RunLook("grade",
                    [&](const std::string& p) { return WriteGradeLook(p, s); },
                    [&](const std::string& p) { return ReadGradeLook(p, s); });
                if (ImGui::Button("Reset")) {
                    s.lightness = s.contrast = s.warmth = s.tint = s.saturation = s.shadow = s.grain = s.sharpen = 0;
                }
                ImGui::SameLine();
                if (ImGui::Button("CP")) CopyBusGrade();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy this grade to the other SUB segments");
                ImGui::SameLine();
                if (ImGui::Button("Delete segment")) {
                    bus.erase(bus.begin() + contextBus);
                    selectedBus = -1;
                    contextBus = -1;
                    open = false;
                }
                ImVec2 w0 = ImGui::GetWindowPos();
                ImVec2 w1 = ImVec2(w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                busPanelRect = ImVec4(w0.x, w0.y, w1.x, w1.y);
                busPanelHoldsMouse = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                    || (ImGui::IsWindowFocused() && ImGui::IsAnyItemActive());
            }
            ImGui::End();
            if (!open) {
                busGradeOpen = false;
                busPanelHoldsMouse = false;
            }
        }

        ImGui::End();

        if (requestExport) {
            ImGui::OpenPopup("Export");
            requestExport = false;
        }
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Export", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(exOutKind == 1 ? "Export audio" : "Export video");
            ImGui::Separator();
            ImGui::TextUnformatted("Output");
            if (ImGui::RadioButton("Video", exOutKind == 0)) exOutKind = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Audio MP3", exOutKind == 1)) exOutKind = 1;
            ImGui::Spacing();
            int ew = 0, eh = 0;
            int fps = 30;
            if (exOutKind == 0) {
            ImGui::TextUnformatted("Aspect");
            if (ImGui::RadioButton("16:9", exAspect == 0) && exAspect != 0) { exAspect = 0; RefitAutoGsImages(); }
            ImGui::SameLine();
            if (ImGui::RadioButton("9:16", exAspect == 1) && exAspect != 1) { exAspect = 1; RefitAutoGsImages(); }
            ImGui::SameLine();
            if (ImGui::RadioButton("1:1", exAspect == 2) && exAspect != 2) { exAspect = 2; RefitAutoGsImages(); }
            ImGui::SameLine();
            if (ImGui::RadioButton("4:3", exAspect == 3) && exAspect != 3) { exAspect = 3; RefitAutoGsImages(); }
            ImGui::SameLine();
            if (ImGui::RadioButton("3:4", exAspect == 4) && exAspect != 4) { exAspect = 4; RefitAutoGsImages(); }
            ImGui::Spacing();
            ImGui::TextUnformatted("Resolution");
            if (ImGui::RadioButton("720p", exRes == 0)) exRes = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("1376x768", exRes == 5)) exRes = 5;
            ImGui::SameLine();
            if (ImGui::RadioButton("1080p", exRes == 1)) exRes = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("2K", exRes == 2)) exRes = 2;
            ImGui::SameLine();
            if (ImGui::RadioButton("4K", exRes == 3)) exRes = 3;
            ImGui::SameLine();
            if (ImGui::RadioButton("8K", exRes == 4)) exRes = 4;
            ImGui::Spacing();
            ImGui::TextUnformatted("FPS");
            if (ImGui::RadioButton("24", exFpsIdx == 0)) exFpsIdx = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("30", exFpsIdx == 1)) exFpsIdx = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("50", exFpsIdx == 2)) exFpsIdx = 2;
            ImGui::SameLine();
            if (ImGui::RadioButton("60", exFpsIdx == 3)) exFpsIdx = 3;
            ImGui::SameLine();
            if (ImGui::RadioButton("72", exFpsIdx == 6)) exFpsIdx = 6;
            ImGui::SameLine();
            if (ImGui::RadioButton("120", exFpsIdx == 4)) exFpsIdx = 4;
            ImGui::SameLine();
            if (ImGui::RadioButton("240", exFpsIdx == 5)) exFpsIdx = 5;
            const int fpsVals[] = {24, 30, 50, 60, 120, 240, 72};
            fps = fpsVals[exFpsIdx < 0 || exFpsIdx > 6 ? 1 : exFpsIdx];
            bool highFps = fps >= 120;
            bool triple = fps == 72;
            if (highFps || triple) exRife = true;
            ExportDims(exAspect, exRes, ew, eh);
            ImGui::Separator();
            ImGui::Text("Output: %d x %d   @ %d fps", ew, eh, fps);
            if (exRes == 3 || exRes == 4) ImGui::TextDisabled("4K and 8K take a long time.");
            ImGui::BeginDisabled(highFps || triple);
            ImGui::Checkbox("Frame interpolation", &exRife);
            ImGui::EndDisabled();
            if (triple)
                ImGui::TextDisabled("72 is multiplier 3 from a 24 fps render, ensemble on, like the ComfyUI graph. Subtitles are added after.");
            else if (highFps)
                ImGui::TextDisabled("120 and 240 are made with RIFE from a 60 fps render. Ensemble is on. Subtitles are added after.");
            else
                ImGui::TextDisabled("Renders the movie first, then fills frames on the GPU. Ensemble is on. 50 or 60 fps. Subtitles are added after.");
            ImGui::TextDisabled("%s", RifeProbe().detail.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("Subtitles");
            if (ImGui::RadioButton("Embedded", exSubMode == 0)) exSubMode = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("SRT file", exSubMode == 1)) exSubMode = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("Both", exSubMode == 2)) exSubMode = 2;
            } else {
                ImGui::TextUnformatted("Timeline mix, 48 kHz stereo, 192 kbps.");
                ImGui::TextDisabled("Video, resolution and subtitles are not written.");
            }
            if (ImGui::Button("Choose file and export")) {
                ImGui::CloseCurrentPopup();
                if (exOutKind == 1) {
                    std::string out = SaveDialog(hwnd, "MP3 audio (*.mp3)\0*.mp3\0", "mp3", "Export Audio");
                    if (!out.empty()) {
#ifdef HAVE_FFMPEG
                        gExport.BeginMp3(out);
#else
                        previewStatus = "FFmpeg not linked";
#endif
                    }
                } else {
                std::string out = SaveDialog(hwnd, "MP4 video (*.mp4)\0*.mp4\0", "mp4", "Export Video");
                if (!out.empty()) {
                    if (exSubMode != 0) {
                        std::string srt = SwapExt(out, ".srt");
                        previewStatus = WriteSrt(srt) ? "SRT written" : "No subtitles for SRT";
                    }
#ifdef HAVE_FFMPEG
                    gExport.Begin(out, ew, eh, fps, exRife, exSubMode != 1);
#else
                    previewStatus = "FFmpeg not linked";
#endif
                }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

#ifdef HAVE_FFMPEG
        gRam.SetPaused(gExport.active || asrOn);
        if (!gExport.active && !asrOn) gRam.Tick(currentTime);
        if (gExport.active) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Exporting", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
            float p = gExport.total > 0 ? (float)gExport.frameIndex / (float)gExport.total : 0.0f;
            ImGui::ProgressBar(p, ImVec2(380, 0));
            ImGui::TextUnformatted(gExport.status.c_str());
            if (ImGui::Button("Cancel export")) gExport.Cancel();
            ImGui::End();
            bool gpu = gExport.phase == 1;
            bool audioOut = gExport.audioOnly;
            Uint32 slice = SDL_GetTicks();
            int guard = 0;
            int limit = audioOut ? 64 : (gpu ? 16 : (gExport.w * gExport.h >= 3840 * 2160 ? 1 : (gExport.w * gExport.h <= 1280 * 720 ? 4 : 2)));
            do {
                gExport.Step();
                guard++;
            } while (gExport.active && guard < limit && (audioOut ? SDL_GetTicks() - slice < 80 : (!gpu || SDL_GetTicks() - slice < 80)));
        } else if (!asrOn) {
            RoutePreviewClip(currentTime, topClip, previewPlate, driveClip);
            if (driveClip >= 0 && playingClip != driveClip) EnsurePlayerForClip(driveClip);
            SyncPreview(driveClip >= 0 ? driveClip : topClip, io.DeltaTime);
            SyncGreen();
            RoutePreviewClip(currentTime, topClip, previewPlate, driveClip);
            if (fxOther >= 0) {
                if (playingClip != driveClip) EnsurePlayerForClip(driveClip);
                SyncSide(fxOther, currentTime);
                ClipEffect* fx = FxFind(fxActiveId.c_str());
                if (fx) previewStatus += std::string(" | ") + fx->label();
            } else if (topClip >= 0 && playingClip != topClip) {
                EnsurePlayerForClip(topClip);
            }
            SyncBackground(topClip, previewPlate, driveClip);
            if (fxOther < 0) PrefetchUpcoming(topClip);
            StopIfFinished();
        }
        if (asrOn) AsrPump();
#else
        if (isPlaying && !draggingPlayhead) currentTime += io.DeltaTime;
        StopIfFinished();
#endif

        gsPanelHolds = false;
        if (gsPanelOpen && gsPanelClip >= 0 && gsPanelClip < (int)clips.size() && clips[gsPanelClip].track == kGsTrack) {
            PlaceAtCursor();
            bool open = true;
            if (ClickOutsidePanels()) open = false;
            if (ImGui::Begin("Green screen", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                Clip& c = clips[gsPanelClip];
                ImGui::TextUnformatted("Green screen");
                ImGui::Separator();
                DrawPictureControls(c);
                ImGui::Separator();
                SliderTyped("Similarity", &c.keySim, 0.0f, 1.0f, "%.2f");
                SliderTyped("Softness", &c.keySoft, 0.0f, 1.0f, "%.2f");
                SliderTyped("Spill", &c.keySpill, 0.0f, 1.0f, "%.2f");
                SliderTyped("Opacity", &c.gsOpacity, 0.0f, 1.0f, "%.2f");
                ImGui::Separator();
                if (ImGui::Checkbox("Mute", &c.muted)) {
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
                if (ImGui::Button("Color")) {
                    colorClip = gsPanelClip;
                    colorOpen = true;
                    openedPanelThisFrame = true;
                }
                RunLook("gs",
                    [&](const std::string& p) { return WriteGsLook(p, c); },
                    [&](const std::string& p) { return ReadGsLook(p, c); });
                ImVec2 w0 = ImGui::GetWindowPos();
                gsPanelRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                gsPanelHolds = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                    || (ImGui::IsWindowFocused() && ImGui::IsAnyItemActive());
            }
            ImGui::End();
            if (!open) gsPanelOpen = false;
        }
        xformHolds = false;
        if (xformOpen) {
            if (xformClip < 0 || xformClip >= (int)clips.size()) { xformOpen = false; xformDrag = 0; }
            else {
                int mid = clips[xformClip].mediaId;
                bool picture = mid >= 0 && mid < (int)media.size() && media[mid].type != "audio";
                if (!picture) { xformOpen = false; xformDrag = 0; }
                else {
                    PlaceAtCursor();
                    bool open = true;
                    if (ClickOutsidePanels()) open = false;
                    if (ImGui::Begin("Transform", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                        ImGui::TextUnformatted("Transform");
                        ImGui::TextDisabled("Drag in the preview. Corner sizes, top handle rotates, right edge changes aspect.");
                        ImGui::Separator();
                        DrawPictureControls(clips[xformClip]);
                        RunLook("xform",
                            [&](const std::string& p) { return WriteXformLook(p, clips[xformClip]); },
                            [&](const std::string& p) { return ReadXformLook(p, clips[xformClip]); });
                        ImVec2 w0 = ImGui::GetWindowPos();
                        xformRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                        xformHolds = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                            || (GImGui->ActiveIdWindow == ImGui::GetCurrentWindow() && ImGui::IsAnyItemActive());
                    }
                    ImGui::End();
                    if (!open) { xformOpen = false; xformDrag = 0; }
                }
            }
        }
        subPanelHolds = false;
        if (subEditorOpen && editingSub >= 0 && editingSub < (int)subs.size()) {
            PlacePanel(0.0f);
            bool open = true;
            if (ClickOutsidePanels()) open = false;
            if (ImGui::Begin("Subtitle", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted("Subtitle text");
                ImGui::SetNextItemWidth(360);
                ImGui::InputText("##subtext", subEditBuf, sizeof(subEditBuf), ImGuiInputTextFlags_CallbackAlways, SubInputCallback);
                if (editingSub >= 0 && editingSub < (int)subs.size()) {
                    std::string visual = subEditBuf;
                    subs[editingSub].text = HasHebrew(visual) ? ReverseUtf8(visual) : visual;
                }
                ImVec2 w0 = ImGui::GetWindowPos();
                subPanelRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                subPanelHolds = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                    || (ImGui::IsWindowFocused() && ImGui::IsAnyItemActive());
            }
            ImGui::End();
            if (!open) {
                subEditorOpen = false;
                SplitLongSubs();
            }
        }
        if (subStyleOpen) {
            PlacePanel(390.0f);
            bool open = true;
            if (ClickOutsidePanels()) open = false;
            if (ImGui::Begin("Subtitle style", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted("All subtitles");
                ImGui::Separator();
                ImGui::ColorEdit3("Color", subStyle.text);
                ImGui::SliderFloat("Size", &subStyle.fontScale, 0.4f, 2.5f, "%.2f");
                ImGui::Checkbox("Border", &subStyle.border);
                ImGui::SliderFloat("Border size", &subStyle.borderPx, 0.0f, 6.0f, "%.1f");
                ImGui::ColorEdit3("Border color", subStyle.edge);
                ImGui::Checkbox("Background", &subStyle.background);
                ImGui::ColorEdit3("Background color", subStyle.bg);
                ImGui::SliderFloat("Background opacity", &subStyle.bgAlpha, 0.0f, 1.0f, "%.2f");
                RunLook("style",
                    [&](const std::string& p) { return WriteStyleLook(p, subStyle); },
                    [&](const std::string& p) { return ReadStyleLook(p, subStyle); });
                ImVec2 w0 = ImGui::GetWindowPos();
                subStyleRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                    || (ImGui::IsWindowFocused() && ImGui::IsAnyItemActive()))
                    subPanelHolds = true;
            }
            ImGui::End();
            if (!open) subStyleOpen = false;
        }
        if (audioPanelOpen) {
            if (fxEdit < 0 || fxEdit >= (int)clips.size()) audioPanelOpen = false;
            else {
                PlacePanel(0.0f);
                bool open = true;
                if (ClickOutsidePanels()) open = false;
                if (ImGui::Begin("Audio Effects", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                    Clip& c = clips[fxEdit];
                    ImGui::TextUnformatted("Audio Effects");
                    ImGui::Separator();
                    if (ImGui::Checkbox("EQ", &c.eqOn)) c.fx.clear();
                    const char* names[8] = {"60", "150", "400", "1k", "2.5k", "6k", "12k", "16k"};
                    for (int b = 0; b < 8; b++) {
                        ImGui::PushID(b);
                        ImGui::BeginGroup();
                        ImGui::VSliderFloat("##g", ImVec2(36, 140), &c.eqGain[b], -12.0f, 12.0f, "%.1f");
                        ImGui::TextUnformatted(names[b]);
                        ImGui::EndGroup();
                        if (b != 7) ImGui::SameLine();
                        ImGui::PopID();
                    }
                    ImGui::TextUnformatted("dB, low to high");
                    ImGui::Separator();
                    if (ImGui::Checkbox("Reverb", &c.reverbOn)) c.fx.clear();
                    ImGui::SliderFloat("Time", &c.reverbTime, 0.2f, 5.0f, "%.2f s");
                    if (ImGui::Checkbox("Delay", &c.delayOn)) c.fx.clear();
                    ImGui::SliderFloat("Tempo", &c.delayTempo, 40.0f, 240.0f, "%.0f BPM");
                    RunLook("audio",
                        [&](const std::string& p) { return WriteAudioLook(p, c); },
                        [&](const std::string& p) { return ReadAudioLook(p, c); });
                    ImVec2 w0 = ImGui::GetWindowPos();
                    audioPanelRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                }
                ImGui::End();
                if (!open) audioPanelOpen = false;
            }
        }
        colorHolds = false;
        if (colorOpen) {
            if (colorClip < 0 || colorClip >= (int)clips.size()) colorOpen = false;
            else {
                PlacePanel(0.0f);
                bool open = true;
                if (ClickOutsidePanels()) open = false;
                if (ImGui::Begin("Clip color", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                    Clip& c = clips[colorClip];
                    ImGui::TextUnformatted("Clip color");
                    ImGui::Separator();
                    DrawGradeSliders(c.gradeLight, c.gradeContrast, c.gradeWarm, c.gradeTint, c.gradeSat, c.gradeShadow, c.gradeGrain, c.gradeSharp);
                    RunLook("grade",
                        [&](const std::string& p) { return WriteGradeLook(p, ClipGradeOf(c)); },
                        [&](const std::string& p) {
                            BusSegment g = ClipGradeOf(c);
                            if (!ReadGradeLook(p, g)) return false;
                            StoreClipGrade(c, g);
                            return true;
                        });
                    if (ImGui::Button("Reset")) {
                        c.gradeLight = c.gradeContrast = c.gradeWarm = c.gradeTint = 0;
                        c.gradeSat = c.gradeShadow = c.gradeGrain = c.gradeSharp = 0;
                    }
                    ImVec2 w0 = ImGui::GetWindowPos();
                    colorRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                    colorHolds = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                        || (ImGui::IsWindowFocused() && ImGui::IsAnyItemActive());
                }
                ImGui::End();
                if (!open) colorOpen = false;
            }
        }
        ovPanelHolds = false;
        if (ovPanelOpen) {
            if (ovPanel < 0 || ovPanel >= (int)gOverlays.size()) ovPanelOpen = false;
            else {
                PlaceAtCursor();
                bool open = true;
                if (ClickOutsidePanels()) open = false;
                if (ImGui::Begin("Overlay", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                    OverlayFx& o = gOverlays[ovPanel];
                    ImGui::TextUnformatted(OverlayKindName(o.kind));
                    ImGui::Separator();
                    FxSlider(o, "X", &o.x, 0, -1.0f, 1.0f, "%.2f");
                    FxSlider(o, "Y", &o.y, 1, -1.0f, 1.0f, "%.2f");
                    FxSlider(o, "Size", &o.scale, 2, 0.15f, 4.0f, "%.2f");
                    if (o.kind == 26 || o.kind == 27) {
                        FxSlider(o, "Direction", &o.rot, 3, -180.0f, 180.0f, "%.1f");
                        FxSlider(o, "Strength", &o.amount, 4, 0.0f, 1.0f, "%.2f");
                    } else if (o.kind == 28) {
                        FxSlider(o, "Strength", &o.amount, 4, 0.0f, 1.0f, "%.2f");
                    } else {
                        FxSlider(o, "Rotate", &o.rot, 3, -180.0f, 180.0f, "%.1f");
                        FxSlider(o, "Amount", &o.amount, 4, 0.0f, 1.0f, "%.2f");
                    }
                    FxSlider(o, "Opacity", &o.opacity, 5, 0.0f, 1.0f, "%.2f");
                    if (o.kind == 26) {
                        SliderTyped("Shutter", &o.knob[0], 0.05f, 1.0f, "%.2f");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long the trail is");
                        SliderTyped("Falloff", &o.knob[1], 0.0f, 1.0f, "%.2f");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Soft edge of the trail");
                    }
                    if (o.kind == 27) DrawBlurGraph(o.knob);
                    if (o.kind == 28) {
                        SliderTyped("Radius", &o.knob[0], 0.05f, 1.0f, "%.2f");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How wide the sharpen reaches");
                    }
                    if (o.kind == 1) {
                        int cw = o.dir < 0 ? 1 : 0;
                        int prev = o.dir;
                        if (ImGui::Combo("Direction", &cw, "Clockwise\0Counter-clockwise\0")) {
                            o.dir = cw == 0 ? 1 : -1;
                            if (o.rec && isPlaying) OverlayWrite(o, 6, (float)prev, (float)o.dir, currentTime);
                        }
                    }
                    if (o.kind == 11) {
                        int axis = o.dir < 0 ? 1 : 0;
                        int prev = o.dir;
                        if (ImGui::Combo("Axis", &axis, "Horizontal\0Vertical\0")) {
                            o.dir = axis == 0 ? 1 : -1;
                            if (o.rec && isPlaying) OverlayWrite(o, 6, (float)prev, (float)o.dir, currentTime);
                        }
                    }
                    ImGui::TextUnformatted("Drag in the preview. Corner scales, top handle rotates.");
                    {
                        int recorded = 0;
                        for (int k = 0; k < 7; k++) recorded += (int)o.anim[k].size();
                        if (recorded > 0) ImGui::TextDisabled("%d recorded keys", recorded);
                    }
                    bool recLit = o.rec;
                    if (recLit) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.16f, 0.14f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.84f, 0.22f, 0.18f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.10f, 0.10f, 1.0f));
                    }
                    if (ImGui::Button("REC")) o.rec = !o.rec;
                    if (recLit) ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(o.rec ? "Recording while Play is on" : "Record slider changes while Play is on");
                    ImGui::SameLine();
                    if (ImGui::Button("Reset")) OverlayResetAnim(o);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear every recorded change");
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) {
                        gOverlays.erase(gOverlays.begin() + ovPanel);
                        gOverlaySel = -1;
                        open = false;
                    }
                    RunLook("overlay",
                        [&](const std::string& p) { return WriteFxLook(p, o); },
                        [&](const std::string& p) { return ReadFxLook(p, o); });
                    ImVec2 w0 = ImGui::GetWindowPos();
                    ovPanelRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                    ovPanelHolds = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                        || (ImGui::IsWindowFocused() && ImGui::IsAnyItemActive());
                }
                ImGui::End();
                if (!open) ovPanelOpen = false;
            }
        }
        if (floatMediaOpen) {
            if (floatMedia < 0 || floatMedia >= (int)media.size()) floatMediaOpen = false;
            else {
                PlacePanel(0.0f);
                bool open = true;
                if (ImGui::Begin("Library clip", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                    MediaItem& m = media[floatMedia];
                    ImGui::TextUnformatted(m.name.c_str());
                    ImGui::Text("%.1fs   %s", m.duration, m.type.c_str());
                    ImVec2 box(440.0f, 248.0f);
                    ImGui::InvisibleButton("libview", box);
                    ImDrawList* fdl = ImGui::GetWindowDrawList();
                    ImVec2 a = ImGui::GetItemRectMin();
                    ImVec2 b = ImGui::GetItemRectMax();
                    fdl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
                    GLuint tex = m.thumb;
                    int tw = m.thumbW, th = m.thumbH;
#ifdef HAVE_FFMPEG
                    if (m.type != "audio") {
                        if (!playerLib) playerLib = new VideoPlayer();
                        if (libPlayId != floatMedia) {
                            libPlayId = floatMedia;
                            libClock = 0;
                            if (playerLib->Open(m.path)) playerLib->GrabFrame();
                        }
                        if (m.type == "video" && playerLib->hasFrame) {
                            libClock += ImGui::GetIO().DeltaTime;
                            int guard = 0;
                            while (guard < 3 && playerLib->lastPts >= 0.0 && playerLib->lastPts + 0.0008 < libClock) {
                                if (!playerLib->GrabFrame()) {
                                    playerLib->Seek(0);
                                    libClock = 0;
                                    playerLib->GrabFrame();
                                    break;
                                }
                                guard++;
                            }
                        }
                        if (playerLib->hasFrame && playerLib->texture) {
                            tex = playerLib->texture;
                            tw = playerLib->width;
                            th = playerLib->height;
                        }
                    }
#endif
                    if (tex && tw > 0 && th > 0) DrawThumb(fdl, a, b, a, b, tex, tw, th);
                    else fdl->AddText(ImVec2(a.x + 12, a.y + 12), IM_COL32(180, 180, 180, 255), m.type == "audio" ? "Audio" : "No picture");
                    ImVec2 w0 = ImGui::GetWindowPos();
                    floatMediaRect = ImVec4(w0.x, w0.y, w0.x + ImGui::GetWindowSize().x, w0.y + ImGui::GetWindowSize().y);
                }
                ImGui::End();
                if (!open) { floatMediaOpen = false; libPlayId = -1; }
            }
        }
        RestoreUiShield();
        if (fontAtlas.empty()) {
            ImGuiIO& fio = ImGui::GetIO();
            unsigned char* px = fio.Fonts->TexPixelsAlpha8;
            if (px && fio.Fonts->TexWidth > 1 && fio.Fonts->TexHeight > 1) {
                fontAtlasW = fio.Fonts->TexWidth;
                fontAtlasH = fio.Fonts->TexHeight;
                fontAtlas.assign(px, px + (size_t)fontAtlasW * fontAtlasH);
            }
        }
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.05f, 0.05f, 0.05f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifdef HAVE_FFMPEG
        ImVec2 view0 = previewFrameValid ? previewFrameMin : previewMin;
        ImVec2 view1 = previewFrameValid ? previewFrameMax : previewMax;
        fitValid = false;
        recordFit = true;
        activeGrade = nullptr;
        previewGradeOn = false;
        previewGrade = BusSegment();
        if (topClip >= 0 && topClip < (int)clips.size()) previewGrade = ClipGradeOf(clips[topClip]);
        if (const BusSegment* bs = BusAt(currentTime)) AddGrade(previewGrade, *bs);
        previewGradeOn = BusActive(previewGrade);
        activeGrade = previewGradeOn ? &previewGrade : nullptr;
        for (int bi = 0; bi < bgShowN; bi++) DrawBackgroundClip(bgShow[bi], view0, view1);
        if (fxOther >= 0 && player && playerB && player->hasFrame && playerB->hasFrame && player->texture && playerB->texture) {
            int drawA = -1, drawB = -1;
            float prog = 0;
            std::string id;
            if (TransitionForClip(topClip, currentTime, drawA, drawB, prog, id)) {
                FxLook look;
                FxEvaluate(id.c_str(), prog, look);
                auto take = [&](int clip, GLuint& tex, int& w, int& h) -> bool {
                    if (clip >= 0 && playingClip == clip && player && player->hasFrame && player->texture) {
                        tex = player->texture; w = player->width; h = player->height; return true;
                    }
                    if (clip >= 0 && playingClipB == clip && playerB && playerB->hasFrame && playerB->texture) {
                        tex = playerB->texture; w = playerB->width; h = playerB->height; return true;
                    }
                    return false;
                };
                GLuint texA = 0, texB = 0;
                int aw = 0, ah = 0, bw = 0, bh = 0;
                bool gotA = take(drawA, texA, aw, ah);
                bool gotB = take(drawB, texB, bw, bh);
                if (!gotA && player && player->hasFrame && player->texture && playingClip != drawB) {
                    texA = player->texture; aw = player->width; ah = player->height; gotA = true;
                }
                if (!gotB && playerB && playerB->hasFrame && playerB->texture && playingClipB != drawA) {
                    texB = playerB->texture; bw = playerB->width; bh = playerB->height; gotB = playerB->texture != 0;
                }
                if (gotA) {
                    BlitFxPair(view0, view1, texA, aw, ah, texB, bw, bh, look,
                               (drawA >= 0 && drawA < (int)clips.size()) ? &clips[drawA] : nullptr,
                               (drawB >= 0 && drawB < (int)clips.size()) ? &clips[drawB] : nullptr);
                }
            } else if (player && player->hasFrame && player->texture) {
                const Clip* pc = nullptr;
                int pic = (playingClip >= 0 && playingClip < (int)clips.size()) ? playingClip : topClip;
                if (pic >= 0 && pic < (int)clips.size()) pc = &clips[pic];
                float cover = pc ? OpacityAt(*pc, currentTime - pc->startTime) : 1.0f;
                BlitVideo(view0, view1, player->texture, player->width, player->height, view0, view1,
                          pc ? pc->gsX : 0, pc ? pc->gsY : 0, pc ? pc->gsScale : 1, pc ? pc->gsAspect : 1, pc ? pc->gsRot : 0, false, cover);
            }
        } else if (player && player->hasFrame && player->texture) {
            const Clip* pc = nullptr;
            int pic = (playingClip >= 0 && playingClip < (int)clips.size()) ? playingClip : topClip;
            if (pic >= 0 && pic < (int)clips.size()) pc = &clips[pic];
            float cover = pc ? OpacityAt(*pc, currentTime - pc->startTime) : 1.0f;
            BlitVideo(view0, view1, player->texture, player->width, player->height, view0, view1,
                      pc ? pc->gsX : 0, pc ? pc->gsY : 0, pc ? pc->gsScale : 1, pc ? pc->gsAspect : 1, pc ? pc->gsRot : 0, false, cover);
        }
        activeGrade = nullptr;
        recordFit = false;
        if (gsDrawClip >= 0) {
            GLuint tex = 0;
            int tw = 0, th = 0;
            if (playerGS && playerGS->hasFrame && playerGS->texture) {
                tex = playerGS->texture;
                tw = playerGS->width;
                th = playerGS->height;
            } else if (gsDrawClip < (int)clips.size()) {
                int mid = clips[gsDrawClip].mediaId;
                if (mid >= 0 && mid < (int)media.size() && media[mid].thumb) {
                    tex = media[mid].thumb;
                    tw = media[mid].thumbW;
                    th = media[mid].thumbH;
                }
            }
            if (tex && tw > 0 && th > 0) {
                ImVec2 a = fitValid ? fitA : view0;
                ImVec2 b = fitValid ? fitB : view1;
                const Clip& gc = clips[gsDrawClip];
                float gsCover = gc.gsOpacity * OpacityAt(gc, currentTime - gc.startTime);
                BlitKeyed(a, b, tex, gc.keySim, gc.keySoft, gc.keySpill, gc.gsX, gc.gsY, gc.gsScale, gsCover, gc.gsAspect, gc.gsRot);
                if (!fitValid) { fitA = a; fitB = b; fitValid = true; }
            }
        }
        for (const auto& tb : thumbBlits)
            BlitVideo(tb.a, tb.b, tb.tex, tb.w, tb.h, tb.c0, tb.c1);
        if (pglUseProgram) pglUseProgram(0);
        OverlayDrawGL(view0.x, view0.y, view1.x, view1.y, currentTime, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        if (pglUseProgram) pglUseProgram(0);
        if (previewPlate >= 0 && previewPlate < (int)clips.size()) {
            int mid = clips[previewPlate].mediaId;
            if (mid >= 0 && mid < (int)media.size() && media[mid].thumb) {
                const Clip& pc = clips[previewPlate];
                BlitVideo(view0, view1, media[mid].thumb, media[mid].thumbW, media[mid].thumbH, view0, view1,
                          pc.gsX, pc.gsY, pc.gsScale, pc.gsAspect, pc.gsRot, true, OpacityAt(pc, currentTime - pc.startTime));
            }
        }
        DrawPreviewSubs(view0, view1);
        if (previewFrameValid) {
            if (pglUseProgram) pglUseProgram(0);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glColor3f(0.f, 0.f, 0.f);
            auto fill = [](float x0, float y0, float x1, float y1) {
                if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
                glBegin(GL_QUADS);
                glVertex2f(x0, y0);
                glVertex2f(x1, y0);
                glVertex2f(x1, y1);
                glVertex2f(x0, y1);
                glEnd();
            };
            fill(previewMin.x, previewMin.y, previewFrameMin.x, previewMax.y);
            fill(previewFrameMax.x, previewMin.y, previewMax.x, previewMax.y);
            fill(std::max(previewMin.x, previewFrameMin.x), previewMin.y, std::min(previewMax.x, previewFrameMax.x), previewFrameMin.y);
            fill(std::max(previewMin.x, previewFrameMin.x), previewFrameMax.y, std::min(previewMax.x, previewFrameMax.x), previewMax.y);
            glColor3f(0.86f, 0.86f, 0.86f);
            glLineWidth(2.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f(previewFrameMin.x, previewFrameMin.y);
            glVertex2f(previewFrameMax.x, previewFrameMin.y);
            glVertex2f(previewFrameMax.x, previewFrameMax.y);
            glVertex2f(previewFrameMin.x, previewFrameMax.y);
            glEnd();
            glLineWidth(1.0f);
            glColor3f(1, 1, 1);
            glPopMatrix();
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
        }
        DrawXformChrome(topClip, previewPlate);
#endif
        RenderFloatingOnTop();
        if (undoRequest > 0) PerformUndo();
        else UndoTick();
        SDL_GL_SwapWindow(window);
    }

#ifdef HAVE_FFMPEG
    gRam.Stop();
    if (gExport.active) gExport.Cancel();
    if (player) { delete player; player = nullptr; }
    if (playerB) { delete playerB; playerB = nullptr; }
    if (playerUnder) { delete playerUnder; playerUnder = nullptr; }
    if (playerNext) { delete playerNext; playerNext = nullptr; }
    if (playerGS) { delete playerGS; playerGS = nullptr; }
    if (playerLib) { delete playerLib; playerLib = nullptr; }
    for (auto& a : audioSrcs) CloseAudioSrc(a);
    audioSrcs.clear();
    if (audioDev) SDL_CloseAudioDevice(audioDev);
#endif
#ifdef _WIN32
    CoUninitialize();
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
#ifdef _MSC_VER
#pragma optimize("", on)
#endif
