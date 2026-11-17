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

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
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
};

struct Clip {
    int mediaId = -1;
    float startTime = 0.0f;
    float duration = 5.0f;
    int track = 0;
};

std::vector<MediaItem> media;
std::vector<Clip> clips;
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

typedef void (APIENTRY *PFN_glUseProgramProc)(GLuint);
PFN_glUseProgramProc pglUseProgram = nullptr;

#ifdef HAVE_FFMPEG
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
                videoStream = (int)i;
                break;
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
                        sws_scale(swsCtx, frame->data, frame->linesize, 0, height,
                                  rgbFrame->data, rgbFrame->linesize);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glBindTexture(GL_TEXTURE_2D, texture);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                                        GL_RGBA, GL_UNSIGNED_BYTE, rgbFrame->data[0]);
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

    float DurationSec() const {
        if (!fmtCtx || fmtCtx->duration <= 0) return 10.0f;
        return (float)fmtCtx->duration / (float)AV_TIME_BASE;
    }
};

VideoPlayer* player = nullptr;
int playingMediaId = -1;
#endif

void BlitVideo(ImVec2 p0, ImVec2 p1, GLuint tex, int vw, int vh) {
    if (!tex || vw <= 0 || vh <= 0) return;
    float areaW = p1.x - p0.x;
    float areaH = p1.y - p0.y;
    if (areaW < 8 || areaH < 8) return;

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

    if (pglUseProgram) pglUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor3f(1.f, 1.f, 1.f);

    ImGuiIO& io = ImGui::GetIO();
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
}

HWND GetSDLWindowHandle(SDL_Window* window) {
#ifdef _WIN32
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) return wmInfo.info.win.window;
#endif
    return NULL;
}

std::string OpenFileDialog(HWND owner) {
#ifdef _WIN32
    char filename[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Video Files\0*.mp4;*.mov;*.avi;*.mkv\0Audio Files\0*.mp3;*.wav;*.aac\0Image Files\0*.jpg;*.jpeg;*.png;*.bmp\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Import Media";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(filename);
#endif
    return "";
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
        lower.find(".avi") != std::string::npos || lower.find(".mkv") != std::string::npos)
        m.type = "video";
    else if (lower.find(".mp3") != std::string::npos || lower.find(".wav") != std::string::npos || lower.find(".aac") != std::string::npos)
        m.type = "audio";
    else
        m.type = "image";
    m.duration = (m.type == "image") ? 5.0f : 10.0f;
    media.push_back(m);
}

void DrawThumbnail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const MediaItem& m) {
    ImU32 bg = (m.type == "video") ? IM_COL32(50, 90, 160, 255) :
               (m.type == "audio") ? IM_COL32(50, 130, 70, 255) : IM_COL32(120, 70, 140, 255);
    dl->AddRectFilled(p0, p1, bg, 4.0f);
    dl->AddRect(p0, p1, IM_COL32(180, 180, 180, 120), 4.0f);
    const char* icon = (m.type == "video") ? "VID" : (m.type == "audio") ? "AUD" : "IMG";
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p0.x + (p1.x - p0.x - ts.x) * 0.5f, p0.y + (p1.y - p0.y - ts.y) * 0.35f), IM_COL32(255, 255, 255, 230), icon);
    char dur[16]; snprintf(dur, sizeof(dur), "%.1fs", m.duration);
    dl->AddText(ImVec2(p0.x + 4, p1.y - 16), IM_COL32(255, 255, 255, 200), dur);
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
        float dur = player->DurationSec();
        if (dur > 0.1f) {
            media[mid].duration = dur;
            clips[clipIdx].duration = dur;
        }
        float localTime = currentTime - clips[clipIdx].startTime;
        if (localTime < 0) localTime = 0;
        player->Seek(localTime);
        if (player->GrabFrame()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "FFmpeg %dx%d", player->width, player->height);
            previewStatus = buf;
            shownLocal = localTime;
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

