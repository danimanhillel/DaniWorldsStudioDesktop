#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_syswm.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}
#endif

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
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

struct Clip {
    int mediaId = -1;
    float startTime = 0.0f;
    float duration = 5.0f;
    float sourceOffset = 0.0f;
    float speed = 1.0f;
    bool fadeIn = false;
    bool fadeOut = false;
    int track = 0;
    std::vector<VolPoint> volume;
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

std::vector<MediaItem> media;
std::vector<Clip> clips;
std::vector<ThumbBlit> thumbBlits;
float currentTime = 0.0f;
float pixelsPerSecond = 50.0f;
bool isPlaying = false;
bool snapEnabled = true;
int selectedClip = -1;
int activeTool = 0;
std::string previewStatus = "Ready";
bool draggingPlayhead = false;
int draggingClip = -1;
float dragOffsetX = 0.0f;
ImVec2 previewMin(0, 0), previewMax(0, 0);
float shownLocal = -1000.0f;
int trimmingClip = -1;
int trimSide = 0;
float trimAnchorX = 0;
float trimOrigStart = 0, trimOrigDur = 0, trimOrigOffset = 0;
int contextClip = -1;
bool openClipMenu = false;
int dragVolClip = -1;
int dragVolPt = -1;
float frameDebt = 0.0f;
float timeOriginX = 0.0f;
int exAspect = 0;
int exRes = 1;
int exFpsIdx = 1;
int mediaMenuIndex = -1;
bool openMediaMenu = false;

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
    double nextTime = -1;
    std::vector<int16_t> fifo;
};

std::vector<AudioSrc> audioSrcs;
SDL_AudioDeviceID audioDev = 0;

class VideoPlayer {
public:
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    uint8_t* rgbBuffer = nullptr;
    int videoStream = -1;
    int width = 0, height = 0;
    GLuint texture = 0;
    bool hasFrame = false;
    double lastPts = -1;
    std::string lastError;

    ~VideoPlayer() { Close(); }

    bool Open(const std::string& path) {
        Close();
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
        codecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codecCtx, fmtCtx->streams[videoStream]->codecpar);
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            lastError = "Cannot open codec"; return false;
        }
        frame = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        lastError.clear();
        return true;
    }

    bool EnsureTexture() {
        if (texture && swsCtx) return true;
        if (!frame || frame->width <= 0 || frame->height <= 0) return false;
        width = frame->width;
        height = frame->height;
        AVPixelFormat srcFmt = (AVPixelFormat)frame->format;
        if (srcFmt == AV_PIX_FMT_NONE) { lastError = "Bad pixel format"; return false; }
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
        rgbBuffer = (uint8_t*)av_malloc(numBytes);
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer, AV_PIX_FMT_RGBA, width, height, 1);
        swsCtx = sws_getContext(width, height, srcFmt, width, height, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) { lastError = "sws failed"; return false; }
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        return true;
    }

    void Seek(float seconds) {
        if (!fmtCtx || videoStream < 0 || !codecCtx) return;
        int64_t ts = (int64_t)(seconds / av_q2d(fmtCtx->streams[videoStream]->time_base));
        av_seek_frame(fmtCtx, videoStream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx);
        lastPts = -1;
    }

    bool GrabFrame() {
        if (!fmtCtx || !codecCtx) return false;
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(fmtCtx, packet) >= 0) {
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
                        if (!EnsureTexture()) { av_packet_free(&packet); return false; }
                        sws_scale(swsCtx, frame->data, frame->linesize, 0, height, rgbFrame->data, rgbFrame->linesize);
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
        if (fmtCtx) { avformat_close_input(&fmtCtx); }
        hasFrame = false;
        width = 0;
        height = 0;
        lastPts = -1;
    }
};

VideoPlayer* player = nullptr;
int playingMediaId = -1;

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
    if (a.nextTime < 0 || std::fabs(a.nextTime - mediaTime) > 0.25) {
        int64_t ts = (int64_t)(mediaTime / av_q2d(a.fmt->streams[a.stream]->time_base));
        av_seek_frame(a.fmt, a.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(a.codec);
        a.fifo.clear();
        a.nextTime = -1;
    }
    int spins = 0;
    while ((int)a.fifo.size() < count * 2 && spins < 48) {
        spins++;
        if (av_read_frame(a.fmt, a.packet) < 0) break;
        if (a.packet->stream_index == a.stream && avcodec_send_packet(a.codec, a.packet) == 0) {
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
                    if (drop >= got) { a.nextTime = -1; continue; }
                    if (drop > 0) start = drop;
                    a.nextTime = mediaTime;
                }
                a.fifo.insert(a.fifo.end(), tmp.begin() + start * 2, tmp.begin() + got * 2);
            }
        }
        av_packet_unref(a.packet);
    }
    int have = (int)a.fifo.size() / 2;
    int n = std::min(count, have);
    if (n > 0) {
        memcpy(dst, a.fifo.data(), (size_t)n * 2 * sizeof(int16_t));
        a.fifo.erase(a.fifo.begin(), a.fifo.begin() + n * 2);
        if (a.nextTime < 0) a.nextTime = mediaTime;
        a.nextTime += n / 48000.0;
    }
    if (n < count) memset(dst + n * 2, 0, (size_t)(count - n) * 2 * sizeof(int16_t));
}

void ResetAudioSync() {
    for (auto& a : audioSrcs) a.nextTime = -1;
    if (audioDev) SDL_ClearQueuedAudio(audioDev);
}

