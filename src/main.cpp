#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_syswm.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
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

#ifdef _WIN32
class VideoPlayer {
public:
    IMFSourceReader* reader = nullptr;
    UINT32 width = 0, height = 0;
    GLuint texture = 0;
    bool hasFrame = false;
    std::string lastError;

    ~VideoPlayer() { Close(); }

    bool Open(const std::string& path) {
        Close();
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) { lastError = "MFStartup failed"; return false; }

        std::wstring wpath(path.begin(), path.end());
        IMFAttributes* attrs = nullptr;
        MFCreateAttributes(&attrs, 2);
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
        if (attrs) attrs->Release();
        if (FAILED(hr)) { lastError = "Cannot open file"; return false; }

        IMFMediaType* type = nullptr;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
        type->Release();
        if (FAILED(hr)) { lastError = "Set media type failed"; return false; }

        IMFMediaType* actual = nullptr;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
        MFGetAttributeSize(actual, MF_MT_FRAME_SIZE, &width, &height);
        actual->Release();

        if (width == 0 || height == 0) { lastError = "Invalid size"; return false; }

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

        lastError.clear();
        return true;
    }

    void Seek(float seconds) {
        if (!reader) return;
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = (LONGLONG)(seconds * 10000000.0);
        reader->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);
    }

    bool GrabFrame() {
        if (!reader) return false;
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (FAILED(hr) || !sample) return false;

        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) { sample->Release(); return false; }
        BYTE* data = nullptr;
        DWORD len = 0;
        buffer->Lock(&data, nullptr, &len);

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, data);

        buffer->Unlock();
        buffer->Release();
        sample->Release();
        hasFrame = true;
        return true;
    }

    void Close() {
        if (texture) { glDeleteTextures(1, &texture); texture = 0; }
        if (reader) { reader->Release(); reader = nullptr; }
        MFShutdown();
        hasFrame = false;
    }
};

VideoPlayer* player = nullptr;
int playingMediaId = -1;
#endif

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
    dl->AddText(ImVec2(p0.x + (p1.x-p0.x-ts.x)*0.5f, p0.y + (p1.y-p0.y-ts.y)*0.35f), IM_COL32(255,255,255,230), icon);
    char dur[16]; snprintf(dur, sizeof(dur), "%.1fs", m.duration);
    dl->AddText(ImVec2(p0.x+4, p1.y-16), IM_COL32(255,255,255,200), dur);
}

