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
    }

    bool GrabFrame() {
        if (!fmtCtx || !codecCtx) return false;
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(fmtCtx, packet) >= 0) {
            if (packet->stream_index == videoStream) {
                int send = avcodec_send_packet(codecCtx, packet);
                if (send == 0 || send == AVERROR(EAGAIN)) {
                    if (avcodec_receive_frame(codecCtx, frame) == 0) {
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

void QueueAudioBlock(double timelineTime, int sampleCount) {
    if (!audioDev || sampleCount <= 0) return;
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
    std::vector<int16_t> out((size_t)sampleCount * 2);
    for (int i = 0; i < sampleCount * 2; i++) {
        int v = mix[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
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

    SDL_Window* window = SDL_CreateWindow("DaniWorldsStudio FFmpeg 3",
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
                    SetPlaying(!isPlaying);
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

        if (ImGui::Button("Import")) ImportFiles(hwnd);
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
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("MEDIA", &i, sizeof(int));
                ImGui::Text("Dragging: %s", m.name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

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
        for (int i = (int)clips.size() - 1; i >= 0; --i) {
            if (clips[i].track >= 3) continue;
            if (currentTime >= clips[i].startTime && currentTime < clips[i].startTime + clips[i].duration)
                topClip = i;
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
        ImGui::Text("| C = split at playhead | Right click clip for fade / speed");
        ImGui::Separator();

        ImGui::BeginChild("Timeline", ImVec2(0, 0), true);
        const char* trackNames[] = {"V1", "V2", "V3", "A1", "A2", "A3"};
        float trackH = 42.0f;
        ImVec2 trackPositions[6];

        for (int t = 0; t < 6; t++) {
            ImGui::PushID(t);
            ImGui::BeginChild("track", ImVec2(0, trackH), true);
            ImGui::Text("%s", trackNames[t]);
            ImGui::SameLine();
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

        ImVec2 tlMin = ImGui::GetWindowPos();
        float phX = tlMin.x + 50 + currentTime * pixelsPerSecond;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(phX, tlMin.y + 8), ImVec2(phX, tlMin.y + 290), IM_COL32(255, 50, 50, 255), 2.0f);

        if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(ImVec2(phX - 6, tlMin.y), ImVec2(phX + 6, tlMin.y + 290)))
            draggingPlayhead = true;
        if (draggingPlayhead) {
            if (ImGui::IsMouseDown(0)) {
                float newTime = (ImGui::GetMousePos().x - (tlMin.x + 50)) / pixelsPerSecond;
                if (newTime < 0) newTime = 0;
                if (std::fabs(newTime - currentTime) > 0.0001f) {
                    currentTime = newTime;
                    shownLocal = -1000.0f;
#ifdef HAVE_FFMPEG
                    ResetAudioSync();
#endif
                }
            } else draggingPlayhead = false;
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

#ifdef HAVE_FFMPEG
        if (topClip >= 0 && player && player->fmtCtx) {
            float local = currentTime - clips[topClip].startTime;
            if (local < 0) local = 0;
            float mediaT = clips[topClip].sourceOffset + local * std::max(0.05f, clips[topClip].speed);
            if (isPlaying && !draggingPlayhead) {
                currentTime += io.DeltaTime;
                static float accum = 0;
                accum += io.DeltaTime;
                if (accum > 0.033f) {
                    frameDebt += std::max(0.05f, clips[topClip].speed);
                    int grabs = (int)frameDebt;
                    if (grabs < 1) grabs = 1;
                    frameDebt -= grabs;
                    if (frameDebt < 0) frameDebt = 0;
                    for (int g = 0; g < grabs; g++) player->GrabFrame();
                    char buf[64];
                    snprintf(buf, sizeof(buf), "FFmpeg %dx%d x%.2g", player->width, player->height, clips[topClip].speed);
                    previewStatus = buf;
                    shownLocal = mediaT;
                    accum = 0;
                }
            } else if (!isPlaying && std::fabs(mediaT - shownLocal) > 0.08f) {
                player->Seek(mediaT);
                if (player->GrabFrame()) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "FFmpeg %dx%d", player->width, player->height);
                    previewStatus = buf;
                    shownLocal = mediaT;
                }
            }
        } else if (isPlaying && !draggingPlayhead) {
            currentTime += io.DeltaTime;
        }
        PumpAudio();
#else
        if (isPlaying && !draggingPlayhead) currentTime += io.DeltaTime;
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