void SetPlaying(bool play) {
    if (play == isPlaying) return;
    isPlaying = play;
    ResetAudioSync();
    if (audioDev) SDL_PauseAudioDevice(audioDev, play ? 0 : 1);
}

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
    if (c.fadeIn) g *= std::min(1.0f, localTime / fadeDur);
    if (c.fadeOut) g *= std::min(1.0f, (c.duration - localTime) / fadeDur);
    if (g < 0) g = 0;
    if (g > 1) g = 1;
    return g;
}

void MixTimeline(double timelineTime, int sampleCount, int16_t* dst) {
    if (!dst || sampleCount <= 0) return;
    std::vector<int32_t> mix((size_t)sampleCount * 2, 0);
    for (int ci = 0; ci < (int)clips.size(); ci++) {
        const Clip& c = clips[ci];
        if (c.mediaId < 0 || c.mediaId >= (int)media.size()) continue;
        if (media[c.mediaId].type == "image") continue;
        if (timelineTime < c.startTime || timelineTime >= c.startTime + c.duration) continue;
        int idx = EnsureAudio(c.mediaId);
        if (audioSrcs[idx].noAudio || audioSrcs[idx].failed) continue;
        float speed = c.speed < 0.05f ? 1.0f : c.speed;
        double mediaT = c.sourceOffset + (timelineTime - c.startTime) * speed;
        int srcN = std::max(1, (int)std::lround(sampleCount * speed));
        std::vector<int16_t> src((size_t)srcN * 2, 0);
        PullSamples(audioSrcs[idx], mediaT, srcN, src.data());
        float local = (float)(timelineTime - c.startTime);
        float blockDur = sampleCount / 48000.0f;
        float g0 = GainAt(c, local);
        float g1 = GainAt(c, local + blockDur);
        for (int i = 0; i < sampleCount; i++) {
            float pos = (sampleCount <= 1) ? 0.0f : (float)i * (float)(srcN - 1) / (float)(sampleCount - 1);
            int i0 = (int)pos;
            int i1 = std::min(srcN - 1, i0 + 1);
            float fr = pos - (float)i0;
            float g = g0 + (g1 - g0) * ((float)i / (float)sampleCount);
            for (int ch = 0; ch < 2; ch++) {
                float s = src[(size_t)i0 * 2 + ch] * (1.0f - fr) + src[(size_t)i1 * 2 + ch] * fr;
                mix[i * 2 + ch] += (int32_t)(s * g);
            }
        }
    }
    for (int i = 0; i < sampleCount * 2; i++) {
        int v = mix[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
    }
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

    void CloseAll();
    bool Begin(const std::string& outPath, int width, int height, int framesPerSec);
    void Step();
    void Cancel();
    void Fail(const char* msg);
    void Finish(bool ok);
    bool WriteEncoded(AVCodecContext* ctx, AVStream* st);
    bool DrainAudio(bool padFinal);
    int OpenDec(int mediaId);
    bool ReadOne(ExpDec& d);
    bool EnsureFrame(int mediaId, double mediaTime);
    void BlitToCanvas(int idx);
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
    const AVCodec* codec = (d.stream >= 0) ? avcodec_find_decoder(d.fmt->streams[d.stream]->codecpar->codec_id) : nullptr;
    d.ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (d.stream < 0 || !d.ctx ||
        avcodec_parameters_to_context(d.ctx, d.fmt->streams[d.stream]->codecpar) < 0 ||
        avcodec_open2(d.ctx, codec, nullptr) < 0) {
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
        if (d.frame->width <= 0 || d.frame->height <= 0) continue;
        AVPixelFormat srcFmt = (AVPixelFormat)d.frame->format;
        if (srcFmt == AV_PIX_FMT_NONE) continue;
        if (!d.sws || d.rw != d.frame->width || d.rh != d.frame->height) {
            if (d.sws) sws_freeContext(d.sws);
            d.rw = d.frame->width;
            d.rh = d.frame->height;
            d.sws = sws_getContext(d.rw, d.rh, srcFmt, d.rw, d.rh, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
            d.rgba.assign((size_t)d.rw * (size_t)d.rh * 4, 0);
            if (d.fit) { sws_freeContext(d.fit); d.fit = nullptr; d.fitW = d.fitH = 0; }
        }
        if (!d.sws || d.rgba.empty()) return false;
        uint8_t* dstData[1] = { d.rgba.data() };
        int dstLines[1] = { d.rw * 4 };
        sws_scale(d.sws, d.frame->data, d.frame->linesize, 0, d.rh, dstData, dstLines);
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
    ExpDec& d = decs[idx];
    if (d.failed || !d.fmt) return false;
    bool isImage = media[mediaId].type == "image";
    if (isImage && d.imageDone) return d.rw > 0;
    if (mediaTime < 0) mediaTime = 0;
    bool needSeek = !isImage && (d.lastPts < 0 || mediaTime + 0.08 < d.lastPts || mediaTime - d.lastPts > 0.50);
    if (needSeek) {
        int64_t ts = (int64_t)(mediaTime / av_q2d(d.fmt->streams[d.stream]->time_base));
        av_seek_frame(d.fmt, d.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d.ctx);
        d.lastPts = -1;
    }
    int guard = 0;
    while (guard++ < 80) {
        if (d.rw > 0 && d.lastPts >= 0 && d.lastPts + 0.0001 >= mediaTime) {
            if (isImage) d.imageDone = true;
            return true;
        }
        if (!ReadOne(d)) break;
        if (isImage) { d.imageDone = true; return d.rw > 0; }
    }
    return d.rw > 0;
}

void ExportSession::BlitToCanvas(int idx) {
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
    for (int y = 0; y < fh; y++) {
        size_t dst = ((size_t)(oy + y) * (size_t)w + (size_t)ox) * 4;
        if (dst + (size_t)fw * 4 > canvas.size()) break;
        memcpy(canvas.data() + dst, d.fitted.data() + (size_t)y * (size_t)fw * 4, (size_t)fw * 4);
    }
}

bool ExportSession::Begin(const std::string& outPath, int width, int height, int framesPerSec) {
    CloseAll();
    path = outPath;
    w = width & ~1;
    h = height & ~1;
    fps = framesPerSec;
    if (w < 2 || h < 2 || fps < 1) { status = "Bad export settings"; previewStatus = status; return false; }
    timelineEnd = 0;
    for (const auto& c : clips)
        timelineEnd = std::max(timelineEnd, (double)c.startTime + (double)c.duration);
    if (timelineEnd < 0.05 || clips.empty()) { status = "Nothing to export"; previewStatus = status; return false; }
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
    const AVCodec* vcodec = avcodec_find_encoder_by_name("libx264");
    if (!vcodec) vcodec = avcodec_find_encoder_by_name("h264_mf");
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) { status = "H.264 encoder not found"; previewStatus = status; CloseAll(); return false; }
    vctx = avcodec_alloc_context3(vcodec);
    vctx->width = w;
    vctx->height = h;
    vctx->time_base = AVRational{1, fps};
    vctx->framerate = AVRational{fps, 1};
    vctx->gop_size = fps;
    vctx->max_b_frames = 0;
    vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    int64_t br = (int64_t)w * (int64_t)h;
    if (br < 4000000) br = 4000000;
    if (br > 80000000) br = 80000000;
    vctx->bit_rate = br;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (vcodec->name && strcmp(vcodec->name, "libx264") == 0) {
        av_opt_set(vctx->priv_data, "preset", (w >= 3840 ? "ultrafast" : "veryfast"), 0);
        av_opt_set(vctx->priv_data, "crf", "20", 0);
    }
    if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
        status = "Cannot open video encoder"; previewStatus = status; CloseAll(); return false;
    }
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
    char buf[160];
    snprintf(buf, sizeof(buf), "Exporting %dx%d %d fps%s", w, h, fps, videoOnly ? " (no audio encoder)" : "");
    status = buf;
    previewStatus = status;
    return true;
}

void ExportSession::Step() {
    if (!active) return;
    if (frameIndex >= total) { Finish(true); return; }
    double t = (double)frameIndex / (double)fps;
    std::fill(canvas.begin(), canvas.end(), 0);
    int top = -1, bestTrack = 99;
    for (int i = 0; i < (int)clips.size(); i++) {
        if (clips[i].track >= 3) continue;
        if (t < clips[i].startTime || t >= clips[i].startTime + clips[i].duration) continue;
        if (clips[i].track <= bestTrack) { bestTrack = clips[i].track; top = i; }
    }
    if (top >= 0) {
        const Clip& c = clips[top];
        float speed = std::max(0.05f, c.speed);
        double mediaT = c.sourceOffset + (t - c.startTime) * (double)speed;
        int idx = OpenDec(c.mediaId);
        if (EnsureFrame(c.mediaId, mediaT)) BlitToCanvas(idx);
    }
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
    char buf[160];
    snprintf(buf, sizeof(buf), "Export %d%%  (%d/%d)  %dx%d", pct, frameIndex, total, w, h);
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
    std::string saved = path;
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

void SyncPreview(int topClip, float dt) {
    bool playing = isPlaying && !draggingPlayhead;
    if (topClip < 0 || !player || !player->fmtCtx) {
        if (playing) currentTime += dt;
        PumpAudio();
        return;
    }
    int mid = clips[topClip].mediaId;
    if (mid >= 0 && mid < (int)media.size() && media[mid].type == "image") {
        if (playing) currentTime += dt;
        if (!player->hasFrame) {
            player->Seek(0);
            player->GrabFrame();
        }
        previewStatus = "Image";
        PumpAudio();
        return;
    }
    if (playing) currentTime += dt;
    float local = currentTime - clips[topClip].startTime;
    if (local < 0) local = 0;
    float speed = std::max(0.05f, clips[topClip].speed);
    float mediaT = clips[topClip].sourceOffset + local * speed;
    if (!playing && player->hasFrame && std::fabs(mediaT - shownLocal) <= 0.04f) {
        PumpAudio();
        return;
    }
    bool needSeek = player->lastPts < 0.0 || (mediaT + 0.12f < (float)player->lastPts) || ((float)player->lastPts + 0.45f < mediaT);
    if (needSeek || !playing) {
        player->Seek(std::max(0.0f, mediaT));
        player->GrabFrame();
    }
    int guard = 0;
    int limit = playing ? 32 : 48;
    while (player->lastPts >= 0.0 && (float)player->lastPts + 0.0005f < mediaT && guard < limit) {
        if (!player->GrabFrame()) break;
        guard++;
    }
    shownLocal = mediaT;
    if (player->hasFrame) {
        char buf[80];
        snprintf(buf, sizeof(buf), "FFmpeg %dx%d  x%.2g", player->width, player->height, speed);
        previewStatus = buf;
    }
    PumpAudio();
}


#endif

void BlitVideo(ImVec2 p0, ImVec2 p1, GLuint tex, int vw, int vh, ImVec2 clip0, ImVec2 clip1) {
    if (!tex || vw <= 0 || vh <= 0) return;
    float areaW = p1.x - p0.x;
    float areaH = p1.y - p0.y;
    if (areaW < 4 || areaH < 4) return;
    float aspect = (float)vw / (float)vh;
    float areaAspect = areaW / areaH;
    ImVec2 a = p0, b = p1;
    if (aspect > areaAspect) {
        float h = areaW / aspect;
        float off = (areaH - h) * 0.5f;
        a.y += off; b.y -= off;
    } else {
        float w = areaH * aspect;
        float off = (areaW - w) * 0.5f;
        a.x += off; b.x -= off;
    }
    ImGuiIO& io = ImGui::GetIO();
    float sx0 = std::max(a.x, std::max(0.0f, clip0.x));
    float sy0 = std::max(a.y, std::max(0.0f, clip0.y));
    float sx1 = std::min(b.x, std::min(io.DisplaySize.x, clip1.x));
    float sy1 = std::min(b.y, std::min(io.DisplaySize.y, clip1.y));
    if (sx1 - sx0 < 1.0f || sy1 - sy0 < 1.0f) return;

    if (pglUseProgram) pglUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)sx0, (int)(io.DisplaySize.y - sy1), std::max(1, (int)(sx1 - sx0)), std::max(1, (int)(sy1 - sy0)));
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor3f(1.f, 1.f, 1.f);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, io.DisplaySize.x, io.DisplaySize.y, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(a.x, a.y);
    glTexCoord2f(1, 0); glVertex2f(b.x, a.y);
    glTexCoord2f(1, 1); glVertex2f(b.x, b.y);
    glTexCoord2f(0, 1); glVertex2f(a.x, b.y);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
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
        float s = clips[i].startTime;
        float e = s + clips[i].duration;
        test(e);
        test(s);
        test(s - dur);
        test(e - dur);
    }
    return best;
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
    c.duration = leftDur;
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
    clips.insert(clips.begin() + selectedClip + 1, right);
    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
    ResetAudioSync();