void EnsurePlayerForClip(int clipIdx) {
#ifdef _WIN32
    if (clipIdx < 0 || clipIdx >= (int)clips.size()) return;
    int mid = clips[clipIdx].mediaId;
    if (mid == playingMediaId && player && player->reader) return;
    if (!player) player = new VideoPlayer();
    previewStatus = "Opening...";
    if (player->Open(media[mid].path)) {
        playingMediaId = mid;
        float localTime = currentTime - clips[clipIdx].startTime;
        if (localTime < 0) localTime = 0;
        player->Seek(localTime);
        if (player->GrabFrame()) {
            previewStatus = "Frame OK";
        } else {
            previewStatus = "Grab failed";
        }
    } else {
        previewStatus = player->lastError.empty() ? "Open failed" : player->lastError;
    }
#endif
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow("DaniWorldsStudio Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { SDL_Quit(); return 1; }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    HWND hwnd = GetSDLWindowHandle(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool running = true;
    SDL_Event event;

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

        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse);

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
        if (ImGui::Button("Stop")) { isPlaying = false; currentTime = 0; }
        ImGui::SameLine();
        ImGui::Text("| %.2fs | Media: %d | Space=Play/Pause | ESC=Exit", currentTime, (int)media.size());

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
        ImVec2 prevSize(avail.x, avail.y - 8);
        ImGui::InvisibleButton("prev", prevSize);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(p0, p1, IM_COL32(0,0,0,255));

        int topClip = -1;
        for (int i = (int)clips.size()-1; i >= 0; --i) {
            if (clips[i].track >= 3) continue;
            if (currentTime >= clips[i].startTime && currentTime < clips[i].startTime + clips[i].duration)
                topClip = i;
        }

#ifdef _WIN32
        if (topClip >= 0) {
            EnsurePlayerForClip(topClip);
            if (player && player->hasFrame && player->texture) {
                ImVec2 texP0 = p0;
                ImVec2 texP1 = p1;
                float aspect = (player->height > 0) ? (float)player->width / (float)player->height : 16.0f/9.0f;
                float areaW = p1.x - p0.x;
                float areaH = p1.y - p0.y;
                float areaAspect = areaW / std::max(1.0f, areaH);
                if (aspect > areaAspect) {
                    float h = areaW / aspect;
                    float off = (areaH - h) * 0.5f;
                    texP0.y += off; texP1.y -= off;
                } else {
                    float w = areaH * aspect;
                    float off = (areaW - w) * 0.5f;
                    texP0.x += off; texP1.x -= off;
                }
                dl->AddImage((ImTextureID)(intptr_t)player->texture, texP0, texP1);
            } else {
                dl->AddText(ImVec2(p0.x+12, p0.y+12), IM_COL32(200,200,200,255), previewStatus.c_str());
            }
        } else {
            dl->AddText(ImVec2(p0.x+12, p0.y+12), IM_COL32(140,140,140,255), "No video on timeline");
            previewStatus = "No video";
        }
#else
        dl->AddText(ImVec2(p0.x+12, p0.y+12), IM_COL32(140,140,140,255), "Video preview (Windows only)");
#endif
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Text("Tools:"); ImGui::SameLine();
        if (ImGui::RadioButton("Select", activeTool == 0)) activeTool = 0; ImGui::SameLine();
        if (ImGui::RadioButton("Trim", activeTool == 1)) activeTool = 1; ImGui::SameLine();
        if (ImGui::RadioButton("Split", activeTool == 2)) activeTool = 2; ImGui::SameLine();
        ImGui::Text("| Del = Delete");

        ImGui::Separator();

        ImGui::BeginChild("Timeline", ImVec2(0,0), true);
        const char* trackNames[] = {"V1","V2","V3","A1","A2","A3"};
        float trackH = 42.0f;

        for (int t = 0; t < 6; t++) {
            ImGui::PushID(t);
            ImGui::BeginChild("track", ImVec2(0, trackH), true);
            ImGui::Text("%s", trackNames[t]);
            ImGui::SameLine();
            ImVec2 trackStart = ImGui::GetCursorScreenPos();
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
                ImU32 ccol = (t < 3) ? IM_COL32(60,120,200,220) : IM_COL32(70,160,90,220);
                tdl->AddRectFilled(cp0, cp1, ccol, 3.0f);
                if (i == selectedClip)
                    tdl->AddRect(cp0, cp1, IM_COL32(255,220,60,255), 3.0f, 0, 2.0f);
                std::string label = media[clips[i].mediaId].name.substr(0, 18);
                tdl->AddText(ImVec2(cp0.x+4, cp0.y+4), IM_COL32(255,255,255,230), label.c_str());
                if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(cp0, cp1))
                    selectedClip = i;
            }
            ImGui::EndChild();
            ImGui::PopID();
        }

        ImVec2 tlMin = ImGui::GetWindowPos();
        float phX = tlMin.x + 50 + currentTime * pixelsPerSecond;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(phX, tlMin.y+8), ImVec2(phX, tlMin.y+290), IM_COL32(255,50,50,255), 2.0f);
        ImGui::EndChild();
        ImGui::End();

        if (isPlaying) {
            currentTime += io.DeltaTime;
#ifdef _WIN32
            if (topClip >= 0 && player) {
                static float accum = 0;
                accum += io.DeltaTime;
                if (accum > 0.04f) {
                    player->GrabFrame();
                    accum = 0;
                }
            }
#endif
        }

        ImGui::Render();
        glViewport(0,0,(int)io.DisplaySize.x,(int)io.DisplaySize.y);
        glClearColor(0.05f,0.05f,0.05f,1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

#ifdef _WIN32
    if (player) { delete player; player = nullptr; }
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