int main(int argc, char* argv[]) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("DaniWorldsStudio FFmpeg",
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
                if (event.key.keysym.sym == SDLK_SPACE) isPlaying = !isPlaying;
                if (event.key.keysym.sym == SDLK_DELETE || event.key.keysym.sym == SDLK_BACKSPACE) {
                    if (selectedClip >= 0 && selectedClip < (int)clips.size()) {
                        clips.erase(clips.begin() + selectedClip);
                        selectedClip = -1;
                    }
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        if (ImGui::Button("Import")) {
            std::string path = OpenFileDialog(hwnd);
            if (!path.empty()) AddMedia(path);
        }
        ImGui::SameLine();
        if (ImGui::Button(snapEnabled ? "Snap ON" : "Snap OFF")) snapEnabled = !snapEnabled;
        ImGui::SameLine();
        if (ImGui::Button("Play")) isPlaying = true;
        ImGui::SameLine();
        if (ImGui::Button("Pause")) isPlaying = false;
        ImGui::SameLine();
        if (ImGui::Button("Stop")) { isPlaying = false; currentTime = 0; shownLocal = -1000.0f; }
        ImGui::SameLine();
        ImGui::Text("| %.2fs | Media: %d | FFMPEG | Space=Play/Pause | ESC=Exit", currentTime, (int)media.size());

        ImGui::Separator();
        float topH = io.DisplaySize.y * 0.38f;

        ImGui::BeginChild("Media", ImVec2(io.DisplaySize.x * 0.26f, topH), true);
        ImGui::Text("Media Library");
        ImGui::Separator();
        for (int i = 0; i < (int)media.size(); i++) {
            const auto& m = media[i];
            ImGui::PushID(i);
            ImGui::BeginGroup();
            ImGui::Button("##dragitem", ImVec2(120, 70));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            DrawThumbnail(dl, p0, p1, m);
            ImGui::TextWrapped("%s", m.name.c_str());
            ImGui::EndGroup();
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("MEDIA", &i, sizeof(int));
                ImGui::Text("Dragging: %s", m.name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("Preview", ImVec2(0, topH), true);
        ImGui::Text("Preview | %s", previewStatus.c_str());
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("prev", ImVec2(avail.x, avail.y - 8));
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

        if (topClip >= 0) {
            EnsurePlayerForClip(topClip);
#ifndef HAVE_FFMPEG
            dl->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(200, 200, 200, 255), "FFmpeg not linked");
#endif
        } else {
            dl->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(140, 140, 140, 255), "No video on timeline");
            previewStatus = "No video";
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Text("Tools:"); ImGui::SameLine();
        if (ImGui::RadioButton("Select", activeTool == 0)) activeTool = 0; ImGui::SameLine();
        if (ImGui::RadioButton("Trim", activeTool == 1)) activeTool = 1; ImGui::SameLine();
        if (ImGui::RadioButton("Split", activeTool == 2)) activeTool = 2; ImGui::SameLine();
        ImGui::Text("| Del = Delete | Drag clips to move / change track");

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
                            c.startTime = 0.0f;
                            c.track = t;
                            clips.push_back(c);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImDrawList* tdl = ImGui::GetWindowDrawList();
            for (int i = 0; i < (int)clips.size(); i++) {
                if (clips[i].track != t) continue;
                float x = trackStart.x + clips[i].startTime * pixelsPerSecond;
                float w = std::max(30.0f, clips[i].duration * pixelsPerSecond);
                ImVec2 cp0(x, trackStart.y);
                ImVec2 cp1(x + w, trackStart.y + trackH - 8);
                ImU32 ccol = (t < 3) ? IM_COL32(60, 120, 200, 220) : IM_COL32(70, 160, 90, 220);
                tdl->AddRectFilled(cp0, cp1, ccol, 3.0f);
                if (i == selectedClip)
                    tdl->AddRect(cp0, cp1, IM_COL32(255, 220, 60, 255), 3.0f, 0, 2.0f);
                std::string label = media[clips[i].mediaId].name.substr(0, 18);
                tdl->AddText(ImVec2(cp0.x + 4, cp0.y + 4), IM_COL32(255, 255, 255, 230), label.c_str());

                if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(cp0, cp1)) {
                    selectedClip = i;
                    draggingClip = i;
                    dragOffsetX = ImGui::GetMousePos().x - cp0.x;
                }
            }
            ImGui::EndChild();
            ImGui::PopID();
        }

        if (draggingClip >= 0 && draggingClip < (int)clips.size()) {
            if (ImGui::IsMouseDown(0)) {
                ImVec2 mouse = ImGui::GetMousePos();
                float newStart = (mouse.x - dragOffsetX - 50) / pixelsPerSecond;
                if (newStart < 0) newStart = 0;
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
            } else {
                draggingClip = -1;
            }
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
                currentTime = newTime;
            } else {
                draggingPlayhead = false;
            }
        }

        ImGui::EndChild();
        ImGui::End();

#ifdef HAVE_FFMPEG
        if (topClip >= 0 && player && player->fmtCtx) {
            float local = currentTime - clips[topClip].startTime;
            if (local < 0) local = 0;
            if (isPlaying && !draggingPlayhead) {
                currentTime += io.DeltaTime;
                static float accum = 0;
                accum += io.DeltaTime;
                if (accum > 0.033f) {
                    if (player->GrabFrame()) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "FFmpeg %dx%d play", player->width, player->height);
                        previewStatus = buf;
                    }
                    shownLocal = local;
                    accum = 0;
                }
            } else if (!isPlaying && std::fabs(local - shownLocal) > 0.08f) {
                player->Seek(local);
                if (player->GrabFrame()) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "FFmpeg %dx%d", player->width, player->height);
                    previewStatus = buf;
                    shownLocal = local;
                }
            }
        } else if (isPlaying && !draggingPlayhead) {
            currentTime += io.DeltaTime;
        }
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
            BlitVideo(previewMin, previewMax, player->texture, player->width, player->height);
#endif
        SDL_GL_SwapWindow(window);
    }

#ifdef HAVE_FFMPEG
    if (player) { delete player; player = nullptr; }
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