#endif
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
    if (mid == playingMediaId && player && player->fmtCtx) return;
    if (!player) player = new VideoPlayer();
    previewStatus = "Opening...";
    shownLocal = -1000.0f;
    if (player->Open(media[mid].path)) {
        playingMediaId = mid;
        float localTime = currentTime - clips[clipIdx].startTime;
        if (localTime < 0) localTime = 0;
        float mediaT = clips[clipIdx].sourceOffset + localTime * std::max(0.05f, clips[clipIdx].speed);
        player->Seek(mediaT);
        if (player->GrabFrame()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "FFmpeg %dx%d", player->width, player->height);
            previewStatus = buf;
            shownLocal = mediaT;
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

void ApplyTrim(int idx, float mouseX) {
    if (idx < 0 || idx >= (int)clips.size()) return;
    Clip& c = clips[idx];
    float srcDur = (c.mediaId >= 0 && c.mediaId < (int)media.size()) ? media[c.mediaId].duration : (c.duration * c.speed);
    float speed = c.speed < 0.05f ? 1.0f : c.speed;
    float dt = (mouseX - trimAnchorX) / pixelsPerSecond;
    if (trimSide == 1) {
        float nd = trimOrigDur + dt;
        float maxD = (srcDur - trimOrigOffset) / speed;
        if (nd < 0.1f) nd = 0.1f;
        if (nd > maxD) nd = maxD;
        float end = SnapTime(idx, trimOrigStart + nd);
        nd = end - trimOrigStart;
        if (nd < 0.1f) nd = 0.1f;
        if (nd > maxD) nd = maxD;
        c.duration = nd;
        c.sourceOffset = trimOrigOffset;
        c.startTime = trimOrigStart;
    } else {
        float nstart = trimOrigStart + dt;
        float noff = trimOrigOffset + dt * speed;
        float ndur = trimOrigDur - dt;
        if (nstart < 0) {
            float back = -nstart;
            nstart = 0;
            noff -= back * speed;
            ndur -= back;
        }
        if (noff < 0) {
            float back = -noff / speed;
            noff = 0;
            nstart -= back;
            ndur += back;
            if (nstart < 0) nstart = 0;
        }
        if (ndur < 0.1f) ndur = 0.1f;
        if (noff + ndur * speed > srcDur) ndur = std::max(0.1f, (srcDur - noff) / speed);
        float snapped = SnapTime(idx, nstart);
        float applied = snapped - trimOrigStart;
        nstart = snapped;
        noff = trimOrigOffset + applied * speed;
        ndur = trimOrigDur - applied;
        if (noff < 0) { ndur += (-noff) / speed; noff = 0; }
        if (nstart < 0) nstart = 0;
        if (ndur < 0.1f) ndur = 0.1f;
        if (noff + ndur * speed > srcDur) ndur = std::max(0.1f, (srcDur - noff) / speed);
        c.startTime = nstart;
        c.sourceOffset = noff;
        c.duration = ndur;
    }
}

void ExportDims(int aspect, int res, int& w, int& h) {
    const int heights[] = {720, 1080, 1440, 2160, 4320};
    const int widths169[] = {1280, 1920, 2560, 3840, 7680};
    if (res < 0) res = 0;
    if (res > 4) res = 4;
    if (aspect < 0) aspect = 0;
    if (aspect > 3) aspect = 3;
    int H = heights[res];
    int W = widths169[res];
    if (aspect == 0) { w = W; h = H; }
    else if (aspect == 1) { w = H; h = W; }
    else if (aspect == 2) { w = H; h = H; }
    else { h = H; w = (H * 4 / 3) & ~1; }
    w &= ~1;
    h &= ~1;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
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

void ClearProject() {
    for (auto& m : media) {
        if (m.thumb) { glDeleteTextures(1, &m.thumb); m.thumb = 0; }
    }
    media.clear();
    clips.clear();
    selectedClip = -1;
    draggingClip = -1;
    trimmingClip = -1;
    dragVolClip = -1;
    contextClip = -1;
    mediaMenuIndex = -1;
    currentTime = 0;
    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
    SetPlaying(false);
    for (auto& a : audioSrcs) CloseAudioSrc(a);
    audioSrcs.clear();
    if (player) player->Close();
    playingMediaId = -1;
#else
    isPlaying = false;
#endif
}

void RemoveMedia(int id) {
    if (id < 0 || id >= (int)media.size()) return;
    if (media[id].thumb) {
        glDeleteTextures(1, &media[id].thumb);
        media[id].thumb = 0;
    }
    for (int i = (int)clips.size() - 1; i >= 0; --i) {
        if (clips[i].mediaId == id) clips.erase(clips.begin() + i);
        else if (clips[i].mediaId > id) clips[i].mediaId--;
    }
    media.erase(media.begin() + id);
    selectedClip = -1;
    draggingClip = -1;
    trimmingClip = -1;
    dragVolClip = -1;
    contextClip = -1;
    mediaMenuIndex = -1;
    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
    for (auto& a : audioSrcs) CloseAudioSrc(a);
    audioSrcs.clear();
    if (player) player->Close();
    playingMediaId = -1;
    ResetAudioSync();
#endif
}

float TimelineEnd() {
    float end = 0.0f;
    for (const auto& c : clips)
        end = std::max(end, c.startTime + c.duration);
    return end;
}

void StopIfFinished() {
    if (!isPlaying || draggingPlayhead) return;
    float end = TimelineEnd();
    if (!clips.empty() && currentTime < end - 0.02f) return;
#ifdef HAVE_FFMPEG
    SetPlaying(false);
#else
    isPlaying = false;
#endif
    if (!clips.empty() && currentTime > end) currentTime = end;
    shownLocal = -1000.0f;
}

bool SaveProject(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "DWP1\n");
    fprintf(f, "time %.6f\n", currentTime);
    fprintf(f, "snap %d\n", snapEnabled ? 1 : 0);
    fprintf(f, "pps %.3f\n", pixelsPerSecond);
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
    }
    fclose(f);
    return true;
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
        } else if (strncmp(line, "pps ", 4) == 0) {
            float p = (float)atof(line + 4);
            if (p > 5.0f && p < 400.0f) pixelsPerSecond = p;
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
    shownLocal = -1000.0f;
    return true;
}


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

    SDL_Window* window = SDL_CreateWindow("DaniWorldsStudio FFmpeg 7",
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
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool running = true;
    SDL_Event event;
    int topClip = -1;

    while (running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (event.key.keysym.sym == SDLK_SPACE) {
#ifdef HAVE_FFMPEG
                    if (!gExport.active) SetPlaying(!isPlaying);
#else
                    isPlaying = !isPlaying;
#endif
                }
                if (event.key.keysym.sym == SDLK_c && !event.key.repeat && !io.WantTextInput)
                    SplitSelected();
                if (event.key.keysym.sym == SDLK_DELETE || event.key.keysym.sym == SDLK_BACKSPACE) {
                    if (selectedClip >= 0 && selectedClip < (int)clips.size()) {
                        clips.erase(clips.begin() + selectedClip);
                        selectedClip = -1;
                        draggingClip = -1;
                        trimmingClip = -1;
                        dragVolClip = -1;
                        contextClip = -1;
                    }
                }
            }
        }

        thumbBlits.clear();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        if (ImGui::Button("New")) {
            ClearProject();
            previewStatus = "New project";
        }
        ImGui::SameLine();
        if (ImGui::Button("Import")) ImportFiles(hwnd);
        ImGui::SameLine();
        if (ImGui::Button("Export")) requestExport = true;
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            std::string p = SaveDialog(hwnd, "DaniWorlds Project (*.dwp)\0*.dwp\0", "dwp", "Save Project");
            if (!p.empty()) previewStatus = SaveProject(p) ? "Project saved" : "Save failed";
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            std::string p = OpenDialog(hwnd, "DaniWorlds Project (*.dwp)\0*.dwp\0", "Load Project");
            if (!p.empty()) previewStatus = LoadProject(p) ? "Project loaded" : "Load failed";
        }
        ImGui::SameLine();
        if (ImGui::Button(snapEnabled ? "Snap ON" : "Snap OFF")) snapEnabled = !snapEnabled;
        ImGui::SameLine();
