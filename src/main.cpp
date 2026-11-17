#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

struct MediaItem {
    std::string path;
    std::string name;
    std::string type;
    float duration = 5.0f;
    float aspect = 16.0f/9.0f;
};

struct Clip {
    int mediaId = -1;
    float startTime = 0.0f;
    float duration = 5.0f;
    float trimStart = 0.0f;
    float trimEnd = 0.0f;
    float volume = 1.0f;
    int track = 0;
};

std::vector<MediaItem> media;
std::vector<Clip> clips;
float currentTime = 0.0f;
float pixelsPerSecond = 40.0f;
bool isPlaying = false;
bool snapEnabled = true;
int selectedClip = -1;

std::string OpenFileDialog() {
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Media Files\0*.mp4;*.mov;*.avi;*.mkv;*.mp3;*.wav;*.jpg;*.png\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) return filename;
#endif
    return "";
}

void AddMedia(const std::string& path) {
    MediaItem m;
    m.path = path;
    size_t pos = path.find_last_of("/\\");
    m.name = (pos == std::string::npos) ? path : path.substr(pos + 1);
    std::string ext = m.name.substr(m.name.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "mp4" || ext == "mov" || ext == "avi" || ext == "mkv") m.type = "video";
    else if (ext == "mp3" || ext == "wav" || ext == "aac") m.type = "audio";
    else m.type = "image";
    m.duration = (m.type == "image") ? 5.0f : 8.0f;
    media.push_back(m);
}

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow("DaniWorldsStudio Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920, 1080,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window) { SDL_Quit(); return 1; }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

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
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        if (ImGui::Button("Import")) {
            std::string path = OpenFileDialog();
            if (!path.empty()) AddMedia(path);
        }
        ImGui::SameLine();
        if (ImGui::Button(snapEnabled ? "Snap: ON" : "Snap: OFF")) snapEnabled = !snapEnabled;
        ImGui::SameLine();
        if (ImGui::Button("Play")) isPlaying = true;
        ImGui::SameLine();
        if (ImGui::Button("Pause")) isPlaying = false;
        ImGui::SameLine();
        if (ImGui::Button("Stop")) { isPlaying = false; currentTime = 0; }
        ImGui::SameLine();
        ImGui::Text("| Time: %.2f | ESC=Exit", currentTime);

        ImGui::Separator();

        float topH = io.DisplaySize.y * 0.40f;
        ImGui::BeginChild("Media", ImVec2(io.DisplaySize.x * 0.28f, topH), true);
        ImGui::Text("Media Library");
        ImGui::Separator();
        for (int i = 0; i < (int)media.size(); i++) {
            const auto& m = media[i];
            ImGui::PushID(i);
            float thumbW = 120.0f;
            float thumbH = thumbW / m.aspect;
            if (ImGui::Button("##thumb", ImVec2(thumbW, thumbH))) {}
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            ImU32 col = (m.type == "video") ? IM_COL32(70,120,200,255) : (m.type == "audio") ? IM_COL32(80,160,90,255) : IM_COL32(160,100,180,255);
            dl->AddRectFilled(p0, p1, col);
            dl->AddText(ImVec2(p0.x+4, p1.y-18), IM_COL32(255,255,255,255), m.type.c_str());
            ImGui::Text("%s", m.name.c_str());
            ImGui::Text("%.1fs", m.duration);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("MEDIA", &i, sizeof(int));
                ImGui::Text("%s", m.name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("Preview", ImVec2(0, topH), true);
        ImGui::Text("Preview");
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("prev", ImVec2(avail.x, avail.y - 10));
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
        if (topClip >= 0) {
            dl->AddText(ImVec2(p0.x+10, p0.y+10), IM_COL32(200,200,200,255), media[clips[topClip].mediaId].name.c_str());
        } else {
            dl->AddText(ImVec2(p0.x+10, p0.y+10), IM_COL32(150,150,150,255), "No video");
        }
        ImGui::EndChild();

        ImGui::BeginChild("Timeline", ImVec2(0,0), true);
        ImGui::Text("Timeline");
        const char* trackNames[] = {"V1","V2","V3","A1","A2","A3"};
        float trackH = 36.0f;
        for (int t = 0; t < 6; t++) {
            ImGui::PushID(t);
            ImGui::BeginChild("track", ImVec2(0, trackH), true);
            ImGui::Text("%s", trackNames[t]);
            ImGui::SameLine();
            ImVec2 trackStart = ImGui::GetCursorScreenPos();
            float trackW = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("trackbg", ImVec2(trackW, trackH-8));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA")) {
                    int mid = *(const int*)payload->Data;
                    Clip c;
                    c.mediaId = mid;
                    c.duration = media[mid].duration;
                    c.startTime = 0;
                    c.track = t;
                    clips.push_back(c);
                }
                ImGui::EndDragDropTarget();
            }
            ImDrawList* tdl = ImGui::GetWindowDrawList();
            for (int i = 0; i < (int)clips.size(); i++) {
                if (clips[i].track != t) continue;
                float x = trackStart.x + clips[i].startTime * pixelsPerSecond;
                float w = clips[i].duration * pixelsPerSecond;
                ImVec2 cp0(x, trackStart.y);
                ImVec2 cp1(x + w, trackStart.y + trackH - 10);
                ImU32 ccol = (t < 3) ? IM_COL32(70,140,220,200) : IM_COL32(90,180,100,200);
                tdl->AddRectFilled(cp0, cp1, ccol, 3.0f);
                if (i == selectedClip) tdl->AddRect(cp0, cp1, IM_COL32(255,220,50,255), 3.0f, 0, 2.0f);
                std::string label = media[clips[i].mediaId].name.substr(0, 12);
                tdl->AddText(ImVec2(cp0.x+4, cp0.y+4), IM_COL32(255,255,255,255), label.c_str());
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
        ImVec2 tlMin = ImGui::GetWindowPos();
        float phX = tlMin.x + 40 + currentTime * pixelsPerSecond;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(phX, tlMin.y+20), ImVec2(phX, tlMin.y+250), IM_COL32(255,60,60,255), 2.0f);
        ImGui::EndChild();

        ImGui::End();

        if (isPlaying) currentTime += io.DeltaTime;

        ImGui::Render();
        glViewport(0,0,(int)io.DisplaySize.x,(int)io.DisplaySize.y);
        glClearColor(0.04f,0.04f,0.04f,1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