#ifdef HAVE_FFMPEG
        ImGui::Text("| %.2fs | Media: %d | %s | Space=Play/Pause | ESC=Exit",
                    currentTime, (int)media.size(), audioDev ? "Audio" : "No audio device");
#else
        ImGui::Text("| %.2fs | Media: %d | Space=Play/Pause | ESC=Exit", currentTime, (int)media.size());
#endif

        ImGui::Separator();
        float topH = io.DisplaySize.y * 0.38f;

        ImGui::BeginChild("Media", ImVec2(io.DisplaySize.x * 0.26f, topH), true);
        ImGui::Text("Media Library");
        ImGui::Separator();
        ImVec2 mediaClip0 = ImGui::GetWindowPos();
        ImVec2 mediaClip1(mediaClip0.x + ImGui::GetWindowSize().x, mediaClip0.y + ImGui::GetWindowSize().y);
        float gap = 6.0f;
        float cellW = std::max(70.0f, (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f);
        float thumbH = cellW * 9.0f / 16.0f;
        for (int i = 0; i < (int)media.size(); i++) {
            const auto& m = media[i];
            if (i % 3 != 0) ImGui::SameLine(0, gap);
            ImGui::PushID(i);
            ImGui::BeginGroup();
            ImGui::InvisibleButton("##dragitem", ImVec2(cellW, thumbH));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            bool vis = p1.y > mediaClip0.y && p0.y < mediaClip1.y && p1.x > mediaClip0.x && p0.x < mediaClip1.x;
            if (m.thumb && vis)
                thumbBlits.push_back({p0, p1, mediaClip0, mediaClip1, m.thumb, m.thumbW, m.thumbH});
            else if (vis)
                DrawPlaceholder(dl, p0, p1, m);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cellW);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::Text("%.1fs %s", m.duration, m.type.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImVec2 g0 = ImGui::GetItemRectMin();
            ImVec2 g1 = ImGui::GetItemRectMax();
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("MEDIA", &i, sizeof(int));
                ImGui::Text("Dragging: %s", m.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsMouseHoveringRect(g0, g1) && ImGui::IsMouseClicked(1)) {
                mediaMenuIndex = i;
                openMediaMenu = true;
            }
            ImGui::PopID();
        }
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(1) && !openMediaMenu) {
            mediaMenuIndex = -1;
            openMediaMenu = true;
        }
        ImGui::EndChild();

        if (openMediaMenu) {
            ImGui::OpenPopup("mediamenu");
            openMediaMenu = false;
        }
        if (ImGui::BeginPopup("mediamenu")) {
            if (mediaMenuIndex >= 0 && mediaMenuIndex < (int)media.size()) {
                ImGui::TextUnformatted(media[mediaMenuIndex].name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) RemoveMedia(mediaMenuIndex);
            }
            if (ImGui::MenuItem("Import")) ImportFiles(hwnd);
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::BeginChild("Preview", ImVec2(0, topH - 40), true);
        ImGui::Text("Preview | %s", previewStatus.c_str());
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("prev", ImVec2(avail.x, std::max(10.0f, avail.y - 8)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
        previewMin = p0;
        previewMax = p1;

        topClip = -1;
        int bestTrack = 99;
        for (int i = 0; i < (int)clips.size(); i++) {
            if (clips[i].track >= 3) continue;
            if (currentTime < clips[i].startTime || currentTime >= clips[i].startTime + clips[i].duration) continue;
            if (clips[i].track <= bestTrack) {
                bestTrack = clips[i].track;
                topClip = i;
            }
        }
        if (topClip >= 0) EnsurePlayerForClip(topClip);
        else {
            dl->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(140, 140, 140, 255), "No video on timeline");
            previewStatus = "No video";
        }
        ImGui::EndChild();

        if (ImGui::Button("Play")) {
#ifdef HAVE_FFMPEG
            SetPlaying(true);
#else
            isPlaying = true;
#endif
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause")) {
#ifdef HAVE_FFMPEG
            SetPlaying(false);
#else
            isPlaying = false;
#endif
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
#ifdef HAVE_FFMPEG
            SetPlaying(false);
#else
            isPlaying = false;
#endif
            currentTime = 0;
            shownLocal = -1000.0f;
        }
        ImGui::SameLine();
        ImGui::Text("Drag clip edges to trim. Snap joins clips with no gap.");
        ImGui::EndGroup();

        ImGui::Separator();
        ImGui::Text("Tools:"); ImGui::SameLine();
        if (ImGui::RadioButton("Select", activeTool == 0)) activeTool = 0; ImGui::SameLine();
        if (ImGui::RadioButton("Trim", activeTool == 1)) activeTool = 1; ImGui::SameLine();
        if (ImGui::RadioButton("Split", activeTool == 2)) activeTool = 2; ImGui::SameLine();
        ImGui::Text("| C = split | Right click: fade / speed | Scroll the bar under the timeline to go later");
        ImGui::Separator();

        float tlSeconds = 600.0f;
        for (const auto& c : clips)
            tlSeconds = std::max(tlSeconds, c.startTime + c.duration + 90.0f);
        float timeW = tlSeconds * pixelsPerSecond;
        ImGui::BeginChild("Timeline", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar);
        const char* trackNames[] = {"V1", "V2", "V3", "A1", "A2", "A3"};
        float trackH = 42.0f;
        ImVec2 trackPositions[6];

        ImGui::BeginChild("ruler", ImVec2(timeW + 90.0f, 26.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::Dummy(ImVec2(4, 14));
        ImGui::SameLine(46.0f);
        float rulerOrigin = ImGui::GetCursorScreenPos().x;
        ImGui::InvisibleButton("rulerhit", ImVec2(std::max(10.0f, timeW), 14.0f));
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
        }
        ImGui::EndChild();

        for (int t = 0; t < 6; t++) {
            ImGui::PushID(t);
            ImGui::BeginChild("track", ImVec2(timeW + 90.0f, trackH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::Text("%s", trackNames[t]);
            ImGui::SameLine(46.0f);
            ImVec2 trackStart = ImGui::GetCursorScreenPos();
            trackPositions[t] = trackStart;
            float trackW = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("dropzone", ImVec2(trackW, trackH - 4));

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA")) {
                    int mid = *(const int*)payload->Data;
                    if (mid >= 0 && mid < (int)media.size()) {
                        bool isVideo = (media[mid].type == "video" || media[mid].type == "image");
                        bool isAudio = (media[mid].type == "audio");
                        if ((t < 3 && isVideo) || (t >= 3 && isAudio)) {
                            Clip c;
                            c.mediaId = mid;
                            c.duration = media[mid].duration;
                            c.sourceOffset = 0;
                            float localX = ImGui::GetMousePos().x - trackStart.x;
                            c.startTime = SnapStart(-1, c.duration, localX / pixelsPerSecond);
                            c.track = t;
                            clips.push_back(c);
                            selectedClip = (int)clips.size() - 1;
                        }
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
                ImVec2 cp0(x, trackStart.y);
                ImVec2 cp1(x + w, trackStart.y + trackH - 8);
                ImU32 ccol = (t < 3) ? IM_COL32(60, 120, 200, 220) : IM_COL32(70, 160, 90, 220);
                tdl->AddRectFilled(cp0, cp1, ccol, 3.0f);
                int mid = clips[i].mediaId;
                if (mid >= 0 && mid < (int)media.size() && media[mid].thumb) {
                    float tw = std::min(40.0f, w - 8.0f);
                    if (tw > 10.0f)
                        thumbBlits.push_back({cp0, ImVec2(cp0.x + tw, cp1.y), trackClip0, trackClip1, media[mid].thumb, media[mid].thumbW, media[mid].thumbH});
                }
                if (i == selectedClip) {
                    EnsureEnvelope(clips[i]);
                    tdl->AddRect(cp0, cp1, IM_COL32(255, 220, 60, 255), 3.0f, 0, 2.0f);
                    tdl->AddLine(cp0, ImVec2(cp0.x, cp1.y), IM_COL32(255, 255, 255, 255), 4.0f);
                    tdl->AddLine(ImVec2(cp1.x, cp0.y), cp1, IM_COL32(255, 255, 255, 255), 4.0f);
                    ImVec2 prev(0, 0);
                    for (int p = 0; p < (int)clips[i].volume.size(); p++) {
                        float px = cp0.x + clips[i].volume[p].norm * w;
                        float py = cp1.y - clips[i].volume[p].gain * (cp1.y - cp0.y);
                        if (p > 0) tdl->AddLine(prev, ImVec2(px, py), IM_COL32(255, 210, 70, 255), 2.0f);
                        tdl->AddCircleFilled(ImVec2(px, py), 4.5f, IM_COL32(255, 255, 255, 255));
                        prev = ImVec2(px, py);
                    }
                }
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
                tdl->AddText(ImVec2(cp0.x + 44, cp0.y + 4), IM_COL32(255, 255, 255, 230), label.c_str());

                if (ImGui::IsMouseHoveringRect(cp0, cp1)) {
                    float mx = ImGui::GetMousePos().x;
                    if (mx < cp0.x + 8 || mx > cp1.x - 8)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }

            if (draggingClip < 0 && trimmingClip < 0 && dragVolClip < 0) {
                for (int i = (int)clips.size() - 1; i >= 0; --i) {
                    if (clips[i].track != t) continue;
                    float x = trackStart.x + clips[i].startTime * pixelsPerSecond;
                    float w = std::max(24.0f, clips[i].duration * pixelsPerSecond);
                    ImVec2 cp0(x, trackStart.y);
                    ImVec2 cp1(x + w, trackStart.y + trackH - 8);
                    if (!ImGui::IsMouseHoveringRect(cp0, cp1)) continue;
                    ImVec2 mouse = ImGui::GetMousePos();
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
                        selectedClip = i;
                        contextClip = i;
                        EnsureEnvelope(clips[i]);
                        openClipMenu = true;
                        break;
                    }
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        selectedClip = i;
                        EnsureEnvelope(clips[i]);
                        float n = (mouse.x - cp0.x) / w;
                        float g = (cp1.y - mouse.y) / std::max(1.0f, cp1.y - cp0.y);
                        if (n < 0) n = 0;
                        if (n > 1) n = 1;
                        if (g < 0) g = 0;
                        if (g > 1) g = 1;
                        auto& pts = clips[i].volume;
                        size_t at = 0;
                        while (at < pts.size() && pts[at].norm < n) at++;
                        pts.insert(pts.begin() + at, {n, g});
                        break;
                    }
                    if (!ImGui::IsMouseClicked(0)) continue;
                    selectedClip = i;
                    EnsureEnvelope(clips[i]);
                    int hit = -1;
                    for (int p = 0; p < (int)clips[i].volume.size(); p++) {
                        float px = cp0.x + clips[i].volume[p].norm * w;
                        float py = cp1.y - clips[i].volume[p].gain * (cp1.y - cp0.y);
                        float dx = mouse.x - px, dy = mouse.y - py;
                        if (dx * dx + dy * dy <= 64.0f) { hit = p; break; }
                    }
                    if (hit >= 0) {
                        dragVolClip = i;
                        dragVolPt = hit;
                    } else {
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
                        }
                    }
                    break;
                }
            }
            ImGui::EndChild();
            ImGui::PopID();
        }

        if (dragVolClip >= 0 && dragVolClip < (int)clips.size()) {
            if (ImGui::IsMouseDown(0) && dragVolPt >= 0 && dragVolPt < (int)clips[dragVolClip].volume.size()) {
                ImVec2 mouse = ImGui::GetMousePos();
                int tr = clips[dragVolClip].track;
                float top = trackPositions[tr].y;
                float bot = top + trackH - 8.0f;
                float x0 = trackPositions[tr].x + clips[dragVolClip].startTime * pixelsPerSecond;
                float w = std::max(24.0f, clips[dragVolClip].duration * pixelsPerSecond);
                float n = (mouse.x - x0) / w;
                float g = (bot - mouse.y) / std::max(1.0f, bot - top);
                if (n < 0) n = 0;
                if (n > 1) n = 1;
                if (g < 0) g = 0;
                if (g > 1) g = 1;
                auto& pts = clips[dragVolClip].volume;
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
                ImGui::SetTooltip("%d", (int)std::lround(g * 128.0f));
                shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                ResetAudioSync();
#endif
            } else {
                dragVolClip = -1;
                dragVolPt = -1;
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

        if (draggingClip >= 0 && draggingClip < (int)clips.size() && trimmingClip < 0) {
            if (ImGui::IsMouseDown(0)) {
                ImVec2 mouse = ImGui::GetMousePos();
                int tr = clips[draggingClip].track;
                float origin = trackPositions[tr].x;
                float newStart = (mouse.x - dragOffsetX - origin) / pixelsPerSecond;
                newStart = SnapStart(draggingClip, clips[draggingClip].duration, newStart);
                clips[draggingClip].startTime = newStart;
                for (int t = 0; t < 6; t++) {
                    float ty = trackPositions[t].y;
                    if (mouse.y >= ty && mouse.y < ty + trackH) {
                        int mid = clips[draggingClip].mediaId;
                        bool isVideo = (media[mid].type == "video" || media[mid].type == "image");
                        bool isAudio = (media[mid].type == "audio");
                        if ((t < 3 && isVideo) || (t >= 3 && isAudio))
                            clips[draggingClip].track = t;
                        break;
                    }
                }
            } else draggingClip = -1;
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

        if (isPlaying || draggingClip >= 0 || draggingPlayhead || trimmingClip >= 0) {
            float mx = ImGui::GetMousePos().x;
            float viewL = tlMin.x;
            float viewR = tlMin.x + ImGui::GetWindowSize().x;
            if (draggingClip >= 0 || draggingPlayhead || trimmingClip >= 0) {
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
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
                if (ImGui::MenuItem("Fade Out", nullptr, &c.fadeOut)) {
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
                if (ImGui::BeginMenu("Speed")) {
                    const float speeds[] = {1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
                    const char* labels[] = {"1.0x", "1.1x", "1.25x", "1.5x", "1.75x", "2x"};
                    for (int s = 0; s < 6; s++) {
                        bool on = std::fabs(c.speed - speeds[s]) < 0.02f;
                        if (ImGui::MenuItem(labels[s], nullptr, on))
                            SetClipSpeed(c, speeds[s]);
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::End();

        if (requestExport) {
            ImGui::OpenPopup("Export Video");
            requestExport = false;
        }
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Export Video", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Export video");
            ImGui::Separator();
            ImGui::TextUnformatted("Aspect");
            if (ImGui::RadioButton("16:9", exAspect == 0)) exAspect = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("9:16", exAspect == 1)) exAspect = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("1:1", exAspect == 2)) exAspect = 2;
            ImGui::SameLine();
            if (ImGui::RadioButton("4:3", exAspect == 3)) exAspect = 3;
            ImGui::Spacing();
            ImGui::TextUnformatted("Resolution");
            if (ImGui::RadioButton("720p", exRes == 0)) exRes = 0;
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
            int ew = 0, eh = 0;
            const int fpsVals[] = {24, 30, 50, 60};
            int fps = fpsVals[exFpsIdx < 0 || exFpsIdx > 3 ? 1 : exFpsIdx];
            ExportDims(exAspect, exRes, ew, eh);
            ImGui::Separator();
            ImGui::Text("Output: %d x %d   @ %d fps", ew, eh, fps);
            if (exRes >= 3) ImGui::TextDisabled("4K and 8K take a long time.");
            if (ImGui::Button("Choose file and export")) {
                ImGui::CloseCurrentPopup();
                std::string out = SaveDialog(hwnd, "MP4 video (*.mp4)\0*.mp4\0", "mp4", "Export Video");
                if (!out.empty()) {
#ifdef HAVE_FFMPEG
                    gExport.Begin(out, ew, eh, fps);
#else
                    previewStatus = "FFmpeg not linked";
#endif
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

#ifdef HAVE_FFMPEG
        if (gExport.active) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Exporting", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
            float p = gExport.total > 0 ? (float)gExport.frameIndex / (float)gExport.total : 0.0f;
            ImGui::ProgressBar(p, ImVec2(380, 0));
            ImGui::TextUnformatted(gExport.status.c_str());
            if (ImGui::Button("Cancel export")) gExport.Cancel();
            ImGui::End();
            int budget = 2;
            long pix = (long)gExport.w * (long)gExport.h;
            if (pix >= 3840L * 2160L) budget = 1;
            else if (pix <= 1280L * 720L) budget = 4;
            for (int s = 0; s < budget && gExport.active; s++) gExport.Step();
        } else {
            SyncPreview(topClip, io.DeltaTime);
            StopIfFinished();
        }
#else
        if (isPlaying && !draggingPlayhead) currentTime += io.DeltaTime;
        StopIfFinished();
#endif

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.05f, 0.05f, 0.05f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifdef HAVE_FFMPEG
        if (player && player->hasFrame && player->texture)
            BlitVideo(previewMin, previewMax, player->texture, player->width, player->height, previewMin, previewMax);
        for (const auto& tb : thumbBlits)
            BlitVideo(tb.a, tb.b, tb.tex, tb.w, tb.h, tb.c0, tb.c1);
#endif
        SDL_GL_SwapWindow(window);
    }

#ifdef HAVE_FFMPEG
    if (gExport.active) gExport.Cancel();
    if (player) { delete player; player = nullptr; }
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
